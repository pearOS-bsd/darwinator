/*
 * The workqueue of libpthread: the kernel side that libdispatch's global queues stand on (workq_open, workq_kernreturn).  Modelled on the pthread workqueue of XNU (bsd/pthread/pthread_workqueue.c
 * and libpthread/kern/kern_support.c, Apple open source), much smaller: a process asks for threads (WQOPS_QUEUE_REQTHREADS) and the kernel makes them, each with its own stack and pthread
 * structure, and starts them at the entry that libpthread registered (start_wqthread) with (pthread, kport, stack bottom, kevent list, flags, count of kevents).  A thread that has no more
 * work calls workq_kernreturn again (WQOPS_THREAD_RETURN and the kevent variants): it parks in the kernel, and is restarted at the same entry when there is more to do: threads asked for, or
 * events on the kqueue of the workqueue (kevent_qos with fd -1: libdispatch's sources, Mach ports among them), which are handed out in the kevent list on the stack of the thread.  An idle
 * thread ends after a few seconds.  No workloops, no cooperative pool, no QoS accounting: one pool, the threads asked for and one poller for the events.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/signalvar.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/thr.h>
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <machine/frame.h>
#include <machine/pcb.h>
#include <machine/md_var.h>
#include <machine/segments.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"
extern int darwin_wl_stale_check;
struct wqdl { int tid; int64_t id; uint64_t qs, em, ev, ea; int ticks; };
static struct wqdl wq_dl[32];
static unsigned wq_dl_n;
void darwin_wq_dump_deliveries(void);
void
darwin_wq_dump_deliveries(void)
{
	int i;

	for (i = 0; i < 32; i++) {
		struct wqdl *r = &wq_dl[(wq_dl_n + i) % 32];

		if (r->tid != 0)
			printf("darwinator:   delivery tid %d wl %#lx state %#lx mask %#lx want %#lx addr %#lx ticks %d\n", r->tid, (u_long)r->id, (u_long)r->qs, (u_long)r->em, (u_long)r->ev, (u_long)r->ea, r->ticks);
	}
}

#define	WQOPS_THREAD_RETURN		0x004
#define	WQOPS_QUEUE_NEWSPISUPP		0x010
#define	WQOPS_QUEUE_REQTHREADS		0x020
#define	WQOPS_QUEUE_REQTHREADS2		0x030
#define	WQOPS_THREAD_KEVENT_RETURN	0x040
#define	WQOPS_SET_EVENT_MANAGER_PRIORITY	0x080
#define	WQOPS_THREAD_WORKLOOP_RETURN	0x100
#define	WQOPS_SHOULD_NARROW		0x200
#define	WQOPS_SETUP_DISPATCH		0x400

#define	WQ_FLAG_THREAD_PRIO_QOS		0x00004000
#define	WQ_FLAG_THREAD_OVERCOMMIT	0x00010000
#define	WQ_FLAG_THREAD_REUSE		0x00020000
#define	WQ_FLAG_THREAD_NEWSPI		0x00040000
#define	WQ_FLAG_THREAD_KEVENT		0x00080000
#define	WQ_FLAG_THREAD_EVENT_MANAGER	0x00100000
#define	PTHREAD_PRIORITY_EVENT_MANAGER	0x02000000
#define	WQ_FLAG_THREAD_WORKLOOP		0x00400000
#define	WQ_FLAG_THREAD_TSD_BASE_SET	0x00200000

#define	WQ_STACK_SIZE		(512 * 1024)		/* PTH_DEFAULT_STACKSIZE */
#define	WQ_KEVENT_LIST_LEN	16
#define	WQ_KEVENT_SIZE		72			/* sizeof(struct kevent_qos_s) */
#define	WQ_KEVENT_DATA_SIZE	(32 * 1024)
#define	WQ_MAX_THREADS		32
#define	WQ_IDLE_LIFE		(5 * hz)
#define	WQ_NREQ			128			/* workloops with a thread request, and workloops that a thread looks at (launchservicesd has more than 16: its listener did not get a thread and never answered) */
#define	WQ_POLL			MAX(1, hz / 500)	/* 2 ms: how often a parked thread looks at its kqueue */
#define	TSD_OFFSET		0xe0			/* the TSD block in the pthread structure */

struct wqthr {
	struct thread	*td;
	lwpid_t		tid;
	uint64_t	stackaddr, self;
	int		used;
	int		prio;			/* the QoS (thread QoS tier) of the work that it was asked for */
	int64_t		bound;			/* the workloop that this thread is working for (0: none) */
	int		parked;			/* in wq_park, looking for work */
	int		ismgr;			/* this thread is the event manager of libdispatch now (there is one at a time: it takes a lock that a second one cannot) */
};

