# darwinator

A FreeBSD kernel module that runs **Mach-O executables** (the program format of macOS) directly, on **arm64 and amd64**.

It is to Mach-O what `linux.ko` is to ELF programs of Linux: an image activator for the file format, plus a second system call table (the Darwin numbers and conventions) that a process gets when it is started from such a file. Native FreeBSD processes are not touched. The module is written for pearOS ([github.com/pearOS-bsd](https://github.com/pearOS-bsd)); `iso/build.sh` of wsbsd can fetch and build it like `virtio_gpu` and `clevo_dchu`.

Written from the public description of the Mach-O format and the system call conventions; no Apple source code is copied.

## State (milestone 1)

| | |
|---|---|
| Mach-O loader (`src/macho.c`) | thin and fat (universal) 64 bit files for the architecture of the kernel; `LC_SEGMENT_64` segments are mapped (`__PAGEZERO` skipped), entry point from `LC_MAIN` or `LC_UNIXTHREAD`, executable stack setup by the kernel (`argc`, `argv`, `envp`) |
| System calls (`src/syscalls.c`) | BSD class: `exit`, `read`, `write`, `close`, `getpid`; everything else fails with `ENOSYS` |
| x86-64 convention (`src/darwin_amd64.c`) | `syscall`, class in the top byte of `rax` (BSD = `0x2000000 + n`), arguments `rdi rsi rdx r10 r8 r9`, carry flag = error |
| arm64 convention (`src/darwin_aarch64.c`) | `svc #0x80`, number in `x16`, arguments `x0-x7`, carry flag = error |
| Not yet | **dyld** (files with `LC_LOAD_DYLINKER` / `LC_LOAD_DYLIB` are refused with a message), Mach traps and Mach IPC (ports, `bootstrap`), `commpage`, `psynch`, signals in the Darwin layout, arm64e (PAC), the other BSD calls (their flags and structures differ from FreeBSD's: open, stat, mmap, sigaction, kevent64 ...) |

The module was **compiled in a syntax check against the 14.5 kernel headers for both architectures**; it has not been loaded on a running kernel yet: the CI job (`.github/workflows/ci.yml`) does that on FreeBSD x86_64 and aarch64 and is the first real test.

## Build and use

On FreeBSD 14 with the kernel sources (`/usr/src/sys`):

```
make SYSDIR=/usr/src/sys
kldload ./darwinator.ko
python3 tools/mkmacho.py --arch x86_64 -o hello      # or --arch arm64, --entry unixthread, --fat
./hello                                              # prints "hello from a Mach-O file", exit status 7
sysctl kern.darwinator.debug=1                       # log why a Mach-O file is refused
sh tests/run.sh                                      # the whole test (root)
kldunload darwinator                                 # EBUSY while a Darwin process lives
```

`tools/mkmacho.py` writes a tiny Mach-O file by hand (no toolchain, no Mac needed). A file built on a Mac works the same if it has no dynamic libraries:

```
clang -nostdlib -Wl,-e,_start -o hello hello.s       # code that calls write/exit with svc #0x80 / syscall
```

## Design

* The system call vector (`darwin_sysvec`) is a copy of the native one (taken from `init` at load time, because it is not exported on arm64): same registers, same stack, same signal page. Replaced: the table, the names, the argument fetching, the ABI flag; no ELF core dump.
* The activator follows `imgact_elf.c`: it returns -1 for files that are not Mach-O (so ELF and `#!` still work), an errno for files that are Mach-O but cannot run. Segments are mapped as anonymous memory and filled from the file, then given their final protection (no mapping is ever writable and executable).
* Only the call numbers and the register convention are Darwin's; the FreeBSD implementation behind a call is used where the arguments and results are the same.

## Next steps

1. Run the CI, fix what the real kernel shows.
2. More BSD calls with their Darwin flags and structures (`open`, `fstat64`, `mmap`, `sigaction`, `sysctl`, `ioctl`) and the errno differences (`sv_errtbl`).
3. `commpage`, thread-local storage (`thread_fast_set_cthread_self`), `psynch` for pthread.
4. Mach traps and Mach IPC (port rights, messages, a bootstrap server) as a part of this module.
5. dyld: the shared region and the dyld shared cache, then a real `dyld`.
