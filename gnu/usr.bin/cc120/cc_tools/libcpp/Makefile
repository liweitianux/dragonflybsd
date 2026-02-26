GCC_NO_PATH=	yes
LOCAL_CONFIG=	yes
.include "../Makefile.inc"
.PATH: ${GCCDIR}/libcpp

LIB=		cpp

CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-Duchar="unsigned char"
CXXFLAGS+=	${NOEXCEPTION_FLAGS}
GOOD_CONFIG=	${.CURDIR}/../../support-libs/libcpp/config.h

CFLAGS+=	-DHAVE_ICONV=0

# From libcpp/Makefile.in (libcpp_a_SOURCES)
SRCS=		charset.cc \
		directives.cc \
		errors.cc \
		expr.cc \
		files.cc \
		identifiers.cc \
		init.cc \
		lex.cc \
		line-map.cc \
		macro.cc \
		mkdeps.cc \
		pch.cc \
		symtab.cc \
		traditional.cc

# Unused as NLS is disabled.
localedir.h:
	touch $@

config.h: ${GOOD_CONFIG}
	cp ${.ALLSRC} .

beforedepend: localedir.h config.h
afterdepend: libcpp.na

CLEANFILES=	localedir.h config.h

.include <bsd.hostlib.mk>
