#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int smaps_fd; // use raw I/O in signal handler, to avoid e.g. malloc deadlock
FILE *out;
_Bool verbose;

static ssize_t get_a_line(char *buf, size_t size, char **p_readpos, char *endpos)
{
	if (size == 0) return -1; // now size is at least 1

	// if we got EOF and read zero bytes, return -1
	if (endpos == *p_readpos) return -1;

#ifndef MIN
#define MIN(x,y) (((x)<(y))?(x):(y))
#endif
	size_t nread = MIN(size - 1, endpos - *p_readpos);
	memcpy(buf, *p_readpos, nread);
	*p_readpos += nread;

	// did we get enough that we have a whole line?
	char *found = memchr(buf, '\n', nread);
	// if so, rewind the readpos to just after the newline
	if (found)
	{
		size_t end_of_newline_displacement = (found - buf) + 1;
		*p_readpos -= nread; // undo what we just did
		*p_readpos += end_of_newline_displacement;
		buf[end_of_newline_displacement - 1] = '\0';
		return end_of_newline_displacement; // i.e. how much we pretend to have read
	}
	else
	{
		/* We didn't read enough. But that should only be because of EOF of error.
		 * So just return whatever we got. */
		buf[nread] = '\0';
		return nread;
	}
}
static unsigned long total_size = 0;
static unsigned long total_resident = 0;
static unsigned long total_dirty = 0;
static unsigned long total_referenced = 0;
static unsigned long total_shared = 0;

static unsigned long sample_num;

// need async-safe I/O for printing
static const char *fmt_hex_num(unsigned long n)
{
        static __thread char buf[19];
        buf[0] = '0';
        buf[1] = 'x';
        signed i_dig = 15;
        do
        {
                unsigned long dig = (n >> (4 * i_dig)) & 0xf;
                buf[2 + 15 - i_dig] = (dig > 9) ? ('a' + dig - 10) : ('0' + dig);
                --i_dig;
        } while (i_dig >= 0);
        buf[18] = '\0';
        return buf;
}
static const char *fmt_dec_num(unsigned n)
{
        static __thread char buf[11]; // 10 digits plus terminator
        signed i_dig = 9;
	unsigned i_pow = 1000000000; // 10^9
        do
        {
                unsigned long dig = (n / i_pow) % 10;
                buf[9 - i_dig] = '0' + dig;
                --i_dig;
		i_pow /= 10;
        } while (i_dig >= 0);
        buf[10] = '\0';
        return buf;
}

#define write_string(s) write(2, (s), sizeof (s) - 1)
#define write_chars(s, t)  write(2, s, t - s)
#define write_ulong_hex(a)   write(2, fmt_hex_num((a)), 18)
#define write_uint_dec(a)   write(2, fmt_dec_num((a)), 10)



void flush_mapping_info(_Bool print, unsigned long *p_size, unsigned long *p_resident,
		unsigned long *p_dirty, unsigned long *p_referenced,
		unsigned long *p_shared, const char *suffix)
{
	const char *pad1, *pad2;
	if (suffix)
	{
		pad1 = " ";
		pad2 = suffix;
	}
	else
	{
		pad1 = "";
		pad2 = "";
	}
	if (print)
	{
		write_string("== ");
		write_uint_dec(getpid());
		write_string(" sample ");
		write_uint_dec(sample_num);
		if (suffix) write_string(" "); else write_string("");
		if (suffix) write_chars(suffix, suffix + strlen(suffix)); else write_string("");
		write_string(" == size ");
		write_uint_dec(*p_size);
		write_string(" kB, resident ");
		write_uint_dec(*p_resident);
		write_string(" kB, dirty ");
		write_uint_dec(*p_dirty);
		write_string(" kB, referenced ");
		write_uint_dec(*p_referenced);
		write_string(",kB, shared ");
		write_uint_dec(*p_shared);
		write_string("\n");
		//fprintf(stderr, "== %d sample %lu%s%s == size %ld kB, resident %ld kB, dirty %ld kB, shared %ld kB\n",
		//getpid(), sample_num, pad1, pad2, *p_size, *p_resident, *p_dirty, *p_shared);
	}
	total_size += *p_size;
	*p_size = 0;
	total_resident += *p_resident;
	*p_resident = 0;
	total_dirty += *p_dirty;
	*p_dirty = 0;
	total_referenced += *p_referenced;
	*p_referenced = 0;
	total_shared += *p_shared;
	*p_shared = 0;
}