struct wqp {
	struct proc	*p;
	pid_t		pid;
	uint64_t	wqstart, pthsize;
	int		want;			/* threads asked for and not started yet */
	int		wantqos;
	int		mgr;			/* an event manager thread is out */
	int		want_oc;		/* the same for the requests with the overcommit flag (libdispatch has separate root queues for them: the thread must say that it is for them) */
	int		wantqos_oc;
	int		nthreads;
	struct { int64_t id; lwpid_t tid; } wl[WQ_NREQ];		/* the workloops that a thread works for now: one thread at a time for each */
	struct { int64_t id; uint64_t udata; uint32_t qos; uint64_t eaddr, emask, evalue; } req[WQ_NREQ];	/* thread requests of workloops (libdispatch wakes a serial queue with one) */
	struct wqthr	thr[WQ_MAX_THREADS];
};

static struct wqp wqs[16];
static struct mtx wq_mtx;
MTX_SYSINIT(darwin_wq, &wq_mtx, "darwin workqueue", MTX_DEF);

static MALLOC_DEFINE(M_DARWIN_WQ, "darwin_wq", "darwinator workqueue");

static struct wqp *
wq_get(struct proc *p, bool create)
{
	int i, freei = -1;

	for (i = 0; i < (int)nitems(wqs); i++) {
		if (wqs[i].p == p && wqs[i].pid == p->p_pid)
			return (&wqs[i]);
		if (wqs[i].p == NULL && freei < 0)
			freei = i;
	}
	if (!create || freei < 0)
		return (NULL);
	memset(&wqs[freei], 0, sizeof(wqs[freei]));
	wqs[freei].p = p; wqs[freei].pid = p->p_pid;
	return (&wqs[freei]);
}

/* debugging: the thread requests that wait and the workloops that threads work for (sysctl kern.darwinator.wl_dump), with the state word of each queue read now */
void
darwin_wq_dump_all(void)
{
	struct { struct proc *p; pid_t pid; int64_t id; uint64_t ea, em, ev; lwpid_t tid; } snap[64];
	int n = 0, i, k;

	mtx_lock(&wq_mtx);
	for (i = 0; i < (int)nitems(wqs) && n < 60; i++) {
		if (wqs[i].p == NULL) continue;
		for (k = 0; k < WQ_NREQ && n < 62; k++)
			if (wqs[i].req[k].id != 0) { snap[n].p = wqs[i].p; snap[n].pid = wqs[i].pid; snap[n].id = wqs[i].req[k].id; snap[n].ea = wqs[i].req[k].eaddr; snap[n].em = wqs[i].req[k].emask; snap[n].ev = wqs[i].req[k].evalue; snap[n].tid = 0; n++; }
		for (k = 0; k < WQ_NREQ && n < 62; k++)
			if (wqs[i].wl[k].id != 0) { snap[n].p = wqs[i].p; snap[n].pid = wqs[i].pid; snap[n].id = wqs[i].wl[k].id; snap[n].ea = 0; snap[n].em = 0; snap[n].ev = 0; snap[n].tid = wqs[i].wl[k].tid; n++; }
	}
	mtx_unlock(&wq_mtx);
	for (i = 0; i < n; i++) {
		uint64_t qs = 0;
		struct proc *p = pfind(snap[i].pid);

		if (p != NULL && p == snap[i].p && snap[i].ea != 0) {
			_PHOLD(p); PROC_UNLOCK(p);
			(void)proc_readmem(curthread, p, (vm_offset_t)snap[i].ea, &qs, 8);
			PRELE(p);
		} else if (p != NULL)
			PROC_UNLOCK(p);
		if (snap[i].tid != 0)
			printf("darwinator: wl bound   pid %d wl %#lx served by tid %d\n", snap[i].pid, (u_long)snap[i].id, snap[i].tid);
		else
			printf("darwinator: wl request pid %d wl %#lx state %#lx mask %#lx want %#lx\n", snap[i].pid, (u_long)snap[i].id, (u_long)qs, (u_long)snap[i].em, (u_long)snap[i].ev);
	}
}

void
darwin_wq_exit(struct proc *p)
{
	mtx_lock(&wq_mtx);
	{
		struct wqp *w = wq_get(p, false);

		if (w != NULL)
			memset(w, 0, sizeof(*w));
	}
	mtx_unlock(&wq_mtx);
}

/* the thread QoS tier of a pthread_priority_t: the QoS is a one-hot field in bits 8..21 of the priority (maintenance 1, background 2, utility 3, legacy 4, default 5, user initiated 6, user interactive 7); the field is empty for "unspecified" */
static int
qos_of_priority(uint32_t pri)
{
	uint32_t v = (pri & 0x003fff00) >> 8;

	return (v != 0 ? ffs((int)v) : 5);
}

