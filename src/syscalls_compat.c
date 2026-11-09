/*
 * The BSD class, the rest that has a meaning on this system: the old 32 bit inode forms (stat, lstat, fstat, getdirentries), the *_extended calls
 * (ACLs are not kept: the extended security argument is dropped), System V IPC and POSIX shared memory, recvmsg/sendmsg, the calls that are the same
 * on FreeBSD (utimes, pathconf, mlockall ...), the guarded descriptors (the guard is ignored), and the answers for what a Mac has and this system
 * has not: ENOTSUP for the file system features of APFS and HFS+ (clonefile, exchangedata, searchfs, setattrlist ...: programs check for it and go
 * on without), success for the hints that change nothing (kdebug, iopolicy, process_policy), ENOSYS for the rest (audit, MAC, networking
 * extensions, the nexus and channel kernel networking, coalitions ...).  What is NOT here: posix_spawn, the aio_* calls, sem_open and the other named
 * semaphores, proc_info (and posix_spawn: a child that starts in the kernel needs the fork machinery of FreeBSD; machrun gives programs FreeBSD's posix_spawn).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/dirent.h>
#include <sys/sysctl.h>
#include <sys/fcntl.h>
#include <sys/ipc.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"
#include "darwin_abi.h"

/* ---- stat, lstat, fstat: the 120 byte structure of the 32 bit inodes ---- */

struct darwin_stat32 {
	int32_t		st_dev;
	uint32_t	st_ino;
	uint16_t	st_mode;
	uint16_t	st_nlink;
	uint32_t	st_uid;
	uint32_t	st_gid;
	int32_t		st_rdev;
	struct { int64_t tv_sec; int64_t tv_nsec; } st_atimespec, st_mtimespec, st_ctimespec;
	int64_t		st_size;
	int64_t		st_blocks;
	int32_t		st_blksize;
	uint32_t	st_flags;
	uint32_t	st_gen;
	int32_t		st_lspare;
	int64_t		st_qspare[2];
};
_Static_assert(sizeof(struct darwin_stat32) == 120, "stat of Darwin (32 bit inodes)");

static int
stat32_out(const struct stat *sb, void *ubuf)
{
	struct darwin_stat32 d;

	memset(&d, 0, sizeof(d));
	d.st_dev = darwin_from_dev(sb->st_dev);
	d.st_ino = (uint32_t)sb->st_ino;
	d.st_mode = sb->st_mode;
	d.st_nlink = sb->st_nlink > 0xffff ? 0xffff : (uint16_t)sb->st_nlink;
	d.st_uid = sb->st_uid;
	d.st_gid = sb->st_gid;
	d.st_rdev = darwin_from_dev(sb->st_rdev);
	d.st_atimespec.tv_sec = sb->st_atim.tv_sec; d.st_atimespec.tv_nsec = sb->st_atim.tv_nsec;
	d.st_mtimespec.tv_sec = sb->st_mtim.tv_sec; d.st_mtimespec.tv_nsec = sb->st_mtim.tv_nsec;
	d.st_ctimespec.tv_sec = sb->st_ctim.tv_sec; d.st_ctimespec.tv_nsec = sb->st_ctim.tv_nsec;
	d.st_size = sb->st_size; d.st_blocks = sb->st_blocks; d.st_blksize = sb->st_blksize;
	d.st_flags = sb->st_flags; d.st_gen = (uint32_t)sb->st_gen;
	return (copyout(&d, ubuf, sizeof(d)));
}

static int
darwin_stat(struct thread *td, struct darwin_args *uap)
{
	struct stat sb;
	int error = kern_statat(td, 0, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, &sb);

	return (error != 0 ? error : stat32_out(&sb, (void *)uap->a[1]));
}

static int
darwin_lstat(struct thread *td, struct darwin_args *uap)
{
	struct stat sb;
	int error = kern_statat(td, AT_SYMLINK_NOFOLLOW, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, &sb);

	return (error != 0 ? error : stat32_out(&sb, (void *)uap->a[1]));
}

static int
darwin_fstat(struct thread *td, struct darwin_args *uap)
{
	struct stat sb;
	int error = kern_fstat(td, (int)uap->a[0], &sb);

	return (error != 0 ? error : stat32_out(&sb, (void *)uap->a[1]));
}

