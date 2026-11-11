# carender-stub

A stand-in for `com.apple.CARenderServer`, only to see what a client sends it (the first step of replacing the CoreAnimation render server of the window server).  It takes the service with `bootstrap_check_in` and prints the header and the first bytes of every message; it answers nothing.  Build: `sh build.sh OUT`.

Found with the real Dock: the first message is id 40202 (0x9d0a), complex, 108 bytes, with five port descriptors of which one holds a port (the reply port of the client).  The Dock waits for an answer.
