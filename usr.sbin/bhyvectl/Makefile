#
# $FreeBSD$
#

PROG=	bhyvectl
SRCS=	bhyvectl.c

MAN=	bhyvectl.8

LIBADD=	vmmapi util

WARNS?=	3

CFLAGS+= -I${.CURDIR}/../../sys/amd64/vmm

.include <bsd.prog.mk>
