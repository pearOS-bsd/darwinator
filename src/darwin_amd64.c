/*
 * Darwin x86-64 system call convention: "syscall" with the class in the top byte of rax and the number in the
 * rest (BSD calls are 0x2000000 + n); arguments in rdi, rsi, rdx, r10, r8, r9 and then on the stack; the result
 * is in rax (rdx) with the carry flag set on error.  FreeBSD's trap frame keeps r10 in tf_rcx, so the six
 * argument registers are consecutive there, and FreeBSD's return convention (carry flag, errno in rax) is the
 * same, so only the fetching of the number differs.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysent.h>
#include <sys/syscall.h>
#include <machine/frame.h>
#include <machine/pcb.h>
#include <x86/x86_var.h>

#include <bsm/audit_kevents.h>

#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/lock.h>
#include <sys/signalvar.h>
#include "darwinator.h"

#define	NARGREGS_DARWIN	6

int
darwin_fetch_syscall_args(struct thread *td)
{
	struct trapframe *frame = td->td_frame;
	struct syscall_args *sa = &td->td_sa;
	uint64_t rax = frame->tf_rax;
	int error = 0;
	bool native_sigreturn = false;

	sa->original_code = (u_int)rax;
	sa->code = (u_int)(rax & 0xffffff);
	switch ((rax >> 24) & 0xff) {
	case DARWIN_CLASS_UNIX:
		if (sa->code >= DARWIN_MACHDEP_BASE)	/* the numbers from the base up are the machdep calls' */
			sa->code = (u_int)darwin_nsyscalls;
		break;
	case DARWIN_CLASS_MACHDEP:
		sa->code = sa->code < 64 ? sa->code + DARWIN_MACHDEP_BASE : (u_int)darwin_nsyscalls;
		break;
	case DARWIN_CLASS_MACH:		/* Mach traps: 0x1000000 | n */
		sa->code = sa->code < 128 ? sa->code + DARWIN_MACH_BASE : (u_int)darwin_nsyscalls;
		break;
	case 0:					/* no class: the signal trampoline of the kernel returning with FreeBSD's sigreturn */
		if (sa->code == SYS_sigreturn)
			native_sigreturn = true;
		else
			sa->code = (u_int)darwin_nsyscalls;
		break;
	default:				/* Mach traps and diag calls are not implemented yet */
		sa->code = (u_int)darwin_nsyscalls;
		break;
	}
	if (native_sigreturn)
		sa->callp = &sysent[SYS_sigreturn];
	else if (sa->code >= (u_int)darwin_nsyscalls)
		sa->callp = &nosys_sysent;
	else
		sa->callp = &darwin_sysent[sa->code];

	memcpy(sa->args, &frame->tf_rdi, sizeof(sa->args[0]) * NARGREGS_DARWIN);
	if (sa->callp->sy_narg > NARGREGS_DARWIN) {
		error = copyin((caddr_t)frame->tf_rsp + sizeof(register_t), &sa->args[NARGREGS_DARWIN],
		    (sa->callp->sy_narg - NARGREGS_DARWIN) * sizeof(sa->args[0]));
		if (error != 0)
			return (error);
	}
	td->td_retval[0] = 0;
	td->td_retval[1] = frame->tf_rdx;
	return (0);
}

/*
 * Machdep class (3), number 3: thread_fast_set_cthread_self(base): the base of the thread's TSD block, which
 * libpthread keeps in %gs (reads like %gs:0 are how userland finds its thread).  The same as FreeBSD's
 * sysarch(AMD64_SET_GSBASE).
 */
static int
darwin_set_tsd_base(struct thread *td, struct darwin_args *uap)
{
	uint64_t base = (uint64_t)uap->a[0];

	if (base >= td->td_proc->p_sysent->sv_maxuser)
		return (EINVAL);
	set_pcb_flags(td->td_pcb, PCB_FULL_IRET);
	td->td_pcb->pcb_gsbase = base;
	td->td_frame->tf_gs = _ugssel;
	return (0);
}

void
darwin_machdep_syscalls_init(void)
{
	const int n = DARWIN_MACHDEP_BASE + 3;

	darwin_sysent[n] = (struct sysent){ .sy_narg = 1, .sy_call = (sy_call_t *)darwin_set_tsd_base,
	    .sy_auevent = AUE_NULL, .sy_thrcnt = SY_THR_STATIC };
	darwin_syscallnames[n] = "thread_fast_set_cthread_self";
}

/*
 * The signal frame in Darwin's layout.  FreeBSD's sendsig builds its frame (sigframe: a ucontext, a siginfo, and above it the saved
 * floating point state); the handler of a Darwin program expects a Darwin ucontext (the registers of the interrupted code in
 * ucontext->uc_mcontext->ss) as its third argument.  After FreeBSD's sendsig the frame is moved down by 768 bytes (the trampoline of the
 * kernel finds the frame at the stack pointer and goes back with FreeBSD's sigreturn, so the frame itself stays as it was) and a
 * Darwin ucontext + mcontext64 (the registers, the exception state; no floating point state) is put in the room that this makes, above it.
 * A handler that changes the registers in the Darwin context changes nothing (sigreturn restores from FreeBSD's copy).
 */
#include <sys/ucontext.h>
#include <machine/sigframe.h>

