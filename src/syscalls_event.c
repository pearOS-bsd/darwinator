/*
 * The BSD class, continued: kqueue/kevent/kevent64 (Darwin's event structures and filter numbers), the signal calls
 * (sigaction with its trampoline argument, sigaltstack, sigsuspend, sigpending), itimers, getfsstat64, mknod (the
 * device numbers of Darwin) and the AF_INET6 socket addresses (family 30 on Darwin, 28 here).
 *
 * Signals: a handler is called by FreeBSD's signal trampoline (the page the kernel maps for the process), with
 * the arguments the Darwin handler expects: (sig, siginfo*, ucontext*).  The siginfo has the same fields at the same
 * places up to si_addr; the ucontext is FreeBSD's, not Darwin's.  The trampoline address of Darwin's sigaction
 * (sa_tramp) is not used: the trampoline of the kernel returns with FreeBSD's sigreturn, which darwin_fetch_syscall_args
 * lets through.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/signalvar.h>
#include <sys/socket.h>
#include <sys/mbuf.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"
#include "darwin_abi.h"

/* ---- mknod: dev_t of Darwin is major << 24 | minor (24 bits) ---- */

dev_t
darwin_to_dev(int32_t d)
{
	return (makedev((d >> 24) & 0xff, d & 0xffffff));
}

int32_t
darwin_from_dev(dev_t d)
{
	return ((int32_t)((major(d) & 0xff) << 24 | (minor(d) & 0xffffff)));
}

static int
darwin_mknod(struct thread *td, struct darwin_args *uap)
{
	return (kern_mknodat(td, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, (int)uap->a[1],
	    darwin_to_dev((int32_t)uap->a[2])));
}

/* ---- getfsstat64(buf, bufsize, flags): the count, or the structures that fit ---- */

static int
darwin_getfsstat64(struct thread *td, struct darwin_args *uap)
{
	struct statfs *sb = NULL;
	size_t count = 0, maxn = 0, i;
	int error, mode = (int)uap->a[2];
	char *ubuf = (char *)uap->a[0];

	if (ubuf != NULL && (int)uap->a[1] > 0)
		maxn = (size_t)(int)uap->a[1] / sizeof(struct darwin_statfs64);
	if (ubuf == NULL) {
		error = kern_getfsstat(td, &sb, 0, &count, UIO_SYSSPACE, mode == 1 ? MNT_WAIT : MNT_NOWAIT);
		if (error == 0)
			td->td_retval[0] = count;
		return (error);
	}
	error = kern_getfsstat(td, &sb, maxn * sizeof(struct statfs), &count, UIO_SYSSPACE,
	    mode == 1 ? MNT_WAIT : MNT_NOWAIT);
	if (error == 0) {
		for (i = 0; i < count && error == 0; i++)
			error = darwin_statfs_out(&sb[i], ubuf + i * sizeof(struct darwin_statfs64));
		if (error == 0)
			td->td_retval[0] = count;
	}
	if (sb != NULL)
		free(sb, M_STATFS);
	return (error);
}

/* ---- kqueue, kevent, kevent64 ---- */

/* Darwin: struct kevent is 32 bytes, kevent64_s is 48 (two more extension words) */
struct dkev {
	uint64_t	ident;
	int16_t		filter;
	uint16_t	flags;
	uint32_t	fflags;
	int64_t		data;
	uint64_t	udata;
	uint64_t	ext[2];		/* kevent64 only */
};

struct dkev_arg {
	char	*in, *out;		/* the user arrays, advanced as the kernel takes and gives events */
	size_t	size;			/* 32 or 48 */
};

/* Darwin's filters: -10 is EVFILT_USER (FreeBSD -11), -8 (Mach ports) and -12 (VM) have no counterpart */
static int
filter_from_darwin(int f)
{
	switch (f) {
	case -1: case -2: case -3: case -4: case -5: case -6: case -7: case -9:
		return (f);
	case -10:
		return (EVFILT_USER);
	default:
		return (0x7000);		/* unknown to the kernel: the change comes back as an EV_ERROR event */
	}
}

static int
filter_to_darwin(int f)
{
	return (f == EVFILT_USER ? -10 : f);
}

