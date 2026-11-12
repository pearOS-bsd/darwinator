/*
 * darwinator: runs Mach-O executables on FreeBSD.  The module registers an image activator for Mach-O files
 * (macho.c) and a system call table with the Darwin numbers (syscalls.c); a process started from a Mach-O file
 * gets this table as its p_sysent, the way a Linux process gets the table of linux.ko.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/exec.h>
#include <sys/imgact.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/errno.h>

#include "darwinator.h"

MALLOC_DEFINE(M_DARWIN, "darwinator", "Mach-O loader and Darwin system call layer");

int darwin_debug;
SYSCTL_NODE(_kern, OID_AUTO, darwinator, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "Darwin (Mach-O) support");
SYSCTL_INT(_kern_darwinator, OID_AUTO, debug, CTLFLAG_RWTUN, &darwin_debug, 0, "1: log why a Mach-O file is refused and the system calls that are not implemented; 2: log every system call");
int darwin_stop;
SYSCTL_INT(_kern_darwinator, OID_AUTO, stop, CTLFLAG_RWTUN, &darwin_stop, 0, "debugging: a Mach-O program stops (SIGSTOP) before its first instruction, so that a debugger can attach");
int darwin_dyld;
SYSCTL_INT(_kern_darwinator, OID_AUTO, dyld, CTLFLAG_RWTUN, &darwin_dyld, 0, "run a dynamic Mach-O program with the dyld that its load commands name (/usr/lib/dyld of a Mac)");

struct sysentvec darwin_sysvec;
static void (*native_set_syscall_retval)(struct thread *, int);
static int (*native_fixup)(uintptr_t *, struct image_params *);
static void (*native_setregs)(struct thread *, struct image_params *, uintptr_t);

/* the stack of a program that starts in dyld: [mach_header of the program] [argc] [argv ... 0] [envp ... 0] [apple ... 0] */
static int
darwin_fixup(uintptr_t *stack_base, struct image_params *imgp)
{
	int error = native_fixup(stack_base, imgp);		/* pushes argc */

	if (error == 0 && imgp->auxargs != NULL) {
		struct darwin_auxargs *aa = imgp->auxargs;
		uint64_t *base = (uint64_t *)*stack_base;

		base--;
		if (suword(base, (long)aa->mh) != 0)
			return (EFAULT);
		*stack_base = (uintptr_t)base;
	}
	return (error);
}

/* the "apple" vector, after the environment: pointers into the string area of the stack */
static int
darwin_copyout_auxargs(struct image_params *imgp, uintptr_t base)
{
	struct darwin_auxargs *aa = imgp->auxargs;
	long first;
	int i;

	if (aa == NULL)
		return (0);
	first = fuword(imgp->argv);				/* the user address of argv[0]: the start of the string area */
	if (first == -1)
		return (EFAULT);
	for (i = 0; i < aa->n; i++)
		if (suword((void *)(base + i * sizeof(long)), first + aa->off[i]) != 0)
			return (EFAULT);
	if (suword((void *)(base + aa->n * sizeof(long)), 0) != 0)
		return (EFAULT);
	return (0);
}

static void
darwin_setregs(struct thread *td, struct image_params *imgp, uintptr_t stack)
{
	native_setregs(td, imgp, stack);
#if defined(__amd64__)
	if (imgp->auxargs != NULL)
		td->td_frame->tf_rsp = stack;			/* dyld expects the stack pointer on the mach_header, as it is */
#endif
}

