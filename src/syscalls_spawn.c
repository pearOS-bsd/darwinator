/*
 * posix_spawn (Darwin syscall 244): a new process is created in the kernel, the way init is: fork1() with RFSTOPPED, a start function in the child
 * (cpu_fork_kthread_handler) that applies the file actions and the attributes and then calls kern_execve(), and the parent waits until the exec has
 * happened (as on Darwin, an exec error is the return value of posix_spawn).
 *
 *   int posix_spawn(pid_t *pid, const char *path, const struct _posix_spawn_args_desc *adesc, char *const argv[], char *const envp[])
 *
 * Understood: the attributes POSIX_SPAWN_SETPGROUP, SETSIGDEF, SETSIGMASK, SETSID, SETEXEC (without the fork), RESETIDS, and the file actions
 * open, close, dup2, inherit, chdir, fchdir.  Mach port actions and the other descriptor extensions are ignored.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/fcntl.h>
#include <sys/filedesc.h>
#include <sys/imgact.h>
#include <sys/priv.h>
#include <sys/resourcevar.h>
#include <sys/sched.h>
#include <sys/signalvar.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/ucred.h>
#include <sys/unistd.h>
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"

MALLOC_DEFINE(M_DSPAWN, "darwin_spawn", "darwinator posix_spawn requests");

#define	PS_RESETIDS	0x0001
#define	PS_SETPGROUP	0x0002
#define	PS_SETSIGDEF	0x0004
#define	PS_SETSIGMASK	0x0008
#define	PS_SETEXEC	0x0040
#define	PS_SETSID	0x0400

#define	PSFA_OPEN	0
#define	PSFA_CLOSE	1
#define	PSFA_DUP2	2
#define	PSFA_INHERIT	3
#define	PSFA_CHDIR	5
#define	PSFA_FCHDIR	6

#define	PSFA_SIZE	1040		/* type, filedes, union { open: oflag, mode (2) + path[1024]; dup2; chdir: path[1024] } */
#define	PSFA_HDR	8
#define	PSFA_MAXCOUNT	1024

struct spawn_req {
	struct image_args args;
	int	flags;
	uint32_t sigdef, sigmask;
	int	pgroup;
	char	*fa;			/* the file actions (kernel copy) */
	int	fa_count;
	int	done, error;
};

static struct mtx spawn_mtx;
static int spawn_inited;

static void
spawn_finish(struct spawn_req *rq, int error)
{
	mtx_lock(&spawn_mtx);
	rq->error = error;
	rq->done = 1;
	wakeup(rq);
	mtx_unlock(&spawn_mtx);
}

