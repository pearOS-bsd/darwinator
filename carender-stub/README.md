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

## The command stream (0x9c42), what is known (REA on QuartzCore, `Server::server_thread`, `CmdStreamMsg::run`, `Decoder::Decoder`)

- The message is not complex (bits 0x1100, no reply port); the body is `u32 0` (the descriptor count), then the stream from message offset 0x1c to its end.
- The stream starts with a header read by `Decoder::Decoder`: `u32` (seed/flags), `u32` context id (must equal the id of the context that was registered, here 1), `u64` timestamp (a double, seconds). Everything is plain little-endian (`decode_int8/16/32/64`, `decode_vm_size_t` = 8 bytes); a read beyond the end is a fatal error of the decoder.
- Then commands, one byte each (`CmdStreamMsg::run`): 0 end, 1 delete_object (id u64, u32), 2 set_object (id u64, u32, object, u32), 3 update_layer (id u64), 4 add_animation (id u64, animation), 5 remove_all_animations, 6 remove_animation (id, CFString), 7 set_layer_id, 8 (CFDictionary), 9 colorspace, 10 (CFString), more up to the table in `run` (decompile `0x7ff80d2ba4cc` of the extracted QuartzCore, REA snapshot is quick after the first run).
- The first commit of the Dock is `09 01 24 20 ...` (colorspace first), the second one carries the layer objects (a `Layer` is read by `CA::Render::Layer::Layer(Decoder*)`: not decompiled yet).
- Next: decompile `decode_colorspace`, `decode_object`, `Layer::Layer(Decoder*)`, write the parser (it only has to build a tree of layers: geometry, background colour, contents as shared memory), hand the tree to wsd/quartzcore.
