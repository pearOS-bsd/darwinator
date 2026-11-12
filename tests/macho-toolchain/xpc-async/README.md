# xpc-async

`asynctest`: an XPC client that sends a message with `xpc_connection_send_message_with_reply` (an asynchronous reply handled by a block) to `com.apple.coreservices.launchservicesd` (the `lsd-stub`).  Build: `sh build.sh OUT` (`ASYNCTEST_CFLAGS=-DASYNCTEST_WAIT=6` waits 6 s first, for a debugger).

It reproduces the crash that every Apple program that waits for an asynchronous XPC answer has here: SIGSEGV in `_xpc_connection_call_reply_async` (libxpc), reading `0x98(%rsi)` with `rsi == NULL`.  On a Mac the handler gets a heap pointer (the context of the call) in `rsi`; here NULL.  What was found (libdispatch `_dispatch_mach_msg_async_reply_invoke` → `_dispatch_client_callout3_a` → the handler of libxpc, same offsets as on the Mac):
- the reply xpc object (`rdx`) has the same shape as on the Mac;
- the message reaches the handler through the async-reply continuation, which needs `dmsg->do_ctxt != NULL` to be pushed (so something sets it and it is gone, or another path makes the continuation);
- fixed on the way: EV_UDATA_SPECIFIC is given back on machport events and errors come back with the flags of their change (before: a one-shot reply note was deleted twice and libdispatch crashed in `_dispatch_unote_unregister_direct`, then in `_dispatch_kevent_print_error`).