static int
spawn_apply(struct thread *td, struct spawn_req *rq)
{
	int i, error, fd, sig;

	if (rq->flags & PS_SETSID) {
		error = sys_setsid(td, NULL);
		if (error != 0)
			return (error);
	}
	if (rq->flags & PS_SETPGROUP) {
		struct setpgid_args pa = { .pid = 0, .pgid = rq->pgroup };

		error = sys_setpgid(td, &pa);
		if (error != 0)
			return (error);
	}
	if (rq->flags & PS_RESETIDS) {
		struct ucred *nc = crget();
		struct proc *p = td->td_proc;

		PROC_LOCK(p);
		crcopy(nc, p->p_ucred);
		nc->cr_uid = nc->cr_ruid;
		nc->cr_gid = nc->cr_rgid;
		proc_set_cred(p, nc);
		PROC_UNLOCK(p);
	}
	if (rq->flags & PS_SETSIGDEF) {
		for (sig = 1; sig <= 31; sig++) {
			struct sigaction sa;

			if (!(rq->sigdef & (1U << (sig - 1))) || sig == SIGKILL || sig == SIGSTOP)
				continue;
			memset(&sa, 0, sizeof(sa));
			sa.sa_handler = SIG_DFL;
			(void)kern_sigaction(td, sig, &sa, NULL, 0);
		}
	}
	if (rq->flags & PS_SETSIGMASK) {
		sigset_t set;

		SIGEMPTYSET(set);
		for (sig = 1; sig <= 31; sig++)
			if (rq->sigmask & (1U << (sig - 1)))
				SIGADDSET(set, sig);
		error = kern_sigprocmask(td, SIG_SETMASK, &set, NULL, 0);
		if (error != 0)
			return (error);
	}
	for (i = 0; i < rq->fa_count; i++) {
		const char *a = rq->fa + (size_t)i * PSFA_SIZE;
		int type, fdv, newfd;

		memcpy(&type, a, 4);
		memcpy(&fdv, a + 4, 4);
		switch (type) {
		case PSFA_OPEN: {
			int oflag, mode;
			uint16_t m16;
			char path[PATH_MAX];

			memcpy(&oflag, a + 8, 4);
			memcpy(&m16, a + 12, 2);
			mode = m16;
			memcpy(path, a + 14, PATH_MAX);
			path[PATH_MAX - 1] = 0;
			error = kern_openat(td, AT_FDCWD, path, UIO_SYSSPACE, darwin_oflags(oflag), mode);
			if (error != 0)
				return (error);
			fd = td->td_retval[0];
			if (fd != fdv) {
				error = kern_dup(td, FDDUP_FIXED, 0, fd, fdv);
				(void)kern_close(td, fd);
				if (error != 0)
					return (error);
			}
			break;
		}
		case PSFA_CLOSE:
			error = kern_close(td, fdv);
			if (error != 0)
				return (error);
			break;
		case PSFA_DUP2:
			memcpy(&newfd, a + 8, 4);
			if (newfd == fdv)
				error = kern_fcntl(td, fdv, F_SETFD, 0);
			else
				error = kern_dup(td, FDDUP_FIXED, 0, fdv, newfd);
			if (error != 0)
				return (error);
			break;
		case PSFA_INHERIT:
			error = kern_fcntl(td, fdv, F_SETFD, 0);
			if (error != 0)
				return (error);
			break;
		case PSFA_CHDIR: {
			char path[PATH_MAX];

			memcpy(path, a + 8, PATH_MAX);
			path[PATH_MAX - 1] = 0;
			error = kern_chdir(td, path, UIO_SYSSPACE);
			if (error != 0)
				return (error);
			break;
		}
		case PSFA_FCHDIR: {
			struct fchdir_args fa = { .fd = fdv };

			error = sys_fchdir(td, &fa);
			if (error != 0)
				return (error);
			break;
		}
		default:
			break;			/* fileport dup2 and the others: ignored */
		}
	}
	return (0);
}

/* the start function of the child; it returns to user mode when the exec worked */
static void
spawn_child(void *arg)
{
	struct spawn_req *rq = arg;
	struct thread *td = curthread;
	struct vmspace *oldvmspace;
	int error;

	memset(td->td_frame, 0, sizeof(*td->td_frame));
	error = spawn_apply(td, rq);
	if (error != 0) {
		exec_free_args(&rq->args);
		spawn_finish(rq, error);
		exit1(td, 127, 0);
		/* NOTREACHED */
	}
	oldvmspace = td->td_proc->p_vmspace;
	error = kern_execve(td, &rq->args, NULL, oldvmspace);
	if (error == EJUSTRETURN) {
		exec_cleanup(td, oldvmspace);
		spawn_finish(rq, 0);
		return;
	}
	spawn_finish(rq, error);
	exit1(td, 127, 0);
}

static int
spawn_collect(struct thread *td, struct spawn_req *rq, const char *path, uint64_t *desc, char **argv, char **envp)
{
	uint64_t attr_size = desc[0], attrp = desc[1], fa_size = desc[2], fap = desc[3];
	int error, n;

	memset(&rq->args, 0, sizeof(rq->args));
	error = exec_alloc_args(&rq->args);
	if (error != 0)
		return (error);
	error = exec_args_add_fname(&rq->args, path, UIO_USERSPACE);
	for (n = 0; error == 0 && argv != NULL; n++) {
		long v;

		if (fueword(argv + n, &v) != 0)
			error = EFAULT;
		else if (v == 0)
			break;
		else
			error = exec_args_add_arg(&rq->args, (char *)v, UIO_USERSPACE);
	}
	for (n = 0; error == 0 && envp != NULL; n++) {
		long v;

		if (fueword(envp + n, &v) != 0)
			error = EFAULT;
		else if (v == 0)
			break;
		else
			error = exec_args_add_env(&rq->args, (char *)v, UIO_USERSPACE);
	}
	if (error != 0) {
		exec_free_args(&rq->args);
		return (error);
	}
	if (attrp != 0 && attr_size >= 16) {
		char ab[16];
		int16_t fl;

		error = copyin((void *)attrp, ab, sizeof(ab));
		if (error == 0) {
			memcpy(&fl, ab, 2);
			rq->flags = (uint16_t)fl;
			memcpy(&rq->sigdef, ab + 4, 4);
			memcpy(&rq->sigmask, ab + 8, 4);
			memcpy(&rq->pgroup, ab + 12, 4);
		}
	}
	if (error == 0 && fap != 0 && fa_size >= PSFA_HDR) {
		int hdr[2];

		error = copyin((void *)fap, hdr, sizeof(hdr));
		if (error == 0 && (hdr[1] < 0 || hdr[1] > PSFA_MAXCOUNT || (uint64_t)hdr[1] * PSFA_SIZE + PSFA_HDR > fa_size))
			error = EINVAL;
		if (error == 0 && hdr[1] > 0) {
			rq->fa = malloc((size_t)hdr[1] * PSFA_SIZE, M_DSPAWN, M_WAITOK);
			rq->fa_count = hdr[1];
			error = copyin((char *)fap + PSFA_HDR, rq->fa, (size_t)hdr[1] * PSFA_SIZE);
		}
	}
	if (error != 0)
		exec_free_args(&rq->args);
	return (error);
}

