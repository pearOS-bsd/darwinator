/* A stand-in for com.apple.CARenderServer, only to see what a client (the Dock) sends it: it takes the service with bootstrap_check_in, receives every message on the port and prints the header and the first
 * bytes of the body to stderr (a complex message: also its descriptors).  It sends no answer.  It is the first step of the replacement of the CoreAnimation render server of the WindowServer. */
typedef unsigned int u32;
typedef unsigned long u64;
extern u32 bootstrap_port;
extern int bootstrap_check_in(u32 bp, const char *name, u32 *port);
extern int mach_msg(void *msg, int opt, u32 ssz, u32 rsz, u32 rname, u32 to, u32 notify);
extern int mach_port_allocate(u32 task, u32 right, u32 *name);
extern int mach_port_insert_right(u32 task, u32 name, u32 poly, u32 how);
extern int mach_port_insert_member(u32 task, u32 name, u32 pset);
extern u32 mach_task_self_;
extern long write(int fd, const void *buf, unsigned long n);

static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(2, s, n); }
extern int open(const char *, int, ...);
extern int close(int);
static void hex(u64 v, int digits) { char b[20]; int i; for (i = 0; i < digits; i++) b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15]; b[digits] = 0; out(b); }

/*
 * CASRegisterClient (id 40202 = 0x9d0a), as the QuartzCore of the Mac sends it and reads the answer (found in its code): the request is complex with five port descriptors; the answer (id 0x9d6e) is complex
 * with two send rights, then NDR(8), two numbers, the offset and the count of a byte array (at most 64 bytes, padded to 4) and one more number at the end: size 0x50 + the padded count.
 */
static void
answer_register(u32 *buf, u32 p1, u32 p2, u32 n1, u32 n2, u32 last)
{
	u32 rep[32];
	int i;

	for (i = 0; i < 32; i++) rep[i] = 0;
	rep[0] = 0x80000012; rep[1] = 0x50; rep[2] = buf[2]; rep[5] = 0x9d6e;
	rep[6] = 2;
	rep[7] = p1; rep[8] = 0; rep[9] = 0x00130000;			/* the descriptors: a send right each (copied) */
	rep[10] = p2; rep[11] = 0; rep[12] = 0x00130000;
	rep[15] = n1; rep[16] = n2;					/* after the NDR (words 13, 14): the two numbers */
	rep[17] = 0; rep[18] = 0;					/* offset, count of the array: empty */
	rep[19] = last;
	mach_msg(rep, 1, 0x50, 0, 0, 0, 0);
}

int
main(void)
{
	static u32 buf[4096];
	u32 port = 0, set = 0, p1 = 0, p2 = 0, i, n;
	int kr = bootstrap_check_in(bootstrap_port, "com.apple.CARenderServer", &port);

	out("carender-stub: check-in "); hex((u32)kr, 8); out(" port "); hex(port, 8); out("\n");
	mach_port_allocate(mach_task_self_, 3 /* PORT_SET */, &set);
	mach_port_insert_member(mach_task_self_, port, set);
	mach_port_allocate(mach_task_self_, 1, &p1); mach_port_insert_right(mach_task_self_, p1, p1, 20); mach_port_insert_member(mach_task_self_, p1, set);
	mach_port_allocate(mach_task_self_, 1, &p2); mach_port_insert_right(mach_task_self_, p2, p2, 20); mach_port_insert_member(mach_task_self_, p2, set);
	for (;;) {
		kr = mach_msg(buf, 2 | 0x100000 /* RCV_MSG | RCV_LARGE */, 0, sizeof(buf), set, 0, 0);
		if (kr != 0) { out("carender-stub: receive error "); hex((u32)kr, 8); out("\n"); continue; }
		out("carender-stub: msg id "); hex(buf[5], 8); out(" size "); hex(buf[1], 8); out(" bits "); hex(buf[0], 8); out(" remote "); hex(buf[2], 8); out(" local "); hex(buf[3], 8); out("\n  ");
		if (buf[5] >= 0x9c42 && buf[5] <= 0x9c45) {	/* the command streams are kept whole in /tmp/ca.bin: a u32 length, then the message */
			int cf = open("/tmp/ca.bin", 0x209, 0644);

			if (cf >= 0) { write(cf, &buf[1], 4); write(cf, buf, buf[1]); close(cf); }
		}
		n = buf[1] / 4; if (n > 4000) n = 4000;
		for (i = 6; i < n; i++) { hex(buf[i], 8); out(i % 8 == 5 ? "\n  " : " "); }
		out("\n");
		{
			/* the simple answers (found in the code of QuartzCore that sends the request and reads the answer): the id of the request, the size of the answer, the numbers that follow the return code */
			static const struct { u32 id, size, v[4]; } simple[] = {
				{ 0x9d3e, 0x28, { 16, 0, 0, 0 } },			/* CARenderServerGetNeededAlignment */
				{ 0x9d51, 0x30, { 16384, 16384, 0, 0 } },		/* CARenderServerGetMaxRenderableIOSurfaceSize */
			};
			u32 k, rp[16];

			for (k = 0; k < sizeof(simple) / sizeof(simple[0]); k++)
				if (simple[k].id == buf[5]) {
					for (i = 0; i < 16; i++) rp[i] = 0;
					rp[0] = 0x12; rp[1] = simple[k].size; rp[2] = buf[2]; rp[5] = buf[5] + 100;
					for (i = 0; i < 4 && 9 + i < simple[k].size / 4; i++) rp[9 + i] = simple[k].v[i];
					mach_msg(rp, 1, simple[k].size, 0, 0, 0, 0);
					out("carender-stub: answered "); hex(buf[5], 8); out("\n");
				}
		}
		if (buf[5] == 0x9c42 && (buf[0] & 0x80000000) && buf[6] >= 1 && ((buf[9] >> 16) & 0xff) == 0x12) {	/* a commit with a send-once right (the Dock waits on it): an empty message to it */
			u32 ack[6];

			ack[0] = 0x12; ack[1] = 24; ack[2] = buf[7]; ack[3] = 0; ack[4] = 0; ack[5] = 0;
			kr = mach_msg(ack, 1, 24, 0, 0, 0, 0);
			out("carender-stub: commit acknowledged "); hex((u32)kr, 8); out("\n");
		}
		if (buf[5] == 0x9d0a) { answer_register(buf, p1, p2, 1, 1, 0); out("carender-stub: answered the register\n"); }
	}
}
