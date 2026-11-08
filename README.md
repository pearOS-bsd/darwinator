# darwinator

A FreeBSD kernel module that runs **Mach-O executables** (the program format of macOS) directly, on **arm64 and amd64**.

It is to Mach-O what `linux.ko` is to ELF programs of Linux: an image activator for the file format, plus a second system call table (the Darwin numbers and conventions) that a process gets when it is started from such a file. Native FreeBSD processes are not touched. The module is written for pearOS ([github.com/pearOS-bsd](https://github.com/pearOS-bsd)); `iso/build.sh` of wsbsd can fetch and build it like `virtio_gpu` and `clevo_dchu`.

Written from the public description of the Mach-O format and the system call conventions; no Apple source code is copied.

![machsh, a Mach-O program, in the Terminal of pearOS](docs/screenshot-machsh-crop.png)

*`file` on a Mach-O program, then the program (`tests/c/machsh.c`) running interactively in the Terminal of the pearOS image (QEMU/KVM): it reads and writes files, maps memory, asks the time and the kernel's entropy with Darwin system calls only.*

![Keka 1.6.8 (the unmodified Mac application) running in pearOS](docs/screenshot-keka-preferences.png)

*The Preferences window of Keka, the original Mach-O application from its release, started through `machrun` in the pearOS image: GNUstep's AppKit on the window server, Apple's compiled nibs, Objective-C classes of the program in libobjc2.*

![AppCleaner 3.7 (the unmodified Mac application) running in pearOS](docs/screenshot-appcleaner.png)

*AppCleaner 3.7, an original Mach-O application of macOS 15 (Objective-C, chained fixups, Xcode 16 nibs, Sparkle), in its main window in the pearOS image. It starts, finds the files that an application leaves in `~/Library` and removes them (`tests/appcleaner.sh`, 13 checks); the window shows the list of files (the rows still show the description of the object instead of the names: `docs/APPCLEANER.md`).*

## State

| | |
|---|---|
| Mach-O loader (`src/macho.c`) | thin and fat (universal) 64 bit files for the architecture of the kernel; `LC_SEGMENT_64` segments are mapped (`__PAGEZERO` skipped), entry point from `LC_MAIN` or `LC_UNIXTHREAD`, executable stack setup by the kernel (`argc`, `argv`, `envp`) |
| System calls (`src/syscalls.c`) | BSD class, 36 calls: `exit read write open close unlink chdir getpid getuid geteuid access kill getppid dup getegid getgid umask munmap mprotect dup2 fsync gettimeofday readv writev mkdir rmdir pread pwrite mmap lseek ftruncate issetugid stat64 fstat64 lstat64 getentropy`; `open` (flags), `mmap` (flags), `stat64` (structure) and errno numbers are translated to Darwin's, the rest goes straight to FreeBSD's code. Machdep class: `thread_fast_set_cthread_self` (amd64). Everything else fails with `ENOSYS` |
| x86-64 convention (`src/darwin_amd64.c`) | `syscall`, class in the top byte of `rax` (BSD = `0x2000000 + n`), arguments `rdi rsi rdx r10 r8 r9`, carry flag = error |
| arm64 convention (`src/darwin_aarch64.c`) | `svc #0x80`, number in `x16`, arguments `x0-x7`, carry flag = error |
| Test programs | `tests/c/machsh.c`: an interactive shell written as a Mach-O program (calculator, files, `stat64`, `mmap`, time, entropy, thread pointer), built on FreeBSD with clang + lld and `tools/elf2macho.py` (`tests/build-c.sh`); `tools/mkmacho.py` writes the minimal ones by hand |
| Not yet | **dyld** (files with `LC_LOAD_DYLINKER` / `LC_LOAD_DYLIB` are refused with a message), arm64 thread pointer (TPIDRRO_EL0), Mach traps and Mach IPC (ports, `bootstrap`), `commpage`, `psynch`, signals in the Darwin layout, arm64e (PAC), the other BSD calls (their flags and structures differ from FreeBSD's: open, stat, mmap, sigaction, kevent64 ...) |

Tested on FreeBSD 14.5 amd64 under KVM (`tools/vm-test.sh`): the module builds with `-Werror`, loads, runs the Mach-O test files (`LC_MAIN`, `LC_UNIXTHREAD`, a universal file), refuses a file of the other architecture, and ELF programs and scripts still run; `kldunload` works. **arm64 compiles (syntax check against the 14.5 headers) but has not been run yet** (needs an arm64 FreeBSD: the Mac's HVF VM, or CI). The GitHub-hosted FreeBSD VMs of the CI job (`.github/workflows/ci.yml`) were unreachable at the time of writing (HTTP 504 when the action downloads its VM image).

## machrun: a dyld for FreeBSD (`dyld/`)

Mach-O programs that need dyld and libSystem (nearly every Mac program) run through `machrun`, a userland loader that maps the file, applies rebase/bind and binds the imports to FreeBSD's libc and libraries, with shims for what differs (`dyld/shims.c`, `dyld/cf.c`). No kernel module is needed for this path. The unmodified command line tools of Keka (xz, zstd, lz4, 7-Zip and bsdtar, with 37 checks) run and produce archives that the system tools read: see `docs/KEKA.md` and `tests/keka.sh`.

```
make -C dyld                      # builds dyld/machrun (FreeBSD)
dyld/machrun [-v] PROGRAM [args]  # -v: what is bound and what is missing
KEKA_DIR=.../Keka.app/Contents/MacOS sh tests/keka.sh
```

XLD, a converter of audio files with plug-ins that are Mach-O bundles (`docs/XLD.md`, `tests/xld.sh`), converts WAV to FLAC and back, byte for byte. Dylibs that are Mach-O files (the frameworks of an application: FFmpeg, libmpv ...) are loaded too, with their dependencies, thread-local variables and initializers; `machrun -L LIB.dylib -x SCRIPT` calls their functions (`docs/IINA.md`, `tests/dylib-ffmpeg.sh`).

What recent applications need beyond that (AppCleaner is the test): `LC_DYLD_CHAINED_FIXUPS`, nibs in the `NIBArchive` format of Xcode 16 (converted to the keyed archive that GNUstep loads, `compat.m`), the constant literals of the compiler (`@[...]`, `@{...}`, `@1` as structures in the file), direct references to the program's own classes, `objc_alloc`, `objc_opt_new` and the other fast paths of newer compilers, libdispatch objects that the compiler retains with `objc_retain`, CoreGraphics and CoreFoundation from libopal and GNUstep corebase, and stand-ins for Sparkle, SF Symbols and asset catalogs.

## In the pearOS image

`wsbsd/iso/build.sh` fetches this repository, builds `dyld/` (machrun, `libmachcompat.so`, `libexctrace.so`) and the kernel module, and installs them: `/usr/local/bin/machrun`, `/usr/local/bin/macapp`, `/usr/local/libexec/wrapapp.sh`, `/usr/local/lib/libmachcompat.so`, `/boot/modules/darwinator.ko` (not loaded by default). `loader.conf` loads `imgact_binmisc`; the rc script `wsbsd_machrun` registers the Mach-O magic numbers (thin and universal) with it, so a Mach-O executable runs like any program (`./kekaxz --version`); `macapp /path/App.app` starts a whole application (the bundle is wrapped in `~/Library/Caches/machrun`, as GNUstep wants it). `machrun` sets what Objective-C programs need by itself (`MACHRUN_LENIENT`, `MALLOC_CONF=junk:true`; `MACHRUN_STRICT=1` turns the first off).

## Build and use

On FreeBSD 14 with the kernel sources (`/usr/src/sys`):

```
make SYSDIR=/usr/src/sys
kldload ./darwinator.ko
python3 tools/mkmacho.py --arch x86_64 -o hello      # or --arch arm64, --entry unixthread, --fat
./hello                                              # prints "hello from a Mach-O file", exit status 7
sysctl kern.darwinator.debug=1                       # log why a Mach-O file is refused
sh tests/run.sh                                      # the whole test (root)
sh tools/vm-test.sh                                  # the same, in the local KVM VM (~/fbsd-vm), from the development machine
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
4. arm64 thread pointer (TPIDRRO_EL0), Mach traps and Mach IPC (port rights, messages, a bootstrap server) as a part of this module.
5. dyld: the shared region and the dyld shared cache, then a real `dyld`.
