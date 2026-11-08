/*
 * The BSD class, continued: what a recent libSystem uses and the earlier files do not have.
 *   - the *at family (openat, fstatat64, faccessat, fchmodat, fchownat, linkat, unlinkat, readlinkat, symlinkat, mkdirat,
 *     renameat): AT_FDCWD is -2 on Darwin (-100 here) and the AT_* flags have other values;
 *   - extended attributes (getxattr ... flistxattr) on FreeBSD's extattr in the "user" namespace;
 *   - ulock_wait / ulock_wake (what os_unfair_lock and libdispatch wait on), on an address-keyed sleep;
 *   - thread_selfid, __pthread_kill (by thread id), csops, shared_region_check_np (there is no shared region: ENOMEM).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/extattr.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/uio.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"
#include "darwin_abi.h"

/* ---- the *at family ---- */

#define	DARWIN_AT_FDCWD			(-2)
#define	DARWIN_AT_EACCESS		0x0010
#define	DARWIN_AT_SYMLINK_NOFOLLOW	0x0020
#define	DARWIN_AT_SYMLINK_FOLLOW	0x0040
#define	DARWIN_AT_REMOVEDIR		0x0080

static int
dfd(int64_t fd)
{
	return ((int)fd == DARWIN_AT_FDCWD ? AT_FDCWD : (int)fd);
}

static int
dat(int64_t d)
{
	int f = 0;

	if (d & DARWIN_AT_EACCESS)		f |= AT_EACCESS;
	if (d & DARWIN_AT_SYMLINK_NOFOLLOW)	f |= AT_SYMLINK_NOFOLLOW;
	if (d & DARWIN_AT_SYMLINK_FOLLOW)	f |= AT_SYMLINK_FOLLOW;
	if (d & DARWIN_AT_REMOVEDIR)		f |= AT_REMOVEDIR;
	return (f);
}

static int
darwin_openat(struct thread *td, struct darwin_args *uap)
{
	return (kern_openat(td, dfd(uap->a[0]), (const char *)uap->a[1], UIO_USERSPACE, darwin_oflags((int)uap->a[2]),
	    (int)uap->a[3]));
}

static int
darwin_fstatat64(struct thread *td, struct darwin_args *uap)
{
	struct stat sb;
	int error = kern_statat(td, dat(uap->a[3]), dfd(uap->a[0]), (const char *)uap->a[1], UIO_USERSPACE, &sb);

	return (error != 0 ? error : darwin_stat_out(td, &sb, (void *)uap->a[2]));
}

static int
darwin_faccessat(struct thread *td, struct darwin_args *uap)
{
	return (kern_accessat(td, dfd(uap->a[0]), (const char *)uap->a[1], UIO_USERSPACE, dat(uap->a[3]), (int)uap->a[2]));
}

static int
darwin_fchmodat(struct thread *td, struct darwin_args *uap)
{
	return (kern_fchmodat(td, dfd(uap->a[0]), (const char *)uap->a[1], UIO_USERSPACE, (mode_t)uap->a[2], dat(uap->a[3])));
}

static int
darwin_fchownat(struct thread *td, struct darwin_args *uap)
{
	return (kern_fchownat(td, dfd(uap->a[0]), (const char *)uap->a[1], UIO_USERSPACE, (int)uap->a[2], (int)uap->a[3],
	    dat(uap->a[4])));
}

static int
darwin_linkat(struct thread *td, struct darwin_args *uap)
{
	return (kern_linkat(td, dfd(uap->a[0]), dfd(uap->a[2]), (const char *)uap->a[1], (const char *)uap->a[3], UIO_USERSPACE,
	    dat(uap->a[4])));
}

static int
darwin_unlinkat(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;

	a.a[0] = dfd(a.a[0]);
	a.a[2] = dat(a.a[2]);
	return (sys_unlinkat(td, (struct unlinkat_args *)&a));
}

static int
darwin_readlinkat(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;

	a.a[0] = dfd(a.a[0]);
	return (sys_readlinkat(td, (struct readlinkat_args *)&a));
}

static int
darwin_symlinkat(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;

	a.a[1] = dfd(a.a[1]);
	return (sys_symlinkat(td, (struct symlinkat_args *)&a));
}

static int
darwin_mkdirat(struct thread *td, struct darwin_args *uap)
{
	return (kern_mkdirat(td, dfd(uap->a[0]), (const char *)uap->a[1], UIO_USERSPACE, (int)uap->a[2]));
}

static int
darwin_renameat(struct thread *td, struct darwin_args *uap)
{
	return (kern_renameat(td, dfd(uap->a[0]), (const char *)uap->a[1], dfd(uap->a[2]), (const char *)uap->a[3], UIO_USERSPACE));
}

