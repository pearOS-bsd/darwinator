# AppCleaner 3.7 on pearOS

AppCleaner 3.7 (freemacsoft.net, the original `AppCleaner_3.7.zip`, built with Xcode 16 for macOS 15) is a second test for `machrun`, after Keka.
It is a small Objective-C program (400 KB), but built the way applications are built now.

![AppCleaner in pearOS](screenshot-appcleaner.png)

Run it in the pearOS image: `macapp /path/AppCleaner.app` (or `machrun` on the bundle after `wrapapp.sh`).

## What it needed (all in `dyld/`)

| Gap | Where |
|---|---|
| `LC_DYLD_CHAINED_FIXUPS` (pointer formats `DYLD_CHAINED_PTR_64` and `_64_OFFSET`, imports with addends) | `machrun.c` `do_chained` |
| Compiled nibs in the `NIBArchive` format (the object graph of the old keyed archive, in a new container) | `compat.m` `machrun_nibarchive_to_keyed`, `tools/nibarchive.py` |
| Constant `@[]`, `@{}`, `@123` (`__objc_arrayobj`, `__objc_dictobj`, `__objc_intobj`, classes `NSConstantArray` ...) | `compat.m` |
| Sends to a class by the address of its structure in the file (`lea rdi,[_OBJC_CLASS_$_Foo]`) | `objc.c` `machrun_fix_recv`, in the trampolines |
| `objc_alloc`, `objc_alloc_init`, `objc_opt_new`, `objc_opt_class`, `objc_opt_isKindOfClass`, `objc_opt_respondsToSelector`, `objc_unsafeClaimAutoreleasedReturnValue` | `objc.c` |
| libdispatch queues retained with `objc_retain` (they are Objective-C objects on a Mac) | `objc.c` `is_dsp` |
| `os_log`, `UTType` constants, `__NSArray0__struct`, `__kCFBooleanTrue`, `setattrlist` | `shims.c`, `objc.c` |
| CoreGraphics = libopal, CoreFoundation = GNUstep corebase, QuartzCore | `shims.c` `shim_open_library` |
| Sparkle's `SUUpdater` in the main nib, SF Symbols and `Assets.car` images (empty images), `-1` as "no selection" in `NSSegmentedControl`, images of size 0 | `compat.m` |
| The activity indicator built from Core Animation layers (`DJActivityIndicator setColor:` does nothing) | `objc.c` `program_patches` |

## More that the search and the removal needed

| Gap | Where |
|---|---|
| `dispatch_async`, `dispatch_group_async`, `barrier_async`, `after`, `notify`, `source_set_event_handler` with a block that lives on the stack of the caller (the block is copied and run with the `_f` variants) | `shims.c` |
| `dispatch_group_wait` (this libdispatch needs `_dispatch_ulock_wait`, which it does not have, and its wait is not woken): a poll of the group | `shims.c` |
| `NSAnimationContext runAnimationGroup:completionHandler:` (the completion handler runs at once), the view of a controller without a nib | `compat.m` |
| UTType (identifiers and conformance), `NSURLContentTypeKey` of a file, `UTTypeApplication` and the other constants | `compat.m`, `objc.c` |
| The Info.plist of a macOS bundle (`Contents/Info.plist`) for `bundleIdentifier`, `infoDictionary`; `kCFBundleExecutableKey` and the other keys | `compat.m`, `objc.c` |
| `~/Library`, `/Library`, `~/Library/Caches`, `~/Library/Application Support` for `URLsForDirectory:inDomains:` (GNUstep answers `~/GNUstep/Library`) | `compat.m` |
| The URLs of `enumeratorAtURL:...` (made from relative names against the working directory) | `compat.m` |
| `trashItemAtURL:resultingItemURL:error:` (the file goes to `~/.Trash`), `hasFullDiskAccess` | `compat.m`, `objc.c` |
| `csr_get_active_config`, code signatures ("unsigned"), IOKit, DiskArbitration, login items, authorization, ASL, `os_log` objects | `shims.c` |

## The test

`tests/appcleaner.sh` (in the desktop session of the pearOS image): it leaves the files that Keka leaves in `~/Library` (preferences, caches, application support, saved state), opens `Keka.app` in AppCleaner (what a drop on its window does), reads what AppCleaner holds (`MACHRUN_KVC=mainWindowController.uninstallViewController.bundles.relatedFiles.path`), presses Remove (`MACHRUN_INVOKE='mainWindowController.uninstallViewController remove:'`) and checks that the files are in `~/.Trash` and not in `~/Library`:

```
ok   AppCleaner lists Preferences/com.aone.keka.plist
ok   AppCleaner lists Caches/com.aone.keka
ok   AppCleaner lists Application Support/Keka
ok   AppCleaner lists Saved Application State/com.aone.keka.savedState
ok   Remove was pressed
ok   Preferences/com.aone.keka.plist is gone from ~/Library   (and the other three)
ok   com.aone.keka.plist is in ~/.Trash                        (and the other three)
13 passed, 0 failed
```

## Where it stops

The application does its work, but the window does not show the result: the list of files is built (the test reads it from the application) and the window stays on "Searching...".
Probably the auto layout of the list view (the solver of GNUstep ignores constraints that it cannot solve).
Open: the images of `Assets.car` (no CoreUI reader), PDF images (`DropApp.pdf`), Sparkle as a library, a crash in `objc_msgSend` that was seen once at start and not again.
