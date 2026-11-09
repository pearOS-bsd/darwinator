/*
 * psynch (what libpthread of Darwin sleeps on: mutexes, condition variables, read-write locks), the workqueue calls, and the Mach traps
 * that a program makes without Mach IPC (virtual memory, port names, the time base, waiting until a time).
 *
 * psynch here is a simplification of xnu's kernel side: every lock or condition variable address of a process has a counter of waiters and a
 * counter of wake tokens, under one lock.  A wake with nobody waiting is remembered for a mutex (a drop before the wait must not be lost)
 * and forgotten for a condition variable (a signal with no waiter is lost, as POSIX says).  The generation numbers that xnu keeps in the word of
 * the lock are not interpreted: the value that comes back from psynch_mutexwait is the generation that was passed in.
 * Mach IPC is here only inside one process (see below): no ports shared between processes, so no bootstrap server and no services.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/sx.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/time.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"
#include "darwin_abi.h"

/* ---- psynch ---- */

struct ps_ent {
	struct proc	*p;
	pid_t		pid;
	uint64_t	addr;
	int		waiters, tokens;
	bool		used;
};
#define	PS_N	512
static struct ps_ent ps_tab[PS_N];
static struct sx ps_sx;
SX_SYSINIT(darwin_ps, &ps_sx, "darwin psynch");

static struct ps_ent *
ps_find(struct proc *p, uint64_t addr, bool create)
{
	int i, free_i = -1, evict = -1;

	for (i = 0; i < PS_N; i++) {
		struct ps_ent *e = &ps_tab[i];

		if (e->used && e->p == p && e->pid == p->p_pid && e->addr == addr)
			return (e);
		if (!e->used && free_i < 0)
			free_i = i;
		else if (e->used && e->waiters == 0 && evict < 0 && (e->tokens == 0 || e->pid != p->p_pid))
			evict = i;
	}
	if (!create)
		return (NULL);
	i = free_i >= 0 ? free_i : evict;
	if (i < 0)
		return (NULL);
	ps_tab[i] = (struct ps_ent){ .p = p, .pid = p->p_pid, .addr = addr, .used = true };
	return (&ps_tab[i]);
}

/* sleeps until a token is there (consumes it); timeout in ticks, 0 = none */
static int
ps_wait(struct ps_ent *e, int timo)
{
	int error = 0;

	e->waiters++;
	while (e->tokens == 0 && error == 0)
		error = sx_sleep(e, &ps_sx, PCATCH, "psynch", timo);
	e->waiters--;
	if (error == 0)
		e->tokens--;
	else if (error == EWOULDBLOCK)
		error = ETIMEDOUT;
	else if (error == ERESTART)
		error = EINTR;
	if (e->waiters == 0 && e->tokens == 0)
		e->used = false;
	return (error);
}

static void
ps_release(struct ps_ent *e, int n, bool remember)
{
	if (!remember && e->waiters - e->tokens < n)
		n = e->waiters - e->tokens;
	if (n > 0) {
		e->tokens += n;
		if (n == 1)
			wakeup_one(e);
		else
			wakeup(e);
	}
}

static int
ps_timo(int64_t sec, int64_t nsec)
{
	int64_t t;

	if (sec == 0 && nsec == 0)
		return (0);
	t = sec * hz + nsec / (1000000000 / hz);
	return (t < 1 ? 1 : t > INT_MAX ? INT_MAX : (int)t);
}

static int
darwin_psynch_mutexwait(struct thread *td, struct darwin_args *uap)
{
	struct ps_ent *e;
	int error;

	sx_xlock(&ps_sx);
	e = ps_find(td->td_proc, uap->a[0], true);
	error = e == NULL ? ENOMEM : ps_wait(e, 0);
	sx_xunlock(&ps_sx);
	td->td_retval[0] = (uint32_t)uap->a[1];
	return (error);
}

