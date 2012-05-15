#
# Simple libevent based STOMP server
#

# Development flags
CFLAGS+=-g -Wall

LOCALBASE?=/usr/local

CFLAGS+=-I${LOCALBASE}/include
LDFLAGS+=-L${LOCALBASE}/lib/event2 -L${LOCALBASE}/lib

SRC=	log.c util.c server.c common.c stomp.c
OBJS=	${SRC:.c=.o}

all:	redqd

clean:
	@rm -f *.o *.core

redqd:	${OBJS}
	$(CC) $(LDFLAGS) -levent -lleveldb ${OBJS} -o redqd

# SUFFIX RULES
.SUFFIXES: .c .o

.c.o:
	$(CC) $(CFLAGS) -c ${.IMPSRC}