static int
darwin_fstatat(struct thread *td, struct darwin_args *uap)		/* (fd, path, ub, flag), the 32 bit form of fstatat64 */
{
	struct stat sb;
	int fd = (int)uap->a[0] == -2 ? AT_FDCWD : (int)uap->a[0], fl = 0;
	int error;

	if (uap->a[3] & 0x20) fl |= AT_SYMLINK_NOFOLLOW;
	error = kern_statat(td, fl, fd, (const char *)uap->a[1], UIO_USERSPACE, &sb);
	return (error != 0 ? error : stat32_out(&sb, (void *)uap->a[2]));
}

/* the *_extended forms: (path, ub, xsecurity, *xsecurity_size): no ACL, the size comes back 0 */
static int
xsec_size_zero(void *p)
{
	size_t z = 0;

	return (p != NULL ? copyout(&z, p, sizeof(z)) : 0);
}

static int
darwin_stat_extended(struct thread *td, struct darwin_args *uap)
{
	int error = darwin_stat(td, uap);

	return (error != 0 ? error : xsec_size_zero((void *)uap->a[3]));
}

static int
darwin_lstat_extended(struct thread *td, struct darwin_args *uap)
{
	int error = darwin_lstat(td, uap);

	return (error != 0 ? error : xsec_size_zero((void *)uap->a[3]));
}

static int
darwin_fstat_extended(struct thread *td, struct darwin_args *uap)
{
	int error = darwin_fstat(td, uap);

	return (error != 0 ? error : xsec_size_zero((void *)uap->a[3]));
}

static int
darwin_stat64_extended_common(struct thread *td, struct darwin_args *uap, int kind)
{
	struct stat sb;
	int error;

	if (kind == 0)
		error = kern_statat(td, 0, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, &sb);
	else if (kind == 1)
		error = kern_statat(td, AT_SYMLINK_NOFOLLOW, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, &sb);
	else
		error = kern_fstat(td, (int)uap->a[0], &sb);
	if (error == 0)
		error = darwin_stat_out(td, &sb, (void *)uap->a[1]);
	return (error != 0 ? error : xsec_size_zero((void *)uap->a[3]));
}
static int darwin_stat64_extended(struct thread *td, struct darwin_args *uap) { return (darwin_stat64_extended_common(td, uap, 0)); }
static int darwin_lstat64_extended(struct thread *td, struct darwin_args *uap) { return (darwin_stat64_extended_common(td, uap, 1)); }
static int darwin_fstat64_extended(struct thread *td, struct darwin_args *uap) { return (darwin_stat64_extended_common(td, uap, 2)); }

/* open_extended(path, flags, uid, gid, mode, xsecurity), umask_extended(newmask, xsecurity), chmod_extended(path, uid, gid, mode, xsec), fchmod_extended(fd, uid, gid, mode, xsec),
 * mkfifo_extended(path, uid, gid, mode, xsec), mkdir_extended(path, uid, gid, mode, xsec): the mode goes on, the owner and the ACL do not */
static int
darwin_open_extended(struct thread *td, struct darwin_args *uap)
{
	return (kern_openat(td, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, darwin_oflags((int)uap->a[1]), (int)uap->a[4]));
}

static int
darwin_umask_extended(struct thread *td, struct darwin_args *uap)
{
	struct umask_args ua = { .newmask = (int)uap->a[0] };

	return (sys_umask(td, &ua));
}

static int
darwin_chmod_extended(struct thread *td, struct darwin_args *uap)
{
	struct chmod_args ca = { .path = (char *)uap->a[0], .mode = (int)uap->a[3] };

	return ((int)uap->a[3] == -1 ? 0 : sys_chmod(td, &ca));
}

static int
darwin_fchmod_extended(struct thread *td, struct darwin_args *uap)
{
	struct fchmod_args fa = { .fd = (int)uap->a[0], .mode = (int)uap->a[3] };

	return ((int)uap->a[3] == -1 ? 0 : sys_fchmod(td, &fa));
}

static int
darwin_mkfifo_extended(struct thread *td, struct darwin_args *uap)
{
	return (kern_mkfifoat(td, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, (int)uap->a[3]));
}

static int
darwin_mkdir_extended(struct thread *td, struct darwin_args *uap)
{
	return (kern_mkdirat(td, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, (int)uap->a[3]));
}

