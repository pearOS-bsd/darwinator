# lsd-stub

A stand-in for `launchservicesd` (the first of the replacements for what comes from the Mac).  A Mach-O program built on FreeBSD (`sh build.sh OUT`, clang + ld64.lld, no Apple header) that takes the service `com.apple.coreservices.launchservicesd` through launchd and answers the XPC messages of LaunchServices clients; it prints every message (libxpc's description) to stderr.

What the Dock sends (found with it): `command` 100 (check-in, `clientversion`: the answer needs `serverversion`), 110, 360 (`restrictto`, `asn`), 410 (`listener` endpoint, `addasn`), 200, 500, 505; all with `session` (100000 for the user, -2 for the session of the process).  Only 100 has an answer with content for now.

The real `launchservicesd` of the Mac (run through the module) crashes in libxpc when a message that it sent cannot be answered (`_xpc_connection_call_reply_async` with no context); the stub does not need what it needs.

## Check-in (command 500) and NSXPC

- 500 (check-in of an application): the answer is `{success, result: <the info of the application + LSAuditToken, LSDisplayName, CFBundleExecutablePath, LSExecutableFilenameLowerCaseKey, BundleIdentifierLowerCase, CFBundleNameLowerCase, LSApplicationMemberCoalitionIDKey, pid, LSApplicationHasRegistered, ChangeCount, LSASN>}` (shape found by asking the daemon of the Mac with a program). `LSASN` must be a dictionary of three entries (`...asn.lo`, `...asn.hi` and `com.apple.coreservices.asn` = the marker uuid C96172A0-CE74-4D62-AFEF-ECFE4D04D827): LaunchServices reads only that form as a serial number; with another uuid the conversion returns NULL and the caller crashes in `CFDictionaryGetValue(NULL)`.
- Messages that carry `replysig` (NSXPC): they are not answered. An answer without the NSXPC fields takes `_xpc_connection_call_reply_async` of libxpc to a NULL context (SIGSEGV in ControlCenter, Dock, ...).
- Other things the apps need (not here): `/System/Library/CoreServices/SystemVersion.plist` (without it `GetMacOSXSystemVersionInformation failed` aborts HIServices `_RegisterApplication`); it is a fact file of the version, copied to the VM only.