static uint32_t
timer_fflags_from_darwin(uint32_t f)
{
	uint32_t r = f & ~0x7;		/* NOTE_SECONDS 1 is the same; Darwin's USECONDS 2 and NSECONDS 4 are 4 and 8 here */

	if (f & 1) r |= NOTE_SECONDS;
	if (f & 2) r |= NOTE_USECONDS;
	if (f & 4) r |= NOTE_NSECONDS;
	return (r);
}

static int
darwin_kev_copyin(void *argp, struct kevent *kevp, int count)
{
	struct dkev_arg *a = argp;
	struct dkev d;
	int i, error;

	for (i = 0; i < count; i++) {
		memset(&d, 0, sizeof(d));
		error = copyin(a->in, &d, a->size);
		if (error != 0)
			return (error);
		a->in += a->size;
		memset(&kevp[i], 0, sizeof(kevp[i]));
		kevp[i].ident = d.ident;
		kevp[i].filter = filter_from_darwin(d.filter);
		kevp[i].flags = d.flags & 0xff;		/* EV_UDATA_SPECIFIC (0x100) has no counterpart; 0x100 here means something else */
		kevp[i].fflags = kevp[i].filter == EVFILT_TIMER ? timer_fflags_from_darwin(d.fflags) : d.fflags;
		kevp[i].data = d.data;
		kevp[i].udata = (void *)d.udata;
		kevp[i].ext[0] = d.ext[0];
		kevp[i].ext[1] = d.ext[1];
	}
	return (0);
}

static int
darwin_kev_copyout(void *argp, struct kevent *kevp, int count)
{
	struct dkev_arg *a = argp;
	struct dkev d;
	int i, error;

	for (i = 0; i < count; i++) {
		memset(&d, 0, sizeof(d));
		d.ident = kevp[i].ident;
		d.filter = filter_to_darwin(kevp[i].filter);
		d.flags = kevp[i].flags;
		d.fflags = kevp[i].fflags;
		d.data = kevp[i].data;
		d.udata = (uint64_t)kevp[i].udata;
		d.ext[0] = kevp[i].ext[0];
		d.ext[1] = kevp[i].ext[1];
		error = copyout(&d, a->out, a->size);
		if (error != 0)
			return (error);
		a->out += a->size;
	}
	return (0);
}

static int
darwin_kevent_common(struct thread *td, int kq, char *chg, int nchg, char *evt, int nevt, const void *utimeout,
    size_t size)
{
	struct dkev_arg a = { .in = chg, .out = evt, .size = size };
	struct kevent_copyops ops = { .arg = &a, .k_copyout = darwin_kev_copyout, .k_copyin = darwin_kev_copyin,
	    .kevent_size = sizeof(struct kevent) };
	struct timespec ts, *tsp = NULL;
	int error;

	if (utimeout != NULL) {
		error = copyin(utimeout, &ts, sizeof(ts));
		if (error != 0)
			return (error);
		tsp = &ts;
	}
	return (kern_kevent(td, kq, nchg, nevt, &ops, tsp));
}

static int
darwin_kevent(struct thread *td, struct darwin_args *uap)
{
	return (darwin_kevent_common(td, (int)uap->a[0], (char *)uap->a[1], (int)uap->a[2], (char *)uap->a[3],
	    (int)uap->a[4], (const void *)uap->a[5], 32));
}

static int
darwin_kevent64(struct thread *td, struct darwin_args *uap)
{
	return (darwin_kevent_common(td, (int)uap->a[0], (char *)uap->a[1], (int)uap->a[2], (char *)uap->a[3],
	    (int)uap->a[4], (const void *)uap->a[6], 48));
}

/* ---- signals ---- */

struct darwin_sigaction {	/* what the sigaction call takes: with the trampoline */
	uint64_t	handler;
	uint64_t	tramp;
	uint32_t	mask;
	int32_t		flags;
};

struct darwin_osigaction {	/* what it gives back */
	uint64_t	handler;
	uint32_t	mask;
	int32_t		flags;
};

static void
mask_from_darwin(uint32_t m, sigset_t *set)
{
	int i;

	SIGEMPTYSET(*set);
	for (i = 1; i <= 32; i++)
		if (m & (1U << (i - 1)))
			SIGADDSET(*set, i);
}

static uint32_t
mask_to_darwin(const sigset_t *set)
{
	uint32_t m = 0;
	int i;

	for (i = 1; i <= 32; i++)
		if (SIGISMEMBER(*set, i))
			m |= 1U << (i - 1);
	return (m);
}

