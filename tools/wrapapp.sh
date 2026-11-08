#!/bin/sh
# wrapapp.sh APP.app OUTDIR [MACHRUN]: makes a GNUstep-style wrapper of a Mac application bundle, so that GNUstep finds the
# application's own bundle as the main bundle (it looks for NAME.app/NAME, NAME.app/Resources and NAME.app/Info-gnustep.plist,
# not for Contents/MacOS):  the launcher (a copy of machrun) runs NAME.macho next to it; resources and frameworks are links.
#   sh tools/wrapapp.sh /mnt/Keka.app /tmp [/path/to/machrun]        ->  /tmp/Keka.app/Keka
set -e
APP="$1"; OUT="$2"; MR="${3:-$(dirname "$0")/../dyld/machrun}"; FIXED="$4"	# FIXED: optional directory of nibs repaired with tools/nibfix.py (NAME.nib/keyedobjects.nib), used instead of the originals
NAME="$(basename "$APP" .app)"; W="$OUT/$NAME.app"; C="$APP/Contents"
EXE="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$C/Info.plist" 2>/dev/null || grep -A1 '<key>CFBundleExecutable</key>' "$C/Info.plist" | sed -n 's/.*<string>\(.*\)<\/string>.*/\1/p')"
[ -n "$EXE" ] || EXE="$NAME"
rm -rf "$W"; mkdir -p "$W"
mkdir "$W/Resources"
for f in "$C/Resources"/*; do b="$(basename "$f")"; case "$b" in en.lproj|English.lproj) ;; *) ln -s "$f" "$W/Resources/$b" ;; esac; done
# GNUstep looks for the nibs in en.lproj / English.lproj and does not know Base.lproj (Apple's fallback language): English = Base + en
for L in en.lproj English.lproj; do
	mkdir "$W/Resources/$L"
	for src in Base.lproj en.lproj; do
		[ -d "$C/Resources/$src" ] || continue
		for f in "$C/Resources/$src"/*; do
			b="$(basename "$f")"
			if [ -n "$FIXED" ] && [ -d "$FIXED/$b" ]; then ln -sf "$FIXED/$b" "$W/Resources/$L/$b"; else ln -sf "$f" "$W/Resources/$L/$b"; fi
		done
	done
done
[ -d "$C/Frameworks" ] && ln -s "$C/Frameworks" "$W/Frameworks"
# plug-ins and the other folders that the program looks for in its bundle
for d in PlugIns SharedSupport Library; do [ -d "$C/$d" ] && ln -s "$C/$d" "$W/$d"; done
cp "$C/Info.plist" "$W/Info-gnustep.plist"
ln -s "$C/MacOS/$EXE" "$W/$EXE.macho"
# the helper programs of the application (NSBundle pathForAuxiliaryExecutable: looks next to the executable): links to the Mach-O files
for f in "$C/MacOS"/*; do b="$(basename "$f")"; [ "$b" = "$EXE" ] || ln -sf "$f" "$W/$b"; done
cp "$MR" "$W/$EXE"; chmod +x "$W/$EXE"
echo "$W/$EXE"
