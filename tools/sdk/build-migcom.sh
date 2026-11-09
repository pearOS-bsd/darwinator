#!/bin/sh
# builds migcom (the MIG compiler of bootstrap_cmds) on FreeBSD; shim headers replace the mach headers of the host that xnu has and FreeBSD has not (tools/sdk/migshim.py).
#   sh tools/sdk/build-migcom.sh [WORK]      -> WORK/build/mig/migcom
W="${1:-/root/oss}"; H="$(cd "$(dirname "$0")" && pwd)"; SRC="$W/bootstrap_cmds/migcom.tproj"
mkdir -p "$W/build/mig"; cd "$W/build/mig" || exit 1
python3 "$H/migshim.py" "$W" >/dev/null
lex -t "$SRC/lexxer.l" > lexxer.c; yacc -d -o parser.c "$SRC/parser.y"; cp parser.h y.tab.h
cc -O1 -w -o migcom -I"$W/shim" -I. -I"$SRC" -DMIG_VERSION='"mig-1"' lexxer.c parser.c "$SRC"/error.c "$SRC"/global.c "$SRC"/header.c "$SRC"/routine.c "$SRC"/server.c "$SRC"/statement.c "$SRC"/string.c "$SRC"/type.c "$SRC"/user.c "$SRC"/utils.c "$SRC"/mig.c && ls -la migcom
