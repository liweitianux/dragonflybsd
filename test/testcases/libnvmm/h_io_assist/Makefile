PROG=	h_io_assist

.PATH:	${.CURDIR}/..
SRCS=	h_io_assist.c h_io_assist_asm.S h_common.c

LDADD+=	-lnvmm
DPADD+=	${LIBNVMM}

NOMAN=

.include <bsd.prog.mk>
