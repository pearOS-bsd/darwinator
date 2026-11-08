# Keka 1.6.8 on darwinator: what happens and what is missing

Experiment (2026-11-08): the release `Keka-1.6.8.dmg` of https://github.com/aonez/Keka (GPL application, downloaded from its GitHub release, not stored in this repository) was unpacked and its Mach-O files were started on FreeBSD 14.5 amd64 with `darwinator.ko` loaded and `kern.darwinator.debug=1`.

## Result

Every file is refused at `execve` (`Exec format error`), and the module says why:

```
darwinator:   needs linker /usr/lib/dyld
darwinator:   needs library /usr/lib/libSystem.B.dylib
darwinator: /root/keka/kekaxz: refused, it needs dyld and its libraries
```

(`Keka` itself lists Cocoa, AppKit, Foundation, CoreFoundation, CoreGraphics, CoreServices, QuartzCore, ImageIO, Security, IOKit, WebKit, AudioToolbox, FinderSync, UniformTypeIdentifiers, libobjc, libSystem and `@rpath/Sparkle.framework`.)

The files are universal (x86_64 + arm64, plain arm64: no arm64e/PAC), so the slice selection works; what stops them is that none of them is a static program: every Mac program asks the kernel to run `/usr/lib/dyld` and needs `libSystem`.

## The files, from the simplest

| File | Needs | Size of the gap |
|---|---|---|
| `kekaxz`, `kekazstd`, `kekalz4`... (the compressors) | `libSystem` (and `libz`) | 91 imported symbols for `kekaxz`: stdio, `pthread_*`, `getopt_long`, `sigaction`, `poll`, `sysctl`, `setlocale`, `fstat$INODE64`... No Objective-C |
| `kekatar` | `libSystem`, `libz`, `libbz2`, `libxml2`, `libexpat`, `libiconv` | the same, plus those libraries (open source: they exist on FreeBSD) |
| `keka7zz`, `7z.so` (7-Zip) | `libSystem`, `libc++`, `CoreFoundation` | C++ runtime and a small part of CoreFoundation |
| `Keka` (the application) | the whole Cocoa stack | about 1470 CoreFoundation symbols (almost all `__CFConstantStringClassReference`, the `@"..."` literals), 313 `libobjc`, 84 AppKit, 42 Foundation, 12 Security, 5 QuartzCore, 4 Sparkle; 151 Objective-C classes imported (NSApplication, NSWindow, NSOpenPanel, NSPopover, CAEmitterLayer...), 188 classes defined by the application in Objective-C 2 metadata (`__objc_classlist` ...) |

The binaries use the classic `LC_DYLD_INFO_ONLY` bind tables with `dyld_stub_binder` (built for an older macOS target); a program built today with chained fixups (`LC_DYLD_CHAINED_FIXUPS`) needs the other format as well.

## What is needed

1. **dyld** (the next step): a loader for the dependency graph: `LC_LOAD_DYLIB`, `@rpath`, rebase and bind (opcode streams and chained fixups), lazy binding (`dyld_stub_binder`), initializers (`__mod_init_func`), thread-local variables, `LC_MAIN` start with the right stack (`mach_header`, `argc`, `argv`, `envp`, `apple[]`). Two ways:
   * **a userland dyld that binds Mach-O imports to FreeBSD's libc** (a table of symbol shims: `_fopen$DARWIN_EXTSN` -> `fopen`, `___stdoutp` -> `stdout`, `fstat$INODE64` -> a function with the Darwin `stat` layout, ...): the quickest way to run the command line tools of Keka, no change in the kernel;
   * **Apple's own `libSystem`** built from the open source releases (Libc, libpthread, libplatform, libdispatch, dyld, xnu `libsyscall`): needs the kernel side to be real: `commpage`, `psynch_*`, `bsdthread_create/register`, Mach traps and the Mach IPC that `libSystem` initialisation uses (`mach_init`, `bootstrap`), `sigaction` with the Darwin signal frame, `sysctl`, `fcntl`, `ioctl`.
2. **Foundation, AppKit, CoreFoundation, CoreGraphics, QuartzCore** for `Keka`: bind the imports to what wsbsd already has (GNUstep, `libwsbsdcompat`, the window server that speaks the SkyLight protocol): this needs a loader of **Objective-C 2 metadata** (turn the 188 classes and their categories and protocols of the Mach-O into libobjc2 runtime calls, fix the selector and class references) and the CoreFoundation constant strings as `NSConstantString`.
3. Smaller items: `Sparkle.framework` (an updater: can be stubbed), the FinderSync extension (`PlugIns/`, ignored), code signature (not checked), the images in the `.car` and `.icns` files (already handled by the wsbsd desktop).

## Suggested order

1. Userland dyld + libc shims, in this repository, for `kekaxz` (decompress a `.xz` file). Then `kekatar`, then `keka7zz` (needs libc++ and a few CoreFoundation functions).
2. The Objective-C metadata loader, tested first with a small Objective-C test program, then the Keka application.
3. Mach IPC and `psynch` in the module only when a program really needs them.
