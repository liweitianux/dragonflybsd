GCC_NO_PATH=	yes
GCC_NO_LIBS=	yes
LOCAL_CONFIG=	yes
.include "../Makefile.inc"
.PATH: ${GCCDIR}/libcody

LIB=		cody
INTERNALLIB=	YES

# From libcody/Makefile.in (LIBCODY.O)
OBJS=	buffer.o client.o fatal.o netclient.o netserver.o \
	resolver.o packet.o server.o

SRCS=	${OBJS:.o=.cc}

.include <bsd.lib.mk>
