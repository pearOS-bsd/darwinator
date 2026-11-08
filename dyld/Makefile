# machrun: a dyld for FreeBSD: runs Mach-O programs that need dyld and libSystem by binding their imports to FreeBSD's libraries.
#   make            (on FreeBSD)         ./machrun PROGRAM [args]      libexctrace.so: LD_PRELOAD helper that traces Objective-C exceptions
PROG=	machrun
SRCS=	machrun.c shims.c cf.c cc.c objc.c
CFLAGS+=	-I${.CURDIR}/../src -O1 -Wall -Wno-unused-parameter
LDADD+=	-lpthread -lmd -lcrypto
MAN=

.include <bsd.prog.mk>

all: libexctrace.so libmachcompat.so
libexctrace.so: exctrace.c
	${CC} -shared -fPIC -O1 -fno-omit-frame-pointer -o ${.TARGET} ${.CURDIR}/exctrace.c -lexecinfo
CLEANFILES+= libexctrace.so
libmachcompat.so: compat.m
	${CC} -shared -fPIC -O1 -fobjc-runtime=gnustep-2.0 -fblocks -I/usr/local/GNUstep/System/Library/Headers -I/usr/local/include \
	    -o ${.TARGET} ${.CURDIR}/compat.m -L/usr/local/GNUstep/System/Library/Libraries -L/usr/local/lib -lgnustep-base -lobjc
CLEANFILES+= libmachcompat.so
