/*
 * Copyright (C) 2011 Bernhard Froehlich <decke@bluelife.at>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Author's name may not be used endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>

/* Required by event.h. */
#include <sys/time.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <sys/queue.h>
#include <sys/stat.h>

/* Libevent. */
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

/* LevelDB */
#include <leveldb/c.h>

#define SERVER_PORT 8080
#define MAX_BUF 16384

/**
 * A struct for client specific data, also includes pointer to create
 * a list of clients.
 */
struct client {
	/* The clients socket. */
	int fd;

	/* The bufferedevent for this client. */
	struct bufferevent *buf_ev;

	TAILQ_ENTRY(client) entries;
};
TAILQ_HEAD(, client) clients;

struct queue {
	char *queuename;
	TAILQ_HEAD(, client) subscribers;
	TAILQ_ENTRY(queue) entries;
};
TAILQ_HEAD(, queue) queues;

struct event_base *base;


#define CheckNoError(err) \
        if ((err) != NULL) { \
                fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, (err)); \
                abort(); \
        }


void signal_handler(int sig) {
	switch(sig) {
		case SIGTERM:
		case SIGHUP:
		case SIGINT:
			event_base_loopbreak(base);
			break;
        default:
            syslog(LOG_WARNING, "Unhandled signal (%d) %s", sig, strsignal(sig));
            break;
    }
}

/**
 * Set a socket to non-blocking mode.
 */
int setnonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return flags;
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0)
		return -1;

	return 0;
}

int find_readers(char *queuename, struct evbuffer *evb)
{
	struct queue *entry, *tmp_entry;
	char buf[MAX_BUF];

	*buf = '\0';
	for (entry = TAILQ_FIRST(&queues); entry != NULL; entry = tmp_entry) {
		tmp_entry = TAILQ_NEXT(entry, entries);
		if (strncmp(queuename, entry->queuename, strlen(entry->queuename)) == 0){
			evbuffer_add_printf(evb, "queue: %s", queuename);
			//TAILQ_REMOVE(&readers, entry, entries);
			return 1;
		}
	}
	return 0;
}

int stomp_parse_headers(struct evkeyvalq* headers, struct evbuffer* buffer)
{
	char *line;
	size_t line_length;
	char *skey, *svalue;

	TAILQ_INIT(headers);

	while ((line = evbuffer_readln(buffer, &line_length, EVBUFFER_EOL_CRLF)) != NULL) {
		skey = NULL;
		svalue = NULL;

		if (*line == '\0') {
			free(line);
			return 0;
		}

		/* Processing of header lines */
		svalue = line;
		skey = strsep(&svalue, ":");
		if (svalue == NULL){
			free(line);
			return 1;
		}

		svalue += strspn(svalue, " ");

		printf("HEADER: <%s> <%s>\n", skey, svalue);

		if (evhttp_add_header(headers, skey, svalue) == -1){
			free(line);
			return 2;
		}

		free(line);
	}

	return 0;
}


/**
 * Called by libevent when there is data to read.
 */
