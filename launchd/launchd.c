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
extern int posix_spawn(int *pid, const char *path, const void *fa, const void *attr, char *const argv[], char *const envp[]);
extern int mach_port_move_member(u32 task, u32 member, u32 after);
extern int mkdir(const char *path, unsigned short mode);
extern int chown(const char *path, unsigned int uid, unsigned int gid);
extern int chmod(const char *path, unsigned short mode);
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

/*
 * The answers.  Every answer is a dictionary; the ones for a requester that the launchd must vouch for carry "req_pid" and "rec_execcnt" (the pid and the exec count of the
 * requester, from the audit token of the request): libxpc checks them when the answer comes from a user 0 (_xpc_domain_routine_authenticate).
 */
static u32 *xw;			/* the message that is being built */
static u32 xn;			/* its size in words */
static u32 xcount, xsizeat, xcountat;

static void
xbegin(const struct hdr *q, int ndesc)
{
	xw = (u32 *)&rp.h;
	rp.h.bits = ndesc ? 0x80000012u : 0x12;		/* MACH_MSG_TYPE_MOVE_SEND_ONCE for the remote port; complex if there are descriptors */
	rp.h.remote = q->remote;
	rp.h.local = 0;
	rp.h.voucher = 0;
	rp.h.id = 0x20000000;				/* the id of an XPC reply: libxpc checks it */
	xn = 6;
	if (ndesc) { xw[xn++] = ndesc; }
}

static void
xdict(void)
{
	xw[xn++] = 0x40585043; xw[xn++] = 5; xw[xn++] = 0xf000;
	xsizeat = xn; xw[xn++] = 0;
	xcountat = xn; xw[xn++] = 0;
	xcount = 0;
}

static void
xkey(const char *k)
{
	u32 n = 0;

	while (k[n]) n++;
	n++;						/* the NUL */
	{
		char *d = (char *)&xw[xn];
		u32 i;

		for (i = 0; i < ((n + 3) & ~3u); i++) d[i] = i < n ? k[i] : 0;
	}
	xn += (n + 3) / 4;
	xcount++;
}

static void xu64(const char *k, u64 v) { xkey(k); xw[xn++] = 0x4000; xw[xn++] = (u32)v; xw[xn++] = (u32)(v >> 32); }
static void xi64(const char *k, i64 v) { xkey(k); xw[xn++] = 0x3000; xw[xn++] = (u32)v; xw[xn++] = (u32)((u64)v >> 32); }
static void
xstr(const char *k, const char *v)
{
	u32 n = 0, i;

	while (v[n]) n++;
	n++;
	xkey(k);
	xw[xn++] = 0x9000; xw[xn++] = n;
	for (i = 0; i < ((n + 3) & ~3u); i++) ((char *)&xw[xn])[i] = i < n ? v[i] : 0;
	xn += (n + 3) / 4;
}

static void xport(const char *k, u32 type) { xkey(k); xw[xn++] = type; }

static void
xsend(void)
{
	xw[xsizeat] = (xn - xsizeat - 1) * 4;
	xw[xcountat] = xcount;
	rp.h.size = xn * 4;
	mach_msg(&rp.h, MACH_SEND_MSG, xn * 4, 0, 0, MACH_MSG_TIMEOUT_NONE, 0);
}

/* the audit token of the request, in the trailer */
static const u32 *
trailer_of(const struct hdr *h)
{
	return ((const u32 *)((const char *)h + ((h->size + 3) & ~3u)));
}

static void
vouch(const struct hdr *q)
{
	const u32 *tr = trailer_of(q);

	xu64("req_pid", tr[10]);
	xu64("rec_execcnt", tr[12]);
}

/* the answer: a dictionary with no entry, or with {"error": code} */
static void
reply(const struct hdr *q, int with_error, int code)
{
	if (q->remote == 0)
		return;
	xbegin(q, 0);
	xdict();
	if (with_error)
		xi64("error", code);
	vouch(q);
	xsend();
}

