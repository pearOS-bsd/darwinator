# make -> launchd (a Mach-O x86_64 program; needs llvm21 with ld64.lld)
LD=/usr/local/llvm21/bin/ld64.lld
launchd: launchd.o libSystem.tbd
	$(LD) -arch x86_64 -platform_version macos 12.0 12.0 -e _main -o $@ launchd.o -L. -lSystem
launchd.o: launchd.c
	cc --target=x86_64-apple-macosx12.0 -O1 -ffreestanding -fno-builtin -fno-stack-protector -c launchd.c -o $@
clean:
	rm -f launchd launchd.o
lookuptest: lookuptest.o libSystem.tbd
	$(LD) -arch x86_64 -platform_version macos 12.0 12.0 -e _main -o $@ lookuptest.o -L. -lSystem
lookuptest.o: lookuptest.c
	cc --target=x86_64-apple-macosx12.0 -O1 -ffreestanding -fno-builtin -fno-stack-protector -c lookuptest.c -o $@