void read_smaps(void)
{
	#define NUM_FIELDS 11
	unsigned long first, second;
	char r, w, x, p;
	unsigned offset;
	unsigned devmaj, devmin;
	unsigned inode;
	char rest[4096];

	const int PAGE_SIZE = sysconf(_SC_PAGE_SIZE);

	assert(smaps_fd != -1);
	off_t new_off = lseek(smaps_fd, 0, SEEK_SET);
	assert(new_off == 0);

	++sample_num;

	unsigned long cur_size = 0;
	unsigned long cur_resident = 0;
	unsigned long cur_dirty = 0;
	unsigned long cur_referenced = 0;
	unsigned long cur_shared = 0;

	total_size = 0;
	total_resident = 0;
	total_dirty = 0;
	total_referenced = 0;
	total_shared = 0;

	/* We used to use getline(), but in some deployments it's not okay to
	 * use malloc when we're called early during initialization. So we write
	 * our own read loop. */
	#define MAXSIZ 262144
	char *filebuf = alloca(MAXSIZ);
	char *filebuf_pos = filebuf;
	{
		ssize_t nread;
		while ((nread = read(smaps_fd, filebuf_pos, MAXSIZ)) > 0)
		{ filebuf_pos += nread; }
	}

	char *readpos = filebuf;
	char *endpos = filebuf_pos;
	char linebuf[8192];
	_Bool first_line = 1;
	ssize_t nread;
	while (-1 != (nread = get_a_line(linebuf, sizeof linebuf, &readpos, endpos)))
	{
		rest[0] = '\0';
		if ((linebuf[0] >= '0' && linebuf[0] <= '9')
				|| (linebuf[0] >= 'a' && linebuf[0] <= 'f'))
		{
			/* It's a maps line. If we're not the first one, flush the stats
			 * for the last one. */
			if (!first_line) flush_mapping_info(verbose, &cur_size, &cur_resident, &cur_dirty, &cur_referenced, &cur_shared, NULL);
			first_line = 0;
			/* If we're verbose, re-print the line, minus the newline, followed by a tab. */
			if (verbose)
			{
				write_chars(linebuf, linebuf + strlen(linebuf));
				// nread - 1, 1, stderr);
				write_string("\t");
			}

			int fields_read = sscanf(linebuf,
				"%lx-%lx %c%c%c%c %8x %2x:%2x %d %4095[\x01-\x09\x0b-\xff]\n",
				&first, &second, &r, &w, &x, &p, &offset, &devmaj, &devmin, &inode, rest);

			assert(fields_read >= (NUM_FIELDS-1)); // we might not get a "rest"
			cur_size = (second - first) / 1024;
		}
		else
		{
			assert(linebuf[0] >= 'A' && linebuf[0] <= 'Z'); // stat key-val line
			char key[sizeof linebuf];
			long num_kb;
			int fields_read = sscanf(linebuf,
				"%[A-Za-z_]: %ld kB\n",
				key, &num_kb);

#define PREFIX_MATCH(s, v) (0 == strncmp((s), (v), sizeof (s) - 1))

			if (PREFIX_MATCH("Size:", linebuf))
			{
				assert(cur_size == num_kb
						|| 0 == PREFIX_MATCH("[stack", rest) &&
						 	(cur_size + (PAGE_SIZE / 1024)) == num_kb);
			}
			else if (PREFIX_MATCH("Rss:", linebuf))
			{
				cur_resident += num_kb;
			}
			else if (PREFIX_MATCH("Shared_Clean:", linebuf))
			{
				cur_shared += num_kb;
			}
			else if (PREFIX_MATCH("Shared_Dirty:", linebuf))
			{
				cur_shared += num_kb;
				cur_dirty += num_kb;
			}
			else if (PREFIX_MATCH("Private_Clean:", linebuf))
			{

			}
			else if (PREFIX_MATCH("Private_Dirty:", linebuf))
			{
				cur_dirty += num_kb;
			}
			else if (PREFIX_MATCH("Referenced:", linebuf))
			{
				cur_referenced += num_kb;
			}
		}
	} // end while read line

	/* flush the last line. */
	flush_mapping_info(verbose, &cur_size, &cur_resident, &cur_dirty, &cur_referenced, &cur_shared, NULL);

	/* flush the totals */
	flush_mapping_info(1, &total_size, &total_resident, &total_dirty, &total_referenced, &total_shared, "totals");

	int tmpfd = open("/proc/self/clear_refs", O_WRONLY);
	if (tmpfd != -1)
	{
		int dummy_ret __attribute__((unused)) = write(tmpfd, "1\n", sizeof "1\n" - 1);
		close(tmpfd);
	}
}

