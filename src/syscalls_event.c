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
		if (a->size == 72) {			/* kevent_qos_s: ident 0, filter 8, flags 10, qos 12, udata 16, fflags 24, xflags 28, data 32, ext[4] 40 */
			char raw[72];

			if ((error = copyin(a->in, raw, 72)) != 0)
				return (error);
			memcpy(&d.ident, raw, 8); memcpy(&d.filter, raw + 8, 2); memcpy(&d.flags, raw + 10, 2); memcpy(&d.udata, raw + 16, 8);
			memcpy(&d.fflags, raw + 24, 4); memcpy(&d.data, raw + 32, 8); memcpy(d.ext, raw + 40, 16);
		} else if ((error = copyin(a->in, &d, a->size)) != 0)
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
		if (a->size == 72) {
			char raw[72];

			memset(raw, 0, sizeof(raw));
			memcpy(raw, &d.ident, 8); memcpy(raw + 8, &d.filter, 2); memcpy(raw + 10, &d.flags, 2); memcpy(raw + 16, &d.udata, 8);
			memcpy(raw + 24, &d.fflags, 4); memcpy(raw + 32, &d.data, 8); memcpy(raw + 40, d.ext, 16);
			error = copyout(raw, a->out, 72);
		} else
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


/* kevent_qos(fd, changelist, nchanges, eventlist, nevents, data_out, data_available, flags) and kevent_id(id, ... same): the events are 72 bytes; fd -1 and the ids of workloops are
 * kqueues of the process that are made when first used (a descriptor that the program does not know of).  There is no timeout: KEVENT_FLAG_IMMEDIATE (1) asks not to wait. */
static struct { struct proc *p; pid_t pid; int64_t id; int fd; } kq_tab[64];
static struct mtx kq_mtx;
MTX_SYSINIT(darwin_kq, &kq_mtx, "darwin kq", MTX_DEF);

static int
kq_for(struct thread *td, int64_t id, int *fdp)
{
	struct proc *p = td->td_proc;
	int i, free_i = -1, error;
	struct kqueue_args ka;

	mtx_lock(&kq_mtx);
	for (i = 0; i < (int)nitems(kq_tab); i++) {
		if (kq_tab[i].p == p && kq_tab[i].pid == p->p_pid && kq_tab[i].id == id) {
			*fdp = kq_tab[i].fd;
			mtx_unlock(&kq_mtx);
			return (0);
		}
		if (kq_tab[i].p == NULL && free_i < 0)
			free_i = i;
	}
	mtx_unlock(&kq_mtx);
	if (free_i < 0)
		return (EMFILE);
	if ((error = sys_kqueue(td, &ka)) != 0)
		return (error);
	mtx_lock(&kq_mtx);
	kq_tab[free_i].p = p; kq_tab[free_i].pid = p->p_pid; kq_tab[free_i].id = id; kq_tab[free_i].fd = td->td_retval[0];
	*fdp = td->td_retval[0];
	mtx_unlock(&kq_mtx);
	return (0);
}

static int
darwin_kevent_qos_common(struct thread *td, struct darwin_args *uap, bool byid)
{
	int fd = byid ? 0 : (int)uap->a[0], error;
	uint32_t flags = (uint32_t)uap->a[7];
	struct timespec zero = { 0, 0 };
	struct dkev_arg a = { .in = (char *)uap->a[1], .out = (char *)uap->a[3], .size = 72 };
	struct kevent_copyops ops = { .arg = &a, .k_copyout = darwin_kev_copyout, .k_copyin = darwin_kev_copyin, .kevent_size = sizeof(struct kevent) };

	if (byid && (int)uap->a[2] > 0) {		/* EVFILT_WORKLOOP (-10) changes make or configure the workloop itself: there is no kernel workqueue here, so they succeed without events */
		int16_t filt;

		if (copyin(a.in + 8, &filt, sizeof(filt)) == 0 && filt == -10) {
			td->td_retval[0] = 0;
			return (0);
		}
	}
	if (byid || fd == -1) {
		if ((error = kq_for(td, byid ? (int64_t)uap->a[0] : -1, &fd)) != 0)
			return (error);
	}
	return (kern_kevent(td, fd, (int)uap->a[2], (int)uap->a[4], &ops, (flags & 1) ? &zero : NULL));
}

