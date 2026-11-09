/*
 * Process information in Darwin's layouts (the offsets were read on a real Mac, tools/macprobe): sysctl KERN_PROC (kinfo_proc, 648 bytes: extern_proc 296 + eproc 352),
 * sysctl KERN_PROCARGS2, and the proc_info call that libproc is made of (listpids, pidinfo: bsdinfo 136, shortbsdinfo 64, taskinfo 96, taskallinfo 232, pidpath).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/ucred.h>
#include <sys/jail.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"

#define	DKP_SIZE	648
#define	DKP_EPROC	296

static void put16(char *b, int off, uint16_t v) { memcpy(b + off, &v, 2); }
static void put32(char *b, int off, uint32_t v) { memcpy(b + off, &v, 4); }
static void put64(char *b, int off, uint64_t v) { memcpy(b + off, &v, 8); }

static int
darwin_p_stat(struct proc *p)
{
	if (p->p_state == PRS_NEW)
		return (1);			/* SIDL */
	if (p->p_state == PRS_ZOMBIE)
		return (5);			/* SZOMB */
	if (P_SHOULDSTOP(p))
		return (4);			/* SSTOP */
	return (2);				/* SRUN */
}

/* the process is locked */
static void
fill_kinfo(struct proc *p, char *b)
{
	struct ucred *cr = p->p_ucred;
	struct pgrp *pg = p->p_pgrp;
	int i, n;

	memset(b, 0, DKP_SIZE);
	if (p->p_stats != NULL) {
		put64(b, 0, (uint64_t)p->p_stats->p_start.tv_sec);
		put32(b, 8, (uint32_t)p->p_stats->p_start.tv_usec);
	}
	put32(b, 32, 0x4 | ((p->p_flag & P_CONTROLT) ? 0x2 : 0));	/* P_LP64, P_CONTROLT */
	b[36] = (char)darwin_p_stat(p);
	put32(b, 40, (uint32_t)p->p_pid);
	put32(b, 44, (uint32_t)(p->p_pptr != NULL ? p->p_pptr->p_pid : 0));
	b[240] = 0; b[242] = (char)p->p_nice;
	strlcpy(b + 243, p->p_comm, 17);
	put16(b, 280, (uint16_t)p->p_xsig);
	/* eproc */
	put32(b, DKP_EPROC + 264, (uint32_t)(p->p_pptr != NULL ? p->p_pptr->p_pid : 0));
	put32(b, DKP_EPROC + 268, (uint32_t)(pg != NULL ? pg->pg_id : 0));
	put32(b, DKP_EPROC + 276, 0xffffffffu);			/* e_tdev: NODEV */
	put32(b, DKP_EPROC + 280, (uint32_t)(pg != NULL ? pg->pg_id : 0));
	if (cr != NULL) {
		put32(b, DKP_EPROC + 16 + 80, cr->cr_ruid);		/* e_pcred.p_ruid */
		put32(b, DKP_EPROC + 16 + 84, cr->cr_svuid);
		put32(b, DKP_EPROC + 16 + 88, cr->cr_rgid);
		put32(b, DKP_EPROC + 16 + 92, cr->cr_svgid);
		put32(b, DKP_EPROC + 120 + 0, 1);			/* e_ucred.cr_ref */
		put32(b, DKP_EPROC + 120 + 4, cr->cr_uid);
		n = MIN(cr->cr_ngroups, 16);
		put16(b, DKP_EPROC + 120 + 8, (uint16_t)n);
		for (i = 0; i < n; i++)
			put32(b, DKP_EPROC + 120 + 12 + 4 * i, cr->cr_groups[i]);
	}
}

struct kproc_list {
	char	*buf;
	int	n, cap;
};

