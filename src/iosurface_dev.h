/* /dev/iosurface: the memory of an IOSurface as a file descriptor, for the native programs of the system (pear-metal-helper, the window server).
 * BSD-2-Clause.  This header is the interface: a native program opens /dev/iosurface and asks IOSURF_IOC_OPEN with the id of a surface (IOSurfaceGetID);
 * it gets a descriptor of the POSIX shared memory object that holds the surface (mmap it, MAP_SHARED, alloc_size bytes rounded to pages) and the layout.
 * Allowed for a global surface, a surface made by the caller's user, or any surface for the superuser.  The memory stays while the descriptor or a mapping does,
 * even after the last Mach process released the surface.  The Mac has no such node: its GPU driver and WindowServer reach a surface's memory descriptor in the kernel. */
#ifndef _DARWIN_IOSURFACE_DEV_H
#define _DARWIN_IOSURFACE_DEV_H
#include <sys/types.h>
#include <sys/ioccom.h>

struct iosurf_open {
	uint32_t id;			/* in: the surface */
	int32_t fd;			/* out: a descriptor of its memory (close-on-exec) */
	uint32_t width, height, bytes_per_row, pixel_format, bytes_per_element, pad;
	uint64_t alloc_size;
};
#define IOSURF_IOC_OPEN	_IOWR('S', 1, struct iosurf_open)	/* 0xc0285301 */
#endif
