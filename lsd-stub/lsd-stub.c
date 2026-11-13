/* A stand-in for launchservicesd: it takes the service com.apple.coreservices.launchservicesd (the check-in through launchd, with the libxpc of the Mac), prints every message that a client sends
 * (the description of libxpc, to stderr) and answers with an empty dictionary.  It is for finding out what the real Dock asks LaunchServices; it is the first of the replacements. */
typedef void *xpc_object_t;
typedef void *dq;
extern xpc_object_t xpc_connection_create_mach_service(const char *name, dq q, unsigned long flags);
extern void xpc_connection_set_event_handler(xpc_object_t c, void (^h)(xpc_object_t));
extern void xpc_connection_resume(xpc_object_t c);
extern void xpc_connection_cancel(xpc_object_t c);
extern void xpc_release(xpc_object_t o);
extern void xpc_dictionary_set_string(xpc_object_t d, const char *k, const char *v);
extern void xpc_connection_send_message(xpc_object_t c, xpc_object_t m);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t m);
extern long xpc_dictionary_get_int64(xpc_object_t d, const char *k);
extern unsigned long xpc_dictionary_get_uint64(xpc_object_t d, const char *k);
extern void xpc_dictionary_set_uint64(xpc_object_t d, const char *k, unsigned long v);
extern void xpc_dictionary_set_int64(xpc_object_t d, const char *k, long v);
extern void xpc_dictionary_set_bool(xpc_object_t d, const char *k, int v);
extern void xpc_dictionary_set_data(xpc_object_t d, const char *k, const void *v, unsigned long n);
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

static int
streq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

static void
lower(char *o, const char *s)
{
	unsigned long i;

	for (i = 0; s[i] && i < 254; i++) o[i] = s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i];
	o[i] = 0;
}

/* the keyed archive that the runningboardd of the Mac sends with a process handle (a BSAuditToken under the key "35770"); the token is at offset 126 */
static const unsigned char rbs_archive[278] = {0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd4,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x0a,0x58,0x24,0x76,0x65,0x72,0x73,0x69,0x6f,0x6e,0x59,0x24,0x61,0x72,0x63,0x68,0x69,0x76,0x65,0x72,0x54,0x24,0x74,0x6f,0x70,0x58,0x24,0x6f,0x62,0x6a,0x65,0x63,0x74,0x73,0x12,0x00,0x01,0x86,0xa0,0x5f,0x10,0x0f,0x4e,0x53,0x4b,0x65,0x79,0x65,0x64,0x41,0x72,0x63,0x68,0x69,0x76,0x65,0x72,0xd1,0x08,0x09,0x55,0x33,0x35,0x37,0x37,0x30,0x80,0x01,0xa4,0x0b,0x0c,0x11,0x12,0x55,0x24,0x6e,0x75,0x6c,0x6c,0xd2,0x0d,0x0e,0x0f,0x10,0x56,0x24,0x63,0x6c,0x61,0x73,0x73,0x5b,0x5f,0x61,0x75,0x64,0x69,0x74,0x54,0x6f,0x6b,0x65,0x6e,0x80,0x03,0x80,0x02,0x4f,0x10,0x20,0xf5,0x01,0x00,0x00,0xf5,0x01,0x00,0x00,0x14,0x00,0x00,0x00,0xf5,0x01,0x00,0x00,0x14,0x00,0x00,0x00,0x2e,0x4e,0x00,0x00,0xc0,0x87,0x01,0x00,0x61,0xcb,0x00,0x00,0xd2,0x13,0x14,0x15,0x16,0x5a,0x24,0x63,0x6c,0x61,0x73,0x73,0x6e,0x61,0x6d,0x65,0x58,0x24,0x63,0x6c,0x61,0x73,0x73,0x65,0x73,0x5c,0x42,0x53,0x41,0x75,0x64,0x69,0x74,0x54,0x6f,0x6b,0x65,0x6e,0xa2,0x17,0x18,0x5c,0x42,0x53,0x41,0x75,0x64,0x69,0x74,0x54,0x6f,0x6b,0x65,0x6e,0x58,0x4e,0x53,0x4f,0x62,0x6a,0x65,0x63,0x74,0x08,0x11,0x1a,0x24,0x29,0x32,0x37,0x49,0x4c,0x52,0x54,0x59,0x5f,0x64,0x6b,0x77,0x79,0x7b,0x9e,0xa3,0xae,0xb7,0xc4,0xc7,0xd4,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x19,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xdd};

