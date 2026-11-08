/*
 * Darwin arm64 system call convention: "svc #0x80" with the number in x16 (positive: BSD call, negative: Mach
 * trap), arguments in x0-x7, the result in x0 with the carry flag set on error.  FreeBSD's arm64 return
 * convention is the same (carry flag, errno in x0); it takes the number from x8 and from "svc #0", so only the
 * fetching of the number differs (the exception handler of the kernel does not look at the immediate).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysent.h>
#include <machine/frame.h>

#include "darwinator.h"

int
darwin_fetch_syscall_args(struct thread *td)
{
	struct trapframe *frame = td->td_frame;
	struct syscall_args *sa = &td->td_sa;
	register_t x16 = frame->tf_x[16];

	sa->original_code = (u_int)x16;
	sa->code = (u_int)x16;
	if (x16 < 0 || x16 >= darwin_nsyscalls)		/* negative: Mach traps, not implemented yet */
		sa->callp = &nosys_sysent;
	else
		sa->callp = &darwin_sysent[sa->code];

	memcpy(sa->args, frame->tf_x, sizeof(sa->args));
	td->td_retval[0] = 0;
	td->td_retval[1] = 0;
	return (0);
}
