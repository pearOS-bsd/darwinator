/*
 * Darwin arm64 system call convention: "svc #0x80" (here "svc #0": the svc handler of FreeBSD 14 sends SIGILL for any other immediate) with the number in x16 (positive: BSD call, negative: Mach
 * trap), arguments in x0-x7, the result in x0 with the carry flag set on error.  FreeBSD's arm64 return
 * convention is the same (carry flag, errno in x0); it takes the number from x8 and from "svc #0", so only the
 * fetching of the number differs (the exception handler of the kernel does not look at the immediate).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysent.h>
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <sys/syscall.h>
#include <machine/frame.h>

#include "darwinator.h"

int
darwin_fetch_syscall_args(struct thread *td)
{
	struct trapframe *frame = td->td_frame;
	struct syscall_args *sa = &td->td_sa;
	register_t x16 = frame->tf_x[16];
	uint64_t sigpc;

	sa->original_code = (u_int)x16;
	sa->code = (u_int)x16;
	/* the signal trampoline of the kernel (in the shared page: shp_base + sv_sigcode_offset) returns with "svc #0" and the number in x8 */
	sigpc = (uint64_t)td->td_proc->p_vmspace->vm_shp_base + (uint64_t)td->td_proc->p_sysent->sv_sigcode_offset;
	if (frame->tf_elr - 4 >= sigpc && frame->tf_elr - 4 < sigpc + (uint64_t)*td->td_proc->p_sysent->sv_szsigcode) {
		sa->code = SYS_sigreturn;		/* the signal trampoline of the kernel (svc #0, number in x8) */
		sa->callp = &sysent[SYS_sigreturn];
	} else if (x16 < 0) {				/* negative: Mach traps */
		sa->code = (-x16 < 128) ? (u_int)(-x16) + DARWIN_MACH_BASE : (u_int)darwin_nsyscalls;
		sa->callp = sa->code < (u_int)darwin_nsyscalls ? &darwin_sysent[sa->code] : &nosys_sysent;
	} else if (x16 >= darwin_nsyscalls || x16 >= DARWIN_MACHDEP_BASE)
		sa->callp = &nosys_sysent;
	else
		sa->callp = &darwin_sysent[sa->code];

	memcpy(sa->args, frame->tf_x, sizeof(sa->args));
	td->td_retval[0] = 0;
	td->td_retval[1] = 0;
	return (0);
}

void
darwin_sigframe_init(struct sysentvec *sv __unused)
{
	/* the handler gets the ucontext of FreeBSD on arm64 */
}

void
darwin_machdep_syscalls_init(void)
{
	/* TPIDRRO_EL0 (the TSD base) is set through a Mach trap on arm64: not implemented yet */
}

void darwin_commpage_init(void) { }
int darwin_commpage_map(struct vmspace *vms __unused) { return (0); }
void darwin_stack_init(struct sysentvec *sv __unused) { }
