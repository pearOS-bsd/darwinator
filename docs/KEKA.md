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

## After machrun (the userland dyld, `dyld/`)

`machrun PROGRAM [args]` maps the Mach-O file, applies rebase and bind, binds the imports to FreeBSD's libraries (`dyld/shims.c` translates what differs), runs the initializers and calls `main()`. Nothing in the kernel is needed. Result with the unmodified files of Keka 1.6.8 (x86-64 slice), `tests/keka.sh`, FreeBSD 14.5 amd64:

| File | Result |
|---|---|
| `kekaxz` (xz 5.8.4) | **works**: compresses (also with 4 threads), decompresses, lists, tests; output is read by the system `xz` and the other way round |
| `kekazstd` (zstd 1.5.7), `kekalz4` (lz4 1.10.0) | **work**: round trips with the system `zstd`, and lz4 |
| `keka7zz` (7-Zip 26.03, "modified by aone for Keka") | **works**: creates, lists, tests and extracts `7z` and `zip` archives (the extracted tree is identical, the system `unzip` accepts the zip); needed the CoreFoundation string subset (`dyld/cf.c`), `fstatat`, `readdir`, `setxattr` and the other shims |
| `kekatar` (bsdtar 3.8.9, libarchive) | **works**: creates `ustar`, `pax`, `gnutar`, `zip`, `7zip` archives (read by the system `tar`, extracted again to an identical tree), `-z` `-j` `-J` filters, extracts a system tar, creates and decrypts an **AES-256 zip** (the wrong passphrase is refused). Needed CommonCrypto on `libmd`/`libcrypto` (`dyld/cc.c`: MD5, SHA-1/256/384/512, HMAC, PBKDF2, AES ECB/CBC/CTR), `iconv`, `statfs`, `utimes`, and stubs for `copyfile`, the extended attribute calls and the membership (ACL) calls; `libxml2` comes from the package |
| `Keka` (the application) | maps, binds and **reaches `main()`**, then stops at the first `objc_msgSend`: 438 unresolved symbols (139 Objective-C classes and 131 metaclasses of Foundation/AppKit/Sparkle, 38 `NS*` constants, 32 `libobjc` functions, CoreFoundation, blocks) |

Things the shims had to fix, found by running real programs (`struct statfs` is 2344 bytes on FreeBSD and 2168 on Darwin: the plain call overflowed the program's buffer and the stack guard stopped it): the entries of FreeBSD's `_DefaultRuneLocale.__runetype` are 64 bits wide (Darwin's 32: every `isprint()` macro of the program read the wrong entry), `struct dirent` and `struct stat`, `AT_FDCWD` (-2 on Darwin) and the `AT_*` bits, `pthread_attr` detach state, pthread locks and `pthread_once` with Darwin's static initializers, `sigaction` and `sigset_t`, `sysctl` names, `open`/`fcntl` flags.

Not done: arm64 (and there the variadic calling convention of Darwin's arm64 differs from the standard one: `printf` and friends need their own shims), chained fixups, thread-local variables, `dlopen` of Mach-O dylibs.

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

1. ~~Userland dyld + libc shims for `kekaxz`, `keka7zz`~~ (done, see above). `kekatar` too (done). Next: the Objective-C 2 metadata loader for the application.
2. The Objective-C metadata loader, tested first with a small Objective-C test program, then the Keka application.
3. Mach IPC and `psynch` in the module only when a program really needs them.
