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
extern int darwin_debug;

/* The Darwin system call number space: the top byte of the number selects a class. */
#define	DARWIN_CLASS_MACH	1	/* Mach traps */
#define	DARWIN_CLASS_UNIX	2	/* BSD system calls */
#define	DARWIN_CLASS_MACHDEP	3
#define	DARWIN_CLASS_DIAG	4

void	darwin_syscalls_init(void);			/* fills the unused entries with nosys */
int	darwin_fetch_syscall_args(struct thread *);	/* per architecture (darwin_<arch>.c) */
int	macho_imgact(struct image_params *);		/* the image activator */

#define	DPRINTF(...)	do { if (darwin_debug) printf("darwinator: " __VA_ARGS__); } while (0)

#endif
