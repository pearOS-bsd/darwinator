# What a real Mac says (macOS 27.0, Apple M1)

`tools/macprobe/probe.c` was run on a MacBook Air (M1, macOS 27.0, Darwin 27.0.0); the output is `tools/macprobe/reference-macos27-arm64.txt`. It was compared with what `darwinator`
assumed (`src/darwin_abi.h` and the system calls):

**Matches** (nothing to change): the size of `struct stat` (144), `struct statfs` (2168), `struct termios` (72), `struct kevent` (32) and `kevent64_s` (48), `struct sigaction` (16 out, 24 in), `FTSENT`
(112, `fts_statp` at 96, `fts_name` at 104), `struct dirent` (`d_seekoff` 8, `d_namlen` 18, `d_type` 20, `d_name` 21), `sockaddr_in6` (family at offset 1, `AF_INET6` 30), the encoding of `st_rdev`
(major << 24 | minor), `AT_FDCWD` -2 and the `AT_*` flags, `O_*`, `F_*`, `MAP_*`, `MS_*`, `SA_*`, the `EVFILT_*` numbers (`EVFILT_USER` -10), the `NOTE_*` time units, the errno numbers 80 to 105,
`SO_NOSIGPIPE` 0x1022, `MSG_NOSIGNAL`, `TIOCGETA`/`TIOCSETA`, the Mach return codes (`MACH_RCV_TIMED_OUT` 0x10004003, `MACH_SEND_INVALID_DEST` 0x10000003), `getattrlist` (length + objtype), extended
attributes (`ENOATTR` 93, the list is NUL separated), `F_GETPATH`, and the Darwin `ucontext` (56 bytes, `uc_mcontext` right after it: what the signal frame of `darwinator.ko` builds on amd64; the size of
`mcontext64` is 816 on arm64, 712 on x86-64).

**Differences that were found and fixed**
* `hw.pagesize`, `hw.cachelinesize`, `hw.l1dcachesize`, `hw.l2cachesize` are 8 bytes with `sysctlbyname` on the Mac (4 bytes here: a program that reads a `long` would have had half of it undefined).
  Now 8 bytes, and 4 for a caller that only has room for 4.
* `kern.osrev` does not exist on the Mac (ENOENT): removed.

**Differences that stay** (they are the hardware or the system, not the ABI): `hw.tbfrequency` is 24 MHz and the time base 125/3 on Apple silicon (here 1/1 nanoseconds and 1 GHz: consistent with each other),
the page size is 16 KB, `kern.version`, the Mach port names (`task_self` 0x203, `host_self` 0x1d03 on the Mac), `RLIMIT_NOFILE` (2560, unlimited maximum), `getdirentries` entries padded to 32 bytes
(ours are 4-aligned: the program only uses `d_reclen`), `siginfo_t` is 104 bytes on the Mac (80 here: the fields up to `si_addr` are the same).

`dtruss` cannot start applications with SIP on ("Operation not permitted"), so no trace of the applications' system calls was made.
