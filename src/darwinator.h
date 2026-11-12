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
struct proc;
void darwin_kq_exit(struct proc *p);
void darwin_wq_exit(struct proc *p);
int darwin_workq_open(struct thread *, struct darwin_args *);
int darwin_workq_kernreturn(struct thread *, struct darwin_args *);
int darwin_pth_info(struct proc *p, uint64_t *wqstart, uint64_t *pthsize);
int darwin_wq_kq_ids(struct proc *p, int64_t *ids, int max);
int darwin_wq_kevent(struct thread *td, int64_t kqid, uint64_t in, int nin, uint64_t out, int nout, uint64_t dout, uint64_t *davail, int *count);
void darwin_wq_note(struct thread *td);
uint32_t darwin_thread_kport(struct thread *td);
int darwin_work_interval_ctl(struct thread *, struct darwin_args *);
void darwin_wq_request(struct thread *td, int64_t id, uint64_t udata, uint32_t qos, bool add);
void darwin_pth_exit(struct proc *p);
struct thread;
void *darwin_mach_port_for_event(struct thread *td, uint32_t name);
int darwin_mach_event_receive(struct thread *td, uint32_t name, uint32_t option, uint64_t buf, uint32_t bufsize, uint32_t *msgsize);
extern int darwin_stop;
extern int darwin_dyld;			/* kern.darwinator.dyld: run a dynamic Mach-O with the dyld that the file names */

/* what the stack of a program that starts in dyld has: the mach_header of the program, then argc, argv, envp, and the "apple" strings (offsets in the string area) */
struct darwin_auxargs {
	uint64_t	mh;
	int		n;
	uint32_t	off[8];
};

/* The Darwin system call number space: the top byte of the number selects a class. */
#define	DARWIN_CLASS_MACH	1	/* Mach traps */
#define	DARWIN_CLASS_UNIX	2	/* BSD system calls */
#define	DARWIN_CLASS_MACHDEP	3
#define	DARWIN_CLASS_DIAG	4

int	darwin_oflags(int);				/* Darwin open flags -> FreeBSD's */
int	darwin_errno(int);				/* FreeBSD errno -> Darwin errno */
void	darwin_sigframe_init(struct sysentvec *);	/* per architecture: the signal frame in Darwin's layout (amd64) */
void	darwin_machdep_syscalls_init(void);		/* per architecture */
void	darwin_commpage_init(void);			/* per architecture: the "comm page" that Darwin maps in every process (amd64) */
int	darwin_commpage_map(struct vmspace *);	/* maps it into a new process */
void	darwin_stack_init(struct sysentvec *);	/* where the stack of a Darwin process is */
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
void	darwin_sem_syscalls_init(void);		/* syscalls_sem.c */
int	darwin_semwait_signal(struct thread *, struct darwin_args *);	/* mach_ipc.c */
int	darwin_fileport_makeport(struct thread *, struct darwin_args *);
int	darwin_fileport_makefd(struct thread *, struct darwin_args *);
void	darwin_misc_syscalls_init(void);		/* syscalls_misc.c */
void	darwin_dyld_syscalls_init(void);		/* syscalls_dyld.c */
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