/* the results of a call are FreeBSD's; the errno numbers are translated to Darwin's before they reach the registers */
static void
darwin_set_syscall_retval(struct thread *td, int error)
{
	if (darwin_debug >= 3 && error == ENOENT && td->td_proc->p_sysent == &darwin_sysvec) {	/* the files that a program looks for and that are not there */
		char path[200] = "";
		size_t done;
		unsigned c = td->td_sa.original_code & 0xffffff;
		void *pp = (void *)td->td_sa.args[(c == 463 || c == 470) ? 1 : 0];

		if ((td->td_sa.original_code >> 24) == 2 && (c == 5 || c == 33 || c == 338 || c == 340 || c == 398 || c == 274 || c == 463 || c == 470 || c == 58 || c == 190 || c == 191 || c == 462 || c == 461))
			(void)copyinstr(pp, path, sizeof(path), &done);
		if (path[0] != 0)
			printf("darwinator: %s[%d]: ENOENT %u %s\n", td->td_proc->p_comm, td->td_proc->p_pid, c, path);
	}
	native_set_syscall_retval(td, error > 0 ? darwin_errno(error) : error);
}

static const struct execsw macho_execsw = {
	.ex_imgact = macho_imgact,
	.ex_name = "Mach-O",
};

/*
 * The system call vector of a Darwin process is the one of a native process (the same registers, the same
 * stack, the same signal trampoline page) with the table, the argument fetching and the names replaced.
 * It is copied from init at load time because the native vector is not exported on every architecture.
 */
static void
darwin_sysvec_init(void)
{
	struct sysentvec *native = initproc->p_sysent;

	darwin_sysvec = *native;
	darwin_syscalls_init();
	darwin_sysvec.sv_name = "Darwin Mach-O";
	darwin_sysvec.sv_size = darwin_nsyscalls;
	darwin_sysvec.sv_table = darwin_sysent;
	darwin_sysvec.sv_syscallnames = darwin_syscallnames;
	darwin_sysvec.sv_fetch_syscall_args = darwin_fetch_syscall_args;
	native_set_syscall_retval = native->sv_set_syscall_retval;
	darwin_sysvec.sv_set_syscall_retval = darwin_set_syscall_retval;
	darwin_sysvec.sv_flags = (native->sv_flags & ~(SV_ABI_MASK | SV_SIGSYS)) | SV_ABI_UNDEF;
	native_fixup = native->sv_fixup;
	darwin_sysvec.sv_fixup = darwin_fixup;
	darwin_sysvec.sv_copyout_auxargs = darwin_copyout_auxargs;
	native_setregs = native->sv_setregs;
	darwin_sysvec.sv_setregs = darwin_setregs;
	darwin_sigframe_init(&darwin_sysvec);
	darwin_stack_init(&darwin_sysvec);
	darwin_commpage_init();
	darwin_sysvec.sv_coredump = NULL;	/* a core file written with the ELF layout would be wrong */
}

static bool
darwin_in_use(void)
{
	struct proc *p;
	bool busy = false;

	sx_slock(&allproc_lock);
	FOREACH_PROC_IN_SYSTEM(p) {
		if (p->p_sysent == &darwin_sysvec) {
			busy = true;
			break;
		}
	}
	sx_sunlock(&allproc_lock);
	return (busy);
}

static int
darwinator_modevent(module_t mod, int type, void *data)
{
	int error = 0;

	switch (type) {
	case MOD_LOAD:
		darwin_sysvec_init();
		darwin_mach_init();
		error = exec_register(&macho_execsw);
		if (error == 0)
			printf("darwinator: Mach-O executables enabled (%s)\n",
			    darwin_sysvec.sv_name);
		break;
	case MOD_UNLOAD:
		if (darwin_in_use())
			return (EBUSY);
		error = exec_unregister(&macho_execsw);
		if (error == 0)
			darwin_mach_fini();
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}
	return (error);
}

static moduledata_t darwinator_mod = {
	"darwinator",
	darwinator_modevent,
	NULL
};
DECLARE_MODULE(darwinator, darwinator_mod, SI_SUB_EXEC, SI_ORDER_ANY);
MODULE_VERSION(darwinator, 1);
MODULE_DEPEND(darwinator, sem, 1, 1, 1);
FEATURE(darwinator, "Mach-O executables");
