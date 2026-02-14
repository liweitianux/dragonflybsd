PROG=	h_mem_assist

.PATH:	${.CURDIR}/..
SRCS=	h_mem_assist.c h_mem_assist_asm.S h_common.c

LDADD+=	-lnvmm
DPADD+=	${LIBNVMM}

NOMAN=

.include <bsd.prog.mk>
