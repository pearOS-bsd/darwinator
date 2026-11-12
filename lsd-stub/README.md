# lsd-stub

A stand-in for `launchservicesd` (the first of the replacements for what comes from the Mac).  A Mach-O program built on FreeBSD (`sh build.sh OUT`, clang + ld64.lld, no Apple header) that takes the service `com.apple.coreservices.launchservicesd` through launchd and answers the XPC messages of LaunchServices clients; it prints every message (libxpc's description) to stderr.

What the Dock sends (found with it): `command` 100 (check-in, `clientversion`: the answer needs `serverversion`), 110, 360 (`restrictto`, `asn`), 410 (`listener` endpoint, `addasn`), 200, 500, 505; all with `session` (100000 for the user, -2 for the session of the process).  Only 100 has an answer with content for now.

The real `launchservicesd` of the Mac (run through the module) crashes in libxpc when a message that it sent cannot be answered (`_xpc_connection_call_reply_async` with no context); the stub does not need what it needs.

## Check-in (command 500) and NSXPC

- 500 (check-in of an application): the answer is `{success, result: <the info of the application + LSAuditToken, LSDisplayName, CFBundleExecutablePath, LSExecutableFilenameLowerCaseKey, BundleIdentifierLowerCase, CFBundleNameLowerCase, LSApplicationMemberCoalitionIDKey, pid, LSApplicationHasRegistered, ChangeCount, LSASN>}` (shape found by asking the daemon of the Mac with a program). `LSASN` must be a dictionary of three entries (`...asn.lo`, `...asn.hi` and `com.apple.coreservices.asn` = the marker uuid C96172A0-CE74-4D62-AFEF-ECFE4D04D827): LaunchServices reads only that form as a serial number; with another uuid the conversion returns NULL and the caller crashes in `CFDictionaryGetValue(NULL)`.
- Messages that carry `replysig` (NSXPC): they are not answered. An answer without the NSXPC fields takes `_xpc_connection_call_reply_async` of libxpc to a NULL context (SIGSEGV in ControlCenter, Dock, ...).
- Other things the apps need (not here): `/System/Library/CoreServices/SystemVersion.plist` (without it `GetMacOSXSystemVersionInformation failed` aborts HIServices `_RegisterApplication`); it is a fact file of the version, copied to the VM only.

## RunningBoard (com.apple.runningboard) as a stub

The real `launchservicesd` (tools/stack-dock-reallsd.sh) waits for RunningBoard at start-up.  The stub answers
`handshakeWithRequest:` with the shape that the runningboardd of the Mac gives (found by asking it with a program under
lldb): `{"return-value": {bsx_class: RBSHandshakeResponse, success-keys/values, fail-keys/values: [], _handle: {bsx_class:
RBSProcessHandle, _codedHandle: {_identity {RBSOpaqueProcessIdentity, _pid, _auid, _description}, _pid, _bsAuditToken
{bsx_index}, _data, _bundle}}}}`.  Any other selector: the reply is released (so the right to reply is destroyed and the
kernel sends MACH_NOTIFY_SEND_ONCE to the waiter) and the connection is cancelled.

Do not put `com.apple.logd` among the names of the stubs: Dock then finds a logd that does not speak the protocol and
`os_trace_metadata_init` crashes (the firehose buffer is NULL).

The handshake reply must also carry `bsx_archive` (a keyed archive with a `BSAuditToken` under the key named by
`bsx_index`): without it RunningBoardServices throws while holding an `os_unfair_lock` and `launchservicesd` then
waits on that lock for ever.  The stub embeds the archive of the Mac (278 bytes) and patches the token (offset 126).
With it, the real `launchservicesd` + Dock run: Dock sends its layers (49 messages in /tmp/ca.bin).
