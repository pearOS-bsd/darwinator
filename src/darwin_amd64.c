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
#include <machine/frame.h>

#include "darwinator.h"

#define	NARGREGS_DARWIN	6

int
darwin_fetch_syscall_args(struct thread *td)
{
	struct trapframe *frame = td->td_frame;
	struct syscall_args *sa = &td->td_sa;
	uint64_t rax = frame->tf_rax;
	int error = 0;

	sa->original_code = (u_int)rax;
	sa->code = (u_int)(rax & 0xffffff);
	if (((rax >> 24) & 0xff) != DARWIN_CLASS_UNIX || sa->code >= (u_int)darwin_nsyscalls)
		sa->callp = &nosys_sysent;	/* Mach traps, machdep and diag calls are not implemented yet */
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
