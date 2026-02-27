GCC_NO_PATH=	yes
LOCAL_CONFIG=	yes
.include "../Makefile.inc"
.PATH: ${GCCDIR}/libcody

LIB=		cody

GOOD_CONFIG=	${.CURDIR}/../../support-libs/libcody/config.h

# From libcody/Makefile.in (LIBCODY.O)
OBJS=	buffer.o client.o fatal.o netclient.o netserver.o \
	resolver.o packet.o server.o

SRCS=	${OBJS:.o=.cc}

config.h: ${GOOD_CONFIG}
	cp ${.ALLSRC} .

beforedepend: config.h

CLEANFILES=	config.h

.include <bsd.hostlib.mk>