static int
darwin_mkfifoat(struct thread *td, struct darwin_args *uap)
{
	return (kern_mkfifoat(td, (int)uap->a[0] == -2 ? AT_FDCWD : (int)uap->a[0], (const char *)uap->a[1], UIO_USERSPACE, (int)uap->a[2]));
}

static int
darwin_mknodat(struct thread *td, struct darwin_args *uap)
{
	return (kern_mknodat(td, (int)uap->a[0] == -2 ? AT_FDCWD : (int)uap->a[0], (const char *)uap->a[1], UIO_USERSPACE, (int)uap->a[2],
	    darwin_to_dev((int32_t)uap->a[3])));
}

/* ---- getdirentries(fd, buf, nbytes, *basep): the old entry, with a 32 bit inode ---- */

static int
darwin_getdirentries(struct thread *td, struct darwin_args *uap)
{
	size_t size = (size_t)uap->a[2], got, off = 0, o = 0;
	char *kbuf, *dbuf;
	off_t base = 0;
	ssize_t resid = 0;
	int error;

	if (size < 16)
		return (EINVAL);
	if (size > 65536)
		size = 65536;
	kbuf = malloc(size, M_DARWIN, M_WAITOK);
	dbuf = malloc(size, M_DARWIN, M_WAITOK | M_ZERO);
	error = kern_getdirentries(td, (int)uap->a[0], kbuf, size, &base, &resid, UIO_SYSSPACE);
	if (error != 0)
		goto out;
	got = size - resid;
	while (off + offsetof(struct dirent, d_name) <= got) {
		struct dirent *de = (struct dirent *)(kbuf + off);
		size_t namlen = de->d_namlen, reclen;

		if (de->d_reclen == 0)
			break;
		reclen = roundup2(8 + namlen + 1, 4);		/* d_ino 4, d_reclen 2, d_type 1, d_namlen 1, d_name */
		if (o + reclen > size)
			break;
		*(uint32_t *)(dbuf + o) = (uint32_t)de->d_fileno;
		*(uint16_t *)(dbuf + o + 4) = (uint16_t)reclen;
		dbuf[o + 6] = de->d_type;
		dbuf[o + 7] = (char)namlen;
		memcpy(dbuf + o + 8, de->d_name, namlen);
		dbuf[o + 8 + namlen] = '\0';
		o += reclen;
		off += de->d_reclen;
	}
	error = copyout(dbuf, (void *)uap->a[1], o);
	if (error == 0 && uap->a[3] != 0) {
		long b = (long)base;

		error = copyout(&b, (void *)uap->a[3], sizeof(b));
	}
	if (error == 0)
		td->td_retval[0] = o;
out:
	free(kbuf, M_DARWIN);
	free(dbuf, M_DARWIN);
	return (error);
}

/* ---- sigwait(set, *sig): the 32 bit set ---- */

static int
darwin_sigwait(struct thread *td, struct darwin_args *uap)
{
	sigset_t set;
	ksiginfo_t ksi;
	uint32_t m;
	int error, sig, i;

	if ((error = copyin((void *)uap->a[0], &m, sizeof(m))) != 0)
		return (error);
	SIGEMPTYSET(set);
	for (i = 1; i <= 32; i++)
		if (m & (1U << (i - 1)))
			SIGADDSET(set, i);
	error = kern_sigtimedwait(td, set, &ksi, NULL);
	if (error != 0)
		return (error == ERESTART ? EINTR : error);
	sig = ksi.ksi_signo;
	td->td_retval[0] = 0;
	return (uap->a[1] != 0 ? copyout(&sig, (void *)uap->a[1], sizeof(sig)) : 0);
}


/* ---- waitid(idtype, id, siginfo_t *, options): P_ALL 0, P_PID 1, P_PGID 2 and other option bits; the siginfo of Darwin is 104 bytes ---- */

static int
darwin_waitid(struct thread *td, struct darwin_args *uap)
{
	siginfo_t si;
	char out[104];
	int opts = (int)uap->a[3], fo = 0, status = 0, error;
	enum idtype it;

	switch ((int)uap->a[0]) {
	case 0: it = P_ALL; break;
	case 1: it = P_PID; break;
	case 2: it = P_PGID; break;
	default: return (EINVAL);
	}
	if (opts & 0x0001) fo |= WNOHANG;
	if (opts & 0x0004) fo |= WEXITED;
	if (opts & 0x0008) fo |= WUNTRACED;
	if (opts & 0x0010) fo |= WCONTINUED;
	if (opts & 0x0020) fo |= WNOWAIT;
	memset(&si, 0, sizeof(si));
	error = kern_wait6(td, it, (id_t)uap->a[1], &status, fo, NULL, &si);
	if (error != 0)
		return (error);
	memset(out, 0, sizeof(out));
	memcpy(out, &si, MIN(sizeof(si), sizeof(out)));
	td->td_retval[0] = 0;
	return (uap->a[2] != 0 ? copyout(out, (void *)uap->a[2], sizeof(out)) : 0);
}