static int
darwin_sigaction(struct thread *td, struct darwin_args *uap)
{
	struct darwin_sigaction dn;
	struct darwin_osigaction dold;
	struct sigaction nact, oact, *np = NULL;
	int error, sig = (int)uap->a[0];

	if (sig <= 0 || sig > 32)
		return (EINVAL);
	if (uap->a[1] != 0) {
		error = copyin((void *)uap->a[1], &dn, sizeof(dn));
		if (error != 0)
			return (error);
		memset(&nact, 0, sizeof(nact));
		nact.sa_handler = (void (*)(int))dn.handler;
		nact.sa_flags = dn.flags & 0xff;	/* SA_ONSTACK ... SA_SIGINFO have the same values; Darwin's SA_USERTRAMP and SA_64REGSET (0x100, 0x200) are internal */
		mask_from_darwin(dn.mask, &nact.sa_mask);
		np = &nact;
	}
	error = kern_sigaction(td, sig, np, &oact, 0);
	if (error != 0)
		return (error);
	if (uap->a[2] != 0) {
		dold.handler = (uint64_t)oact.sa_handler;
		dold.mask = mask_to_darwin(&oact.sa_mask);
		dold.flags = oact.sa_flags & 0xff;
		error = copyout(&dold, (void *)uap->a[2], sizeof(dold));
	}
	return (error);
}

static int
darwin_sigsuspend(struct thread *td, struct darwin_args *uap)	/* the mask is the argument itself (32 bits) */
{
	sigset_t set;

	mask_from_darwin((uint32_t)uap->a[0], &set);
	return (kern_sigsuspend(td, set));
}

static int
darwin_sigpending(struct thread *td, struct darwin_args *uap)
{
	sigset_t pending;
	uint32_t m;
	struct proc *p = td->td_proc;

	PROC_LOCK(p);
	pending = td->td_siglist;
	SIGSETOR(pending, p->p_siglist);
	PROC_UNLOCK(p);
	m = mask_to_darwin(&pending);
	return (copyout(&m, (void *)uap->a[0], sizeof(m)));
}

/* ---- itimers: the structures are the same ---- */

static int
darwin_setitimer(struct thread *td, struct darwin_args *uap)
{
	struct itimerval nv, ov;
	int error;

	if (uap->a[1] == 0)
		return (EFAULT);
	error = copyin((void *)uap->a[1], &nv, sizeof(nv));
	if (error != 0)
		return (error);
	error = kern_setitimer(td, (u_int)uap->a[0], &nv, &ov);
	if (error == 0 && uap->a[2] != 0)
		error = copyout(&ov, (void *)uap->a[2], sizeof(ov));
	return (error);
}

static int
darwin_getitimer(struct thread *td, struct darwin_args *uap)
{
	struct itimerval v;
	int error = kern_getitimer(td, (u_int)uap->a[0], &v);

	return (error != 0 ? error : copyout(&v, (void *)uap->a[1], sizeof(v)));
}

/* ---- AF_INET6: the family byte of a socket address is 30 on Darwin, 28 here ---- */

static int
darwin_sockaddr_in(struct thread *td, struct darwin_args *uap, struct sockaddr **sa)
{
	struct sockaddr *s;
	int error;

	error = getsockaddr(&s, (const struct sockaddr *)uap->a[1], (size_t)uap->a[2]);
	if (error != 0)
		return (error);
	if (s->sa_family == DARWIN_AF_INET6)
		s->sa_family = AF_INET6;
	*sa = s;
	return (0);
}

static int
darwin_bind(struct thread *td, struct darwin_args *uap)
{
	struct sockaddr *sa;
	int error = darwin_sockaddr_in(td, uap, &sa);

	if (error != 0)
		return (error);
	error = kern_bindat(td, AT_FDCWD, (int)uap->a[0], sa);
	free(sa, M_SONAME);
	return (error);
}

static int
darwin_connect(struct thread *td, struct darwin_args *uap)
{
	struct sockaddr *sa;
	int error = darwin_sockaddr_in(td, uap, &sa);

	if (error != 0)
		return (error);
	error = kern_connectat(td, AT_FDCWD, (int)uap->a[0], sa);
	free(sa, M_SONAME);
	return (error);
}