static int
darwin_psynch_mutexdrop(struct thread *td, struct darwin_args *uap)
{
	struct ps_ent *e;

	sx_xlock(&ps_sx);
	e = ps_find(td->td_proc, uap->a[0], true);
	if (e != NULL)
		ps_release(e, 1, true);
	sx_xunlock(&ps_sx);
	td->td_retval[0] = 0;
	return (e == NULL ? ENOMEM : 0);
}

static int
darwin_psynch_cvwait(struct thread *td, struct darwin_args *uap)
{
	struct ps_ent *cv, *mx;
	int error;

	sx_xlock(&ps_sx);
	cv = ps_find(td->td_proc, uap->a[0], true);
	if (cv == NULL) {
		sx_xunlock(&ps_sx);
		return (ENOMEM);
	}
	if (uap->a[3] != 0 && (mx = ps_find(td->td_proc, uap->a[3], true)) != NULL)
		ps_release(mx, 1, true);		/* the mutex is dropped and the wait begins under one lock */
	error = ps_wait(cv, ps_timo((int64_t)uap->a[6], (int64_t)uap->a[7]));
	sx_xunlock(&ps_sx);
	td->td_retval[0] = 0;
	return (error);
}

static int
darwin_psynch_cvsignal(struct thread *td, struct darwin_args *uap)
{
	struct ps_ent *cv;

	sx_xlock(&ps_sx);
	if ((cv = ps_find(td->td_proc, uap->a[0], false)) != NULL)
		ps_release(cv, 1, false);
	sx_xunlock(&ps_sx);
	td->td_retval[0] = 0;
	return (0);
}

static int
darwin_psynch_cvbroad(struct thread *td, struct darwin_args *uap)
{
	struct ps_ent *cv;

	sx_xlock(&ps_sx);
	if ((cv = ps_find(td->td_proc, uap->a[0], false)) != NULL)
		ps_release(cv, cv->waiters, false);
	sx_xunlock(&ps_sx);
	td->td_retval[0] = 0;
	return (0);
}

/* read-write locks: a waiter sleeps until the unlock; the unlock wakes every waiter (they try again in user space) */
static int
darwin_psynch_rw_wait(struct thread *td, struct darwin_args *uap)
{
	struct ps_ent *e;
	int error;

	sx_xlock(&ps_sx);
	e = ps_find(td->td_proc, uap->a[0], true);
	error = e == NULL ? ENOMEM : ps_wait(e, 0);
	sx_xunlock(&ps_sx);
	td->td_retval[0] = 0;
	return (error);
}

static int
darwin_psynch_rw_unlock(struct thread *td, struct darwin_args *uap)
{
	struct ps_ent *e;

	sx_xlock(&ps_sx);
	if ((e = ps_find(td->td_proc, uap->a[0], false)) != NULL)
		ps_release(e, e->waiters, false);
	sx_xunlock(&ps_sx);
	td->td_retval[0] = 0;
	return (0);
}

/* ---- the workqueue: not implemented (a program that uses it gets the answer of a kernel without it) ---- */

static int
darwin_workq_open(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = 0;
	return (0);
}

static int
darwin_workq_kernreturn(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = 0;	/* what a Mac answers a thread that returns to the pool: 0 */
	return (0);
}

/* ---- Mach traps: the return value is a kern_return_t in the result register, not an errno ---- */

#define	KERN_SUCCESS		0
#define	KERN_INVALID_ADDRESS	1
#define	KERN_INVALID_ARGUMENT	4
#define	KERN_NOT_SUPPORTED	46
#define	MACH_SEND_INVALID_DEST	0x10000003
#define	VM_FLAGS_ANYWHERE	0x0001
#define	MACH_TASK_SELF		0x103
#define	MACH_HOST_SELF		0x203

static int
kr_of(struct thread *td, int error)
{
	td->td_retval[0] = error == 0 ? KERN_SUCCESS : error == EFAULT ? KERN_INVALID_ADDRESS : KERN_INVALID_ARGUMENT;
	return (0);
}