void buffered_on_read(struct bufferevent *bev, void *arg)
{
	/* Write back the read buffer. It is important to note that
	 * bufferevent_write_buffer will drain the incoming data so it
	 * is effectively gone after we call it. */
	struct client *cli = (struct client *)arg;
	struct queue *entry, *tmp_entry;
	struct evbuffer *evb, *evb2;
	char *request, *header_begin;
	struct evkeyvalq *headers;
	const char *queuename;
	
	request = evbuffer_readln(bufferevent_get_input(bev), NULL, EVBUFFER_EOL_NUL);
	if (request == NULL) {
		return;
	}
	
	header_begin = strstr(request, "\r\n");
	if(header_begin == NULL){
		free(request);
		return;
	}

	evb = evbuffer_new();
	evb2 = evbuffer_new();

	evbuffer_prepend(evb2, header_begin+2, strlen(header_begin+2));

	headers = calloc(1, sizeof(struct evkeyvalq));
	if(headers == NULL){
		goto error;
	}

	if(stomp_parse_headers(headers, evb2) != 0){
		evbuffer_add_printf(evb, "Invalid Request\n");
	}
	else if (strncmp(request, "SUBSCRIBE", 9) == 0) {
		queuename = evhttp_find_header(headers, "destination");
		if(queuename == NULL){
			evbuffer_add_printf(evb, "Destination header missing\n");
			goto error;
		}

		for (entry = TAILQ_FIRST(&queues); entry != NULL; entry = tmp_entry) {
			tmp_entry = TAILQ_NEXT(entry, entries);
			if (strcmp(entry->queuename, queuename) == 0){
				evbuffer_add_printf(evb, "queue %s found\n", queuename);
				entry = tmp_entry;
				break;
			}
		}

		if (entry == NULL){
			entry = malloc(sizeof(*entry));
			entry->queuename = malloc(strlen(queuename)+1);
			strcpy(entry->queuename, queuename);
			TAILQ_INIT(&entry->subscribers);
			TAILQ_INSERT_TAIL(&queues, entry, entries);
			evbuffer_add_printf(evb, "queue %s created\n", queuename);
		}

		/* TODO: check if already subscribed */

		TAILQ_INSERT_TAIL(&entry->subscribers, cli, entries);
	}
	else if (strncmp(request, "exit", 4) == 0 || strncmp(request, "quit", 4) == 0) {
		evbuffer_add_printf(evb, "ok bye\n");
		shutdown(cli->fd, SHUT_RDWR);
	}
	else {
		evbuffer_add_printf(evb, "error unknown command\n");
	}

error:
	bufferevent_write_buffer(bev, evb);
	evbuffer_free(evb2);
	evbuffer_free(evb);
	free(request);
	free(headers);
}

/**
 * Called by libevent when the write buffer reaches 0.  We only
 * provide this because libevent expects it, but we don't use it.
 */
void buffered_on_write(struct bufferevent *bev, void *arg)
{
}

/**
 * Called by libevent when there is an error on the underlying socket
 * descriptor.
 */
void buffered_on_error(struct bufferevent *bev, short what, void *arg)
{
	struct client *client = (struct client *)arg;
	struct client *entry, *tmp_entry;

	if (what & BEV_EVENT_EOF) {
		/* Client disconnected, remove the read event and the
		 * free the client structure. */
		printf("Client disconnected.\n");
	}
	else {
		warn("Client socket error, disconnecting.\n");
	}

	for (entry = TAILQ_FIRST(&clients); entry != NULL; entry = tmp_entry) {
		tmp_entry = TAILQ_NEXT(entry, entries);
		if ((void *)tmp_entry != NULL && client->fd == tmp_entry->fd) {
			TAILQ_REMOVE(&clients, entry, entries);
			free(entry);
		}
	}

	/* TODO: remove from subscribers */

	bufferevent_free(client->buf_ev);
	close(client->fd);
	free(client);
}

/**
 * This function will be called by libevent when there is a connection
 * ready to be accepted.
 */
