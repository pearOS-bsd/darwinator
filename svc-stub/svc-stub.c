/* A service that answers every request with the plain success: svc-stub NAME takes the service NAME from launchd (the check-in), prints the id of each request and answers with a simple message (the id plus 100,
 * size 0x28: the return code 0 and one number 0).  For the services of the Mac that a program only needs to hear "done" from (power management: IOPMLib's io_pm_* routines); a service that needs real answers has its own program. */
typedef unsigned int u32;
typedef unsigned long u64;
extern u32 bootstrap_port;
extern int bootstrap_check_in(u32 bp, const char *name, u32 *port);
extern int mach_msg(void *msg, int opt, u32 ssz, u32 rsz, u32 rname, u32 to, u32 notify);
extern long write(int fd, const void *buf, unsigned long n);

static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(2, s, n); }
static void hex(u64 v, int digits) { char b[20]; int i; for (i = 0; i < digits; i++) b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15]; b[digits] = 0; out(b); }

int
main(int argc, char **argv)
{
	static u32 buf[4096];
	u32 port = 0, rep[10], i;
	int kr;

	if (argc < 2) { out("usage: svc-stub NAME\n"); return (1); }
	kr = bootstrap_check_in(bootstrap_port, argv[1], &port);
	out("svc-stub: "); out(argv[1]); out(" check-in "); hex((u32)kr, 8); out(" port "); hex(port, 8); out("\n");
	for (;;) {
		kr = mach_msg(buf, 2 | 0x100000, 0, sizeof(buf), port, 0, 0);
		if (kr != 0) continue;
		out("svc-stub: id "); hex(buf[5], 8); out(" size "); hex(buf[1], 8); out("\n");
		if (buf[2] == 0)
			continue;
		for (i = 0; i < 10; i++) rep[i] = 0;
		rep[0] = 0x12; rep[1] = 0x28; rep[2] = buf[2]; rep[5] = buf[5] + 100;
		mach_msg(rep, 1, 0x28, 0, 0, 0, 0);
	}
}
