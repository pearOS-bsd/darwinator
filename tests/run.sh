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

# a C program (machsh) that uses the Darwin system calls: files, memory, time, entropy, the thread pointer
if sh tests/build-c.sh $T/machsh; then
	out="$(printf 'calc 6*7\ncalc (1+2)*(3+4)-5\ncalc 1/0\npid\nwrite /tmp/darwinator-t hello darwin\ncat /tmp/darwinator-t\nstat /tmp/darwinator-t\nstat /nonexistent\nmem 64\nrand\ntime\ntls\nrm /tmp/darwinator-t\nexit 3\n' | $T/machsh 2>&1)"; st=$?
	echo "$out" | sed 's/^/     | /'
	ok=1
	for want in '= 42' '= 16' 'division by zero' 'wrote 13 bytes' 'hello darwin' 'size 13  mode 0644 (file)' 'error 2' 'contents ok' 'removed'; do
		echo "$out" | grep -qF -- "$want" || { echo "FAIL machsh: missing '$want'"; ok=0; }
	done
	[ "$(uname -p)" = amd64 ] && { echo "$out" | grep -qF 'tls ok' || { echo "FAIL machsh: tls"; ok=0; }; }
	[ $st -eq 3 ] || { echo "FAIL machsh: exit status $st"; ok=0; }
	[ $ok -eq 1 ] && echo "ok   machsh (interactive Mach-O program)" || fail=1
else echo "FAIL building machsh"; fail=1; fi

# the other BSD calls (fork, execve, pipe, fcntl, ioctl, getdirentries64, statfs64, sigprocmask, getrlimit, sysctl, sockets ...)
if sh tests/build-c.sh $T/syscalls "" tests/c/syscalls.c; then
	out="$($T/syscalls 2>&1)"; st=$?
	echo "$out" | grep -v '^ok ' | sed 's/^/     | /'
	echo "$out" | grep -c '^ok ' | sed 's/^/     | checks passed: /'
	[ $st -eq 0 ] && echo "ok   syscalls (the other BSD calls)" || { echo "FAIL syscalls: $st failure(s)"; fail=1; }
else echo "FAIL building syscalls"; fail=1; fi

# posix_spawn (Darwin syscall 244) asked for by an application
if sh tests/build-c.sh $T/spawn "" tests/c/spawn.c; then
	out="$($T/spawn 2>&1)"; st=$?
	echo "$out" | grep -v '^ok ' | sed 's/^/     | /'
	echo "$out" | grep -c '^ok ' | sed 's/^/     | checks passed: /'
	[ $st -eq 0 ] && echo "ok   spawn (posix_spawn)" || { echo "FAIL spawn: $st failure(s)"; fail=1; }
else echo "FAIL building spawn"; fail=1; fi

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
