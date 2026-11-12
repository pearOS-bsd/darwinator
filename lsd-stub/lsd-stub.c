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
extern void xpc_dictionary_set_bool(xpc_object_t d, const char *k, int v);
extern void xpc_dictionary_set_value(xpc_object_t d, const char *k, xpc_object_t v);
extern xpc_object_t xpc_dictionary_create(const char *const *keys, const xpc_object_t *values, unsigned long count);
extern xpc_object_t xpc_shmem_create(void *region, unsigned long length);
extern void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t d, const char *k);
extern unsigned long xpc_array_get_count(xpc_object_t a);
extern xpc_object_t xpc_array_get_value(xpc_object_t a, unsigned long i);
extern const char *xpc_string_get_string_ptr(xpc_object_t s);
extern xpc_object_t xpc_copy(xpc_object_t o);
extern const char *xpc_dictionary_get_string(xpc_object_t d, const char *k);
extern xpc_object_t xpc_string_create(const char *s);
extern xpc_object_t xpc_data_create(const void *b, unsigned long n);
extern xpc_object_t xpc_uuid_create(const unsigned char *u);
extern int xpc_connection_get_pid(xpc_object_t c);
extern xpc_object_t xpc_array_create(const xpc_object_t *objects, unsigned long count);
extern void xpc_array_append_value(xpc_object_t a, xpc_object_t v);
extern xpc_object_t xpc_uint64_create(unsigned long v);
extern xpc_object_t xpc_string_create(const char *s);
extern char *xpc_copy_description(xpc_object_t o);
extern void *xpc_get_type(xpc_object_t o);
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
extern dq dispatch_queue_create(const char *label, void *attr);
extern void dispatch_main(void);
extern long write(int fd, const void *buf, unsigned long n);
extern void free(void *p);

static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(2, s, n); }

static void
lower(char *o, const char *s)
{
	unsigned long i;

	for (i = 0; s[i] && i < 254; i++) o[i] = s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i];
	o[i] = 0;
}

