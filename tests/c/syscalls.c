/*
 * syscalls: checks the BSD calls of the Darwin class that are not in machsh (fork, wait4, execve, pipe, fcntl, ioctl,
 * getdirentries64, statfs64, sigprocmask, getrlimit, sysctl, sysctlbyname, sockets, links, locks, select, poll ...).
 * A freestanding Mach-O program with the Darwin convention (no libSystem).  Prints "ok NAME" or "FAIL NAME ..." for each
 * check; the exit status is the number of failures.
 */
#include "dsys.h"

__attribute__((used)) void *memcpy(void *d, const void *s, u64 n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
__attribute__((used)) void *memset(void *d, int c, u64 n) { char *a = d; while (n--) *a++ = (char)c; return d; }

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { dsys(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0, 0); }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void num(i64 v)
{
	char b[24]; int i = 23; int neg = v < 0; u64 u = neg ? -(u64)v : (u64)v;
	b[i] = 0;
	do { b[--i] = '0' + u % 10; u /= 10; } while (u);
	if (neg) b[--i] = '-';
	out(b + i);
}

/* a call that also gives the second result register (rdx on x86-64, x1 on arm64): pipe */
static i64 dsys_2(i64 n, i64 *second)
{
#if defined(__x86_64__)
	i64 ret = 0x2000000 | n, rdx = 0; unsigned char err;
	__asm__ volatile("syscall; setc %1" : "+a"(ret), "=q"(err), "+d"(rdx) : : "rcx", "r11", "rdi", "rsi", "r8", "r9", "r10", "memory", "cc");
	*second = rdx; return err ? -ret : ret;
#else
	register i64 x0 __asm__("x0") = 0, x1 __asm__("x1") = 0, x16 __asm__("x16") = n; unsigned long err;
	__asm__ volatile("svc #0\n\tcset %0, cs" : "=r"(err), "+r"(x0), "+r"(x1) : "r"(x16) : "memory", "cc");
	*second = x1; return err ? -x0 : x0;
#endif
}


/* a call with seven arguments (kevent64): the seventh is on the stack on x86-64 (above a return address), in x6 on arm64 */
static i64 dsys7(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f, i64 g)
{
#if defined(__x86_64__)
	register i64 r10 __asm__("r10") = d;
	register i64 r8 __asm__("r8") = e;
	register i64 r9 __asm__("r9") = f;
	i64 ret = 0x2000000 | n;
	unsigned char err;
	__asm__ volatile("push %8; push $0; syscall; setc %1; add $16, %%rsp" : "+a"(ret), "=q"(err), "+d"(c), "+D"(a), "+S"(b), "+r"(r10), "+r"(r8), "+r"(r9)
	    : "r"(g) : "rcx", "r11", "memory", "cc");
	return err ? -ret : ret;
#else
	register i64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f, x6 __asm__("x6") = g, x16 __asm__("x16") = n;
	unsigned long err;
	__asm__ volatile("svc #0\n\tcset %0, cs" : "=r"(err), "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6) : "r"(x16) : "memory", "cc");
	return err ? -x0 : x0;
#endif
}


/* a thread of libpthread's kind: bsdthread_create starts thread_entry with (pthread, kport, func, arg, stack, flags) in the argument registers */
static volatile u64 thread_val, thread_port;
static void tfunc(u64 arg) { thread_val = arg; }
void thread_entry(void);
#if defined(__x86_64__)
__asm__(".globl thread_entry\nthread_entry:\n\tmov %rsi, thread_port(%rip)\n\tmov %rdx, %rax\n\tmov %rcx, %rdi\n\tcall *%rax\n"
	"\txor %edi, %edi\n\txor %esi, %esi\n\txor %edx, %edx\n\txor %r10d, %r10d\n\tmov $0x2000169, %eax\n\tsyscall\n\tud2\n");
#else
__asm__(".globl thread_entry\nthread_entry:\n\tadrp x9, thread_port\n\tstr x1, [x9, :lo12:thread_port]\n\tmov x0, x3\n\tblr x2\n"
	"\tmov x0, #0\n\tmov x1, #0\n\tmov x2, #0\n\tmov x3, #0\n\tmov x16, #361\n\tsvc #0\n\tbrk #1\n");
#endif


/* a Mach trap: the result is a kern_return_t (no carry flag) */
static i64 dmach(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
#if defined(__x86_64__)
	register i64 r10 __asm__("r10") = d; register i64 r8 __asm__("r8") = e; register i64 r9 __asm__("r9") = f;
	i64 ret = 0x1000000 | n;
	__asm__ volatile("syscall" : "+a"(ret), "+d"(c), "+D"(a), "+S"(b), "+r"(r10), "+r"(r8), "+r"(r9) : : "rcx", "r11", "memory", "cc");
	return ret;
#else
	register i64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f, x16 __asm__("x16") = -n;
	__asm__ volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5) : "r"(x16) : "memory", "cc");
	return x0;
#endif
}

/* a call with eight arguments (psynch_cvwait): two on the stack on x86-64, x6 and x7 on arm64 */
static i64 dsys8(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f, i64 g, i64 h)
{
#if defined(__x86_64__)
	register i64 r10 __asm__("r10") = d; register i64 r8 __asm__("r8") = e; register i64 r9 __asm__("r9") = f;
	i64 ret = 0x2000000 | n; unsigned char err;
	__asm__ volatile("push %9; push %8; push $0; syscall; setc %1; add $24, %%rsp" : "+a"(ret), "=q"(err), "+d"(c), "+D"(a), "+S"(b), "+r"(r10), "+r"(r8), "+r"(r9) : "r"(g), "r"(h) : "rcx", "r11", "memory", "cc");
	return err ? -ret : ret;
#else
	register i64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f, x6 __asm__("x6") = g, x7 __asm__("x7") = h, x16 __asm__("x16") = n;
	unsigned long err;
	__asm__ volatile("svc #0\n\tcset %0, cs" : "=r"(err), "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7) : "r"(x16) : "memory", "cc");
	return err ? -x0 : x0;
#endif
}
static volatile u64 psy_mx; static volatile int psy_state; static volatile unsigned dummy2 = 1;
static void twait(u64 arg) { (void)arg; psy_state = 1; dsys(301, (i64)psy_mx, 0, 0, 0, 0, 0); psy_state = 2; }


/* a Mach trap with seven arguments (mach_msg_trap): the seventh on the stack on x86-64, x6 on arm64 */
static i64 dmach7(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f, i64 g)
{
#if defined(__x86_64__)
	register i64 r10 __asm__("r10") = d; register i64 r8 __asm__("r8") = e; register i64 r9 __asm__("r9") = f;
	i64 ret = 0x1000000 | n;
	__asm__ volatile("push %7; push $0; syscall; add $16, %%rsp" : "+a"(ret), "+d"(c), "+D"(a), "+S"(b), "+r"(r10), "+r"(r8), "+r"(r9) : "r"(g) : "rcx", "r11", "memory", "cc");
	return ret;
#else
	register i64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f, x6 __asm__("x6") = g, x16 __asm__("x16") = -n;
	__asm__ volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6) : "r"(x16) : "memory", "cc");
	return x0;
#endif
}

static int fails;
static void check(const char *name, int cond, i64 detail)
{
	if (cond) { out("ok   "); out(name); out("\n"); }
	else { out("FAIL "); out(name); out(" ("); num(detail); out(")\n"); fails++; }
}

#define	S(n, a, b, c, d, e, f)	dsys(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), (i64)(e), (i64)(f))

enum {
	SYS_fork = 2, SYS_wait4 = 7, SYS_link = 9, SYS_chmod = 15, SYS_sync = 36, SYS_pipe = 42, SYS_sigprocmask = 48, SYS_ioctl = 54,
	SYS_symlink = 57, SYS_readlink = 58, SYS_execve = 59, SYS_getgroups = 79, SYS_getpgrp = 81, SYS_getdtablesize = 89,
	SYS_fcntl = 92, SYS_select = 93, SYS_socket = 97, SYS_connect = 98, SYS_bind = 104, SYS_setsockopt = 105,
	SYS_listen = 106, SYS_getsockopt = 118, SYS_getrusage = 117, SYS_fchmod = 124, SYS_rename = 128, SYS_flock = 131, SYS_mkfifo = 132,
	SYS_socketpair = 135, SYS_mkdir = 136, SYS_rmdir = 137, SYS_getrlimit = 194, SYS_truncate = 200, SYS_sysctl = 202,
	SYS_poll = 230, SYS_sysctlbyname = 274, SYS_getsid = 310, SYS_getdirentries64 = 344, SYS_statfs64 = 345, SYS_read_nocancel = 396,
	SYS_write_nocancel = 397, SYS_mknod = 14, SYS_kevent_qos = 374, SYS_kevent_id = 375, SYS_gettid_ = 286, SYS_proc_info = 336, SYS_getpid_ = 20, SYS_waitid = 173, SYS_pselect = 394, SYS_getlogin_ = 49, SYS_gethostuuid = 142, SYS_stat32 = 188, SYS_fstat32 = 189, SYS_lstat32 = 190, SYS_getdirentries32 = 196, SYS_pathconf = 191, SYS_utimes = 138, SYS_fdatasync = 187,
	SYS_stat64_extended = 341, SYS_open_extended = 277, SYS_mkdir_extended = 292, SYS_shm_open = 266, SYS_shm_unlink = 267, SYS_shmget = 265, SYS_shmat = 262, SYS_shmdt = 264, SYS_shmctl = 263, SYS_semget = 255, SYS_semop = 256, SYS_semctl = 254,
	SYS_guarded_open = 441, SYS_guarded_close = 442, SYS_clonefileat = 462, SYS_exchangedata = 223, SYS_getattrlistbulk = 461, SYS_ptrace = 26, SYS_kdebug_trace = 180, SYS_sigwait = 330, SYS_sendfile_ = 337, SYS_fgetattrlist = 228, SYS_psynch_mutexwait = 301, SYS_psynch_mutexdrop = 302, SYS_psynch_cvbroad = 303, SYS_psynch_cvsignal = 304, SYS_psynch_cvwait = 305, SYS_workq_open = 367, SYS_workq_kernreturn = 368, SYS_mmap_ = 197, SYS_bsdthread_create = 360, SYS_bsdthread_terminate = 361, SYS_bsdthread_register = 366, SYS_csops = 169, SYS_getxattr = 234, SYS_setxattr = 236, SYS_removexattr = 238, SYS_listxattr = 240, SYS_shared_region_check_np = 294,
	SYS_pthread_kill = 328, SYS_thread_selfid = 372, SYS_openat = 463, SYS_renameat = 465, SYS_faccessat = 466, SYS_fchmodat = 467, SYS_fstatat64 = 470, SYS_linkat = 471,
	SYS_unlinkat = 472, SYS_readlinkat = 473, SYS_symlinkat = 474, SYS_mkdirat = 475, SYS_ulock_wait = 515, SYS_ulock_wake = 516, SYS_getattrlist = 220, SYS_getsockname = 32, SYS_kill = 37, SYS_sigaction = 46, SYS_sigpending = 52, SYS_pthread_sigmask = 329,
	SYS_setitimer = 83, SYS_getitimer = 86, SYS_getfsstat64 = 347, SYS_kqueue = 362, SYS_kevent = 363, SYS_kevent64 = 369, SYS_open_nocancel = 398, SYS_close_nocancel = 399, SYS_fstatfs64 = 346, SYS_getentropy_ = 500
};