/* after a call that gave an address back (accept, getsockname, getpeername, recvfrom): family 28 -> 30 */
static void
darwin_fix_family_out(void *uaddr, void *ulen)
{
	uint32_t len;
	uint8_t fam;

	if (uaddr == NULL || ulen == NULL || copyin(ulen, &len, sizeof(len)) != 0 || len < 2)
		return;
	if (copyin((char *)uaddr + 1, &fam, 1) == 0 && fam == AF_INET6) {
		fam = DARWIN_AF_INET6;
		(void)copyout(&fam, (char *)uaddr + 1, 1);
	}
}

static int
darwin_accept(struct thread *td, struct darwin_args *uap)
{
	int error = sys_accept(td, (struct accept_args *)uap);

	if (error == 0)
		darwin_fix_family_out((void *)uap->a[1], (void *)uap->a[2]);
	return (error);
}

static int
darwin_getsockname(struct thread *td, struct darwin_args *uap)
{
	int error = sys_getsockname(td, (struct getsockname_args *)uap);

	if (error == 0)
		darwin_fix_family_out((void *)uap->a[1], (void *)uap->a[2]);
	return (error);
}

static int
darwin_getpeername(struct thread *td, struct darwin_args *uap)
{
	int error = sys_getpeername(td, (struct getpeername_args *)uap);

	if (error == 0)
		darwin_fix_family_out((void *)uap->a[1], (void *)uap->a[2]);
	return (error);
}

static int
darwin_recvfrom(struct thread *td, struct darwin_args *uap)
{
	int error = sys_recvfrom(td, (struct recvfrom_args *)uap);

	if (error == 0)
		darwin_fix_family_out((void *)uap->a[4], (void *)uap->a[5]);
	return (error);
}

/* ---- the entries ---- */

static const struct {
	int		num, nargs;
	sy_call_t	*fn;
	int		auevent;
	const char	*name;
} darwin_event[] = {
#define	E(num, nargs, fn, ev, name)	{ (num), (nargs), (sy_call_t *)(fn), (ev), (name) }
	E(14,  3, darwin_mknod,		AUE_MKNOD,		"mknod"),
	E(347, 3, darwin_getfsstat64,	AUE_GETFSSTAT,		"getfsstat64"),
	E(362, 0, sys_kqueue,		AUE_KQUEUE,		"kqueue"),
	E(363, 6, darwin_kevent,	AUE_NULL,		"kevent"),
	E(369, 7, darwin_kevent64,	AUE_NULL,		"kevent64"),
	E(46,  3, darwin_sigaction,	AUE_SIGACTION,		"sigaction"),
	E(53,  2, sys_sigaltstack,	AUE_SIGALTSTACK,	"sigaltstack"),
	E(52,  1, darwin_sigpending,	AUE_SIGPENDING,		"sigpending"),
	E(111, 1, darwin_sigsuspend,	AUE_SIGSUSPEND,		"sigsuspend"),
	E(83,  3, darwin_setitimer,	AUE_SETITIMER,		"setitimer"),
	E(86,  2, darwin_getitimer,	AUE_GETITIMER,		"getitimer"),
	/* these replace the entries of syscalls_more.c (the address families) */
	E(104, 3, darwin_bind,		AUE_BIND,		"bind"),
	E(98,  3, darwin_connect,	AUE_CONNECT,		"connect"),
	E(409, 3, darwin_connect,	AUE_CONNECT,		"connect_nocancel"),
	E(30,  3, darwin_accept,	AUE_ACCEPT,		"accept"),
	E(404, 3, darwin_accept,	AUE_ACCEPT,		"accept_nocancel"),
	E(32,  3, darwin_getsockname,	AUE_GETSOCKNAME,	"getsockname"),
	E(31,  3, darwin_getpeername,	AUE_GETPEERNAME,	"getpeername"),
	E(29,  6, darwin_recvfrom,	AUE_RECVFROM,		"recvfrom"),
	E(403, 6, darwin_recvfrom,	AUE_RECVFROM,		"recvfrom_nocancel"),
#undef E
};

void
darwin_event_syscalls_init(void)
{
	u_int i;

	for (i = 0; i < nitems(darwin_event); i++) {
		int n = darwin_event[i].num;

		darwin_sysent[n] = (struct sysent){ .sy_narg = darwin_event[i].nargs, .sy_call = darwin_event[i].fn,
		    .sy_auevent = darwin_event[i].auevent, .sy_thrcnt = SY_THR_STATIC };
		darwin_syscallnames[n] = darwin_event[i].name;
	}
}
