# machrun: a dyld for FreeBSD: runs Mach-O programs that need dyld and libSystem by binding their imports to FreeBSD's libraries.
#   make            (on FreeBSD)         ./machrun PROGRAM [args]
PROG=	machrun
SRCS=	machrun.c shims.c cf.c
CFLAGS+=	-I${.CURDIR}/../src -O1 -Wall -Wno-unused-parameter
LDADD+=	-lpthread
MAN=

.include <bsd.prog.mk>
