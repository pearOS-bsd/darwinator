# shims
fastsleep.c: DYLD_INSERT_LIBRARIES interposer (not honoured by dyld on pearOS yet); the working way is the sysctl `kern.darwinator.fast_sleep_ms` (sleep(3) of 1..5 s lasts that many ms).
