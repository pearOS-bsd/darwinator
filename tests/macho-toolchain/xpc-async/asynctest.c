/* an XPC client with an asynchronous reply (xpc_connection_send_message_with_reply + a block): the reply has to reach the block with its context */
typedef void *xpc_object_t;
typedef void *dq;
extern xpc_object_t xpc_connection_create_mach_service(const char *name, dq q, unsigned long flags);
extern void xpc_connection_set_event_handler(xpc_object_t c, void (^h)(xpc_object_t));
extern void xpc_connection_resume(xpc_object_t c);
extern void xpc_connection_send_message_with_reply(xpc_object_t c, xpc_object_t m, dq q, void (^h)(xpc_object_t));
extern xpc_object_t xpc_dictionary_create(const char *const *keys, const xpc_object_t *values, unsigned long count);
extern void xpc_dictionary_set_int64(xpc_object_t d, const char *k, long v);
extern void xpc_dictionary_set_uint64(xpc_object_t d, const char *k, unsigned long v);
extern char *xpc_copy_description(xpc_object_t o);
extern dq dispatch_get_global_queue(long id, unsigned long flags);
extern dq dispatch_queue_create(const char *l, void *a);
#ifndef ASYNCTEST_WAIT
#define ASYNCTEST_WAIT 0
#endif
extern unsigned int sleep(unsigned int);
extern long write(int, const void *, unsigned long);
extern void free(void *);
static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(1, s, n); }

int
main(void)
{
	xpc_object_t c = xpc_connection_create_mach_service("com.apple.coreservices.launchservicesd", 0, 0);
	xpc_object_t m;

	sleep(ASYNCTEST_WAIT);	/* time for a debugger to attach */

	xpc_connection_set_event_handler(c, ^(xpc_object_t e) { });
	xpc_connection_resume(c);
	m = xpc_dictionary_create(0, 0, 0);
	xpc_dictionary_set_int64(m, "command", 100);
	xpc_dictionary_set_uint64(m, "clientversion", 1);
	xpc_dictionary_set_int64(m, "session", -2);
	xpc_connection_send_message_with_reply(c, m, dispatch_get_global_queue(0, 0), ^(xpc_object_t r) {
		char *d = xpc_copy_description(r);

		out("asynctest: reply: "); out(d); out("\n");
		free(d);
	});
	sleep(3);
	out("asynctest: done\n");
	return 0;
}
