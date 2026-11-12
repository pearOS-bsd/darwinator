/*
 * More of the BSD class of the Darwin system calls (the base ones are in syscalls.c).  Numbers are xnu's
 * syscalls.master.  Three kinds of entry:
 *   - the call means the same on FreeBSD (process, ids, links, locks, poll, select, ...): it goes to FreeBSD's
 *     implementation as it is (the argument registers are laid out like FreeBSD's argument structures);
 *   - the call takes flags, commands or a structure that differ (fcntl, ioctl for termios, getdirentries64,
 *     statfs64, sigprocmask, getrlimit/setrlimit, msync, sockets, sysctl): a function here translates them;
 *   - the *_nocancel variants that libSystem uses (read_nocancel ...) are the same calls under another number.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/filedesc.h>
#include <sys/ioccom.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/smp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/capsicum.h>
#include <sys/dirent.h>
#include <sys/limits.h>
#include <sys/_termios.h>
#include <sys/ttycom.h>
#include <sys/tty.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"
#include "darwin_abi.h"

/* ---- fcntl ---- */

static int
darwin_fcntl(struct thread *td, struct darwin_args *uap)
{
	int fd = (int)uap->a[0], cmd = (int)uap->a[1], error;
	long arg = (long)uap->a[2];
	struct darwin_flock df;
	struct flock fl;

	switch (cmd) {
	case DARWIN_F_DUPFD:		return (kern_fcntl(td, fd, F_DUPFD, arg));
	case DARWIN_F_GETFD:		return (kern_fcntl(td, fd, F_GETFD, arg));
	case DARWIN_F_SETFD:		return (kern_fcntl(td, fd, F_SETFD, arg));
	case DARWIN_F_DUPFD_CLOEXEC:	return (kern_fcntl(td, fd, F_DUPFD_CLOEXEC, arg));
	case DARWIN_F_GETOWN:		return (kern_fcntl(td, fd, F_GETOWN, arg));
	case DARWIN_F_SETOWN:		return (kern_fcntl(td, fd, F_SETOWN, arg));
	case DARWIN_F_GETFL:
		error = kern_fcntl(td, fd, F_GETFL, 0);
		if (error == 0)
			td->td_retval[0] &= 0xff;	/* access mode, O_NONBLOCK, O_APPEND, O_ASYNC, O_SYNC: the same bits */
		return (error);
	case DARWIN_F_SETFL:
		return (kern_fcntl(td, fd, F_SETFL, arg & (DARWIN_O_NONBLOCK | DARWIN_O_APPEND | DARWIN_O_ASYNC)));
	case DARWIN_F_GETLK:
	case DARWIN_F_SETLK:
	case DARWIN_F_SETLKW:
		error = copyin((void *)uap->a[2], &df, sizeof(df));
		if (error != 0)
			return (error);
		memset(&fl, 0, sizeof(fl));
		fl.l_start = df.l_start;
		fl.l_len = df.l_len;
		fl.l_pid = df.l_pid;
		fl.l_type = df.l_type;		/* F_RDLCK 1, F_UNLCK 2, F_WRLCK 3 on both */
		fl.l_whence = df.l_whence;
		error = kern_fcntl(td, fd, cmd == DARWIN_F_GETLK ? F_GETLK : cmd == DARWIN_F_SETLK ? F_SETLK : F_SETLKW,
		    (intptr_t)&fl);
		if (error == 0 && cmd == DARWIN_F_GETLK) {
			df.l_start = fl.l_start;
			df.l_len = fl.l_len;
			df.l_pid = fl.l_pid;
			df.l_type = fl.l_type;
			df.l_whence = fl.l_whence;
			error = copyout(&df, (void *)uap->a[2], sizeof(df));
		}
		return (error);
	case DARWIN_F_FULLFSYNC: {
		struct fsync_args fa = { .fd = fd };

		return (sys_fsync(td, &fa));
	}
	case 59: case 61: case 98:	/* F_ADDSIGS, F_ADDFILESIGS, F_CHECK_LV: code signatures are not checked */
		return (0);
	case 97: {			/* F_ADDFILESIGS_RETURN(fsignatures_t *): fs_file_start comes back as the end of the signed part of the file (where the signature starts) */
		uint64_t sig[3];

		error = copyin((void *)uap->a[2], sig, sizeof(sig));	/* fs_file_start, fs_blob_start (the offset of the blob), fs_blob_size */
		if (error != 0)
			return (error);
		sig[0] = sig[1];
		return (copyout(sig, (void *)uap->a[2], sizeof(uint64_t)));
	}
	case DARWIN_F_NOCACHE:		/* hints for the cache of the file system: nothing to do */
	case DARWIN_F_RDAHEAD:
	case DARWIN_F_GLOBAL_NOCACHE:
	case DARWIN_F_NODIRECT:
		return (0);
	case DARWIN_F_GETPATH: {
		struct file *fp;
		struct vnode *vp;
		char *path, *freepath = NULL;
		cap_rights_t rights;

		error = getvnode(td, fd, cap_rights_init_one(&rights, CAP_FSTAT), &fp);
		if (error != 0)
			return (error);
		vp = fp->f_vnode;
		vref(vp);
		error = vn_fullpath(vp, &path, &freepath);
		vrele(vp);
		if (error == 0)
			error = copyout(path, (void *)uap->a[2], strlen(path) + 1);
		if (freepath != NULL)
			free(freepath, M_TEMP);
		fdrop(fp, td);
		return (error);
	}
	default:
		return (EINVAL);
	}
}

