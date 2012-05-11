#
# Simple libevent based STOMP server
#

LOCALBASE?=/usr/local

CPPFLAGS=-I${LOCALBASE}/include -g -Wall
LDFLAGS=-L${LOCALBASE}/lib/event2 -L${LOCALBASE}/lib

SRC =	server.c common.c stomp.c
OBJS =	${SRC:.c=.o}

all:	redqueue

clean:
	@rm -f *.o *.core

redqueue:	${OBJS}
	$(CC) $(LDFLAGS) -levent -lleveldb ${OBJS} -o redqueue

# SUFFIX RULES
.SUFFIXES: .c .o

.c.o:
	$(CC) $(CPPFLAGS) -c ${.IMPSRC}