/* the answer with a port: {"port": <right>}.  disp: 20 makes a send right from the receive right that this task holds, 19 copies a send right that it holds, 16 moves the receive right to
 * the requester (the check-in of a service; the type word of a receive right in the XPC data is 0x15000, of a send right 0xd000) */
static void
reply_port(const struct hdr *q, u32 svc, u32 disp)
{
	if (q->remote == 0)
		return;
	xbegin(q, 1);
	xw[xn++] = svc; xw[xn++] = 0; xw[xn++] = (disp << 16) | 0;	/* the descriptor: the right, type 0 = port */
	xdict();
	xport("port", disp == 16 ? 0x15000 : 0xd000);
	vouch(q);
	xsend();
}

/* the services: the ones that this task serves (the dirhelper) and the ones that a program checked in (this task keeps a send right for the look-ups) */
/* the domain (the name of the manager: "System", "Aqua" for a user session) of the jobs that this launchd started, by pid; a program that was not started here is in "System" */
static struct { int pid; char domain[16]; } jobs[32];

static const char *
domain_of(u32 pid)
{
	int i;

	for (i = 0; i < 32; i++)
		if (jobs[i].pid == (int)pid) return (jobs[i].domain);
	return ("System");
}

struct svc { int used, own; char name[96]; u32 port; };
static struct svc svcs[64];

static struct svc *
svc_find(const char *n)
{
	int i;

	for (i = 0; i < 64; i++)
		if (svcs[i].used && same(n, svcs[i].name)) return (&svcs[i]);
	return (0);
}

static struct svc *
svc_add(const char *n, u32 port, int own)
{
	int i, k;

	for (i = 0; i < 64; i++)
		if (!svcs[i].used) {
			for (k = 0; k < 95 && n[k]; k++) svcs[i].name[k] = n[k];
			svcs[i].name[k] = 0; svcs[i].used = 1; svcs[i].port = port; svcs[i].own = own;
			return (&svcs[i]);
		}
	return (0);
}

/*
 * The service com.apple.bsd.dirhelper: the per-user directories of libsystem_coreservices (confstr(_CS_DARWIN_USER_DIR, _TEMP_DIR, _CACHE_DIR)) live in
 * /var/folders/<2 characters>/<30 characters>/{0,T,C}.  The name is the UUID of the user (16 bytes: the compatibility prefix FFFFEEEE-DDDD-CCCC-BBBB-AAAA and the uid, as
 * membership gives it without a directory service) and the uid again (4 bytes, big endian), 20 bytes in all, written in base 32 with the alphabet below (5 bits at a
 * time, the most significant first).  Found by reading libsystem_coreservices and checked against the names that it asks for (root: zz/zyxvpxvq6csfxvn_n0000000000000).
 * The client asks the service to make the directories when they are missing: the message 0xb86d (no body); the answer is a MIG reply, id 0xb8d1, with the return code.
 */
static const char b32[] = "0123456789_bcdfghjklmnpqrstvwxyz";

static void
user_dir_name(u32 uid, char out[33])
{
	unsigned char b[20] = { 0xff, 0xff, 0xee, 0xee, 0xdd, 0xdd, 0xcc, 0xcc, 0xbb, 0xbb, 0xaa, 0xaa };
	int i, bit;

	for (i = 0; i < 4; i++)
		b[12 + i] = b[16 + i] = (unsigned char)(uid >> (24 - 8 * i));
	for (i = 0; i < 32; i++) {
		u32 v = 0;

		for (bit = 0; bit < 5; bit++) {
			int p = i * 5 + bit;

			v = (v << 1) | ((b[p / 8] >> (7 - p % 8)) & 1);
		}
		out[i] = b32[v];
	}
	out[32] = 0;
}

