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

/* fcntl(2) commands (the locking ones and F_DUPFD_CLOEXEC are numbered differently on FreeBSD) */
#define	DARWIN_F_DUPFD		0
#define	DARWIN_F_GETFD		1
#define	DARWIN_F_SETFD		2
#define	DARWIN_F_GETFL		3
#define	DARWIN_F_SETFL		4
#define	DARWIN_F_GETOWN		5
#define	DARWIN_F_SETOWN		6
#define	DARWIN_F_GETLK		7
#define	DARWIN_F_SETLK		8
#define	DARWIN_F_SETLKW		9
#define	DARWIN_F_RDAHEAD	45
#define	DARWIN_F_NOCACHE	48
#define	DARWIN_F_GETPATH	50
#define	DARWIN_F_FULLFSYNC	51
#define	DARWIN_F_GLOBAL_NOCACHE	55
#define	DARWIN_F_NODIRECT	62
#define	DARWIN_F_DUPFD_CLOEXEC	67

/* struct flock of Darwin: no l_sysid */
struct darwin_flock {
	int64_t		l_start;
	int64_t		l_len;
	int32_t		l_pid;
	int16_t		l_type;
	int16_t		l_whence;
};

/* struct termios of Darwin: the flags and the speeds are 64 bits (72 bytes); the requests that carry it */
struct darwin_termios {
	uint64_t	c_iflag, c_oflag, c_cflag, c_lflag;
	uint8_t		c_cc[20];
	uint64_t	c_ispeed, c_ospeed;
};
#define	DARWIN_TIOCGETA		0x40487413UL
#define	DARWIN_TIOCSETA		0x80487414UL
#define	DARWIN_TIOCSETAW	0x80487415UL
#define	DARWIN_TIOCSETAF	0x80487416UL

/* the head of a directory entry of getdirentries64 (the name follows at offset 21; reclen is rounded up to 4) */
struct darwin_dirent_head {
	uint64_t	d_ino;
	uint64_t	d_seekoff;
	uint16_t	d_reclen;
	uint16_t	d_namlen;
	uint8_t		d_type;
} __attribute__((packed));
#define	DARWIN_DIRENT_NAME	21

/* struct statfs of statfs64/fstatfs64 (2168 bytes) */
struct darwin_statfs64 {
	uint32_t	f_bsize;
	int32_t		f_iosize;
	uint64_t	f_blocks, f_bfree, f_bavail, f_files, f_ffree;
	int32_t		f_fsid[2];
	uint32_t	f_owner, f_type, f_flags, f_fssubtype;
	char		f_fstypename[16];
	char		f_mntonname[1024];
	char		f_mntfromname[1024];
	uint32_t	f_reserved[8];
};

#define	DARWIN_MS_ASYNC		0x0001
#define	DARWIN_MS_INVALIDATE	0x0002
#define	DARWIN_AF_INET6		30
#define	DARWIN_MSG_NOSIGNAL	0x80000
#define	DARWIN_SO_NOSIGPIPE	0x1022

#endif
