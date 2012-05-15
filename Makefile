#
# Simple libevent based STOMP server
#

LOCALBASE?=/usr/local

# Development flags
.if !defined(NODEBUG)
CFLAGS+=-g -Wall -DDEBUG
.endif

# optional LevelDB support
.if !defined(NOLEVELDB)
CFLAGS+=-DWITH_LEVELDB
LDFLAGS+=-lleveldb
SRC+=	leveldb.c
.endif


CFLAGS+=-I${LOCALBASE}/include
LDFLAGS+=-L${LOCALBASE}/lib/event2 -L${LOCALBASE}/lib

SRC+=	log.c util.c server.c common.c stomp.c
OBJS=	${SRC:.c=.o}

all:	redqd

clean:
	@rm -f *.o *.core

redqd:	${OBJS}
	$(CC) $(LDFLAGS) -levent ${OBJS} -o redqd

# SUFFIX RULES
.SUFFIXES: .c .o

.c.o:
	$(CC) $(CFLAGS) -c ${.IMPSRC}
