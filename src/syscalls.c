/*
 * The BSD class of the Darwin system calls.  The numbers are the ones of xnu's syscalls.master.  A call whose
 * arguments and results are the same as FreeBSD's goes straight to FreeBSD's implementation (PASS); a call whose
 * flags or structures differ has a function here that translates them (open, mmap, stat64, getentropy); errno
 * numbers are translated on the way out (darwin_set_syscall_retval).  What is not in the table fails with ENOSYS.
 *
 * All the functions take the six or eight argument registers as they are (td->td_sa.args).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/uio.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"
#include "darwin_abi.h"

/* ---- errno ---- */

int
darwin_errno(int e)
{
	switch (e) {
	case EIDRM:		return (DARWIN_EIDRM);
	case ENOMSG:		return (DARWIN_ENOMSG);
	case ECANCELED:		return (DARWIN_ECANCELED);
	case EILSEQ:		return (DARWIN_EILSEQ);
	case ENOATTR:		return (DARWIN_ENOATTR);
	case EBADMSG:		return (DARWIN_EBADMSG);
	case EMULTIHOP:		return (DARWIN_EMULTIHOP);
	case ENOLINK:		return (DARWIN_ENOLINK);
	case EPROTO:		return (DARWIN_EPROTO);
	case ENOTRECOVERABLE:	return (DARWIN_ENOTRECOVERABLE);
	case EOWNERDEAD:	return (DARWIN_EOWNERDEAD);
	case EDOOFUS:		return (EINVAL);	/* no counterpart on Darwin */
	case ENOTCAPABLE:
	case ECAPMODE:		return (EPERM);
	case EINTEGRITY:	return (EIO);
	default:		return (e);		/* the same number (up to 79, EAUTH, ENEEDAUTH, EOVERFLOW), or not an errno (ERESTART, EJUSTRETURN) */
	}
}

/* ---- open ---- */

int
darwin_oflags(int d)
{
	int f = d & DARWIN_O_ACCMODE;

#define	MAPFLAG(dflag, fflag)	do { if (d & (dflag)) f |= (fflag); } while (0)
	MAPFLAG(DARWIN_O_NONBLOCK, O_NONBLOCK);
	MAPFLAG(DARWIN_O_APPEND, O_APPEND);
	MAPFLAG(DARWIN_O_SHLOCK, O_SHLOCK);
	MAPFLAG(DARWIN_O_EXLOCK, O_EXLOCK);
	MAPFLAG(DARWIN_O_ASYNC, O_ASYNC);
	MAPFLAG(DARWIN_O_SYNC, O_FSYNC);
	MAPFLAG(DARWIN_O_NOFOLLOW, O_NOFOLLOW);
	MAPFLAG(DARWIN_O_CREAT, O_CREAT);
	MAPFLAG(DARWIN_O_TRUNC, O_TRUNC);
	MAPFLAG(DARWIN_O_EXCL, O_EXCL);
	MAPFLAG(DARWIN_O_NOCTTY, O_NOCTTY);
	MAPFLAG(DARWIN_O_DIRECTORY, O_DIRECTORY);
	MAPFLAG(DARWIN_O_DSYNC, O_DSYNC);
	MAPFLAG(DARWIN_O_CLOEXEC, O_CLOEXEC);
#undef MAPFLAG
	return (f);
}

static int
darwin_open(struct thread *td, struct darwin_args *uap)
{
	return (kern_openat(td, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, darwin_oflags((int)uap->a[1]), (int)uap->a[2]));
}

/* ---- mmap ---- */

static int
darwin_mmap(struct thread *td, struct darwin_args *uap)
{
	struct mmap_args ma;
	int d = (int)uap->a[3], f = 0;

	if (d & DARWIN_MAP_SHARED)	f |= MAP_SHARED;
	if (d & DARWIN_MAP_PRIVATE)	f |= MAP_PRIVATE;
	if (d & DARWIN_MAP_FIXED)	f |= MAP_FIXED;
	if (d & DARWIN_MAP_HASSEMAPHORE) f |= MAP_HASSEMAPHORE;
	if (d & DARWIN_MAP_ANON)	f |= MAP_ANON;
	ma.addr = (void *)uap->a[0];
	ma.len = (size_t)uap->a[1];
	ma.prot = (int)uap->a[2];
	ma.flags = f;
	ma.fd = (int)uap->a[4];
	ma.pos = (off_t)uap->a[5];
	return (sys_mmap(td, &ma));
}

/* ---- stat64, fstat64, lstat64 ---- */