/* ---- ioctl: the termios structure of Darwin has 64 bit flags; the rest goes to FreeBSD ---- */

static void
termios_to_darwin(const struct termios *t, struct darwin_termios *d)
{
	memset(d, 0, sizeof(*d));
	d->c_iflag = t->c_iflag;
	d->c_oflag = t->c_oflag;
	d->c_cflag = t->c_cflag;
	d->c_lflag = t->c_lflag;
	memcpy(d->c_cc, t->c_cc, sizeof(d->c_cc) < sizeof(t->c_cc) ? sizeof(d->c_cc) : sizeof(t->c_cc));
	d->c_ispeed = t->c_ispeed;
	d->c_ospeed = t->c_ospeed;
}

static void
termios_from_darwin(const struct darwin_termios *d, struct termios *t)
{
	memset(t, 0, sizeof(*t));
	t->c_iflag = (tcflag_t)d->c_iflag;
	t->c_oflag = (tcflag_t)d->c_oflag;
	t->c_cflag = (tcflag_t)d->c_cflag;
	t->c_lflag = (tcflag_t)d->c_lflag;
	memcpy(t->c_cc, d->c_cc, sizeof(t->c_cc) < sizeof(d->c_cc) ? sizeof(t->c_cc) : sizeof(d->c_cc));
	t->c_ispeed = (speed_t)d->c_ispeed;
	t->c_ospeed = (speed_t)d->c_ospeed;
}

static int
darwin_ioctl(struct thread *td, struct darwin_args *uap)
{
	u_long com = (u_long)uap->a[1] & 0xffffffff;
	int fd = (int)uap->a[0], error;
	struct darwin_termios dt;
	struct termios t;
	u_long fcom;

	switch (com) {
	case DARWIN_TIOCGETA:
		error = kern_ioctl(td, fd, TIOCGETA, (caddr_t)&t);
		if (error == 0) {
			termios_to_darwin(&t, &dt);
			error = copyout(&dt, (void *)uap->a[2], sizeof(dt));
		}
		return (error);
	case DARWIN_TIOCSETA:
	case DARWIN_TIOCSETAW:
	case DARWIN_TIOCSETAF:
		error = copyin((void *)uap->a[2], &dt, sizeof(dt));
		if (error != 0)
			return (error);
		termios_from_darwin(&dt, &t);
		fcom = com == DARWIN_TIOCSETA ? TIOCSETA : com == DARWIN_TIOCSETAW ? TIOCSETAW : TIOCSETAF;
		return (kern_ioctl(td, fd, fcom, (caddr_t)&t));
	default:
		/* the request numbers are built the same way on both systems (IOC_IN/OUT, size, group, number) */
		return (sys_ioctl(td, (struct ioctl_args *)uap));
	}
}

/* ---- pipe: the two descriptors come back in rax and rdx ---- */

static int
darwin_pipe(struct thread *td, struct darwin_args *uap)
{
	int fds[2], error;

	error = kern_pipe(td, fds, 0, NULL, NULL);
	if (error == 0) {
		td->td_retval[0] = fds[0];
		td->td_retval[1] = fds[1];
	}
	return (error);
}

/* ---- getdirentries64(fd, buf, size, *position): Darwin's struct dirent (64 bit inode) ---- */

static int
darwin_getdirentries64(struct thread *td, struct darwin_args *uap)
{
	size_t size = (size_t)uap->a[2], got, off = 0, o = 0;
	char *kbuf, *dbuf;
	off_t base = 0;
	ssize_t resid = 0;
	int error;

	if (size < sizeof(struct darwin_dirent_head) + 4)
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
		struct darwin_dirent_head *dd = (struct darwin_dirent_head *)(dbuf + o);
		size_t namlen = de->d_namlen, reclen;

		if (de->d_reclen == 0)
			break;
		reclen = roundup2(DARWIN_DIRENT_NAME + namlen + 1, 4);
		if (o + reclen > size)
			break;
		dd->d_ino = de->d_fileno;
		dd->d_seekoff = de->d_off;
		dd->d_reclen = reclen;
		dd->d_namlen = namlen;
		dd->d_type = de->d_type;
		memcpy(dbuf + o + DARWIN_DIRENT_NAME, de->d_name, namlen);
		dbuf[o + DARWIN_DIRENT_NAME + namlen] = 0;
		o += reclen;
		off += de->d_reclen;
	}
	error = copyout(dbuf, (void *)uap->a[1], o);
	if (error == 0 && uap->a[3] != 0)
		error = copyout(&base, (void *)uap->a[3], sizeof(base));
	if (error == 0)
		td->td_retval[0] = o;
out:
	free(kbuf, M_DARWIN);
	free(dbuf, M_DARWIN);
	return (error);
}

