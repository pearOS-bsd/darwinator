# darwinator: FreeBSD kernel module that runs Mach-O executables (arm64 and amd64).
#   make SYSDIR=/usr/src/sys        (SYSDIR defaults to /usr/src/sys)
.PATH: ${.CURDIR}/src

KMOD=	darwinator
SRCS=	darwinator.c macho.c syscalls.c syscalls_more.c syscalls_event.c darwin_${MACHINE_CPUARCH}.c
SRCS+=	vnode_if.h

.include <bsd.kmod.mk>
