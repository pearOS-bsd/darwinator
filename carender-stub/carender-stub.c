/* A stand-in for com.apple.CARenderServer, only to see what a client (the Dock) sends it: it takes the service with bootstrap_check_in, receives every message on the port and prints the header and the first
 * bytes of the body to stderr (a complex message: also its descriptors).  It sends no answer.  It is the first step of the replacement of the CoreAnimation render server of the WindowServer. */
typedef unsigned int u32;
typedef unsigned long u64;
extern u32 bootstrap_port;
extern int bootstrap_check_in(u32 bp, const char *name, u32 *port);
extern int mach_msg(void *msg, int opt, u32 ssz, u32 rsz, u32 rname, u32 to, u32 notify);
extern long write(int fd, const void *buf, unsigned long n);

static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(2, s, n); }
static void hex(u64 v, int digits) { char b[20]; int i; for (i = 0; i < digits; i++) b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15]; b[digits] = 0; out(b); }

int
main(void)
{
	static u32 buf[4096];
	u32 port = 0, i, n;
	int kr = bootstrap_check_in(bootstrap_port, "com.apple.CARenderServer", &port);

	out("carender-stub: check-in "); hex((u32)kr, 8); out(" port "); hex(port, 8); out("\n");
	for (;;) {
		kr = mach_msg(buf, 2 | 0x100000 /* RCV_MSG | RCV_LARGE */, 0, sizeof(buf), port, 0, 0);
		if (kr != 0) { out("carender-stub: receive error "); hex((u32)kr, 8); out("\n"); continue; }
		out("carender-stub: msg id "); hex(buf[5], 8); out(" size "); hex(buf[1], 8); out(" bits "); hex(buf[0], 8); out(" remote "); hex(buf[2], 8); out(" local "); hex(buf[3], 8); out("\n  ");
		n = buf[1] / 4; if (n > 64) n = 64;
		for (i = 6; i < n; i++) { hex(buf[i], 8); out(i % 8 == 5 ? "\n  " : " "); }
		out("\n");
	}
}