/* ---- statfs64, fstatfs64 ---- */

int
darwin_statfs_out(const struct statfs *s, void *ubuf)
{
	struct darwin_statfs64 *d;
	int error;

	d = malloc(sizeof(*d), M_DARWIN, M_WAITOK | M_ZERO);
	d->f_bsize = s->f_bsize;
	d->f_iosize = s->f_iosize;
	d->f_blocks = s->f_blocks;
	d->f_bfree = s->f_bfree;
	d->f_bavail = s->f_bavail;
	d->f_files = s->f_files;
	d->f_ffree = s->f_ffree;
	d->f_fsid[0] = s->f_fsid.val[0];
	d->f_fsid[1] = s->f_fsid.val[1];
	d->f_owner = s->f_owner;
	d->f_type = s->f_type;
	d->f_flags = (uint32_t)(s->f_flags & 0xffffffff);	/* MNT_RDONLY, MNT_NOSUID ... have the low values of Darwin's */
	strlcpy(d->f_fstypename, s->f_fstypename, sizeof(d->f_fstypename));
	strlcpy(d->f_mntonname, s->f_mntonname, sizeof(d->f_mntonname));
	strlcpy(d->f_mntfromname, s->f_mntfromname, sizeof(d->f_mntfromname));
	error = copyout(d, ubuf, sizeof(*d));
	free(d, M_DARWIN);
	return (error);
}

/* statfs/fstatfs (157, 158): the old struct statfs of the real Mac (tools/macprobe/probe3): short otype, oflags; 8 byte bsize iosize blocks bfree bavail files ffree at 8..63;
 * fsid 64, owner 72, type (short) 78, flags (8 bytes) 80, fstypename[15] 104, mntonname[90] 119, mntfromname[90] 209; 304 bytes are written */
static int
darwin_statfs_old_out(const struct statfs *s, void *ubuf)
{
	char b[304];
	uint64_t v;
	uint32_t w;
	uint16_t h;

	memset(b, 0, sizeof(b));
#define	P64(off, val)	do { v = (uint64_t)(val); memcpy(b + (off), &v, 8); } while (0)
	P64(8, s->f_bsize); P64(16, s->f_iosize); P64(24, s->f_blocks); P64(32, s->f_bfree);
	P64(40, s->f_bavail); P64(48, s->f_files); P64(56, s->f_ffree);
	P64(80, s->f_flags & 0xffffffff);
#undef P64
	w = s->f_fsid.val[0]; memcpy(b + 64, &w, 4);
	w = s->f_fsid.val[1]; memcpy(b + 68, &w, 4);
	w = s->f_owner; memcpy(b + 72, &w, 4);
	h = (uint16_t)s->f_type; memcpy(b + 78, &h, 2);
	strlcpy(b + 104, s->f_fstypename, 15);
	strlcpy(b + 119, s->f_mntonname, 90);
	strlcpy(b + 209, s->f_mntfromname, 90);
	return (copyout(b, ubuf, sizeof(b)));
}

static int
darwin_statfs_old(struct thread *td, struct darwin_args *uap)
{
	struct statfs *s = malloc(sizeof(*s), M_DARWIN, M_WAITOK | M_ZERO);
	int error = kern_statfs(td, (const char *)uap->a[0], UIO_USERSPACE, s);

	if (error == 0)
		error = darwin_statfs_old_out(s, (void *)uap->a[1]);
	free(s, M_DARWIN);
	return (error);
}

static int
darwin_fstatfs_old(struct thread *td, struct darwin_args *uap)
{
	struct statfs *s = malloc(sizeof(*s), M_DARWIN, M_WAITOK | M_ZERO);
	int error = kern_fstatfs(td, (int)uap->a[0], s);

	if (error == 0)
		error = darwin_statfs_old_out(s, (void *)uap->a[1]);
	free(s, M_DARWIN);
	return (error);
}

static int
darwin_statfs64(struct thread *td, struct darwin_args *uap)
{
	struct statfs *s = malloc(sizeof(*s), M_DARWIN, M_WAITOK | M_ZERO);
	int error = kern_statfs(td, (const char *)uap->a[0], UIO_USERSPACE, s);

	if (error == 0)
		error = darwin_statfs_out(s, (void *)uap->a[1]);
	free(s, M_DARWIN);
	return (error);
}

static int
darwin_fstatfs64(struct thread *td, struct darwin_args *uap)
{
	struct statfs *s = malloc(sizeof(*s), M_DARWIN, M_WAITOK | M_ZERO);
	int error = kern_fstatfs(td, (int)uap->a[0], s);

	if (error == 0)
		error = darwin_statfs_out(s, (void *)uap->a[1]);
	free(s, M_DARWIN);
	return (error);
}

/* ---- sigprocmask(how, *set, *oset): Darwin's sigset_t is 32 bits ---- */

