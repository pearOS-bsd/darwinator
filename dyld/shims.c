/*
 * The symbols of libSystem for a Mach-O program, on top of FreeBSD's libraries.
 *
 * Most functions are the same on both systems (strlen, malloc, printf, fopen, read ...): they are found by name in the
 * running libc (the leading underscore and a decoration like "$DARWIN_EXTSN" are taken off).  What differs is written
 * here: structures with another layout (stat, sigaction, timeval, pthread locks with their static initializers),
 * flags with other values (open, fcntl), sysctl names, and a few functions that exist only on the Mac.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <runetype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "darwin_abi.h"
#include "machrun.h"

/* ---- the process: what _NSGetArgv and friends return ---- */
static int g_argc; static char **g_argv; static char **g_envp; static char *g_path; static const char *g_progname;
static uintptr_t g_stack_guard;

void
shim_set_process(int argc, char **argv, char **envp, const char *progpath)
{
	g_argc = argc; g_argv = argv; g_envp = envp; g_path = strdup(progpath);
	g_progname = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	arc4random_buf(&g_stack_guard, sizeof(g_stack_guard));
	g_stack_guard &= ~(uintptr_t)0xff;			/* a zero byte, like the real guard */
}
static char ***dw_NSGetArgv(void) { return (&g_argv); }
static int *dw_NSGetArgc(void) { return (&g_argc); }
static char ***dw_NSGetEnviron(void) { return (&g_envp); }
static const char **dw_NSGetProgname(void) { return (&g_progname); }
static int dw_NSGetExecutablePath(char *buf, uint32_t *size)
{
	uint32_t need = (uint32_t)strlen(g_path) + 1;
	if (*size < need) { *size = need; return (-1); }
	memcpy(buf, g_path, need);
	return (0);
}

static void dw_stack_chk_fail(void) { fprintf(stderr, "machrun: stack smashing detected\n"); abort(); }
static void dw_stub_binder(void) { fprintf(stderr, "machrun: lazy binding was asked for (dyld_stub_binder): the lazy pointers should have been bound already\n"); abort(); }
static void dw_bzero(void *p, size_t n) { memset(p, 0, n); }
static void dw_memset_pattern16(void *b, const void *pat, size_t len)
{
	size_t i; for (i = 0; i < len; i++) ((unsigned char *)b)[i] = ((const unsigned char *)pat)[i % 16];
}

/* ---- open, fcntl ---- */
static int
flags_d2f(int d)
{
	int f = d & DARWIN_O_ACCMODE;
#define	M(dflag, fflag)	do { if (d & (dflag)) f |= (fflag); } while (0)
	M(DARWIN_O_NONBLOCK, O_NONBLOCK); M(DARWIN_O_APPEND, O_APPEND); M(DARWIN_O_SHLOCK, O_SHLOCK); M(DARWIN_O_EXLOCK, O_EXLOCK);
	M(DARWIN_O_ASYNC, O_ASYNC); M(DARWIN_O_SYNC, O_FSYNC); M(DARWIN_O_NOFOLLOW, O_NOFOLLOW); M(DARWIN_O_CREAT, O_CREAT);
	M(DARWIN_O_TRUNC, O_TRUNC); M(DARWIN_O_EXCL, O_EXCL); M(DARWIN_O_NOCTTY, O_NOCTTY); M(DARWIN_O_DIRECTORY, O_DIRECTORY);
	M(DARWIN_O_DSYNC, O_DSYNC); M(DARWIN_O_CLOEXEC, O_CLOEXEC);
#undef M
	return (f);
}
static int
dw_open(const char *path, int d, ...)
{
	int mode = 0;
	if (d & DARWIN_O_CREAT) { va_list ap; va_start(ap, d); mode = va_arg(ap, int); va_end(ap); }
	return (open(path, flags_d2f(d), mode));
}
static int
dw_fcntl(int fd, int cmd, ...)
{
	va_list ap; long arg; int r;
	va_start(ap, cmd); arg = va_arg(ap, long); va_end(ap);
	switch (cmd) {
	case 0: case 1: case 2: case 5: case 6: return (fcntl(fd, cmd, arg));	/* F_DUPFD F_GETFD F_SETFD F_GETOWN F_SETOWN */
	case 3: return (fcntl(fd, F_GETFL));
	case 4: return (fcntl(fd, F_SETFL, flags_d2f((int)arg) & ~O_ACCMODE));
	case 67: return (fcntl(fd, F_DUPFD_CLOEXEC, arg));			/* F_DUPFD_CLOEXEC */
	case 51: r = fsync(fd); return (r);					/* F_FULLFSYNC */
	case 73: case 74: return (0);						/* F_SETNOSIGPIPE, F_GETNOSIGPIPE */
	default: errno = EINVAL; return (-1);
	}
}

