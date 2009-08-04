PROG=	cblog.cgi
SRCS+=	cblog.c tools.c
PREFIX?=	/usr/local
DEFINE=-DETCDIR=\"${PREFIX}/etc\"
INCLUDE=-I/usr/local/include -I/usr/local/include/ClearSilver
CFLAGS+=	-Wall -g ${INCLUDE} ${DEFINE}
LDADD+=	-L/usr/local/lib -lmrss -lnxml -lz -lneo_cgi -lneo_cs -lneo_utl -lmarkdown

.include <bsd.prog.mk>
