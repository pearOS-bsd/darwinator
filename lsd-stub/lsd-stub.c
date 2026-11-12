/* A stand-in for launchservicesd: it takes the service com.apple.coreservices.launchservicesd (the check-in through launchd, with the libxpc of the Mac), prints every message that a client sends
 * (the description of libxpc, to stderr) and answers with an empty dictionary.  It is for finding out what the real Dock asks LaunchServices; it is the first of the replacements. */
typedef void *xpc_object_t;
typedef void *dq;
extern xpc_object_t xpc_connection_create_mach_service(const char *name, dq q, unsigned long flags);
extern void xpc_connection_set_event_handler(xpc_object_t c, void (^h)(xpc_object_t));
extern void xpc_connection_resume(xpc_object_t c);
extern void xpc_connection_send_message(xpc_object_t c, xpc_object_t m);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t m);
extern long xpc_dictionary_get_int64(xpc_object_t d, const char *k);
extern unsigned long xpc_dictionary_get_uint64(xpc_object_t d, const char *k);
extern void xpc_dictionary_set_uint64(xpc_object_t d, const char *k, unsigned long v);
extern void xpc_dictionary_set_int64(xpc_object_t d, const char *k, long v);
extern char *xpc_copy_description(xpc_object_t o);
extern void *xpc_get_type(xpc_object_t o);
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
extern dq dispatch_queue_create(const char *label, void *attr);
extern void dispatch_main(void);
extern long write(int fd, const void *buf, unsigned long n);
extern void free(void *p);

static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(2, s, n); }

static void
handle(xpc_object_t peer, xpc_object_t m)
{
	char *d = xpc_copy_description(m);
	xpc_object_t r;

	out("lsd-stub: message: "); out(d); out("\n");
	free(d);
	if (xpc_get_type(m) == (void *)&_xpc_type_dictionary && (r = xpc_dictionary_create_reply(m)) != 0) {
		long cmd = xpc_dictionary_get_int64(m, "command");

		if (cmd == 100)					/* the check-in of a client: the server version (LaunchServices refuses an answer without it) */
			xpc_dictionary_set_uint64(r, "serverversion", xpc_dictionary_get_uint64(m, "clientversion"));
		xpc_connection_send_message(peer, r);
	}
}

int
main(int argc, char **argv)
{
	dq q = dispatch_queue_create("lsd-stub", 0);
	xpc_object_t svc = xpc_connection_create_mach_service(argc > 1 ? argv[1] : "com.apple.coreservices.launchservicesd", q, 1);

	xpc_connection_set_event_handler(svc, ^(xpc_object_t peer) {
		out("lsd-stub: a connection\n");
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) { handle(peer, m); });
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(svc);
	out("lsd-stub: serving\n");
	dispatch_main();
	return (0);
}
