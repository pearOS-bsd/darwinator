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
	if (error != 0 && darwin_debug >= 2)
		DPRINTF("%s[%d]: vm trap failed: error %d\n", td->td_proc->p_comm, td->td_proc->p_pid, error);
	td->td_retval[0] = error == 0 ? KERN_SUCCESS : error == EFAULT ? KERN_INVALID_ADDRESS : KERN_INVALID_ARGUMENT;
	return (0);
}

#define	VM_FLAGS_OVERWRITE	0x4000

/* anonymous memory: at the address (fixed; with VM_FLAGS_OVERWRITE over what is there), or anywhere, aligned to mask + 1 (a power of two) when a mask is given */
static int
mach_anon(struct thread *td, uint64_t *addrp, size_t size, int flags, int prot, uint64_t mask)
{
	struct mmap_args ma;
	uint64_t addr = 0, align = mask + 1, a2;
	int error;

	if ((error = copyin((void *)addrp, &addr, sizeof(addr))) != 0)
		return (error);
	ma.prot = prot;
	ma.fd = -1;
	ma.pos = 0;
	if (!(flags & VM_FLAGS_ANYWHERE)) {
		ma.addr = (void *)addr;
		ma.len = size;
		ma.flags = MAP_ANON | MAP_PRIVATE | MAP_FIXED | ((flags & VM_FLAGS_OVERWRITE) ? 0 : MAP_EXCL);
		if ((error = sys_mmap(td, &ma)) != 0)
			return (error);
		return (0);
	}
	if (mask != 0 && (align & (align - 1)) == 0 && align > PAGE_SIZE) {
		ma.addr = NULL;
		ma.len = size + align;
		ma.flags = MAP_ANON | MAP_PRIVATE;
		if ((error = sys_mmap(td, &ma)) != 0)
			return (error);
		addr = (uint64_t)td->td_retval[0];
		a2 = roundup2(addr, align);
		if (a2 > addr) {					/* the part before the aligned start, and the part after the end */
			struct munmap_args mu = { .addr = (void *)addr, .len = a2 - addr };

			(void)sys_munmap(td, &mu);
		}
		if (addr + size + align > a2 + size) {
			struct munmap_args mu = { .addr = (void *)(a2 + size), .len = addr + size + align - (a2 + size) };

			(void)sys_munmap(td, &mu);
		}
		addr = a2;
		return (copyout(&addr, (void *)addrp, sizeof(addr)));
	}
	ma.addr = NULL;
	ma.len = size;
	ma.flags = MAP_ANON | MAP_PRIVATE;
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
	return (kr_of(td, mach_anon(td, (uint64_t *)uap->a[1], (size_t)uap->a[2], (int)uap->a[4], (int)uap->a[5] & 7, uap->a[3])));
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
	E(297, 5, darwin_psynch_rw_wait,	"psynch_rw_longrdlock"),
	E(298, 5, darwin_psynch_rw_wait,	"psynch_rw_yieldwrlock"),
	E(299, 5, darwin_psynch_rw_unlock,	"psynch_rw_downgrade"),
	E(300, 5, darwin_psynch_rw_wait,	"psynch_rw_upgrade"),
	E(367, 0, darwin_workq_open,		"workq_open"),
	E(368, 4, darwin_workq_kernreturn,	"workq_kernreturn"),
	/* Mach traps (DARWIN_MACH_BASE + n) */
	E(DARWIN_MACH_BASE + 10, 4, darwin_mach_vm_allocate,	"_kernelrpc_mach_vm_allocate_trap"),
	E(DARWIN_MACH_BASE + 12, 3, darwin_mach_vm_deallocate,	"_kernelrpc_mach_vm_deallocate_trap"),
	E(DARWIN_MACH_BASE + 14, 5, darwin_mach_vm_protect,	"_kernelrpc_mach_vm_protect_trap"),
	E(DARWIN_MACH_BASE + 15, 6, darwin_mach_vm_map,		"_kernelrpc_mach_vm_map_trap"),
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