static int darwin_kevent_qos(struct thread *td, struct darwin_args *uap) { return (darwin_kevent_qos_common(td, uap, false)); }
static int darwin_kevent_id(struct thread *td, struct darwin_args *uap) { return (darwin_kevent_qos_common(td, uap, true)); }

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


/* ---- getattrlist(path, attrlist, buffer, size, options): the common and file attributes that come from stat ---- */

struct darwin_attrlist {
	uint16_t	bitmapcount, reserved;
	uint32_t	commonattr, volattr, dirattr, fileattr, forkattr;
};

#define	DA_CMN_NAME		0x00000001
#define	DA_CMN_DEVID		0x00000002
#define	DA_CMN_FSID		0x00000004
#define	DA_CMN_OBJTYPE		0x00000008
#define	DA_CMN_OBJTAG		0x00000010
#define	DA_CMN_OBJID		0x00000020
#define	DA_CMN_CRTIME		0x00000200
#define	DA_CMN_MODTIME		0x00000400
#define	DA_CMN_CHGTIME		0x00000800
#define	DA_CMN_ACCTIME		0x00001000
#define	DA_CMN_OWNERID		0x00008000
#define	DA_CMN_GRPID		0x00010000
#define	DA_CMN_ACCESSMASK	0x00020000
#define	DA_CMN_FLAGS		0x00040000
#define	DA_CMN_FILEID		0x02000000
#define	DA_CMN_SUPPORTED	(DA_CMN_NAME | DA_CMN_DEVID | DA_CMN_FSID | DA_CMN_OBJTYPE | DA_CMN_OBJTAG | DA_CMN_OBJID | \
	DA_CMN_CRTIME | DA_CMN_MODTIME | DA_CMN_CHGTIME | DA_CMN_ACCTIME | DA_CMN_OWNERID | DA_CMN_GRPID | \
	DA_CMN_ACCESSMASK | DA_CMN_FLAGS | DA_CMN_FILEID)
#define	DA_FILE_LINKCOUNT	0x00000001
#define	DA_FILE_TOTALSIZE	0x00000002
#define	DA_FILE_ALLOCSIZE	0x00000004
#define	DA_FILE_IOBLOCKSIZE	0x00000008
#define	DA_FILE_DATALENGTH	0x00000200
#define	DA_FILE_DATAALLOCSIZE	0x00000400
#define	DA_FILE_SUPPORTED	(DA_FILE_LINKCOUNT | DA_FILE_TOTALSIZE | DA_FILE_ALLOCSIZE | DA_FILE_IOBLOCKSIZE | \
	DA_FILE_DATALENGTH | DA_FILE_DATAALLOCSIZE)

