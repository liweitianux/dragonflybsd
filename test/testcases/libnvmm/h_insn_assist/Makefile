PROG=	h_insn_assist

.PATH:	${.CURDIR}/..
SRCS=	h_insn_assist.c h_insn_assist_asm.S

LDADD+=	-lnvmm
DPADD+=	${LIBNVMM}

NOMAN=

.include <bsd.prog.mk>
