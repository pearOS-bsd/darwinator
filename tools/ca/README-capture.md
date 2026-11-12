# Capturing the CoreAnimation stream of a Dock on a Mac

`capture-mac.py` is an lldb script (Mac VM, SIP off): it puts a breakpoint with a script callback on `mach_msg2_trap` and writes every
message with id 0x9c42..0x9c45 / 0x9d0a to /tmp/lsd/cap.bin in the format of /tmp/ca.bin (u32 length + the buffer; the header is in
registers, so its first 24 bytes are not meaningful, the body starts where caparse.py expects it).

    lldb -b -o "process attach -n Dock -w" -o "command script import /tmp/lsd/capture-mac.py" -o "script cap.run()"   # then: killall Dock

Result of the first run (2026-11-13): the stream of the Mac has object types 0x30, 0x17 and 0x1f that caparse.py does not know and
that our Dock never sends -- the tile layers of the Mac carry them; ours are 0x1e layers without contents.

Object types found in QuartzCore (`CA::Render::Decoder::decode_object` jump table, address of the table 0x7ff80d34fd44, index = type - 1):
0x17 = `Image` (decode: i32, i8 format, i8 planes, i32 w, i32 h, planes x u64 row bytes, i8 shared, then a Shmem object or inline data),
0x1f = `LayerHost` (Object flags i32, port of the message, i32, i8; it shows another context -- the wallpaper), 0x30 = `SDFLayer`
(signed distance field: the glass of the Dock; Object flags, decode_data 0x70, ...).  caparse.py knows Image and LayerHost now; SDFLayer not yet.
The capture under lldb stops after ~15 KB (Dock stalls under the debugger), so the Mac stream is the start of the Dock only: one 54x54 image and a
128x128 one, a LayerHost, no tiles yet.  A different way to capture (not ptrace) is needed for the whole stream.

## Drawing the scene (2026-11-13)
`cadraw.py ca.bin out.ppm` paints the layers of /tmp/ca.bin (the last state of every layer, depth first from the roots: centre position, size,
background colour from the first floats of the 0x58 block) and `wsbsd/tools/wsshow` puts the PPM in a window of wsd (reloads it when it changes):

    python3 cadraw.py /tmp/ca.bin /tmp/cadock.ppm --watch &   wsshow /tmp/cadock.ppm 0 0 5 &

With the Dock as it runs now the picture is the WallpaperLayer only: 1280x800 of (0.176, 0.176, 0.195) -- the solid colour the Dock
sends when there is no wallpaper image (docs/screenshot-dock-wallpaper-ca.png).  The tile layers have no size and no contents yet.
