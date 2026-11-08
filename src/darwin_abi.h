/* Darwin values that differ from FreeBSD's (open flags, errno numbers, struct stat64): written from the public headers of the platform. */
#ifndef _DARWIN_ABI_H_
#define _DARWIN_ABI_H_

#include <sys/types.h>

/* open(2) flags */
#define	DARWIN_O_ACCMODE	0x0003
#define	DARWIN_O_NONBLOCK	0x0004
#define	DARWIN_O_APPEND		0x0008
#define	DARWIN_O_SHLOCK		0x0010
#define	DARWIN_O_EXLOCK		0x0020
#define	DARWIN_O_ASYNC		0x0040
#define	DARWIN_O_SYNC		0x0080
#define	DARWIN_O_NOFOLLOW	0x0100
#define	DARWIN_O_CREAT		0x0200
#define	DARWIN_O_TRUNC		0x0400
#define	DARWIN_O_EXCL		0x0800
#define	DARWIN_O_NOCTTY		0x20000
#define	DARWIN_O_DIRECTORY	0x100000
#define	DARWIN_O_DSYNC		0x400000
#define	DARWIN_O_CLOEXEC	0x1000000

/* mmap(2) flags that mean the same as on FreeBSD (the others are dropped: MAP_JIT 0x800 is MAP_NOSYNC there) */
#define	DARWIN_MAP_SHARED	0x0001
#define	DARWIN_MAP_PRIVATE	0x0002
#define	DARWIN_MAP_FIXED	0x0010
/* MAP_NORESERVE (0x40) has no meaning on FreeBSD: dropped */
#define	DARWIN_MAP_HASSEMAPHORE	0x0200
#define	DARWIN_MAP_ANON		0x1000

/* errno numbers of Darwin that are not FreeBSD's (the numbers up to 79 are the same) */
#define	DARWIN_EAUTH		80
#define	DARWIN_ENEEDAUTH	81
#define	DARWIN_EOVERFLOW	84
#define	DARWIN_ECANCELED	89
#define	DARWIN_EIDRM		90
#define	DARWIN_ENOMSG		91
#define	DARWIN_EILSEQ		92
#define	DARWIN_ENOATTR		93
#define	DARWIN_EBADMSG		94
#define	DARWIN_EMULTIHOP	95
#define	DARWIN_ENOLINK		97
#define	DARWIN_EPROTO		100
#define	DARWIN_ENOTRECOVERABLE	104
#define	DARWIN_EOWNERDEAD	105

/* struct stat64 of Darwin (144 bytes, the same on x86-64 and arm64) */
struct darwin_stat64 {
	int32_t		st_dev;
	uint16_t	st_mode;
	uint16_t	st_nlink;
	uint64_t	st_ino;
	uint32_t	st_uid;
	uint32_t	st_gid;
	int32_t		st_rdev;
	struct { int64_t tv_sec; int64_t tv_nsec; } st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
	int64_t		st_size;
	int64_t		st_blocks;
	int32_t		st_blksize;
	uint32_t	st_flags;
	uint32_t	st_gen;
	int32_t		st_lspare;
	int64_t		st_qspare[2];
};

#endif
