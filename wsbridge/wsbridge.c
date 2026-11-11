/*
 * wsbridge: the front of the window server for programs of the Mac.  SkyLight (the client library in the dyld cache) sends its MIG messages to a Mach port named by the bootstrap
 * service com.apple.windowserver.active; wsd (github.com/pearOS-bsd/misc, wsbsd) speaks the same messages on a Unix socket.  This program is the service: it takes the name
 * from the launchd, receives the Mach messages and (next steps) forwards them to wsd and the replies back, turning port rights into descriptors and the other way round.
 *
 * First step, now: it only listens and logs each message (id, size, sender, bits) and answers requests with the MIG error "bad id", so that the clients fail instead of waiting.  The log
 * is what the plan of the bridge is made from (which routines the Dock, the Finder and the SystemUIServer need first).  Own code (BSD); no header of Apple's.
 */
typedef unsigned int u32;
extern u32 mach_task_self_;
extern u32 bootstrap_port;
extern int mach_msg(void *msg, int option, u32 send_size, u32 rcv_size, u32 rcv_name, u32 timeout, u32 notify);
extern int bootstrap_check_in(u32 bp, const char *name, u32 *port);
extern long write(int fd, const void *buf, unsigned long n);

#define	MACH_SEND_MSG	1
#define	MACH_RCV_MSG	2
#define	MACH_RCV_LARGE	4

struct hdr { u32 bits, size, remote, local, voucher, id; };

static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(2, s, n); }
static void outx(u32 v) { char b[9]; int i; for (i = 7; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 15]; b[8] = 0; out(b); }

static union { struct hdr h; char raw[0x12100]; } rq, rp;

int
main(void)
{
	u32 port = 0;
	int kr;

	if ((kr = bootstrap_check_in(bootstrap_port, "com.apple.windowserver.active", &port)) != 0) {
		out("wsbridge: bootstrap_check_in failed: "); outx((u32)kr); out("\n");
		return (1);
	}
	out("wsbridge: serving com.apple.windowserver.active\n");
	for (;;) {
		const u32 *tr;

		kr = mach_msg(&rq.h, MACH_RCV_MSG | MACH_RCV_LARGE | (3u << 24), 0, sizeof(rq), port, 0, 0);
		if (kr != 0) {
			out("wsbridge: receive error "); outx((u32)kr); out("\n");
			if (kr == 0x10004002) return (1);
			continue;
		}
		tr = (const u32 *)(rq.raw + ((rq.h.size + 3) & ~3u));
		out("wsbridge: id "); outx(rq.h.id); out(" size "); outx(rq.h.size); out(" bits "); outx(rq.h.bits); out(" from pid "); outx(tr[10]); out("\n");
		if (rq.h.remote != 0) {		/* the reply port: a MIG error reply (MIG_BAD_ID), so that the caller does not wait */
			u32 *w = (u32 *)&rp.h;

			rp.h.bits = 0x12; rp.h.remote = rq.h.remote; rp.h.local = 0; rp.h.voucher = 0; rp.h.id = rq.h.id + 100;
			w[6] = 0; w[7] = 1; w[8] = (u32)-303; rp.h.size = 36;
			mach_msg(&rp.h, MACH_SEND_MSG, 36, 0, 0, 0, 0);
		}
	}
}
