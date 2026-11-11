# Real dyld and real libSystem of a Mac on the module

`kern.darwinator.dyld=1` makes `darwinator.ko` run a dynamic Mach-O program the way macOS does: the kernel maps the program, maps the `dyld` that the file names (`LC_LOAD_DYLINKER`, `/usr/lib/dyld`), builds the stack that dyld expects and starts dyld.  dyld maps the dyld shared cache and
initializes the real libSystem, libdispatch and libobjc.  Nothing of Apple's is in this repository: the files come from a Mac that you own (see "What has to be on the machine").

    sysctl kern.darwinator.dyld=1
    /path/of/a/mac/bin/echo hello          # the original macOS echo prints: hello
    /path/of/a/mac/bin/date                # Mon Nov  9 19:20:47 UTC 2026

(amd64 only for now; a Mac with Intel or Rosetta files; tested with macOS 26.7.2 files.)

## What has to be on the machine

* `/usr/lib/dyld` (thin x86_64 slice of the Mac's file)
* the dyld shared cache at the path that dyld looks in: `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h` and its sub caches `.01` ... `.06` (about 5 GB; `/System/Cryptexes/OS` is a link to the same place)
* the Mac program that is run

## What the kernel does for it

| | |
|---|---|
| exec | the Mach-O of the program is mapped (slide 0), dyld is mapped at `0x7ff700000000`, the stack is `[mach_header][argc][argv][envp][apple]` with `executable_path`, `ptr_munge`, `stack_guard`, `malloc_entropy`, `th_port`; the stack of a Darwin process is at `0x7ff7c0000000` (`src/macho.c`, `src/darwinator.c`) |
| comm page | `0x7fffffe00000`, read only: the CPU capabilities, the number of CPUs, the page shift and the time base (`nt_tsc_base`, `nt_scale`: `mach_absolute_time` runs on the TSC of the machine); the layout is that of xnu's `cpu_capabilities.h` and the bytes of the CPU part were read on a Mac (`src/darwin_amd64.c`) |
| shared region | `shared_region_check_np` (294) and `shared_region_map_and_slide_2_np` (536): the 7 files of the cache are mapped at their addresses plus the slide that dyld asks for, the pointers of the data mappings are moved with the slide info of version 2, the dynamic data (`fd -1`) is copied (`src/syscalls_dyld.c`) |
| Mach messages | `mach_msg2_trap` (47), the form that libsyscall uses since macOS 12: the header comes in registers, the kernel puts it in the memory and uses the classic path; `mach_port_construct/destruct` (24, 25), `thread_get_special_reply_port` (50), `host_create_mach_voucher_trap` (70) |
| kernel as a server (MIG) | `host_info` with the values of a Mac (BASIC, SCHED, PRIORITY: the CPU subtype 8 makes dyld choose the `x86_64h` cache), `host_get_clock_service` and `clock_get_time`, `task_info` (TASK_AUDIT_TOKEN), `task_restartable_ranges_register` |
| launchd | the bootstrap port of a process is a port that the kernel answers for: the requests of libxpc (XPC messages, id 0xcf) get an empty dictionary, which means "unknown service" (`bootstrap_server` in `src/mach_ipc.c`) |
| a few calls | `fcntl` F_ADDFILESIGS_RETURN (code signatures are not checked), `mach_vm_map` with an alignment mask, `mach_host_self` and the other self traps give a new send right every time |

## How the work is done

`kern.darwinator.debug=1` logs what is not implemented; `=2` logs every system call (with the paths, the `fcntl` commands, the names of `sysctl`), every MIG request and every mach_msg2.  `kern.darwinator.stop=1` stops a Mach-O program before its first
instruction so that `lldb -p PID` can attach (`process handle SIGSTOP -p false -s false -n false`, `continue`).  The addresses of the cache are looked up with `ipsw dyld a2s` (the slide is the one that dyld logs).
`tools/maccli/sweep-dyld.sh` runs every tool of a list through it.

## Update: libdispatch start-up on the real dyld

- `bsdthread_register` now returns `0x400000ff` (adds the KEVENT and WORKLOOP features): libdispatch aborted in `_dispatch_root_queues_init_once` without them.
- Mach traps added: 22/23 (port insert/extract member), 43 (`mach_generate_activity_id`), 76 (`mach_port_type`), 91-95 (`mk_timer_*`, a port that receives a message at its time, through a timeout task).
- `kevent_id` with an `EVFILT_WORKLOOP` change succeeds without events (there is no kernel workqueue).
- `task_get_special_port` of a valid port that is not set answers success with a null port.
- The bootstrap routines 0x320-0x325 answer `{"error": ESRCH}`.
- Result: `archiveutil` runs to the end. The other tools that died with SIGILL now stop in libxpc (`_xpc_connection_init_failed`) when a service such as cfprefsd is needed: this is the launchd work (a real bootstrap with services), not the kernel.
