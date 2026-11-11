# notifyd for pearOS

The daemon of `notify(3)` (the service `com.apple.system.notification_center`) as a Mach-O program of its own, built on FreeBSD with `clang` and `ld64.lld`, linked against the libSystem of the Mac (no header or file of Apple's).  Own code (BSD), written from the interface that libnotify speaks: `notify_ipc.defs` of Libnotify (APSL, fetched, not copied: subsystem 1000, the ids 1002-1032 and the layouts of their messages) and from what `notify(3)` documents.  The notifyd of Apple needs headers that are not open (sandbox, xpc private, os_log) and is 5000 lines; this one is a few hundred.

- Takes its service from the launchd (`bootstrap_check_in`), which gives it the receive right of the port and answers the look-ups of the clients with a send right.
- The shared page `apple.shm.notification_center` (slot 0: the pid of the daemon; one 32 bit counter per name from slot 1) that `notify_check` reads.  `shm_open` of Darwin takes names without the first slash: the kernel module adds it.
- Registrations: check, plain, signal (`kill`), mach port (a message with the token as id; common port per client, or a port per registration), file descriptor (the token, big endian, written to the descriptor made from the fileport).
- States (`notify_set_state`, `notify_get_state`), post by name and by id, cancel, suspend and resume, `checkin` with the protocol version 3.
- Not done: `notify_monitor_file`, regenerate, the filtered (sandbox) check-in, dump, the names with special meaning (`com.apple.system.notify.service.*`), access control by uid.

`notifytest` (the `notify_*` calls of libSystem against the daemon): `notifytest: all ok`.  A post is a message without an answer: a check right after it can come before the daemon has seen it.

Build: `make` (llvm21 with ld64.lld).  The launchd starts `/usr/sbin/notifyd` (or the programs on its command line).
