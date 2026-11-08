/*
 * machsh: a small interactive shell written as a Mach-O program for darwinator: it talks to the kernel only with the
 * Darwin system call convention (no libSystem).  It exercises what the module implements: read/write on the terminal,
 * open/stat64/mmap/getentropy/gettimeofday and the thread pointer.   Commands: help calc pid ppid uid time rand cat
 * stat write rm mem tls exit.   Also scriptable: it reads standard input line by line, a prompt is shown only on a tty.
 */
#include "dsys.h"

__attribute__((used)) void *memcpy(void *d, const void *s, u64 n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
__attribute__((used)) void *memset(void *d, int c, u64 n) { char *a = d; while (n--) *a++ = (char)c; return d; }

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { dsys(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0, 0); }
static int starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

static void num(i64 v)
{
	char b[24]; int i = 23; int neg = v < 0; u64 u = neg ? -(u64)v : (u64)v;
	b[i] = 0;
	do { b[--i] = '0' + u % 10; u /= 10; } while (u);
	if (neg) b[--i] = '-';
	out(b + i);
}
static void hex(u64 v, int digits)
{
	char b[20]; int i;
	for (i = 0; i < digits; i++) b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15];
	b[digits] = 0; out(b);
}
static void oct(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + (v & 7); v >>= 3; } while (v); out(b + i); }

/* ---- an expression evaluator: + - * / % and parentheses, 64 bit integers ---- */
static const char *ep; static int eerr;
static i64 expr(void);
static void skip(void) { while (*ep == ' ') ep++; }
static i64 atom(void)
{
	i64 v = 0; skip();
	if (*ep == '(') { ep++; v = expr(); skip(); if (*ep == ')') ep++; else eerr = 1; return v; }
	if (*ep == '-') { ep++; return -atom(); }
	if (*ep < '0' || *ep > '9') { eerr = 1; return 0; }
	while (*ep >= '0' && *ep <= '9') v = v * 10 + (*ep++ - '0');
	return v;
}
static i64 term(void)
{
	i64 v = atom();
	for (;;) {
		skip();
		if (*ep == '*') { ep++; v *= atom(); }
		else if (*ep == '/' || *ep == '%') {
			char op = *ep++; i64 r = atom();
			if (r == 0) { eerr = 2; return 0; }
			v = op == '/' ? v / r : v % r;
		} else return v;
	}
}
static i64 expr(void)
{
	i64 v = term();
	for (;;) {
		skip();
		if (*ep == '+') { ep++; v += term(); }
		else if (*ep == '-') { ep++; v -= term(); }
		else return v;
	}
}

/* ---- input: whole lines, also from a pipe ---- */
static char ibuf[1024]; static int ipos, ilen;
static int getline_(char *line, int max)
{
	int n = 0;
	for (;;) {
		if (ipos == ilen) {
			i64 r = dsys(SYS_read, 0, (i64)ibuf, sizeof ibuf, 0, 0, 0);
			if (r <= 0) { line[n] = 0; return n ? 1 : 0; }
			ipos = 0; ilen = (int)r;
		}
		char c = ibuf[ipos++];
		if (c == '\n') { line[n] = 0; return 1; }
		if (n < max - 1) line[n++] = c;
	}
}

static void err(i64 r) { out("error "); num(-r); out("\n"); }

