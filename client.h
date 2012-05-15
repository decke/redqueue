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

#ifndef _CLIENT_H_
#define _CLIENT_H_

#include <sys/queue.h>

/**
 * A struct for client specific data, also includes
 * pointer to create a list of clients.
 */
struct client {
   /* The clients socket. */
   int fd;

   /* Authentication flag for this connection. */
   int authenticated;

   /* The bufferedevent for this client. */
   struct bufferevent *bev;


   /* Request with probably padding */
   char *rawrequest;

   /* Parsed command */
   int request_cmd;

   /* Plain request including headers and command */
   char *request;

   /* Request body */
   char *request_body;

   /* Parsed Headers */
   struct evkeyvalq *request_headers;


   /* The output buffer for this client. */
   struct evbuffer *response_buf;

   /* Response command */
   int response_cmd;

   /* Response body */
   char *response;

   /* Response Headers */
   struct evkeyvalq *response_headers;


   TAILQ_ENTRY(client) entries;
};

TAILQ_HEAD(, client) clients;


struct queue {
   char *queuename;
   volatile u_int read;
   volatile u_int write;

   TAILQ_HEAD(, client) subscribers;
   TAILQ_ENTRY(queue) entries;
};

TAILQ_HEAD(, queue) queues;
 
#endif /* _CLIENT_H_ */
