#!/bin/sh
# Loads the module, runs Mach-O test files and checks the output and the exit status.  Run as root on FreeBSD
# after "make":   sh tests/run.sh
set -u
cd "$(dirname "$0")/.."
T="$(mktemp -d)"; fail=0
case "$(uname -p)" in amd64) ARCH=x86_64 ;; aarch64) ARCH=arm64 ;; *) echo "unsupported $(uname -p)"; exit 2 ;; esac

kldstat -qm darwinator || kldload ./darwinator.ko || { echo "FAIL: kldload"; exit 1; }
sysctl kern.darwinator.debug=1 >/dev/null

check() {   # name file
	out="$("$2" 2>&1)"; st=$?
	if [ "$out" = "hello from a Mach-O file" ] && [ $st -eq 7 ]; then echo "ok   $1"
	else echo "FAIL $1: status $st, output '$out'"; fail=1; fi
}
python3 tools/mkmacho.py --arch $ARCH --entry main       -o $T/main       && check "LC_MAIN ($ARCH)"       $T/main
python3 tools/mkmacho.py --arch $ARCH --entry unixthread -o $T/unixthread && check "LC_UNIXTHREAD ($ARCH)" $T/unixthread
python3 tools/mkmacho.py --fat                           -o $T/fat        && check "universal file"        $T/fat

# a Mach-O file of the other architecture is refused, ELF programs and scripts still run
OTHER=arm64; [ $ARCH = arm64 ] && OTHER=x86_64
python3 tools/mkmacho.py --arch $OTHER -o $T/other
if "$T/other" >/dev/null 2>&1; then echo "FAIL other architecture ran"; fail=1; else echo "ok   other architecture refused"; fi
[ "$(/bin/echo elf)" = elf ] && echo "ok   ELF program" || { echo "FAIL ELF program"; fail=1; }
printf '#!/bin/sh\necho script\n' > $T/s.sh; chmod +x $T/s.sh
[ "$($T/s.sh)" = script ] && echo "ok   script" || { echo "FAIL script"; fail=1; }

kldunload darwinator && echo "ok   kldunload" || { echo "FAIL kldunload"; fail=1; }
rm -rf "$T"
exit $fail
