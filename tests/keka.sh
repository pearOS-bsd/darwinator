#!/bin/sh
# Runs the command line tools of Keka (the original Mach-O files) through machrun and checks them against the system tools.
# The Keka files are not in this repository: unpack Keka-x.y.z.dmg (7z x) and point KEKA_DIR at Keka.app/Contents/MacOS.
#   KEKA_DIR=/root/keka sh tests/keka.sh        (machrun from ../dyld, built with make -C dyld)
set -u
cd "$(dirname "$0")/.."
K="${KEKA_DIR:?set KEKA_DIR to Keka.app/Contents/MacOS}"; M="$PWD/dyld/machrun"; T="$(mktemp -d)"; fail=0
ok() { echo "ok   $1"; }; bad() { echo "FAIL $1"; fail=1; }

mkdir -p $T/src/sub; (for i in $(seq 1 5000); do echo "line $i: the quick brown fox"; done) > $T/src/text.txt
head -c 500000 /dev/urandom > $T/src/sub/random.bin; echo "keka" > $T/src/sub/small.txt

# xz
$M $K/kekaxz -k -c $T/src/sub/random.bin > $T/a.xz && xz -d -c $T/a.xz | cmp - $T/src/sub/random.bin && ok "kekaxz compress, system xz reads" || bad "kekaxz compress"
xz -c $T/src/text.txt > $T/b.xz; $M $K/kekaxz -d -c $T/b.xz | cmp - $T/src/text.txt && ok "kekaxz decompress a system xz file" || bad "kekaxz decompress"
$M $K/kekaxz -T4 -6 -c $T/src/text.txt | xz -d -c | cmp - $T/src/text.txt && ok "kekaxz, 4 threads" || bad "kekaxz threads"
# zstd, lz4
$M $K/kekazstd -c $T/src/text.txt | zstd -d -c 2>/dev/null | cmp - $T/src/text.txt && ok "kekazstd compress" || bad "kekazstd compress (is zstd installed?)"
zstd -c $T/src/text.txt 2>/dev/null | $M $K/kekazstd -d -c | cmp - $T/src/text.txt && ok "kekazstd decompress" || bad "kekazstd decompress"
$M $K/kekalz4 -c $T/src/text.txt > $T/t.lz4 && $M $K/kekalz4 -d -c $T/t.lz4 | cmp - $T/src/text.txt && ok "kekalz4 round trip" || bad "kekalz4"
# 7-Zip: 7z and zip archives, listing, test, extraction, update
for fmt in 7z zip; do
	(cd $T/src && $M $K/keka7zz a -t$fmt $T/arc.$fmt . >/dev/null) && ok "keka7zz create $fmt" || bad "keka7zz create $fmt"
	$M $K/keka7zz l $T/arc.$fmt | grep -q "random.bin" && ok "keka7zz list $fmt" || bad "keka7zz list $fmt"
	$M $K/keka7zz t $T/arc.$fmt | grep -q "Everything is Ok" && ok "keka7zz test $fmt" || bad "keka7zz test $fmt"
	rm -rf $T/out; mkdir $T/out; (cd $T/out && $M $K/keka7zz x -y $T/arc.$fmt >/dev/null) && diff -r $T/src $T/out >/dev/null && ok "keka7zz extract $fmt, identical tree" || bad "keka7zz extract $fmt"
done
unzip -tq $T/arc.zip >/dev/null 2>&1 && ok "system unzip accepts the zip of keka7zz" || echo "note: system unzip did not test the zip (not installed or different)"
rm -rf $T; exit $fail
