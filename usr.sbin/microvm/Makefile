# microvm(8) - command-line front-end to libmicrovm

PROG=	microvm
SRCS=	main.c
MAN=	microvm.8

CSTD=	gnu99
WARNS?=	6

LDADD=	-lmicrovm -lnvmm -lpthread

.include <bsd.prog.mk>
