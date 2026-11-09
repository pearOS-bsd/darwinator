/*
 * The calls that are left and can be answered: POSIX AIO (done at once, in the call: the result is kept until aio_return), terminate_with_payload and
 * abort_with_payload, pid_suspend/pid_resume, open_dprotected_np, initgroups, memorystatus_available_memory, getauid/setauid, and the privileged system
 * management calls (mount, unmount, reboot, acct, swapon, quotactl ...), which fail with EPERM as they do for a user who is not allowed to.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/signalvar.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/vmmeter.h>
#include <sys/ucred.h>
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"

/* ---- POSIX AIO ---- */

#define	AIO_N		256
static struct { bool used; pid_t pid; uint64_t ptr; int64_t result; int err; } aio_tab[AIO_N];
static struct mtx aio_mtx;
static bool aio_inited;

static void
aio_put(uint64_t ptr, int64_t result, int err)
{
	int i, free_i = -1;

	mtx_lock(&aio_mtx);
	for (i = 0; i < AIO_N; i++) {
		if (aio_tab[i].used && aio_tab[i].pid == curproc->p_pid && aio_tab[i].ptr == ptr) { free_i = i; break; }
		if (!aio_tab[i].used && free_i < 0)
			free_i = i;
	}
	if (free_i < 0)
		free_i = ptr % AIO_N;				/* the table is full: the oldest slot of this address is used */
	aio_tab[free_i].used = true; aio_tab[free_i].pid = curproc->p_pid; aio_tab[free_i].ptr = ptr;
	aio_tab[free_i].result = result; aio_tab[free_i].err = err;
	mtx_unlock(&aio_mtx);
}

static int
aio_find(uint64_t ptr, int64_t *result, int *err, bool take)
{
	int i, found = ENOENT;

	mtx_lock(&aio_mtx);
	for (i = 0; i < AIO_N; i++)
		if (aio_tab[i].used && aio_tab[i].pid == curproc->p_pid && aio_tab[i].ptr == ptr) {
			*result = aio_tab[i].result; *err = aio_tab[i].err;
			if (take)
				aio_tab[i].used = false;
			found = 0;
			break;
		}
	mtx_unlock(&aio_mtx);
	return (found);
}

/* one operation of an aiocb (fildes 0, offset 8, buf 16, nbytes 24, lio_opcode 72) */
static int
aio_do(struct thread *td, uint64_t ptr, int op)		/* op: 1 read, 2 write, 3 fsync (op 0: the one in the aiocb) */
{
	uint8_t cb[80];
	int fd, error;
	int64_t off, res = -1;
	uint64_t buf, nbytes;

	error = copyin((void *)(uintptr_t)ptr, cb, sizeof(cb));
	if (error != 0)
		return (error);
	memcpy(&fd, cb, 4); memcpy(&off, cb + 8, 8); memcpy(&buf, cb + 16, 8); memcpy(&nbytes, cb + 24, 8);
	if (op == 0)
		memcpy(&op, cb + 72, 4);
	switch (op) {
	case 1:
		error = kern_pread(td, fd, (void *)(uintptr_t)buf, nbytes, off);
		if (error == ESPIPE) {
			struct read_args ra = { .fd = fd, .buf = (void *)(uintptr_t)buf, .nbyte = nbytes };

			error = sys_read(td, &ra);
		}
		break;
	case 2:
		error = kern_pwrite(td, fd, (void *)(uintptr_t)buf, nbytes, off);
		if (error == ESPIPE) {
			struct write_args wa = { .fd = fd, .buf = (const void *)(uintptr_t)buf, .nbyte = nbytes };

			error = sys_write(td, &wa);
		}
		break;
	case 3: {
		struct fsync_args fa = { .fd = fd };

		error = sys_fsync(td, &fa);
		td->td_retval[0] = 0;
		break;
	}
	default:
		return (0);			/* LIO_NOP */
	}
	if (error == 0)
		res = td->td_retval[0];
	aio_put(ptr, res, darwin_errno(error));
	td->td_retval[0] = 0;
	return (0);
}