static int
mach_anon(struct thread *td, uint64_t *addrp, size_t size, int flags, int prot, int usereg_addr)
{
	struct mmap_args ma;
	uint64_t addr = 0;
	int error;

	(void)usereg_addr;
	if ((error = copyin((void *)addrp, &addr, sizeof(addr))) != 0)
		return (error);
	ma.addr = (flags & VM_FLAGS_ANYWHERE) ? NULL : (void *)addr;
	ma.len = size;
	ma.prot = prot;
	ma.flags = MAP_ANON | MAP_PRIVATE | ((flags & VM_FLAGS_ANYWHERE) ? 0 : MAP_FIXED);
	ma.fd = -1;
	ma.pos = 0;
	if ((error = sys_mmap(td, &ma)) != 0)
		return (error);
	addr = (uint64_t)td->td_retval[0];
	return (copyout(&addr, (void *)addrp, sizeof(addr)));
}

static int
darwin_mach_vm_allocate(struct thread *td, struct darwin_args *uap)	/* (target, *addr, size, flags) */
{
	return (kr_of(td, mach_anon(td, (uint64_t *)uap->a[1], (size_t)uap->a[2], (int)uap->a[3], PROT_READ | PROT_WRITE, 0)));
}

static int
darwin_mach_vm_map(struct thread *td, struct darwin_args *uap)		/* (target, *addr, size, mask, flags, cur_protection): anonymous memory only */
{
	return (kr_of(td, mach_anon(td, (uint64_t *)uap->a[1], (size_t)uap->a[2], (int)uap->a[4], (int)uap->a[5] & 7, 0)));
}

static int
darwin_mach_vm_deallocate(struct thread *td, struct darwin_args *uap)	/* (target, addr, size) */
{
	struct munmap_args ma = { .addr = (void *)uap->a[1], .len = (size_t)uap->a[2] };

	return (kr_of(td, sys_munmap(td, &ma)));
}

static int
darwin_mach_vm_protect(struct thread *td, struct darwin_args *uap)	/* (target, addr, size, set_maximum, prot) */
{
	struct mprotect_args ma = { .addr = (void *)uap->a[1], .len = (size_t)uap->a[2], .prot = (int)uap->a[4] & 7 };

	return (kr_of(td, sys_mprotect(td, &ma)));
}

/* ---- Mach IPC inside a process: ports with a queue of messages ----
 * A port is an entry of one table.  The name that a task uses is 0x1003 + 0x100 * index.  A name can hold the receive right and a send right at
 * once (as in Mach).  Messages: a header and inline data (no descriptors: out-of-line memory and port rights in a message are not supported),
 * copied into the queue; on receive the remote and local ports are swapped (the receiver sees the reply port as the remote port).  What is not
 * here: ports shared between processes (so no bootstrap server and no services), port sets, notifications, rights in messages. */

#define	MACH_PORT_RIGHT_SEND		0
#define	MACH_PORT_RIGHT_RECEIVE		1
#define	MACH_MSG_TYPE_MOVE_SEND		17
#define	MACH_MSG_TYPE_COPY_SEND		19
#define	MACH_MSG_TYPE_MAKE_SEND		20
#define	MACH_SEND_MSG			0x00000001
#define	MACH_RCV_MSG			0x00000002
#define	MACH_RCV_TIMEOUT		0x00000100
#define	MACH_MSGH_BITS_COMPLEX		0x80000000
#define	MACH_SEND_INVALID_DATA		0x1000000c
#define	MACH_SEND_TOO_LARGE		0x10000007
#define	MACH_SEND_MSG_TOO_SMALL		0x10000008
#define	MACH_RCV_INVALID_NAME		0x10004002
#define	MACH_RCV_TIMED_OUT		0x10004003
#define	MACH_RCV_TOO_LARGE		0x10004004
#define	MACH_MSG_MAXSIZE		65536

