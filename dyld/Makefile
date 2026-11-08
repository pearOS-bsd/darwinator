# machrun: a dyld for FreeBSD: runs Mach-O programs that need dyld and libSystem by binding their imports to FreeBSD's libraries.
#   make            (on FreeBSD)         ./machrun PROGRAM [args]      libexctrace.so: LD_PRELOAD helper that traces Objective-C exceptions
PROG=	machrun
SRCS=	machrun.c shims.c cf.c cc.c objc.c
CFLAGS+=	-I${.CURDIR}/../src -O1 -Wall -Wno-unused-parameter
LDADD+=	-lpthread -lmd -lcrypto
MAN=

.include <bsd.prog.mk>

all: libexctrace.so
libexctrace.so: exctrace.c
	${CC} -shared -fPIC -O1 -fno-omit-frame-pointer -o ${.TARGET} ${.CURDIR}/exctrace.c -lexecinfo
CLEANFILES+= libexctrace.so
