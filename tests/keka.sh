#!/bin/sh
# Runs the command line tools of Keka (the original Mach-O files) through machrun and checks them against the system tools.
# The Keka files are not in this repository: unpack Keka-x.y.z.dmg (7z x) and point KEKA_DIR at Keka.app/Contents/MacOS.
#   KEKA_DIR=/root/keka sh tests/keka.sh        (machrun from MACHRUN, else ./dyld/machrun built with make -C dyld, else the installed one)
set -u
cd "$(dirname "$0")/.."
K="${KEKA_DIR:?set KEKA_DIR to Keka.app/Contents/MacOS}"; M="${MACHRUN:-$PWD/dyld/machrun}"; [ -x "$M" ] || M=/usr/local/bin/machrun; T="$(mktemp -d)"; fail=0
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
# kekatar (bsdtar / libarchive): formats and filters, read back by the system tar and the other way round; encrypted zip (CommonCrypto)
TAR=$M" "$K/kekatar
for f in ustar pax gnutar zip 7zip; do
	rm -f $T/t.$f; (cd $T/src && $M $K/kekatar --format $f -cf $T/t.$f .) 2>/dev/null && ok "kekatar create $f" || bad "kekatar create $f"
	tar -tf $T/t.$f >/dev/null 2>&1 && ok "system tar reads the $f archive" || bad "system tar reads $f"
	rm -rf $T/x; mkdir $T/x; (cd $T/x && $M $K/kekatar -xf $T/t.$f) && diff -r $T/src $T/x >/dev/null && ok "kekatar extracts its own $f, identical tree" || bad "kekatar extract $f"
done
for f in z:gz j:bz2 J:xz; do
	o=${f%%:*}; e=${f##*:}
	(cd $T/src && $M $K/kekatar -c${o}f $T/f.tar.$e .) && tar -xOf $T/f.tar.$e ./text.txt 2>/dev/null | cmp - $T/src/text.txt && ok "kekatar -c$o ($e), system tar reads" || bad "kekatar -c$o"
done
(cd $T/src && tar -cf $T/sys.tar .) && rm -rf $T/x && mkdir $T/x && (cd $T/x && $M $K/kekatar -xf $T/sys.tar) && diff -r $T/src $T/x >/dev/null && ok "kekatar extracts a system tar" || bad "kekatar extracts system tar"
(cd $T/src && $M $K/kekatar --format zip --options zip:encryption=aes256 --passphrase s3cret -cf $T/enc.zip .) 2>/dev/null && ok "kekatar creates an AES-256 zip" || bad "kekatar AES zip"
rm -rf $T/x; mkdir $T/x; (cd $T/x && $M $K/kekatar --passphrase s3cret -xf $T/enc.zip) 2>/dev/null && diff -r $T/src $T/x >/dev/null && ok "kekatar decrypts its AES zip, identical tree" || bad "kekatar decrypt AES zip"
rm -rf $T/x; mkdir $T/x; (cd $T/x && $M $K/kekatar --passphrase wrong -xf $T/enc.zip) >/dev/null 2>&1 && bad "wrong passphrase accepted" || ok "wrong passphrase refused"
rm -rf $T; exit $fail
