/*
 * launchd for pearOS: a Mach-O program (built with clang and ld64.lld, linked against the libSystem of the Mac, with no header of Apple's) that serves the bootstrap
 * port of the system, like the launchd of a Mac: it makes a port, sets it as its own bootstrap port (the kernel hands it to every task that has none) and answers the
 * XPC requests that libxpc sends there.  License: BSD (own code; the protocol was read from the messages that libxpc sends).
 *
 * The XPC message: the header of Mach (24 bytes), for a complex message the descriptors, and then "CPX@", the version 5 and a dictionary: the type 0xf000, its size, the
 * count of entries and for each entry the key (a C string padded to 4) and the value (the type word and the data).
 */
typedef unsigned int u32;
typedef unsigned long u64;
typedef long i64;

extern u32 mach_task_self_;
extern int mach_msg(void *msg, int option, u32 send_size, u32 rcv_size, u32 rcv_name, u32 timeout, u32 notify);
extern int mach_port_allocate(u32 task, u32 right, u32 *name);
extern int mach_port_insert_right(u32 task, u32 name, u32 poly, u32 poly_poly);
extern int task_set_special_port(u32 task, int which, u32 port);
extern long write(int fd, const void *buf, unsigned long n);
extern int getpid(void);
extern void exit(int);

#define	MACH_SEND_MSG	1
#define	MACH_RCV_MSG	2
#define	MACH_RCV_LARGE	4
#define	MACH_MSG_TIMEOUT_NONE	0

struct hdr { u32 bits, size, remote, local, voucher, id; };

static void
out(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	write(2, s, n);
}

static void
outx(u32 v)
{
	char b[9];
	int i;

	for (i = 7; i >= 0; i--, v >>= 4)
		b[i] = "0123456789abcdef"[v & 15];
	b[8] = 0;
	out(b);
}

static union { struct hdr h; char raw[8192]; } rq, rp;

/* the dictionary of a request: the position of the XPC data (after "CPX@"), or 0 */
static u32
xpc_start(const struct hdr *h)
{
	u32 off = 24, *w = (u32 *)h;

	if (h->bits & 0x80000000u)
		off += 4 + 12 * w[6];			/* the count of descriptors and the descriptors (ports: 12 bytes) */
	if (off + 16 <= h->size && w[off / 4] == 0x40585043)
		return (off);
	return (0);
}

/* debugging: the keys of the dictionary with the types and the small values */
static void
xpc_dump(const struct hdr *h, u32 start)
{
	const u32 *w = (const u32 *)h;
	u32 count = w[start / 4 + 4], p = start / 4 + 5, i, n, t;
	const char *k;

	for (i = 0; i < count && p * 4 < h->size; i++) {
		k = (const char *)&w[p];
		for (n = 0; k[n]; n++)
			;
		out("launchd:     "); out(k); out(" type "); outx(w[p + (n + 4) / 4]);
		p += (n + 4) / 4;
		t = w[p] >> 12;
		if (t == 3 || t == 4 || t == 2) { out(" = "); outx(w[p + 2]); out("/"); outx(w[p + 1]); }
		if (t == 9) { out(" = "); out((const char *)&w[p + 2]); }
		out("\n");
		switch (t) {
		case 0x2: p += 2; break;
		case 0x3: case 0x4: case 0x5: case 0x6: p += 3; break;
		case 0xa: p += 5; break;
		case 0x9: case 0x8: p += 2 + (w[p + 1] + 3) / 4; break;
		case 0xd: p += 1; break;
		default: return;
		}
	}
}

static int
same(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (*a == *b);
}

/* the value of the key in the dictionary (one level): the pointer to the type word, or 0 */
static const u32 *
xpc_find(const struct hdr *h, u32 start, const char *key)
{
	const u32 *w = (const u32 *)h;
	u32 count = w[start / 4 + 4], p = start / 4 + 5, i, n;
	const char *k;

	for (i = 0; i < count && p * 4 < h->size; i++) {
		k = (const char *)&w[p];
		for (n = 0; k[n]; n++)
			;
		if (same(k, key))
			return (&w[p + (n + 4) / 4]);
		p += (n + 4) / 4;				/* the key with its NUL, padded */
		switch (w[p] >> 12) {
		case 0x2: p += 2; break;			/* bool: the type word and 4 bytes */
		case 0x3: case 0x4: case 0x5: case 0x6: p += 3; break;
		case 0xa: p += 5; break;			/* uuid */
		case 0x9: case 0x8: p += 2 + (w[p + 1] + 3) / 4; break;
		case 0xd: p += 1; break;
		default: return (0);
		}
	}
	return (0);
}

