#	$FreeBSD$

SRCDIR:=${.PARSEDIR:tA}

.include <src.opts.mk>

PROG=	makefs

CFLAGS+=-I${SRCDIR}

SRCS=	cd9660.c ffs.c \
	makefs.c \
	mtree.c \
	walk.c
MAN=	makefs.8

WARNS?=	3

.include "${SRCDIR}/cd9660/Makefile.inc"
.include "${SRCDIR}/ffs/Makefile.inc"

CFLAGS+=-DHAVE_STRUCT_STAT_ST_FLAGS=1

.PATH: ${SRCTOP}/contrib/mtree
CFLAGS+=-I${SRCTOP}/contrib/mtree
SRCS+=	getid.c misc.c spec.c

.PATH: ${SRCTOP}/contrib/mknod
CFLAGS+=-I${SRCTOP}/contrib/mknod
SRCS+=	pack_dev.c

.PATH:	${SRCTOP}/sys/ufs/ffs
SRCS+=	ffs_tables.c

CFLAGS+=	-I${SRCTOP}/lib/libnetbsd
LIBADD=		netbsd util sbuf

.if ${MK_TESTS} != "no"
SUBDIR+=	tests
.endif

.include <bsd.prog.mk>
