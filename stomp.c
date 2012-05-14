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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include "server.h"
#include "client.h"
#include "stomp.h"

/* internal data structs */
struct CommandHandler
{
   enum stomp_cmd cmd;
   char command[15];
   enum stomp_direction direction;
   int (*handler)(struct client *client);
};

struct CommandHandler commandreg[] = {
   { STOMP_CMD_CONNECT, "CONNECT", STOMP_IN, stomp_connect },
   { STOMP_CMD_CONNECTED, "CONNECTED", STOMP_OUT, NULL },
   { STOMP_CMD_SEND, "SEND", STOMP_IN, NULL },
   { STOMP_CMD_MESSAGE, "MESSAGE", STOMP_OUT, NULL },
   { STOMP_CMD_SUBSCRIBE, "SUBSCRIBE", STOMP_IN, stomp_subscribe },
   { STOMP_CMD_UNSUBSCRIBE, "UNSUBSCRIBE", STOMP_IN, NULL },
   { STOMP_CMD_ACK, "ACK", STOMP_IN, NULL },
   { STOMP_CMD_RECEIPT, "RECEIPT", STOMP_OUT, NULL },
   { STOMP_CMD_DISCONNECT, "DISCONNECT", STOMP_OUT, stomp_disconnect },
   { STOMP_CMD_ERROR, "ERROR", STOMP_OUT, NULL },
};


int stomp_handle_request(struct client *client)
{
   int i;

   for(i=0; i < sizeof(commandreg)/sizeof(struct CommandHandler); i++){
      if(commandreg[i].direction != STOMP_IN)
         continue;

      if(strncmp(client->request, commandreg[i].command, strlen(commandreg[i].command)) == 0){
         if(client->authenticated == 0){
            if(commandreg[i].cmd != STOMP_CMD_CONNECT && commandreg[i].cmd != STOMP_CMD_DISCONNECT){
               client->response_cmd = STOMP_CMD_ERROR;
               evhttp_add_header(client->response_headers, "message", "Authentication required");
               return 1;
            }
         }

         client->request_cmd = commandreg[i].cmd;
         commandreg[i].handler(client);
         return 0;
      }
   }

   client->response_cmd = STOMP_CMD_ERROR;
   evhttp_add_header(client->response_headers, "message", "Unknown command");

   return 1;
}

int stomp_handle_response(struct client *client)
{
   int i;

   for(i=0; i < sizeof(commandreg)/sizeof(struct CommandHandler); i++){
      if(commandreg[i].direction != STOMP_OUT)
         continue;

      if(commandreg[i].cmd == client->response_cmd){
         evbuffer_add_printf(client->response_buf, "%s\n", commandreg[i].command);
         /* TODO: iterate all response_headers and append them */

         if(evhttp_find_header(client->response_headers, "message") != NULL)
            evbuffer_add_printf(client->response_buf, "message:%s\n", evhttp_find_header(client->response_headers, "message"));

         evbuffer_add_printf(client->response_buf, "\n");
         evbuffer_add(client->response_buf, client->response, strlen(client->response));
         evbuffer_add(client->response_buf, '\0', 1);

         if(commandreg[i].cmd == STOMP_CMD_ERROR){
            client->authenticated = 0;
            shutdown(client->fd, SHUT_RDWR);
         }

         return 0;
      }
   }

   evbuffer_add_printf(client->response_buf, "ERROR\nmessage:Internal error\n");
   evbuffer_add(client->response_buf, '\0', 1);

   return 1;
}


int stomp_connect(struct client *client)
{
   const char *login;
   const char *passcode;

   login = evhttp_find_header(client->request_headers, "login");
   if(login == NULL){
      client->response_cmd = STOMP_CMD_ERROR;
      evhttp_add_header(client->response_headers, "message", "Authentication failed");
      return 1;
   }

   passcode = evhttp_find_header(client->request_headers, "passcode");
   if(passcode == NULL){
      client->response_cmd = STOMP_CMD_ERROR;
      evhttp_add_header(client->response_headers, "message", "Authentication failed");
      return 1;
   }

   if(strcmp(login, AUTH_USER) != 0 || strcmp(passcode, AUTH_PASS) != 0){
      client->response_cmd = STOMP_CMD_ERROR;
      evhttp_add_header(client->response_headers, "message", "Authentication failed");
      return 1;
   }

   client->authenticated = 1;

   client->response_cmd = STOMP_CMD_CONNECTED;
   evhttp_add_header(client->response_headers, "session", "0");

   return 0;
}

int stomp_disconnect(struct client *client)
{
   client->authenticated = 0;
   shutdown(client->fd, SHUT_RDWR);

   return 0;
}


int stomp_subscribe(struct client *client)
{
   struct queue *entry, *tmp_entry;
   const char *queuename;

   queuename = evhttp_find_header(client->request_headers, "destination");
   if(queuename == NULL){
      client->response_cmd = STOMP_CMD_ERROR;
      evhttp_add_header(client->response_headers, "message", "Destination header missing");
      return 1;
   }
         
   for (entry = TAILQ_FIRST(&queues); entry != NULL; entry = tmp_entry) {
      tmp_entry = TAILQ_NEXT(entry, entries);
      if (strcmp(entry->queuename, queuename) == 0){
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
   }

   /* TODO: check if already subscribed */

   TAILQ_INSERT_TAIL(&entry->subscribers, client, entries);

   return 0;
}


int stomp_parse_headers(struct evkeyvalq *headers, char *request)
{
   char *line;
   size_t line_length;
   char *skey, *svalue;
   struct evbuffer *buffer;

   buffer = evbuffer_new();

   evbuffer_add(buffer, request, strlen(request));

   TAILQ_INIT(headers);

   while ((line = evbuffer_readln(buffer, &line_length, EVBUFFER_EOL_CRLF)) != NULL) {
      skey = NULL;
      svalue = NULL;

      if(strchr(line, ':') == NULL){
         printf("IGNORING: <%s>\n", line);
         continue;
      }

      /* Processing of header lines */
      svalue = line;
      skey = strsep(&svalue, ":");
      if (svalue == NULL){
         free(line);
         evbuffer_free(buffer);
         return 2;
      }

      svalue += strspn(svalue, " ");

      /* TODO: check if header with same name already parsed */

      if (evhttp_add_header(headers, skey, svalue) == -1){
         free(line);
         evbuffer_free(buffer);
         return 1;
      }

      free(line);
   }

   evbuffer_free(buffer);

   return 0;
}