static int darwin_aio_read(struct thread *td, struct darwin_args *uap) { return (aio_do(td, uap->a[0], 1)); }
static int darwin_aio_write(struct thread *td, struct darwin_args *uap) { return (aio_do(td, uap->a[0], 2)); }
static int darwin_aio_fsync(struct thread *td, struct darwin_args *uap) { return (aio_do(td, uap->a[1], 3)); }

static int
darwin_aio_return(struct thread *td, struct darwin_args *uap)
{
	int64_t r; int e;

	if (aio_find(uap->a[0], &r, &e, true) != 0)
		return (EINVAL);
	td->td_retval[0] = (register_t)r;
	return (0);
}

static int
darwin_aio_error(struct thread *td, struct darwin_args *uap)
{
	int64_t r; int e;

	if (aio_find(uap->a[0], &r, &e, false) != 0)
		return (EINVAL);
	td->td_retval[0] = e;			/* the error number is the value of the call */
	return (0);
}

static int
darwin_aio_cancel(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = 2;			/* AIO_ALLDONE: everything is done already */
	return (0);
}

static int
darwin_aio_suspend(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = 0;
	return (0);
}

static int
darwin_lio_listio(struct thread *td, struct darwin_args *uap)	/* (mode, list[], nent, sigevent) */
{
	int i, n = (int)uap->a[2], error = 0;

	if (n < 0 || n > 64)
		return (EINVAL);
	for (i = 0; i < n && error == 0; i++) {
		uint64_t p;

		if (copyin((uint64_t *)uap->a[1] + i, &p, 8) != 0)
			return (EFAULT);
		if (p != 0)
			error = aio_do(td, p, 0);
	}
	td->td_retval[0] = 0;
	return (error);
}

/* ---- the rest ---- */

static int darwin_m_ok(struct thread *td, struct darwin_args *uap __unused) { td->td_retval[0] = 0; return (0); }
static int darwin_m_eperm(struct thread *td __unused, struct darwin_args *uap __unused) { return (EPERM); }
static int darwin_m_enotsup(struct thread *td __unused, struct darwin_args *uap __unused) { return (ENOTSUP); }

/* terminate_with_payload(pid, namespace, code, payload, size, string, flags): SIGKILL */
static int
darwin_terminate_with_payload(struct thread *td, struct darwin_args *uap)
{
	struct kill_args ka = { .pid = (int)uap->a[0], .signum = SIGKILL };

	return (sys_kill(td, &ka));
}

/* abort_with_payload(...): the process ends with SIGABRT */
static int
darwin_abort_with_payload(struct thread *td, struct darwin_args *uap __unused)
{
	struct proc *p = td->td_proc;

	PROC_LOCK(p);
	kern_psignal(p, SIGABRT);
	PROC_UNLOCK(p);
	return (0);
}

static int
darwin_pid_suspend(struct thread *td, struct darwin_args *uap)
{
	struct kill_args ka = { .pid = (int)uap->a[0], .signum = SIGSTOP };

	return (sys_kill(td, &ka));
}

static int
darwin_pid_resume(struct thread *td, struct darwin_args *uap)
{
	struct kill_args ka = { .pid = (int)uap->a[0], .signum = SIGCONT };

	return (sys_kill(td, &ka));
}

/* open_dprotected_np(path, flags, class, dpflags, mode) */
static int
darwin_open_dprotected(struct thread *td, struct darwin_args *uap)
{
	return (kern_openat(td, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, darwin_oflags((int)uap->a[1]), (int)uap->a[4]));
}

/* initgroups(ngroups, gidset, gmuid) */
static int
darwin_initgroups(struct thread *td, struct darwin_args *uap)
{
	struct setgroups_args sa = { .gidsetsize = (u_int)uap->a[0], .gidset = (gid_t *)uap->a[1] };

	return (sys_setgroups(td, &sa));
}

/* memorystatus_available_memory(): the bytes that are free */
static int
darwin_memorystatus_available(struct thread *td, struct darwin_args *uap __unused)
{
	td->td_retval[0] = (register_t)((uint64_t)vm_free_count() * PAGE_SIZE);
	return (0);
}

/* getauid(au_id_t *): AU_DEFAUDITID */
static int
darwin_getauid(struct thread *td __unused, struct darwin_args *uap)
{
	uint32_t v = 0xffffffffu;

	return (copyout(&v, (void *)uap->a[0], 4));
}

