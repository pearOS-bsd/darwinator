# The Unarchiver 3.11 on pearOS

The Unarchiver 3.11.1 (theunarchiver.com, the original `TheUnarchiver.zip` of the site's CDN) is the fourth application of the tests: Objective-C,
64 bit x86-64, no Swift. (The 4.x versions of MacPaw use the Swift runtime: they are not for this system yet.) It opens and extracts ZIP, RAR,
7-Zip, tar.gz, tar.bz2, tar.xz, StuffIt, LhA and about fifty other formats with its own libraries (`XADMaster.framework`, `UniversalDetector.framework`).

![The Unarchiver in pearOS](screenshot-unarchiver.png)

Install and run it in the pearOS image: `macapp-install unarchiver`, then `macapp "/Applications/The Unarchiver.app"` (the Preferences window) or
`macapp "/Applications/The Unarchiver.app" archive.zip` (what a double click on an archive does: it extracts it next to the archive).

## What it needed

| Gap | Where |
|---|---|
| LaunchServices: `LSCopyDefaultRoleHandlerForContentType` (answers with the application's own identifier: the Preferences window compares it with that), `LSCopyAllRoleHandlersForContentType`, `LSCopyApplicationURLsForURL`, `LSSetDefaultRoleHandlerForContentType`, `UTTypeCopyPreferredTagWithClass`, `GetCurrentKeyModifiers` | `shims.c` |
| `application:openFile:` (the single file form of the delegate, used by `MACHRUN_OPENFILE`) | `objc.c` |
| It crashes in `objc_autoreleasePoolPop` with `MALLOC_CONF=junk:true` (the default for Mac programs, see `machrun.c`); `macapp` starts it with `junk:false` | `tools/macapp` |

## Crashes: what is known

The crashes depend on the length of the path of the archive. With `MALLOC_CONF=junk:false` the application crashed in a third to a half of the runs; counting by the path:
a path of 21 to 26 characters never crashed, 27 to 33 crashed (every time at 27 and 28), 34 did not. The same archive in directories of other names behaves the same way every time,
so it is not a race (one CPU gives the same result) and not the state of the preferences. The crashes are in several places (`objc_msgSend` with a receiver that is not an object, a
call through a null pointer, a SIGBUS that is a general protection fault on a non-canonical receiver, in the draw of the progress indicator, in `awakeFromNib` of the application's controller):
memory that is read without having been written. Found:

* the string that `MACHRUN_OPENFILE` hands to `application:openFile:` was autoreleased and freed under the extraction (fixed: it is owned now);
* `MALLOC_CONF=zero:true` (the allocator gives zeroed memory, as a Mac's `+alloc` does for the instance variables) takes away the crashes of the controller; `macapp` and the test start it that way;
* what is left depends on the path length (27 to 33 characters). The crash is in `-[NSAutoreleasePool emptyPool]` -> `objc_autoreleasePoolPop` in the run loop (the pool of the main thread is
  damaged: something is released once too often and its memory is reused; `MACHRUN_TRACEPOOL=1` with `LD_PRELOAD=libexctrace.so` follows the pools per thread). **`NSZombieEnabled=YES` makes the crashes go away**
  (objects are not freed; 0 crashes in 24 runs for the lengths 26 to 33, 3 of 3 extracted each), and `macapp` starts the application that way. The cause (an over-release; with the zombies on, the first
  report is `-[GSCInlineString release]: message sent to deallocated instance` for the path string that `application:openFile:` got) is not found; the price of the workaround is memory that is not given back.

`MACHRUN_SEGV=1` prints the frames of the crash (`frame[n]`, by the frame pointers) and `MACHRUN_SEGV_HOLD=1` keeps the process for 90 seconds so that `procstat -v PID` can show its memory.

## Test

`sh tests/unar.sh "/Applications/The Unarchiver.app"` makes archives of a small tree (from `tests/data/unar`),
has the application open each one (up to `ATTEMPTS`, 3 by default, runs for each archive) and compares the extracted files with the originals byte for byte.