void on_accept(int fd, short ev, void *arg)
{
	int client_fd;
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	struct client *client;

	client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
	if (client_fd < 0) {
		warn("accept failed");
		return;
	}

	/* Set the client socket to non-blocking mode. */
	if (setnonblock(client_fd) < 0)
		warn("failed to set client socket non-blocking");

	/* We've accepted a new client, create a client object. */
	client = calloc(1, sizeof(*client));
	if (client == NULL)
		err(1, "malloc failed");
	client->fd = client_fd;
	
	/* Create the buffered event.
	 *
	 * The first argument is the file descriptor that will trigger
	 * the events, in this case the clients socket.
	 *
	 * The second argument is the callback that will be called
	 * when data has been read from the socket and is available to
	 * the application.
	 *
	 * The third argument is a callback to a function that will be
	 * called when the write buffer has reached a low watermark.
	 * That usually means that when the write buffer is 0 length,
	 * this callback will be called.  It must be defined, but you
	 * don't actually have to do anything in this callback.
	 *
	 * The fourth argument is a callback that will be called when
	 * there is a socket error.  This is where you will detect
	 * that the client disconnected or other socket errors.
	 *
	 * The fifth and final argument is to store an argument in
	 * that will be passed to the callbacks.  We store the client
	 * object here.
	 */
	/*
	client->buf_ev = bufferevent_new(client_fd, buffered_on_read,
	    buffered_on_write, buffered_on_error, client);
	*/

	client->buf_ev = bufferevent_socket_new(base, client_fd, 0); 
	bufferevent_setcb(client->buf_ev, buffered_on_read, buffered_on_write,
		buffered_on_error, client);

	/* We have to enable it before our callbacks will be
	 * called. */
	bufferevent_enable(client->buf_ev, EV_READ);
}

int main(int argc, char **argv)
{
	int listen_fd, ch;
	int daemon = 0;
	int port = SERVER_PORT;
	struct sockaddr_in listen_addr;
	struct event *ev_accept;
	int reuseaddr_on;
	pid_t pid, sid;

	leveldb_t* db;
	leveldb_cache_t* cache;
	leveldb_env_t* env;
	leveldb_options_t* options;
	char *error = NULL;

	signal(SIGHUP, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGQUIT, signal_handler);

	while ((ch = getopt(argc, argv, "dp:")) != -1) {
	    switch (ch) {
	    case 'd':
	        daemon = 1;
	        break;
	    case 'p':
	        port = atoi(optarg);
	        break;
	    }
	}

	if (daemon) {
	    pid = fork();
	    if (pid < 0) {
			exit(EXIT_FAILURE);
	    } else if (pid > 0) {
			exit(EXIT_SUCCESS);
	    }

	    umask(0);
	    sid = setsid();
	    if (sid < 0) {
	    	exit(EXIT_FAILURE);
	    }
	}


	TAILQ_INIT(&clients);
	TAILQ_INIT(&queues);

	/* Initialize LevelDB */
	env = leveldb_create_default_env();
	cache = leveldb_cache_create_lru(100000);

	options = leveldb_options_create();
	leveldb_options_set_cache(options, cache);
	leveldb_options_set_env(options, env);
	leveldb_options_set_create_if_missing(options, 1);
	leveldb_options_set_error_if_exists(options, 0);

	db = leveldb_open(options, "/tmp/redqueue.db", &error);
	CheckNoError(error);
	
	/* Initialize libevent. */
	base = event_base_new();

	/* Create our listening socket. */
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
		err(1, "listen failed");

	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.sin_family = AF_INET;
	listen_addr.sin_addr.s_addr = INADDR_ANY;
	listen_addr.sin_port = htons(port);

	if (bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0)
		err(1, "bind failed");

	if (listen(listen_fd, 5) < 0)
		err(1, "listen failed");

	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_on, sizeof(reuseaddr_on));

	/* Set the socket to non-blocking, this is essential in event
	 * based programming with libevent. */
	if (setnonblock(listen_fd) < 0)
		err(1, "failed to set server socket to non-blocking");

	/* We now have a listening socket, we create a read event to
	 * be notified when a client connects. */
	ev_accept = event_new(base, listen_fd, EV_READ|EV_PERSIST, on_accept, NULL);
	event_add(ev_accept, NULL);

	/* Start the event loop. */
	event_base_dispatch(base);

	shutdown(listen_fd, SHUT_RDWR);
	close(listen_fd);
	printf("dying\n");

	leveldb_close(db);
	leveldb_options_destroy(options);
	leveldb_cache_destroy(cache);
	leveldb_env_destroy(env);

	return 0;
}
