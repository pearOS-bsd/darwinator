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

#include "darwinator.h"

MALLOC_DEFINE(M_DARWIN, "darwinator", "Mach-O loader and Darwin system call layer");

int darwin_debug;
SYSCTL_NODE(_kern, OID_AUTO, darwinator, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "Darwin (Mach-O) support");
SYSCTL_INT(_kern_darwinator, OID_AUTO, debug, CTLFLAG_RWTUN, &darwin_debug, 0, "log why a Mach-O file is refused");

struct sysentvec darwin_sysvec;

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
	darwin_sysvec.sv_flags = (native->sv_flags & ~(SV_ABI_MASK | SV_SIGSYS)) | SV_ABI_UNDEF;
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
		error = exec_register(&macho_execsw);
		if (error == 0)
			printf("darwinator: Mach-O executables enabled (%s)\n",
			    darwin_sysvec.sv_name);
		break;
	case MOD_UNLOAD:
		if (darwin_in_use())
			return (EBUSY);
		error = exec_unregister(&macho_execsw);
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
FEATURE(darwinator, "Mach-O executables");
