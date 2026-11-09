# Building Mach-O programs on FreeBSD

`clang` of the base system makes Mach-O objects (`--target=x86_64-apple-macosx12.0`); `ld64.lld` (the package `llvm21`, `/usr/local/llvm21/bin/ld64.lld`) links them into executables and dynamic libraries.  The library that the program links
against is a text stub written in this repository (`tests/macho-toolchain/libSystem.tbd`: the install name and the symbols); no header and no file of an SDK is used.

    pkg install llvm21
    sh tests/macho-toolchain/build.sh /tmp/out      # /tmp/out/hello and /tmp/out/libhello.dylib
    sh tests/macho-toolchain/run.sh                 # run through dyld on darwinator.ko

The result runs on the module: dyld (still the one of a Mac) maps the program, finds `libhello.dylib` and `libSystem` and calls `write` through them.  This is the first step of the plan to build the whole userland (dyld, libSystem, libdispatch, objc4, frameworks) from open sources
and to stop depending on the files of a Mac.
