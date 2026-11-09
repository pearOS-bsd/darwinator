# iOS 16.7.16 command line tools on FreeBSD arm64

The root file system of an iPhone X restore (iOS 16.7.16, `iPhone10,3_16.7.16_20H392`, HFS+ image) holds real Darwin arm64 programs.  They were run with `machrun` on FreeBSD 14.5 arm64 (QEMU, TCG), without the kernel module:

| program | imports | result |
|---|---|---|
| `/usr/bin/vm_stat` | libSystem | **runs**: the page counts are those of the FreeBSD VM (`vm.stats.vm`) |
| `/bin/df` | libSystem, libutil | **runs**: the real mounts of the FreeBSD VM (`getmntinfo` with Darwin's `struct statfs`, and `getattrlist` for ATTR_VOL_SPACEUSED) |
| `/bin/ps` | libSystem | **runs**: the process list through `sysctl KERN_PROC` and `KERN_PROCARGS2` in Darwin's layout (kinfo_proc of 648 bytes) |
| `/usr/bin/taskinfo` | CoreFoundation, libSystem | starts; stops at `processor_set_default` (it needs the real task and processor set ports) |
| `/usr/bin/ltop`, `/usr/libexec/cc_fips_test`, `/usr/sbin/aslmanager`, `/usr/libexec/tzlinkd` | libSystem | start and stop at a missing private symbol (`ledger`, `cc_clear`, `vproc_swap_integer`, `xpc_track_activity`) |
| `/usr/sbin/rtadvd`, `/usr/libexec/dirs_cleaner`, `/sbin/mount_lifs` | libSystem | start and print their usage text |
| `/usr/libexec/tzinit` | libc++ | crashes: Apple's libc++ objects meet FreeBSD's libc++ (not supported) |

What had to be added to `machrun` for this (all in `dyld/shims.c`):

* **The arm64 Darwin way of passing variable arguments.**  Darwin arm64 puts the variadic arguments of a call on the stack; FreeBSD's AAPCS64 puts them in registers.  A thunk per variadic function (`printf` family, `warn/err`, `syslog`, `sscanf`, `ioctl`, `open`, `openat`, `fcntl`, `__sprintf_chk`, `__snprintf_chk`) gives the stack pointer of the call to a C function that builds a `va_list` without a register save area.  Without it `vm_stat` printed addresses instead of numbers.
* `mach_host_self`, `host_statistics64` (VM_INFO64, VM_INFO, CPU_LOAD_INFO), `host_page_size`, `vm_page_size`, `vm_kernel_page_size`, `mach_task_self_`, `mach_error`, `mach_error_string`, `compat_mode`, and KERN_FAILURE or `ENOTSUP` for the task, thread, processor set and `proc_*` calls that a FreeBSD process cannot answer.

How to repeat it: mount the HFS+ image read-only (`mount -t hfsplus -o ro,loop,force`), copy the programs to a FreeBSD arm64 machine, build `dyld/` there (`make machrun`) and run `./machrun ./vm_stat`.
