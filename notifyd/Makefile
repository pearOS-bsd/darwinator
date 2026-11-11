# make -> notifyd (a Mach-O x86_64 program; needs llvm21 with ld64.lld)
LD=/usr/local/llvm21/bin/ld64.lld
notifyd: notifyd.o libSystem.tbd
	$(LD) -arch x86_64 -platform_version macos 12.0 12.0 -e _main -o $@ notifyd.o -L. -lSystem
notifyd.o: notifyd.c
	cc --target=x86_64-apple-macosx12.0 -O1 -ffreestanding -fno-builtin -fno-stack-protector -c notifyd.c -o $@
clean:
	rm -f notifyd notifyd.o
notifytest: notifytest.o libSystem.tbd
	$(LD) -arch x86_64 -platform_version macos 12.0 12.0 -e _main -o $@ notifytest.o -L. -lSystem
notifytest.o: notifytest.c
	cc --target=x86_64-apple-macosx12.0 -O1 -ffreestanding -fno-builtin -fno-stack-protector -c notifytest.c -o $@