/* ---- stat ---- */
static void
st_out(const struct stat *sb, struct darwin_stat64 *d)
{
	memset(d, 0, sizeof(*d));
	d->st_dev = (int32_t)sb->st_dev; d->st_mode = sb->st_mode; d->st_nlink = sb->st_nlink > 0xffff ? 0xffff : (uint16_t)sb->st_nlink;
	d->st_ino = sb->st_ino; d->st_uid = sb->st_uid; d->st_gid = sb->st_gid; d->st_rdev = (int32_t)sb->st_rdev;
	d->st_atimespec.tv_sec = sb->st_atim.tv_sec; d->st_atimespec.tv_nsec = sb->st_atim.tv_nsec;
	d->st_mtimespec.tv_sec = sb->st_mtim.tv_sec; d->st_mtimespec.tv_nsec = sb->st_mtim.tv_nsec;
	d->st_ctimespec.tv_sec = sb->st_ctim.tv_sec; d->st_ctimespec.tv_nsec = sb->st_ctim.tv_nsec;
	d->st_birthtimespec.tv_sec = sb->st_birthtim.tv_sec; d->st_birthtimespec.tv_nsec = sb->st_birthtim.tv_nsec;
	d->st_size = sb->st_size; d->st_blocks = sb->st_blocks; d->st_blksize = sb->st_blksize; d->st_flags = sb->st_flags; d->st_gen = (uint32_t)sb->st_gen;
}
static int dw_stat(const char *p, struct darwin_stat64 *d) { struct stat sb; int r = stat(p, &sb); if (r == 0) st_out(&sb, d); return (r); }
static int dw_lstat(const char *p, struct darwin_stat64 *d) { struct stat sb; int r = lstat(p, &sb); if (r == 0) st_out(&sb, d); return (r); }
static int dw_fstat(int fd, struct darwin_stat64 *d) { struct stat sb; int r = fstat(fd, &sb); if (r == 0) st_out(&sb, d); return (r); }

/* fstatat: Darwin's AT_FDCWD is -2 and its AT_* flags are other bits */
static int
dw_fstatat(int fd, const char *path, struct darwin_stat64 *d, int dflag)
{
	struct stat sb; int f = 0, r;
	if (fd == -2) fd = AT_FDCWD;
	if (dflag & 0x10) f |= AT_EACCESS;
	if (dflag & 0x20) f |= AT_SYMLINK_NOFOLLOW;
	if (dflag & 0x40) f |= AT_SYMLINK_FOLLOW;
	r = fstatat(fd, path, &sb, f);
	if (r == 0) st_out(&sb, d);
	return (r);
}
static int dw_openat(int fd, const char *path, int d, ...)
{
	int mode = 0;
	if (d & DARWIN_O_CREAT) { va_list ap; va_start(ap, d); mode = va_arg(ap, int); va_end(ap); }
	return (openat(fd == -2 ? AT_FDCWD : fd, path, flags_d2f(d), mode));
}
static int dw_unlinkat(int fd, const char *path, int dflag) { return (unlinkat(fd == -2 ? AT_FDCWD : fd, path, (dflag & 0x80) ? AT_REMOVEDIR : 0)); }
static int dw_attr_setdetachstate(pthread_attr_t *a, int st) { return (pthread_attr_setdetachstate(a, st == 2 ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE)); }