int
darwin_sigprocmask(struct thread *td, struct darwin_args *uap)
{
	sigset_t set, oset, *setp = NULL;
	uint32_t m, om = 0;
	int error, i, how = (int)uap->a[0];

	if (uap->a[1] != 0) {
		error = copyin((void *)uap->a[1], &m, sizeof(m));
		if (error != 0)
			return (error);
		SIGEMPTYSET(set);
		for (i = 1; i <= 32; i++)
			if (m & (1U << (i - 1)))
				SIGADDSET(set, i);
		setp = &set;
		switch (how) {
		case 1: how = SIG_BLOCK; break;		/* the same numbers, spelled out for the reader */
		case 2: how = SIG_UNBLOCK; break;
		case 3: how = SIG_SETMASK; break;
		default: return (EINVAL);
		}
	}
	error = kern_sigprocmask(td, how, setp, &oset, 0);
	if (error != 0)
		return (error);
	if (uap->a[2] != 0) {
		for (i = 1; i <= 32; i++)
			if (SIGISMEMBER(oset, i))
				om |= 1U << (i - 1);
		error = copyout(&om, (void *)uap->a[2], sizeof(om));
	}
	return (error);
}

/* ---- getrlimit / setrlimit: the resources 0..8 are the same; Darwin's RLIMIT_AS is 5 (RSS here) ---- */

static int
darwin_getrlimit(struct thread *td, struct darwin_args *uap)
{
	struct rlimit rl;

	if ((u_int)uap->a[0] > 8)
		return (EINVAL);
	lim_rlimit(td, (int)uap->a[0], &rl);
	return (copyout(&rl, (void *)uap->a[1], sizeof(rl)));
}

static int
darwin_setrlimit(struct thread *td, struct darwin_args *uap)
{
	struct rlimit rl;
	int error;

	if ((u_int)uap->a[0] > 8)
		return (EINVAL);
	error = copyin((void *)uap->a[1], &rl, sizeof(rl));
	return (error != 0 ? error : kern_setrlimit(td, (u_int)uap->a[0], &rl));
}

/* ---- msync: Darwin's MS_SYNC is 0x10 (FreeBSD's is 0) ---- */

static int
darwin_msync(struct thread *td, struct darwin_args *uap)
{
	struct msync_args ma;
	int d = (int)uap->a[2], f = 0;

	if (d & DARWIN_MS_ASYNC)	f |= MS_ASYNC;
	if (d & DARWIN_MS_INVALIDATE)	f |= MS_INVALIDATE;
	ma.addr = (void *)uap->a[0];
	ma.len = (size_t)uap->a[1];
	ma.flags = f;
	return (sys_msync(td, &ma));
}

/* ---- sockets: AF_INET6 is 30 on Darwin and 28 here; SO_NOSIGPIPE and MSG_NOSIGNAL have other values ---- */

static int
darwin_domain(int d)
{
	return (d == DARWIN_AF_INET6 ? AF_INET6 : d);
}

static int
darwin_socket(struct thread *td, struct darwin_args *uap)
{
	return (kern_socket(td, darwin_domain((int)uap->a[0]), (int)uap->a[1], (int)uap->a[2]));
}

static int
darwin_socketpair(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;

	a.a[0] = darwin_domain((int)a.a[0]);
	return (sys_socketpair(td, (struct socketpair_args *)&a));
}

int
darwin_msgflags_pub(int d)
{
	return (d & ~DARWIN_MSG_NOSIGNAL) | ((d & DARWIN_MSG_NOSIGNAL) ? MSG_NOSIGNAL : 0);
}

static int
darwin_msgflags(int d)
{
	return ((d & ~DARWIN_MSG_NOSIGNAL) | ((d & DARWIN_MSG_NOSIGNAL) ? MSG_NOSIGNAL : 0));
}

static int
darwin_sendto(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;

	a.a[3] = darwin_msgflags((int)a.a[3]);
	return (sys_sendto(td, (struct sendto_args *)&a));
}

static int
darwin_sockopt_name(int level, int name)
{
	if (level == SOL_SOCKET && name == DARWIN_SO_NOSIGPIPE)
		return (SO_NOSIGPIPE);
	return (name);
}

static int
darwin_setsockopt(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;

	a.a[2] = darwin_sockopt_name((int)a.a[1], (int)a.a[2]);
	return (sys_setsockopt(td, (struct setsockopt_args *)&a));
}

static int
darwin_getsockopt(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;

	a.a[2] = darwin_sockopt_name((int)a.a[1], (int)a.a[2]);
	return (sys_getsockopt(td, (struct getsockopt_args *)&a));
}

/* ---- sysctl: a read-only set of the names programs ask for first (hw.*, kern.*) ---- */

#if defined(__amd64__)
#define	DARWIN_MACHINE	"x86_64"
#else
#define	DARWIN_MACHINE	"arm64"
#endif

enum { K_STR, K_INT, K_U64, K_HOST, K_BOOTTIME };