static int
attr_fill(struct thread *td, struct darwin_args *uap, const struct stat *sbp, const char *path)
{
	struct darwin_attrlist al;
	struct stat sb = *sbp;
	char *out, *p;
	const char *name;
	size_t bufsize = (size_t)uap->a[3], len, namelen, copy;
	uint32_t cmn, file, objtype;
	int error;

	error = copyin((void *)uap->a[1], &al, sizeof(al));
	if (error != 0)
		return (error);
	if (al.bitmapcount != 5)
		return (EINVAL);
	cmn = al.commonattr;
	file = al.fileattr;
	if ((cmn & ~DA_CMN_SUPPORTED) || al.volattr || al.dirattr || al.forkattr || (file & ~DA_FILE_SUPPORTED))
		return (EINVAL);
	if (S_ISDIR(sb.st_mode) && file != 0)
		return (EINVAL);			/* file attributes of a directory */
	name = strrchr(path, '/');
	name = (name != NULL && name[1] != '\0') ? name + 1 : path;
	namelen = strlen(name) + 1;
	out = malloc(4 + 256 + roundup2(namelen, 4) + 8, M_DARWIN, M_WAITOK | M_ZERO);
	p = out + 4;				/* the length comes first */
#define	PUT32(v)	do { uint32_t t_ = (uint32_t)(v); memcpy(p, &t_, 4); p += 4; } while (0)
#define	PUT64(v)	do { uint64_t t_ = (uint64_t)(v); memcpy(p, &t_, 8); p += 8; } while (0)
#define	PUTTS(ts)	do { PUT64((ts).tv_sec); PUT64((ts).tv_nsec); } while (0)
	switch (sb.st_mode & S_IFMT) {
	case S_IFREG: objtype = 1; break;
	case S_IFDIR: objtype = 2; break;
	case S_IFBLK: objtype = 3; break;
	case S_IFCHR: objtype = 4; break;
	case S_IFLNK: objtype = 5; break;
	case S_IFSOCK: objtype = 6; break;
	default: objtype = 7; break;		/* FIFO */
	}
	{
		char *nameref = NULL;

		if (cmn & DA_CMN_NAME) { nameref = p; p += 8; }		/* attrreference: offset (from itself), length */
		if (cmn & DA_CMN_DEVID)		PUT32(darwin_from_dev(sb.st_dev));
		if (cmn & DA_CMN_FSID)		{ PUT32(darwin_from_dev(sb.st_dev)); PUT32(0); }
		if (cmn & DA_CMN_OBJTYPE)	PUT32(objtype);
		if (cmn & DA_CMN_OBJTAG)	PUT32(0);
		if (cmn & DA_CMN_OBJID)		{ PUT32(sb.st_ino); PUT32(sb.st_gen); }
		if (cmn & DA_CMN_CRTIME)	PUTTS(sb.st_birthtim);
		if (cmn & DA_CMN_MODTIME)	PUTTS(sb.st_mtim);
		if (cmn & DA_CMN_CHGTIME)	PUTTS(sb.st_ctim);
		if (cmn & DA_CMN_ACCTIME)	PUTTS(sb.st_atim);
		if (cmn & DA_CMN_OWNERID)	PUT32(sb.st_uid);
		if (cmn & DA_CMN_GRPID)		PUT32(sb.st_gid);
		if (cmn & DA_CMN_ACCESSMASK)	PUT32(sb.st_mode);
		if (cmn & DA_CMN_FLAGS)		PUT32(sb.st_flags);
		if (cmn & DA_CMN_FILEID)	PUT64(sb.st_ino);
		if (file & DA_FILE_LINKCOUNT)	PUT32(sb.st_nlink);
		if (file & DA_FILE_TOTALSIZE)	PUT64(sb.st_size);
		if (file & DA_FILE_ALLOCSIZE)	PUT64(sb.st_blocks * 512);
		if (file & DA_FILE_IOBLOCKSIZE)	PUT32(sb.st_blksize);
		if (file & DA_FILE_DATALENGTH)	PUT64(sb.st_size);
		if (file & DA_FILE_DATAALLOCSIZE) PUT64(sb.st_blocks * 512);
		if (nameref != NULL) {		/* the name follows the fixed part, padded to 4 */
			int32_t off = (int32_t)(p - nameref);
			uint32_t l = (uint32_t)namelen;

			memcpy(nameref, &off, 4);
			memcpy(nameref + 4, &l, 4);
			memcpy(p, name, namelen);
			p += roundup2(namelen, 4);
		}
	}
	len = p - out;
	{
		uint32_t total = (uint32_t)len;

		memcpy(out, &total, 4);
	}
	copy = len < bufsize ? len : bufsize;
	error = copyout(out, (void *)uap->a[2], copy);
	free(out, M_DARWIN);
	return (error);
}


