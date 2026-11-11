#!/bin/sh
# /var/folders for the per-user directories of libsystem_coreservices (confstr(_CS_DARWIN_USER_DIR / _TEMP_DIR / _CACHE_DIR)).  On a Mac the service com.apple.bsd.dirhelper makes them on
# demand; the path comes from the UUID of the user (the hash was read from the accesses that the library makes).  Until the launchd makes them itself, they are made here for root and nobody.
set -e
mk() { d=/var/folders/zz/$1; mkdir -p "$d/T" "$d/C" "$d/0"; chown -R "$2:$3" "$d"; chmod -R 700 "$d"; }
mkdir -p /var/folders/zz; chmod 755 /var/folders /var/folders/zz
mk zyxvpxvq6csfxvn_n0000000000000 root wheel		# uid 0
mk zyxvpxvq6csfxvn_n0007zzr001zzy nobody nobody		# uid 65534 (the -2 of the Mac)
