# XLD on pearOS

XLD (tmkk.undo.jp/xld, the 20250302 release) is an audio converter for macOS: closed source, free, Objective-C, no Swift, x86_64 Mach-O with frameworks (FLAC, Ogg, Vorbis, Sndfile, SoxR, WavPack, CDDB) and
plug-ins (Mach-O bundles for each format) in its bundle.  It is the test of the dylib and bundle loading of `machrun`, and its command line mode makes the result checkable.

```
sh tests/xld.sh /mnt/XLD.app       # in the desktop session of the pearOS image
ok   WAV to FLAC
ok   the output is a FLAC stream
ok   FLAC to WAV
ok   the audio is the same, byte for byte
4 passed, 0 failed
```

A WAV (one second of noise) is converted to FLAC with `XLD --cmdline -f flac`, the FLAC back to WAV, and the audio of the result is the same bytes.  Checked outside too: ffmpeg decodes the FLAC that XLD made on pearOS to
the same MD5 as the source WAV.

## What it needed

| Gap | Where |
|---|---|
| `LC_UNIXTHREAD` programs (no `LC_MAIN`): the entry `start` gets the stack of the kernel (argc, argv, envp, apple) | `machrun.c` |
| Plug-ins: `-[NSBundle load]` of a bundle in the layout of macOS (`Contents/MacOS/NAME`) loads the Mach-O bundle with the loader of `machrun` (`MH_BUNDLE`, with its dependencies, initializers and Objective-C metadata); a bundle without code for this machine (i386, arm64) is skipped | `machrun.c` `machrun_load_macho`, `compat.m` |
| `+[NSBundle bundleForClass:]` of a class that a plug-in defines, `resourcePath` and `pathForResource:` of `Contents/Resources`, `PlugIns` and the other folders of the bundle in the wrapper | `compat.m`, `tools/wrapapp.sh` |
| The messages of the older Objective-C ABI (`objc_msgSend_fixup`, `__objc_msgrefs`) | `objc.c` |
| The export trie of some linkers is not compact (`error` and `error_str` are two edges of the same node): every edge that matches is tried | `machrun.c` `trie_walk` |
| Plain `stat`, `lstat`, `fstat` have the older structure (120 bytes, 32 bit inode); FreeBSD's is 224 bytes, which smashed the stack of libsndfile and ended in a call to address 2 | `shims.c` |
| `floor`, `fmod` and the other math functions of libSystem come from libm | `shims.c` |
| `Gestalt`, `ExtAudioFileOpenURL` and the other AudioToolbox calls (a decoder that cannot open the file says so and XLD tries the next one) | `shims.c` |
| AppKit before `NSApplication` (a nib or a font in command line mode) | `compat.m` |

## Not done

The window of XLD (the GUI) was not tried; ripping CDs (DiscRecording, DiskArbitration) cannot work without a drive; the AAC, ALAC and MP3 formats need AudioToolbox, which is only stubbed.