/* the answer: a dictionary with no entry, or with {"error": code} */
static void
reply(const struct hdr *q, int with_error, int code)
{
	u32 *w = (u32 *)&rp.h, n = 0;

	if (q->remote == 0)
		return;
	rp.h.bits = 0x12;				/* MACH_MSG_TYPE_MOVE_SEND_ONCE for the remote port */
	rp.h.remote = q->remote;
	rp.h.local = 0;
	rp.h.voucher = 0;
	rp.h.id = q->id;
	w[6] = 0x40585043;
	w[7] = 5;
	w[8] = 0xf000;
	if (with_error) {
		w[9] = 24; w[10] = 1;
		w[11] = 0x6f727265; w[12] = 0x72;	/* "error" and the NUL, padded to 8 */
		w[13] = 0x3000;				/* int64 */
		w[14] = (u32)code; w[15] = 0;
		n = 64;
	} else {
		w[9] = 4; w[10] = 0;
		n = 44;
	}
	rp.h.size = n;
	mach_msg(&rp.h, MACH_SEND_MSG, n, 0, 0, MACH_MSG_TIMEOUT_NONE, 0);
}

/* the answer with a port: {"port": <send right>}; the right is made from the receive right of the service in this task */
static void
reply_port(const struct hdr *q, u32 svc)
{
	u32 *w = (u32 *)&rp.h;

	if (q->remote == 0)
		return;
	rp.h.bits = 0x80000012u;
	rp.h.remote = q->remote;
	rp.h.local = 0;
	rp.h.voucher = 0;
	rp.h.id = q->id;
	w[6] = 1;					/* one descriptor: the port */
	w[7] = svc; w[8] = 0;
	w[9] = (20u << 16) | 0;				/* MAKE_SEND (20), type 0 = port */
	w[10] = 0x40585043;
	w[11] = 5;
	w[12] = 0xf000;
	w[13] = 16; w[14] = 1;				/* a dictionary of 16 bytes with one entry: */
	w[15] = 0x74726f70; w[16] = 0;			/* "port" */
	w[17] = 0xd000;
	rp.h.size = 72;
	mach_msg(&rp.h, MACH_SEND_MSG, 72, 0, 0, MACH_MSG_TIMEOUT_NONE, 0);
}

static u32 svc_port;

int
main(void)
{
	u32 port = 0;
	int kr;

	if ((kr = mach_port_allocate(mach_task_self_, 1, &port)) != 0 ||
	    (kr = mach_port_insert_right(mach_task_self_, port, port, 20)) != 0 ||
	    (kr = task_set_special_port(mach_task_self_, 4, port)) != 0) {
		out("launchd: no bootstrap port, error "); outx((u32)kr); out("\n");
		return (1);
	}
	out("launchd: serving the bootstrap port\n");
	for (;;) {
		kr = mach_msg(&rq.h, MACH_RCV_MSG | MACH_RCV_LARGE, 0, sizeof(rq), port, MACH_MSG_TIMEOUT_NONE, 0);
		if (kr != 0) {
			out("launchd: receive error "); outx((u32)kr); out("\n");
			continue;
		}
		out("launchd: request "); outx(rq.h.id); out("\n");
		if (xpc_start(&rq.h) == 0)
			continue;
		xpc_dump(&rq.h, xpc_start(&rq.h));
		{
			const u32 *v = xpc_find(&rq.h, xpc_start(&rq.h), "name");

			if (v != 0 && (*v >> 12) == 9) {
				out("launchd:   name "); out((const char *)(v + 2)); out("\n");
			}
		}
		{
			const u32 *v = xpc_find(&rq.h, xpc_start(&rq.h), "name");

			if (v != 0 && (rq.h.id & 0xfff) >= 0x320 && same((const char *)(v + 2), "com.apple.cfprefsd.daemon")) {
				if (svc_port == 0)
					mach_port_allocate(mach_task_self_, 1, &svc_port);
				reply_port(&rq.h, svc_port);
				continue;
			}
		}
		reply(&rq.h, (rq.h.id & 0xfff) >= 0x320, 3);
	}
}
