/* darwinator: shared declarations. */
#ifndef _DARWINATOR_H_
#define _DARWINATOR_H_

#include <sys/param.h>
#include <sys/malloc.h>

MALLOC_DECLARE(M_DARWIN);

struct thread;
struct image_params;
struct sysentvec;
struct sysent;

extern struct sysentvec darwin_sysvec;
extern struct sysent darwin_sysent[];
extern const char *darwin_syscallnames[];
extern const int darwin_nsyscalls;
/* the argument registers of a call, as td->td_sa.args holds them: what every function of the table receives */
struct darwin_args {
	register_t a[8];
};

#define	DARWIN_NSYSCALLS	1216		/* BSD class: numbers below 1024 (xnu has them up to about 560); machdep class: 1024 + number (below 64); Mach traps: 1088 + number (below 128) */
#define	DARWIN_MACHDEP_BASE	1024
#define	DARWIN_MACH_BASE	1088
extern int darwin_debug;

/* The Darwin system call number space: the top byte of the number selects a class. */
#define	DARWIN_CLASS_MACH	1	/* Mach traps */
#define	DARWIN_CLASS_UNIX	2	/* BSD system calls */
#define	DARWIN_CLASS_MACHDEP	3
#define	DARWIN_CLASS_DIAG	4

int	darwin_oflags(int);				/* Darwin open flags -> FreeBSD's */
int	darwin_errno(int);				/* FreeBSD errno -> Darwin errno */
void	darwin_sigframe_init(struct sysentvec *);	/* per architecture: the signal frame in Darwin's layout (amd64) */
void	darwin_machdep_syscalls_init(void);		/* per architecture */
dev_t	darwin_to_dev(int32_t);				/* Darwin dev_t (major << 24 | minor) <-> FreeBSD's */
int32_t	darwin_from_dev(dev_t);
struct statfs;
struct stat;
int	darwin_statfs_out(const struct statfs *, void *);
int	darwin_sigprocmask(struct thread *, struct darwin_args *);
int	darwin_stat_out(struct thread *, const struct stat *, void *);
int	darwin_msgflags_pub(int);
void	darwin_event_syscalls_init(void);		/* syscalls_event.c */
void	darwin_ipc_syscalls_init(void);		/* mach_ipc.c */
void	darwin_mach_init(void);
void	darwin_mach_fini(void);
void	darwin_spawn_syscalls_init(void);		/* syscalls_spawn.c */
void	darwin_proc_syscalls_init(void);		/* syscalls_proc.c */
int	darwin_sysctl_proc(struct thread *, int *, u_int, void *, void *, bool);
int	darwin_sysctl_procargs2(struct thread *, int, void *, void *);
void	darwin_compat_syscalls_init(void);		/* syscalls_compat.c */
void	darwin_sync_syscalls_init(void);		/* syscalls_sync.c: psynch, workq, the Mach traps */
void	darwin_posix_syscalls_init(void);		/* syscalls_posix.c */
void	darwin_more_syscalls_init(void);		/* syscalls_more.c */
void	darwin_syscalls_init(void);			/* fills the unused entries with nosys */
int	darwin_fetch_syscall_args(struct thread *);	/* per architecture (darwin_<arch>.c) */
int	macho_imgact(struct image_params *);		/* the image activator */

#define	DPRINTF(...)	do { if (darwin_debug) printf("darwinator: " __VA_ARGS__); } while (0)

#endif