static void
make_user_dirs(u32 uid, u32 gid)
{
	char n[33], top[24], dir[64], sub[72];
	int i;
	const char *pre = "/var/folders/", *names[] = { "/0", "/T", "/C" };

	user_dir_name(uid, n);
	for (i = 0; pre[i]; i++) top[i] = pre[i];
	top[i++] = n[0]; top[i++] = n[1]; top[i] = 0;
	mkdir("/var/folders", 0755);
	mkdir(top, 0755);
	for (i = 0; top[i]; i++) dir[i] = top[i];
	dir[i++] = '/';
	{
		int k;

		for (k = 2; k < 32; k++) dir[i++] = n[k];
		dir[i] = 0;
	}
	mkdir(dir, 0700);
	chown(dir, uid, gid);
	chmod(dir, 0700);
	for (i = 0; i < 3; i++) {
		int j = 0, k;

		for (k = 0; dir[k]; k++) sub[j++] = dir[k];
		for (k = 0; names[i][k]; k++) sub[j++] = names[i][k];
		sub[j] = 0;
		mkdir(sub, 0700);
		chown(sub, uid, gid);
		chmod(sub, 0700);
	}
}

/* "/var/folders/XX/<30>/" + the subdirectory of which (0: "0", 1: "T", 2: "C") + "/" + the suffix + "/" -> out; the directories are made (mode 0700, owned by the user) when make is set */
static int
suffix_dir(u32 uid, u32 gid, u32 which, const char *suffix, u32 slen, int make, char *out, u32 outsz)
{
	char n[33];
	const char *sub = which == 1 ? "T" : which == 2 ? "C" : "0";
	u32 i, p = 0;

	if (which > 2 || slen == 0 || slen > 200)
		return (22);
	for (i = 0; i < slen; i++) {			/* the suffix is a relative path of plain components */
		char c = suffix[i];

		if (c == 0 && i == slen - 1) break;
		if (c == 0 || c == '\\' || c < 32) return (22);
		if (c == '/' && (i == 0 || suffix[i + 1] == '/')) return (22);
		if (c == '.' && (i == 0 || suffix[i - 1] == '/') && (suffix[i + 1] == '.' || suffix[i + 1] == '/' || suffix[i + 1] == 0)) return (22);
	}
	user_dir_name(uid, n);
	{
		const char *pre = "/var/folders/";

		for (i = 0; pre[i]; i++) out[p++] = pre[i];
	}
	out[p++] = n[0]; out[p++] = n[1]; out[p++] = '/';
	for (i = 2; i < 32; i++) out[p++] = n[i];
	out[p++] = '/'; out[p++] = sub[0]; out[p++] = '/';
	if (make)
		make_user_dirs(uid, gid);
	for (i = 0; i < slen && suffix[i]; i++) {
		if (p + 3 >= outsz) return (63);
		out[p++] = suffix[i];
		if (make && suffix[i + 1] == '/') {
			out[p] = 0; mkdir(out, 0700); chown(out, uid, gid); chmod(out, 0700);
		}
	}
	if (make) { out[p] = 0; mkdir(out, 0700); chown(out, uid, gid); chmod(out, 0700); }
	if (out[p - 1] != '/') out[p++] = '/';
	out[p] = 0;
	return (0);
}

/* the answer of a MIG routine: the return code only */
static void
mig_reply_code(const struct hdr *q, u32 code)
{
	u32 *w = (u32 *)&rp.h;

	if (q->remote == 0)
		return;
	rp.h.bits = 0x12;
	rp.h.remote = q->remote;
	rp.h.local = 0;
	rp.h.voucher = 0;
	rp.h.id = q->id + 100;
	rp.h.size = 36;
	w[6] = 0; w[7] = 1;				/* NDR record */
	w[8] = code;
	mach_msg(&rp.h, MACH_SEND_MSG, 36, 0, 0, MACH_MSG_TIMEOUT_NONE, 0);
}

static u32 svc_port;
static u32 dir_port;
static int debug_dump = 1;

static u32
tr_uid(const struct hdr *h)
{
	return (trailer_of(h)[6]);
}

