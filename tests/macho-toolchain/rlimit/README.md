# rlimit

`rlimit`: `getrlimit(RLIMIT_NOFILE)`, `sysconf(_SC_STREAM_MAX)` and `fopen` through the Mac's libc.  The libc's `getrlimit` is the UNIX03 variant: it adds `_RLIMIT_POSIX_FLAG` (0x1000) to the resource and XNU takes it away; darwinator refused it (resource > 8), `sysconf(_SC_STREAM_MAX)` became -1 and every `fopen` of a Mac program failed with `EMFILE` (found with Metal's `tests/art` on pearOS, 2026-11-28).  Build: `sh build.sh OUT`; run with `kern.darwinator.dyld=1`: exit 0 and `rlimit ok`.
