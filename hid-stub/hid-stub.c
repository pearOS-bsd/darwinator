/* A stand-in for the server of com.apple.iohideventsystem (the HID event system that the window server hosts): it takes the service from launchd (the port was made at the start), prints the id of every
 * request and answers the open (id 0x11170) with a port (a complex reply with one send right, as the IOKit library of the Mac expects) and everything else with the plain "success" (KERN_SUCCESS, no data). */
typedef unsigned int u32;
typedef unsigned long u64;
extern u32 bootstrap_port, mach_task_self_;
extern int bootstrap_check_in(u32 bp, const char *name, u32 *port);
extern int mach_msg(void *msg, int opt, u32 ssz, u32 rsz, u32 rname, u32 to, u32 notify);
extern int mach_port_allocate(u32 task, u32 right, u32 *name);
extern int mach_port_insert_right(u32 task, u32 name, u32 poly, u32 how);
extern long write(int fd, const void *buf, unsigned long n);

static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(2, s, n); }
static void hex(u64 v, int digits) { char b[20]; int i; for (i = 0; i < digits; i++) b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15]; b[digits] = 0; out(b); }

int
main(void)
{
	static u32 buf[4096], rep[16];
	u32 port = 0, session = 0;
	int kr = bootstrap_check_in(bootstrap_port, "com.apple.iohideventsystem", &port);

	out("hid-stub: check-in "); hex((u32)kr, 8); out(" port "); hex(port, 8); out("\n");
	mach_port_allocate(mach_task_self_, 1, &session);
	mach_port_insert_right(mach_task_self_, session, session, 20);
	for (;;) {
		u32 id, i;

		kr = mach_msg(buf, 2 | 0x100000, 0, sizeof(buf), port, 0, 0);
		if (kr != 0) { out("hid-stub: receive error "); hex((u32)kr, 8); out("\n"); continue; }
		id = buf[5];
		out("hid-stub: id "); hex(id, 8); out(" size "); hex(buf[1], 8); out(" bits "); hex(buf[0], 8); out("\n");
		for (i = 0; i < 16; i++) rep[i] = 0;
		rep[2] = buf[2]; rep[5] = id + 100;			/* the reply port of the request (its remote port) is the destination of the answer */
		if (id == 0x11170) {					/* the open: a complex answer with a send right (the session port) */
			rep[0] = 0x80000012; rep[1] = 40;
			rep[6] = 1; rep[7] = session; rep[8] = 0; rep[9] = 0x00130000;
			rep[0] = 0x80000000 | 0x12;			/* complex, remote disposition MOVE_SEND_ONCE (18) */
			kr = mach_msg(rep, 1, 40, 0, 0, 0, 0);
			out("hid-stub: open answered "); hex((u32)kr, 8); out("\n");
		} else {
			rep[0] = 0x12; rep[1] = 36; rep[6] = 0; rep[7] = 0; rep[8] = 0;
			mach_msg(rep, 1, 36, 0, 0, 0, 0);
		}
	}
}
