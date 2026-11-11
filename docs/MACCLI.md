# The command line tools of macOS 26.7 (Intel) through machrun

`tools/maccli/sweep.sh` runs every Mach-O program of `/bin /sbin /usr/bin /usr/sbin /usr/libexec` of a real Mac (macOS 26.7.2, x86_64, in QEMU/KVM) through `machrun` on FreeBSD 14.5 amd64, one after the other, as the user `nobody`, with no arguments, no input and 8 seconds each
(1265 programs; the Mac files are universal x86_64 + arm64e).  Results: `tools/maccli/results-macos26.7-intel.tsv` (after) and `results-before-shims.tsv` (before).

| | before | after the shims of this commit |
|---|---|---|
| the program starts (it runs, prints something or ends) | 468 (37%) | **604 (48%)** |
| it calls a symbol that machrun does not have | 692 | 546 |
| it does not load (too many libraries, not for this architecture ...) | 105 | 115 |

"Starts" only means that `machrun` did not stop it: not that the output is right (a tool with no arguments mostly prints its usage text).

What the shims of this commit are (`dyld/shims.c`): `dispatch_once` and `dispatch_once_f`, `malloc_type_malloc/calloc/realloc/valloc/posix_memalign/aligned_alloc` (macOS 15 and later allocate with a type id), `_set_user_dir_suffix`,
`__strlcpy_chk`, `__strlcat_chk`, `CFAbsoluteTimeGetCurrent`, `fcopyfile`, `os_transaction_create`, and "succeeds, does nothing" for `sandbox_init*`, `sandbox_check`, `os_variant_*`, `__os_feature_enabled_impl`, `setiopolicy_np`, `xpc_set_event_stream_handler`, `notify_set_options`.

The symbols that stop most programs now: `xcselect_invoke_xcrun` (95: `git`, `clang`, `make` ... are launchers of Xcode), `_cupsSetLocale` (20), a C++ operator new with a type descriptor (16), `snmp_parse_args` (12), the Swift runtime (`swift_task_create`, `swift_getSingletonMetadata` ...),
`xpc_connection_create_mach_service` and `vproc_swap_integer` (launchd and XPC), `IOServiceMatching` (IOKit), `krb5_init_context`, `apr_app_initialize`, `sasl_version`.

## The same 1262 tools through the real dyld (`kern.darwinator.dyld=1`, `tools/maccli/sweep-dyld.sh`)

The tools run with the dyld and the libSystem of the Mac itself (the module of this repository, amd64), as `nobody`, with no arguments and no input, 8 seconds each.  The results are in `tools/maccli/results-dyld-macos26.7-intel.tsv`.

| | machrun (shims) | the real dyld |
|---|---|---|
| the program ends or runs for the 8 seconds (any exit status, mostly the usage text or an error about the missing arguments) | 604 | **932** |
| the program is killed by a signal | | 330 (267 SIGILL, 33 SIGUSR-like 127, 17 SIGABRT, 7 SIGSEGV) |

How it went: 900 tools were killed at the first run, 617 after the Mach traps `thread_get_special_reply_port` and `host_create_mach_voucher_trap` and the answer of the bootstrap port, 267 after `proc_info` (flavor 17) for libdispatch.  The tools that are left die in libdispatch (the work queue: `workq_kernreturn` is not a real work queue), in libxpc (check-ins and routines of launchd other than the look-up, ids 0x324 and 0x325) or because a library of the Mac is not on the machine.

## The 267 tools that died with SIGILL, compared with the Mac (2026-11-11)

The list of 267 (the tools that ended on SIGILL in the first full sweep) was run on the pearOS VM (real dyld, `nobody`, no arguments, 8 s) and on a Mac (Tahoe 26.7.2, Intel VM, 5 s).  Same exit class as the Mac's: 43 before the fixes of this day, **127 of 265** after them.

| class | before | after |
|---|---|---|
| SIGILL | 267 | 44 (34 of them do not die on the Mac) |
| timeout (daemons that wait, as on the Mac) | 18 | 190 |
| exit 1 (usage) | 4 | 21 |

The fixes: the features of `bsdthread_register` (kevent, workloop), the Mach traps 22/23/43/76 and `mk_timer` (91-95), `EVFILT_USER` no longer swallowed (only the workloop filter -17), the trailer of received messages (audit token; the launchd is pid 1), the threads of libpthread (TSD base, `MACH_THREAD_SELF`), and the kqueue table (64 slots) that was not freed when a process exited, which made the tools after the 64th die.  What is left: services that need the launchd (`com.apple.bsd.dirhelper` for `confstr(_CS_DARWIN_USER_DIR)`, check-in of daemons) and a few signature checks (`platformBinary`).
