# The SDK of the open source userland (in progress)

To build dyld, libSystem and the frameworks from source on FreeBSD (no Apple binary, no file of an Xcode SDK) the headers of a Darwin SDK have to be assembled from open sources:

1. `fetch-sources.sh`: dyld, xnu, Libc, libplatform, libdispatch, libpthread, CarbonHeaders, bootstrap_cmds from GitHub (`apple-oss-distributions`).
2. `build-migcom.sh`: the MIG compiler of bootstrap_cmds built on FreeBSD (`migshim.py` makes the few headers that it needs).  Checked: `migcom` turns `osfmk/mach/clock.defs` into `clock.h`.
3. Next: the headers that MIG generates from every `.defs` of xnu (`mach/task.h`, `mach/mach_port.h`, `mach/host_priv.h`, `mach/vm_map.h` ...), `sys/_symbol_aliasing.h` and `sys/_posix_availability.h` (xnu's `make_*.sh`), `TargetConditionals.h` (CarbonHeaders), libc++ from `/usr/include/c++/v1`;
   then the syntax check of the sources of dyld (`dyld/dyld/*.cpp`) with `--target=x86_64-apple-macosx14.0 -nostdinc`, and the link with `ld64.lld`.

Licenses: the sources are Apple's open source (mostly APSL 2.0, some Apache 2.0 and BSD): they are fetched into a work directory, not copied into this repository, and each file keeps its notice.  The scripts here are ours.