int
darwin_stat_out(struct thread *td, const struct stat *sb, void *ubuf)
{
	struct darwin_stat64 d;

	memset(&d, 0, sizeof(d));
	d.st_dev = darwin_from_dev(sb->st_dev);
	d.st_mode = sb->st_mode;
	d.st_nlink = sb->st_nlink > 0xffff ? 0xffff : (uint16_t)sb->st_nlink;
	d.st_ino = sb->st_ino;
	d.st_uid = sb->st_uid;
	d.st_gid = sb->st_gid;
	d.st_rdev = darwin_from_dev(sb->st_rdev);
	d.st_atimespec.tv_sec = sb->st_atim.tv_sec;	d.st_atimespec.tv_nsec = sb->st_atim.tv_nsec;
	d.st_mtimespec.tv_sec = sb->st_mtim.tv_sec;	d.st_mtimespec.tv_nsec = sb->st_mtim.tv_nsec;
	d.st_ctimespec.tv_sec = sb->st_ctim.tv_sec;	d.st_ctimespec.tv_nsec = sb->st_ctim.tv_nsec;
	d.st_birthtimespec.tv_sec = sb->st_birthtim.tv_sec; d.st_birthtimespec.tv_nsec = sb->st_birthtim.tv_nsec;
	d.st_size = sb->st_size;
	d.st_blocks = sb->st_blocks;
	d.st_blksize = sb->st_blksize;
	d.st_flags = sb->st_flags;
	d.st_gen = (uint32_t)sb->st_gen;
	return (copyout(&d, ubuf, sizeof(d)));
}

static int
darwin_stat64(struct thread *td, struct darwin_args *uap)
{
	struct stat sb;
	int error = kern_statat(td, 0, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, &sb);

	return (error != 0 ? error : darwin_stat_out(td, &sb, (void *)uap->a[1]));
}

static int
darwin_lstat64(struct thread *td, struct darwin_args *uap)
{
	struct stat sb;
	int error = kern_statat(td, AT_SYMLINK_NOFOLLOW, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, &sb);

	return (error != 0 ? error : darwin_stat_out(td, &sb, (void *)uap->a[1]));
}

static int
darwin_fstat64(struct thread *td, struct darwin_args *uap)
{
	struct stat sb;
	int error = kern_fstat(td, (int)uap->a[0], &sb);

	return (error != 0 ? error : darwin_stat_out(td, &sb, (void *)uap->a[1]));
}

/* ---- getentropy(buf, size): 0 or an error, at most 256 bytes ---- */

static int
darwin_getentropy(struct thread *td, struct darwin_args *uap)
{
	struct getrandom_args ga;
	int error;

	if (uap->a[1] > 256)
		return (EIO);
	ga.buf = (void *)uap->a[0];
	ga.buflen = (size_t)uap->a[1];
	ga.flags = 0;
	error = sys_getrandom(td, &ga);
	if (error == 0)
		td->td_retval[0] = 0;
	return (error);
}

/* ---- the table ---- */

#define	ENTRY(num, nargs, fn, ev) \
	[num] = { .sy_narg = (nargs), .sy_call = (sy_call_t *)(fn), .sy_auevent = (ev), .sy_thrcnt = SY_THR_STATIC }
#define	NAME(num, s)	[num] = (s)

struct sysent darwin_sysent[DARWIN_NSYSCALLS] = {
	ENTRY(1,   1, sys_exit,		AUE_EXIT),
	ENTRY(3,   3, sys_read,		AUE_READ),
	ENTRY(4,   3, sys_write,	AUE_WRITE),
	ENTRY(5,   3, darwin_open,	AUE_OPEN_RWTC),
	ENTRY(6,   1, sys_close,	AUE_CLOSE),
	ENTRY(10,  1, sys_unlink,	AUE_UNLINK),
	ENTRY(12,  1, sys_chdir,	AUE_CHDIR),
	ENTRY(20,  0, sys_getpid,	AUE_GETPID),
	ENTRY(24,  0, sys_getuid,	AUE_GETUID),
	ENTRY(25,  0, sys_geteuid,	AUE_GETEUID),
	ENTRY(33,  2, sys_access,	AUE_ACCESS),
	ENTRY(37,  2, sys_kill,		AUE_KILL),
	ENTRY(39,  0, sys_getppid,	AUE_GETPPID),
	ENTRY(41,  1, sys_dup,		AUE_DUP),
	ENTRY(43,  0, sys_getegid,	AUE_GETEGID),
	ENTRY(47,  0, sys_getgid,	AUE_GETGID),
	ENTRY(60,  1, sys_umask,	AUE_UMASK),
	ENTRY(73,  2, sys_munmap,	AUE_MUNMAP),
	ENTRY(74,  3, sys_mprotect,	AUE_MPROTECT),
	ENTRY(90,  2, sys_dup2,		AUE_DUP2),
	ENTRY(95,  1, sys_fsync,	AUE_FSYNC),
	ENTRY(120, 3, sys_readv,	AUE_READV),
	ENTRY(121, 3, sys_writev,	AUE_WRITEV),
	ENTRY(136, 2, sys_mkdir,	AUE_MKDIR),
	ENTRY(137, 1, sys_rmdir,	AUE_RMDIR),
	ENTRY(153, 4, sys_pread,	AUE_PREAD),
	ENTRY(154, 4, sys_pwrite,	AUE_PWRITE),
	ENTRY(197, 6, darwin_mmap,	AUE_MMAP),
	ENTRY(199, 3, sys_lseek,	AUE_LSEEK),
	ENTRY(201, 2, sys_ftruncate,	AUE_FTRUNCATE),
	ENTRY(327, 0, sys_issetugid,	AUE_ISSETUGID),
	ENTRY(338, 2, darwin_stat64,	AUE_STAT),
	ENTRY(339, 2, darwin_fstat64,	AUE_FSTAT),
	ENTRY(340, 2, darwin_lstat64,	AUE_LSTAT),
	ENTRY(398, 3, darwin_open,	AUE_OPEN_RWTC),
	ENTRY(500, 2, darwin_getentropy, AUE_NULL),
};