/* ---- extended attributes: Darwin's names are FreeBSD's names in the "user" namespace; XATTR_NOFOLLOW is 1 ---- */

#define	XATTR_NOFOLLOW	0x0001

/* kern_extattr_* take the attribute name in kernel memory and the data in user memory (they build a user uio) */
static int
xattr_name(const void *uname, char *name)
{
	return (copyinstr(uname, name, EXTATTR_MAXNAMELEN + 1, NULL));
}

static int
darwin_getxattr(struct thread *td, struct darwin_args *uap)
{
	char name[EXTATTR_MAXNAMELEN + 1];
	int error;

	if (uap->a[4] != 0)
		return (EINVAL);			/* a position: resource forks only */
	if ((error = xattr_name((const void *)uap->a[1], name)) != 0)
		return (error);
	return (kern_extattr_get_path(td, (const char *)uap->a[0], EXTATTR_NAMESPACE_USER, name, (void *)uap->a[2],
	    uap->a[2] == 0 ? 0 : (size_t)uap->a[3], (uap->a[5] & XATTR_NOFOLLOW) ? NOFOLLOW : FOLLOW, UIO_USERSPACE));
}

static int
darwin_fgetxattr(struct thread *td, struct darwin_args *uap)
{
	char name[EXTATTR_MAXNAMELEN + 1];
	int error;

	if (uap->a[4] != 0)
		return (EINVAL);
	if ((error = xattr_name((const void *)uap->a[1], name)) != 0)
		return (error);
	return (kern_extattr_get_fd(td, (int)uap->a[0], EXTATTR_NAMESPACE_USER, name, (void *)uap->a[2],
	    uap->a[2] == 0 ? 0 : (size_t)uap->a[3]));
}

static int
darwin_setxattr(struct thread *td, struct darwin_args *uap)
{
	char name[EXTATTR_MAXNAMELEN + 1];
	int error;

	if (uap->a[4] != 0)
		return (EINVAL);
	if ((error = xattr_name((const void *)uap->a[1], name)) != 0)
		return (error);
	error = kern_extattr_set_path(td, (const char *)uap->a[0], EXTATTR_NAMESPACE_USER, name, (void *)uap->a[2],
	    (size_t)uap->a[3], (uap->a[5] & XATTR_NOFOLLOW) ? NOFOLLOW : FOLLOW, UIO_USERSPACE);
	if (error == 0)
		td->td_retval[0] = 0;
	return (error);
}

static int
darwin_fsetxattr(struct thread *td, struct darwin_args *uap)
{
	char name[EXTATTR_MAXNAMELEN + 1];
	int error;

	if (uap->a[4] != 0)
		return (EINVAL);
	if ((error = xattr_name((const void *)uap->a[1], name)) != 0)
		return (error);
	error = kern_extattr_set_fd(td, (int)uap->a[0], EXTATTR_NAMESPACE_USER, name, (void *)uap->a[2], (size_t)uap->a[3]);
	if (error == 0)
		td->td_retval[0] = 0;
	return (error);
}

static int
darwin_removexattr(struct thread *td, struct darwin_args *uap)
{
	char name[EXTATTR_MAXNAMELEN + 1];
	int error;

	if ((error = xattr_name((const void *)uap->a[1], name)) != 0)
		return (error);
	error = kern_extattr_delete_path(td, (const char *)uap->a[0], EXTATTR_NAMESPACE_USER, name,
	    (uap->a[2] & XATTR_NOFOLLOW) ? NOFOLLOW : FOLLOW, UIO_USERSPACE);
	if (error == 0)
		td->td_retval[0] = 0;
	return (error);
}

static int
darwin_fremovexattr(struct thread *td, struct darwin_args *uap)
{
	char name[EXTATTR_MAXNAMELEN + 1];
	int error;

	if ((error = xattr_name((const void *)uap->a[1], name)) != 0)
		return (error);
	error = kern_extattr_delete_fd(td, (int)uap->a[0], EXTATTR_NAMESPACE_USER, name);
	if (error == 0)
		td->td_retval[0] = 0;
	return (error);
}

/* the names come from FreeBSD as length-prefixed strings, to Darwin as NUL terminated ones */
static int
xattr_list_out(struct thread *td, char *kbuf, size_t klen, void *ubuf, size_t usize)
{
	char *out;
	size_t i = 0, o = 0;
	int error = 0;

	out = malloc(MAX(klen, 1), M_DARWIN, M_WAITOK);
	while (i < klen) {
		size_t n = (unsigned char)kbuf[i++];

		if (i + n > klen)
			break;
		memcpy(out + o, kbuf + i, n);
		out[o + n] = '\0';
		o += n + 1;
		i += n;
	}
	if (ubuf != NULL) {
		if (usize < o)
			error = ERANGE;
		else
			error = copyout(out, ubuf, o);
	}
	if (error == 0)
		td->td_retval[0] = o;
	free(out, M_DARWIN);
	return (error);
}