/* the registers of a thread of the pool: where it starts (libpthread's start_wqthread) and what it gets */
static void
wq_set_regs(struct thread *td, struct wqthr *t, uint64_t wqstart, uint64_t kevent_list, uint32_t flags, uint32_t nkevents, uint64_t stack_top)
{
	td->td_frame->tf_rdi = t->self;
	td->td_frame->tf_rsi = ((uint32_t)t->tid << 8) | 0x03;
	td->td_frame->tf_rdx = t->stackaddr + PAGE_SIZE;
	td->td_frame->tf_rcx = kevent_list;
	td->td_frame->tf_r8 = flags;
	td->td_frame->tf_r9 = nkevents;
	td->td_frame->tf_rip = wqstart;
	td->td_frame->tf_rsp = stack_top;
	td->td_pcb->pcb_gsbase = t->self + TSD_OFFSET;
	set_pcb_flags(td->td_pcb, PCB_FULL_IRET);
	td->td_frame->tf_gs = _ugssel;
}

struct wq_start {
	struct wqthr	*t;
	uint64_t	wqstart;
	int		qos;
	bool		mgr;			/* the event manager thread of libdispatch: it comes out with the KEVENT and EVENT_MANAGER flags and an empty list */
};

static int
wq_initthr(struct thread *td, void *thunk)
{
	struct wq_start *ws = thunk;
	int error;
	stack_t st;

	st.ss_sp = (void *)(uintptr_t)(ws->t->self - 16); st.ss_size = 16;
	error = cpu_set_upcall(td, (void (*)(void *))(uintptr_t)ws->wqstart, NULL, &st);
	if (error != 0)
		return (error);
	ws->t->td = td; ws->t->tid = td->td_tid;
	if (ws->mgr)
		wq_set_regs(td, ws->t, ws->wqstart, ws->t->self - (uint64_t)WQ_KEVENT_LIST_LEN * WQ_KEVENT_SIZE, WQ_FLAG_THREAD_NEWSPI | WQ_FLAG_THREAD_PRIO_QOS | WQ_FLAG_THREAD_TSD_BASE_SET | WQ_FLAG_THREAD_KEVENT | WQ_FLAG_THREAD_EVENT_MANAGER | (uint32_t)ws->qos, 0,
		    (ws->t->self - (uint64_t)WQ_KEVENT_LIST_LEN * WQ_KEVENT_SIZE - 16 - WQ_KEVENT_DATA_SIZE) & ~0xfULL);
	else
		wq_set_regs(td, ws->t, ws->wqstart, 0, WQ_FLAG_THREAD_NEWSPI | WQ_FLAG_THREAD_PRIO_QOS | WQ_FLAG_THREAD_TSD_BASE_SET | ((ws->qos & 0x100) ? WQ_FLAG_THREAD_OVERCOMMIT : 0) | (uint32_t)(ws->qos & 0xf), 0, (ws->t->self - 64) & ~0xfULL);
	return (0);
}

/* makes a thread of the pool (the lock is held on entry and on return) */
static int
wq_spawn(struct thread *td, struct wqp *w, int qos, bool mgr)
{
	struct wq_start ws;
	struct mmap_args ma;
	struct wqthr *t = NULL;
	uint64_t size, base;
	int i, error;

	for (i = 0; i < WQ_MAX_THREADS; i++)
		if (!w->thr[i].used) { t = &w->thr[i]; break; }
	if (t == NULL)
		return (EAGAIN);
	t->used = 1;
	t->ismgr = mgr;
	if (mgr) w->mgr = 1;
	w->nthreads++;
	mtx_unlock(&wq_mtx);
	size = round_page(PAGE_SIZE + WQ_STACK_SIZE + w->pthsize);
	ma = (struct mmap_args){ .addr = NULL, .len = size, .prot = PROT_READ | PROT_WRITE, .flags = MAP_ANON | MAP_PRIVATE, .fd = -1, .pos = 0 };
	error = sys_mmap(td, &ma);
	if (error == 0) {
		base = td->td_retval[0];
		t->stackaddr = base;
		t->self = base + PAGE_SIZE + WQ_STACK_SIZE;
		ws.t = t; ws.wqstart = w->wqstart; ws.qos = qos; ws.mgr = mgr;
		error = thread_create(td, NULL, wq_initthr, &ws);
		if (error != 0) {
			struct munmap_args ua = { .addr = (void *)base, .len = size };

			(void)sys_munmap(td, &ua);
		}
	}
	mtx_lock(&wq_mtx);
	if (error != 0) {
		t->used = 0;
		w->nthreads--;
	}
	return (error);
}

static struct wqthr *
wq_self(struct wqp *w, struct thread *td)
{
	int i;

	for (i = 0; i < WQ_MAX_THREADS; i++)
		if (w->thr[i].used && w->thr[i].tid == td->td_tid)
			return (&w->thr[i]);
	return (NULL);
}