struct dkev { u64 ident; short filter; unsigned short flags; unsigned fflags; i64 data; u64 udata; u64 ext[2]; };	/* 32 bytes for kevent, 48 for kevent64 */
struct dsa { u64 handler, tramp; unsigned mask; int flags; };
struct dtv { i64 sec; int usec; int pad; };
struct ditimer { struct dtv interval, value; };
static struct dkev mk(u64 id, short f, unsigned short fl, unsigned ff, i64 d, u64 u) { struct dkev k = { id, f, fl, ff, d, u, { 0, 0 } }; return k; }
static struct dtv tv0 = { 0, 0, 0 }, tv1 = { 1, 0, 0 }, tv2 = { 2, 0, 0 };
static volatile int got_sig, got_signo_from_info;
static void on_usr1(int sig) { got_sig = sig; }
struct duc { int onstack; unsigned mask; u64 sp, size; int flags, pad; u64 link, mcsize, mcontext; };
struct dmc { unsigned short trapno, cpu; unsigned err; u64 fault; u64 r[21]; };
static volatile u64 uc_rip, uc_rsp, uc_mcsize, uc_mcptr_ok, uc_rbp; static volatile unsigned uc_mask;
static void on_usr1_info(int sig, void *info, void *uc)
{
	got_sig = sig; got_signo_from_info = *(int *)info;
#if defined(__x86_64__)		/* the Darwin ucontext is built on amd64 only */
	{ struct duc *u = uc; struct dmc *m = (struct dmc *)u->mcontext;
	  uc_mcsize = u->mcsize; uc_mcptr_ok = (u64)m == (u64)uc + 56; uc_rip = m->r[16]; uc_rsp = m->r[7]; uc_rbp = m->r[6]; uc_mask = u->mask; }
#else
	(void)uc;
#endif
}
struct dflock { i64 start, len; int pid; short type, whence; };
struct dpollfd { int fd; short events, revents; };
struct drlimit { u64 cur, max; };
struct dstatfs { unsigned bsize; int iosize; u64 blocks, bfree, bavail, files, ffree; int fsid[2]; unsigned owner, type, flags, sub; char fstype[16], on[1024], from[1024]; unsigned res[8]; };

static void cleanup(void)
{
	S(SYS_unlink, "/tmp/dsc/a", 0, 0, 0, 0, 0); S(SYS_unlink, "/tmp/dsc/bb", 0, 0, 0, 0, 0);
	S(SYS_unlink, "/tmp/dsc/ln", 0, 0, 0, 0, 0); S(SYS_unlink, "/tmp/dsc/sl", 0, 0, 0, 0, 0);
	S(SYS_unlink, "/tmp/dsc/ren", 0, 0, 0, 0, 0); S(SYS_unlink, "/tmp/dsc/fifo", 0, 0, 0, 0, 0); S(SYS_unlink, "/tmp/dsc/fifo2", 0, 0, 0, 0, 0);
	S(SYS_rmdir, "/tmp/dsc", 0, 0, 0, 0, 0);
}