int
main(int argc, char **argv, char **envp)
{
	u32 port = 0, set = 0;
	int kr;

	if ((kr = mach_port_allocate(mach_task_self_, 1, &port)) != 0 ||
	    (kr = mach_port_insert_right(mach_task_self_, port, port, 20)) != 0 ||
	    (kr = mach_port_allocate(mach_task_self_, 1, &dir_port)) != 0 ||
	    (kr = mach_port_allocate(mach_task_self_, 3, &set)) != 0 ||
	    (kr = mach_port_move_member(mach_task_self_, port, set)) != 0 ||
	    (kr = mach_port_move_member(mach_task_self_, dir_port, set)) != 0 ||
	    (kr = task_set_special_port(mach_task_self_, 4, port)) != 0) {
		out("launchd: no bootstrap port, error "); outx((u32)kr); out("\n");
		return (1);
	}
	svc_add("com.apple.bsd.dirhelper", dir_port, 1);
	out("launchd: serving the bootstrap port\n");
	{
		/* the services of the system that are started with the launchd (on a Mac the plists of /System/Library/LaunchDaemons say which): the programs on the command line, or notifyd */
		static const char *dflt[] = { "/usr/sbin/notifyd", 0 };
		const char **progs = dflt;
		int i, n = argc > 1 ? argc - 1 : 1;
		static const char *given[16];

		if (argc > 1) {
			for (i = 0; i < argc - 1 && i < 15; i++) given[i] = argv[i + 1];
			given[i] = 0; progs = given;
		}
		for (i = 0; i < n && progs[i]; i++) {
			int pid = 0, na = 0, k;
			char buf[256], *av[8];

			for (k = 0; progs[i][k] && k < 255; k++) buf[k] = progs[i][k];
			buf[k] = 0;
			for (k = 0; buf[k] && na < 7; ) {		/* "path arg arg": the words are separated by spaces */
				av[na++] = &buf[k];
				while (buf[k] && buf[k] != ' ') k++;
				if (buf[k] == ' ') buf[k++] = 0;
			}
			av[na] = 0;
			if (posix_spawn(&pid, av[0], 0, 0, av, envp) == 0) {
				int j;

				for (j = 0; j < 32 && jobs[j].pid; j++) ;
				if (j < 32) {
					const char *dm = na > 1 && same(av[na - 1], "agent") ? "Aqua" : "System";
					int q;

					jobs[j].pid = pid;
					for (q = 0; dm[q]; q++) jobs[j].domain[q] = dm[q];
					jobs[j].domain[q] = 0;
				}
				out("launchd: started "); out(progs[i]); out("\n");
			}
			else { out("launchd: cannot start "); out(progs[i]); out("\n"); }
		}
	}
	for (;;) {
		const u32 *tr;

		kr = mach_msg(&rq.h, MACH_RCV_MSG | MACH_RCV_LARGE | (3u << 24), 0, sizeof(rq), set, MACH_MSG_TIMEOUT_NONE, 0);
		if (kr != 0) {
			out("launchd: receive error "); outx((u32)kr); out("\n");
			continue;
		}
		tr = (const u32 *)(rq.raw + ((rq.h.size + 3) & ~3u));	/* the trailer: type, size, seqno, security token (2 words), audit token (auid euid egid ruid rgid pid ...) */
		if (rq.h.local == dir_port) {			/* the dirhelper: the directories of the sender */
			out("launchd: dirhelper request "); outx(rq.h.id); out(" uid "); outx(tr[6]); out("\n");
			if (rq.h.id != 0xb86d) {
				u32 i;

				out("launchd:   size "); outx(rq.h.size); out(":");
				for (i = 0; i < rq.h.size / 4 && i < 40; i++) { out(" "); outx(((u32 *)&rq.h)[i]); }
				out("\n");
			}
			if (rq.h.id == 0xb86d) {
				make_user_dirs(tr[6], tr[7]);
				mig_reply_code(&rq.h, 0);
			} else if (rq.h.id == 0xb872 && rq.h.size >= 48) {		/* (which, flags, suffix: a string, size of the answer): the directory of the process in the user directory, made if need be; the answer has its path */
				const u32 *w = (const u32 *)&rq.h;
				u32 which = w[8], slen = w[11], plen, i;
				static char path[1100];
				int rc = rq.h.size >= 48 + ((slen + 3) & ~3u) - 4 ? suffix_dir(tr[6], tr[7], which, (const char *)&w[12], slen, (w[9] & 0xff) != 0, path, 1024) : 22;

				if (rc != 0) { mig_reply_code(&rq.h, 0x10000000 | rc); continue; }
				for (plen = 0; path[plen]; plen++)
					;
				plen++;
				{
					u32 *x = (u32 *)&rp.h;
					u32 pad = (plen + 3) & ~3u;

					rp.h.bits = 0x12; rp.h.remote = rq.h.remote; rp.h.local = 0; rp.h.voucher = 0; rp.h.id = rq.h.id + 100;
					x[6] = 0; x[7] = 1;
					x[8] = 0;		/* RetCode */
					x[9] = 0;		/* the offset of the string */
					x[10] = plen;		/* its length with the NUL */
					for (i = 0; i < pad; i++) ((char *)&x[11])[i] = i < plen ? path[i] : 0;
					x[11 + pad / 4] = 0;	/* the out int that follows */
					rp.h.size = (11 + pad / 4 + 1) * 4;
					mach_msg(&rp.h, MACH_SEND_MSG, rp.h.size, 0, 0, MACH_MSG_TIMEOUT_NONE, 0);
				}
			} else
				mig_reply_code(&rq.h, 0x47);	/* MIG_BAD_ID-like */
			continue;
		}
		out("launchd: request "); outx(rq.h.id); out(" from pid "); outx(trailer_of(&rq.h)[10]); out("\n");
		if (xpc_start(&rq.h) == 0)
			continue;
		if (debug_dump) xpc_dump(&rq.h, xpc_start(&rq.h));
		if ((rq.h.id & 0xfff) == 0x12d) {		/* vproc_swap_*: a property of the job or of the manager (VPROC_GSK_*) */
			const u32 *g = xpc_find(&rq.h, xpc_start(&rq.h), "outgsk");

			if (g && (*g >> 12) == 4 && g[1] == 6 && g[2] == 0 /* VPROC_GSK_MGR_NAME */) {
				xbegin(&rq.h, 0); xdict(); xstr("out", domain_of(trailer_of(&rq.h)[10])); vouch(&rq.h); xsend();
			} else
				reply(&rq.h, 1, 45);	/* not supported */
			continue;
		}
		{
			const u32 *v = xpc_find(&rq.h, xpc_start(&rq.h), "name");

			if (v != 0 && (*v >> 12) == 9) {
				out("launchd:   name "); out((const char *)(v + 2)); out("\n");
				{
					u32 rt = rq.h.id & 0xfff;
					struct svc *sv = svc_find((const char *)(v + 2));

					if (rt == 0xce || rt == 0x325) {		/* check-in (bootstrap_check_in: 0xce; the listener of an XPC connection: 0x325): the service gets the receive right of its port */
						if (sv == 0 && tr_uid(&rq.h) == 0) {
							u32 p = 0;

							if (mach_port_allocate(mach_task_self_, 1, &p) == 0 && mach_port_insert_right(mach_task_self_, p, p, 20) == 0)
								sv = svc_add((const char *)(v + 2), p, 0);
							if (sv) { reply_port(&rq.h, sv->port, 16); continue; }
						}
						reply(&rq.h, 1, 1);		/* known already, or not allowed: BOOTSTRAP_NOT_PRIVILEGED-like */
						continue;
					}
					if ((rt >= 0x320 || rt == 0xcf) && sv) {	/* look-up */
						reply_port(&rq.h, sv->port, sv->own ? 20 : 19);
						continue;
					}
				}
			}
		}
		reply(&rq.h, (rq.h.id & 0xfff) >= 0x320, 3);
	}
}