static bool
wl_busy(struct wqp *w, int64_t id)
{
	int i;

	for (i = 0; i < WQ_NREQ; i++)
		if (w->wl[i].id == id && w->wl[i].tid != 0)
			return (true);
	return (false);
}

static bool
wl_claim(struct wqp *w, int64_t id, lwpid_t tid)
{
	int i, freei = -1;

	for (i = 0; i < WQ_NREQ; i++) {
		if (w->wl[i].id == id) { if (w->wl[i].tid != 0) return (false); w->wl[i].tid = tid; return (true); }
		if (w->wl[i].id == 0 && freei < 0) freei = i;
	}
	if (freei < 0)
		return (false);
	w->wl[freei].id = id; w->wl[freei].tid = tid;
	return (true);
}

static void
wl_release(struct wqp *w, int64_t id)
{
	int i;

	for (i = 0; i < WQ_NREQ; i++)
		if (w->wl[i].id == id)
			w->wl[i].tid = 0;
}

/* the thread of the pool that comes back: it parks until there is work, and then starts again at the entry (the lock is not held) */
static int
wq_park(struct thread *td, struct wqp *w, struct wqthr *t, uint64_t item, int nevents, bool kevent_return)
{
	uint64_t kevent_list, data_buf, davail, top;
	int64_t ids[WQ_NREQ], first_kq;
	int count, error, n, k, quiet = 0;
	uint64_t wqstart;
	const uint32_t base_flags = WQ_FLAG_THREAD_NEWSPI | WQ_FLAG_THREAD_PRIO_QOS | WQ_FLAG_THREAD_REUSE | WQ_FLAG_THREAD_TSD_BASE_SET | WQ_FLAG_THREAD_KEVENT | 5;

	kevent_list = t->self - (uint64_t)WQ_KEVENT_LIST_LEN * WQ_KEVENT_SIZE;
	data_buf = kevent_list - 16 - WQ_KEVENT_DATA_SIZE;		/* 8 bytes before the kevent list: the id of the workloop */
	wqstart = w->wqstart;
	/* the changes that the thread brings go to the kqueue that it worked for (its workloop, or the one of the workqueue) */
	first_kq = t->bound != 0 ? t->bound : -1;
	if (kevent_return) {
		davail = WQ_KEVENT_DATA_SIZE; count = 0;
		error = darwin_wq_kevent(td, first_kq, item, nevents, kevent_list, 0, data_buf, &davail, &count);
		if (darwin_debug >= 2)
			printf("darwinator: %s[%d]: the changes of the thread (%d) for the kqueue %ld: error %d\n", td->td_proc->p_comm, td->td_proc->p_pid, nevents, (long)first_kq, error);
		if (darwin_debug >= 2 && error != 0 && nevents > 0) {
			uint8_t c[72];

			if (copyin((void *)item, c, sizeof(c)) == 0) {
				uint64_t id; int16_t fl; uint16_t fg; uint32_t ff;

				memcpy(&id, c, 8); memcpy(&fl, c + 8, 2); memcpy(&fg, c + 10, 2); memcpy(&ff, c + 24, 4);
				printf("darwinator: %s[%d]: the failed change: ident %#lx filter %d flags %#x fflags %#x\n", td->td_proc->p_comm, td->td_proc->p_pid, (u_long)id, fl, fg, ff);
			}
		}
	}
	if (t->bound != 0) {
		mtx_lock(&wq_mtx);
		wl_release(w, t->bound);
		mtx_unlock(&wq_mtx);
		t->bound = 0;
	}
	mtx_lock(&wq_mtx);
	if (t->ismgr) { t->ismgr = 0; w->mgr = 0; }
	mtx_unlock(&wq_mtx);
	t->parked = 1;
	for (;;) {
		/* work that was asked for */
		mtx_lock(&wq_mtx);
		if (w->want > 0 || w->want_oc > 0) {
			bool oc = w->want_oc > 0;
			int qos = oc ? w->wantqos_oc : w->wantqos;

			if (oc) w->want_oc--; else w->want--;
			t->parked = 0;
			mtx_unlock(&wq_mtx);
			wq_set_regs(td, t, wqstart, 0, (base_flags & ~WQ_FLAG_THREAD_KEVENT & ~0xfu) | (oc ? WQ_FLAG_THREAD_OVERCOMMIT : 0) | (uint32_t)qos, 0, (t->self - 64) & ~0xfULL);
			return (EJUSTRETURN);
		}
		/* a thread request of a workloop that nobody serves */
		for (k = 0; k < 16; k++) {
			if (darwin_wl_stale_check && w->req[k].id != 0 && w->req[k].eaddr != 0 && w->req[k].emask != 0) {		/* the queue was taken (or emptied) since the request: the state is not the one that the request was made for (libdispatch aborts: "Invalid wlh state") */
				uint64_t qs = 0;
				uint64_t ea = w->req[k].eaddr, em = w->req[k].emask, ev = w->req[k].evalue;

				mtx_unlock(&wq_mtx);
				if (copyin((void *)(uintptr_t)ea, &qs, 8) == 0 && (qs & 0xfffffffcULL) != 0 && (qs & 1) != 0) {
					/* the queue is owned by a thread that does a dispatch_sync on it (the owner is in the lock bits of dq_state: NOTE_WL_DISCOVER_OWNER makes XNU read it): as XNU does, the request is not served while somebody owns the workloop;
					 * it stays, and the thread that looks again (the pool polls) takes it when the owner has gone (libdispatch aborts when it gets the queue locked by another thread and not enqueued any more) */
					mtx_lock(&wq_mtx);
					continue;
				}
				if (qs != 0 && (qs & em & ~0x700000000ULL) != (ev & em & ~0x700000000ULL)) {
					if (darwin_debug >= 2)
						printf("darwinator: %s[%d]: the request of the workloop %#lx is stale (state %#lx mask %#lx want %#lx)\n", td->td_proc->p_comm, td->td_proc->p_pid, (u_long)w->req[k].id, (u_long)qs, (u_long)em, (u_long)ev);
					mtx_lock(&wq_mtx);
					if (w->req[k].eaddr == ea) w->req[k].id = 0;
					continue;
				}
				mtx_lock(&wq_mtx);
			}
			if (w->req[k].id != 0 && !wl_busy(w, w->req[k].id) && wl_claim(w, w->req[k].id, td->td_tid)) {
				int64_t rid = w->req[k].id;
				uint64_t udata = w->req[k].udata, kqid = (uint64_t)rid;
				uint32_t rqos = w->req[k].qos;
				uint8_t ev[WQ_KEVENT_SIZE];

				w->req[k].id = 0;
				t->parked = 0;
				if (darwin_debug >= 2) {
					uint64_t qs2 = 0;

					if (w->req[k].eaddr != 0) (void)copyin((void *)(uintptr_t)w->req[k].eaddr, &qs2, 8);
					printf("darwinator: serving workloop %#lx: state now %#lx (mask %#lx want %#lx)\n", (u_long)rid, (u_long)qs2, (u_long)w->req[k].emask, (u_long)w->req[k].evalue);
				}
				if (darwin_debug >= 2)
					printf("darwinator: %s[%d]: a thread serves the request of the workloop %#lx\n", td->td_proc->p_comm, td->td_proc->p_pid, (u_long)rid);
				{
					uint64_t qn = 0;
					struct wqdl *r;

					if (w->req[k].eaddr != 0) (void)copyin((void *)(uintptr_t)w->req[k].eaddr, &qn, 8);
					r = &wq_dl[wq_dl_n++ % 32]; r->tid = td->td_tid; r->id = rid; r->qs = qn; r->em = w->req[k].emask; r->ev = w->req[k].evalue; r->ea = w->req[k].eaddr; r->ticks = ticks;
				}
				mtx_unlock(&wq_mtx);
				memset(ev, 0, sizeof(ev));
				memcpy(ev, &kqid, 8);					/* ident: the workloop */
				{ int16_t flt = -17; uint16_t fl = 0x0005; memcpy(ev + 8, &flt, 2); memcpy(ev + 10, &fl, 2); }
				memcpy(ev + 12, &rqos, 4);
				memcpy(ev + 16, &udata, 8);
				{ uint32_t ff = 0x1; memcpy(ev + 24, &ff, 4); }			/* NOTE_WL_THREAD_REQUEST */
				(void)copyout(ev, (void *)kevent_list, sizeof(ev));
				(void)copyout(&kqid, (void *)(kevent_list - 8), sizeof(kqid));
				davail = WQ_KEVENT_DATA_SIZE; count = 0;
				error = darwin_wq_kevent(td, rid, 0, 0, kevent_list + WQ_KEVENT_SIZE, WQ_KEVENT_LIST_LEN - 1, data_buf, &davail, &count);
				t->bound = rid;
				top = (data_buf + davail) & ~0xfULL;
				wq_set_regs(td, t, wqstart, kevent_list, base_flags | WQ_FLAG_THREAD_WORKLOOP, (uint32_t)(count + 1), top);
				return (EJUSTRETURN);
			}
		}
		mtx_unlock(&wq_mtx);
		/* events of the workloops: the thread works for the first that has some (and nobody else works for) */
		n = darwin_wq_kq_ids(td->td_proc, ids, WQ_NREQ);
		for (k = 0; k < n; k++) {
			bool got;

			mtx_lock(&wq_mtx);
			got = !wl_busy(w, ids[k]) && wl_claim(w, ids[k], td->td_tid);
			mtx_unlock(&wq_mtx);
			if (!got)
				continue;
			davail = WQ_KEVENT_DATA_SIZE; count = 0;
			error = darwin_wq_kevent(td, ids[k], 0, 0, kevent_list, WQ_KEVENT_LIST_LEN, data_buf, &davail, &count);
			if (error == 0 && count > 0) {
				uint64_t kqid = (uint64_t)ids[k];
				t->parked = 0;

				if (darwin_debug >= 2)
					printf("darwinator: %s[%d]: a thread serves %d event(s) of the workloop %#lx (no thread request)\n", td->td_proc->p_comm, td->td_proc->p_pid, count, (u_long)kqid);
				(void)copyout(&kqid, (void *)(kevent_list - 8), sizeof(kqid));
				t->bound = ids[k];
				top = (data_buf + davail) & ~0xfULL;
				wq_set_regs(td, t, wqstart, kevent_list, base_flags | WQ_FLAG_THREAD_WORKLOOP, (uint32_t)count, top);
				return (EJUSTRETURN);
			}
			mtx_lock(&wq_mtx);
			wl_release(w, ids[k]);
			mtx_unlock(&wq_mtx);
		}
		/* events of the kqueue of the workqueue (not while the event manager is out: its events are for it, and a second manager would fail on its lock) */
		bool claimed;

		davail = WQ_KEVENT_DATA_SIZE; count = 0;
		mtx_lock(&wq_mtx);
		n = w->mgr;
		if (n == 0)
			w->mgr = 1;			/* claimed before the look: two threads that look at once would both find events and both become the manager (libdispatch: "Locking the manager should not fail") */
		mtx_unlock(&wq_mtx);
		claimed = n == 0;
		if (!claimed && darwin_debug >= 1 && (quiet % 500) == 499) {		/* the event manager has been out for long: who is it? */
			int q;

			mtx_lock(&wq_mtx);
			for (q = 0; q < WQ_MAX_THREADS; q++)
				if (w->thr[q].used && w->thr[q].ismgr)
					printf("darwinator: %s[%d]: the event manager is thread %d (tid %d), out for a long time (wait: %s)\n", td->td_proc->p_comm, td->td_proc->p_pid, q, w->thr[q].td != NULL ? (int)w->thr[q].td->td_tid : -1, w->thr[q].td != NULL && w->thr[q].td->td_wmesg != NULL ? w->thr[q].td->td_wmesg : "-");
			mtx_unlock(&wq_mtx);
		}
		error = claimed ? darwin_wq_kevent(td, -1, 0, 0, kevent_list, WQ_KEVENT_LIST_LEN, data_buf, &davail, &count) : 0;
		if (claimed && !(error == 0 && count > 0)) {		/* nothing: the claim goes back */
			mtx_lock(&wq_mtx);
			w->mgr = 0;
			mtx_unlock(&wq_mtx);
			claimed = false;
		}
		if (error == 0 && count > 0) {
			t->parked = 0;
			if (darwin_debug >= 2) {
				int16_t f0 = 0;

				(void)copyin((void *)(kevent_list + 8), &f0, sizeof(f0));
				printf("darwinator: %s[%d]: %d event(s) of the workqueue kqueue to a thread of the pool (first: filter %d)\n", td->td_proc->p_comm, td->td_proc->p_pid, count, f0);
			}
			top = (data_buf + davail) & ~0xfULL;
			{
				/* the events of the workqueue's own kqueue are the event manager's (the timers of libdispatch, its manager queue), except a batch of Mach messages only: those
				 * are for an ordinary thread, which runs the queue that they wake (the manager thread only drains the manager queue, so a message for an XPC listener stayed unread) */
				bool only_mach = true;
				int j;

				for (j = 0; j < (int)count; j++) {
					int16_t fj = 0;

					if (copyin((void *)(kevent_list + (uint64_t)j * WQ_KEVENT_SIZE + 8), &fj, sizeof(fj)) != 0 || fj != -8)
						only_mach = false;
				}
				mtx_lock(&wq_mtx);
				if (!only_mach) { t->ismgr = 1; w->mgr = 1; }		/* the claim becomes the manager */
				else if (claimed) w->mgr = 0;				/* only Mach messages: an ordinary thread, the claim goes back */
				mtx_unlock(&wq_mtx);
				wq_set_regs(td, t, wqstart, kevent_list, base_flags | (only_mach ? 0 : WQ_FLAG_THREAD_EVENT_MANAGER), (uint32_t)count, top);
			}
			return (EJUSTRETURN);
		}
		/* a thread beyond the first two that was idle for long sleeps for long (a request for threads wakes it): the first two look at the kqueues.  A thread does not end here: libpthread keeps
		 * every thread in its list, and the memory of one that ended is gone, so the next new thread would write there */
		quiet++;
		error = tsleep(t, PCATCH, "wqpark", (t - w->thr) >= 2 ? hz : WQ_POLL * (1 + MIN(quiet / 100, 9)));	/* the poll backs off while nothing comes (up to 20 ms): launchservicesd has 16 workloops and two threads that looked at all of them 500 times a second used 1.5 CPUs */
		if (error == EINTR || error == ERESTART) {
			/* a stop for the debugger (lldb attaches with a signal) wakes the sleep with nothing to deliver; libpthread takes an error here as a fault and aborts, so the thread goes on parking */
			bool deliver;

			PROC_LOCK(td->td_proc);
			deliver = (td->td_proc->p_flag & P_TRACED) == 0 || SIGPENDING(td) || (td->td_proc->p_flag & (P_WEXIT | P_SINGLE_EXIT | P_STOPPED_SINGLE | P_SINGLE_BOUNDARY | P_WKILLED)) != 0;	/* only a process that is being debugged goes on (anything else would spin here when the sleep returns at once) */
			PROC_UNLOCK(td->td_proc);
			if (!deliver) {
				continue;
			}
		}
		if (error != 0 && error != EWOULDBLOCK)
			break;
	}
	return (error == ERESTART ? EINTR : error);
}