/* ---- pselect(nd, in, ou, ex, timespec *, sigmask *): the mask is 32 bits, the time is a timespec ---- */

static int
darwin_pselect(struct thread *td, struct darwin_args *uap)
{
	struct timespec ts;
	struct timeval tv, *tvp = NULL;
	sigset_t set, *setp = NULL;
	uint32_t m;
	int error, i;

	if (uap->a[4] != 0) {
		if ((error = copyin((void *)uap->a[4], &ts, sizeof(ts))) != 0)
			return (error);
		TIMESPEC_TO_TIMEVAL(&tv, &ts);
		tvp = &tv;
	}
	if (uap->a[5] != 0) {
		if ((error = copyin((void *)uap->a[5], &m, sizeof(m))) != 0)
			return (error);
		SIGEMPTYSET(set);
		for (i = 1; i <= 32; i++)
			if (m & (1U << (i - 1)))
				SIGADDSET(set, i);
		setp = &set;
	}
	return (kern_pselect(td, (int)uap->a[0], (fd_set *)uap->a[1], (fd_set *)uap->a[2], (fd_set *)uap->a[3], tvp, setp, sizeof(long) * 8));
}

/* ---- sendfile(fd, s, offset, *len, hdtr, flags): FreeBSD takes the length by value and gives the sent bytes in another pointer ---- */

static int
darwin_sendfile(struct thread *td, struct darwin_args *uap)
{
	struct sendfile_args sa;
	off_t len = 0;
	int error;

	if (uap->a[3] != 0 && (error = copyin((void *)uap->a[3], &len, sizeof(len))) != 0)
		return (error);
	sa.fd = (int)uap->a[0];
	sa.s = (int)uap->a[1];
	sa.offset = (off_t)uap->a[2];
	sa.nbytes = (size_t)len;
	sa.hdtr = (struct sf_hdtr *)uap->a[4];
	sa.sbytes = (off_t *)uap->a[3];
	sa.flags = 0;
	return (sys_sendfile(td, &sa));
}

/* ---- System V IPC and POSIX shared memory: the calls that mean the same ---- */

static int
darwin_shm_open(struct thread *td, struct darwin_args *uap)	/* (name, oflag, mode) */
{
	struct shm_open2_args sa = { .path = (const char *)uap->a[0], .flags = darwin_oflags((int)uap->a[1]), .mode = (mode_t)uap->a[2], .shmflags = 0, .name = NULL };

	return (sys_shm_open2(td, &sa));
}

static int
darwin_semctl(struct thread *td, struct darwin_args *uap)	/* (semid, semnum, cmd, arg): the union by value on Darwin */
{
	union semun arg;
	int cmd = (int)uap->a[2], error;
	register_t rval = 0;

	arg.val = (int)uap->a[3];
	if (cmd == GETALL || cmd == SETALL)
		arg.array = (unsigned short *)uap->a[3];
	else if (cmd == IPC_STAT || cmd == IPC_SET)
		return (ENOSYS);			/* the semid_ds of Darwin has another layout */
	error = kern_semctl(td, (int)uap->a[0], (int)uap->a[1], cmd, &arg, &rval);
	td->td_retval[0] = rval;
	return (error);
}

static int
darwin_shmctl(struct thread *td, struct darwin_args *uap)	/* (shmid, cmd, buf): IPC_RMID only; the shmid_ds of Darwin has another layout */
{
	if ((int)uap->a[1] != IPC_RMID)
		return (ENOSYS);
	return (kern_shmctl(td, (int)uap->a[0], IPC_RMID, NULL, NULL));
}

static int
darwin_msgctl(struct thread *td, struct darwin_args *uap)
{
	if ((int)uap->a[1] != IPC_RMID)
		return (ENOSYS);
	return (kern_msgctl(td, (int)uap->a[0], IPC_RMID, NULL));
}