static int
darwin_listxattr_common(struct thread *td, int fd, const char *path, void *ubuf, size_t usize, int options)
{
	struct iovec iov;
	struct uio auio;
	char *kbuf;
	size_t klen = 65536;
	int error;

	kbuf = malloc(klen, M_DARWIN, M_WAITOK);
	iov.iov_base = kbuf;
	iov.iov_len = klen;
	auio.uio_iov = &iov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = 0;
	auio.uio_resid = klen;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_td = td;
	if (path != NULL)
		error = kern_extattr_list_path(td, path, EXTATTR_NAMESPACE_USER, &auio, (options & XATTR_NOFOLLOW) ? NOFOLLOW : FOLLOW,
		    UIO_USERSPACE);
	else
		error = kern_extattr_list_fd(td, fd, EXTATTR_NAMESPACE_USER, &auio);
	if (error == 0)
		error = xattr_list_out(td, kbuf, klen - auio.uio_resid, ubuf, usize);
	free(kbuf, M_DARWIN);
	return (error);
}

static int
darwin_listxattr(struct thread *td, struct darwin_args *uap)
{
	return (darwin_listxattr_common(td, -1, (const char *)uap->a[0], (void *)uap->a[1], (size_t)uap->a[2], (int)uap->a[3]));
}

static int
darwin_flistxattr(struct thread *td, struct darwin_args *uap)
{
	return (darwin_listxattr_common(td, (int)uap->a[0], NULL, (void *)uap->a[1], (size_t)uap->a[2], (int)uap->a[3]));
}

/* ---- ulock_wait / ulock_wake: sleeping on an address (private to the process: the shared variants use the same key) ---- */

#define	UL_OP_MASK		0xff
#define	ULF_WAKE_ALL		0x00000100
#define	ULF_NO_ERRNO		0x01000000

/* an sx lock, not a mutex: reading the user's word can fault, and a fault sleeps */
static struct sx ulock_sx;
SX_SYSINIT(darwin_ulock, &ulock_sx, "darwin ulock");

static int
darwin_ulock_wait_common(struct thread *td, uint32_t op, uint64_t addr, uint64_t value, uint64_t timeout_us)
{
	uint32_t cur;
	int error = 0, timo = 0, wide = ((op & UL_OP_MASK) == 5 || (op & UL_OP_MASK) == 6);

	if (timeout_us != 0)
		timo = MAX(1, (int)(timeout_us * hz / 1000000));
	sx_xlock(&ulock_sx);
	if (wide) {
		uint64_t c64;

		if (fueword((void *)addr, (long *)&c64) == -1) { sx_xunlock(&ulock_sx); return (EFAULT); }
		if (c64 != value) { sx_xunlock(&ulock_sx); td->td_retval[0] = 0; return (0); }
	} else {
		if (fueword32((void *)addr, (int32_t *)&cur) == -1) { sx_xunlock(&ulock_sx); return (EFAULT); }
		if (cur != (uint32_t)value) { sx_xunlock(&ulock_sx); td->td_retval[0] = 0; return (0); }
	}
	error = sx_sleep((void *)(uintptr_t)addr, &ulock_sx, PCATCH, "ulock", timo);
	sx_xunlock(&ulock_sx);
	if (error == EWOULDBLOCK)
		error = ETIMEDOUT;
	else if (error == ERESTART)
		error = EINTR;
	td->td_retval[0] = 0;
	return (error);
}

static int
darwin_ulock_wait(struct thread *td, struct darwin_args *uap)
{
	uint32_t op = (uint32_t)uap->a[0];
	int error = darwin_ulock_wait_common(td, op, uap->a[1], uap->a[2], (uint32_t)uap->a[3]);

	if (error != 0 && (op & ULF_NO_ERRNO)) {
		td->td_retval[0] = -error;		/* the error comes as a negative result */
		return (0);
	}
	return (error);
}

static int
darwin_ulock_wait2(struct thread *td, struct darwin_args *uap)
{
	uint32_t op = (uint32_t)uap->a[0];
	int error = darwin_ulock_wait_common(td, op, uap->a[1], uap->a[2], uap->a[3] / 1000);	/* nanoseconds */

	if (error != 0 && (op & ULF_NO_ERRNO)) {
		td->td_retval[0] = -error;
		return (0);
	}
	return (error);
}

static int
darwin_ulock_wake(struct thread *td, struct darwin_args *uap)
{
	uint32_t op = (uint32_t)uap->a[0];

	sx_xlock(&ulock_sx);
	if (op & ULF_WAKE_ALL)
		wakeup((void *)uap->a[1]);
	else
		wakeup_one((void *)uap->a[1]);
	sx_xunlock(&ulock_sx);
	td->td_retval[0] = 0;
	return (0);
}

