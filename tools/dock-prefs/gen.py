#!/usr/bin/env python3
"""gen.py APP.app... : writes com.apple.dock.plist with those applications in persistent-apps (the shape of `defaults read com.apple.dock`).
   python3 gen.py /System/Applications/TextEdit.app ... > com.apple.dock.plist   (bundle ids and names are read from the Info.plist when run where the apps are; here from the arguments NAME=BUNDLEID)"""
import sys, plistlib, zlib
items = []
for i, a in enumerate(sys.argv[1:]):
    path, _, rest = a.partition("=")
    bid, _, label = rest.partition(",")
    label = label or path.rsplit("/", 1)[-1][:-4]
    items.append({"GUID": 3751510712 + i, "tile-type": "file-tile", "tile-data": {
        "bundle-identifier": bid, "dock-extra": 0,
        "file-data": {"_CFURLString": "file://" + path.replace(" ", "%20") + "/", "_CFURLStringType": 15},
        "file-label": label, "file-mod-date": 3875510296, "file-type": 41, "is-beta": 0, "parent-mod-date": 3875510296}})
plistlib.dump({"version": 1, "region": "US", "loc": "en_US:US", "mod-count": 1, "persistent-apps": items, "persistent-others": []}, sys.stdout.buffer, fmt=plistlib.FMT_XML)
