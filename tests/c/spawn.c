/*
 * spawn: an application that asks for posix_spawn (Darwin syscall 244) the way libSystem does it: the _posix_spawn_args_desc, the _posix_spawnattr and the
 * _posix_spawn_file_actions are built by hand in the layouts of xnu (bsd/sys/spawn_internal.h).  Freestanding Mach-O, no libSystem.
 * Prints "ok NAME" or "FAIL NAME ..."; the exit status is the number of failures.
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
static int fails;
static void check(const char *name, int ok, i64 got)
{
	if (ok) { out("ok "); out(name); out("\n"); return; }
	fails++; out("FAIL "); out(name); out(" ("); num(got); out(")\n");
}

/* ---- xnu's structures ---- */
#define	PSFA_SIZE 1040
struct desc {				/* _posix_spawn_args_desc: pairs of (size, pointer) */
	u64 attr_size; void *attr;
	u64 fa_size; void *fa;
	u64 rest[14];
};
struct attr { short flags, pad; unsigned sigdefault, sigmask; int pgroup; char rest[200]; };
struct fa { int alloc, count; char acts[8 * PSFA_SIZE]; };

static void act(struct fa *f, int type, int fd, int a, int b, const char *path)
{
	char *p = f->acts + (u64)f->count * PSFA_SIZE;
	memset(p, 0, PSFA_SIZE);
	*(int *)p = type; *(int *)(p + 4) = fd;
	if (type == 0) { *(int *)(p + 8) = a; *(unsigned short *)(p + 12) = (unsigned short)b; for (int i = 0; path[i]; i++) p[14 + i] = path[i]; }
	else if (type == 2) *(int *)(p + 8) = a;
	else if (type == 5) for (int i = 0; path[i]; i++) p[8 + i] = path[i];
	f->count++; f->alloc = 8;
}

static i64 spawn(int *pid, const char *path, struct attr *at, struct fa *f, char **argv, char **envp)
{
	struct desc d; memset(&d, 0, sizeof d);
	if (at) { d.attr_size = sizeof *at; d.attr = at; }
	if (f) { d.fa_size = 8 + (u64)f->count * PSFA_SIZE; d.fa = f; }
	return dsys(244, (i64)pid, (i64)path, (i64)&d, (i64)argv, (i64)envp, 0);
}
static i64 reap(int pid, int *st) { return dsys(7, pid, (i64)st, 0, 0, 0, 0); }	/* wait4(pid, &st, 0, 0) */
static i64 slurp(const char *path, char *buf, int n)
{
	i64 fd = dsys(5, (i64)path, 0, 0, 0, 0, 0), r;
	if (fd < 0) return fd;
	r = dsys(SYS_read, fd, (i64)buf, n - 1, 0, 0, 0);
	dsys(6, fd, 0, 0, 0, 0, 0);
	if (r >= 0) buf[r] = 0;
	return r;
}

#define	O_WRONLY 1
#define	O_CREAT 0x200
#define	O_TRUNC 0x400
#define	OUT "/tmp/darwinator-spawn.out"

