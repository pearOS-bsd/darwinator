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
	dprintf(2, " rdi %#lx rsi %#lx rax %#lx rbx %#lx\n", (unsigned long)((ucontext_t *)ucv)->uc_mcontext.mc_rdi, (unsigned long)((ucontext_t *)ucv)->uc_mcontext.mc_rsi, (unsigned long)((ucontext_t *)ucv)->uc_mcontext.mc_rax, (unsigned long)((ucontext_t *)ucv)->uc_mcontext.mc_rbx);
	dprintf(2, " trapno %ld err %#lx si_code %d\n", (long)((ucontext_t *)ucv)->uc_mcontext.mc_trapno, (unsigned long)((ucontext_t *)ucv)->uc_mcontext.mc_err, si->si_code);
	{ uintptr_t *sp = (uintptr_t *)((ucontext_t *)ucv)->uc_mcontext.mc_rsp; int i;	/* the words on top of the stack: the return address of a call to a bad address is the first */
	  for (i = 0; i < 12; i++) dprintf(2, " sp[%d] %#lx\n", i, (unsigned long)sp[i]);
	  for (i = 0; i < 400; i++) if (sp[i] >= 0x100000000UL && sp[i] < 0x100200000UL) dprintf(2, " machostack[%d] %#lx\n", i, (unsigned long)sp[i]);
	  for (i = 0; i < 60; i++) { Dl_info di; if (dladdr((void *)sp[i], &di) && di.dli_sname && !strstr(di.dli_fname, "libexctrace")) dprintf(2, " stack[%d] %s (%s+%#lx)\n", i, di.dli_fname, di.dli_sname, (unsigned long)((char *)sp[i] - (char *)di.dli_saddr)); } }
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

/* MACHRUN_TRACEPOOL=1 (with LD_PRELOAD=libexctrace.so): the autorelease pools of every thread, as libobjc sees them (NSAutoreleasePool -init pushes, -dealloc pops):
 * a pop whose token is not the last push of the thread is reported with a stack trace */
#include <pthread.h>
#include <stdlib.h>
static __thread void *pl_stack[512]; static __thread int pl_depth;
void *
objc_autoreleasePoolPush(void)
{
	static void *(*real)(void);
	void *t;
	if (!real) real = (void *(*)(void))dlsym(RTLD_NEXT, "objc_autoreleasePoolPush");
	t = real();
	if (getenv("MACHRUN_TRACEPOOL")) { if (pl_depth < 512) pl_stack[pl_depth] = t; pl_depth++; }
	return (t);
}
void
objc_autoreleasePoolPop(void *t)
{
	static void (*real)(void *);
	if (!real) real = (void (*)(void *))dlsym(RTLD_NEXT, "objc_autoreleasePoolPop");
	if (getenv("MACHRUN_TRACEPOOL") && pl_depth > 0 && pl_depth <= 512) {
		if (pl_stack[pl_depth - 1] != t) {
			int k, found = -1; void *bt[24];
			for (k = pl_depth - 1; k >= 0; k--) if (pl_stack[k] == t) { found = k; break; }
			dprintf(2, "POOL: pop %p on thread %p, the last push here was %p (%s)\n", t, (void *)pthread_self(), pl_stack[pl_depth - 1], found >= 0 ? "an outer pool: the inner ones are lost" : "not a pool of this thread");
			k = backtrace(bt, 24); backtrace_symbols_fd(bt, k, 2);
			if (found >= 0) pl_depth = found + 1;
		}
		pl_depth--;
	} else if (pl_depth > 0) pl_depth--;
	real(t);
}
