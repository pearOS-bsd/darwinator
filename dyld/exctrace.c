/* exctrace: LD_PRELOAD library that prints a stack trace whenever an Objective-C exception is thrown (GNUstep catches and logs
 * many of them without saying where they came from).   LD_PRELOAD=./libexctrace.so ./Keka */
#include <dlfcn.h>
#include <execinfo.h>
#include <stdio.h>
#include <unistd.h>

typedef void *id; typedef void *SEL;
extern SEL sel_registerName(const char *);
static const char *
describe(id e, const char *what)
{
	void *(*lookup)(id, SEL) = (void *(*)(id, SEL))dlsym(RTLD_DEFAULT, "objc_msg_lookup");
	SEL (*reg)(const char *) = (SEL (*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
	id s; const char *(*utf8)(id, SEL);
	if (!lookup || !reg || e == NULL) return ("?");
	s = ((id (*)(id, SEL))lookup(e, reg(what)))(e, reg(what));
	if (s == NULL) return ("(nil)");
	utf8 = (const char *(*)(id, SEL))lookup(s, reg("UTF8String"));
	return (utf8 ? utf8(s, reg("UTF8String")) : "?");
}

static void
fpwalk(void)
{
	void **fp = (void **)__builtin_frame_address(0); int i;
	for (i = 0; i < 40 && fp != NULL; i++) {
		void **next;
		if (((unsigned long)fp & 7) != 0 || fp[1] == NULL) break;
		dprintf(2, "fp %p\n", fp[1]);
		next = (void **)fp[0];
		if (next <= fp || (unsigned long)next - (unsigned long)fp > (1UL << 22)) break;
		fp = next;
	}
}

void
objc_exception_throw(void *e)
{
	void *bt[48]; int n = backtrace(bt, 48);
	void (*real)(void *) = (void (*)(void *))dlsym(RTLD_NEXT, "objc_exception_throw");
	dprintf(2, "--- Objective-C exception thrown: %s: %s (%d frames)\n", describe(e, "name"), describe(e, "reason"), n);
	backtrace_symbols_fd(bt, n, 2);
	fpwalk();
	dprintf(2, "---\n");
	real(e);
	_exit(1);
}

/* fatal signals: where and why (the program has no debugger on the image) */
#include <signal.h>
#include <stdlib.h>
#include <ucontext.h>
#include <string.h>

static void
fatal(int sig, siginfo_t *si, void *ucv)
{
	void *bt[48]; int n;
#if defined(__amd64__)
	void *rip = (void *)((ucontext_t *)ucv)->uc_mcontext.mc_rip;
	dprintf(2, "--- signal %d (%s), fault address %p, rip %p\n", sig, strsignal(sig), si->si_addr, rip);
	{ Dl_info di; if (dladdr(rip, &di)) dprintf(2, " rip in %s (%s+%#lx)\n", di.dli_fname, di.dli_sname ? di.dli_sname : "?", (unsigned long)((char *)rip - (char *)di.dli_saddr)); }
	dprintf(2, " trapno %ld err %#lx si_code %d\n", (long)((ucontext_t *)ucv)->uc_mcontext.mc_trapno, (unsigned long)((ucontext_t *)ucv)->uc_mcontext.mc_err, si->si_code);
	{ uintptr_t *sp = (uintptr_t *)((ucontext_t *)ucv)->uc_mcontext.mc_rsp; int i;	/* the words on top of the stack: the return address of a call to a bad address is the first */
	  for (i = 0; i < 12; i++) dprintf(2, " sp[%d] %#lx\n", i, (unsigned long)sp[i]); }
#else
	dprintf(2, "--- signal %d (%s), fault address %p\n", sig, strsignal(sig), si->si_addr);
#endif
	n = backtrace(bt, 48);
	backtrace_symbols_fd(bt, n, 2);
	fpwalk();
	dprintf(2, "---\n");
	_exit(128 + sig);
}

__attribute__((constructor)) static void
exctrace_init(void)
{
	static char stack[1 << 16]; stack_t ss = { .ss_sp = stack, .ss_size = sizeof(stack), .ss_flags = 0 };
	struct sigaction sa; int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT }; size_t i;
	sigaltstack(&ss, NULL);
	memset(&sa, 0, sizeof(sa)); sa.sa_sigaction = fatal; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
	for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}