/* gathers the processes that match (what, arg): ALL 0, PID 1, PGRP 2, TTY 4, UID 5, RUID 6 */
static int
collect_procs(struct thread *td, int what, int arg, struct kproc_list *l)
{
	struct proc *p;

	sx_slock(&allproc_lock);
	FOREACH_PROC_IN_SYSTEM(p) {
		bool ok = false;

		PROC_LOCK(p);
		if (p->p_state != PRS_NEW && p_cansee(td, p) == 0) {
			switch (what) {
			case 0: ok = true; break;
			case 1: ok = p->p_pid == arg; break;
			case 2: ok = p->p_pgrp != NULL && p->p_pgrp->pg_id == arg; break;
			case 5: ok = p->p_ucred != NULL && p->p_ucred->cr_uid == (uid_t)arg; break;
			case 6: ok = p->p_ucred != NULL && p->p_ucred->cr_ruid == (uid_t)arg; break;
			default: break;
			}
		}
		if (ok && l->n < l->cap) {
			fill_kinfo(p, l->buf + (size_t)l->n * DKP_SIZE);
			l->n++;
		}
		PROC_UNLOCK(p);
	}
	sx_sunlock(&allproc_lock);
	return (0);
}

int
darwin_sysctl_proc(struct thread *td, int *mib, u_int namelen, void *old, void *uoldlenp, bool writing)
{
	struct kproc_list l;
	size_t have = 0, need;
	int error, what, arg = 0;

	if (writing)
		return (EPERM);
	if (namelen < 3)
		return (ENOENT);
	what = mib[2];
	if (what != 0 && namelen < 4)
		return (EINVAL);
	if (what != 0)
		arg = mib[3];
	if (what == 3 || what > 6)
		return (EINVAL);
	if (uoldlenp != NULL && (error = copyin(uoldlenp, &have, sizeof(have))) != 0)
		return (error);
	l.cap = old == NULL ? 4096 : (int)MIN((size_t)(have / DKP_SIZE) + 1, 4096);
	l.n = 0;
	l.buf = malloc((size_t)l.cap * DKP_SIZE, M_DARWIN, M_WAITOK);
	collect_procs(td, what, arg, &l);
	need = (size_t)l.n * DKP_SIZE;
	if (old == NULL) {
		need += 5 * DKP_SIZE;			/* room for the processes that start meanwhile: ps asks twice */
		error = uoldlenp != NULL ? copyout(&need, uoldlenp, sizeof(need)) : 0;
	} else if (have < need) {
		size_t fit = (have / DKP_SIZE) * DKP_SIZE;

		error = copyout(l.buf, old, fit);
		if (error == 0 && uoldlenp != NULL)
			error = copyout(&fit, uoldlenp, sizeof(fit));
		if (error == 0)
			error = ENOMEM;
	} else {
		error = copyout(l.buf, old, need);
		if (error == 0 && uoldlenp != NULL)
			error = copyout(&need, uoldlenp, sizeof(need));
	}
	free(l.buf, M_DARWIN);
	return (error);
}

/* KERN_PROCARGS2: argc (int), the path of the executable, NULs, argv; the environment is not given */
int
darwin_sysctl_procargs2(struct thread *td, int pid, void *old, void *uoldlenp)
{
	struct proc *p;
	char *args, *path, *out;
	size_t have = 0, alen = 0, plen = 0, o = 0;
	int mib[4], argc = 0, error;
	size_t i;

	if (uoldlenp != NULL && (error = copyin(uoldlenp, &have, sizeof(have))) != 0)
		return (error);
	if ((p = pfind(pid)) == NULL)
		return (ESRCH);
	error = p_cansee(td, p);
	PROC_UNLOCK(p);
	if (error != 0)
		return (ESRCH);
	args = malloc(ARG_MAX, M_DARWIN, M_WAITOK | M_ZERO);
	path = malloc(MAXPATHLEN, M_DARWIN, M_WAITOK | M_ZERO);
	mib[0] = CTL_KERN; mib[1] = KERN_PROC; mib[2] = KERN_PROC_ARGS; mib[3] = pid;
	alen = ARG_MAX;
	if (kernel_sysctl(td, mib, 4, args, &alen, NULL, 0, &alen, 0) != 0)
		alen = 0;
	mib[2] = KERN_PROC_PATHNAME;
	plen = MAXPATHLEN;
	if (kernel_sysctl(td, mib, 4, path, &plen, NULL, 0, &plen, 0) != 0 || plen == 0)
		strlcpy(path, "?", MAXPATHLEN);
	for (i = 0; i < alen; i++)
		if (args[i] == '\0')
			argc++;
	out = malloc(4 + strlen(path) + 1 + 4 + alen + 1, M_DARWIN, M_WAITOK | M_ZERO);
	memcpy(out, &argc, 4); o = 4;
	memcpy(out + o, path, strlen(path) + 1); o += strlen(path) + 1;
	o = roundup2(o, 4);			/* the Mac pads the path with NULs */
	memcpy(out + o, args, alen); o += alen;
	if (old == NULL)
		error = uoldlenp != NULL ? copyout(&o, uoldlenp, sizeof(o)) : 0;
	else {
		size_t n = MIN(o, have);

		error = copyout(out, old, n);
		if (error == 0 && uoldlenp != NULL)
			error = copyout(&n, uoldlenp, sizeof(n));
		if (error == 0 && n < o)
			error = ENOMEM;
	}
	free(out, M_DARWIN); free(path, M_DARWIN); free(args, M_DARWIN);
	return (error);
}