static const struct darwin_sysctl {
	const char	*name;
	int		mib0, mib1;
	int		kind;
	const char	*str;
	int		id;		/* which number for K_INT/K_U64 */
} darwin_sysctls[] = {
	{ "kern.ostype",	1, 1,	K_STR, "Darwin", 0 },
	{ "kern.osrelease",	1, 2,	K_STR, "23.6.0", 0 },
	{ "kern.version",	1, 4,	K_STR, "Darwin Kernel Version 23.6.0: darwinator on FreeBSD", 0 },
	{ "kern.hostname",	1, 10,	K_HOST, NULL, 0 },
	{ "kern.argmax",	1, 8,	K_INT, NULL, 1 },
	{ "kern.osversion",	1, 65,	K_STR, "23G0", 0 },
	{ "hw.machine",		6, 1,	K_STR, DARWIN_MACHINE, 0 },
	{ "hw.model",		6, 2,	K_STR, "pearOS", 0 },
	{ "hw.ncpu",		6, 3,	K_INT, NULL, 2 },
	{ "hw.physmem",		6, 5,	K_INT, NULL, 4 },
	{ "hw.pagesize",	6, 7,	K_U64, NULL, 3 },	/* 8 bytes on the Mac (probe of macOS 27); 4 bytes when the caller only has room for 4 */
	{ "hw.memsize",		6, 24,	K_U64, NULL, 5 },
	{ "hw.activecpu",	6, 25,	K_INT, NULL, 2 },
	{ "kern.boottime",	1, 21,	K_BOOTTIME, NULL, 0 },
	{ "kern.maxproc",	1, 6,	K_INT, NULL, 6 },
	{ "kern.maxfiles",	1, 7,	K_INT, NULL, 7 },
	{ "kern.maxfilesperproc", 1, 29, K_INT, NULL, 8 },
	{ "hw.cachelinesize",	-1, -1,	K_U64, NULL, 10 },
	{ "hw.cpu64bit_capable", -1, -1, K_INT, NULL, 11 },
	{ "hw.byteorder",	6, 4,	K_INT, NULL, 12 },
	{ "hw.logicalcpu_max",	-1, -1,	K_INT, NULL, 2 },
	{ "hw.physicalcpu_max",	-1, -1,	K_INT, NULL, 2 },
	{ "hw.l1dcachesize",	-1, -1,	K_U64, NULL, 13 },
	{ "hw.l1icachesize",	-1, -1,	K_U64, NULL, 13 },
	{ "hw.l2cachesize",	-1, -1,	K_U64, NULL, 14 },
	{ "hw.tbfrequency",	-1, -1,	K_U64, NULL, 15 },
	{ "hw.optional.x86_64",	-1, -1,	K_INT, NULL, 11 },
	{ "hw.optional.arm64",	-1, -1,	K_INT, NULL, 11 },
	{ "hw.logicalcpu",	-1, -1,	K_INT, NULL, 2 },
	{ "hw.physicalcpu",	-1, -1,	K_INT, NULL, 2 },
};

static int
darwin_sysctl_value(struct thread *td, const struct darwin_sysctl *e, void *old, size_t *oldlenp_k, int have_old,
    void *uoldlenp)
{
	char hostname[MAXHOSTNAMELEN];
	union { int i; uint64_t u; } v;
	struct { int64_t sec; int32_t usec; int32_t pad; } tv;		/* Darwin's struct timeval */
	const char *data = NULL;
	size_t size = 0, have = 0;
	bool shrink = false;
	int error;

	switch (e->kind) {
	case K_STR:
		data = e->str;
		size = strlen(data) + 1;
		break;
	case K_HOST: {
		int mib[2] = { CTL_KERN, KERN_HOSTNAME };
		size_t len = sizeof(hostname);

		error = kernel_sysctl(td, mib, 2, hostname, &len, NULL, 0, &len, 0);
		if (error != 0)
			return (error);
		data = hostname;
		size = strlen(hostname) + 1;
		break;
	}
	case K_INT:
		switch (e->id) {
		case 1: v.i = 1048576; break;
		case 2: v.i = mp_ncpus; break;
		case 3: v.i = PAGE_SIZE; break;
		case 6: v.i = maxproc; break;
		case 7: v.i = maxfiles; break;
		case 8: v.i = maxfilesperproc; break;
		case 9: v.i = 199506; break;		/* KERN_OSREV of BSD 4.4 */
		case 10: v.i = 64; break;
		case 11: v.i = 1; break;
		case 12: v.i = 1234; break;
		case 13: v.i = 32768; break;
		case 14: v.i = 262144; break;
		default: v.i = (int)MIN((uint64_t)ptoa((uint64_t)physmem), (uint64_t)INT_MAX); break;
		}
		data = (const char *)&v;
		size = sizeof(v.i);
		break;
	case K_BOOTTIME: {
		struct timeval bt;

		getboottime(&bt);
		tv.sec = bt.tv_sec;
		tv.usec = bt.tv_usec;
		tv.pad = 0;
		data = (const char *)&tv;
		size = sizeof(tv);
		break;
	}
	default:
		switch (e->id) {
		case 3: v.u = PAGE_SIZE; shrink = true; break;
		case 10: v.u = 64; shrink = true; break;
		case 13: v.u = 32768; shrink = true; break;
		case 14: v.u = 262144; shrink = true; break;
		case 15: v.u = 1000000000ULL; break;
		default: v.u = (uint64_t)ptoa((uint64_t)physmem); break;
		}
		data = (const char *)&v;
		size = sizeof(v.u);
		break;
	}
	if (uoldlenp != NULL && copyin(uoldlenp, &have, sizeof(have)) == 0 && shrink && have == 4)
		size = 4;			/* the caller asks with an int: the low half (little endian) */
	if (uoldlenp != NULL) {
		error = copyin(uoldlenp, &have, sizeof(have));
		if (error != 0)
			return (error);
	}
	if (have_old && old != NULL) {
		error = copyout(data, old, MIN(have, size));
		if (error != 0)
			return (error);
		if (have < size) {
			(void)copyout(&have, uoldlenp, sizeof(have));
			return (ENOMEM);
		}
	}
	if (uoldlenp != NULL)
		error = copyout(&size, uoldlenp, sizeof(size));
	else
		error = 0;
	(void)oldlenp_k;
	return (error);
}