static int
darwin_posix_spawn(struct thread *td, struct darwin_args *uap)
{
	struct spawn_req *rq;
	struct fork_req fr;
	struct proc *p2 = NULL;
	struct thread *td2;
	uint64_t desc[4] = { 0 };
	int error, pid;

	if (uap->a[2] != 0) {
		error = copyin((void *)uap->a[2], desc, sizeof(desc));
		if (error != 0)
			return (error);
	}
	rq = malloc(sizeof(*rq), M_DSPAWN, M_WAITOK | M_ZERO);
	error = spawn_collect(td, rq, (const char *)uap->a[1], desc, (char **)uap->a[3], (char **)uap->a[4]);
	if (error != 0)
		goto out;

	if (rq->flags & PS_SETEXEC) {		/* no new process: the exec of this one */
		struct vmspace *oldvmspace;

		error = spawn_apply(td, rq);
		if (error != 0) {
			exec_free_args(&rq->args);
			goto out;
		}
		error = pre_execve(td, &oldvmspace);
		if (error != 0) {
			exec_free_args(&rq->args);
			goto out;
		}
		error = kern_execve(td, &rq->args, NULL, oldvmspace);
		post_execve(td, error, oldvmspace);
		free(rq->fa, M_DSPAWN);
		free(rq, M_DSPAWN);
		return (error == EJUSTRETURN ? EJUSTRETURN : error);
	}

	memset(&fr, 0, sizeof(fr));
	fr.fr_flags = RFFDG | RFPROC | RFSTOPPED;
	fr.fr_procp = &p2;
	error = fork1(td, &fr);
	if (error != 0) {
		exec_free_args(&rq->args);
		goto out;
	}
	pid = p2->p_pid;
	td2 = FIRST_THREAD_IN_PROC(p2);
	cpu_fork_kthread_handler(td2, spawn_child, rq);
	mtx_lock(&spawn_mtx);
	rq->done = 0;
	mtx_unlock(&spawn_mtx);
	thread_lock(td2);
	TD_SET_CAN_RUN(td2);
	sched_add(td2, SRQ_BORING);
	mtx_lock(&spawn_mtx);
	while (!rq->done)
		mtx_sleep(rq, &spawn_mtx, PWAIT, "dspawn", 0);
	error = rq->error;
	mtx_unlock(&spawn_mtx);
	if (error == 0 && uap->a[0] != 0)
		error = copyout(&pid, (void *)uap->a[0], sizeof(pid));
out:
	free(rq->fa, M_DSPAWN);
	free(rq, M_DSPAWN);
	return (error);
}

void
darwin_spawn_syscalls_init(void)
{
	if (!spawn_inited) {
		mtx_init(&spawn_mtx, "darwin spawn", NULL, MTX_DEF);
		spawn_inited = 1;
	}
	darwin_sysent[244] = (struct sysent){ .sy_narg = 5, .sy_call = (sy_call_t *)darwin_posix_spawn, .sy_auevent = AUE_POSIX_SPAWN, .sy_thrcnt = SY_THR_STATIC };
	darwin_syscallnames[244] = "posix_spawn";
}
