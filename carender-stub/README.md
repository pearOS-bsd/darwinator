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

## Parser of the command streams (tools/ca/caparse.py, carender.py)

`carender-stub` keeps every 0x9c4x message in `/tmp/ca.bin` (u32 length + the message). `tools/ca/caparse.py` (run in the directory with `ca.bin`) decodes them with the formats read from QuartzCore: all 25 commits of the Dock parse to the last byte.
- Complex messages (a send-once reply port): the stream starts after `u32 count` + 12 bytes per descriptor.
- Ops: 1 delete, 2 set_object, 3 update_layer, 7 layer id, 8 / 10 CFObject (dict of "SLSSID"...), 9 colorspace (kind u8, enum u8), 0xb/0xc/0xd order, 0x1a/0x1c/0x1d/0x1e (u64: input times), 0x1f (u8), 0x20 (u32, u8: the commit), 0x25 (port: nothing in the stream).
- Objects: u8 type (0 null, 0xff reference u64 id + u32, 0xfe definition), 0x1e Layer (u32 flags, 0x58 bytes of properties: six doubles at 0x28 = position x, y, 0, 0, bounds w, h; floats for colours; contents; sublayers array = u8 tag, u32 flags, u32 count, ids; an optional extension block with filters/transforms), 0x36 String (u32 length + bytes), 0x3e Vector (u32 n + n doubles), 0x34 Shmem (u32 flags; inline `u64 size + bytes`, or `u64, u64` + a port descriptor), 0x12 Filter (u32 flags; bit 8: `atom` only; else u8 1, atom, String, array of KeyValue).
- atom: u8 (< 0xfe the id), 0xfe + u16, else u64 length + bytes.
- CFObject: u8 type: 1 null, 2 string (u8 present, u64 length, bytes), 3 number (u8 kind, u64), 4 bool, 5 data, 6 array (u64 count), 7 dictionary (u64 count, key and value pairs).
- What the Dock sends so far: a window layer of 1280x800 and a handful of layers (items of 52x52 and 56x56 at x = 599, 639.5, 681...); no images yet.