int cmain(void)
{
	char buf[4096]; i64 r, r2; int fd, i;

	cleanup();
	check("mkdir", S(SYS_mkdir, "/tmp/dsc", 0755, 0, 0, 0, 0) == 0, 0);

	/* fork, execve, wait4 */
	r = S(SYS_fork, 0, 0, 0, 0, 0, 0);
	if (r == 0) { S(SYS_exit, 5, 0, 0, 0, 0, 0); }
	{ int st = 0; i64 w = S(SYS_wait4, r, &st, 0, 0, 0, 0);
	  check("fork + wait4 (status of exit(5))", r > 0 && w == r && st == (5 << 8), st); }
	r = S(SYS_fork, 0, 0, 0, 0, 0, 0);
	if (r == 0) { char *argv[] = { "true", 0 }; char *envp[] = { 0 }; S(SYS_execve, "/usr/bin/true", argv, envp, 0, 0, 0); S(SYS_exit, 99, 0, 0, 0, 0, 0); }
	{ int st = -1; i64 w = S(SYS_wait4, r, &st, 0, 0, 0, 0); check("fork + execve + wait4", w == r && st == 0, st); }

	/* pipe (two results), read/write nocancel, select, poll, FIONREAD */
	{ i64 p1; i64 p0 = dsys_2(SYS_pipe, &p1); char c[4];
	  check("pipe", p0 >= 0 && p1 > 0 && p0 != p1, p0);
	  check("write_nocancel", S(SYS_write_nocancel, p1, "abc", 3, 0, 0, 0) == 3, 0);
	  { int arg = 0; r = S(SYS_ioctl, p0, 0x4004667fUL, &arg, 0, 0, 0); check("ioctl FIONREAD", r == 0 && arg == 3, arg); }
	  { struct dpollfd pf = { (int)p0, 1, 0 }; r = S(SYS_poll, &pf, 1, 0, 0, 0, 0); check("poll (readable pipe)", r == 1 && (pf.revents & 1), r); }
	  { unsigned long set[16]; for (i = 0; i < 16; i++) set[i] = 0; set[0] = 1UL << p0; struct dtimeval tv = { 0, 0, 0 };
	    r = S(SYS_select, p0 + 1, set, 0, 0, &tv, 0); check("select (readable pipe)", r == 1, r); }
	  check("read_nocancel", S(SYS_read_nocancel, p0, c, 3, 0, 0, 0) == 3 && c[0] == 'a' && c[2] == 'c', 0);
	  { unsigned char t[72]; r = S(SYS_ioctl, p0, 0x40487413UL, t, 0, 0, 0); check("ioctl TIOCGETA on a pipe is ENOTTY", r == -25, r); }
	  S(SYS_close_nocancel, p0, 0, 0, 0, 0, 0); S(SYS_close_nocancel, p1, 0, 0, 0, 0, 0); }

	/* files: open_nocancel, fcntl, F_GETPATH, locks */
	fd = (int)S(SYS_open_nocancel, "/tmp/dsc/a", O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
	check("open_nocancel", fd >= 0, fd);
	check("write", S(SYS_write, fd, "hello", 5, 0, 0, 0) == 5, 0);
	r = S(SYS_fcntl, fd, 3, 0, 0, 0, 0); check("fcntl F_GETFL (O_WRONLY)", r == 1, r);
	check("fcntl F_SETFL O_NONBLOCK", S(SYS_fcntl, fd, 4, 4, 0, 0, 0) == 0 && (S(SYS_fcntl, fd, 3, 0, 0, 0, 0) & 4), 0);
	r = S(SYS_fcntl, fd, 67, 10, 0, 0, 0); check("fcntl F_DUPFD_CLOEXEC", r >= 10, r);
	if (r >= 0) { check("  its FD_CLOEXEC", S(SYS_fcntl, r, 1, 0, 0, 0, 0) == 1, 0); S(SYS_close, r, 0, 0, 0, 0, 0); }
	for (i = 0; i < (int)sizeof(buf); i++) buf[i] = 0;
	{ int fd2 = (int)S(SYS_open, "/tmp/dsc/a", 0, 0, 0, 0, 0);	/* opened by name: the name cache of the kernel knows the path (a file just created may not be there yet) */
	  r = S(SYS_fcntl, fd2, 50, buf, 0, 0, 0); check("fcntl F_GETPATH", r == 0 && seq(buf, "/tmp/dsc/a"), r); S(SYS_close, fd2, 0, 0, 0, 0, 0); }
	{ struct dflock fl = { 0, 0, 0, 3, 0 }; r = S(SYS_fcntl, fd, 8, &fl, 0, 0, 0); check("fcntl F_SETLK (write lock)", r == 0, r);
	  fl.type = 2; r = S(SYS_fcntl, fd, 8, &fl, 0, 0, 0); check("fcntl F_SETLK (unlock)", r == 0, r); }
	check("fcntl F_FULLFSYNC", S(SYS_fcntl, fd, 51, 0, 0, 0, 0) == 0, 0);
	check("fcntl unknown command is EINVAL", S(SYS_fcntl, fd, 9999, 0, 0, 0, 0) == -22, 0);
	check("flock (LOCK_EX)", S(SYS_flock, fd, 2, 0, 0, 0, 0) == 0 && S(SYS_flock, fd, 8, 0, 0, 0, 0) == 0, 0);
	check("fchmod", S(SYS_fchmod, fd, 0600, 0, 0, 0, 0) == 0, 0);
	S(SYS_close, fd, 0, 0, 0, 0, 0);

	/* names: link, symlink, readlink, rename, chmod, truncate, mkfifo */
	check("link", S(SYS_link, "/tmp/dsc/a", "/tmp/dsc/ln", 0, 0, 0, 0) == 0, 0);
	check("symlink", S(SYS_symlink, "/tmp/dsc/a", "/tmp/dsc/sl", 0, 0, 0, 0) == 0, 0);
	for (i = 0; i < 64; i++) buf[i] = 0;
	r = S(SYS_readlink, "/tmp/dsc/sl", buf, 63, 0, 0, 0); check("readlink", r == 10 && seq(buf, "/tmp/dsc/a"), r);
	check("rename", S(SYS_rename, "/tmp/dsc/ln", "/tmp/dsc/ren", 0, 0, 0, 0) == 0, 0);
	check("chmod", S(SYS_chmod, "/tmp/dsc/ren", 0640, 0, 0, 0, 0) == 0, 0);
	check("truncate", S(SYS_truncate, "/tmp/dsc/ren", 2, 0, 0, 0, 0) == 0, 0);
	{ struct dstat64 st; r = S(SYS_stat64, "/tmp/dsc/ren", &st, 0, 0, 0, 0); check("  size after truncate, mode after chmod", r == 0 && st.st_size == 2 && (st.st_mode & 0777) == 0640, st.st_size); }
	check("mkfifo", S(SYS_mkfifo, "/tmp/dsc/fifo", 0600, 0, 0, 0, 0) == 0, 0);
	S(SYS_unlink, "/tmp/dsc/ren", 0, 0, 0, 0, 0);
	fd = (int)S(SYS_open, "/tmp/dsc/bb", O_WRONLY | O_CREAT, 0644, 0, 0, 0); S(SYS_close, fd, 0, 0, 0, 0, 0);

	/* getdirentries64 */
	fd = (int)S(SYS_open, "/tmp/dsc", 0, 0, 0, 0, 0);
	{ i64 pos = 0; int seen_a = 0, seen_bb = 0, seen_dot = 0, bad = 0; i64 n = S(SYS_getdirentries64, fd, buf, sizeof(buf), &pos, 0, 0), o = 0;
	  while (o < n) {
		  unsigned short reclen = *(unsigned short *)(buf + o + 16), namlen = *(unsigned short *)(buf + o + 18); const char *name = buf + o + 21;
		  if (reclen % 4 || reclen < 22 || name[namlen] != 0) bad = 1;
		  if (seq(name, "a")) seen_a = 1; else if (seq(name, "bb") && namlen == 2) seen_bb = 1; else if (seq(name, ".")) seen_dot = 1;
		  o += reclen; }
	  check("getdirentries64 (names, reclen, namlen)", n > 0 && seen_a && seen_bb && seen_dot && !bad, n); }
	S(SYS_close, fd, 0, 0, 0, 0, 0);

	/* statfs64, fstatfs64 */
	{ static struct dstatfs sf; r = S(SYS_statfs64, "/", &sf, 0, 0, 0, 0);
	  check("statfs64 /", r == 0 && sf.bsize > 0 && sf.blocks > 0 && sf.fstype[0] && sf.on[0] == '/', r);
	  fd = (int)S(SYS_open, "/", 0, 0, 0, 0, 0); r = S(SYS_fstatfs64, fd, &sf, 0, 0, 0, 0);
	  check("fstatfs64", r == 0 && sf.blocks > 0, r);
	  { static char ob[512]; unsigned long bs = 0, bl = 0; r = S(157, "/", ob, 0, 0, 0, 0); memcpy(&bs, ob + 8, 8); memcpy(&bl, ob + 24, 8);
	  check("statfs (157, old layout)", r == 0 && bs > 0 && bl > 0 && ob[119] == '/' && ob[104] != 0, r);
	  r = S(158, fd, ob, 0, 0, 0, 0); memcpy(&bl, ob + 24, 8); check("fstatfs (158)", r == 0 && bl > 0, r); }
	  S(SYS_close, fd, 0, 0, 0, 0, 0); }

	/* named semaphores, access_extended */
	{ r = S(268, "/darwinator-sem", 0x200, 0600, 2, 0, 0);
	  check("sem_open creates", r >= 0, r);
	  if (r >= 0) { i64 h = r;
	    check("sem_wait 1", S(271, h, 0, 0, 0, 0, 0) == 0, 0);
	    check("sem_trywait 2", S(272, h, 0, 0, 0, 0, 0) == 0, 0);
	    check("sem_trywait is EAGAIN at zero", S(272, h, 0, 0, 0, 0, 0) == -35, 0);
	    check("sem_post", S(273, h, 0, 0, 0, 0, 0) == 0, 0);
	    check("sem_trywait after post", S(272, h, 0, 0, 0, 0, 0) == 0, 0);
	    check("sem_close", S(269, h, 0, 0, 0, 0, 0) == 0, 0); }
	  check("sem_unlink", S(270, "/darwinator-sem", 0, 0, 0, 0, 0) == 0, 0);
	  check("sem_open without O_CREAT is ENOENT", S(268, "/darwinator-sem", 0, 0, 0, 0, 0) == -2, 0); }
	{ static char ent[64]; static int res[2] = { 99, 99 };
	  unsigned o0 = 32, o1 = 40; int f0 = 4, f1 = 2;
	  memcpy(ent, &o0, 4); memcpy(ent + 4, &f0, 4); memcpy(ent + 16, &o1, 4); memcpy(ent + 20, &f1, 4);
	  memcpy(ent + 32, "/\0", 2); memcpy(ent + 40, "/nonexistent\0", 13);
	  r = S(284, ent, 64, res, 0, 0, 0);
	  check("access_extended", r == 0 && res[0] == 0 && res[1] == 2, res[1]);
	  f0 = 1 << 9; memcpy(ent + 4, &f0, 4); res[0] = 99;
	  r = S(284, ent, 64, res, 0, 0, 0);
	  check("access_extended with _READ_OK", r == 0 && res[0] == 0 && res[1] == 2, res[0]); }

	/* signals' mask, limits, ids */
	{ unsigned set = 1U << 29, old = 0, cur = 0; r = S(SYS_sigprocmask, 1, &set, &old, 0, 0, 0);
	  check("sigprocmask (SIG_BLOCK SIGUSR1)", r == 0 && !(old & set), r);
	  r = S(SYS_sigprocmask, 1, 0, &cur, 0, 0, 0); check("  the mask has it", r == 0 && (cur & set), cur);
	  S(SYS_sigprocmask, 3, &old, 0, 0, 0, 0); }
	{ struct drlimit rl = { 0, 0 }; r = S(SYS_getrlimit, 8, &rl, 0, 0, 0, 0); check("getrlimit (RLIMIT_NOFILE)", r == 0 && rl.cur > 0 && rl.max >= rl.cur, r);
	  check("getrlimit with a bad resource is EINVAL", S(SYS_getrlimit, 99, &rl, 0, 0, 0, 0) == -22, 0); }
	check("getpgrp", S(SYS_getpgrp, 0, 0, 0, 0, 0, 0) > 0, 0);
	check("getsid", S(SYS_getsid, 0, 0, 0, 0, 0, 0) > 0, 0);
	check("getdtablesize", S(SYS_getdtablesize, 0, 0, 0, 0, 0, 0) > 0, 0);
	{ long ru[18]; r = S(SYS_getrusage, 0, ru, 0, 0, 0, 0); check("getrusage", r == 0, r); }
	{ unsigned g[16]; r = S(SYS_getgroups, 16, g, 0, 0, 0, 0); check("getgroups", r >= 0, r); }

	/* sysctl */
	{ int mib[2] = { 6, 3 }; int ncpu = 0; u64 len = sizeof(ncpu); r = S(SYS_sysctl, mib, 2, &ncpu, &len, 0, 0); check("sysctl hw.ncpu", r == 0 && ncpu > 0 && len == 4, ncpu); }
	{ int ncpu = 0; u64 len = sizeof(ncpu); r = S(SYS_sysctlbyname, "hw.ncpu", 7, &ncpu, &len, 0, 0); check("sysctlbyname hw.ncpu", r == 0 && ncpu > 0, r); }
	{ u64 mem = 0, len = sizeof(mem); r = S(SYS_sysctlbyname, "hw.memsize", 10, &mem, &len, 0, 0); check("sysctlbyname hw.memsize", r == 0 && mem > (64UL << 20) && len == 8, r); }
	{ char t[32]; u64 len = sizeof(t); r = S(SYS_sysctlbyname, "kern.ostype", 11, t, &len, 0, 0); check("sysctlbyname kern.ostype is Darwin", r == 0 && seq(t, "Darwin") && len == 7, r); }
	{ char t[3]; u64 len = sizeof(t); r = S(SYS_sysctlbyname, "kern.ostype", 11, t, &len, 0, 0); check("  too small a buffer is ENOMEM", r == -12, r); }
	{ u64 len = 0; r = S(SYS_sysctlbyname, "kern.hostname", 13, 0, &len, 0, 0); check("  size query (no buffer)", r == 0 && len > 1, len); }
	{ char t[8]; u64 len = sizeof(t); r = S(SYS_sysctlbyname, "no.such.name", 12, t, &len, 0, 0); check("  unknown name is ENOENT", r == -2, r); }

	/* sockets */
	{ int sv[2] = { -1, -1 }; char c[4]; r = S(SYS_socketpair, 1, 1, 0, sv, 0, 0); check("socketpair (AF_UNIX)", r == 0 && sv[0] >= 0, r);
	  if (r == 0) { S(SYS_write, sv[0], "xyz", 3, 0, 0, 0); check("  data crosses", S(SYS_read, sv[1], c, 3, 0, 0, 0) == 3 && c[1] == 'y', 0);
		  int one = 1; check("setsockopt SO_NOSIGPIPE (Darwin's number)", S(SYS_setsockopt, sv[0], 0xffff, 0x1022, &one, 4, 0) == 0, 0);
		  int v = 0; unsigned vl = 4; r = S(SYS_getsockopt, sv[0], 0xffff, 0x1022, &v, &vl, 0); check("getsockopt SO_NOSIGPIPE", r == 0 && v != 0, r);
		  S(SYS_close, sv[0], 0, 0, 0, 0, 0); S(SYS_close, sv[1], 0, 0, 0, 0, 0); } }
	r = S(SYS_socket, 30, 2, 0, 0, 0, 0); check("socket (AF_INET6 = 30 on Darwin)", r >= 0, r); if (r >= 0) S(SYS_close, r, 0, 0, 0, 0, 0);
	r = S(SYS_socket, 2, 1, 0, 0, 0, 0);
	if (r >= 0) {	/* bind a TCP socket to 127.0.0.1:0 and listen */
		unsigned char sa[16] = { 16, 2, 0, 0, 127, 0, 0, 1, 0 }; check("bind (127.0.0.1)", S(SYS_bind, r, sa, 16, 0, 0, 0) == 0, 0);
		check("listen", S(SYS_listen, r, 1, 0, 0, 0, 0) == 0, 0); S(SYS_close, r, 0, 0, 0, 0, 0); }
	else check("socket (AF_INET)", 0, r);

	/* mknod (a FIFO), the device numbers of stat */
	{ struct dstat64 st; r = S(SYS_mknod, "/tmp/dsc/fifo2", 010600, 0, 0, 0, 0); check("mknod (S_IFIFO)", r == 0, r);
	  r = S(SYS_stat64, "/tmp/dsc/fifo2", &st, 0, 0, 0, 0); check("  stat says FIFO", r == 0 && (st.st_mode & 0170000) == 010000 && st.st_dev != 0, st.st_mode);
	  r = S(SYS_stat64, "/dev/null", &st, 0, 0, 0, 0); check("stat /dev/null: st_rdev is set", r == 0 && st.st_rdev != 0 && (st.st_mode & 0170000) == 020000, st.st_rdev);
	  S(SYS_unlink, "/tmp/dsc/fifo2", 0, 0, 0, 0, 0); }

	/* getfsstat64 */
	{ static struct dstatfs fs[4]; r = S(SYS_getfsstat64, 0, 0, 2, 0, 0, 0); check("getfsstat64 (count)", r >= 1, r);
	  r2 = S(SYS_getfsstat64, fs, sizeof(fs), 2, 0, 0, 0); check("getfsstat64 (structures)", r2 >= 1 && fs[0].fstype[0] && fs[0].on[0] == '/' && fs[0].blocks > 0, r2); }

	/* kqueue: a pipe becoming readable, a user event (Darwin's filter -10), a timer, kevent64 */
	{ i64 kq = S(SYS_kqueue, 0, 0, 0, 0, 0, 0); i64 p1, p0; struct dkev c, e[2]; check("kqueue", kq >= 0, kq);
	  p0 = dsys_2(SYS_pipe, &p1);
	  c = mk((u64)p0, -1, 0x1, 0, 0, 0x1234);
	  r = S(SYS_kevent, kq, &c, 1, 0, 0, 0); check("kevent EV_ADD EVFILT_READ", r == 0, r);
	  r = S(SYS_kevent, kq, 0, 0, e, 1, &tv0); check("  nothing to read yet", r == 0, r);
	  S(SYS_write, p1, "z", 1, 0, 0, 0);
	  r = S(SYS_kevent, kq, 0, 0, e, 1, &tv1); check("  readable: filter, ident, data, udata", r == 1 && e[0].filter == -1 && e[0].ident == (u64)p0 && e[0].data >= 1 && e[0].udata == 0x1234, r);
	  S(SYS_read, p0, buf, 1, 0, 0, 0);	/* the pipe is empty again: a level-triggered filter would report it every time */
	  c = (struct dkev){ 7, -10, 0x21, 0, 0, 0x55, { 0, 0 } };	/* EVFILT_USER, EV_ADD | EV_CLEAR */
	  r = dsys7(SYS_kevent64, kq, (i64)&c, 1, 0, 0, 0, 0); check("kevent64 EV_ADD EVFILT_USER", r == 0, r);
	  c = mk(7, -10, 0, 0x01000000, 0, 0x55);	/* NOTE_TRIGGER */
	  r = dsys7(SYS_kevent64, kq, (i64)&c, 1, (i64)e, 1, 0, (i64)&tv1); check("  triggered: filter -10 comes back", r == 1 && e[0].filter == -10 && e[0].ident == 7 && e[0].udata == 0x55, r);
	  c = mk(1, -7, 0x11, 0, 20, 0);	/* EVFILT_TIMER 20 ms, EV_ADD | EV_ONESHOT */
	  r = S(SYS_kevent, kq, &c, 1, 0, 0, 0); check("kevent EVFILT_TIMER", r == 0, r);
	  r = S(SYS_kevent, kq, 0, 0, e, 2, &tv2); check("  timer fires", r >= 1 && e[0].filter == -7, r);
	  c = mk(9, -8, 0x1, 0, 0, 0);	/* EVFILT_MACHPORT: not supported: an error event */
	  r = S(SYS_kevent, kq, &c, 1, e, 1, &tv0); check("  a Mach port filter is reported as an error event", r == 1 && (e[0].flags & 0x4000), r);
	  S(SYS_close, kq, 0, 0, 0, 0, 0); S(SYS_close, p0, 0, 0, 0, 0, 0); S(SYS_close, p1, 0, 0, 0, 0, 0); }

	/* signals: sigaction with a handler, SA_SIGINFO, sigpending, pthread_sigmask, itimers */
	{ struct dsa sa = { (u64)on_usr1, 0, 0, 0 }, old = { 1, 1, 1, 1 }; u64 me = S(SYS_getpid, 0, 0, 0, 0, 0, 0);
	  r = S(SYS_sigaction, 30, &sa, &old, 0, 0, 0); check("sigaction (SIGUSR1)", r == 0 && old.handler == 0, r);
	  got_sig = 0; S(SYS_kill, me, 30, 1, 0, 0, 0); check("  the handler ran and returned (kill SIGUSR1 to self)", got_sig == 30, got_sig);
	  sa.handler = (u64)on_usr1_info; sa.flags = 0x40; got_sig = 0; got_signo_from_info = 0;
	  r = S(SYS_sigaction, 30, &sa, &old, 0, 0, 0); check("sigaction SA_SIGINFO (the old handler comes back)", r == 0 && old.handler == (u64)on_usr1, r);
	  S(SYS_kill, me, 30, 1, 0, 0, 0); check("  siginfo: si_signo", got_sig == 30 && got_signo_from_info == 30, got_signo_from_info);
#if defined(__x86_64__)
	  check("  ucontext in Darwin's layout (uc_mcsize 712, uc_mcontext right after it)", uc_mcsize == 712 && uc_mcptr_ok, uc_mcsize);
	  check("  the registers of the interrupted code (rip in the program, rsp on the stack)", uc_rip >= 0x100000000UL && uc_rip < 0x200000000UL && uc_rsp != 0 && uc_rbp != 0, uc_rip);
#endif
	  { unsigned m = 1U << 29, pend = 0, oldm = 0; got_sig = 0;
	    S(SYS_pthread_sigmask, 1, &m, &oldm, 0, 0, 0); S(SYS_kill, me, 30, 1, 0, 0, 0);
	    r = S(SYS_sigpending, &pend, 0, 0, 0, 0, 0); check("sigpending (blocked SIGUSR1 is pending)", r == 0 && (pend & m) && got_sig == 0, pend);
	    S(SYS_pthread_sigmask, 3, &oldm, 0, 0, 0, 0); check("  unblocked: the handler runs", got_sig == 30, got_sig); }
	  sa.handler = 0; sa.flags = 0; S(SYS_sigaction, 30, &sa, 0, 0, 0, 0);
	  check("sigaction with a bad signal is EINVAL", S(SYS_sigaction, 99, &sa, 0, 0, 0, 0) == -22, 0);
	  { struct ditimer it = { { 0, 0, 0 }, { 100, 0, 0 } }, cur; r = S(SYS_setitimer, 0, &it, 0, 0, 0, 0); check("setitimer", r == 0, r);
	    r = S(SYS_getitimer, 0, &cur, 0, 0, 0, 0); check("  getitimer", r == 0 && cur.value.sec >= 99 && cur.value.sec <= 100, cur.value.sec);
	    it.value.sec = 0; S(SYS_setitimer, 0, &it, 0, 0, 0, 0); } }

	/* getattrlist: name, type, times, mode, file id, data length */
	{ struct { unsigned short n, res; unsigned cmn, vol, dir, file, fork; } al = { 5, 0, 0x1 | 0x8 | 0x400 | 0x20000 | 0x02000000, 0, 0, 0x200, 0 };
	  static char ab[512]; int fd3 = (int)S(SYS_open, "/tmp/dsc/a", O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
	  S(SYS_write, fd3, "hello", 5, 0, 0, 0); S(SYS_close, fd3, 0, 0, 0, 0, 0);
	  r = S(SYS_getattrlist, "/tmp/dsc/a", &al, ab, sizeof(ab), 0, 0);
	  if (r == 0) { unsigned total = *(unsigned *)ab; int off = *(int *)(ab + 4); unsigned nl = *(unsigned *)(ab + 8); unsigned type = *(unsigned *)(ab + 12);
		  const char *nm = ab + 4 + off; unsigned mode = *(unsigned *)(ab + 12 + 4 + 16); u64 fid = *(u64 *)(ab + 12 + 4 + 16 + 4); u64 dl = *(u64 *)(ab + 12 + 4 + 16 + 4 + 8);
		  check("getattrlist (name, objtype, modtime, mode, fileid, datalength)", seq(nm, "a") && nl == 2 && type == 1 && (mode & 0777) == 0640 && fid != 0 && dl == 5 && total == 56, (i64)dl); }
	  else check("getattrlist", 0, r);
	  al.cmn = 0x80000000u; r = S(SYS_getattrlist, "/tmp/dsc/a", &al, ab, sizeof(ab), 0, 0);
	  check("  an attribute that is not supported is EINVAL", r == -22, r);
	  al.cmn = 1; r = S(SYS_getattrlist, "/tmp/dsc/none", &al, ab, sizeof(ab), 0, 0); check("  a missing file is ENOENT", r == -2, r); }

	/* the *at family: AT_FDCWD is -2 on Darwin */
	{ i64 dirfd = S(SYS_open, "/tmp/dsc", 0, 0, 0, 0, 0); struct dstat64 st; char lb[64]; for (i = 0; i < 64; i++) lb[i] = 0;
	  check("open a directory for the *at calls", dirfd >= 0, dirfd);
	  r = S(SYS_openat, dirfd, "at1", O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0); check("openat (relative to a directory)", r >= 0, r); if (r >= 0) S(SYS_close, r, 0, 0, 0, 0, 0);
	  r = S(SYS_openat, -2, "/tmp/dsc/at2", O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0); check("openat (AT_FDCWD = -2)", r >= 0, r); if (r >= 0) S(SYS_close, r, 0, 0, 0, 0, 0);
	  r = S(SYS_fstatat64, dirfd, "at1", &st, 0, 0, 0); check("fstatat64", r == 0 && (st.st_mode & 0170000) == 0100000, r);
	  check("faccessat", S(SYS_faccessat, dirfd, "at1", 0, 0, 0, 0) == 0 && S(SYS_faccessat, dirfd, "none", 0, 0, 0, 0) == -2, 0);
	  check("fchmodat", S(SYS_fchmodat, dirfd, "at1", 0600, 0, 0, 0) == 0, 0);
	  check("symlinkat + readlinkat", S(SYS_symlinkat, "at1", dirfd, "atl", 0, 0, 0) == 0 && S(SYS_readlinkat, dirfd, "atl", lb, 63, 0, 0) == 3 && lb[0] == 'a', 0);
	  r = S(SYS_fstatat64, dirfd, "atl", &st, 0x20, 0, 0); check("fstatat64 with AT_SYMLINK_NOFOLLOW (0x20) sees the link", r == 0 && (st.st_mode & 0170000) == 0120000, st.st_mode);
	  check("linkat", S(SYS_linkat, dirfd, "at1", dirfd, "atk", 0, 0) == 0, 0);
	  check("renameat", S(SYS_renameat, dirfd, "atk", dirfd, "atr", 0, 0) == 0, 0);
	  check("mkdirat", S(SYS_mkdirat, dirfd, "atd", 0755, 0, 0, 0) == 0, 0);
	  check("unlinkat (AT_REMOVEDIR = 0x80 on Darwin)", S(SYS_unlinkat, dirfd, "atd", 0x80, 0, 0, 0) == 0, 0);
	  check("unlinkat of the files", S(SYS_unlinkat, dirfd, "atr", 0, 0, 0, 0) == 0 && S(SYS_unlinkat, dirfd, "atl", 0, 0, 0, 0) == 0 && S(SYS_unlinkat, dirfd, "at1", 0, 0, 0, 0) == 0 && S(SYS_unlinkat, -2, "/tmp/dsc/at2", 0, 0, 0, 0) == 0, 0);
	  S(SYS_close, dirfd, 0, 0, 0, 0, 0); }

	/* extended attributes (not on every file system: tmpfs has none) */
	{ i64 sr = S(SYS_setxattr, "/tmp/dsc/a", "com.test.x", "value", 5, 0, 0);
	  if (sr == -45 || sr == -102) out("skip extended attributes (not supported here)\n");
	  else { char vb[16]; i64 gr; check("setxattr", sr == 0, sr);
		  gr = S(SYS_getxattr, "/tmp/dsc/a", "com.test.x", vb, sizeof(vb), 0, 0); check("getxattr", gr == 5 && vb[0] == 'v' && vb[4] == 'e', gr);
		  gr = S(SYS_getxattr, "/tmp/dsc/a", "com.test.x", 0, 0, 0, 0); check("  the size of an attribute (no buffer)", gr == 5, gr);
		  { char nb[64]; gr = S(SYS_listxattr, "/tmp/dsc/a", nb, sizeof(nb), 0, 0, 0); check("listxattr (NUL terminated names)", gr == 11 && seq(nb, "com.test.x"), gr); }
		  check("removexattr", S(SYS_removexattr, "/tmp/dsc/a", "com.test.x", 0, 0, 0, 0) == 0, 0);
		  gr = S(SYS_getxattr, "/tmp/dsc/a", "com.test.x", vb, sizeof(vb), 0, 0); check("  gone: ENOATTR (93)", gr == -93, gr); } }

	/* ulock: no waiting when the value is another; a timeout; thread id; kill by thread id; csops; the shared region */
	{ static volatile unsigned lockword = 1; r = S(SYS_ulock_wait, 1, &lockword, 2, 0, 0, 0); check("ulock_wait with another value returns at once", r == 0, r);
	  r = S(SYS_ulock_wait, 1, &lockword, 1, 20000, 0, 0); check("ulock_wait times out (ETIMEDOUT = 60)", r == -60, r);
	  r = S(SYS_ulock_wait, 1 | 0x01000000, &lockword, 1, 20000, 0, 0); check("  with ULF_NO_ERRNO the error is the negative result", r == -60, r);
	  r = S(SYS_ulock_wake, 1, &lockword, 0, 0, 0, 0); check("ulock_wake with nobody waiting", r == 0, r); }
	{ i64 tid = S(SYS_thread_selfid, 0, 0, 0, 0, 0, 0); check("thread_selfid", tid > 0, tid); check("__pthread_kill (signal 0 to this thread)", S(SYS_pthread_kill, tid, 0, 0, 0, 0, 0) == 0, 0); }
	{ unsigned cs = 7; r = S(SYS_csops, 0, 0, &cs, 4, 0, 0); check("csops (status)", r == 0 && cs == 0, cs); }
	check("shared_region_check_np is ENOMEM (no shared region)", S(SYS_shared_region_check_np, &buf, 0, 0, 0, 0, 0) == -12, 0);

	/* threads: bsdthread_register, bsdthread_create (the registers of the new thread), bsdthread_terminate */
	{ i64 base = S(SYS_mmap_, 0, 65536, 3, 0x1002, -1, 0), top; static volatile unsigned dummy = 1;
	  check("bsdthread_create without bsdthread_register is EINVAL", S(SYS_bsdthread_create, tfunc, 1, base + 65000, 0x1000, 0, 0) == -22, 0);
	  r = S(SYS_bsdthread_register, thread_entry, 0, 0, 0, 0, 0); check("bsdthread_register", r == 0, r);
	  top = (base + 65536 - 64) & ~15L; thread_val = 0; thread_port = 0;
	  r = S(SYS_bsdthread_create, tfunc, 0x1234, top, 0xabc000, 0, 0); check("bsdthread_create gives the pthread back", r == 0xabc000, r);
	  for (i = 0; i < 300 && thread_val == 0; i++) S(SYS_ulock_wait, 1, &dummy, 1, 10000, 0, 0);
	  check("  the new thread ran func(arg) (registers of the start routine)", thread_val == 0x1234, (i64)thread_val);
	  check("  its kernel port is its thread id", thread_port > 0, (i64)thread_port); }

	/* psynch: a drop before the wait is remembered (mutex); a signal with nobody waiting is lost (condition variable); a wait that times out; a thread that waits and is woken */
	{ static unsigned long mx, cv, mx2; i64 base2, top2;
	  r = S(SYS_psynch_mutexdrop, &mx, 0, 0, 0, 0, 0); check("psynch_mutexdrop", r == 0, r);
	  r = S(SYS_psynch_mutexwait, &mx, 7, 0, 0, 0, 0); check("psynch_mutexwait after a drop returns at once (gives the generation back)", r == 7, r);
	  r = S(SYS_psynch_cvsignal, &cv, 0, 0, 0, 0, 0); check("psynch_cvsignal with no waiter", r == 0, r);
	  r = dsys8(SYS_psynch_cvwait, (i64)&cv, 0, 0, 0, 0, 0, 0, 30000000); check("psynch_cvwait times out (ETIMEDOUT = 60): the earlier signal was not kept", r == -60, r);
	  psy_mx = (u64)&mx2; psy_state = 0;
	  base2 = S(SYS_mmap_, 0, 65536, 3, 0x1002, -1, 0); top2 = (base2 + 65536 - 64) & ~15L;
	  S(SYS_bsdthread_register, thread_entry, 0, 0, 0, 0, 0);
	  r = S(SYS_bsdthread_create, twait, 0, top2, 0xdef000, 0, 0); check("a second thread is made", r == 0xdef000, r);
	  for (i = 0; i < 200 && psy_state < 1; i++) S(SYS_ulock_wait, 1, &dummy2, 1, 5000, 0, 0);
	  check("  it is blocked in psynch_mutexwait (it has not run on)", psy_state == 1, psy_state);
	  S(SYS_psynch_mutexdrop, &mx2, 0, 0, 0, 0, 0);
	  for (i = 0; i < 400 && psy_state < 2; i++) S(SYS_ulock_wait, 1, &dummy2, 1, 5000, 0, 0);
	  check("  psynch_mutexdrop wakes it", psy_state == 2, psy_state); }
	check("workq_open", S(SYS_workq_open, 0, 0, 0, 0, 0, 0) == 0, 0);
	check("workq_kernreturn returns 0 (a thread back to the pool: what a Mac answers)", S(SYS_workq_kernreturn, 0, 0, 0, 0, 0, 0) == 0, 0);

	/* Mach traps */
	{ u64 task = dmach(28, 0, 0, 0, 0, 0, 0), host = dmach(29, 0, 0, 0, 0, 0, 0), rp = dmach(26, 0, 0, 0, 0, 0, 0), th = dmach(27, 0, 0, 0, 0, 0, 0);
	  check("task_self_trap, host_self_trap, mach_reply_port, thread_self_trap give names", task == 0x203 && host == 0x1d03 && rp != 0 && th != 0 && rp != dmach(26, 0, 0, 0, 0, 0, 0), rp);
	  { u64 addr = 0; i64 kr = dmach(10, (i64)task, (i64)&addr, 65536, 1, 0, 0);
	    check("mach_vm_allocate (anywhere)", kr == 0 && addr != 0, kr);
	    if (kr == 0) { *(volatile char *)addr = 5; check("  the memory is usable", *(volatile char *)addr == 5, 0);
		    check("mach_vm_protect", dmach(14, (i64)task, (i64)addr, 65536, 0, 1, 0) == 0, 0);
		    check("mach_vm_deallocate", dmach(12, (i64)task, (i64)addr, 65536, 0, 0, 0) == 0, 0); } }
	  { unsigned tb[2] = { 0, 0 }; check("mach_timebase_info_trap (1/1: nanoseconds)", dmach(89, (i64)tb, 0, 0, 0, 0, 0) == 0 && tb[0] == 1 && tb[1] == 1, tb[0]); }
	  { struct dtv t1, t2; i64 base = 0, kr; S(SYS_gettimeofday, &t1, 0, 0, 0, 0, 0); kr = dmach(90, 0, 0, 0, 0, 0, 0); (void)base; (void)t2;
	    check("mach_wait_until_trap (a past time returns at once)", kr == 0, kr); }
	  /* Mach IPC inside the process: a port, a send right, a message with data to itself and its reply port, a receive with a timeout, a bad destination */
	  { unsigned pn = 0; i64 kr = dmach(16, (i64)task, 1, (i64)&pn, 0, 0, 0);
	    check("mach_port_allocate (receive right)", kr == 0 && pn != 0, kr);
	    kr = dmach(21, (i64)task, pn, pn, 20, 0, 0); check("mach_port_insert_right (MAKE_SEND)", kr == 0, kr);
	    { unsigned rp = (unsigned)dmach(26, 0, 0, 0, 0, 0, 0); unsigned msg[10] = { 19 | (21 << 8), 40, pn, rp, 0, 77, 0xcafe, 0xbabe, 0x1234, 0x5678 }; unsigned rcv[16]; int k;
	      for (k = 0; k < 16; k++) rcv[k] = 0;
	      kr = dmach7(31, (i64)msg, 1, 40, 0, 0, 0, 0); check("mach_msg send (header, id 77, 16 bytes of data)", kr == 0, kr);
	      kr = dmach7(31, (i64)rcv, 2, 0, 64, pn, 0, 0); check("mach_msg receive: the same message", kr == 0 && rcv[1] == 40 && rcv[5] == 77 && rcv[6] == 0xcafe && rcv[9] == 0x5678, kr);
	      check("  the reply port is the remote port, the destination the local port (swapped)", rcv[2] == rp && rcv[3] == pn, rcv[2]);
	      kr = dmach7(31, (i64)rcv, 2 | 0x100, 0, 64, pn, 20, 0); check("mach_msg receive of an empty port with a timeout is MACH_RCV_TIMED_OUT", kr == 0x10004003, kr);
	      msg[2] = 0x5555; kr = dmach7(31, (i64)msg, 1, 40, 0, 0, 0, 0); check("mach_msg send to a name that is no port is MACH_SEND_INVALID_DEST", kr == 0x10000003, kr);
	      msg[2] = pn; dmach7(31, (i64)msg, 1, 40, 0, 0, 0, 0);
	      kr = dmach7(31, (i64)rcv, 2, 0, 16, pn, 0, 0); check("mach_msg receive with a buffer that is too small is MACH_RCV_TOO_LARGE", kr == 0x10004004, kr); }
	    kr = dmach(19, (i64)task, pn, 1, -1, 0, 0); check("mach_port_mod_refs (receive, -1) destroys the port", kr == 0, kr);
	  /* between processes: the special port 4 (bootstrap) is set by a message to the task port, the child gets it at fork and sends through it */
	  { unsigned P = 0, rp2, req[13], rcv2[16]; i64 kr2; int k;
	    dmach(16, (i64)task, 1, (i64)&P, 0, 0, 0); dmach(21, (i64)task, P, P, 20, 0, 0); rp2 = (unsigned)dmach(26, 0, 0, 0, 0, 0, 0);
	    for (k = 0; k < 13; k++) req[k] = 0; for (k = 0; k < 16; k++) rcv2[k] = 0;
	    req[0] = 0x80000000u | 19 | (21 << 8); req[1] = 52; req[2] = (unsigned)task; req[3] = rp2; req[5] = 3410; req[6] = 1; req[7] = P; req[9] = 20 << 16; req[12] = 4;
	    kr2 = dmach7(31, (i64)req, 3, 52, 64, rp2, 2000, 0); check("task_set_special_port (id 3410) with a port descriptor: the reply says success", kr2 == 0 && ((rcv2[0] = 0), 1), kr2);
	    /* the reply is in req (the same buffer was used for both: send then receive) */
	    check("  reply id 3510, return code 0", req[5] == 3510 && req[8] == 0, req[5]);
	    { i64 ch = S(SYS_fork, 0, 0, 0, 0, 0, 0);
	      if (ch == 0) { unsigned crp = (unsigned)dmach(26, 0, 0, 0, 0, 0, 0), q[10]; for (k = 0; k < 10; k++) q[k] = 0;
		      q[0] = 19 | (21 << 8); q[1] = 36; q[2] = 0x203; q[3] = crp; q[5] = 3409; q[8] = 4;
		      if (dmach7(31, (i64)q, 3, 36, 40, crp, 3000, 0) == 0 && q[5] == 3509 && q[7] != 0) { unsigned msg2[7] = { 19, 28, q[7], 0, 0, 99, 0xdead }; dmach7(31, (i64)msg2, 1, 28, 0, 0, 0, 0); S(SYS_exit, 0, 0, 0, 0, 0, 0); }
		      S(SYS_exit, 1, 0, 0, 0, 0, 0); }
	      kr2 = dmach7(31, (i64)rcv2, 2 | 0x100, 0, 64, P, 4000, 0);
	      check("a message from the child, through the bootstrap port it inherited, arrives (id 99, data 0xdead)", kr2 == 0 && rcv2[5] == 99 && rcv2[6] == 0xdead, kr2);
	      { int st = -1; S(SYS_wait4, ch, &st, 0, 0, 0, 0); check("  the child ended well (it found the special port)", st == 0, st); } }
	    /* a right in a message: the send right to Q goes to P in a complex message and comes out as a name with the disposition PORT_SEND */
	    { unsigned Q = 0, cm[10] = { 0x80000000u | 19, 40, P, 0, 0, 55, 1, 0, 0, 20 << 16 }, rv[16]; for (k = 0; k < 16; k++) rv[k] = 0;
	      dmach(16, (i64)task, 1, (i64)&Q, 0, 0, 0); cm[7] = Q;
	      kr2 = dmach7(31, (i64)cm, 1, 40, 0, 0, 0, 0); check("mach_msg with a port descriptor (MAKE_SEND of Q) is sent", kr2 == 0, kr2);
	      kr2 = dmach7(31, (i64)rv, 2, 0, 64, P, 0, 0); check("  received: the descriptor holds the name of Q with the disposition 17 (PORT_SEND)", kr2 == 0 && rv[7] == Q && ((rv[9] >> 16) & 0xff) == 17, rv[9]);
	      { unsigned m3[7] = { 19, 28, Q, 0, 0, 56, 0x77 }, r3[16]; for (k = 0; k < 16; k++) r3[k] = 0; kr2 = dmach7(31, (i64)m3, 1, 28, 0, 0, 0, 0); kr2 |= dmach7(31, (i64)r3, 2, 0, 64, Q, 0, 0); check("  and the right works: a message to Q arrives", kr2 == 0 && r3[5] == 56, kr2); } }
	  /* MIG routines of the task and the host port (the ids come from the .defs of xnu: mach_port 3200, task 3400, host 200) */
	  { unsigned q[24]; unsigned rpm = (unsigned)dmach(26, 0, 0, 0, 0, 0, 0), PT = 0; int k; i64 kr4;
	    dmach(16, (i64)task, 1, (i64)&PT, 0, 0, 0); dmach(21, (i64)task, PT, PT, 20, 0, 0);
	    for (k = 0; k < 24; k++) q[k] = 0; q[0] = 19 | (21 << 8); q[1] = 36; q[2] = (unsigned)task; q[3] = rpm; q[5] = 3201; q[8] = PT;
	    kr4 = dmach7(31, (i64)q, 3, 36, 96, rpm, 2000, 0); check("mach_port_type (id 3201): receive and send rights (the bits 1<<17 and 1<<16)", kr4 == 0 && q[5] == 3301 && q[8] == 0 && q[9] == ((1u << 17) | (1u << 16)), q[9]);
	    for (k = 0; k < 24; k++) q[k] = 0; q[0] = 19 | (21 << 8); q[1] = 44; q[2] = (unsigned)task; q[3] = rpm; q[5] = 3217; q[8] = PT; q[9] = 2; q[10] = 10;
	    kr4 = dmach7(31, (i64)q, 3, 44, 96, rpm, 2000, 0); check("mach_port_get_attributes (id 3217, RECEIVE_STATUS): 10 ints, queue limit 5", kr4 == 0 && q[8] == 0 && q[9] == 10 && q[10 + 4] == 5, q[9]);
	    for (k = 0; k < 24; k++) q[k] = 0; q[0] = 19 | (21 << 8); q[1] = 40; q[2] = (unsigned)task; q[3] = rpm; q[5] = 3405; q[8] = 20; q[9] = 12;
	    kr4 = dmach7(31, (i64)q, 3, 40, 96, rpm, 2000, 0); check("task_info (id 3405, MACH_TASK_BASIC_INFO): 12 ints, a virtual size", kr4 == 0 && q[8] == 0 && q[9] == 12 && (q[10] | q[11]) != 0, q[9]);
	    for (k = 0; k < 24; k++) q[k] = 0; q[0] = 19 | (21 << 8); q[1] = 40; q[2] = 0x1d03; q[3] = rpm; q[5] = 200; q[8] = 1; q[9] = 12;
	    kr4 = dmach7(31, (i64)q, 3, 40, 96, rpm, 2000, 0); check("host_info (id 200, HOST_BASIC_INFO) to the host port: 12 ints, the number of CPUs, the CPU type", kr4 == 0 && q[8] == 0 && q[9] == 12 && q[10] >= 1 && (q[13] == 0x01000007 || q[13] == 0x0100000c), q[13]);
	    for (k = 0; k < 24; k++) q[k] = 0; q[0] = 19 | (21 << 8); q[1] = 24; q[2] = 0x1d03; q[3] = rpm; q[5] = 202;
	    kr4 = dmach7(31, (i64)q, 3, 24, 96, rpm, 2000, 0); check("_host_page_size (id 202)", kr4 == 0 && q[8] == 0 && q[9] >= 4096, q[9]); }
	  { unsigned q[64]; unsigned rpm = (unsigned)dmach(26, 0, 0, 0, 0, 0, 0); int k; i64 kr4;
	    for (k = 0; k < 64; k++) q[k] = 0; q[0] = 19 | (21 << 8); q[1] = 40; q[2] = 0x1d03; q[3] = rpm; q[5] = 220; q[8] = 4; q[9] = 38;
	    kr4 = dmach7(31, (i64)q, 3, 40, 256, rpm, 2000, 0); check("host_statistics64 (id 220, HOST_VM_INFO64): 38 ints, free pages", kr4 == 0 && q[8] == 0 && q[9] == 38 && q[10] > 0, q[5] * 1000 + q[8]);
	    for (k = 0; k < 64; k++) q[k] = 0; q[0] = 19 | (21 << 8); q[1] = 40; q[2] = 0x1d03; q[3] = rpm; q[5] = 217; q[8] = 3; q[9] = 4;
	    kr4 = dmach7(31, (i64)q, 3, 40, 256, rpm, 2000, 0); check("host_statistics (id 217, HOST_CPU_LOAD_INFO): 4 ints", kr4 == 0 && q[8] == 0 && q[9] == 4, q[9]); }
	  /* mach_port_request_notification (id 3213): a dead-name notification (id 72) arrives when the port dies */
	  { unsigned P = 0, N = 0, q[64], rb[64]; unsigned rpm = (unsigned)dmach(26, 0, 0, 0, 0, 0, 0); int k; i64 kr5;
	    dmach(16, (i64)task, 1, (i64)&P, 0, 0, 0); dmach(21, (i64)task, P, P, 20, 0, 0);
	    dmach(16, (i64)task, 1, (i64)&N, 0, 0, 0);
	    for (k = 0; k < 64; k++) q[k] = 0;
	    q[0] = 19 | (21 << 8) | 0x80000000u; q[1] = 60; q[2] = (unsigned)task; q[3] = rpm; q[5] = 3213; q[6] = 1; q[7] = N; q[9] = (21u << 16); q[10] = 0; q[11] = 0;
	    q[12] = P; q[13] = 72; q[14] = 0;
	    kr5 = dmach7(31, (i64)q, 3, 60, 256, rpm, 2000, 0);
	    check("mach_port_request_notification (id 3213) is answered with a complex reply", kr5 == 0 && q[5] == 3313 && q[6] == 1, q[5]);
	    dmach(19, (i64)task, P, 1, -1, 0, 0);
	    for (k = 0; k < 64; k++) rb[k] = 0;
	    kr5 = dmach7(31, (i64)rb, 2 | 0x100, 0, 256, N, 500, 0);
	    check("the dead-name notification (id 72) arrives with the name of the port", kr5 == 0 && rb[5] == 72 && rb[8] == P, rb[5]); }
	  /* mach_port_names (id 3200): two out-of-line arrays; the port that was just made is in the list with the receive and send bits */
	  { unsigned P = 0, q[64], rb[64]; unsigned rpm = (unsigned)dmach(26, 0, 0, 0, 0, 0, 0); int k, found = 0; i64 kr5; unsigned *nm, *ty, n;
	    dmach(16, (i64)task, 1, (i64)&P, 0, 0, 0); dmach(21, (i64)task, P, P, 20, 0, 0);
	    for (k = 0; k < 64; k++) { q[k] = 0; rb[k] = 0; }
	    q[0] = 19 | (21 << 8); q[1] = 32; q[2] = (unsigned)task; q[3] = rpm; q[5] = 3200;
	    kr5 = dmach7(31, (i64)q, 3, 32, 256, rpm, 2000, 0);
	    n = q[17];
	    nm = (unsigned *)(unsigned long)(((unsigned long)q[7] << 0) | ((unsigned long)q[8] << 32)); ty = (unsigned *)(unsigned long)(((unsigned long)q[11] << 0) | ((unsigned long)q[12] << 32));
	    check("mach_port_names (id 3200): a complex reply with two arrays", kr5 == 0 && q[5] == 3300 && q[6] == 2 && n >= 3 && q[18] == n, kr5 * 1000 + q[5]);
	    if (kr5 == 0 && n >= 3 && n < 100) for (k = 0; k < (int)n; k++) if (nm[k] == P && ty[k] == ((1u << 17) | (1u << 16))) found = 1;
	    check("  the new port is listed with RECEIVE and SEND", found, n); }
	  /* no-more-senders (variant 70): sent when the last send right is gone */
	  { unsigned P = 0, N = 0, q[64], rb[64]; unsigned rpm = (unsigned)dmach(26, 0, 0, 0, 0, 0, 0); int k; i64 kr5;
	    dmach(16, (i64)task, 1, (i64)&P, 0, 0, 0); dmach(21, (i64)task, P, P, 20, 0, 0);
	    dmach(16, (i64)task, 1, (i64)&N, 0, 0, 0);
	    for (k = 0; k < 64; k++) q[k] = 0;
	    q[0] = 19 | (21 << 8) | 0x80000000u; q[1] = 60; q[2] = (unsigned)task; q[3] = rpm; q[5] = 3213; q[6] = 1; q[7] = N; q[9] = (21u << 16);
	    q[12] = P; q[13] = 70; q[14] = 0;
	    kr5 = dmach7(31, (i64)q, 3, 60, 256, rpm, 2000, 0);
	    check("request_notification (no-more-senders) is answered", kr5 == 0 && q[5] == 3313, q[5]);
	    for (k = 0; k < 64; k++) rb[k] = 0;
	    kr5 = dmach7(31, (i64)rb, 2 | 0x100, 0, 256, N, 30, 0);
	    check("  no notification while a send right exists", kr5 == 0x10004003, kr5);
	    dmach(19, (i64)task, P, 0 /* MACH_PORT_RIGHT_SEND */, -1, 0, 0);
	    for (k = 0; k < 64; k++) rb[k] = 0;
	    kr5 = dmach7(31, (i64)rb, 2 | 0x100, 0, 256, N, 500, 0);
	    check("  the no-more-senders notification (id 70) arrives when the last send right goes", kr5 == 0 && rb[5] == 70, kr5 * 1000 + rb[5]); }
	  /* a port set: two ports are members; a receive on the set gives the message of the one that has one (the local port of the header is that port) */
	  { unsigned S2 = 0, A = 0, B = 0, ma[7] = { 19, 28, 0, 0, 0, 11, 0x11 }, rb[16]; int k; for (k = 0; k < 16; k++) rb[k] = 0;
	    dmach(16, (i64)task, 3, (i64)&S2, 0, 0, 0); dmach(16, (i64)task, 1, (i64)&A, 0, 0, 0); dmach(16, (i64)task, 1, (i64)&B, 0, 0, 0);
	    dmach(21, (i64)task, A, A, 20, 0, 0); dmach(21, (i64)task, B, B, 20, 0, 0);
	    check("mach_port_allocate (port set) and move_member for two ports", S2 != 0 && dmach(20, (i64)task, A, S2, 0, 0, 0) == 0 && dmach(20, (i64)task, B, S2, 0, 0, 0) == 0, S2);
	    ma[2] = B; dmach7(31, (i64)ma, 1, 28, 0, 0, 0, 0);
	    { i64 kr3 = dmach7(31, (i64)rb, 2 | 0x100, 0, 64, S2, 2000, 0); check("  mach_msg receive on the set: the message sent to B (id 11), local port B", kr3 == 0 && rb[5] == 11 && rb[3] == B && rb[6] == 0x11, kr3); }
	    { i64 kr3 = dmach7(31, (i64)rb, 2 | 0x100, 0, 64, S2, 20, 0); check("  an empty set times out", kr3 == 0x10004003, kr3); }
	    dmach(20, (i64)task, A, 0, 0, 0, 0); ma[2] = A; dmach7(31, (i64)ma, 1, 28, 0, 0, 0, 0);
	    { i64 kr3 = dmach7(31, (i64)rb, 2 | 0x100, 0, 64, S2, 20, 0); check("  a port that left the set is not in it any more", kr3 == 0x10004003, kr3); } }
	  /* a Mach semaphore: made with the MIG routine semaphore_create of the task port, then the traps */
	  { unsigned rq[16], rps = (unsigned)dmach(26, 0, 0, 0, 0, 0, 0); int k; for (k = 0; k < 16; k++) rq[k] = 0;
	    rq[0] = 19 | (21 << 8); rq[1] = 40; rq[2] = (unsigned)task; rq[3] = rps; rq[5] = 3418; rq[8] = 0; rq[9] = 1;
	    { i64 kr3 = dmach7(31, (i64)rq, 3, 40, 64, rps, 2000, 0); unsigned sem = rq[7]; check("semaphore_create (id 3418): a send right to a new semaphore (value 1)", kr3 == 0 && rq[5] == 3518 && sem != 0, kr3);
	      check("  semaphore_wait_trap takes the value", dmach(36, sem, 0, 0, 0, 0, 0) == 0, 0);
	      check("  semaphore_timedwait_trap on 0 times out (KERN_OPERATION_TIMED_OUT 49)", dmach(38, sem, 0, 20000000, 0, 0, 0) == 49, 0);
	      check("  semaphore_signal_trap then semaphore_wait_trap", dmach(33, sem, 0, 0, 0, 0, 0) == 0 && dmach(36, sem, 0, 0, 0, 0, 0) == 0, 0); } }
	    /* dead ports: the send to a port whose receive right is gone fails */
	    { unsigned D2 = 0, dm[7] = { 19, 28, 0, 0, 0, 1, 2 }; dmach(16, (i64)task, 1, (i64)&D2, 0, 0, 0); dmach(21, (i64)task, D2, D2, 20, 0, 0); dm[2] = D2; dmach(19, (i64)task, D2, 1, -1, 0, 0);
	      kr2 = dmach7(31, (i64)dm, 1, 28, 0, 0, 0, 0); check("a send to a port that was destroyed is MACH_SEND_INVALID_DEST", kr2 == 0x10000003, kr2); } }
	  } }

	/* fgetattrlist; sysctl hw.tbfrequency */
	{ struct { unsigned short n, res; unsigned cmn, vol, dir, file, fork; } fal = { 5, 0, 0x8, 0, 0, 0x200, 0 }; static char fb[128]; int fdf = (int)S(SYS_open, "/tmp/dsc/a", 0, 0, 0, 0, 0);
	  r = S(SYS_fgetattrlist, fdf, &fal, fb, sizeof(fb), 0, 0); check("fgetattrlist (objtype, data length)", r == 0 && *(unsigned *)fb == 16 && *(unsigned *)(fb + 4) == 1 && *(u64 *)(fb + 8) == 5, r); S(SYS_close, fdf, 0, 0, 0, 0, 0); }
	{ u64 ps = 0, len = 8; r = S(SYS_sysctlbyname, "hw.pagesize", 11, &ps, &len, 0, 0); check("sysctlbyname hw.pagesize is 8 bytes (as on the Mac)", r == 0 && len == 8 && ps >= 4096, ps); }
	{ unsigned ps4 = 0; u64 len = 4; r = S(SYS_sysctlbyname, "hw.pagesize", 11, &ps4, &len, 0, 0); check("  and 4 bytes for a caller with an int", r == 0 && len == 4 && ps4 >= 4096, ps4); }
	{ u64 f = 0, len = 8; r = S(SYS_sysctlbyname, "hw.tbfrequency", 14, &f, &len, 0, 0); check("sysctlbyname hw.tbfrequency", r == 0 && f == 1000000000UL, r); }

	/* the old stat forms, getdirentries, *_extended, utimes, pathconf, guarded descriptors, shared memory, System V semaphores, the ENOTSUP answers */
	{ struct { int dev; unsigned ino; unsigned short mode, nlink; unsigned uid, gid; int rdev; i64 t[6]; i64 size, blocks; int blksize; unsigned flags, gen; int lspare; i64 q[2]; } st32; struct dstat64 st64;
	  r = S(SYS_stat32, "/tmp/dsc/a", &st32, 0, 0, 0, 0); S(SYS_stat64, "/tmp/dsc/a", &st64, 0, 0, 0, 0);
	  check("stat (32 bit inodes, 120 bytes)", r == 0 && st32.size == 5 && st32.mode == st64.st_mode && st32.ino == (unsigned)st64.st_ino && st32.blksize == st64.st_blksize, r);
	  r = S(SYS_lstat32, "/tmp/dsc/none", &st32, 0, 0, 0, 0); check("lstat of a missing file is ENOENT", r == -2, r);
	  { int fdq = (int)S(SYS_open, "/tmp/dsc/a", 0, 0, 0, 0, 0); r = S(SYS_fstat32, fdq, &st32, 0, 0, 0, 0); check("fstat (32 bit inodes)", r == 0 && st32.size == 5, r); S(SYS_close, fdq, 0, 0, 0, 0, 0); }
	  { u64 xs = 99; r = S(SYS_stat64_extended, "/tmp/dsc/a", &st64, 0, &xs, 0, 0); check("stat64_extended (no ACL: the size is 0)", r == 0 && xs == 0 && st64.st_size == 5, r); } }
	{ int fdd = (int)S(SYS_open, "/tmp/dsc", 0, 0, 0, 0, 0); long bs = 0; i64 n = S(SYS_getdirentries32, fdd, buf, sizeof(buf), &bs, 0, 0), o = 0; int seen = 0, bad = 0;
	  while (o < n) { unsigned short reclen = *(unsigned short *)(buf + o + 4); unsigned char namlen = (unsigned char)buf[o + 7]; const char *name = buf + o + 8; if (reclen < 9 || reclen % 4 || name[namlen]) bad = 1; if (seq(name, "a")) seen = 1; o += reclen; }
	  check("getdirentries (the old entry: d_ino 4, d_reclen 2, d_type, d_namlen, name)", n > 0 && seen && !bad, n); S(SYS_close, fdd, 0, 0, 0, 0, 0); }
	{ struct dtv tv2[2] = { { 1000000, 0, 0 }, { 2000000, 0, 0 } }; struct dstat64 st64; r = S(SYS_utimes, "/tmp/dsc/a", tv2, 0, 0, 0, 0); S(SYS_stat64, "/tmp/dsc/a", &st64, 0, 0, 0, 0);
	  check("utimes", r == 0 && st64.mt[0] == 2000000, r); }
	check("pathconf (_PC_NAME_MAX)", S(SYS_pathconf, "/tmp", 4, 0, 0, 0, 0) >= 255, 0);
	{ int fdn = (int)S(SYS_open_extended, "/tmp/dsc/ox", O_WRONLY | O_CREAT, -1, -1, 0600, 0); check("open_extended", fdn >= 0, fdn); if (fdn >= 0) { S(SYS_fdatasync, fdn, 0, 0, 0, 0, 0); S(SYS_close, fdn, 0, 0, 0, 0, 0); } S(SYS_unlink, "/tmp/dsc/ox", 0, 0, 0, 0, 0); }
	check("mkdir_extended", S(SYS_mkdir_extended, "/tmp/dsc/mx", -1, -1, 0755, 0, 0) == 0 && S(SYS_rmdir, "/tmp/dsc/mx", 0, 0, 0, 0, 0) == 0, 0);
	{ unsigned char uu[16] = { 0 }; r = S(SYS_gethostuuid, uu, 0, 0, 0, 0, 0); check("gethostuuid", r == 0, r); }
	{ u64 guard = 0x1234; i64 g = S(SYS_guarded_open, "/tmp/dsc/gx", &guard, 1, O_WRONLY | O_CREAT, 0600, 0); check("guarded_open_np", g >= 0, g); if (g >= 0) check("guarded_close_np", S(SYS_guarded_close, g, &guard, 0, 0, 0, 0) == 0, 0); S(SYS_unlink, "/tmp/dsc/gx", 0, 0, 0, 0, 0); }
	check("clonefileat is ENOTSUP (45): programs go on with a copy", S(SYS_clonefileat, -2, "/tmp/dsc/a", -2, "/tmp/dsc/cl", 0, 0) == -45, 0);
	check("exchangedata is ENOTSUP", S(SYS_exchangedata, "/tmp/dsc/a", "/tmp/dsc/a", 0, 0, 0, 0) == -45, 0);
	check("getattrlistbulk is ENOTSUP", S(SYS_getattrlistbulk, 0, 0, 0, 0, 0, 0) == -45, 0);
	check("ptrace PT_DENY_ATTACH is accepted", S(SYS_ptrace, 31, 0, 0, 0, 0, 0) == 0, 0);
	check("kdebug_trace is accepted", S(SYS_kdebug_trace, 0, 0, 0, 0, 0, 0) == 0, 0);
	{ i64 shm = S(SYS_shm_open, "/dsc_shm", 2 | O_CREAT, 0600, 0, 0, 0); check("shm_open", shm >= 0, shm); if (shm >= 0) S(SYS_close, shm, 0, 0, 0, 0, 0); check("shm_unlink", S(SYS_shm_unlink, "/dsc_shm", 0, 0, 0, 0, 0) == 0, 0); }
	{ i64 sid = S(SYS_shmget, 0, 4096, 01000 | 0600, 0, 0, 0); check("shmget (IPC_PRIVATE)", sid >= 0, sid);
	  if (sid >= 0) { i64 at = S(SYS_shmat, sid, 0, 0, 0, 0, 0); check("  shmat", at > 0, at); if (at > 0) { *(volatile char *)at = 7; check("  shmdt", S(SYS_shmdt, at, 0, 0, 0, 0, 0) == 0, 0); } check("  shmctl IPC_RMID", S(SYS_shmctl, sid, 0, 0, 0, 0, 0) == 0, 0); } }
	{ i64 sm = S(SYS_semget, 0, 1, 01000 | 0600, 0, 0, 0); check("semget", sm >= 0, sm);
	  if (sm >= 0) { struct { unsigned short n; short op, fl; } so = { 0, 1, 0 }; check("  semctl SETVAL", S(SYS_semctl, sm, 0, 8, 3, 0, 0) == 0, 0); check("  semctl GETVAL", S(SYS_semctl, sm, 0, 5, 0, 0, 0) == 3, 0);
	    check("  semop (+1)", S(SYS_semop, sm, &so, 1, 0, 0, 0) == 0 && S(SYS_semctl, sm, 0, 5, 0, 0, 0) == 4, 0); check("  semctl IPC_RMID", S(SYS_semctl, sm, 0, 0, 0, 0, 0) == 0, 0); } }

	/* waitid and pselect */
	{ i64 ch = S(SYS_fork, 0, 0, 0, 0, 0, 0); if (ch == 0) S(SYS_exit, 9, 0, 0, 0, 0, 0);
	  { unsigned char si[104]; int k; for (k = 0; k < 104; k++) si[k] = 0xAA; r = S(SYS_waitid, 1, ch, si, 0x4, 0, 0);
	    check("waitid (P_PID, WEXITED): the siginfo of the child (SIGCHLD, its pid, the status 9)", r == 0 && *(int *)si == 20 && *(int *)(si + 12) == (int)ch && *(int *)(si + 20) == 9 && si[103] == 0, r); } }
	{ i64 p1, p0 = dsys_2(SYS_pipe, &p1); unsigned long set[16]; int k; struct dtv ts0 = { 0, 0, 0 };
	  for (k = 0; k < 16; k++) set[k] = 0; set[0] = 1UL << p0; r = S(SYS_pselect, p0 + 1, set, 0, 0, &ts0, 0); check("pselect on an empty pipe times out (0)", r == 0, r);
	  S(SYS_write, p1, "x", 1, 0, 0, 0); for (k = 0; k < 16; k++) set[k] = 0; set[0] = 1UL << p0; { unsigned msk = 0; r = S(SYS_pselect, p0 + 1, set, 0, 0, &ts0, &msk); }
	  check("pselect on a readable pipe (with a signal mask)", r == 1, r); S(SYS_close, p0, 0, 0, 0, 0, 0); S(SYS_close, p1, 0, 0, 0, 0, 0); }

	/* process information in Darwin's layouts: sysctl KERN_PROC, KERN_PROCARGS2, proc_info */
	{ static char kp[648 * 2]; int mib[4] = { 1, 14, 1, 0 }; u64 len = sizeof(kp); i64 me = S(SYS_getpid, 0, 0, 0, 0, 0, 0); mib[3] = (int)me;
	  r = S(SYS_sysctl, mib, 4, kp, &len, 0, 0);
	  check("sysctl KERN_PROC_PID: one kinfo_proc of 648 bytes with this pid, ppid, status SRUN", r == 0 && len == 648 && *(int *)(kp + 40) == (int)me && *(int *)(kp + 296 + 264) > 0 && kp[36] == 2, r);
	  check("  p_flag has P_LP64; p_comm is at 243; e_ucred.cr_uid is at 420", (*(int *)(kp + 32) & 4) && kp[243] != 0 && *(unsigned *)(kp + 420) == *(unsigned *)(kp + 296 + 16 + 80) , *(int *)(kp + 32)); }
	{ int mib[3] = { 1, 14, 0 }; u64 len = 0; r = S(SYS_sysctl, mib, 3, 0, &len, 0, 0); check("sysctl KERN_PROC_ALL (size query) gives room for at least this process", r == 0 && len >= 648, len); }
	{ static char pa[2048]; int mib[3] = { 1, 49, 0 }; u64 len = sizeof(pa); mib[2] = (int)S(SYS_getpid, 0, 0, 0, 0, 0, 0); r = S(SYS_sysctl, mib, 3, pa, &len, 0, 0);
	  check("sysctl KERN_PROCARGS2: argc and the path of the executable", r == 0 && *(int *)pa >= 1 && pa[4] == '/', r); }
	{ static char bi[136]; r = S(SYS_proc_info, 2, S(SYS_getpid, 0, 0, 0, 0, 0, 0), 3, 0, bi, 136); check("proc_info PIDINFO TBSDINFO (136 bytes: pid, ppid, comm)", r == 136 && *(int *)(bi + 12) == (int)S(SYS_getpid, 0, 0, 0, 0, 0, 0) && bi[48] != 0, r);
	  static char pp[1024]; r = S(SYS_proc_info, 2, S(SYS_getpid, 0, 0, 0, 0, 0, 0), 11, 0, pp, 1024); check("proc_info PIDPATHINFO", r > 0 && pp[0] == '/', r);
	  static int pids[2048]; r = S(SYS_proc_info, 1, 1, 0, 0, pids, sizeof(pids)); check("proc_info LISTPIDS (ALL_PIDS) gives pids", r >= 4 && pids[0] > 0, r); }

	/* kevent_qos (events of 72 bytes) on the kqueue of the process (fd -1) and on a workloop id (kevent_id) */
	{ struct { u64 ident; short filter; unsigned short flags; int qos; u64 udata; unsigned fflags, xflags; i64 data; u64 ext[4]; } q = { 7, -10, 0x21, 0, 0xfeed, 0, 0, 0, { 0, 0, 0, 0 } }, ev;
	  r = dsys8(374, -1, (i64)&q, 1, 0, 0, 0, 0, 1); check("kevent_qos (fd -1): EVFILT_USER added", r == 0, r);
	  q.flags = 0; q.fflags = 0x01000000; r = dsys8(374, -1, (i64)&q, 1, (i64)&ev, 1, 0, 0, 1); check("  triggered: the event comes back (72 bytes: ident, filter -10, udata)", r == 1 && ev.ident == 7 && ev.filter == -10 && ev.udata == 0xfeed, r);
	  q.flags = 0x21; q.ident = 8; r = dsys8(375, 0x7b5911, (i64)&q, 1, 0, 0, 0, 0, 1); check("kevent_id (a workloop id): EVFILT_USER added", r == 0, r); }

	/* AF_INET6: Darwin's family number in the address */
	{ unsigned char a6[28] = { 28, 30 }; unsigned char b6[28] = { 0 }; unsigned bl = 28; i64 s6 = S(SYS_socket, 30, 1, 0, 0, 0, 0);
	  check("socket AF_INET6 (stream)", s6 >= 0, s6);
	  r = S(SYS_bind, s6, a6, 28, 0, 0, 0); check("bind [::]:0 with family 30", r == 0, r);
	  r = S(SYS_getsockname, s6, b6, &bl, 0, 0, 0); check("  getsockname says family 30 and gives a port", r == 0 && b6[1] == 30 && bl == 28 && (b6[2] | b6[3]), b6[1]);
	  S(SYS_close, s6, 0, 0, 0, 0, 0); }

	cleanup();
	check("cleanup", S(SYS_stat64, "/tmp/dsc", buf, 0, 0, 0, 0) == -2, 0);
	out(fails ? "FAILED: " : "all passed: "); num(fails); out(" failure(s)\n");
	S(SYS_exit, fails > 100 ? 100 : fails, 0, 0, 0, 0, 0);
	return fails;
}

#if defined(__x86_64__)
__asm__(".globl _start\n_start:\n\tandq $-16, %rsp\n\tcall cmain\n");
#else
__asm__(".globl _start\n_start:\n\tmov x0, sp\n\tand sp, x0, #-16\n\tbl cmain\n");
#endif
