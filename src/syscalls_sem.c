/*
 * Named POSIX semaphores (Darwin sem_open 268, sem_close 269, sem_unlink 270, sem_wait 271, sem_trywait 272, sem_post 273) on FreeBSD's ksem,
 * and access_extended (284).  The handle sem_open returns is the ksem id (libSystem hands it back as the sem_t *).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/ksem.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/malloc.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"

/* sem_open(name, oflag, mode, value): FreeBSD gives the id through a pointer, so a few bytes of the user stack (below the red zone) carry it */
static int
darwin_sem_open(struct thread *td, struct darwin_args *uap)
{
	struct ksem_open_args oa;
	semid_t id = 0;
	uintptr_t sp;
	int error;

#if defined(__amd64__)
	sp = td->td_frame->tf_rsp;
#else
	sp = td->td_frame->tf_sp;
#endif
	sp = (sp - 512) & ~(uintptr_t)15;
	oa.name = (const char *)uap->a[0];
	oa.oflag = darwin_oflags((int)uap->a[1]) & (O_CREAT | O_EXCL);
	oa.mode = (mode_t)uap->a[2];
	oa.value = (unsigned)uap->a[3];
	oa.idp = (semid_t *)sp;
	error = sys_ksem_open(td, &oa);
	if (error != 0)
		return (error);
	error = copyin((void *)sp, &id, sizeof(id));
	if (error != 0)
		return (error);
	td->td_retval[0] = (register_t)(uint32_t)id;
	return (0);
}

static int
darwin_sem_close(struct thread *td, struct darwin_args *uap)
{
	struct ksem_close_args a = { .id = (semid_t)uap->a[0] };

	return (sys_ksem_close(td, &a));
}

static int
darwin_sem_unlink(struct thread *td, struct darwin_args *uap)
{
	struct ksem_unlink_args a = { .name = (const char *)uap->a[0] };

	return (sys_ksem_unlink(td, &a));
}

static int
darwin_sem_wait(struct thread *td, struct darwin_args *uap)
{
	struct ksem_wait_args a = { .id = (semid_t)uap->a[0] };

	return (sys_ksem_wait(td, &a));
}

static int
darwin_sem_trywait(struct thread *td, struct darwin_args *uap)
{
	struct ksem_trywait_args a = { .id = (semid_t)uap->a[0] };

	return (sys_ksem_trywait(td, &a));
}

static int
darwin_sem_post(struct thread *td, struct darwin_args *uap)
{
	struct ksem_post_args a = { .id = (semid_t)uap->a[0] };

	return (sys_ksem_post(td, &a));
}

/*
 * access_extended(entries, size, results, uid): entries is an array of {uint32 ad_name_offset; int ad_flags; int ad_pad[2]} (16 bytes) followed by the
 * names; the first offset says where the array ends.  results[i] is the errno of entry i.  (ACLs are not checked: R_OK W_OK X_OK only.)
 */
static int
darwin_access_extended(struct thread *td, struct darwin_args *uap)
{
	size_t size = (size_t)uap->a[1];
	char *buf;
	int *res, n, i, error;
	uint32_t first;

	if (size < 16 || size > 65536)
		return (EINVAL);
	buf = malloc(size + 1, M_DARWIN, M_WAITOK);
	error = copyin((void *)uap->a[0], buf, size);
	if (error != 0)
		goto out;
	buf[size] = 0;
	memcpy(&first, buf, 4);
	if (first < 16 || first > size || first % 16 != 0) {
		error = EINVAL;
		goto out;
	}
	n = first / 16;
	res = malloc(n * sizeof(int), M_DARWIN, M_WAITOK | M_ZERO);
	for (i = 0; i < n; i++) {
		uint32_t off;
		int flags;

		memcpy(&off, buf + i * 16, 4);
		memcpy(&flags, buf + i * 16 + 4, 4);
		if (off >= size) {
			res[i] = EINVAL;
			continue;
		}
		res[i] = kern_accessat(td, AT_FDCWD, buf + off, UIO_SYSSPACE, 0, flags & 7);
		res[i] = darwin_errno(res[i]);
	}
	error = copyout(res, (void *)uap->a[2], n * sizeof(int));
	free(res, M_DARWIN);
out:
	free(buf, M_DARWIN);
	return (error);
}

void
darwin_sem_syscalls_init(void)
{
	static const struct { int num, nargs; sy_call_t *fn; int au; const char *name; } t[] = {
		{ 268, 4, (sy_call_t *)darwin_sem_open, AUE_SEMOPEN, "sem_open" },
		{ 269, 1, (sy_call_t *)darwin_sem_close, AUE_SEMCLOSE, "sem_close" },
		{ 270, 1, (sy_call_t *)darwin_sem_unlink, AUE_SEMUNLINK, "sem_unlink" },
		{ 271, 1, (sy_call_t *)darwin_sem_wait, AUE_NULL, "sem_wait" },
		{ 272, 1, (sy_call_t *)darwin_sem_trywait, AUE_NULL, "sem_trywait" },
		{ 273, 1, (sy_call_t *)darwin_sem_post, AUE_NULL, "sem_post" },
		{ 284, 4, (sy_call_t *)darwin_access_extended, AUE_ACCESS_EXTENDED, "access_extended" },
	};
	u_int i;

	for (i = 0; i < nitems(t); i++) {
		darwin_sysent[t[i].num] = (struct sysent){ .sy_narg = t[i].nargs, .sy_call = t[i].fn, .sy_auevent = t[i].au, .sy_thrcnt = SY_THR_STATIC };
		darwin_syscallnames[t[i].num] = t[i].name;
	}
}
