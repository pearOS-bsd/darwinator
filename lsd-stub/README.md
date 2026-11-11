# lsd-stub

A stand-in for `launchservicesd` (the first of the replacements for what comes from the Mac).  A Mach-O program built on FreeBSD (`sh build.sh OUT`, clang + ld64.lld, no Apple header) that takes the service `com.apple.coreservices.launchservicesd` through launchd and answers the XPC messages of LaunchServices clients; it prints every message (libxpc's description) to stderr.

What the Dock sends (found with it): `command` 100 (check-in, `clientversion`: the answer needs `serverversion`), 110, 360 (`restrictto`, `asn`), 410 (`listener` endpoint, `addasn`), 200, 500, 505; all with `session` (100000 for the user, -2 for the session of the process).  Only 100 has an answer with content for now.

The real `launchservicesd` of the Mac (run through the module) crashes in libxpc when a message that it sent cannot be answered (`_xpc_connection_call_reply_async` with no context); the stub does not need what it needs.
