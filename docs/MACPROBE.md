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

## The probe, second run (process structures) and the trace of Keka

The offsets of `kinfo_proc` (`extern_proc` 296 bytes: `p_flag` 32, `p_stat` 36, `p_pid` 40, `p_comm` 243, `p_pgrp` 264; `eproc` 352 bytes: `e_pcred` 16, `e_ucred` 120, `e_ppid` 264, `e_login` 320),
of `proc_bsdinfo` (136) and `proc_taskinfo` (96) were read from `reference-macos27-arm64.txt` and are used by `src/syscalls_proc.c`.

`dtruss` of Keka (SIP off; 14043 calls, 53 distinct) showed what a real application asks of the kernel: `kdebug_trace64` and `kdebug_trace_string` (11000 of 14000: tracing, here accepted and ignored), `bsdthread_ctl`,
`workq_kernreturn` (answers 0), `gettid` (ESRCH on the Mac: fixed, it was EINVAL here), `kevent_id` and `kevent_qos` (the workloops and the queues of libdispatch: implemented), `ulock_wait2`, `psynch_mutexwait`
and `psynch_mutexdrop`, `getattrlist`, `fstatfs64`, `getdirentries64`, `crossarch_trap` (ENOTSUP, as on the Mac), `csrctl` (EPERM), `__mac_syscall` (0), `fsgetpath` (found a path on the Mac: ENOTSUP here), `proc_info`.

## An Intel Mac (macOS 26.7.2, x86_64, in QEMU/KVM with OSX-KVM): `tools/macprobe/intel/`

`probe.sh`, `probe2.sh`, `probe3.sh`, `probe4.sh` were run on macOS 26.7.2 (Tahoe, xnu-12377, Intel) and the results are in `tools/macprobe/intel/`.  The comparison with the Apple silicon result (`reference-macos27-arm64*.txt`):

* **The same on both architectures** (so the arm64 reads are good for amd64 too): `posix_spawn` (all the cases of `probe2`), the named semaphores, `access_extended` (including the `_READ_OK` bits), `statfs` and the old `statfs` (157), `sizeof` of `kinfo_proc` (648), `extern_proc` (296), `eproc` (352), `proc_bsdinfo` (136), `proc_taskinfo` (96), `proc_taskallinfo` (232).
* **Differs, and darwinator already has the Intel value:** `hw.pagesize` 4096 (16384 on arm64), the timebase is 1/1 (125/3 on arm64), `getdirentries` and the number of entries of `/` are those of the file system.
* **The x86-64 specifics of `probe4`:**
  * the system call convention is what darwinator does: class 2 in the top byte of rax, the carry flag is the error flag with errno in rax, `pipe` gives the two descriptors in rax and rdx, `fork` gives rdx 0 in the parent and 1 in the child, `getpid` has no error;
  * the registers of `x86_THREAD_STATE64`: rax rbx rcx rdx rdi rsi rbp rsp r8-r15 rip rflags cs fs gs (21 registers, 168 bytes, count 42), `x86_FLOAT_STATE64` has count 131 (524 bytes), `x86_EXCEPTION_STATE64` count 4 (16 bytes); the flavors are 4, 5, 6 and `x86_THREAD_STATE` is 7;
  * **the signal frame differs in two details from what darwinator builds:** `sizeof(ucontext_t)` is 56 and `sizeof(mcontext64)` 712 as in darwinator, but the real kernel reports `uc_mcsize` 1032 (the AVX state is part of the context) and puts the mcontext *below* the ucontext (`uc_mcontext - uc` is -1136), where darwinator puts it right above (+56) with `uc_mcsize` 712 and no floating point state.  A handler that follows the pointer is not affected; one that reads `uc_mcsize` or the floating point registers is.  `fcw` is 0x37f and `mxcsr` 0x1f80 in the handler, `rflags` 0x206, `cs` 7, `fs` and `gs` 0, `trapno` 0x85 and `err` 0x2000025 (the system call that sent the signal).

### The MIG ids, checked on the real Mac (`probe5`, `tools/macprobe/intel/probe5.txt`)

The message ids that darwinator answers (`tools/migids.py` on the `.defs` of xnu) are found as immediate values in the code of the user stubs of the real libSystem (`_kernelrpc_*`): **34 of 36 are confirmed**, among them `host_info` 200, `_host_page_size` 202, `host_statistics` 216, `host_statistics64` 219, `mach_port_names` 3200 ... `mach_port_set_context` 3229, `task_threads` 3402, `task_info` 3405, `task_get_special_port` 3409, `semaphore_create` 3418, `thread_info` 3612, `thread_get_state` 3603, `mach_vm_read` 4804, `mach_vm_write` 4806, `mach_vm_read_overwrite` 4808, `mach_vm_region` 4816.  `mach_ports_register` (3403) and `mach_ports_lookup` (3404) are not exported by libSystem and stay unchecked.  This only checks the ids, not the layout of the messages: a process that sends MIG messages by hand to the task or host port is killed on a recent macOS (EXC_GUARD), so the layouts were not tried that way.