/* ---- proc_info(callnum, pid, flavor, arg, buffer, buffersize) ---- */

static void
fill_bsdinfo(struct proc *p, char *b)
{
	struct ucred *cr = p->p_ucred;

	memset(b, 0, 136);
	put32(b, 4, (uint32_t)darwin_p_stat(p));
	put32(b, 12, (uint32_t)p->p_pid);
	put32(b, 16, (uint32_t)(p->p_pptr != NULL ? p->p_pptr->p_pid : 0));
	if (cr != NULL) {
		put32(b, 20, cr->cr_uid); put32(b, 24, cr->cr_gid); put32(b, 28, cr->cr_ruid); put32(b, 32, cr->cr_rgid);
		put32(b, 36, cr->cr_svuid); put32(b, 40, cr->cr_svgid);
	}
	strlcpy(b + 48, p->p_comm, 16);
	strlcpy(b + 64, p->p_comm, 32);
	put32(b, 100, (uint32_t)(p->p_pgrp != NULL ? p->p_pgrp->pg_id : 0));
	put32(b, 108, 0xffffffffu);
	put32(b, 116, (uint32_t)(int32_t)p->p_nice);
	if (p->p_stats != NULL) {
		put64(b, 120, (uint64_t)p->p_stats->p_start.tv_sec);
		put64(b, 128, (uint64_t)p->p_stats->p_start.tv_usec);
	}
}

static void
fill_taskinfo(struct proc *p, char *b)
{
	memset(b, 0, 96);
	if (p->p_vmspace != NULL) {
		put64(b, 0, (uint64_t)p->p_vmspace->vm_map.size);
		put64(b, 8, (uint64_t)pmap_resident_count(vmspace_pmap(p->p_vmspace)) * PAGE_SIZE);
	}
	put32(b, 84, (uint32_t)p->p_numthreads);
	put32(b, 88, 1);
}