struct dmsg {
	TAILQ_ENTRY(dmsg)	link;
	uint32_t		size;
	uint8_t			data[];
};
struct mport {
	struct proc	*p;
	pid_t		pid;
	bool		used, recv, send;
	int		waiters;
	TAILQ_HEAD(, dmsg) q;
};
#define	MP_N	256
static struct mport mp_tab[MP_N];
static struct sx mp_sx;
SX_SYSINIT(darwin_mp, &mp_sx, "darwin mach ports");
#define	MP_NAME(i)	(0x1003u + 0x100u * (i))

static struct mport *
mp_lookup(struct proc *p, uint32_t name)
{
	uint32_t i;

	if (name < 0x1003 || ((name - 0x1003) & 0xff) != 0)
		return (NULL);
	i = (name - 0x1003) >> 8;
	if (i >= MP_N || !mp_tab[i].used || mp_tab[i].p != p || mp_tab[i].pid != p->p_pid)
		return (NULL);
	return (&mp_tab[i]);
}

static uint32_t
mp_new(struct proc *p, bool recv)
{
	int i, victim = -1;

	for (i = 0; i < MP_N; i++) {
		if (!mp_tab[i].used)
			break;
		if (victim < 0 && mp_tab[i].p != p && mp_tab[i].waiters == 0)
			victim = i;
	}
	if (i == MP_N)
		i = victim;			/* the table is full: a port of another process goes (its process may be gone) */
	if (i < 0)
		return (0);
	if (mp_tab[i].used) {
		struct dmsg *m;

		while ((m = TAILQ_FIRST(&mp_tab[i].q)) != NULL) { TAILQ_REMOVE(&mp_tab[i].q, m, link); free(m, M_DARWIN); }
	}
	mp_tab[i] = (struct mport){ .p = p, .pid = p->p_pid, .used = true, .recv = recv, .send = false };
	TAILQ_INIT(&mp_tab[i].q);
	return (MP_NAME(i));
}

static int
darwin_mach_port_allocate(struct thread *td, struct darwin_args *uap)	/* (target, right, *name) */
{
	uint32_t name;
	int error;

	if ((int)uap->a[1] != MACH_PORT_RIGHT_RECEIVE) {
		td->td_retval[0] = KERN_INVALID_ARGUMENT;
		return (0);
	}
	sx_xlock(&mp_sx);
	name = mp_new(td->td_proc, true);
	sx_xunlock(&mp_sx);
	if (name == 0) {
		td->td_retval[0] = 6;		/* KERN_NO_SPACE */
		return (0);
	}
	error = copyout(&name, (void *)uap->a[2], sizeof(name));
	return (kr_of(td, error));
}

static int
darwin_mach_port_insert_right(struct thread *td, struct darwin_args *uap)	/* (target, name, poly, polyPoly): a send right for a port that this task receives on */
{
	struct mport *m;
	int kr = KERN_INVALID_ARGUMENT;

	sx_xlock(&mp_sx);
	if ((m = mp_lookup(td->td_proc, (uint32_t)uap->a[2])) != NULL && (uint32_t)uap->a[1] == (uint32_t)uap->a[2] &&
	    (m->recv || m->send) && ((int)uap->a[3] == MACH_MSG_TYPE_MAKE_SEND || (int)uap->a[3] == MACH_MSG_TYPE_COPY_SEND ||
	    (int)uap->a[3] == MACH_MSG_TYPE_MOVE_SEND)) {
		m->send = true;
		kr = KERN_SUCCESS;
	}
	sx_xunlock(&mp_sx);
	td->td_retval[0] = kr;
	return (0);
}