/* ---- guarded descriptors: the guard is ignored ---- */

static int
darwin_guarded_open(struct thread *td, struct darwin_args *uap)	/* (path, guard, guardflags, flags, mode) */
{
	return (kern_openat(td, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, darwin_oflags((int)uap->a[3]), (int)uap->a[4]));
}

static int
darwin_guarded_close(struct thread *td, struct darwin_args *uap)	/* (fd, guard) */
{
	struct close_args ca = { .fd = (int)uap->a[0] };

	return (sys_close(td, &ca));
}

static int
darwin_guarded_kqueue(struct thread *td, struct darwin_args *uap __unused)
{
	struct kqueue_args ka;

	return (sys_kqueue(td, &ka));
}

static int
darwin_guarded_write(struct thread *td, struct darwin_args *uap)	/* (fd, guard, buf, nbyte) */
{
	struct write_args wa = { .fd = (int)uap->a[0], .buf = (const void *)uap->a[2], .nbyte = (size_t)uap->a[3] };

	return (sys_write(td, &wa));
}

static int
darwin_guarded_pwrite(struct thread *td, struct darwin_args *uap)	/* (fd, guard, buf, nbyte, offset) */
{
	struct pwrite_args wa = { .fd = (int)uap->a[0], .buf = (const void *)uap->a[2], .nbyte = (size_t)uap->a[3], .offset = (off_t)uap->a[4] };

	return (sys_pwrite(td, &wa));
}

static int
darwin_guarded_writev(struct thread *td, struct darwin_args *uap)	/* (fd, guard, iovp, iovcnt) */
{
	struct writev_args wa = { .fd = (int)uap->a[0], .iovp = (struct iovec *)uap->a[2], .iovcnt = (u_int)uap->a[3] };

	return (sys_writev(td, &wa));
}

/* ---- small ones ---- */

static int
darwin_ok(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = 0;
	return (0);
}

static int
darwin_enotsup(struct thread *td __unused, struct darwin_args *uap __unused)
{
	return (ENOTSUP);
}

static int
darwin_esrch(struct thread *td __unused, struct darwin_args *uap __unused)
{
	return (ESRCH);
}

static int
darwin_eperm(struct thread *td __unused, struct darwin_args *uap __unused)
{
	return (EPERM);
}

static int
darwin_einval(struct thread *td __unused, struct darwin_args *uap __unused)
{
	return (EINVAL);
}

static int
darwin_ptrace(struct thread *td, struct darwin_args *uap)	/* PT_DENY_ATTACH (31) is accepted: nobody may attach anyway; the rest is not supported */
{
	if ((int)uap->a[0] == 31 /* PT_DENY_ATTACH */ || (int)uap->a[0] == 0 /* PT_TRACE_ME */) {
		td->td_retval[0] = 0;
		return (0);
	}
	return (ENOTSUP);
}

static int
darwin_gethostuuid(struct thread *td, struct darwin_args *uap)	/* (uuid_t *uuid, const struct timespec *timeout): the 16 bytes of kern.hostuuid */
{
	unsigned char u[16];
	char s[40];
	size_t len = sizeof(s);
	int mib[2] = { CTL_KERN, KERN_HOSTUUID }, error, i, k = 0, hi = -1;

	memset(s, 0, sizeof(s));
	error = kernel_sysctl(td, mib, 2, s, &len, NULL, 0, &len, 0);
	memset(u, 0, sizeof(u));
	for (i = 0; error == 0 && s[i] != '\0' && k < 16; i++) {
		int c = s[i], v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;

		if (v < 0)
			continue;
		if (hi < 0) hi = v; else { u[k++] = (unsigned char)(hi << 4 | v); hi = -1; }
	}
	return (copyout(u, (void *)uap->a[0], sizeof(u)));
}

static int
darwin_pthread_chdir(struct thread *td, struct darwin_args *uap)	/* per thread on Darwin: here for the process */
{
	struct chdir_args ca = { .path = (char *)uap->a[0] };

	return (sys_chdir(td, &ca));
}

static int
darwin_pthread_fchdir(struct thread *td, struct darwin_args *uap)
{
	struct fchdir_args fa = { .fd = (int)uap->a[0] };

	return (sys_fchdir(td, &fa));
}