static int
darwin_getattrlist(struct thread *td, struct darwin_args *uap)
{
	char path[MAXPATHLEN];
	struct stat sb;
	int error = copyinstr((void *)uap->a[0], path, sizeof(path), NULL);

	if (error != 0)
		return (error);
	error = kern_statat(td, ((uint32_t)uap->a[4] & 1) ? AT_SYMLINK_NOFOLLOW : 0, AT_FDCWD, path, UIO_SYSSPACE, &sb);
	return (error != 0 ? error : attr_fill(td, uap, &sb, path));
}

/* getattrlistat(fd, path, alist, buffer, size, options): relative to a directory */
static int
darwin_getattrlistat(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;
	char path[MAXPATHLEN];
	struct stat sb;
	int error = copyinstr((void *)uap->a[1], path, sizeof(path), NULL);

	if (error != 0)
		return (error);
	a.a[1] = uap->a[2]; a.a[2] = uap->a[3]; a.a[3] = uap->a[4]; a.a[4] = uap->a[5];
	error = kern_statat(td, ((uint32_t)uap->a[5] & 1) ? AT_SYMLINK_NOFOLLOW : 0, (int)uap->a[0] == -2 ? AT_FDCWD : (int)uap->a[0], path, UIO_SYSSPACE, &sb);
	return (error != 0 ? error : attr_fill(td, &a, &sb, path));
}

/* fgetattrlist(fd, alist, buffer, size, options): the name comes from the path of the vnode when the kernel knows one */
static int
darwin_fgetattrlist(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;
	struct stat sb;
	int error = kern_fstat(td, (int)uap->a[0], &sb);

	a.a[0] = 0; a.a[1] = uap->a[1]; a.a[2] = uap->a[2]; a.a[3] = uap->a[3]; a.a[4] = uap->a[4];
	return (error != 0 ? error : attr_fill(td, &a, &sb, "?"));
}
/* sendto to an AF_INET6 address: the family byte of the program's sockaddr is 30; it is FreeBSD's 28 for the duration of the call (and put back) */
static int
darwin_sendto6(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;
	uint8_t fam = 0, fix = AF_INET6;
	char *addr = (char *)uap->a[4];
	bool swapped = false;
	int error;

	a.a[3] = darwin_msgflags_pub((int)a.a[3]);
	if (addr != NULL && uap->a[5] >= 2 && copyin(addr + 1, &fam, 1) == 0 && fam == DARWIN_AF_INET6 && copyout(&fix, addr + 1, 1) == 0)
		swapped = true;
	error = sys_sendto(td, (struct sendto_args *)&a);
	if (swapped) {
		uint8_t back = DARWIN_AF_INET6;

		(void)copyout(&back, addr + 1, 1);
	}
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
	E(220, 5, darwin_getattrlist,	AUE_GETATTRLIST,	"getattrlist"),
	E(228, 5, darwin_fgetattrlist,	AUE_GETATTRLIST,	"fgetattrlist"),
	E(476, 6, darwin_getattrlistat,	AUE_GETATTRLIST,	"getattrlistat"),
	E(410, 1, darwin_sigsuspend,	AUE_SIGSUSPEND,		"sigsuspend_nocancel"),
	E(14,  3, darwin_mknod,		AUE_MKNOD,		"mknod"),
	E(347, 3, darwin_getfsstat64,	AUE_GETFSSTAT,		"getfsstat64"),
	E(362, 0, sys_kqueue,		AUE_KQUEUE,		"kqueue"),
	E(363, 6, darwin_kevent,	AUE_NULL,		"kevent"),
	E(369, 7, darwin_kevent64,	AUE_NULL,		"kevent64"),
	E(374, 8, darwin_kevent_qos,	AUE_NULL,		"kevent_qos"),
	E(375, 8, darwin_kevent_id,	AUE_NULL,		"kevent_id"),
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
	E(133, 6, darwin_sendto6,	AUE_SENDTO,		"sendto"),
	E(413, 6, darwin_sendto6,	AUE_SENDTO,		"sendto_nocancel"),
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