static int
darwin_mach_port_deallocate(struct thread *td, struct darwin_args *uap)	/* (target, name) */
{
	struct mport *m;
	int kr = KERN_INVALID_ARGUMENT;

	sx_xlock(&mp_sx);
	if ((m = mp_lookup(td->td_proc, (uint32_t)uap->a[1])) != NULL) {
		if (m->recv)
			m->send = false;	/* the send right goes, the receive right stays */
		else
			m->used = false;
		kr = KERN_SUCCESS;
	}
	sx_xunlock(&mp_sx);
	td->td_retval[0] = kr;
	return (0);
}

static int
darwin_mach_port_destroy(struct thread *td, struct darwin_args *uap)		/* mod_refs(target, name, right, delta) with the receive right and delta -1: the port goes */
{
	struct mport *m;
	struct dmsg *d;
	int kr = KERN_SUCCESS;

	sx_xlock(&mp_sx);
	if ((m = mp_lookup(td->td_proc, (uint32_t)uap->a[1])) != NULL && (int)uap->a[2] == MACH_PORT_RIGHT_RECEIVE && (int)uap->a[3] < 0) {
		while ((d = TAILQ_FIRST(&m->q)) != NULL) { TAILQ_REMOVE(&m->q, d, link); free(d, M_DARWIN); }
		m->used = false;
		wakeup(m);
	} else if (m == NULL)
		kr = KERN_INVALID_ARGUMENT;
	sx_xunlock(&mp_sx);
	td->td_retval[0] = kr;
	return (0);
}

static int
darwin_mach_reply_port(struct thread *td, struct darwin_args *uap __unused)
{
	uint32_t name;

	sx_xlock(&mp_sx);
	name = mp_new(td->td_proc, true);
	if (name != 0)
		mp_lookup(td->td_proc, name)->send = true;	/* the reply port can be sent to by its owner (a reply is made with a send right to it) */
	sx_xunlock(&mp_sx);
	td->td_retval[0] = name;
	return (0);
}

static int
mach_msg_common(struct thread *td, uint64_t msg, uint32_t option, uint32_t ssize, uint32_t rsize, uint32_t rname, uint32_t timeout_ms)
{
	struct proc *p = td->td_proc;
	struct mport *m;
	struct dmsg *d;
	uint32_t hdr[6];
	int error;

	if (option & MACH_SEND_MSG) {
		struct mport *dest;
		uint32_t dname;

		if (ssize < sizeof(hdr))
			return (MACH_SEND_MSG_TOO_SMALL);
		if (ssize > MACH_MSG_MAXSIZE)
			return (MACH_SEND_TOO_LARGE);
		d = malloc(sizeof(*d) + ssize, M_DARWIN, M_WAITOK);
		if (copyin((void *)msg, d->data, ssize) != 0) {
			free(d, M_DARWIN);
			return (KERN_INVALID_ADDRESS);
		}
		d->size = ssize;
		memcpy(hdr, d->data, sizeof(hdr));
		if (hdr[0] & MACH_MSGH_BITS_COMPLEX) {		/* descriptors: not supported */
			free(d, M_DARWIN);
			return (MACH_SEND_INVALID_DATA);
		}
		dname = hdr[2];
		sx_xlock(&mp_sx);
		dest = mp_lookup(p, dname);
		if (dest == NULL || !(dest->send || dest->recv)) {
			sx_xunlock(&mp_sx);
			free(d, M_DARWIN);
			return (MACH_SEND_INVALID_DEST);
		}
		/* the receiver sees the reply port as the remote port and the destination as the local port */
		hdr[2] = hdr[3]; hdr[3] = dname;
		memcpy(d->data, hdr, sizeof(hdr));
		TAILQ_INSERT_TAIL(&dest->q, d, link);
		wakeup(dest);
		sx_xunlock(&mp_sx);
	}
	if (option & MACH_RCV_MSG) {
		int timo = (option & MACH_RCV_TIMEOUT) ? MAX(1, (int)((int64_t)timeout_ms * hz / 1000)) : 0;
		uint32_t kr = 0;

		sx_xlock(&mp_sx);
		m = mp_lookup(p, rname);
		if (m == NULL || !m->recv) {
			sx_xunlock(&mp_sx);
			return (MACH_RCV_INVALID_NAME);
		}
		m->waiters++;
		error = 0;
		while ((d = TAILQ_FIRST(&m->q)) == NULL && error == 0) {
			error = sx_sleep(m, &mp_sx, PCATCH, "machmsg", timo);
			if (!m->used)
				error = EINVAL;
		}
		m->waiters--;
		if (d == NULL) {
			sx_xunlock(&mp_sx);
			return (error == EWOULDBLOCK ? MACH_RCV_TIMED_OUT : KERN_INVALID_ARGUMENT);
		}
		if (d->size > rsize)
			kr = MACH_RCV_TOO_LARGE;			/* the message stays queued */
		else
			TAILQ_REMOVE(&m->q, d, link);
		sx_xunlock(&mp_sx);
		if (kr != 0)
			return (kr);
		error = copyout(d->data, (void *)msg, d->size);
		free(d, M_DARWIN);
		if (error != 0)
			return (KERN_INVALID_ADDRESS);
	}
	return (KERN_SUCCESS);
}

