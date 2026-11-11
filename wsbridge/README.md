# wsbridge

The front of the window server for programs of the Mac: a Mach-O service (`com.apple.windowserver.active`, taken from the launchd with `bootstrap_check_in`) that receives the MIG messages of SkyLight (the client library in the dyld cache) and, in the next steps, forwards them to `wsd` of wsbsd (github.com/pearOS-bsd/misc, `wsbsd/`), which speaks the same messages on a Unix socket (`SOCK_SEQPACKET`, port descriptors as descriptors of `SCM_RIGHTS`; `wsbsd/docs/SPEC-SLS-WIRE.md`).

Now (first step): it listens and logs `id size bits pid` of each message and answers with the MIG error "bad id".  The Dock of the Mac, started under the launchd, asks first for `29000` (GetCoreGraphicsServerVersion) and `29010` (GetSessionPort).

Plan: forward each message to `wsd` (one socket for each connection that the client makes with `NewConnectionPort`, 32000), turn the Mach port rights of the messages into descriptors and the other way round (the events go back as messages on the port that the client gave), and the memory of the surfaces (OOL descriptors, `shm`) into descriptors of `wsd`.

`tools/macdeps.sh PROGRAM` brings from the Mac VM the libraries that a Mac program needs and the dyld cache does not have (to `/System/Volumes/Preboot/Cryptexes/OS/...` on the FreeBSD VM) and keeps the list (`/root/macdeps.list`): what comes from the Mac and has to be replaced, one by one.  Those files are never committed.
