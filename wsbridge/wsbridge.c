/*
 * wsbridge: the front of the window server for programs of the Mac.  SkyLight (the client library in the dyld cache) sends its MIG messages to a Mach port named by the bootstrap
 * service com.apple.windowserver.active; wsd (github.com/pearOS-bsd/misc, wsbsd) speaks the same messages on a Unix socket.  This program is the service: it takes the name
 * from the launchd, receives the Mach messages and (next steps) forwards them to wsd and the replies back, turning port rights into descriptors and the other way round.
 *
 * Now: it forwards the simple messages (no ports, no memory) to wsd on its socket and the replies back; the complex ones are logged and answered with the MIG error "bad id", so that the clients fail instead of waiting.
 * is what the plan of the bridge is made from (which routines the Dock, the Finder and the SystemUIServer need first).  Own code (BSD); no header of Apple's.
 */
typedef unsigned int u32;
extern u32 mach_task_self_;
extern u32 bootstrap_port;
extern int mach_msg(void *msg, int option, u32 send_size, u32 rcv_size, u32 rcv_name, u32 timeout, u32 notify);
extern int bootstrap_check_in(u32 bp, const char *name, u32 *port);
extern long write(int fd, const void *buf, unsigned long n);
extern int socket(int domain, int type, int protocol);
extern int connect(int s, const void *name, unsigned int namelen);
extern long send(int s, const void *buf, unsigned long len, int flags);
extern long recv(int s, void *buf, unsigned long len, int flags);
extern int close(int fd);
extern char *getenv(const char *name);

#define	MACH_SEND_MSG	1
#define	MACH_RCV_MSG	2
#define	MACH_RCV_LARGE	4

struct hdr { u32 bits, size, remote, local, voucher, id; };

static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(2, s, n); }
static void outx(u32 v) { char b[9]; int i; for (i = 7; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 15]; b[8] = 0; out(b); }

static union { struct hdr h; char raw[0x12100]; } rq, rp;
static char wbuf[0x12100];

/* one socket to wsd for each client process (the window server counts a connection for each client) */
static struct { u32 pid; int fd; } clients[64];

static int
wsd_socket(u32 pid)
{
	int i, fd;
	struct { unsigned char len, family; char path[104]; } sa;
	const char *p = getenv("WS_SOCKET");
	unsigned k;

	for (i = 0; i < 64; i++)
		if (clients[i].fd != 0 && clients[i].pid == pid) return (clients[i].fd);
	if (p == 0) p = "/tmp/wsd.sock";
	for (k = 0; k < sizeof(sa); k++) ((char *)&sa)[k] = 0;
	for (k = 0; p[k] && k < 103; k++) sa.path[k] = p[k];
	sa.len = sizeof(sa); sa.family = 1;				/* AF_UNIX */
	fd = socket(1, 5, 0);						/* SOCK_SEQPACKET: one message for each datagram */
	if (fd < 0 || connect(fd, &sa, sizeof(sa)) != 0) {
		out("wsbridge: cannot connect to wsd at "); out(p); out("\n");
		if (fd >= 0) close(fd);
		return (-1);
	}
	for (i = 0; i < 64; i++)
		if (clients[i].fd == 0) { clients[i].pid = pid; clients[i].fd = fd; break; }
	return (fd);
}

/* a message of a client to wsd and the reply back (for messages that do not carry rights or memory yet) */
static void
forward(const u32 *tr)
{
	int fd = wsd_socket(tr[10]);
	long n;
	u32 *w = (u32 *)&rp.h;

	if (fd < 0)
		goto fail;
	if (rq.h.bits & 0x80000000u) {					/* complex: ports and memory are not handled yet */
		out("wsbridge:   complex message: not forwarded yet\n");
		goto fail;
	}
	if (send(fd, &rq.h, rq.h.size, 0) != (long)rq.h.size) goto fail;
	if (rq.h.remote == 0)
		return;							/* no reply port: nothing to wait for */
	n = recv(fd, wbuf, sizeof(wbuf), 0);
	if (n < 24) goto fail;
	{
		unsigned long i;

		for (i = 0; i < (unsigned long)n; i++) ((char *)&rp)[i] = wbuf[i];
	}
	out("wsbridge:   reply id "); outx(rp.h.id); out(" size "); outx(rp.h.size); out(" bits "); outx(rp.h.bits); out("\n");
	rp.h.bits = (rp.h.bits & 0x80000000u) | 0x12;			/* the reply right of the client: a send-once right, moved */
	rp.h.remote = rq.h.remote; rp.h.local = 0; rp.h.voucher = 0;
	mach_msg(&rp.h, MACH_SEND_MSG, rp.h.size, 0, 0, 0, 0);
	(void)w;
	return;
fail:
	if (rq.h.remote != 0) {						/* MIG_BAD_ID */
		rp.h.bits = 0x12; rp.h.remote = rq.h.remote; rp.h.local = 0; rp.h.voucher = 0; rp.h.id = rq.h.id + 100;
		w[6] = 0; w[7] = 1; w[8] = (u32)-303; rp.h.size = 36;
		mach_msg(&rp.h, MACH_SEND_MSG, 36, 0, 0, 0, 0);
	}
}

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
		forward(tr);
	}
}