static const struct {
	int		num, nargs;
	sy_call_t	*fn;
	const char	*name;
} darwin_misc[] = {
#define	E(num, nargs, fn, name)	{ (num), (nargs), (sy_call_t *)(fn), (name) }
	E(430, 2, darwin_fileport_makeport,	"fileport_makeport"),
	E(431, 1, darwin_fileport_makefd,	"fileport_makefd"),
	E(334, 6, darwin_semwait_signal,	"__semwait_signal"),
	E(423, 6, darwin_semwait_signal,	"__semwait_signal_nocancel"),
	E(313, 2, darwin_aio_fsync,		"aio_fsync"),
	E(314, 1, darwin_aio_return,		"aio_return"),
	E(315, 3, darwin_aio_suspend,		"aio_suspend"),
	E(316, 2, darwin_aio_cancel,		"aio_cancel"),
	E(317, 1, darwin_aio_error,		"aio_error"),
	E(318, 1, darwin_aio_read,		"aio_read"),
	E(319, 1, darwin_aio_write,		"aio_write"),
	E(320, 4, darwin_lio_listio,		"lio_listio"),
	E(421, 3, darwin_aio_suspend,		"aio_suspend_nocancel"),
	E(520, 7, darwin_terminate_with_payload, "terminate_with_payload"),
	E(521, 6, darwin_abort_with_payload,	"abort_with_payload"),
	E(529, 5, darwin_m_ok,			"os_fault_with_payload"),
	E(433, 1, darwin_pid_suspend,		"pid_suspend"),
	E(434, 1, darwin_pid_resume,		"pid_resume"),
	E(216, 5, darwin_open_dprotected,	"open_dprotected_np"),
	E(243, 3, darwin_initgroups,		"initgroups"),
	E(534, 0, darwin_memorystatus_available, "memorystatus_available_memory"),
	E(353, 1, darwin_getauid,		"getauid"),
	E(354, 1, darwin_m_ok,			"setauid"),
	E(533, 2, darwin_m_ok,			"log_data"),
	E(530, 3, darwin_m_ok,			"kqueue_workloop_ctl"),
	E(535, 3, darwin_m_ok,			"objc_bp_assist_cfg_np"),
	E(152, 1, darwin_m_ok,			"setprivexec"),
	/* system management: not for this user */
	E(51,  1, darwin_m_eperm,		"acct"),
	E(55,  2, darwin_m_eperm,		"reboot"),
	E(85,  1, darwin_m_eperm,		"swapon"),
	E(159, 2, darwin_m_eperm,		"unmount"),
	E(164, 2, darwin_m_eperm,		"funmount"),
	E(165, 4, darwin_m_eperm,		"quotactl"),
	E(167, 4, darwin_m_eperm,		"mount"),
	E(526, 4, darwin_m_eperm,		"fmount"),
	E(537, 2, darwin_m_eperm,		"pivot_root"),
	E(439, 3, darwin_m_eperm,		"kas_info"),
	E(455, 1, darwin_m_eperm,		"vfs_purge"),
	E(454, 2, darwin_m_eperm,		"system_override"),
	E(445, 3, darwin_m_enotsup,		"usrctl"),
	E(451, 3, darwin_m_enotsup,		"telemetry"),
	E(456, 2, darwin_m_enotsup,		"sfi_ctl"),
	E(457, 2, darwin_m_enotsup,		"sfi_pidctl"),
#undef E
};

void
darwin_misc_syscalls_init(void)
{
	u_int i;

	if (!aio_inited) {
		mtx_init(&aio_mtx, "darwin aio", NULL, MTX_DEF);
		aio_inited = true;
	}
	for (i = 0; i < nitems(darwin_misc); i++) {
		int n = darwin_misc[i].num;

		if (darwin_sysent[n].sy_call != NULL)
			continue;
		darwin_sysent[n] = (struct sysent){ .sy_narg = darwin_misc[i].nargs, .sy_call = darwin_misc[i].fn,
		    .sy_auevent = AUE_NULL, .sy_thrcnt = SY_THR_STATIC };
		darwin_syscallnames[n] = darwin_misc[i].name;
	}
}