/* the error that the runningboardd of the Mac answers to a refused call (an NSError in a keyed archive under the key "36190"): used for the calls that are not served */
static const unsigned char rbs_error[492] = {0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd4,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x0a,0x58,0x24,0x76,0x65,0x72,0x73,0x69,0x6f,0x6e,0x59,0x24,0x61,0x72,0x63,0x68,0x69,0x76,0x65,0x72,0x54,0x24,0x74,0x6f,0x70,0x58,0x24,0x6f,0x62,0x6a,0x65,0x63,0x74,0x73,0x12,0x00,0x01,0x86,0xa0,0x5f,0x10,0x0f,0x4e,0x53,0x4b,0x65,0x79,0x65,0x64,0x41,0x72,0x63,0x68,0x69,0x76,0x65,0x72,0xd1,0x08,0x09,0x55,0x33,0x36,0x31,0x39,0x30,0x80,0x01,0xa8,0x0b,0x0c,0x15,0x16,0x1e,0x1f,0x20,0x26,0x55,0x24,0x6e,0x75,0x6c,0x6c,0xd4,0x0d,0x0e,0x0f,0x10,0x11,0x12,0x13,0x14,0x56,0x4e,0x53,0x43,0x6f,0x64,0x65,0x5a,0x4e,0x53,0x55,0x73,0x65,0x72,0x49,0x6e,0x66,0x6f,0x58,0x4e,0x53,0x44,0x6f,0x6d,0x61,0x69,0x6e,0x56,0x24,0x63,0x6c,0x61,0x73,0x73,0x10,0x01,0x80,0x03,0x80,0x02,0x80,0x07,0x5f,0x10,0x15,0x52,0x42,0x53,0x53,0x65,0x72,0x76,0x69,0x63,0x65,0x45,0x72,0x72,0x6f,0x72,0x44,0x6f,0x6d,0x61,0x69,0x6e,0xd3,0x17,0x18,0x10,0x19,0x1b,0x1d,0x57,0x4e,0x53,0x2e,0x6b,0x65,0x79,0x73,0x5a,0x4e,0x53,0x2e,0x6f,0x62,0x6a,0x65,0x63,0x74,0x73,0xa1,0x1a,0x80,0x04,0xa1,0x1c,0x80,0x05,0x80,0x06,0x5f,0x10,0x18,0x4e,0x53,0x4c,0x6f,0x63,0x61,0x6c,0x69,0x7a,0x65,0x64,0x46,0x61,0x69,0x6c,0x75,0x72,0x65,0x52,0x65,0x61,0x73,0x6f,0x6e,0x5f,0x10,0x48,0x6f,0x72,0x69,0x67,0x69,0x6e,0x61,0x74,0x6f,0x72,0x20,0x64,0x6f,0x65,0x73,0x6e,0x27,0x74,0x20,0x68,0x61,0x76,0x65,0x20,0x65,0x6e,0x74,0x69,0x74,0x6c,0x65,0x6d,0x65,0x6e,0x74,0x20,0x63,0x6f,0x6d,0x2e,0x61,0x70,0x70,0x6c,0x65,0x2e,0x70,0x72,0x69,0x76,0x61,0x74,0x65,0x2e,0x72,0x75,0x6e,0x6e,0x69,0x6e,0x67,0x62,0x6f,0x61,0x72,0x64,0x2e,0x72,0x6f,0x6c,0x65,0x73,0xd2,0x21,0x22,0x23,0x24,0x5a,0x24,0x63,0x6c,0x61,0x73,0x73,0x6e,0x61,0x6d,0x65,0x58,0x24,0x63,0x6c,0x61,0x73,0x73,0x65,0x73,0x5c,0x4e,0x53,0x44,0x69,0x63,0x74,0x69,0x6f,0x6e,0x61,0x72,0x79,0xa2,0x23,0x25,0x58,0x4e,0x53,0x4f,0x62,0x6a,0x65,0x63,0x74,0xd2,0x21,0x22,0x27,0x28,0x57,0x4e,0x53,0x45,0x72,0x72,0x6f,0x72,0xa2,0x27,0x25,0x00,0x08,0x00,0x11,0x00,0x1a,0x00,0x24,0x00,0x29,0x00,0x32,0x00,0x37,0x00,0x49,0x00,0x4c,0x00,0x52,0x00,0x54,0x00,0x5d,0x00,0x63,0x00,0x6c,0x00,0x73,0x00,0x7e,0x00,0x87,0x00,0x8e,0x00,0x90,0x00,0x92,0x00,0x94,0x00,0x96,0x00,0xae,0x00,0xb5,0x00,0xbd,0x00,0xc8,0x00,0xca,0x00,0xcc,0x00,0xce,0x00,0xd0,0x00,0xd2,0x00,0xed,0x01,0x38,0x01,0x3d,0x01,0x48,0x01,0x51,0x01,0x5e,0x01,0x61,0x01,0x6a,0x01,0x6f,0x01,0x77,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x29,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x7a};