static int
darwin_mach_msg(struct thread *td, struct darwin_args *uap)	/* (msg, option, send_size, rcv_size, rcv_name, timeout, notify) */
{
	td->td_retval[0] = mach_msg_common(td, uap->a[0], (uint32_t)uap->a[1], (uint32_t)uap->a[2], (uint32_t)uap->a[3], (uint32_t)uap->a[4], (uint32_t)uap->a[5]);
	return (0);
}

static int
darwin_mach_thread_self(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = ((uint32_t)td->td_tid << 8) | 0x03;
	return (0);
}

static int
darwin_mach_task_self(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = MACH_TASK_SELF;
	return (0);
}

static int
darwin_mach_host_self(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = MACH_HOST_SELF;
	return (0);
}

static int
darwin_mach_unsupported(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = KERN_NOT_SUPPORTED;
	return (0);
}

static int
darwin_mach_timebase_info(struct thread *td, struct darwin_args *uap)	/* (*info): the absolute time is in nanoseconds */
{
	uint32_t tb[2] = { 1, 1 };

	return (kr_of(td, copyout(tb, (void *)uap->a[0], sizeof(tb))));
}

static int darwin_wait_chan;

static int
darwin_mach_wait_until(struct thread *td, struct darwin_args *uap)	/* (deadline, in nanoseconds of the clock of mach_absolute_time = the uptime) */
{
	struct timespec now;
	int64_t left;
	int error = 0;

	for (;;) {
		nanouptime(&now);
		left = (int64_t)uap->a[0] - ((int64_t)now.tv_sec * 1000000000 + now.tv_nsec);
		if (left <= 0)
			break;
		error = tsleep((void *)&darwin_wait_chan, PCATCH, "machwait", MAX(1, (int)(left / (1000000000 / hz))));
		if (error != 0 && error != EWOULDBLOCK)
			break;
		error = 0;
	}
	td->td_retval[0] = error == 0 ? KERN_SUCCESS : 14 /* KERN_ABORTED */;
	return (0);
}