/* ---- directories: Darwin's struct dirent (64 bit inodes) is not FreeBSD's ---- */
struct ddirent { uint64_t d_ino; uint64_t d_seekoff; uint16_t d_reclen; uint16_t d_namlen; uint8_t d_type; char d_name[1024]; };
static void
dent_out(const struct dirent *e, struct ddirent *d)
{
	d->d_ino = e->d_fileno; d->d_seekoff = 0; d->d_namlen = e->d_namlen; d->d_type = e->d_type;
	memcpy(d->d_name, e->d_name, e->d_namlen); d->d_name[e->d_namlen] = 0;
	d->d_reclen = (uint16_t)(offsetof(struct ddirent, d_name) + e->d_namlen + 1);
}
static __thread struct ddirent tl_dirent;
static struct ddirent *dw_readdir(DIR *dir) { struct dirent *e = readdir(dir); if (e == NULL) return (NULL); dent_out(e, &tl_dirent); return (&tl_dirent); }
static int
dw_readdir_r(DIR *dir, struct ddirent *entry, struct ddirent **result)
{
	struct dirent *e = readdir(dir);
	if (e == NULL) { *result = NULL; return (0); }
	dent_out(e, entry); *result = entry; return (0);
}

/* ---- signals: Darwin's sigset_t is 32 bits, its struct sigaction has no padding in front of the mask ---- */
struct dsigaction { void *d_handler; uint32_t d_mask; int d_flags; };
static void d2s(uint32_t m, sigset_t *s) { int i; sigemptyset(s); for (i = 1; i < 32; i++) if (m & (1u << (i - 1))) sigaddset(s, i); }
static uint32_t s2d(const sigset_t *s) { uint32_t m = 0; int i; for (i = 1; i < 32; i++) if (sigismember(s, i) == 1) m |= 1u << (i - 1); return (m); }
static int
dw_sigaction(int sig, const struct dsigaction *act, struct dsigaction *oact)
{
	struct sigaction a, o; int r;
	if (act) { memset(&a, 0, sizeof(a)); a.sa_handler = (void (*)(int))act->d_handler; d2s(act->d_mask, &a.sa_mask); a.sa_flags = act->d_flags; }
	r = sigaction(sig, act ? &a : NULL, oact ? &o : NULL);
	if (r == 0 && oact) { oact->d_handler = (void *)o.sa_handler; oact->d_mask = s2d(&o.sa_mask); oact->d_flags = o.sa_flags; }
	return (r);
}
static int
dw_pthread_sigmask(int how, const uint32_t *set, uint32_t *old)
{
	sigset_t s, o; int r;
	if (set) d2s(*set, &s);
	r = pthread_sigmask(how, set ? &s : NULL, old ? &o : NULL);
	if (r == 0 && old) *old = s2d(&o);
	return (r);
}

/* ---- pthread locks: Darwin's are 64 byte blocks that start with a signature (also the static initializers); FreeBSD's start with a pointer, NULL = static initializer ---- */
#define	D_MUTEX_SIG	0x32AAABA7u
#define	D_COND_SIG	0x3CB0B1BBu
#define	D_ONCE_SIG	0x30B1BCBAu
static void mfix(void *m) { if (*(uint32_t *)m == D_MUTEX_SIG) *(void **)m = NULL; }
static void cfix(void *c) { if (*(uint32_t *)c == D_COND_SIG) *(void **)c = NULL; }
static int dw_mutex_init(pthread_mutex_t *m, const void *attr) { *(void **)m = NULL; return (pthread_mutex_init(m, NULL)); }
static int dw_mutex_lock(pthread_mutex_t *m) { mfix(m); return (pthread_mutex_lock(m)); }
static int dw_mutex_trylock(pthread_mutex_t *m) { mfix(m); return (pthread_mutex_trylock(m)); }
static int dw_mutex_unlock(pthread_mutex_t *m) { mfix(m); return (pthread_mutex_unlock(m)); }
static int dw_mutex_destroy(pthread_mutex_t *m) { if (*(uint32_t *)m == D_MUTEX_SIG) return (0); return (pthread_mutex_destroy(m)); }
static int dw_cond_init(pthread_cond_t *c, const void *attr) { *(void **)c = NULL; return (pthread_cond_init(c, NULL)); }
static int dw_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { cfix(c); mfix(m); return (pthread_cond_wait(c, m)); }
static int dw_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t) { cfix(c); mfix(m); return (pthread_cond_timedwait(c, m, t)); }
static int dw_cond_signal(pthread_cond_t *c) { cfix(c); return (pthread_cond_signal(c)); }
static int dw_cond_broadcast(pthread_cond_t *c) { cfix(c); return (pthread_cond_broadcast(c)); }
static int dw_cond_destroy(pthread_cond_t *c) { if (*(uint32_t *)c == D_COND_SIG) return (0); return (pthread_cond_destroy(c)); }
static int dw_pthread_once(void *once, void (*fn)(void))
{
	if (*(uint32_t *)once == D_ONCE_SIG) memset(once, 0, 16);
	return (pthread_once((pthread_once_t *)once, fn));
}
static int dw_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) { return (pthread_create(t, attr, fn, arg)); }

