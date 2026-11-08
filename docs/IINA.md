# IINA and Mach-O dylibs

IINA 1.5.1 (the official `.dmg`, x86_64) is the next test for `machrun`: a Swift application that carries about 60 dylibs (libmpv, FFmpeg, libass, libplacebo ...).

## Mach-O dylibs (done)

`machrun` loads the Mach-O dylibs that a program or another dylib needs (`@rpath`, `@loader_path`, `@executable_path`, paths of the bundle): it maps them, resolves what they import against each
other (the export trie, re-exports of whole libraries and of single symbols, flat and self lookups), applies rebase, bind and chained fixups, sets up thread-local variables (`__thread`: libjpeg, LuaJIT),
sets up their Objective-C metadata, and runs their initializers, the dylibs first. A library of this system (`/usr/lib/...`, `/System/...`) is still a FreeBSD library (shims).
Sparkle stays a stub (`MACHRUN_REAL_SPARKLE=1` loads the real one).

Library mode, for testing a dylib without a program: `machrun -L LIB.dylib [-L ...] -x SCRIPT` loads the dylibs and runs a script of calls (`$1 = calloc(1, 8)`, `avformat_open_input($1, "a.mkv", 0, 0)`,
`peek(addr, off)`, `peek32`, strings, numbers, reals, results `$N`).

`tests/dylib-ffmpeg.sh IINA.app/Contents/Frameworks` (6 checks): the FFmpeg dylibs of IINA (libavformat, libavcodec, libavutil and what they need: 46 libraries) open a Matroska file, find the video stream and
decode a 320x240 frame, on FreeBSD, from the unmodified Mach-O files.

## Where IINA stops: Swift

- `libmpv.2.dylib` of IINA has mpv's Swift code (the macOS parts: `swift.Application` ...) and calls `swift_once` already in `mpv_create`.
- IINA itself binds 8,400 symbols of libswiftCore, libswiftFoundation, SwiftUI and the other Swift overlays (27 libraries from `/usr/lib/swift/`, which are in the dyld shared cache of macOS and not in the `.dmg`).
- The Swift runtime of FreeBSD has no Objective-C interoperability, and SwiftUI, Combine, CryptoKit, VideoToolbox ... exist only on macOS.

So IINA does not start; what works is everything below it. ravynOS (Apple's dyld, objc4, its own Foundation and AppKit) has no Swift runtime either.