static const struct {
	int		num, nargs;
	sy_call_t	*fn;
	const char	*name;
} darwin_sync[] = {
#define	E(num, nargs, fn, name)	{ (num), (nargs), (sy_call_t *)(fn), (name) }
	E(301, 5, darwin_psynch_mutexwait,	"psynch_mutexwait"),
	E(302, 5, darwin_psynch_mutexdrop,	"psynch_mutexdrop"),
	E(303, 7, darwin_psynch_cvbroad,	"psynch_cvbroad"),
	E(304, 8, darwin_psynch_cvsignal,	"psynch_cvsignal"),
	E(305, 8, darwin_psynch_cvwait,		"psynch_cvwait"),
	E(306, 5, darwin_psynch_rw_wait,	"psynch_rw_rdlock"),
	E(307, 5, darwin_psynch_rw_wait,	"psynch_rw_wrlock"),
	E(308, 5, darwin_psynch_rw_unlock,	"psynch_rw_unlock"),
	E(309, 5, darwin_psynch_rw_unlock,	"psynch_rw_unlock2"),
	E(367, 0, darwin_workq_open,		"workq_open"),
	E(368, 4, darwin_workq_kernreturn,	"workq_kernreturn"),
	/* Mach traps (DARWIN_MACH_BASE + n) */
	E(DARWIN_MACH_BASE + 10, 4, darwin_mach_vm_allocate,	"_kernelrpc_mach_vm_allocate_trap"),
	E(DARWIN_MACH_BASE + 12, 3, darwin_mach_vm_deallocate,	"_kernelrpc_mach_vm_deallocate_trap"),
	E(DARWIN_MACH_BASE + 14, 5, darwin_mach_vm_protect,	"_kernelrpc_mach_vm_protect_trap"),
	E(DARWIN_MACH_BASE + 15, 6, darwin_mach_vm_map,		"_kernelrpc_mach_vm_map_trap"),
	E(DARWIN_MACH_BASE + 16, 3, darwin_mach_port_allocate,	"_kernelrpc_mach_port_allocate_trap"),
	E(DARWIN_MACH_BASE + 18, 2, darwin_mach_port_deallocate,	"_kernelrpc_mach_port_deallocate_trap"),
	E(DARWIN_MACH_BASE + 19, 4, darwin_mach_port_destroy,	"_kernelrpc_mach_port_mod_refs_trap"),
	E(DARWIN_MACH_BASE + 21, 4, darwin_mach_port_insert_right,	"_kernelrpc_mach_port_insert_right_trap"),
	E(DARWIN_MACH_BASE + 26, 0, darwin_mach_reply_port,	"mach_reply_port"),
	E(DARWIN_MACH_BASE + 27, 0, darwin_mach_thread_self,	"thread_self_trap"),
	E(DARWIN_MACH_BASE + 28, 0, darwin_mach_task_self,	"task_self_trap"),
	E(DARWIN_MACH_BASE + 29, 0, darwin_mach_host_self,	"host_self_trap"),
	E(DARWIN_MACH_BASE + 31, 7, darwin_mach_msg,		"mach_msg_trap"),
	E(DARWIN_MACH_BASE + 32, 9, darwin_mach_msg,		"mach_msg_overwrite_trap"),
	E(DARWIN_MACH_BASE + 33, 1, darwin_mach_unsupported,	"semaphore_signal_trap"),
	E(DARWIN_MACH_BASE + 34, 1, darwin_mach_unsupported,	"semaphore_signal_all_trap"),
	E(DARWIN_MACH_BASE + 36, 1, darwin_mach_unsupported,	"semaphore_wait_trap"),
	E(DARWIN_MACH_BASE + 38, 3, darwin_mach_unsupported,	"semaphore_timedwait_trap"),
	E(DARWIN_MACH_BASE + 89, 1, darwin_mach_timebase_info,	"mach_timebase_info_trap"),
	E(DARWIN_MACH_BASE + 90, 1, darwin_mach_wait_until,	"mach_wait_until_trap"),
#undef E
};

void
darwin_sync_syscalls_init(void)
{
	u_int i;

	for (i = 0; i < nitems(darwin_sync); i++) {
		int n = darwin_sync[i].num;

		darwin_sysent[n] = (struct sysent){ .sy_narg = darwin_sync[i].nargs, .sy_call = darwin_sync[i].fn,
		    .sy_auevent = AUE_NULL, .sy_thrcnt = SY_THR_STATIC };
		darwin_syscallnames[n] = darwin_sync[i].name;
	}
}