/* ---- time, sysctl ---- */
struct dtimeval { int64_t sec; int32_t usec; int32_t pad; };
static int
dw_futimes(int fd, const struct dtimeval *tv)
{
	struct timeval t[2];
	if (tv == NULL) return (futimes(fd, NULL));
	t[0].tv_sec = tv[0].sec; t[0].tv_usec = tv[0].usec; t[1].tv_sec = tv[1].sec; t[1].tv_usec = tv[1].usec;
	return (futimes(fd, t));
}
static int
dw_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen)
{
	int n[2];
	if (namelen == 2 && name[0] == 6) {			/* CTL_HW */
		n[0] = 6;
		switch (name[1]) { case 24: n[1] = 5; break; case 25: n[1] = 3; break; default: n[1] = name[1]; }	/* HW_MEMSIZE -> HW_PHYSMEM, HW_AVAILCPU -> HW_NCPU */
		return (sysctl(n, 2, oldp, oldlenp, newp, newlen));
	}
	return (sysctl(name, namelen, oldp, oldlenp, newp, newlen));
}
static int
dw_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen)
{
	if (strcmp(name, "hw.memsize") == 0) name = "hw.physmem";
	else if (strncmp(name, "hw.logicalcpu", 13) == 0 || strncmp(name, "hw.physicalcpu", 14) == 0 || strcmp(name, "hw.activecpu") == 0) name = "hw.ncpu";
	return (sysctlbyname(name, oldp, oldlenp, newp, newlen));
}

/* ---- ctype: the macros of Darwin's <ctype.h> read __DefaultRuneLocale directly; its entries are 32 bits wide, FreeBSD's runetype entries are 64 bits ---- */
struct dw_runelocale {
	char		magic[8], encoding[32];
	void		*sgetrune, *sputrune;
	int32_t		invalid_rune;
	uint32_t	runetype[256];
	int32_t		maplower[256], mapupper[256];
	char		rest[512];			/* the extension tables: empty for the C locale */
};
static struct dw_runelocale g_runelocale;
extern const _RuneLocale _DefaultRuneLocale;
static void *
runelocale(void)
{
	int i;
	memcpy(g_runelocale.magic, "RuneMagi", 8);
	strlcpy(g_runelocale.encoding, "NONE", sizeof(g_runelocale.encoding));
	g_runelocale.invalid_rune = _DefaultRuneLocale.__invalid_rune;
	for (i = 0; i < 256; i++) {
		g_runelocale.runetype[i] = (uint32_t)_DefaultRuneLocale.__runetype[i];	/* the flag bits are the same on both systems */
		g_runelocale.maplower[i] = _DefaultRuneLocale.__maplower[i];
		g_runelocale.mapupper[i] = _DefaultRuneLocale.__mapupper[i];
	}
	return (&g_runelocale);
}

