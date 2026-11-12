# Capturing the CoreAnimation stream of a Dock on a Mac

`capture-mac.py` is an lldb script (Mac VM, SIP off): it puts a breakpoint with a script callback on `mach_msg2_trap` and writes every
message with id 0x9c42..0x9c45 / 0x9d0a to /tmp/lsd/cap.bin in the format of /tmp/ca.bin (u32 length + the buffer; the header is in
registers, so its first 24 bytes are not meaningful, the body starts where caparse.py expects it).

    lldb -b -o "process attach -n Dock -w" -o "command script import /tmp/lsd/capture-mac.py" -o "script cap.run()"   # then: killall Dock

Result of the first run (2026-11-13): the stream of the Mac has object types 0x30, 0x17 and 0x1f that caparse.py does not know and
that our Dock never sends -- the tile layers of the Mac carry them; ours are 0x1e layers without contents.
