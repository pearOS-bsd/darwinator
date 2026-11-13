/* A service that answers every request with the plain success: svc-stub NAME takes the service NAME from launchd (the check-in), prints the id of each request and answers with a simple message (the id plus 100,
 * size 0x28: the return code 0 and one number 0).  For the services of the Mac that a program only needs to hear "done" from (power management: IOPMLib's io_pm_* routines); a service that needs real answers has its own program. */
typedef unsigned int u32;
typedef unsigned long u64;
extern u32 bootstrap_port;
extern int bootstrap_check_in(u32 bp, const char *name, u32 *port);
extern int mach_msg(void *msg, int opt, u32 ssz, u32 rsz, u32 rname, u32 to, u32 notify);
extern long write(int fd, const void *buf, unsigned long n);
extern int mach_port_allocate(u32 task, u32 right, u32 *name);
extern int mach_port_insert_right(u32 task, u32 name, u32 poly, u32 how);
extern int mach_port_insert_member(u32 task, u32 name, u32 pset);
extern u32 mach_task_self_;

static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(2, s, n); }
static void hex(u64 v, int digits) { char b[20]; int i; for (i = 0; i < digits; i++) b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15]; b[digits] = 0; out(b); }

int
main(int argc, char **argv)
{
	static u32 buf[4096];
	u32 port = 0, pset = 0, rep[10], i;
	int kr;

	if (argc < 2) { out("usage: svc-stub NAME\n"); return (1); }
	kr = bootstrap_check_in(bootstrap_port, argv[1], &port);
	out("svc-stub: "); out(argv[1]); out(" check-in "); hex((u32)kr, 8); out(" port "); hex(port, 8); out("\n");
	mach_port_allocate(mach_task_self_, 3 /* PORT_SET */, &pset);
	mach_port_insert_member(mach_task_self_, port, pset);
	for (;;) {
		kr = mach_msg(buf, 2 | 0x100000, 0, sizeof(buf), pset, 0, 0);
		if (kr != 0) continue;
		out("svc-stub: id "); hex(buf[5], 8); out(" size "); hex(buf[1], 8); out("\n");
		if (buf[2] == 0)
			continue;
		if (buf[5] == 20000) {		/* SystemConfiguration's configopen (SCDynamicStoreCreate): the answer carries a send right (the session) and the status 0; the other calls of the session fail (no key) */
			u32 np = 0, cm[16];

			mach_port_allocate(mach_task_self_, 1, &np);
			mach_port_insert_right(mach_task_self_, np, np, 20);
			mach_port_insert_member(mach_task_self_, np, pset);
			for (i = 0; i < 16; i++) cm[i] = 0;
			cm[0] = 0x80000012; cm[1] = 0x34; cm[2] = buf[2]; cm[5] = 20100;
			cm[6] = 1; cm[7] = np; cm[8] = 0; cm[9] = 0x00130000;		/* one port descriptor (copy send) */
			cm[10] = 0; cm[11] = 1;						/* NDR */
			cm[12] = 0;							/* the status */
			mach_msg(cm, 1, 0x34, 0, 0, 0, 0);
			continue;
		}
		if (buf[5] >= 20000 && buf[5] < 20100) {	/* the other routines of configd: an error status (kSCStatusNoKey 1003 in the return code position) */
			u32 em[10];

			for (i = 0; i < 10; i++) em[i] = 0;
			em[0] = 0x12; em[1] = 0x24; em[2] = buf[2]; em[5] = buf[5] + 100; em[8] = 1003;
			mach_msg(em, 1, 0x24, 0, 0, 0, 0);
			continue;
		}
		for (i = 0; i < 10; i++) rep[i] = 0;
		rep[0] = 0x12; rep[1] = 0x28; rep[2] = buf[2]; rep[5] = buf[5] + 100;
		mach_msg(rep, 1, 0x28, 0, 0, 0, 0);
	}
}