static void
handle(xpc_object_t peer, xpc_object_t m)
{
	char *d = xpc_copy_description(m);
	xpc_object_t r;

	out("lsd-stub: message: "); out(d); out("\n");
	free(d);
	if (xpc_get_type(m) == (void *)&_xpc_type_dictionary && xpc_dictionary_get_value(m, "replysig")) return;	/* the answers of an NSXPC message (no "replysig" reply) crash libxpc of the Mac: not answering is the only thing that works here */
	if (xpc_get_type(m) == (void *)&_xpc_type_dictionary && (r = xpc_dictionary_create_reply(m)) != 0) {
		long cmd = xpc_dictionary_get_int64(m, "command");

		/* the answers of the launchservicesd of the Mac (found by asking it with a program on the Mac): 100 check-in, 110 the info page (a shared memory of 15 pages), 200 the meta information
		 * (the session of the graphics server), 505 and 360 (an application that is not known): "success": false */
		if (cmd == 100) {
			xpc_dictionary_set_uint64(r, "serverversion", xpc_dictionary_get_uint64(m, "clientversion"));
			xpc_dictionary_set_bool(r, "success", 1);
			xpc_dictionary_set_int64(r, "session", 100001);
		} else if (cmd == 110) {
			void *pg = mmap(0, 61440, 3, 0x1002, -1, 0);

			if ((long)pg != -1) xpc_dictionary_set_value(r, "lsinfopage", xpc_shmem_create(pg, 61440));
			xpc_dictionary_set_bool(r, "success", 1);
		} else if (cmd == 200) {
			xpc_object_t meta = xpc_dictionary_create(0, 0, 0);

			xpc_dictionary_set_int64(meta, "LSCGSSessionID", 257);
			xpc_dictionary_set_value(r, "metainfo", meta);
			xpc_dictionary_set_bool(r, "success", 1);
		} else if (cmd == 360) {			/* the value of a key for an application (here: the Dock; every key answers its bundle identifier) */
			xpc_object_t keys = xpc_dictionary_get_value(m, "restrictto");
			xpc_object_t res = xpc_dictionary_create(0, 0, 0);

			if (keys && xpc_array_get_count(keys) > 0)
				xpc_dictionary_set_value(res, xpc_string_get_string_ptr(xpc_array_get_value(keys, 0)), xpc_string_create("com.apple.dock"));
			xpc_dictionary_set_value(r, "result", res);
			xpc_dictionary_set_bool(r, "cacheable", 0);
			xpc_dictionary_set_bool(r, "success", 1);
		} else if (cmd == 410) {			/* the registration of a listener for the notifications about applications: an id, the applications that it was asked for, the session */
			xpc_object_t add = xpc_dictionary_get_value(m, "addasn");
			xpc_object_t aff = xpc_array_create(0, 0);
			unsigned long i;

			for (i = 0; add && i < xpc_array_get_count(add); i++)
				xpc_array_append_value(aff, xpc_array_get_value(add, i));
			xpc_dictionary_set_uint64(r, "id", 1);
			xpc_dictionary_set_value(r, "affectedasns", aff);
			xpc_dictionary_set_bool(r, "success", 1);
			xpc_dictionary_set_int64(r, "session", 100001);
		} else if (cmd == 500) {			/* the check-in of an application: the answer is its own information plus what the daemon adds (found by asking the daemon of the Mac with a program) */
			xpc_object_t info = xpc_dictionary_get_value(m, "info");
			xpc_object_t res = info ? xpc_copy(info) : xpc_dictionary_create(0, 0, 0);
			char low[256];
			const char *s;
			unsigned char tok[32];
			static const unsigned char asnid[16] = { 0xc9, 0x61, 0x72, 0xa0, 0xce, 0x74, 0x4d, 0x62, 0xaf, 0xef, 0xec, 0xfe, 0x4d, 0x04, 0xd8, 0x27 };	/* the marker that makes LaunchServices read the dictionary as an application serial number (seen in the answer of the Mac) */
			unsigned long i;
			xpc_object_t asn = xpc_dictionary_create(0, 0, 0);

			for (i = 0; i < 32; i++) tok[i] = 0;
			tok[0] = 0xf5; tok[1] = 1; tok[8] = 0xf5; tok[9] = 1;
			xpc_dictionary_set_value(res, "LSAuditToken", xpc_data_create(tok, 32));
			if ((s = xpc_dictionary_get_string(res, "CFBundleName")) != 0) { lower(low, s); xpc_dictionary_set_value(res, "CFBundleNameLowerCase", xpc_string_create(low)); xpc_dictionary_set_value(res, "LSDisplayName", xpc_string_create(s)); }
			if ((s = xpc_dictionary_get_string(res, "CFBundleIdentifier")) != 0) { lower(low, s); xpc_dictionary_set_value(res, "BundleIdentifierLowerCase", xpc_string_create(low)); }
			if ((s = xpc_dictionary_get_string(res, "CFBundleExecutable")) != 0) {
				lower(low, s);
				xpc_dictionary_set_value(res, "CFBundleExecutablePath", xpc_string_create(s));
				xpc_dictionary_set_value(res, "LSExecutableFilenameLowerCaseKey", xpc_string_create(low));
			}
			xpc_dictionary_set_int64(res, "LSApplicationMemberCoalitionIDKey", 1);
			xpc_dictionary_set_int64(res, "pid", xpc_connection_get_pid(peer));
			xpc_dictionary_set_uint64(asn, "com.apple.coreservices.asn.lo", 100000 + xpc_connection_get_pid(peer));
			xpc_dictionary_set_uint64(asn, "com.apple.coreservices.asn.hi", 0);
			xpc_dictionary_set_value(asn, "com.apple.coreservices.asn", xpc_uuid_create(asnid));
			xpc_dictionary_set_value(res, "LSASN", asn);
			xpc_dictionary_set_bool(res, "LSApplicationHasRegistered", 1);
			xpc_dictionary_set_int64(res, "ChangeCount", 1);
			xpc_dictionary_set_value(r, "result", res);
			xpc_dictionary_set_bool(r, "success", 1);
		} else if (cmd == 505) {
			xpc_dictionary_set_bool(r, "success", 0);
		} else
			xpc_dictionary_set_bool(r, "success", 1);
		if (cmd == 500) { char *rd = xpc_copy_description(r); out("lsd-stub: 500 reply: "); out(rd); out("\n"); free(rd); }
		xpc_connection_send_message(peer, r);
	}
}

int
main(int argc, char **argv)
{
	dq q = dispatch_queue_create("lsd-stub", 0);
	int i;

	/* one listener for each name on the command line (several services in one process: every process of the Mac costs memory here); no name: the LaunchServices daemon */
	for (i = 1; i < (argc > 1 ? argc : 2); i++) {
		xpc_object_t svc = xpc_connection_create_mach_service(argc > 1 ? argv[i] : "com.apple.coreservices.launchservicesd", q, 1);

		xpc_connection_set_event_handler(svc, ^(xpc_object_t peer) {
			out("lsd-stub: a connection\n");
			xpc_connection_set_event_handler(peer, ^(xpc_object_t m) { handle(peer, m); });
			xpc_connection_resume(peer);
		});
		xpc_connection_resume(svc);
	}
	out("lsd-stub: serving\n");
	dispatch_main();
	return (0);
}