static void do_cat(const char *path)
{
	char b[512]; i64 fd = dsys(SYS_open, (i64)path, O_RDONLY, 0, 0, 0, 0), r;
	if (fd < 0) { err(fd); return; }
	while ((r = dsys(SYS_read, fd, (i64)b, sizeof b, 0, 0, 0)) > 0) dsys(SYS_write, 1, (i64)b, r, 0, 0, 0);
	dsys(SYS_close, fd, 0, 0, 0, 0, 0);
}
static void do_stat(const char *path)
{
	struct dstat64 st; i64 r = dsys(SYS_stat64, (i64)path, (i64)&st, 0, 0, 0, 0);
	if (r < 0) { err(r); return; }
	out("size "); num(st.st_size); out("  mode 0"); oct(st.st_mode & 07777);
	out((st.st_mode & 0170000) == 0040000 ? " (directory)" : (st.st_mode & 0170000) == 0100000 ? " (file)" : " (other)");
	out("  nlink "); num(st.st_nlink); out("  uid "); num(st.st_uid); out("  inode "); num((i64)st.st_ino); out("\n");
}
static void do_write(const char *args)
{
	char path[256]; int n = 0; while (*args && *args != ' ' && n < 255) path[n++] = *args++;
	path[n] = 0; while (*args == ' ') args++;
	i64 fd = dsys(SYS_open, (i64)path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644, 0, 0, 0);
	if (fd < 0) { err(fd); return; }
	dsys(SYS_write, fd, (i64)args, (i64)slen(args), 0, 0, 0); dsys(SYS_write, fd, (i64)"\n", 1, 0, 0, 0);
	dsys(SYS_close, fd, 0, 0, 0, 0, 0);
	out("wrote "); num((i64)slen(args) + 1); out(" bytes to "); out(path); out("\n");
}
static void do_mem(i64 pages)
{
	if (pages < 1 || pages > 4096) { out("mem PAGES (1..4096)\n"); return; }
	i64 len = pages * 4096, a = dsys(SYS_mmap, 0, len, PROT_RW, MAP_PRIVATE_ANON, -1, 0);
	if (a < 0 && a > -4096) { err(a); return; }
	unsigned char *p = (unsigned char *)a; u64 sum = 0, want = 0;
	for (i64 i = 0; i < pages; i++) { p[i * 4096] = (unsigned char)(i * 7 + 1); want += (unsigned char)(i * 7 + 1); }
	for (i64 i = 0; i < pages; i++) sum += p[i * 4096];
	out("mapped "); num(pages); out(" pages at 0x"); hex((u64)a, 12); out(sum == want ? ", contents ok\n" : ", CONTENTS WRONG\n");
	dsys(SYS_munmap, a, len, 0, 0, 0, 0);
}
static void do_time(void)
{
	struct dtimeval tv; dsys(SYS_gettimeofday, (i64)&tv, 0, 0, 0, 0, 0);
	i64 s = tv.sec % 86400;
	num(tv.sec); out(" s since 1970, "); num(s / 3600 / 10); num(s / 3600 % 10); out(":"); num(s / 60 % 60 / 10); num(s / 60 % 10);
	out(":"); num(s % 60 / 10); num(s % 10); out(" UTC\n");
}
static void do_rand(void)
{
	unsigned char b[8]; i64 r = dsys(SYS_getentropy, (i64)b, 8, 0, 0, 0, 0);
	if (r < 0) { err(r); return; }
	out("0x"); for (int i = 0; i < 8; i++) hex(b[i], 2); out("\n");
}
static void do_tls(void)
{
#if defined(__x86_64__)
	static u64 block[4] = { 0x1122334455667788UL, 0, 0, 0 };
	u64 got;
	i64 r = dsys(0x1000000 | 3, (i64)block, 0, 0, 0, 0, 0);	/* machdep class (3), number 3: thread_fast_set_cthread_self; the class bits are the top byte */
	(void)r;
	__asm__ volatile("movq %%gs:0, %0" : "=r"(got));
	out(got == block[0] ? "tls ok (gs:0 reads the block)\n" : "tls WRONG\n");
#else
	out("tls: not implemented on this architecture yet\n");
#endif
}

static void run(char *line)
{
	if (!*line) return;
	if (starts(line, "help")) out("help calc EXPR pid ppid uid time rand cat F stat F write F TEXT rm F mem PAGES tls exit [N]\n");
	else if (starts(line, "calc ")) {
		ep = line + 5; eerr = 0; i64 v = expr(); skip();
		if (eerr == 2) out("division by zero\n"); else if (eerr || *ep) out("syntax error\n"); else { out("= "); num(v); out("\n"); }
	}
	else if (starts(line, "pid")) { out("pid "); num(dsys(SYS_getpid, 0, 0, 0, 0, 0, 0)); out("\n"); }
	else if (starts(line, "ppid")) { out("ppid "); num(dsys(SYS_getppid, 0, 0, 0, 0, 0, 0)); out("\n"); }
	else if (starts(line, "uid")) { out("uid "); num(dsys(SYS_getuid, 0, 0, 0, 0, 0, 0)); out("\n"); }
	else if (starts(line, "time")) do_time();
	else if (starts(line, "rand")) do_rand();
	else if (starts(line, "cat ")) do_cat(line + 4);
	else if (starts(line, "stat ")) do_stat(line + 5);
	else if (starts(line, "write ")) do_write(line + 6);
	else if (starts(line, "rm ")) { i64 r = dsys(SYS_unlink, (i64)(line + 3), 0, 0, 0, 0, 0); if (r < 0) err(r); else out("removed\n"); }
	else if (starts(line, "mem ")) { i64 n = 0; const char *s = line + 4; while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0'); do_mem(n); }
	else if (starts(line, "tls")) do_tls();
	else if (starts(line, "exit")) { i64 n = 0; const char *s = line + 4; while (*s == ' ') s++; while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0'); dsys(SYS_exit, n, 0, 0, 0, 0, 0); }
	else { out("unknown command: "); out(line); out("  (try help)\n"); }
}

__attribute__((used)) void cmain(void)
{
	char line[256];
	out("machsh: a Mach-O program on FreeBSD, Darwin system calls only.  pid "); num(dsys(SYS_getpid, 0, 0, 0, 0, 0, 0));
	out(", uid "); num(dsys(SYS_getuid, 0, 0, 0, 0, 0, 0)); out("\ntype help\n");
	for (;;) {
		out("mach> ");
		if (!getline_(line, sizeof line)) break;
		run(line);
	}
	out("\n");
	dsys(SYS_exit, 0, 0, 0, 0, 0, 0);
}

#if defined(__x86_64__)
__asm__(".globl _start\n_start:\n\tandq $-16, %rsp\n\tcall cmain\n");
#else
__asm__(".globl _start\n_start:\n\tmov x0, sp\n\tand sp, x0, #-16\n\tbl cmain\n");
#endif
