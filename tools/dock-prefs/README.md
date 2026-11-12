# Preferences of the Dock for pearOS (root)

`com.apple.dock.plist` goes to `/Library/Preferences/` (the cfprefsd of a Mac program that runs as uid 0 reads the preferences of root from there;
create the directory).  Without `version = 1` the Dock builds no tiles at all (`-[DockBar ...]` at 0x1000bc3e8 checks `[prefs version] == 1` before it
reads `persistent-apps`; found by following the tile creation of a Dock on a Mac with lldb).  The entries of `persistent-apps` have the shape of the
ones of a Mac (`defaults read com.apple.dock`): GUID, tile-type, tile-data{bundle-identifier, dock-extra, file-data{_CFURLString, _CFURLStringType},
file-label, file-mod-date, file-type, is-beta, parent-mod-date}; here they point to applications that exist on pearOS.