int
darwin_workq_open(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = 0;
	return (0);
}

int
darwin_workq_kernreturn(struct thread *td, struct darwin_args *uap)
{
	struct wqp *w;
	uint64_t wqstart, pthsize;
	int options = (int)uap->a[0], arg2 = (int)uap->a[2], error = 0;
	uint32_t arg3 = (uint32_t)uap->a[3];

	td->td_retval[0] = 0;
	if (darwin_debug >= 2)
		printf("darwinator: %s[%d]: workq_kernreturn(options %#x, item %#lx, arg2 %d, arg3 %#x)\n", td->td_proc->p_comm, td->td_proc->p_pid, options, (u_long)uap->a[1], arg2, arg3);
	if (!darwin_pth_info(td->td_proc, &wqstart, &pthsize))
		return (EINVAL);
	switch (options) {
	case WQOPS_QUEUE_NEWSPISUPP:
	case WQOPS_SET_EVENT_MANAGER_PRIORITY:
	case WQOPS_SETUP_DISPATCH:
		return (0);
	case WQOPS_SHOULD_NARROW:
		td->td_retval[0] = 0;
		return (0);
	case WQOPS_QUEUE_REQTHREADS:
	case WQOPS_QUEUE_REQTHREADS2: {
		int n = arg2 > 0 ? arg2 : 0;
		uint32_t pri = options == WQOPS_QUEUE_REQTHREADS2 ? (uint32_t)uap->a[1] : arg3;
		int qos = qos_of_priority(pri);

		if ((pri & PTHREAD_PRIORITY_EVENT_MANAGER) != 0) {	/* libdispatch asks for its event manager: one thread that comes out as such */
			mtx_lock(&wq_mtx);
			w = wq_get(td->td_proc, true);
			if (w == NULL) { mtx_unlock(&wq_mtx); return (EAGAIN); }
			w->wqstart = wqstart; w->pthsize = pthsize;
			if (w->mgr != 0) { mtx_unlock(&wq_mtx); return (0); }
			if (darwin_debug >= 2)
				printf("darwinator: %s[%d]: the event manager thread is asked for\n", td->td_proc->p_comm, td->td_proc->p_pid);
			error = wq_spawn(td, w, qos, true);
			mtx_unlock(&wq_mtx);
			return (error == EAGAIN ? 0 : error);
		}

		mtx_lock(&wq_mtx);
		w = wq_get(td->td_proc, true);
		if (w == NULL) { mtx_unlock(&wq_mtx); return (EAGAIN); }
		w->wqstart = wqstart; w->pthsize = pthsize;
		if ((pri & 0x80000000u) != 0) {			/* _PTHREAD_PRIORITY_OVERCOMMIT_FLAG */
			w->want_oc += n; w->wantqos_oc = qos;
			if (w->want_oc > WQ_MAX_THREADS)
				w->want_oc = WQ_MAX_THREADS;
		} else {
			w->want += n; w->wantqos = qos;
			if (w->want > WQ_MAX_THREADS)
				w->want = WQ_MAX_THREADS;
		}
		{
			int i, idle = 0, woken = 0;

			for (i = 0; i < WQ_MAX_THREADS; i++)		/* wake the parked threads first: they take the work (as many as the requests: waking all of them at every request kept 33 threads of launchservicesd at 4 CPUs) */
				if (w->thr[i].used && w->thr[i].td != NULL) {
					if (woken < MAX((int)n, 1)) { wakeup(&w->thr[i]); woken++; }
					idle++;
				}
			while ((w->want > 0 || w->want_oc > 0) && w->nthreads < WQ_MAX_THREADS && w->nthreads >= idle) {
				bool oc = w->want_oc > 0;

				if (oc) w->want_oc--; else w->want--;	/* the new thread starts with this request */
				error = wq_spawn(td, w, oc ? (w->wantqos_oc | 0x100) : w->wantqos, false);
				if (error != 0) { if (oc) w->want_oc++; else w->want++; break; }
			}
		}
		mtx_unlock(&wq_mtx);
		return (error == EAGAIN ? 0 : error);
	}
	case WQOPS_THREAD_RETURN:
	case WQOPS_THREAD_KEVENT_RETURN:
	case WQOPS_THREAD_WORKLOOP_RETURN: {
		struct wqthr *t;

		mtx_lock(&wq_mtx);
		w = wq_get(td->td_proc, false);
		t = w != NULL ? wq_self(w, td) : NULL;
		mtx_unlock(&wq_mtx);
		if (t == NULL)
			return (EINVAL);
		return (wq_park(td, w, t, uap->a[1], arg2, options != WQOPS_THREAD_RETURN));
	}
	default:
		return (EINVAL);
	}
}


