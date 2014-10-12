default: libmemprof.so

libmemprof.so: CFLAGS += -fPIC -g

libmemprof.so: libmemprof.c
	$(CC) $(CFLAGS) -shared -o "$@" "$<"
