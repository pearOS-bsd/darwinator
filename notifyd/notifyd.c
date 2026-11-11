/*
 * notifyd for pearOS: the daemon of notify(3) (notify_post, notify_register_check / signal / mach_port / file_descriptor, notify_get_state, notify_set_state, notify_check, notify_cancel) as
 * a Mach-O program of its own (clang and ld64.lld, linked against the libSystem of the Mac, no header of Apple's).  Own code, BSD license: written from the interface that libnotify
 * speaks (notify_ipc.defs of Libnotify, subsystem 1000: the MIG ids 1002-1032 and the layouts of their messages) and from what notify(3) documents.
 *
 * It takes the service com.apple.system.notification_center from the launchd (bootstrap_check_in), makes the shared memory page that notify_check reads (one 32 bit counter per name),
 * and answers the routines below.  Not done yet: notify_monitor_file, regenerate, the filtered (sandbox) checkin, suspend/resume, dump.
 */
typedef unsigned int u32;
typedef unsigned long u64;
typedef long i64;

extern u32 mach_task_self_;
extern u32 bootstrap_port;
extern int mach_msg(void *msg, int option, u32 send_size, u32 rcv_size, u32 rcv_name, u32 timeout, u32 notify);
extern int mach_port_allocate(u32 task, u32 right, u32 *name);
extern int mach_port_insert_right(u32 task, u32 name, u32 poly, u32 poly_poly);
extern int bootstrap_check_in(u32 bp, const char *name, u32 *port);
extern int shm_open(const char *name, int oflag, ...);
extern int ftruncate(int fd, long len);
extern void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);
extern int close(int fd);
extern int kill(int pid, int sig);
extern int fileport_makefd(u32 port);
extern long write(int fd, const void *buf, unsigned long n);
extern int getpid(void);
extern void exit(int);

#define	O_RDWR		2
#define	O_CREAT		0x200
#define	O_TRUNC		0x400
#define	PROT_RW		3
#define	MAP_SHARED	1
#define	MACH_SEND_MSG	1
#define	MACH_RCV_MSG	2
#define	MACH_RCV_LARGE	4
#define	MACH_SEND_TIMEOUT	0x10

struct hdr { u32 bits, size, remote, local, voucher, id; };

static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(2, s, n); }
static void outx(u32 v) { char b[9]; int i; for (i = 7; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 15]; b[8] = 0; out(b); }

/* the names, the registrations, the shared page */
#define	NAMES	4096
#define	REGS	16384
#define	SHM_SIZE	((NAMES + 1) * 4)
#define	SLOT(i)	((i) + 1)		/* slot 0 of the page is the pid of the server (libnotify watches it to notice a restart) */
#define	SHM_NAME	"apple.shm.notification_center"

#define	R_PLAIN	0
#define	R_CHECK	1
#define	R_SIGNAL	2
#define	R_PORT	3
#define	R_FD	4

struct name { char name[512]; u64 id; u64 state; int used; };
struct reg { int used; u32 pid, uid; int token; int name; int type; int sig; u32 port; int fd; u32 last; int suspended; };

static struct name names[NAMES];
static struct reg regs[REGS];
static volatile u32 *shm;
static u64 next_id = 1;
static int getenv_debug = 1;

static int
streq(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return (*a == *b);
}

static int
name_find(const char *n, int create)
{
	int i, free_i = -1, k;

	for (i = 0; i < NAMES; i++) {
		if (names[i].used && streq(names[i].name, n)) return (i);
		if (!names[i].used && free_i < 0) free_i = i;
	}
	if (!create || free_i < 0) return (-1);
	for (k = 0; k < 511 && n[k]; k++) names[free_i].name[k] = n[k];
	names[free_i].name[k] = 0;
	names[free_i].used = 1; names[free_i].id = next_id++; names[free_i].state = 0;
	if (shm) shm[SLOT(free_i)] = 1;
	return (free_i);
}

static int
name_by_id(u64 id)
{
	int i;

	for (i = 0; i < NAMES; i++)
		if (names[i].used && names[i].id == id) return (i);
	return (-1);
}

static struct reg *
reg_find(u32 pid, int token)
{
	int i;

	for (i = 0; i < REGS; i++)
		if (regs[i].used && regs[i].pid == pid && regs[i].token == token) return (&regs[i]);
	return (0);
}

static struct reg *
reg_new(u32 pid, u32 uid, int token, int name, int type)
{
	int i;
	struct reg *r = reg_find(pid, token);

	if (r) { r->used = 0; }
	for (i = 0; i < REGS; i++)
		if (!regs[i].used) {
			r = &regs[i];
			r->used = 1; r->pid = pid; r->uid = uid; r->token = token; r->name = name; r->type = type;
			r->sig = 0; r->port = 0; r->fd = -1; r->suspended = 0; r->last = shm ? shm[SLOT(name)] : 0;
			return (r);
		}
	return (0);
}