static void print_sample(int ignored)
{
	/* Sometimes, reading smaps can incur a lot of system execution overhead.
	 * (FIXME: understand why.)
	 * So, to compensate and avoid cumulative slowdowns,
	 * we re-set the timerdifference the time we've spent in read_smaps and re-add it on to the timer. */
	struct itimerval cur_value = { (struct timeval) { 0, 0 }, (struct timeval) { 0, 0 } };
	int ret = getitimer(ITIMER_PROF, &cur_value);
	assert(cur_value.it_value.tv_usec > 0 || cur_value.it_value.tv_sec > 0);
	assert(ret == 0);


	/* To keep it simple, for now let's say a sample is
	 * - the /proc/<pid>/maps mapping line
	 * - a few numbers: total size, resident, dirty, shared */
	read_smaps();

	/* Re-set the timer to the value it had when we started. */
	ret = setitimer(ITIMER_PROF, &cur_value, NULL);
	assert(ret == 0);
}

static void print_exit_summary(void)
{
	if (getenv("LIBALLOCS_DUMP_SMAPS_AT_EXIT"))
	{
		char buffer[4096];
		size_t bytes;
		if (smaps_fd)
		{
			while (0 < (bytes = read(smaps_fd, buffer, sizeof(buffer))))
			{
				fwrite(buffer, 1, bytes, stderr);
			}
		}
		else fprintf(stderr, "Couldn't read from smaps!\n");
		fflush(stderr);
	}
}

static void init(void) __attribute__((constructor));
static void init(void)
{
	if (getenv("MEMPROF_DELAY_STARTUP")) sleep(10);

	unsigned long sample_period_us = 1000 * 1000; // default 1s
	/* How to make this work? SIGALRM might be used by the program.
	 * Or the program might use sleep.
	 * We use ITIMER_PROF, because no others profiler is likely to be running.
	 * It uses SIGPROF. */
	struct itimerval new_value = {
		/* current value */ (struct timeval) {
			sample_period_us / (1000 * 1000),
			sample_period_us % (1000 * 1000)
		},
		/* interval a.k.a. reset-to value */ (struct timeval) {
			sample_period_us / (1000 * 1000),
			sample_period_us % (1000 * 1000)
		}
	};

	if (getenv("MEMPROF_OUT"))
	{
		out = fopen(getenv("MEMPROF_OUT"), "w");
		assert(out);
	}
	else
	{
		out = stderr;
	}

	if (getenv("MEMPROF_VERBOSE"))
	{
		if (atoi(getenv("MEMPROF_VERBOSE")) != 0)
		{
			verbose = 1;
		}
	}

	smaps_fd = open("/proc/self/smaps", O_RDONLY);
	assert(smaps_fd != -1);

	struct itimerval old_value = { (struct timeval) { 0, 0 }, (struct timeval) { 0, 0 } };
	int ret = setitimer(ITIMER_PROF, &new_value, &old_value);
	assert(ret == 0);
	//assert(old_value.it_value.tv_sec == 0);
	//assert(old_value.it_value.tv_usec == 0);
	atexit(print_exit_summary);

	struct sigaction old_action = { NULL };
	struct sigaction new_action = {
		.sa_handler = print_sample,
		.sa_mask = 0,
		.sa_flags = 0
	};
	ret = sigaction(SIGPROF, &new_action, &old_action);
	assert(ret == 0);
	assert(old_action.sa_handler == NULL);
}

