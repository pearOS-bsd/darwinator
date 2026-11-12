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
#include <machine/md_var.h>
#include <machine/cpufunc.h>
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <sys/smp.h>
#include <sys/exec.h>
#include <vm/vm_extern.h>

extern uint64_t tsc_freq;

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
	if (darwin_debug >= 2) {
		char path[160] = "";
		const char *nm = sa->code < (u_int)darwin_nsyscalls && darwin_syscallnames[sa->code] != NULL ? darwin_syscallnames[sa->code] : "?";
		size_t done;

		/* the path of the calls that take one (first argument, or the second of the *at calls) */
		if ((rax >> 24) == 2) {
			switch (rax & 0xffffff) {
			case 5: case 33: case 58: case 338: case 340: case 398: case 274:
				(void)copyinstr((void *)frame->tf_rdi, path, sizeof(path), &done); break;
			case 463: case 470:
				(void)copyinstr((void *)frame->tf_rsi, path, sizeof(path), &done); break;
			case 336:						/* proc_info: callnum, pid, flavor */
				snprintf(path, sizeof(path), "callnum %ld pid %ld flavor %ld", (long)frame->tf_rdi, (long)frame->tf_rsi, (long)frame->tf_rdx);
				break;
			case 92:						/* fcntl: the command */
				snprintf(path, sizeof(path), "fd %ld cmd %ld arg %#lx", (long)frame->tf_rdi, (long)frame->tf_rsi, (u_long)frame->tf_rdx);
				break;
			case 202: {						/* sysctl: the numbers of the name */
				int mib[6] = { 0 }, n = (int)frame->tf_rsi > 6 ? 6 : (int)frame->tf_rsi, q;
				size_t l = 0;

				if (copyin((void *)frame->tf_rdi, mib, n * sizeof(int)) == 0)
					for (q = 0; q < n; q++)
						l += snprintf(path + l, sizeof(path) - l, "%d ", mib[q]);
				if (n >= 2 && mib[0] == 0 && mib[1] == 3)		/* name to oid: the name is the new value */
					(void)copyinstr((void *)frame->tf_r8, path + l, sizeof(path) - l, &done);
				break;
			}
			}
		}
		printf("darwinator: %s[%d]: syscall %#lx -> %s %s\n", td->td_proc->p_comm, td->td_proc->p_pid, (u_long)rax, nm, path);
	}
	if (native_sigreturn)
		sa->callp = &sysent[SYS_sigreturn];
	else if (sa->code >= (u_int)darwin_nsyscalls)
		sa->callp = &nosys_sysent;
	else
		sa->callp = &darwin_sysent[sa->code];

	memcpy(sa->args, &frame->tf_rdi, sizeof(sa->args[0]) * NARGREGS_DARWIN);
	if (sa->code == 0 && ((rax >> 24) & 0xff) == DARWIN_CLASS_UNIX) {
		/* syscall(2): the number is the first argument and the others move up by one (libc's syscall(), used by libraries that call by number) */
		u_int c = (u_int)frame->tf_rdi;

		sa->code = c != 0 && c < (u_int)darwin_nsyscalls ? c : (u_int)darwin_nsyscalls;
		sa->callp = sa->code >= (u_int)darwin_nsyscalls ? &nosys_sysent : &darwin_sysent[sa->code];
		memmove(&sa->args[0], &sa->args[1], sizeof(sa->args[0]) * (NARGREGS_DARWIN - 1));
		if (sa->callp->sy_narg > NARGREGS_DARWIN - 1) {
			error = copyin((caddr_t)frame->tf_rsp + sizeof(register_t), &sa->args[NARGREGS_DARWIN - 1],
			    (sa->callp->sy_narg - (NARGREGS_DARWIN - 1)) * sizeof(sa->args[0]));
			if (error != 0)
				return (error);
		}
	} else if (sa->callp->sy_narg > NARGREGS_DARWIN) {
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

/*
 * The comm page: Darwin maps a read only page at 0x7fffffe00000 in every process, and libSystem reads the CPU capabilities, the number of CPUs and (for mach_absolute_time) the
 * time base from it.  The layout is that of xnu's cpu_capabilities.h; the bytes of the CPU part were read from a Mac (tools/macprobe), the time is the TSC of this machine.
 *   0x00 signature "commpage 64-bit"   0x10 capabilities (64 bit)   0x1e version (14)   0x20 capabilities (32 bit)   0x22 ncpus   0x34 active, 0x35 physical, 0x36 logical cpus
 *   0x50 nt_tsc_base (64)   0x58 nt_scale (32)   0x5c nt_shift (32)   0x60 nt_ns_base (64)   0x68 nt_generation (32, 0 means that the time is being changed)
 * mach_absolute_time is ((tsc - nt_tsc_base) << nt_shift) * nt_scale >> 32 + nt_ns_base (in nanoseconds: the timebase of an Intel Mac is 1/1).
 */
#define	DARWIN_COMMPAGE_ADDR	0x7fffffe00000UL
static uint8_t darwin_commpage[PAGE_SIZE];

void
darwin_commpage_init(void)
{
	/* the CPU part of the page, from a Mac, at its offsets (0x4d and 0x4e are the kernel page shift and the page shift: 12) */
	static const struct { int off; uint8_t v[8]; } head[] = {
		{ 0x10, { 0xaf, 0x1f, 0x04, 0xff, 0x0c, 0, 0, 0 } },		/* capabilities (64 bit) */
		{ 0x18, { 0, 0, 0, 0, 0, 0, 0x0e, 0 } },			/* the version (14) is at 0x1e */
		{ 0x20, { 0xaf, 0x1f, 0x04, 0xff, 0, 0, 0x40, 0 } },
		{ 0x30, { 0, 0, 0, 0, 0x04, 0x02, 0x04, 0x01 } },
		{ 0x38, { 0, 0, 0, 0, 0x02, 0, 0, 0 } },
		{ 0x40, { 0x9f, 0x21, 0xfc, 0x37, 0, 0, 0, 0 } },
		{ 0x48, { 0x01, 0, 0, 0, 0x01, 0x0c, 0x0c, 0 } },
	};
	int hi;
	uint64_t base = rdtsc();
	uint32_t scale = (uint32_t)(((uint64_t)1000000000 << 32) / (tsc_freq != 0 ? tsc_freq : 2000000000ULL));
	uint32_t gen = 1, shift = 0;
	uint64_t nsbase;
	uint8_t n = (uint8_t)mp_ncpus;
	struct timespec upts;

	nanouptime(&upts);			/* mach_absolute_time starts at the uptime of the kernel (it started at the load of the module): the deadlines that a program gives (timers, mach_wait_until) are compared with the uptime in the kernel */
	nsbase = (uint64_t)upts.tv_sec * 1000000000ULL + (uint64_t)upts.tv_nsec;

	memset(darwin_commpage, 0, sizeof(darwin_commpage));
	memcpy(darwin_commpage, "commpage 64-bit", 16);
	for (hi = 0; hi < (int)nitems(head); hi++)
		memcpy(darwin_commpage + head[hi].off, head[hi].v, 8);
	darwin_commpage[0x22] = n; darwin_commpage[0x34] = n; darwin_commpage[0x35] = n; darwin_commpage[0x36] = n;
	memcpy(darwin_commpage + 0x50, &base, 8);
	memcpy(darwin_commpage + 0x58, &scale, 4);
	memcpy(darwin_commpage + 0x5c, &shift, 4);
	memcpy(darwin_commpage + 0x60, &nsbase, 8);
	memcpy(darwin_commpage + 0x68, &gen, 4);
}

int
darwin_commpage_map(struct vmspace *vms)
{
	vm_map_t map = &vms->vm_map;
	int rv, error;

	rv = vm_map_fixed(map, NULL, 0, DARWIN_COMMPAGE_ADDR, PAGE_SIZE, VM_PROT_RW, VM_PROT_ALL, MAP_CHECK_EXCL);
	if (rv != KERN_SUCCESS)
		return (vm_mmap_to_errno(rv));
	error = copyout(darwin_commpage, (void *)DARWIN_COMMPAGE_ADDR, PAGE_SIZE);
	if (error != 0)
		return (error);
	rv = vm_map_protect(map, DARWIN_COMMPAGE_ADDR, DARWIN_COMMPAGE_ADDR + PAGE_SIZE, VM_PROT_READ, 0, VM_MAP_PROTECT_SET_PROT);
	return (rv == KERN_SUCCESS ? 0 : vm_mmap_to_errno(rv));
}

/* the stack of a Mac program is near 0x7ff7bfeff000 (the shared region of the dyld cache starts at 0x7ff800000000 and the comm page is at 0x7fffffe00000) */
void
darwin_stack_init(struct sysentvec *sv)
{
	sv->sv_usrstack = 0x7ff7c0000000UL;
	sv->sv_psstrings = sv->sv_usrstack - sizeof(struct ps_strings);
}