/* ---- ids, kill, code signing, the shared region ---- */

static int
darwin_thread_selfid(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = td->td_tid;
	return (0);
}

static int
darwin_pthread_kill(struct thread *td, struct darwin_args *uap)	/* the "thread port" is the thread id here (thread_selfid) */
{
	struct thr_kill_args a = { .id = (long)uap->a[0], .sig = (int)uap->a[1] };

	return (sys_thr_kill(td, &a));
}

static int
darwin_csops(struct thread *td, struct darwin_args *uap)	/* CS_OPS_STATUS (0): no flags; the rest: nothing to report */
{
	uint32_t zero = 0;

	if (uap->a[2] != 0 && uap->a[3] >= sizeof(zero))
		return (copyout(&zero, (void *)uap->a[2], sizeof(zero)));
	return (0);
}

static int
darwin_shared_region_check_np(struct thread *td __unused, struct darwin_args *uap __unused)
{
	return (ENOMEM);			/* no dyld shared cache is mapped: dyld loads the libraries itself */
}

static const struct {
	int		num, nargs;
	sy_call_t	*fn;
	int		auevent;
	const char	*name;
} darwin_posix[] = {
#define	E(num, nargs, fn, ev, name)	{ (num), (nargs), (sy_call_t *)(fn), (ev), (name) }
	E(463, 4, darwin_openat,	AUE_OPENAT_RWTC,	"openat"),
	E(464, 4, darwin_openat,	AUE_OPENAT_RWTC,	"openat_nocancel"),
	E(465, 4, darwin_renameat,	AUE_RENAMEAT,		"renameat"),
	E(466, 4, darwin_faccessat,	AUE_FACCESSAT,		"faccessat"),
	E(467, 4, darwin_fchmodat,	AUE_FCHMODAT,		"fchmodat"),
	E(468, 5, darwin_fchownat,	AUE_FCHOWNAT,		"fchownat"),
	E(470, 4, darwin_fstatat64,	AUE_FSTATAT,		"fstatat64"),
	E(471, 5, darwin_linkat,	AUE_LINKAT,		"linkat"),
	E(472, 3, darwin_unlinkat,	AUE_UNLINKAT,		"unlinkat"),
	E(473, 4, darwin_readlinkat,	AUE_READLINKAT,		"readlinkat"),
	E(474, 3, darwin_symlinkat,	AUE_SYMLINKAT,		"symlinkat"),
	E(475, 3, darwin_mkdirat,	AUE_MKDIRAT,		"mkdirat"),
	E(234, 6, darwin_getxattr,	AUE_EXTATTR_GET_FILE,	"getxattr"),
	E(235, 6, darwin_fgetxattr,	AUE_EXTATTR_GET_FD,	"fgetxattr"),
	E(236, 6, darwin_setxattr,	AUE_EXTATTR_SET_FILE,	"setxattr"),
	E(237, 6, darwin_fsetxattr,	AUE_EXTATTR_SET_FD,	"fsetxattr"),
	E(238, 3, darwin_removexattr,	AUE_EXTATTR_DELETE_FILE,	"removexattr"),
	E(239, 3, darwin_fremovexattr,	AUE_EXTATTR_DELETE_FD,	"fremovexattr"),
	E(240, 4, darwin_listxattr,	AUE_EXTATTR_LIST_FILE,	"listxattr"),
	E(241, 4, darwin_flistxattr,	AUE_EXTATTR_LIST_FD,	"flistxattr"),
	E(515, 4, darwin_ulock_wait,	AUE_NULL,		"ulock_wait"),
	E(516, 3, darwin_ulock_wake,	AUE_NULL,		"ulock_wake"),
	E(544, 5, darwin_ulock_wait2,	AUE_NULL,		"ulock_wait2"),
	E(372, 0, darwin_thread_selfid,	AUE_NULL,		"thread_selfid"),
	E(328, 2, darwin_pthread_kill,	AUE_NULL,	"__pthread_kill"),
	E(169, 4, darwin_csops,		AUE_NULL,		"csops"),
	E(294, 1, darwin_shared_region_check_np, AUE_NULL,	"shared_region_check_np"),
#undef E
};

void
darwin_posix_syscalls_init(void)
{
	u_int i;

	for (i = 0; i < nitems(darwin_posix); i++) {
		int n = darwin_posix[i].num;

		darwin_sysent[n] = (struct sysent){ .sy_narg = darwin_posix[i].nargs, .sy_call = darwin_posix[i].fn,
		    .sy_auevent = darwin_posix[i].auevent, .sy_thrcnt = SY_THR_STATIC };
		darwin_syscallnames[n] = darwin_posix[i].name;
	}
}