static int
darwin_sysctl(struct thread *td, struct darwin_args *uap)
{
	int mib[4] = { -1, -1, -1, -1 }, error;
	u_int namelen = (u_int)uap->a[1], i;

	if (namelen < 2 || namelen > 4)
		return (ENOENT);
	error = copyin((void *)uap->a[0], mib, namelen * sizeof(int));
	if (error != 0)
		return (error);
	if (mib[0] == 1 && mib[1] == 14)		/* CTL_KERN, KERN_PROC */
		return (darwin_sysctl_proc(td, mib, namelen, (void *)uap->a[2], (void *)uap->a[3], uap->a[4] != 0 && uap->a[5] != 0));
	if (mib[0] == 1 && mib[1] == 49 && namelen == 3)	/* KERN_PROCARGS2 */
		return (darwin_sysctl_procargs2(td, mib[2], (void *)uap->a[2], (void *)uap->a[3]));
	if (uap->a[4] != 0 && uap->a[5] != 0)
		return (EPERM);			/* read-only */
	if (mib[0] == 0 && mib[1] == 3)
		return (ENOENT);		/* name to number: programs use sysctlbyname */
	for (i = 0; i < nitems(darwin_sysctls); i++)
		if (darwin_sysctls[i].mib0 == mib[0] && darwin_sysctls[i].mib1 == mib[1] && namelen == 2)
			return (darwin_sysctl_value(td, &darwin_sysctls[i], (void *)uap->a[2], NULL, 1,
			    (void *)uap->a[3]));
	return (ENOENT);
}

static int
darwin_sysctlbyname(struct thread *td, struct darwin_args *uap)
{
	char name[64];
	size_t len = (size_t)uap->a[1];
	u_int i;
	int error;

	if (len == 0 || len >= sizeof(name))
		return (ENOENT);
	error = copyin((void *)uap->a[0], name, len);
	if (error != 0)
		return (error);
	name[len] = '\0';
	if (uap->a[4] != 0 && uap->a[5] != 0) {
		if (strncmp(name, "kern.wq_", 8) == 0)	/* the limits of the workqueue (libdispatch sets kern.wq_limit_cooperative_threads at its start): accepted, there is no workqueue to limit */
			return (0);
		return (EPERM);
	}
	for (i = 0; i < nitems(darwin_sysctls); i++)
		if (strcmp(darwin_sysctls[i].name, name) == 0)
			return (darwin_sysctl_value(td, &darwin_sysctls[i], (void *)uap->a[2], NULL, 1,
			    (void *)uap->a[3]));
	return (ENOENT);
}

/* ---- the entries ---- */

/* getaudit_addr(auditinfo_addr_t *, length) (357): the audit session of the process; LaunchServices and the window server name sessions with the id.  There is no audit session here: one id for each user (100000 + uid), no mask, no terminal. */
static int
darwin_getaudit_addr(struct thread *td, struct darwin_args *uap)
{
	uint8_t ai[48];

	if (uap->a[1] < sizeof(ai))
		return (EOVERFLOW);
	memset(ai, 0, sizeof(ai));
	*(uint32_t *)(ai + 0) = 0xffffffffu;						/* ai_auid: AU_DEFAUDITID */
	*(uint32_t *)(ai + 36) = 100000 + td->td_ucred->cr_ruid;			/* ai_asid */
	td->td_retval[0] = 0;
	return (copyout(ai, (void *)uap->a[0], sizeof(ai)));
}

/* audit_session_join(port) (351): the session of the port becomes the one of the process, and the id comes back; there is one session per user here (100000 + uid, the id of getaudit_addr).
 * launchservicesd calls it for every request and does not answer when it fails. */
static int
darwin_audit_session_join(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = 100000 + td->td_ucred->cr_ruid;
	return (0);
}

/* gettimeofday(tv, tz, mach_absolute_time *): the third argument of Darwin's libsystem_kernel (__gettimeofday_with_mach), which mach_get_times needs: the time of the uptime clock at the moment of the call */
static int
darwin_gettimeofday(struct thread *td, struct darwin_args *uap)
{
	struct gettimeofday_args ga = { .tp = (struct timeval *)uap->a[0], .tzp = (struct timezone *)uap->a[1] };
	int error = sys_gettimeofday(td, &ga);

	if (error == 0 && uap->a[2] != 0) {
		struct timespec ts;
		uint64_t t;

		nanouptime(&ts);
		t = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
		error = copyout(&t, (void *)uap->a[2], sizeof(t));
	}
	td->td_retval[0] = 0;
	return (error);
}

