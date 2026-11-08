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
	__asm__ volatile("push %7; push $0; syscall; setc %1; add $16, %%rsp" : "+a"(ret), "=q"(err), "+d"(c), "+D"(a), "+S"(b), "+r"(r10), "+r"(r8), "+r"(r9)
	    : "r"(g) : "rcx", "r11", "memory", "cc");
	return err ? -ret : ret;
#else
	register i64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f, x6 __asm__("x6") = g, x16 __asm__("x16") = n;
	unsigned long err;
	__asm__ volatile("svc #0\n\tcset %0, cs" : "=r"(err), "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6) : "r"(x16) : "memory", "cc");
	return err ? -x0 : x0;
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
	SYS_write_nocancel = 397, SYS_mknod = 14, SYS_csops = 169, SYS_getxattr = 234, SYS_setxattr = 236, SYS_removexattr = 238, SYS_listxattr = 240, SYS_shared_region_check_np = 294,
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
static void on_usr1_info(int sig, void *info, void *uc) { got_sig = sig; got_signo_from_info = *(int *)info; (void)uc; }
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
	  check("fstatfs64", r == 0 && sf.blocks > 0, r); S(SYS_close, fd, 0, 0, 0, 0, 0); }

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