__attribute__((used)) int cmain(void)
{
	static char buf[512];
	char *env0[] = { 0 };
	int pid = 0, st = 0;
	i64 r;
	struct fa f;

	/* 1. echo with stdout redirected to a file by an open action */
	memset(&f, 0, sizeof f);
	act(&f, 0, 1, O_WRONLY | O_CREAT | O_TRUNC, 0644, OUT);
	{ char *av[] = { "echo", "hello", "spawn", 0 };
	  r = spawn(&pid, "/bin/echo", 0, &f, av, env0);
	  check("spawn returns 0", r == 0, r);
	  check("spawn gives a pid", pid > 1, pid);
	  r = reap(pid, &st);
	  check("wait4 of the child", r == pid, r);
	  check("child exited with 0", st == 0, st);
	  r = slurp(OUT, buf, sizeof buf);
	  check("open action redirected stdout", r == 12 && seq(buf, "hello spawn\n"), r); }

	/* 2. exit status through sh -c */
	{ char *av[] = { "sh", "-c", "exit 7", 0 };
	  r = spawn(&pid, "/bin/sh", 0, 0, av, env0);
	  check("spawn sh", r == 0, r);
	  reap(pid, &st);
	  check("exit status 7", st == (7 << 8), st); }

	/* 3. a program that does not exist is the error of posix_spawn */
	{ char *av[] = { "nothere", 0 }; pid = -5;
	  r = spawn(&pid, "/no/such/program", 0, 0, av, env0);
	  check("missing program gives ENOENT", r == -2 || r == 2, r);
	  check("pid left alone on error", pid == -5, pid); }

	/* 4. the environment */
	memset(&f, 0, sizeof f);
	act(&f, 0, 1, O_WRONLY | O_CREAT | O_TRUNC, 0644, OUT);
	{ char *av[] = { "env", 0 }; char *ev[] = { "FOO=bar", "SPAWN=1", 0 };
	  r = spawn(&pid, "/usr/bin/env", 0, &f, av, ev);
	  reap(pid, &st);
	  slurp(OUT, buf, sizeof buf);
	  check("environment passed", r == 0 && seq(buf, "FOO=bar\nSPAWN=1\n"), r); }

	/* 5. chdir + dup2 of an opened descriptor onto stdout */
	memset(&f, 0, sizeof f);
	act(&f, 0, 9, O_WRONLY | O_CREAT | O_TRUNC, 0644, OUT);
	act(&f, 2, 9, 1, 0, 0);
	act(&f, 1, 9, 0, 0, 0);
	act(&f, 5, 0, 0, 0, "/usr");
	{ char *av[] = { "pwd", 0 };
	  r = spawn(&pid, "/bin/pwd", 0, &f, av, env0);
	  reap(pid, &st);
	  slurp(OUT, buf, sizeof buf);
	  check("chdir and dup2 actions", r == 0 && seq(buf, "/usr\n"), r); }

	/* 6. attributes: a new process group (SETPGROUP, pgroup 0) and a signal mask */
	{ struct attr a; memset(&a, 0, sizeof a);
	  a.flags = 0x2 | 0x8; a.pgroup = 0; a.sigmask = 1u << 1;	/* SIGINT blocked */
	  memset(&f, 0, sizeof f);
	  act(&f, 0, 1, O_WRONLY | O_CREAT | O_TRUNC, 0644, OUT);
	  char *av[] = { "sh", "-c", "echo $$ $(ps -o pgid= -p $$ | tr -d ' ')", 0 };
	  r = spawn(&pid, "/bin/sh", &a, &f, av, env0);
	  reap(pid, &st);
	  slurp(OUT, buf, sizeof buf);
	  /* the child's pid and its pgid are the same number */
	  int i = 0, p1 = 0, p2 = 0;
	  while (buf[i] >= '0' && buf[i] <= '9') p1 = p1 * 10 + buf[i++] - '0';
	  i++;
	  while (buf[i] >= '0' && buf[i] <= '9') p2 = p2 * 10 + buf[i++] - '0';
	  check("SETPGROUP puts the child in its own group", r == 0 && p1 == pid && p1 == p2, p2); }

	/* 7. several children at the same time */
	{ int pids[4]; char *av[] = { "sh", "-c", "exit 3", 0 }; int ok = 1;
	  for (int i = 0; i < 4; i++) if (spawn(&pids[i], "/bin/sh", 0, 0, av, env0) != 0) ok = 0;
	  for (int i = 0; i < 4; i++) { reap(pids[i], &st); if (st != (3 << 8)) ok = 0; }
	  check("four children", ok, st); }

	dsys(10, (i64)OUT, 0, 0, 0, 0, 0);	/* unlink */
	out(fails ? "spawn: failures\n" : "spawn: all ok\n");
	dsys(SYS_exit, fails, 0, 0, 0, 0, 0);
	return fails;
}

#if defined(__x86_64__)
__asm__(".globl _start\n_start:\n\tandq $-16, %rsp\n\tcall cmain\n");
#else
__asm__(".globl _start\n_start:\n\tmov x0, sp\n\tand sp, x0, #-16\n\tbl cmain\n");
#endif