static const struct {
	int		num, nargs;
	sy_call_t	*fn;
	int		auevent;
	const char	*name;
} darwin_more[] = {
#define	E(num, nargs, fn, ev, name)	{ (num), (nargs), (sy_call_t *)(fn), (ev), (name) }
	E(499, 4, darwin_work_interval_ctl,	AUE_NULL,		"work_interval_ctl"),
	E(357, 2, darwin_getaudit_addr,	AUE_NULL,		"getaudit_addr"),
	E(351, 1, darwin_audit_session_join,	AUE_NULL,		"audit_session_join"),
	E(116, 3, darwin_gettimeofday,	AUE_GETTIMEOFDAY,	"gettimeofday"),
	/* processes and ids */
	E(2,   0, sys_fork,		AUE_FORK,		"fork"),
	E(7,   4, sys_wait4,		AUE_WAIT4,		"wait4"),
	E(59,  3, sys_execve,		AUE_EXECVE,		"execve"),
	E(23,  1, sys_setuid,		AUE_SETUID,		"setuid"),
	E(79,  2, sys_getgroups,	AUE_GETGROUPS,		"getgroups"),
	E(81,  0, sys_getpgrp,		AUE_GETPGRP,		"getpgrp"),
	E(82,  2, sys_setpgid,		AUE_SETPGRP,		"setpgid"),
	E(96,  3, sys_setpriority,	AUE_SETPRIORITY,	"setpriority"),
	E(100, 2, sys_getpriority,	AUE_GETPRIORITY,	"getpriority"),
	E(117, 2, sys_getrusage,	AUE_GETRUSAGE,		"getrusage"),
	E(126, 2, sys_setreuid,		AUE_SETREUID,		"setreuid"),
	E(127, 2, sys_setregid,		AUE_SETREGID,		"setregid"),
	E(147, 0, sys_setsid,		AUE_SETSID,		"setsid"),
	E(151, 1, sys_getpgid,		AUE_GETPGID,		"getpgid"),
	E(181, 1, sys_setgid,		AUE_SETGID,		"setgid"),
	E(182, 1, sys_setegid,		AUE_SETEGID,		"setegid"),
	E(183, 1, sys_seteuid,		AUE_SETEUID,		"seteuid"),
	E(310, 1, sys_getsid,		AUE_GETSID,		"getsid"),
	E(194, 2, darwin_getrlimit,	AUE_GETRLIMIT,		"getrlimit"),
	E(195, 2, darwin_setrlimit,	AUE_SETRLIMIT,		"setrlimit"),
	E(89,  0, sys_getdtablesize,	AUE_GETDTABLESIZE,	"getdtablesize"),
	/* files */
	E(9,   2, sys_link,		AUE_LINK,		"link"),
	E(13,  1, sys_fchdir,		AUE_FCHDIR,		"fchdir"),
	E(15,  2, sys_chmod,		AUE_CHMOD,		"chmod"),
	E(16,  3, sys_chown,		AUE_CHOWN,		"chown"),
	E(34,  2, sys_chflags,		AUE_CHFLAGS,		"chflags"),
	E(35,  2, sys_fchflags,		AUE_FCHFLAGS,		"fchflags"),
	E(36,  0, sys_sync,		AUE_SYNC,		"sync"),
	E(42,  0, darwin_pipe,		AUE_PIPE,		"pipe"),
	E(57,  2, sys_symlink,		AUE_SYMLINK,		"symlink"),
	E(58,  3, sys_readlink,		AUE_READLINK,		"readlink"),
	E(123, 3, sys_fchown,		AUE_FCHOWN,		"fchown"),
	E(124, 2, sys_fchmod,		AUE_FCHMOD,		"fchmod"),
	E(128, 2, sys_rename,		AUE_RENAME,		"rename"),
	E(131, 2, sys_flock,		AUE_FLOCK,		"flock"),
	E(132, 2, sys_mkfifo,		AUE_MKFIFO,		"mkfifo"),
	E(200, 2, sys_truncate,		AUE_TRUNCATE,		"truncate"),
	E(92,  3, darwin_fcntl,		AUE_FCNTL,		"fcntl"),
	E(54,  3, darwin_ioctl,		AUE_IOCTL,		"ioctl"),
	E(344, 4, darwin_getdirentries64, AUE_GETDIRENTRIES,	"getdirentries64"),
	E(345, 2, darwin_statfs64,	AUE_STATFS,		"statfs64"),
	E(346, 2, darwin_fstatfs64,	AUE_FSTATFS,		"fstatfs64"),
	E(157, 2, darwin_statfs_old,	AUE_STATFS,		"statfs"),
	E(158, 2, darwin_fstatfs_old,	AUE_FSTATFS,		"fstatfs"),
	/* memory, signals, waiting */
	E(65,  3, darwin_msync,		AUE_MSYNC,		"msync"),
	E(75,  3, sys_madvise,		AUE_MADVISE,		"madvise"),
	E(203, 2, sys_mlock,		AUE_MLOCK,		"mlock"),
	E(204, 2, sys_munlock,		AUE_MUNLOCK,		"munlock"),
	E(48,  3, darwin_sigprocmask,	AUE_SIGPROCMASK,	"sigprocmask"),
	E(329, 3, darwin_sigprocmask,	AUE_SIGPROCMASK,	"pthread_sigmask"),
	E(93,  5, sys_select,		AUE_SELECT,		"select"),
	E(230, 3, sys_poll,		AUE_POLL,		"poll"),
	/* sockets */
	E(97,  3, darwin_socket,	AUE_SOCKET,		"socket"),
	E(98,  3, sys_connect,		AUE_CONNECT,		"connect"),
	E(104, 3, sys_bind,		AUE_BIND,		"bind"),
	E(106, 2, sys_listen,		AUE_LISTEN,		"listen"),
	E(30,  3, sys_accept,		AUE_ACCEPT,		"accept"),
	E(29,  6, sys_recvfrom,		AUE_RECVFROM,		"recvfrom"),
	E(31,  3, sys_getpeername,	AUE_GETPEERNAME,	"getpeername"),
	E(32,  3, sys_getsockname,	AUE_GETSOCKNAME,	"getsockname"),
	E(133, 6, darwin_sendto,	AUE_SENDTO,		"sendto"),
	E(134, 2, sys_shutdown,		AUE_SHUTDOWN,		"shutdown"),
	E(135, 4, darwin_socketpair,	AUE_SOCKETPAIR,		"socketpair"),
	E(105, 5, darwin_setsockopt,	AUE_SETSOCKOPT,		"setsockopt"),
	E(118, 5, darwin_getsockopt,	AUE_GETSOCKOPT,		"getsockopt"),
	/* sysctl */
	E(202, 6, darwin_sysctl,	AUE_SYSCTL,		"sysctl"),
	E(274, 6, darwin_sysctlbyname,	AUE_SYSCTL,		"sysctlbyname"),
	/* what libSystem calls under the *_nocancel names (the same calls, a cancellation point less) */
	E(396, 3, sys_read,		AUE_READ,		"read_nocancel"),
	E(397, 3, sys_write,		AUE_WRITE,		"write_nocancel"),
	E(399, 1, sys_close,		AUE_CLOSE,		"close_nocancel"),
	E(400, 4, sys_wait4,		AUE_WAIT4,		"wait4_nocancel"),
	E(403, 6, sys_recvfrom,		AUE_RECVFROM,		"recvfrom_nocancel"),
	E(404, 3, sys_accept,		AUE_ACCEPT,		"accept_nocancel"),
	E(405, 3, darwin_msync,		AUE_MSYNC,		"msync_nocancel"),
	E(406, 3, darwin_fcntl,		AUE_FCNTL,		"fcntl_nocancel"),
	E(407, 5, sys_select,		AUE_SELECT,		"select_nocancel"),
	E(408, 1, sys_fsync,		AUE_FSYNC,		"fsync_nocancel"),
	E(409, 3, sys_connect,		AUE_CONNECT,		"connect_nocancel"),
	E(411, 3, sys_readv,		AUE_READV,		"readv_nocancel"),
	E(412, 3, sys_writev,		AUE_WRITEV,		"writev_nocancel"),
	E(413, 6, darwin_sendto,	AUE_SENDTO,		"sendto_nocancel"),
	E(414, 4, sys_pread,		AUE_PREAD,		"pread_nocancel"),
	E(415, 4, sys_pwrite,		AUE_PWRITE,		"pwrite_nocancel"),
	E(417, 3, sys_poll,		AUE_POLL,		"poll_nocancel"),
#undef E
};

