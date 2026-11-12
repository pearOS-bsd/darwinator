# hid-stub

A small test server for the service `com.apple.iohideventsystem`: it takes the service from launchd, prints the id of every request and answers the open (id 70000) with a port and everything else with a plain success.  Only for finding out what a client of the HID event system sends; a real replacement comes later.  Build: `sh build.sh OUT`.