struct darwin_ucontext64 {
	int32_t		uc_onstack;
	uint32_t	uc_sigmask;
	uint64_t	ss_sp, ss_size;
	int32_t		ss_flags, pad0;
	uint64_t	uc_link, uc_mcsize, uc_mcontext;
};
struct darwin_mcontext64 {
	uint16_t	trapno, cpu;
	uint32_t	err;
	uint64_t	faultvaddr;
	uint64_t	rax, rbx, rcx, rdx, rdi, rsi, rbp, rsp, r8, r9, r10, r11, r12, r13, r14, r15, rip, rflags, cs, fs, gs;
	uint8_t		fpstate[524];
	uint32_t	pad1;
};
#define	DARWIN_FRAME_ROOM	768
_Static_assert(sizeof(struct darwin_ucontext64) == 56, "ucontext of Darwin");
_Static_assert(sizeof(struct darwin_ucontext64) + sizeof(struct darwin_mcontext64) <= DARWIN_FRAME_ROOM, "room for the context");
_Static_assert(sizeof(struct darwin_mcontext64) == 712, "mcontext64 of Darwin");

static void (*native_sendsig)(sig_t, ksiginfo_t *, sigset_t *);

static void
darwin_sendsig(sig_t catcher, ksiginfo_t *ksi, sigset_t *mask)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct sigacts *psp = p->p_sigacts;
	struct trapframe *regs = td->td_frame;
	struct sigframe *sf;
	struct darwin_ucontext64 *du;
	struct darwin_mcontext64 *dm;
	char *buf, *base, *xf_end, *newbase;
	size_t len;
	bool siginfo = SIGISMEMBER(psp->ps_siginfo, ksi->ksi_signo);
	int i;

	native_sendsig(catcher, ksi, mask);
	if (!siginfo)
		return;
	/* both locks are held again: let go of them for the copies, in the order that FreeBSD's sendsig does */
	mtx_unlock(&psp->ps_mtx);
	PROC_UNLOCK(p);
	base = (char *)regs->tf_rsp;
	buf = malloc(sizeof(*sf) + 4096, M_DARWIN, M_WAITOK | M_ZERO);
	if (copyin(base, buf, sizeof(*sf)) != 0)
		goto out;
	sf = (struct sigframe *)buf;
	xf_end = base + sizeof(*sf);
	if (sf->sf_uc.uc_mcontext.mc_xfpustate != 0)
		xf_end = (char *)(uintptr_t)sf->sf_uc.uc_mcontext.mc_xfpustate + sf->sf_uc.uc_mcontext.mc_xfpustate_len;
	len = xf_end - base;
	if (len > sizeof(*sf) + 4096 || (char *)regs->tf_rdx != (char *)&((struct sigframe *)base)->sf_uc)
		goto out;
	if (len > sizeof(*sf) && copyin(base + sizeof(*sf), buf + sizeof(*sf), len - sizeof(*sf)) != 0)
		goto out;
	newbase = base - DARWIN_FRAME_ROOM;
	if (sf->sf_uc.uc_mcontext.mc_xfpustate != 0)
		sf->sf_uc.uc_mcontext.mc_xfpustate -= DARWIN_FRAME_ROOM;
	{	/* the Darwin context, built in the room above the moved frame */
		struct { struct darwin_ucontext64 u; struct darwin_mcontext64 m; } d;
		mcontext_t *mc = &sf->sf_uc.uc_mcontext;
		char *dbase = xf_end - DARWIN_FRAME_ROOM;

		memset(&d, 0, sizeof(d));
		du = &d.u; dm = &d.m;
		du->uc_onstack = mc->mc_onstack;
		for (i = 1; i <= 32; i++)
			if (SIGISMEMBER(sf->sf_uc.uc_sigmask, i))
				du->uc_sigmask |= 1U << (i - 1);
		du->ss_sp = (uint64_t)(uintptr_t)sf->sf_uc.uc_stack.ss_sp;
		du->ss_size = sf->sf_uc.uc_stack.ss_size;
		du->ss_flags = sf->sf_uc.uc_stack.ss_flags;
		du->uc_mcsize = sizeof(*dm);
		du->uc_mcontext = (uint64_t)(uintptr_t)(dbase + sizeof(*du));
		dm->trapno = (uint16_t)mc->mc_trapno; dm->err = (uint32_t)mc->mc_err; dm->faultvaddr = mc->mc_addr;
		dm->rax = mc->mc_rax; dm->rbx = mc->mc_rbx; dm->rcx = mc->mc_rcx; dm->rdx = mc->mc_rdx; dm->rdi = mc->mc_rdi; dm->rsi = mc->mc_rsi;
		dm->rbp = mc->mc_rbp; dm->rsp = mc->mc_rsp; dm->r8 = mc->mc_r8; dm->r9 = mc->mc_r9; dm->r10 = mc->mc_r10; dm->r11 = mc->mc_r11;
		dm->r12 = mc->mc_r12; dm->r13 = mc->mc_r13; dm->r14 = mc->mc_r14; dm->r15 = mc->mc_r15; dm->rip = mc->mc_rip; dm->rflags = mc->mc_rflags;
		dm->cs = mc->mc_cs; dm->fs = mc->mc_fs; dm->gs = mc->mc_gs;
		if (copyout(buf, newbase, len) != 0 || copyout(&d, dbase, sizeof(d)) != 0) {
			PROC_LOCK(p);
			sigexit(td, SIGILL);
		}
		regs->tf_rsp = (long)newbase;
		regs->tf_rsi = (register_t)&((struct sigframe *)newbase)->sf_si;
		regs->tf_rdx = (register_t)dbase;		/* arg 3: Darwin's ucontext */
	}
out:
	free(buf, M_DARWIN);
	PROC_LOCK(p);
	mtx_lock(&psp->ps_mtx);
}

void
darwin_sigframe_init(struct sysentvec *sv)
{
	native_sendsig = sv->sv_sendsig;
	sv->sv_sendsig = darwin_sendsig;
}