void
darwin_more_syscalls_init(void)
{
	u_int i;

	for (i = 0; i < nitems(darwin_more); i++) {
		int n = darwin_more[i].num;

		if (darwin_sysent[n].sy_call != NULL)
			continue;			/* the base table has it */
		darwin_sysent[n] = (struct sysent){ .sy_narg = darwin_more[i].nargs, .sy_call = darwin_more[i].fn,
		    .sy_auevent = darwin_more[i].auevent, .sy_thrcnt = SY_THR_STATIC };
		darwin_syscallnames[n] = darwin_more[i].name;
	}
	darwin_event_syscalls_init();		/* syscalls_event.c: replaces some of the above */
	darwin_posix_syscalls_init();		/* syscalls_posix.c */
	darwin_sync_syscalls_init();		/* syscalls_sync.c */
	darwin_compat_syscalls_init();		/* syscalls_compat.c */
	darwin_spawn_syscalls_init();		/* syscalls_spawn.c */
	darwin_sem_syscalls_init();		/* syscalls_sem.c */
	darwin_misc_syscalls_init();		/* syscalls_misc.c */
	darwin_dyld_syscalls_init();		/* syscalls_dyld.c: replaces the shared_region_check_np of syscalls_posix.c */
	darwin_proc_syscalls_init();		/* syscalls_proc.c */
	darwin_ipc_syscalls_init();		/* mach_ipc.c */
}

/* open_nocancel (398) is the open of syscalls.c: filled by darwin_syscalls_init */
