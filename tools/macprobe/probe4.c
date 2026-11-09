/* probe4: what is specific to x86-64 Darwin: the registers in the signal frame, the thread state flavors, and the raw system call convention (carry flag, rdx).
 * Meant for an Intel Mac (it also builds on Apple silicon, where it prints that it is not x86-64).  Build: cc -o probe4 probe4.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <pthread.h>

#if defined(__x86_64__)
#include <mach/thread_status.h>
static volatile int got;
static void on_usr1(int sig, siginfo_t *si, void *uc)
{
	ucontext_t *u = uc; _STRUCT_MCONTEXT64 *m = u->uc_mcontext; (void)sig; (void)si;
	got = 1;
	printf("uc_mcsize %zu  uc_mcontext-uc %ld  sizeof(ucontext_t) %zu  sizeof(mcontext64) %zu\n", (size_t)u->uc_mcsize, (long)((char *)m - (char *)u), sizeof(ucontext_t), sizeof(_STRUCT_MCONTEXT64));
	printf("es: trapno %#x err %#x faultvaddr %#llx\n", m->__es.__trapno, m->__es.__err, (unsigned long long)m->__es.__faultvaddr);
	printf("ss: rax %#llx rbx %#llx rcx %#llx rdx %#llx rdi %#llx rsi %#llx rbp %#llx rsp %#llx\n", m->__ss.__rax, m->__ss.__rbx, m->__ss.__rcx, m->__ss.__rdx, m->__ss.__rdi, m->__ss.__rsi, m->__ss.__rbp, m->__ss.__rsp);
	printf("ss: r8 %#llx r9 %#llx r10 %#llx r11 %#llx r12 %#llx r13 %#llx r14 %#llx r15 %#llx\n", m->__ss.__r8, m->__ss.__r9, m->__ss.__r10, m->__ss.__r11, m->__ss.__r12, m->__ss.__r13, m->__ss.__r14, m->__ss.__r15);
	printf("ss: rip %#llx rflags %#llx cs %#llx fs %#llx gs %#llx\n", m->__ss.__rip, m->__ss.__rflags, m->__ss.__cs, m->__ss.__fs, m->__ss.__gs);
	printf("fs: fcw %#x mxcsr %#x  sizeof(float_state64) %zu\n", (unsigned)m->__fs.__fpu_fcw, (unsigned)m->__fs.__fpu_mxcsr, sizeof(m->__fs));
}

static void signal_frame(void)
{
	struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_usr1; sa.sa_flags = SA_SIGINFO; sigaction(SIGUSR1, &sa, NULL);
	/* load known values into callee-saved registers, then send the signal to ourselves */
	long pid = getpid(), sg = SIGUSR1, kr = 0x2000025;
	__asm__ volatile("movq $0x1111111111111111, %%r12\n\tmovq $0x2222222222222222, %%r13\n\tmovq $0x3333333333333333, %%r14\n\tmovq $0x4444444444444444, %%r15\n\tmovq $0x5555555555555555, %%rbx\n\t"
	    "syscall\n" : "+a"(kr), "+D"(pid), "+S"(sg) : : "r12", "r13", "r14", "r15", "rbx", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
	printf("handler ran: %d\n", got);
}

static void raw(void)
{
	/* open("/nonexistent") by hand: class 2 (BSD) in the top byte of rax, error = carry flag set, rax = errno */
	long ret = 0x2000005; unsigned char cf;
	const char *p = "/nonexistent"; long fl = 0, md = 0;
	__asm__ volatile("syscall; setc %1" : "+a"(ret), "=q"(cf), "+D"(p), "+S"(fl), "+d"(md) : : "rcx", "r11", "memory", "cc");
	printf("raw open(/nonexistent): rax=%ld carry=%d\n", ret, cf);
	ret = 0x2000014; unsigned char cf2;			/* getpid */
	__asm__ volatile("syscall; setc %1" : "+a"(ret), "=q"(cf2) : : "rcx", "rdx", "r11", "memory", "cc");
	printf("raw getpid: rax=%ld (== getpid() %d) carry=%d\n", ret, getpid(), cf2);
	long r2 = 0x2000002a, rd = 0xdead;			/* pipe: fds in rax and rdx */
	__asm__ volatile("syscall; setc %1" : "+a"(r2), "=q"(cf2), "+d"(rd) : : "rcx", "r11", "memory", "cc");
	printf("raw pipe: rax=%ld rdx=%ld carry=%d\n", r2, rd, cf2);
	long f = 0x2000002, rdx = 0xdead;			/* fork: child has rdx = 1, parent 0 */
	__asm__ volatile("syscall" : "+a"(f), "+d"(rdx) : : "rcx", "r11", "memory", "cc");
	if (rdx == 1) _exit(0);
	printf("raw fork (parent): rax=pid>0 %d rdx=%ld\n", f > 0, rdx);
	wait(NULL);
}

static void *thr(void *a) { (void)a; return NULL; }
static void thread_state(void)
{
	mach_port_t t = mach_thread_self(); x86_thread_state64_t ts; mach_msg_type_number_t n = x86_THREAD_STATE64_COUNT;
	kern_return_t kr = thread_get_state(t, x86_THREAD_STATE64, (thread_state_t)&ts, &n);
	printf("thread_get_state x86_THREAD_STATE64: kr %d count %u (want %u) sizeof %zu\n", kr, n, (unsigned)x86_THREAD_STATE64_COUNT, sizeof ts);
	x86_float_state64_t fs; n = x86_FLOAT_STATE64_COUNT; kr = thread_get_state(t, x86_FLOAT_STATE64, (thread_state_t)&fs, &n);
	printf("x86_FLOAT_STATE64: kr %d count %u sizeof %zu\n", kr, n, sizeof fs);
	x86_exception_state64_t es; n = x86_EXCEPTION_STATE64_COUNT; kr = thread_get_state(t, x86_EXCEPTION_STATE64, (thread_state_t)&es, &n);
	printf("x86_EXCEPTION_STATE64: kr %d count %u sizeof %zu\n", kr, n, sizeof es);
	printf("flavors: THREAD_STATE64 %d FLOAT_STATE64 %d EXCEPTION_STATE64 %d x86_THREAD_STATE %d\n", x86_THREAD_STATE64, x86_FLOAT_STATE64, x86_EXCEPTION_STATE64, x86_THREAD_STATE);
	pthread_t th; pthread_create(&th, NULL, thr, NULL); pthread_join(th, NULL);
}
int main(void) { signal_frame(); raw(); thread_state(); return 0; }
#else
int main(void) { printf("not x86-64 (this is the Intel-only probe)\n"); return 0; }
#endif