/* a name was posted: the counter of the page goes up and the registrations are told */
static void
post(int idx)
{
	int i;
	struct hdr h;

	if (shm) shm[SLOT(idx)]++;
	if (getenv_debug) { out("notifyd: post "); out(names[idx].name); out(" -> "); outx(shm ? shm[SLOT(idx)] : 0); out("\n"); }
	for (i = 0; i < REGS; i++) {
		struct reg *r = &regs[i];

		if (!r->used || r->name != idx || r->suspended) continue;
		switch (r->type) {
		case R_SIGNAL:
			if (kill((int)r->pid, r->sig) != 0) r->used = 0;	/* the process is gone */
			break;
		case R_PORT:
			h.bits = 19;				/* MACH_MSG_TYPE_COPY_SEND for the port */
			h.size = 24; h.remote = r->port; h.local = 0; h.voucher = 0; h.id = (u32)r->token;
			if (mach_msg(&h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, 24, 0, 0, 0, 0) != 0)
				;				/* the queue is full or the port is dead: dropped (a dead port ends the registration later) */
			break;
		case R_FD:
			if (r->fd >= 0) {
				u32 t = (u32)r->token;
				u32 be = (t >> 24) | ((t >> 8) & 0xff00) | ((t << 8) & 0xff0000) | (t << 24);

				if (write(r->fd, &be, 4) < 0) { }
			}
			break;
		}
	}
}

/* the request: the pointer to the words after the name (the name is nameCnt bytes padded to 4) */
static const u32 *
after_name(const u32 *w, u32 base_word, u32 *cnt)
{
	*cnt = w[base_word + 1];
	return (&w[base_word + 2 + ((*cnt + 3) / 4)]);
}

static union { struct hdr h; char raw[8192]; } rq, rp;

static void
send_reply(const struct hdr *q, u32 code, const u32 *outs, u32 nouts)
{
	u32 *w = (u32 *)&rp.h, i;

	if (q->remote == 0) return;
	rp.h.bits = 0x12; rp.h.remote = q->remote; rp.h.local = 0; rp.h.voucher = 0; rp.h.id = q->id + 100;
	w[6] = 0; w[7] = 1;
	w[8] = code;
	for (i = 0; i < nouts; i++) w[9 + i] = outs[i];
	rp.h.size = (9 + nouts) * 4;
	mach_msg(&rp.h, MACH_SEND_MSG, rp.h.size, 0, 0, 0, 0);
}

/* statuses of notify.h */
#define	N_OK	0
#define	N_INVALID_NAME	2
#define	N_INVALID_TOKEN	3
#define	N_INVALID_REQUEST	5
#define	N_NOT_AUTHORIZED	7

/* the ports on which a client wants its notifications (generate_common_port: one per client; register_mach_port_3: one per registration): this task keeps a send right of each */
static struct { u32 pid, port; } commons[256];

static u32
new_port_for_client(void)
{
	u32 p = 0;

	if (mach_port_allocate(mach_task_self_, 1, &p) != 0) return (0);
	if (mach_port_insert_right(mach_task_self_, p, p, 20) != 0) return (0);
	return (p);
}

/* the answer of a routine whose out parameters are a port (moved to the client) and a status: the port descriptor, the NDR record and the status, no return code */
static void
send_reply_recv(const struct hdr *q, u32 port, u32 status)
{
	u32 *w = (u32 *)&rp.h;

	if (q->remote == 0) return;
	rp.h.bits = 0x80000012u; rp.h.remote = q->remote; rp.h.local = 0; rp.h.voucher = 0; rp.h.id = q->id + 100;
	w[6] = 1; w[7] = port; w[8] = 0; w[9] = (16u << 16);		/* the descriptor: MOVE_RECEIVE */
	w[10] = 0; w[11] = 1;						/* NDR */
	w[12] = status;
	rp.h.size = 13 * 4;
	mach_msg(&rp.h, MACH_SEND_MSG, rp.h.size, 0, 0, 0, 0);
}