static const struct {
	int		num, nargs;
	sy_call_t	*fn;
	const char	*name;
} darwin_compat[] = {
#define	E(num, nargs, fn, name)	{ (num), (nargs), (sy_call_t *)(fn), (name) }
	/* the same calls as FreeBSD's */
	E(27,  3, sys_recvmsg,		"recvmsg"),
	E(28,  3, sys_sendmsg,		"sendmsg"),
	E(401, 3, sys_recvmsg,		"recvmsg_nocancel"),
	E(402, 3, sys_sendmsg,		"sendmsg_nocancel"),
	E(49,  2, sys_getlogin,		"getlogin"),
	E(50,  1, sys_setlogin,		"setlogin"),
	E(56,  1, sys_revoke,		"revoke"),
	E(61,  1, sys_chroot,		"chroot"),
	E(66,  0, sys_vfork,		"vfork"),
	E(78,  3, sys_mincore,		"mincore"),
	E(80,  2, sys_setgroups,	"setgroups"),
	E(122, 2, sys_settimeofday,	"settimeofday"),
	E(138, 2, sys_utimes,		"utimes"),
	E(139, 2, sys_futimes,		"futimes"),
	E(140, 2, sys_adjtime,		"adjtime"),
	E(187, 1, sys_fdatasync,	"fdatasync"),
	E(191, 2, sys_pathconf,		"pathconf"),
	E(192, 2, sys_fpathconf,	"fpathconf"),
	E(250, 3, sys_minherit,		"minherit"),
	E(324, 1, sys_mlockall,		"mlockall"),
	E(325, 0, sys_munlockall,	"munlockall"),
	E(364, 3, sys_lchown,		"lchown"),
	E(540, 4, sys_preadv,		"preadv"),
	E(541, 4, sys_pwritev,		"pwritev"),
	E(542, 4, sys_preadv,		"preadv_nocancel"),
	E(543, 4, sys_pwritev,		"pwritev_nocancel"),
	E(255, 3, sys_semget,		"semget"),
	E(256, 3, sys_semop,		"semop"),
	E(259, 2, sys_msgget,		"msgget"),
	E(260, 4, sys_msgsnd,		"msgsnd"),
	E(261, 5, sys_msgrcv,		"msgrcv"),
	E(262, 3, sys_shmat,		"shmat"),
	E(264, 1, sys_shmdt,		"shmdt"),
	E(265, 3, sys_shmget,		"shmget"),
	E(267, 1, sys_shm_unlink,	"shm_unlink"),
	/* with a translation */
	E(188, 2, darwin_stat,		"stat"),
	E(189, 2, darwin_fstat,		"fstat"),
	E(190, 2, darwin_lstat,		"lstat"),
	E(469, 4, darwin_fstatat,	"fstatat"),
	E(279, 4, darwin_stat_extended,	"stat_extended"),
	E(280, 4, darwin_lstat_extended, "lstat_extended"),
	E(281, 4, darwin_fstat_extended, "fstat_extended"),
	E(341, 4, darwin_stat64_extended, "stat64_extended"),
	E(342, 4, darwin_lstat64_extended, "lstat64_extended"),
	E(343, 4, darwin_fstat64_extended, "fstat64_extended"),
	E(277, 6, darwin_open_extended,	"open_extended"),
	E(278, 2, darwin_umask_extended, "umask_extended"),
	E(282, 5, darwin_chmod_extended, "chmod_extended"),
	E(283, 5, darwin_fchmod_extended, "fchmod_extended"),
	E(291, 5, darwin_mkfifo_extended, "mkfifo_extended"),
	E(292, 5, darwin_mkdir_extended, "mkdir_extended"),
	E(553, 3, darwin_mkfifoat,	"mkfifoat"),
	E(554, 4, darwin_mknodat,	"mknodat"),
	E(196, 4, darwin_getdirentries,	"getdirentries"),
	E(330, 2, darwin_sigwait,	"__sigwait"),
	E(173, 4, darwin_waitid,	"waitid"),
	E(416, 4, darwin_waitid,	"waitid_nocancel"),
	E(394, 6, darwin_pselect,	"pselect"),
	E(395, 6, darwin_pselect,	"pselect_nocancel"),
	E(422, 2, darwin_sigwait,	"__sigwait_nocancel"),
	E(337, 6, darwin_sendfile,	"sendfile"),
	E(266, 3, darwin_shm_open,	"shm_open"),
	E(254, 4, darwin_semctl,	"semctl"),
	E(263, 3, darwin_shmctl,	"shmctl"),
	E(258, 3, darwin_msgctl,	"msgctl"),
	E(441, 5, darwin_guarded_open,	"guarded_open_np"),
	E(442, 2, darwin_guarded_close,	"guarded_close_np"),
	E(443, 2, darwin_guarded_kqueue, "guarded_kqueue_np"),
	E(444, 5, darwin_ok,		"change_fdguard_np"),
	E(485, 4, darwin_guarded_write,	"guarded_write_np"),
	E(486, 5, darwin_guarded_pwrite, "guarded_pwrite_np"),
	E(487, 4, darwin_guarded_writev, "guarded_writev_np"),
	E(26,  4, darwin_ptrace,	"ptrace"),
	E(142, 2, darwin_gethostuuid,	"gethostuuid"),
	E(348, 1, darwin_pthread_chdir,	"__pthread_chdir"),
	E(349, 1, darwin_pthread_fchdir, "__pthread_fchdir"),
	/* hints that change nothing here */
	E(170, 4, darwin_ok,		"csops_audittoken"),
	E(177, 4, darwin_ok,		"kdebug_typefilter"),
	E(178, 4, darwin_ok,		"kdebug_trace_string"),
	E(179, 6, darwin_ok,		"kdebug_trace64"),
	E(180, 6, darwin_ok,		"kdebug_trace"),
	E(296, 1, darwin_ok,		"vm_pressure_monitor"),
	E(322, 3, darwin_ok,		"iopolicysys"),
	E(323, 4, darwin_ok,		"process_policy"),
	E(331, 1, darwin_ok,		"__disable_threadsignal"),
	E(332, 1, darwin_ok,		"__pthread_markcancel"),
	E(333, 1, darwin_ok,		"__pthread_canceled"),
	E(446, 4, darwin_ok,		"proc_rlimit_control"),
	E(478, 3, darwin_ok,		"bsdthread_ctl"),
	E(482, 0, darwin_ok,		"thread_selfusage"),
	E(186, 3, darwin_enotsup,	"thread_selfcounts"),
	E(286, 2, darwin_esrch,		"gettid"),
	E(38,  3, darwin_enotsup,	"crossarch_trap"),
	E(483, 3, darwin_eperm,		"csrctl"),
	E(381, 3, darwin_ok,		"__mac_syscall"),
	/* what APFS and HFS+ have and this system has not: ENOTSUP, the answer that programs check for */
	E(205, 2, darwin_enotsup,	"undelete"),
	E(221, 5, darwin_enotsup,	"setattrlist"),
	E(222, 7, darwin_enotsup,	"getdirentriesattr"),
	E(223, 3, darwin_enotsup,	"exchangedata"),
	E(225, 6, darwin_enotsup,	"searchfs"),
	E(226, 1, darwin_enotsup,	"delete"),
	E(227, 4, darwin_enotsup,	"copyfile"),
	E(229, 5, darwin_enotsup,	"fsetattrlist"),
	E(242, 4, darwin_enotsup,	"fsctl"),
	E(245, 4, darwin_enotsup,	"ffsctl"),
	E(427, 4, darwin_enotsup,	"fsgetpath"),
	E(461, 5, darwin_enotsup,	"getattrlistbulk"),
	E(462, 4, darwin_enotsup,	"clonefileat"),
	E(479, 4, darwin_enotsup,	"openbyid_np"),
	E(517, 4, darwin_enotsup,	"fclonefileat"),
	E(518, 4, darwin_enotsup,	"fs_snapshot"),
	E(524, 6, darwin_enotsup,	"setattrlistat"),
	E(549, 4, darwin_enotsup,	"graftdmg"),
	E(555, 2, darwin_enotsup,	"ungraftdmg"),
#undef E
};

void
darwin_compat_syscalls_init(void)
{
	u_int i;

	for (i = 0; i < nitems(darwin_compat); i++) {
		int n = darwin_compat[i].num;

		if (darwin_sysent[n].sy_call != NULL)
			continue;
		darwin_sysent[n] = (struct sysent){ .sy_narg = darwin_compat[i].nargs, .sy_call = darwin_compat[i].fn,
		    .sy_auevent = AUE_NULL, .sy_thrcnt = SY_THR_STATIC };
		darwin_syscallnames[n] = darwin_compat[i].name;
	}
}