static int
darwin_proc_info(struct thread *td, struct darwin_args *uap)
{
	int callnum = (int)uap->a[0], pid = (int)uap->a[1], flavor = (int)uap->a[2];
	char *ubuf = (char *)uap->a[4];
	int bufsize = (int)uap->a[5], error = 0;
	struct proc *p;
	char buf[240];

	if (callnum == 1) {				/* PROC_INFO_CALL_LISTPIDS: (type, typeinfo): the pids that are there */
		struct kproc_list l;
		int *pids, i, n = 0;

		l.cap = 4096; l.n = 0;
		l.buf = malloc((size_t)l.cap * DKP_SIZE, M_DARWIN, M_WAITOK);
		collect_procs(td, 0, 0, &l);
		pids = malloc(sizeof(int) * (l.n + 1), M_DARWIN, M_WAITOK);
		for (i = 0; i < l.n; i++) {
			uint32_t v;

			memcpy(&v, l.buf + (size_t)i * DKP_SIZE + 40, 4);
			if (pid == 1 /* PROC_ALL_PIDS */ || (pid == 6 && 0))
				pids[n++] = (int)v;
		}
		if (ubuf == NULL)
			td->td_retval[0] = n * sizeof(int);
		else {
			int m = MIN(n, bufsize / (int)sizeof(int));

			error = copyout(pids, ubuf, m * sizeof(int));
			td->td_retval[0] = m * sizeof(int);
		}
		free(pids, M_DARWIN); free(l.buf, M_DARWIN);
		return (error);
	}
	if (callnum == 15) {				/* what Keka asks first (a Mac answers 0) */
		td->td_retval[0] = 0;
		return (0);
	}
	if (callnum != 2)				/* PROC_INFO_CALL_PIDINFO only (setcontrol, pidfdinfo, kernmsgbuf ... are not here) */
		return (ENOTSUP);
	if ((p = pfind(pid)) == NULL)
		return (ESRCH);
	if (p_cansee(td, p) != 0) {
		PROC_UNLOCK(p);
		return (ESRCH);
	}
	switch (flavor) {
	case 3:						/* PROC_PIDTBSDINFO */
		fill_bsdinfo(p, buf); PROC_UNLOCK(p);
		if (bufsize < 136) return (ENOMEM);
		error = copyout(buf, ubuf, 136);
		td->td_retval[0] = 136;
		return (error);
	case 13: {					/* PROC_PIDT_SHORTBSDINFO: pid, ppid, pgid, status, comm[16], flags, uid, gid, ruid, rgid, svuid, svgid, rfu */
		char b2[136];

		fill_bsdinfo(p, b2); PROC_UNLOCK(p);
		memset(buf, 0, 64);
		memcpy(buf + 0, b2 + 12, 4); memcpy(buf + 4, b2 + 16, 4); memcpy(buf + 8, b2 + 100, 4); memcpy(buf + 12, b2 + 4, 4);
		memcpy(buf + 16, b2 + 48, 16); memcpy(buf + 36, b2 + 20, 4); memcpy(buf + 40, b2 + 24, 4); memcpy(buf + 44, b2 + 28, 4);
		memcpy(buf + 48, b2 + 32, 4); memcpy(buf + 52, b2 + 36, 4); memcpy(buf + 56, b2 + 40, 4);
		if (bufsize < 64) return (ENOMEM);
		error = copyout(buf, ubuf, 64);
		td->td_retval[0] = 64;
		return (error);
	}
	case 4:						/* PROC_PIDTASKINFO */
		fill_taskinfo(p, buf); PROC_UNLOCK(p);
		if (bufsize < 96) return (ENOMEM);
		error = copyout(buf, ubuf, 96);
		td->td_retval[0] = 96;
		return (error);
	case 2: {					/* PROC_PIDTASKALLINFO: bsdinfo then taskinfo */
		char all[232];

		fill_bsdinfo(p, all); fill_taskinfo(p, all + 136); PROC_UNLOCK(p);
		if (bufsize < 232) return (ENOMEM);
		error = copyout(all, ubuf, 232);
		td->td_retval[0] = 232;
		return (error);
	}
	case 11: {					/* PROC_PIDPATHINFO */
		char *path = malloc(MAXPATHLEN, M_DARWIN, M_WAITOK | M_ZERO);
		size_t plen = MAXPATHLEN;
		int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, pid };

		PROC_UNLOCK(p);
		if (kernel_sysctl(td, mib, 4, path, &plen, NULL, 0, &plen, 0) != 0 || path[0] == 0)
			error = ESRCH;
		else if (bufsize <= (int)strlen(path))
			error = ENOMEM;
		else {
			error = copyout(path, ubuf, strlen(path) + 1);
			td->td_retval[0] = strlen(path);
		}
		free(path, M_DARWIN);
		return (error);
	}
	default:
		PROC_UNLOCK(p);
		return (ENOTSUP);
	}
}

void
darwin_proc_syscalls_init(void)
{
	darwin_sysent[336] = (struct sysent){ .sy_narg = 6, .sy_call = (sy_call_t *)darwin_proc_info, .sy_auevent = AUE_PROCINFO, .sy_thrcnt = SY_THR_STATIC };
	darwin_syscallnames[336] = "proc_info";
}