static void
handle(void)
{
	const u32 *w = (const u32 *)&rq.h;
	const u32 *tr = (const u32 *)(rq.raw + ((rq.h.size + 3) & ~3u));
	u32 pid = tr[10], uid = tr[6], id = rq.h.id, cnt, o[8];
	const u32 *a;
	const char *nm;
	int idx, token;
	struct reg *r;

	if (getenv_debug) { out("notifyd: request "); outx(id); out(" from pid "); outx(pid); out("\n"); }
	switch (id) {
	case 1023: case 1028:			/* checkin: version, server pid, status */
		o[0] = 3; o[1] = (u32)getpid(); o[2] = N_OK;			/* NOTIFY_IPC_VERSION of Libnotify: 3 */
		send_reply(&rq.h, 0, o, 3);
		break;
	case 1008:				/* post_2(name, claim_root) -> name_id, status */
	case 1010: case 1029:			/* post_4(name, claim_root), filtered post: no answer */
		cnt = w[9]; nm = (const char *)&w[10];
		idx = cnt > 0 && cnt <= 512 ? name_find(nm, 1) : -1;
		if (idx >= 0) post(idx);
		if (id == 1008) {
			o[0] = idx >= 0 ? (u32)names[idx].id : 0; o[1] = idx >= 0 ? (u32)(names[idx].id >> 32) : 0; o[2] = idx >= 0 ? N_OK : N_INVALID_NAME;
			send_reply(&rq.h, 0, o, 3);
		}
		break;
	case 1009:				/* post_3(name_id, claim_root) */
		idx = name_by_id(*(const u64 *)&w[8]);
		if (idx >= 0) post(idx);
		break;
	case 1011: case 1012: case 1013:		/* register_plain_2, register_check_2, register_signal_2 */
		cnt = w[9]; nm = (const char *)&w[10];
		a = after_name(w, 8, &cnt);
		token = (int)a[0];
		idx = cnt > 0 && cnt <= 512 ? name_find(nm, 1) : -1;
		if (idx < 0) { if (id == 1012) { o[0] = o[1] = o[2] = o[3] = 0; o[4] = N_INVALID_NAME; send_reply(&rq.h, 0, o, 5); } break; }
		r = reg_new(pid, uid, token, idx, id == 1012 ? R_CHECK : id == 1013 ? R_SIGNAL : R_PLAIN);
		if (r && id == 1013) r->sig = (int)a[1];
		if (id == 1012) {		/* size of the page, slot, name_id (64 bits), status */
			o[0] = SHM_SIZE; o[1] = (u32)SLOT(idx); o[2] = (u32)names[idx].id; o[3] = (u32)(names[idx].id >> 32); o[4] = r ? N_OK : N_INVALID_REQUEST;
			send_reply(&rq.h, 0, o, 5);
		}
		break;
	case 1014: case 1015:			/* register_file_descriptor_2, register_mach_port_2: a port descriptor first */
		{
			u32 portname = w[7 + 0 + 0], base = 6 + 1 + 3;		/* header 6 words, the body count, the descriptor (3 words), then NDR (2 words) */

			portname = w[7];
			cnt = w[base + 2 + 1]; nm = (const char *)&w[base + 2 + 2];
			a = after_name(w, base + 2, &cnt);
			token = (int)a[0];
			idx = cnt > 0 && cnt <= 512 ? name_find(nm, 1) : -1;
			if (idx < 0) break;
			r = reg_new(pid, uid, token, idx, id == 1014 ? R_FD : R_PORT);
			if (r) {
				if (id == 1015) r->port = portname;
				else r->fd = fileport_makefd(portname);
			}
		}
		break;
	case 1025:				/* generate_common_port -> status, port (the receive right) */
		{
			int k, slot = -1;
			u32 p;

			for (k = 0; k < 256; k++) { if (commons[k].pid == pid && commons[k].port) { slot = k; break; } if (slot < 0 && !commons[k].port) slot = k; }
			p = slot >= 0 ? new_port_for_client() : 0;
			if (!p) { o[0] = 0; send_reply(&rq.h, 0x10000000 | 5, o, 0); break; }
			commons[slot].pid = pid; commons[slot].port = p;
			send_reply_recv(&rq.h, p, N_OK);
		}
		break;
	case 1026:				/* register_common_port(name, token): the client's common port is where the notification goes */
		{
			int k;

			a = after_name(w, 8, &cnt); token = (int)a[0];
			idx = cnt > 0 && cnt <= 512 ? name_find((const char *)&w[10], 1) : -1;
			for (k = 0; k < 256 && !(commons[k].pid == pid && commons[k].port); k++) ;
			if (idx >= 0 && k < 256) { r = reg_new(pid, uid, token, idx, R_PORT); if (r) r->port = commons[k].port; }
		}
		break;
	case 1027:				/* register_mach_port_3(name, token) -> status, port (the receive right of a port of this registration) */
		{
			u32 p;

			a = after_name(w, 8, &cnt); token = (int)a[0];
			idx = cnt > 0 && cnt <= 512 ? name_find((const char *)&w[10], 1) : -1;
			p = idx >= 0 ? new_port_for_client() : 0;
			if (!p) { o[0] = 0; send_reply(&rq.h, 0x10000000 | 5, o, 0); break; }
			r = reg_new(pid, uid, token, idx, R_PORT);
			if (r) r->port = p;
			send_reply_recv(&rq.h, p, N_OK);
		}
		break;
	case 1016:				/* cancel_2(token) */
		r = reg_find(pid, (int)w[8]);
		if (r) r->used = 0;
		break;
	case 1002:				/* check(token) -> check, status */
		r = reg_find(pid, (int)w[8]);
		if (!r) { o[0] = 0; o[1] = N_INVALID_TOKEN; send_reply(&rq.h, 0, o, 2); break; }
		o[0] = shm && shm[SLOT(r->name)] != r->last; o[1] = N_OK;
		if (shm) r->last = shm[SLOT(r->name)];
		send_reply(&rq.h, 0, o, 2);
		break;
	case 1003: case 1018:			/* get_state(token) -> state, status;  get_state_3(token) -> state, nid, status */
		r = reg_find(pid, (int)w[8]);
		if (!r) { o[0] = o[1] = o[2] = o[3] = 0; o[id == 1003 ? 2 : 4] = N_INVALID_TOKEN; send_reply(&rq.h, 0, o, id == 1003 ? 3 : 5); break; }
		o[0] = (u32)names[r->name].state; o[1] = (u32)(names[r->name].state >> 32);
		if (id == 1003) { o[2] = N_OK; send_reply(&rq.h, 0, o, 3); }
		else { o[2] = (u32)names[r->name].id; o[3] = (u32)(names[r->name].id >> 32); o[4] = N_OK; send_reply(&rq.h, 0, o, 5); }
		break;
	case 1017:				/* get_state_2(name_id) -> state, status */
		idx = name_by_id(*(const u64 *)&w[8]);
		o[0] = idx >= 0 ? (u32)names[idx].state : 0; o[1] = idx >= 0 ? (u32)(names[idx].state >> 32) : 0; o[2] = idx >= 0 ? N_OK : N_INVALID_NAME;
		send_reply(&rq.h, 0, o, 3);
		break;
	case 1019: case 1031:			/* set_state_2(name_id, state, claim_root): no answer */
		idx = name_by_id(*(const u64 *)&w[8]);
		if (idx >= 0) names[idx].state = *(const u64 *)&w[10];
		break;
	case 1020: case 1032:			/* set_state_3(token, state, claim_root) -> nid, status */
		r = reg_find(pid, (int)w[8]);
		if (!r) { o[0] = o[1] = 0; o[2] = N_INVALID_TOKEN; send_reply(&rq.h, 0, o, 3); break; }
		names[r->name].state = *(const u64 *)&w[9];
		o[0] = (u32)names[r->name].id; o[1] = (u32)(names[r->name].id >> 32); o[2] = N_OK;
		send_reply(&rq.h, 0, o, 3);
		break;
	case 1004: case 1005:			/* suspend(token), resume(token) -> status */
		r = reg_find(pid, (int)w[8]);
		if (r) r->suspended = id == 1004;
		o[0] = r ? N_OK : N_INVALID_TOKEN;
		send_reply(&rq.h, 0, o, 1);
		break;
	default:
		out("notifyd: request not handled: "); outx(id); out("\n");
		if (rq.h.remote) { o[0] = 0; send_reply(&rq.h, 0x10000000 | 0x127, o, 0); }
		break;
	}
}

int
main(void)
{
	u32 port = 0;
	int kr, fd;

	fd = shm_open(SHM_NAME, O_RDWR | O_CREAT, 0644);
	if (fd >= 0) {
		ftruncate(fd, SHM_SIZE);
		shm = (volatile u32 *)mmap(0, SHM_SIZE, PROT_RW, MAP_SHARED, fd, 0);
		if ((long)shm == -1) shm = 0;
		if (shm) shm[0] = (u32)getpid();
		close(fd);
	} else
		out("notifyd: no shared memory: notify_check will not see the changes\n");
	kr = bootstrap_check_in(bootstrap_port, "com.apple.system.notification_center", &port);
	if (kr != 0) {
		out("notifyd: bootstrap_check_in failed: "); outx((u32)kr); out("\n");
		return (1);
	}
	out("notifyd: serving com.apple.system.notification_center\n");
	for (;;) {
		kr = mach_msg(&rq.h, MACH_RCV_MSG | MACH_RCV_LARGE | (3u << 24), 0, sizeof(rq), port, 0, 0);
		if (kr != 0) { out("notifyd: receive error "); outx((u32)kr); out("\n"); if (kr == 0x10004002) return (1); continue; }
		handle();
	}
}
