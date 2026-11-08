/* Darwin system calls from freestanding C: no libSystem, no dyld.  dsys() returns the result, or -errno (Darwin's numbers). */
#ifndef DSYS_H
#define DSYS_H

typedef unsigned long u64;
typedef long i64;

#if defined(__x86_64__)
static inline i64 dsys(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
	register i64 r10 __asm__("r10") = d;
	register i64 r8 __asm__("r8") = e;
	register i64 r9 __asm__("r9") = f;
	i64 ret = 0x2000000 | n;
	unsigned char err;
	__asm__ volatile("syscall; setc %1" : "+a"(ret), "=q"(err), "+d"(c), "+D"(a), "+S"(b), "+r"(r10), "+r"(r8), "+r"(r9)
	    : : "rcx", "r11", "memory", "cc");
	return err ? -ret : ret;
}
#elif defined(__aarch64__)
static inline i64 dsys(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
	register i64 x0 __asm__("x0") = a;
	register i64 x1 __asm__("x1") = b;
	register i64 x2 __asm__("x2") = c;
	register i64 x3 __asm__("x3") = d;
	register i64 x4 __asm__("x4") = e;
	register i64 x5 __asm__("x5") = f;
	register i64 x16 __asm__("x16") = n;
	unsigned long err;
	__asm__ volatile("svc #0x80\n\tcset %0, cs" : "=r"(err), "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5)
	    : "r"(x16) : "memory", "cc");
	return err ? -x0 : x0;
}
#else
#error "x86-64 or arm64"
#endif

#define	SYS_exit	1
#define	SYS_read	3
#define	SYS_write	4
#define	SYS_open	5
#define	SYS_close	6
#define	SYS_unlink	10
#define	SYS_getpid	20
#define	SYS_getuid	24
#define	SYS_getppid	39
#define	SYS_munmap	73
#define	SYS_gettimeofday 116
#define	SYS_mmap	197
#define	SYS_stat64	338
#define	SYS_getentropy	500

#define	O_RDONLY	0
#define	O_WRONLY	1
#define	O_CREAT		0x200
#define	O_TRUNC		0x400
#define	O_CLOEXEC	0x1000000
#define	PROT_RW		3
#define	MAP_PRIVATE_ANON 0x1002

struct dstat64 {
	int st_dev; unsigned short st_mode, st_nlink; u64 st_ino; unsigned st_uid, st_gid; int st_rdev;
	i64 at[2], mt[2], ct[2], bt[2]; i64 st_size, st_blocks; int st_blksize; unsigned st_flags, st_gen; int st_lspare; i64 st_qspare[2];
};
struct dtimeval { i64 sec; int usec; int pad; };

#endif
