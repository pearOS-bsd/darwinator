/* IOSurface: the user client of IOSurfaceRoot (see iosurface.c).  The Mach side (mach_ipc.c) unpacks the request of io_connect_method and packs the answer; the functions here know nothing of messages. */
#ifndef _DARWIN_IOSURFACE_H
#define _DARWIN_IOSURFACE_H

#include <sys/types.h>

struct thread;
struct proc;

#define IOSURF_SEL_CREATE	0	/* the properties as a binary serialized dictionary in, IOSURF_CREATE_SIZE bytes out */
#define IOSURF_SEL_LOOKUP	4	/* one scalar: the id; IOSURF_CREATE_SIZE bytes out */
#define IOSURF_SEL_LIMITS	13	/* nothing in, IOSURF_LIMITS_SIZE bytes out */
#define IOSURF_CREATE_SIZE	0xc68
#define IOSURF_LIMITS_SIZE	40

#define IOSURF_TRAP_LOCK	2	/* iokit_user_client_trap indices of IOSurfaceRoot */
#define IOSURF_TRAP_UNLOCK	3
#define IOSURF_TRAP_RELEASE	5

/* the number that a key of a binary serialized dictionary (OSSerializeBinary) has: 1 and the number, 0 when there is none */
int	iosurface_dict_number(const uint8_t *buf, size_t len, const char *key, uint64_t *out);
/* the surface of the properties in buf: memory made and mapped into the task of td, the structure that the framework reads written to out (IOSURF_CREATE_SIZE bytes, zeroed by the caller); a kern_return_t */
int	iosurface_create(struct thread *td, const uint8_t *props, size_t len, uint8_t *out);
int	iosurface_lookup(struct thread *td, uint64_t id, uint8_t *out);
void	iosurface_limits(uint8_t *out);
/* a process that exits loses its uses of the surfaces (the last use of a surface frees it) */
void	iosurface_proc_exit(struct proc *p);
/* the fast methods (iokit_user_client_trap): a kern_return_t */
int	iosurface_trap(struct thread *td, uint32_t index, uint64_t id, uint64_t options, uint64_t seed_user_addr);

#endif