/* ---- time and checks that exist only on the Mac ---- */
#include <time.h>
static uint64_t dw_mach_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return ((uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec); }
static int dw_mach_timebase_info(uint32_t *info) { info[0] = 1; info[1] = 1; return (0); }	/* ticks are nanoseconds */
static void
dw_assert_rtn(const char *func, const char *file, int line, const char *expr)
{
	fprintf(stderr, "Assertion failed: (%s), function %s, file %s, line %d.\n", expr, func, file, line);
	abort();
}
static int dw_sprintf_chk(char *d, int flag, size_t len, const char *fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = vsnprintf(d, len, fmt, ap); va_end(ap); return (r); }
static int dw_snprintf_chk(char *d, size_t n, int flag, size_t len, const char *fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = vsnprintf(d, n < len ? n : len, fmt, ap); va_end(ap); return (r); }
static int dw_vsprintf_chk(char *d, int flag, size_t len, const char *fmt, va_list ap) { return (vsnprintf(d, len, fmt, ap)); }
static int dw_vsnprintf_chk(char *d, size_t n, int flag, size_t len, const char *fmt, va_list ap) { return (vsnprintf(d, n < len ? n : len, fmt, ap)); }
static void *dw_memcpy_chk(void *d, const void *s, size_t n, size_t dl) { if (n > dl) abort(); return (memcpy(d, s, n)); }
static void *dw_memmove_chk(void *d, const void *s, size_t n, size_t dl) { if (n > dl) abort(); return (memmove(d, s, n)); }
static void *dw_memset_chk(void *d, int c, size_t n, size_t dl) { if (n > dl) abort(); return (memset(d, c, n)); }
static char *dw_strcpy_chk(char *d, const char *s, size_t dl) { if (strlen(s) + 1 > dl) abort(); return (strcpy(d, s)); }
static char *dw_strcat_chk(char *d, const char *s, size_t dl) { if (strlen(d) + strlen(s) + 1 > dl) abort(); return (strcat(d, s)); }
static char *dw_strncpy_chk(char *d, const char *s, size_t n, size_t dl) { if (n > dl) abort(); return (strncpy(d, s, n)); }
static char *dw_strncat_chk(char *d, const char *s, size_t n, size_t dl) { if (strlen(d) + n + 1 > dl) abort(); return (strncat(d, s, n)); }
static int dw_setxattr(const char *p, const char *n, const void *v, size_t sz, uint32_t pos, int opt) { errno = ENOTSUP; return (-1); }	/* Darwin xattrs (quarantine ...): not supported */
static ssize_t dw_getxattr(const char *p, const char *n, void *v, size_t sz, uint32_t pos, int opt) { errno = ENOATTR; return (-1); }
static ssize_t dw_listxattr(const char *p, char *l, size_t sz, int opt) { return (0); }

/* CoreFoundation strings (cf.c) */
void *cf_CFStringCreateWithCString(void *, const char *, uint32_t);
void *cf_CFStringCreateWithCharacters(void *, const uint16_t *, long);
long cf_CFStringGetLength(const void *);
uint16_t cf_CFStringGetCharacterAtIndex(const void *, long);
void *cf_CFStringCreateMutableCopy(void *, long, const void *);
unsigned char cf_CFStringGetCString(const void *, char *, long, uint32_t);
void cf_CFStringNormalize(void *, int);
const void *cf_CFRetain(void *);
void cf_CFRelease(void *);

