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
