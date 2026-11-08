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

## Not solved

About a third of the runs crash (`objc_msgSend` or `objc_msg_lookup` with a receiver that is not an object, `objc_autoreleasePoolPop` when `junk:true`); the others
extract. Found so far: the string that `MACHRUN_OPENFILE` hands to `application:openFile:` was autoreleased and freed under the extraction (fixed: it is owned now, with
`NSZombieEnabled=YES` there are no more messages to a freed `GSCInlineString`), and archives made by the image's own `tar` (bsdtar) crash it more often than archives
of GNU tar. The remaining crash is not found. The test (`tests/unar.sh`) tries each archive up to three times.

## Test

`sh tests/unar.sh "/Applications/The Unarchiver.app"` makes archives of a small tree (from `tests/data/unar`),
has the application open each one (up to `ATTEMPTS`, 3 by default, runs for each archive, because of the crashes) and compares the extracted files with the originals byte for byte.
