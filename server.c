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
#include <sys/stat.h>

/* Libevent. */
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

/* LevelDB */
#include <leveldb/c.h>

#include "common.h"
#include "server.h"
#include "client.h"
#include "stomp.h"

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

/**
 * Called by libevent when there is data to read.
 */
void buffered_on_read(struct bufferevent *bev, void *arg)
{
	/* Write back the read buffer. It is important to note that
	 * bufferevent_write_buffer will drain the incoming data so it
	 * is effectively gone after we call it. */
	struct client *client = (struct client *)arg;
	
	client->rawrequest = evbuffer_readln(bufferevent_get_input(bev), NULL, EVBUFFER_EOL_NUL);
	if (client->rawrequest == NULL)
		goto error;

	client->request_headers = calloc(1, sizeof(struct evkeyvalq));
	if(client->request_headers == NULL)
		goto error;

	TAILQ_INIT(client->request_headers);

	client->response_buf = evbuffer_new();
	if(client->response_buf == NULL)
		goto error;

	client->response_headers = calloc(1, sizeof(struct evkeyvalq));
	if(client->response_headers == NULL)
		goto error;

	TAILQ_INIT(client->response_headers);

	client->request = client->rawrequest;

	/* skip leading whitespace */
	while(*client->request == '\r' || *client->request == '\n')
		*(client->request)++;

	if(stomp_parse_headers(client->request_headers, client->request) != 0){
		client->response_cmd = STOMP_CMD_ERROR;
		evhttp_add_header(client->response_headers, "message", "Invalid Request");
		goto error;
	}

        stomp_handle_request(client);
        stomp_handle_response(client);

error:
	if(client->response_buf){
		bufferevent_write_buffer(bev, client->response_buf);
		evbuffer_free(client->response_buf);
		client->response_buf = NULL;
	}

	if(client->response_headers){
		free(client->response_headers);
		client->response_headers = NULL;
	}

	if(client->request_headers){
		free(client->request_headers);
		client->request_headers = NULL;
	}

	if(client->rawrequest){
		free(client->rawrequest);
		client->rawrequest = NULL;
		client->request = NULL;
	}
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

	bufferevent_free(client->request_buf);
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
	client->authenticated = 0;
	client->request_buf = bufferevent_socket_new(base, client_fd, 0); 
	bufferevent_setcb(client->request_buf, buffered_on_read, buffered_on_write,
		buffered_on_error, client);

	/* We have to enable it before our callbacks will be
	 * called. */
	bufferevent_enable(client->request_buf, EV_READ);
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
