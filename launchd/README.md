# launchd for pearOS

A Mach-O program (built on FreeBSD with `clang` and `ld64.lld`, linked against the libSystem of the Mac at run time; no Apple header or file in this directory) that serves the bootstrap port of the system, the way the launchd of a Mac does.

- It makes a port and sets it with `task_set_special_port(TASK_BOOTSTRAP_PORT)`. The kernel module notes that a task of uid 0 did it, and from then on every task that asks for its bootstrap port gets a send right to this port (without a launchd, the kernel answers as a launchd that has no services).
- It receives the XPC requests of libxpc (`CPX@`, version 5: look-up `0xcf`, `0x324` and `0x325` for the services by name, `0x12d`), prints them and answers. For now: `{}` for `0xcf`, `{"error": ESRCH}` for the services it does not have; for `com.apple.cfprefsd.daemon` it answers with a port (not enough yet for the real cfprefsd to start).
- Wire format found so far: a port in an XPC dictionary is the type word `0xd000` with no data, and the right travels as a port descriptor of the complex message, in the order of the values. The look-up answer has the keys `port` and `non-launching`.

Build: `make` (needs the llvm21 package). Run (as root, with `kern.darwinator.dyld=1`): `./launchd &`.

Next: the check-in of a daemon (the service gets the receive right), starting the daemons from the plists of `/System/Library/LaunchDaemons` on demand, `launchctl`, and the start from the boot scripts of pearOS.

`mkfolders.sh` makes `/var/folders/zz/<hash>/{T,C,0}` for root and nobody: the daemons written in Swift stop at `Unable to read _CS_DARWIN_USER_CACHE_DIR` without it (the service `com.apple.bsd.dirhelper` of the Mac makes these on demand; the launchd does not yet).