static void
handle(xpc_object_t peer, xpc_object_t m, int nsxpc, const char *svc)
{
	char *d = xpc_copy_description(m);
	xpc_object_t r;

	out("lsd-stub: message ("); out(svc); out("): "); out(d); out("\n");
	free(d);
	if (xpc_get_type(m) == (void *)&_xpc_type_dictionary && xpc_dictionary_get_value(m, "replysig")) {
		xpc_object_t nr;

		/* a service whose clients wait for the answer (the fonts): an empty NSXPC answer (the client reports an error and goes on) */
		if (nsxpc && (nr = xpc_dictionary_create_reply(m)) != 0) {
			/* the reply of NSXPC: only "root", the archive of the arguments of the reply block (found with a real NSXPC service: this is (nil, nil)) */
			static const unsigned char nilnil[36] = { 0x62,0x70,0x6c,0x69,0x73,0x74,0x31,0x37, 0xa0,0x23,0,0,0,0,0,0, 0,0xe0,0x76,0x76,0x40,0x3f,0x40,0x40, 0,0xa0,0x23,0,0,0,0,0, 0,0,0xe0,0xe0 };

			static const unsigned char boolno[35] = { 0x62,0x70,0x6c,0x69,0x73,0x74,0x31,0x37, 0xa0,0x22,0,0,0,0,0,0, 0,0xe0,0x75,0x76,0x40,0x3f,0x63,0, 0xa0,0x22,0,0,0,0,0,0, 0,0x11,0 };	/* a reply block with one BOOL: NO */
			const char *sig = xpc_dictionary_get_string(m, "replysig");

			if (sig != 0 && streq(sig, "v12@?0c8"))
				xpc_dictionary_set_value(nr, "root", xpc_data_create(boolno, sizeof(boolno)));
			else
				xpc_dictionary_set_value(nr, "root", xpc_data_create(nilnil, sizeof(nilnil)));
			xpc_dictionary_set_uint64(nr, "sequence", xpc_dictionary_get_uint64(m, "sequence"));
			xpc_connection_send_message(peer, nr);
			{ out("lsd-stub: empty NSXPC answer sent for "); out(svc); out("\n"); }
		} else
			{ out("lsd-stub: NO answer to an NSXPC message of "); out(svc); out(" (not in the NSXPC list)\n"); }
		return;
	}	/* the answers of an NSXPC message (no "replysig" reply) crash libxpc of the Mac: not answering is the only thing that works here */
	if (xpc_get_type(m) == (void *)&_xpc_type_dictionary && (r = xpc_dictionary_create_reply(m)) != 0) {
		long cmd = xpc_dictionary_get_int64(m, "command");

		if (streq(svc, "com.apple.pasteboard.1") && xpc_dictionary_get_string(m, "com.apple.pboard.message") != 0) {
			/* the pasteboard server: the answer of the one of the Mac to "create" (found with a program): the change count, the generation, whether we own it, a uuid */
			static const unsigned char u[16] = { 0x4c, 0xfb, 0x29, 0x07, 0x91, 0x27, 0x41, 0xd7, 0x85, 0x41, 0x3f, 0x3a, 0x41, 0x7b, 0x0b, 0x19 };

			xpc_dictionary_set_uint64(r, "com.apple.pboard.change", 1);
			xpc_dictionary_set_uint64(r, "com.apple.pboard.generation", 1);
			xpc_dictionary_set_bool(r, "com.apple.pboard.isOwner", 0);
			xpc_dictionary_set_value(r, "com.apple.pboard.uuid", xpc_uuid_create(u));
			xpc_connection_send_message(peer, r);
			return;
		}
		if (streq(svc, "com.apple.pluginkit.pkd") && xpc_dictionary_get_string(m, "request") != 0) {
			/* PlugInKit: {"matches": {key: data}}; the data is a bplist15 of an empty list of plug-ins (the answer of the pkd of the Mac to a match with no result, found with a program) */
			static const unsigned char bp[23] = { 'b','p','l','i','s','t','1','5', 0x13, 0x17, 0,0,0,0,0,0, 0x80, 0x12, 0,0,0,0, 0xa0 };
			xpc_object_t mt = xpc_dictionary_create(0, 0, 0);

			xpc_dictionary_set_data(mt, "ECF19A18-7AA6-4141-B4DC-A2E5123B2B5C", bp, sizeof(bp));
			xpc_dictionary_set_value(r, "matches", mt);
			xpc_connection_send_message(peer, r);
			return;
		}

		if (streq(svc, "com.apple.runningboard") && xpc_dictionary_get_string(m, "rbs_selector") != 0) {
			/* RunningBoard: the answer to the handshake (the shape found by asking the runningboardd of the Mac with a program): the response with the handle of the calling process.  Anything else: no answer, and the connection ends (the client is told) */
			const char *sel = xpc_dictionary_get_string(m, "rbs_selector");

			if (streq(sel, "handshakeWithRequest:")) {
				long pid = xpc_connection_get_pid(peer);
				xpc_object_t rv = xpc_dictionary_create(0, 0, 0), h = xpc_dictionary_create(0, 0, 0), ch = xpc_dictionary_create(0, 0, 0), id = xpc_dictionary_create(0, 0, 0);
				xpc_object_t at = xpc_dictionary_create(0, 0, 0), bu = xpc_dictionary_create(0, 0, 0);

				xpc_dictionary_set_string(rv, "bsx_class", "RBSHandshakeResponse");
				xpc_dictionary_set_value(rv, "success-keys", xpc_array_create(0, 0));
				xpc_dictionary_set_value(rv, "success-values", xpc_array_create(0, 0));
				xpc_dictionary_set_value(rv, "fail-keys", xpc_array_create(0, 0));
				xpc_dictionary_set_value(rv, "fail-values", xpc_array_create(0, 0));
				xpc_dictionary_set_string(id, "bsx_class", "RBSOpaqueProcessIdentity");
				xpc_dictionary_set_int64(id, "_pid", pid);
				xpc_dictionary_set_int64(id, "_auid", 0);
				xpc_dictionary_set_string(id, "_description", "anon<lsd>(0)");
				xpc_dictionary_set_value(ch, "_identity", id);
				xpc_dictionary_set_uint64(ch, "_pid", pid);
				{
					unsigned char ar[278];
					unsigned long q;
					unsigned int w[8] = { 0, 0, 0, 0, 0, (unsigned int)pid, 100001, 1 };

					for (q = 0; q < 278; q++) ar[q] = rbs_archive[q];
					for (q = 0; q < 32; q++) ar[126 + q] = ((unsigned char *)w)[q];
					xpc_dictionary_set_value(ch, "bsx_archive", xpc_data_create(ar, 278));
				}
				xpc_dictionary_set_string(at, "bsx_index", "35770");
				xpc_dictionary_set_value(ch, "_bsAuditToken", at);
				xpc_dictionary_set_uint64(ch, "_data", 0);
				xpc_dictionary_set_string(bu, "bsx_class", "RBSProcessBundle");
				xpc_dictionary_set_value(ch, "_bundle", bu);
				xpc_dictionary_set_value(h, "_codedHandle", ch);
				xpc_dictionary_set_string(h, "bsx_class", "RBSProcessHandle");
				xpc_dictionary_set_value(rv, "_handle", h);
				xpc_dictionary_set_value(r, "return-value", rv);
				xpc_connection_send_message(peer, r);
				return;
			}
			if (sel[0] == 'v' && sel[1] == 'o' && sel[2] == 'i' && sel[3] == 'd' && sel[4] == '_') {	/* a call without a return value: an empty reply */
				xpc_connection_send_message(peer, r);
				return;
			}
			if (streq(sel, "acquireAssertionWithDescriptor:error:")) {	/* refused: the error of the Mac for a client without the entitlement */
				xpc_object_t er = xpc_dictionary_create(0, 0, 0);

				xpc_dictionary_set_value(r, "bsx_archive", xpc_data_create(rbs_error, 492));
				xpc_dictionary_set_string(er, "bsx_index", "36190");
				xpc_dictionary_set_value(r, "__RBSXPCError__", er);
				xpc_connection_send_message(peer, r);
				return;
			}
			xpc_release(r);					/* not sent: the destruction of the reply drops the right to reply, and the client is told that no answer will come */
			xpc_connection_cancel(peer);
			return;
		}
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
		const char *nm = argc > 1 ? argv[i] : "";
		int nsx = streq(nm, "com.apple.fonts") || streq(nm, "com.apple.carboncore.csnameddata") || streq(nm, "com.apple.lsd.mapdb") || streq(nm, "com.apple.analyticsd") || streq(nm, "com.apple.bird") || streq(nm, "com.apple.bird.token") || streq(nm, "com.apple.metadata.mds") || streq(nm, "com.apple.quicklook.ThumbnailsAgent") || streq(nm, "com.apple.quicklook.QuickLookUIService") || streq(nm, "com.apple.widget-observation");	/* the services whose clients wait for an NSXPC answer */

		xpc_connection_set_event_handler(svc, ^(xpc_object_t peer) {
			out("lsd-stub: a connection\n");
			xpc_connection_set_event_handler(peer, ^(xpc_object_t m) { handle(peer, m, nsx, nm); });
			xpc_connection_resume(peer);
		});
		xpc_connection_resume(svc);
	}
	out("lsd-stub: serving\n");
	dispatch_main();
	return (0);
}