/*
 * EVFILT_WORKLOOP with NOTE_WL_THREAD_REQUEST: libdispatch asks the kernel for a thread to run the serial queue that is a workloop (the change is made in kevent_id; with EV_DELETE the request is
 * taken back).  The request stays until a thread of the pool takes it: it is then handed out as the first event of the list (the same change, as XNU does), with the other events of the
 * workloop after it.  The check of the queue state (ext[0..2]) that XNU makes is not made: a thread may come for a queue that has no more work, which dispatch accepts.
 */
void
darwin_wq_request(struct thread *td, int64_t id, uint64_t udata, uint32_t qos, bool add, uint64_t eaddr, uint64_t emask, uint64_t evalue)
{
	struct wqp *w;
	uint64_t wqstart, pthsize;
	int i, freei = -1, parked = 0;

	if (darwin_debug >= 2)
		printf("darwinator: %s[%d]: thread request for the workloop %#lx: %s\n", td->td_proc->p_comm, td->td_proc->p_pid, (u_long)id, add ? "add" : "delete");
	if (!darwin_pth_info(td->td_proc, &wqstart, &pthsize))
		return;
	mtx_lock(&wq_mtx);
	w = wq_get(td->td_proc, true);
	if (w == NULL) { mtx_unlock(&wq_mtx); return; }
	w->wqstart = wqstart; w->pthsize = pthsize;
	for (i = 0; i < WQ_NREQ; i++) {
		if (w->req[i].id == id) {
			if (add) { w->req[i].udata = udata; w->req[i].qos = qos; w->req[i].eaddr = eaddr; w->req[i].emask = emask; w->req[i].evalue = evalue; } else w->req[i].id = 0;
			freei = -2;
			break;
		}
		if (w->req[i].id == 0 && freei == -1) freei = i;
	}
	if (add && freei >= 0) { w->req[freei].id = id; w->req[freei].udata = udata; w->req[freei].qos = qos; w->req[freei].eaddr = eaddr; w->req[freei].emask = emask; w->req[freei].evalue = evalue; }
	if (add) {
		for (i = 0; i < WQ_MAX_THREADS; i++)
			if (w->thr[i].used && w->thr[i].td != NULL) {
				wakeup(&w->thr[i]);
				if (w->thr[i].parked) parked++;
			}
		if (parked == 0 && w->nthreads < WQ_MAX_THREADS)
			(void)wq_spawn(td, w, 5, false);
	}
	mtx_unlock(&wq_mtx);
}

/* a change in the kqueue of the workqueue: make sure that one thread of the pool looks at it (the events of the sources of libdispatch are handed out only to a thread of the pool) */
void
darwin_wq_note(struct thread *td)
{
	struct wqp *w;
	uint64_t wqstart, pthsize;

	if (!darwin_pth_info(td->td_proc, &wqstart, &pthsize))
		return;
	mtx_lock(&wq_mtx);
	w = wq_get(td->td_proc, true);
	if (w != NULL && w->nthreads == 0) {
		w->wqstart = wqstart; w->pthsize = pthsize;
		(void)wq_spawn(td, w, 5, false);
	} else if (w != NULL) {
		int i;

		for (i = 0; i < 2; i++)			/* only the first two threads look at the kqueues (the others sleep): a program that adds sources all the time woke all of them (33 in launchservicesd) and used 4 CPUs */
			if (w->thr[i].used && w->thr[i].td != NULL)
				wakeup(&w->thr[i]);
	}
	mtx_unlock(&wq_mtx);
}
