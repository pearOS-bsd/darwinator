# Compatibility: Mac applications on pearOS

What was run on the image, as the original files of the applications' own sites (not ours: `macapp-install NAME` fetches them). One command checks the list in the image:
`sh tests/all.sh` (it installs what is missing and prints a table).

| Application | Version | Built with | Result | Notes |
|---|---|---|---|---|
| [Keka](KEKA.md) | 1.6.8 | Objective-C | Extracts (command line tools: 37 checks, and the window) | The task window ("Keka (ZIP)") draws an empty grey box (auto layout) |
| [AppCleaner](APPCLEANER.md) | 3.7 | Objective-C, Xcode 16 nibs | Finds the files of an application and moves them to the Trash; the window lists them with names and sizes | rows have no icons or check boxes |
| [XLD](XLD.md) | 2025-03-02 | Objective-C, Mach-O plug-ins | WAV to FLAC and back, byte for byte (command line mode) | the window is not tried; AAC, ALAC and MP3 need AudioToolbox |
| [The Unarchiver](UNARCHIVER.md) | 3.11.1 | Objective-C, no Swift | Preferences window; extracts zip, tar.gz, tar.bz2, tar.xz and 7z | started with `NSZombieEnabled=YES`; without it it crashes for an archive path of 27 to 33 characters (an over-release, cause not found), see the document |
| [Sloth](https://sveinbjornt.com/sloth) | 3.6 | Objective-C, no Swift | Partly: the main window opens and is drawn (filters, popups, buttons), the list stays empty | the outline view is released too early while its nib is decoded (`NSOutlineView ... message sent to deallocated instance` with `NSZombieEnabled`) and the data source then does not answer; before that were fixed: `libproc` (`proc_name`, `proc_pidpath`), `getfsstat` in Darwin's struct, an `NSPathControl` that crashed (`registerForDraggedTypes:` without a window), and the `convertPoint:toView:` assertion of a table header (`compat.m`) |
| [GrandPerspective](https://grandperspectiv.sourceforge.net) | 3.5.1 | Objective-C, no Swift | Partly: scans a folder (`MACHRUN_OPENFILE=folder`) and opens the result window with the toolbar and the total size (91.7 MB for `/usr/share`); the treemap itself does not show yet although the program draws it (2090 rectangles into its bitmap, `MACHRUN_TRACEMSG`); an image of 32 bits with 3 samples and no alpha is made opaque when it is added to an image (`compat.m`), without effect so far (screenshot: `docs/screenshot-grandperspective.png`) | needed: stubs for `UTTypeCreatePreferredIdentifierForTag` (a dynamic identifier for every extension, also for none), `UTTypeCopyDeclaration`, `UTTypeCopyPreferredTagWithClass`, FSEvents and `SecCodeCopySelf`; `NSURL` resource values (directory, volume, capacity, format, dates); `fts_*` with the `FTSENT` and `struct stat` of Darwin (`fts_name` at 104); `bundleForClass:` of the program's own classes; `-[NSWindowController windowNibPath]` for a subclass that overrides `-windowNibName` |
| FFmpeg dylibs of [IINA](IINA.md) | | C | Decode (6 checks, on the host) | IINA itself: Swift, not possible yet |
| System Settings, other Apple applications | | Swift, SwiftUI, private frameworks | Not possible | no Swift runtime, frameworks are in the shared cache of a real Mac, licence |

The Finder starts a Mac `.app` on a double click (`macapp`); the test: `tools/qmp-click.py` and the screenshot in the README.

What an application needs from this system is in the notes of each one; the list of what the kernel module implements is in the README (about 170 Darwin system calls, 119 checks, amd64 and arm64).
