# make -> wsbridge (a Mach-O x86_64 program; needs llvm21 with ld64.lld)
LD=/usr/local/llvm21/bin/ld64.lld
wsbridge: wsbridge.o libSystem.tbd
	$(LD) -arch x86_64 -platform_version macos 12.0 12.0 -e _main -o $@ wsbridge.o -L. -lSystem
wsbridge.o: wsbridge.c
	cc --target=x86_64-apple-macosx12.0 -O1 -ffreestanding -fno-builtin -fno-stack-protector -c wsbridge.c -o $@
clean:
	rm -f wsbridge wsbridge.o
