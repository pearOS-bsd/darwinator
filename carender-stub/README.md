# carender-stub

A stand-in for `com.apple.CARenderServer`, only to see what a client sends it (the first step of replacing the CoreAnimation render server of the window server).  It takes the service with `bootstrap_check_in` and prints the header and the first bytes of every message; it answers nothing.  Build: `sh build.sh OUT`.

Found with the real Dock: the first message is id 40202 (0x9d0a), complex, 108 bytes, with five port descriptors of which one holds a port (the reply port of the client).  The Dock waits for an answer.

## Requests that are answered (found in the code of QuartzCore on the Mac; the answer is the id of the request plus 100)

| id | routine | answer |
|---|---|---|
| 0x9d0a (40202) | `CASRegisterClient`: five send rights in the request | complex, two send rights, NDR, two numbers, the offset and count of a byte array (at most 64) and one more number: size 0x50 + the padded count |
| 0x9d3e (40254) | `CARenderServerGetNeededAlignment` | simple, size 0x28: the return code and the alignment (16) |
| 0x9d51 (40273) | `CARenderServerGetMaxRenderableIOSurfaceSize` | simple, size 0x30: the return code and three numbers (16384, 16384, 0) |

After these the Dock stops in an assertion of its own QuartzCore, `(scheduled)` in `commit_transaction` (CAContextInternal.mm, line 3725).  `casid.sh`-style search: the function that sends an id has `movabsq $0xID00000000` in its code.
