GCC_NO_PATH=	yes
GCC_NO_LIBS=	yes
LOCAL_CONFIG=	yes
.include "../Makefile.inc"
.PATH: ${GCCDIR}/libcpp

LIB=		cpp
INTERNALLIB=	YES

CFLAGS+=	-Duchar="unsigned char"
CXXFLAGS+=	${NOEXCEPTION_FLAGS}

.if defined(BOOTSTRAPPING)
CFLAGS+=	-DHAVE_ICONV=0
.endif

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

# generated
SRCS+= localedir.h

localedir.h:
	touch $@

CLEANFILES=	localedir.h

.include <bsd.lib.mk>
