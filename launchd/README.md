# launchd for pearOS

A Mach-O program (built on FreeBSD with `clang` and `ld64.lld`, linked against the libSystem of the Mac at run time; no Apple header or file in this directory) that serves the bootstrap port of the system, the way the launchd of a Mac does.

- It makes a port and sets it with `task_set_special_port(TASK_BOOTSTRAP_PORT)`. The kernel module notes that a task of uid 0 did it, and from then on every task that asks for its bootstrap port gets a send right to this port (without a launchd, the kernel answers as a launchd that has no services).
- It receives the XPC requests of libxpc (`CPX@`, version 5: look-up `0xcf`, `0x324` and `0x325` for the services by name, `0x12d`), prints them and answers. For now: `{}` for `0xcf`, `{"error": ESRCH}` for the services it does not have; for `com.apple.cfprefsd.daemon` it answers with a port (not enough yet for the real cfprefsd to start).
- Wire format found so far: a port in an XPC dictionary is the type word `0xd000` with no data, and the right travels as a port descriptor of the complex message, in the order of the values. The look-up answer has the keys `port` and `non-launching`.

Build: `make` (needs the llvm21 package). Run (as root, with `kern.darwinator.dyld=1`): `./launchd &`.

Next: the check-in of a daemon (the service gets the receive right), starting the daemons from the plists of `/System/Library/LaunchDaemons` on demand, `launchctl`, and the start from the boot scripts of pearOS.

## The services

- **Bootstrap look-up and check-in** (libxpc: the ids `0xcf`, `0x324`, `0x325`): `{"port": <send right>}` for a service that exists, `{"error": ESRCH}` for the others.  Every answer has the id `0x20000000` (libxpc checks it), comes from a sender that the kernel reports as pid 1, and carries `req_pid` and `rec_execcnt` of the requester (libxpc checks them when the answer comes from user 0).
- **`com.apple.bsd.dirhelper`**: the per-user directories of libsystem_coreservices (`confstr(_CS_DARWIN_USER_DIR / _TEMP_DIR / _CACHE_DIR)`), for any user: `/var/folders/<2>/<30>/{0,T,C}/<suffix>/`.  The name is the base-32 encoding (alphabet `0123456789_bcdfghjklmnpqrstvwxyz`, most significant bits first) of the 20 bytes `FFFFEEEE-DDDD-CCCC-BBBB-AAAA` + uid (the compatibility UUID of the uid) + uid, all big endian; checked against the names that the library asks for (root: `zz/zyxvpxvq6csfxvn_n0000000000000`).  The routines: `0xb86d` (make the user's directory and `0`, `T`, `C`) and `0xb872` (make the directory of a process in one of them, the suffix being its bundle id; the answer has the path).  The owner is read from the audit token of the request.

`mkfolders.sh` makes the directories of root and nobody at the start, so the first program does not have to wait for the service.

`lookuptest NAME` asks the bootstrap port for a service (what `bootstrap_look_up` of a program does); `lookuptest NAME stop` stops itself first, to attach a debugger.