const char *darwin_syscallnames[DARWIN_NSYSCALLS] = {
	NAME(1, "exit"), NAME(3, "read"), NAME(4, "write"), NAME(5, "open"), NAME(6, "close"), NAME(10, "unlink"),
	NAME(12, "chdir"), NAME(20, "getpid"), NAME(24, "getuid"), NAME(25, "geteuid"), NAME(33, "access"),
	NAME(37, "kill"), NAME(39, "getppid"), NAME(41, "dup"), NAME(43, "getegid"), NAME(47, "getgid"),
	NAME(60, "umask"), NAME(73, "munmap"), NAME(74, "mprotect"), NAME(90, "dup2"), NAME(95, "fsync"),
	NAME(116, "gettimeofday"), NAME(120, "readv"), NAME(121, "writev"), NAME(136, "mkdir"), NAME(137, "rmdir"),
	NAME(153, "pread"), NAME(154, "pwrite"), NAME(197, "mmap"), NAME(199, "lseek"), NAME(201, "ftruncate"),
	NAME(327, "issetugid"), NAME(338, "stat64"), NAME(339, "fstat64"), NAME(340, "lstat64"),
	NAME(398, "open_nocancel"), NAME(500, "getentropy"),
};

const int darwin_nsyscalls = DARWIN_NSYSCALLS;

/* what is not implemented fails with ENOSYS; with kern.darwinator.debug it is logged: the list of what a program needs */
static int
darwin_nosys(struct thread *td, struct nosys_args *uap)
{
	if (darwin_debug)
		printf("darwinator: %s[%d]: unimplemented system call %#x (%s)\n", td->td_proc->p_comm, td->td_proc->p_pid,
		    td->td_sa.original_code, td->td_sa.code < (u_int)DARWIN_NSYSCALLS && darwin_syscallnames[td->td_sa.code] != NULL ? darwin_syscallnames[td->td_sa.code] : "?");
	if (darwin_debug && td->td_sa.original_code == 0x2000226) {		/* os_fault_with_payload(namespace, code, payload, size, string, flags): who complains, and why */
		char str[120] = "";
		size_t done;

		(void)copyinstr((void *)td->td_sa.args[4], str, sizeof(str), &done);
		printf("darwinator: %s[%d]: os_fault_with_payload ns %ld code %#lx string \"%s\"\n", td->td_proc->p_comm, td->td_proc->p_pid,
		    (long)td->td_sa.args[0], (long)td->td_sa.args[1], str);
	}
	return (nosys(td, uap));
}

void
darwin_syscalls_init(void)
{
	int i;

	darwin_more_syscalls_init();		/* syscalls_more.c */
	darwin_machdep_syscalls_init();		/* the entries of the machdep class (per architecture) */
	for (i = 0; i < DARWIN_NSYSCALLS; i++) {
		if (darwin_sysent[i].sy_call == NULL) {
			darwin_sysent[i] = (struct sysent){ .sy_narg = 0, .sy_call = (sy_call_t *)darwin_nosys,
			    .sy_auevent = AUE_NULL, .sy_thrcnt = SY_THR_STATIC };
			darwin_syscallnames[i] = "#unimplemented";
		} else if (darwin_syscallnames[i] == NULL)
			darwin_syscallnames[i] = "#unnamed";
	}
}