/* ---- the table of what is not simply the same name ---- */
static const struct { const char *name; void *addr; } overrides[] = {
	{ "_open", dw_open }, { "_fcntl", dw_fcntl },
	{ "_readdir$INODE64", dw_readdir }, { "_readdir", dw_readdir }, { "_readdir_r$INODE64", dw_readdir_r }, { "_readdir_r", dw_readdir_r },
	{ "_stat$INODE64", dw_stat }, { "_lstat$INODE64", dw_lstat }, { "_fstat$INODE64", dw_fstat },
	{ "_fstatat$INODE64", dw_fstatat }, { "_fstatat", dw_fstatat }, { "_openat", dw_openat }, { "_unlinkat", dw_unlinkat },
	{ "_pthread_attr_setdetachstate", dw_attr_setdetachstate },
	{ "_stat64", dw_stat }, { "_lstat64", dw_lstat }, { "_fstat64", dw_fstat },
	{ "_sigaction", dw_sigaction }, { "_pthread_sigmask", dw_pthread_sigmask },
	{ "_pthread_mutex_init", dw_mutex_init }, { "_pthread_mutex_lock", dw_mutex_lock }, { "_pthread_mutex_trylock", dw_mutex_trylock },
	{ "_pthread_mutex_unlock", dw_mutex_unlock }, { "_pthread_mutex_destroy", dw_mutex_destroy },
	{ "_pthread_cond_init", dw_cond_init }, { "_pthread_cond_wait", dw_cond_wait }, { "_pthread_cond_timedwait", dw_cond_timedwait },
	{ "_pthread_cond_signal", dw_cond_signal }, { "_pthread_cond_broadcast", dw_cond_broadcast }, { "_pthread_cond_destroy", dw_cond_destroy },
	{ "_pthread_once", dw_pthread_once }, { "_pthread_create", dw_pthread_create },
	{ "_futimes", dw_futimes }, { "_sysctl", dw_sysctl }, { "_sysctlbyname", dw_sysctlbyname },
	{ "_memset_pattern16", dw_memset_pattern16 }, { "___bzero", dw_bzero },
	{ "___stack_chk_guard", &g_stack_guard }, { "___stack_chk_fail", dw_stack_chk_fail }, { "dyld_stub_binder", dw_stub_binder },
	{ "__NSGetArgv", dw_NSGetArgv }, { "__NSGetArgc", dw_NSGetArgc }, { "__NSGetEnviron", dw_NSGetEnviron },
	{ "__NSGetProgname", dw_NSGetProgname }, { "__NSGetExecutablePath", dw_NSGetExecutablePath },
	{ "_mach_absolute_time", dw_mach_absolute_time }, { "_mach_timebase_info", dw_mach_timebase_info }, { "___assert_rtn", dw_assert_rtn },
	{ "___sprintf_chk", dw_sprintf_chk }, { "___snprintf_chk", dw_snprintf_chk }, { "___vsprintf_chk", dw_vsprintf_chk }, { "___vsnprintf_chk", dw_vsnprintf_chk },
	{ "___memcpy_chk", dw_memcpy_chk }, { "___memmove_chk", dw_memmove_chk }, { "___memset_chk", dw_memset_chk },
	{ "___strcpy_chk", dw_strcpy_chk }, { "___strcat_chk", dw_strcat_chk }, { "___strncpy_chk", dw_strncpy_chk }, { "___strncat_chk", dw_strncat_chk },
	{ "_setxattr", dw_setxattr }, { "_getxattr", dw_getxattr }, { "_listxattr", dw_listxattr },
	{ "_CFStringCreateWithCString", cf_CFStringCreateWithCString }, { "_CFStringCreateWithCharacters", cf_CFStringCreateWithCharacters },
	{ "_CFStringGetLength", cf_CFStringGetLength }, { "_CFStringGetCharacterAtIndex", cf_CFStringGetCharacterAtIndex },
	{ "_CFStringCreateMutableCopy", cf_CFStringCreateMutableCopy }, { "_CFStringGetCString", cf_CFStringGetCString },
	{ "_CFStringNormalize", cf_CFStringNormalize }, { "_CFRetain", cf_CFRetain }, { "_CFRelease", cf_CFRelease },
};

void *
shim_lookup(void *lib, const char *name)
{
	char buf[256]; size_t i, n; void *a;

	for (i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++)
		if (strcmp(overrides[i].name, name) == 0)
			return (overrides[i].addr);
	if (strcmp(name, "__DefaultRuneLocale") == 0)
		return (runelocale());
	if (name[0] != '_')
		return (NULL);
	n = strcspn(name + 1, "$");				/* "_fopen$DARWIN_EXTSN" -> "fopen" */
	if (n >= sizeof(buf)) return (NULL);
	memcpy(buf, name + 1, n); buf[n] = 0;
	if (lib == NULL) lib = RTLD_DEFAULT;
	a = dlsym(lib, buf);
	if (a == NULL && lib != RTLD_DEFAULT) a = dlsym(RTLD_DEFAULT, buf);
	return (a);
}

void *
shim_open_library(const char *installname)
{
	static const struct { const char *prefix, *so; } map[] = {
		{ "/usr/lib/libz.", "libz.so.6" }, { "/usr/lib/libbz2", "libbz2.so.4" }, { "/usr/lib/libexpat", "libexpat.so.1" },
		{ "/usr/lib/libxml2", "libxml2.so.2" }, { "/usr/lib/libc++.", "libc++.so.1" }, { "/usr/lib/libiconv", "libiconv.so.2" },
		{ "/usr/lib/liblzma", "liblzma.so.5" }, { "/usr/lib/libcurses", "libncurses.so.9" }, { "/usr/lib/libncurses", "libncurses.so.9" },
	};
	size_t i; void *h;

	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (strncmp(installname, map[i].prefix, strlen(map[i].prefix)) == 0) {
			h = dlopen(map[i].so, RTLD_NOW | RTLD_GLOBAL);
			if (h != NULL) return (h);
			VLOG("%s: dlopen(%s) failed: %s\n", installname, map[i].so, dlerror());
			return (RTLD_DEFAULT);
		}
	return (RTLD_DEFAULT);		/* libSystem, libobjc, the frameworks: whatever the process has */
}
