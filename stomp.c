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
#include <unistd.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include "log.h"
#include "util.h"
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
   { STOMP_CMD_NONE, "", STOMP_OUT, NULL },
   { STOMP_CMD_DISCONNECT, "", STOMP_OUT, NULL },
   { STOMP_CMD_CONNECT, "CONNECT", STOMP_IN, stomp_connect },
   { STOMP_CMD_CONNECTED, "CONNECTED", STOMP_OUT, NULL },
   { STOMP_CMD_SEND, "SEND", STOMP_IN, stomp_send },
   { STOMP_CMD_MESSAGE, "MESSAGE", STOMP_OUT, NULL },
   { STOMP_CMD_SUBSCRIBE, "SUBSCRIBE", STOMP_IN, stomp_subscribe },
   { STOMP_CMD_UNSUBSCRIBE, "UNSUBSCRIBE", STOMP_IN, NULL },
   { STOMP_CMD_ACK, "ACK", STOMP_IN, NULL },
   { STOMP_CMD_RECEIPT, "RECEIPT", STOMP_OUT, NULL },
   { STOMP_CMD_DISCONNECT, "DISCONNECT", STOMP_IN, stomp_disconnect },
   { STOMP_CMD_ERROR, "ERROR", STOMP_OUT, NULL },
};


int stomp_handle_request(struct client *client)
{
   int i;

   for(i=0; i < sizeof(commandreg)/sizeof(struct CommandHandler); i++){
      if(commandreg[i].direction != STOMP_IN || commandreg[i].handler == NULL)
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
         return commandreg[i].handler(client);
      }
   }

   client->response_cmd = STOMP_CMD_ERROR;
   evhttp_add_header(client->response_headers, "message", "Unknown command");

   return 1;
}

int stomp_handle_response(struct client *client)
{
   int i;
   int found;
   const char *receipt;
   struct evkeyval *header;

   if(client->response_buf == NULL)
      client->response_buf = evbuffer_new();

   if(client->request_headers){
      receipt = evhttp_find_header(client->request_headers, "receipt");
      if(receipt != NULL && client->response_cmd != STOMP_CMD_ERROR){
         evbuffer_add_printf(client->response_buf, "RECEIPT\n");
         evbuffer_add_printf(client->response_buf, "receipt:%s\n", receipt);
         evbuffer_add_printf(client->response_buf, "\n");
         evbuffer_add(client->response_buf, "\0", 1);

         evhttp_remove_header(client->request_headers, "receipt");
      }
   }

   for(i=0,found=0; i < sizeof(commandreg)/sizeof(struct CommandHandler); i++){
      if(commandreg[i].direction != STOMP_OUT)
         continue;

      if(commandreg[i].cmd == client->response_cmd){
         found = 1;

         if(commandreg[i].command[0] == '\0')
            break;

         evbuffer_add_printf(client->response_buf, "%s\n", commandreg[i].command);

         TAILQ_FOREACH(header, client->response_headers, next) {
            if(strcmp(header->key, "receipt") == 0)
               continue;

            evbuffer_add_printf(client->response_buf, "%s:%s\n", header->key, header->value);
         }

         evbuffer_add_printf(client->response_buf, "\n");

         if(client->response != NULL){
            evbuffer_add(client->response_buf, client->response, strlen(client->response));
         }

         evbuffer_add(client->response_buf, "\0", 1);

         break;
      }
   }

   if(found == 0){
      evbuffer_add_printf(client->response_buf, "ERROR\n");
      evbuffer_add_printf(client->response_buf, "message:Internal error\n\n");
      evbuffer_add(client->response_buf, "\0", 1);
   }

   bufferevent_write_buffer(client->bev, client->response_buf);
   bufferevent_flush(client->bev, EV_WRITE, BEV_FINISHED);

   if(client->response_cmd == STOMP_CMD_ERROR || client->response_cmd == STOMP_CMD_DISCONNECT){
      stomp_free_client(client);
   }

   return !found;
}


int stomp_connect(struct client *client)
{
   const char *login;
   const char *passcode;

   if(strlen(configget("authUser")) > 0 && strlen(configget("authPass")) > 0){
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

      if(strcmp(login, configget("authUser")) != 0 || strcmp(passcode, configget("authPass")) != 0){
         client->response_cmd = STOMP_CMD_ERROR;
         evhttp_add_header(client->response_headers, "message", "Authentication failed");
         return 1;
      }
   }

   if(evhttp_find_header(client->request_headers, "receipt") != NULL){
      client->response_cmd = STOMP_CMD_ERROR;
      evhttp_add_header(client->response_headers, "message", "Receipt for connect not supported");
      return 1;
   }

   client->authenticated = 1;

   client->response_cmd = STOMP_CMD_CONNECTED;
   evhttp_add_header(client->response_headers, "session", "0");

   return 0;
}

int stomp_disconnect(struct client *client)
{
   client->response_cmd = STOMP_CMD_DISCONNECT;

   return 0;
}


int stomp_subscribe(struct client *client)
{
   struct queue *entry, *tmp_entry;
   const char *queuename;

   client->response_cmd = STOMP_CMD_NONE;

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

int stomp_send(struct client *client)
{
   struct client *subscriber;
   struct queue *queue;
   const char *queuename;

   queuename = evhttp_find_header(client->request_headers, "destination");
   if(queuename == NULL){
      client->response_cmd = STOMP_CMD_ERROR;
      evhttp_add_header(client->response_headers, "message", "Destination header missing");
      return 1;
   }

   TAILQ_FOREACH(queue, &queues, entries) {
      if(strcmp(queue->queuename, queuename) == 0)
         break;
   }

   if(queue != NULL){
      /* Send it out to the subscribers */
      TAILQ_FOREACH(subscriber, &queue->subscribers, entries){
         subscriber->response_cmd = STOMP_CMD_MESSAGE;
         subscriber->response_headers = client->request_headers;
         subscriber->response = client->request_body;

         stomp_handle_response(subscriber);

         subscriber->response_cmd = STOMP_CMD_NONE;
         subscriber->response_headers = NULL;
         subscriber->response = NULL;
      }
   }
   else if(strncmp(queuename, "/topic/", 7) != 0){
      /* TODO: Store message in LevelDB */

      loginfo("No active subscribers on that queue");
   }

   client->response_cmd = STOMP_CMD_NONE;
   client->response = NULL;

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

   while ((line = evbuffer_readln(buffer, &line_length, EVBUFFER_EOL_CRLF)) != NULL) {
      skey = NULL;
      svalue = NULL;

      if(strchr(line, ':') == NULL){
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


void stomp_free_client(struct client *client)
{
   /* TODO: remove all subscriptions */
   /* TODO: free all allocated memory */

   struct client *entry, *tmp_entry;

   for (entry = TAILQ_FIRST(&clients); entry != NULL; entry = tmp_entry) {
      tmp_entry = TAILQ_NEXT(entry, entries);
      if ((void *)tmp_entry != NULL && client->fd == tmp_entry->fd) {
         TAILQ_REMOVE(&clients, entry, entries);
         free(entry);
      }
   }

   client->authenticated = 0;
   logwarn("Free client %d", client->fd);

   if(client->response_cmd == STOMP_CMD_DISCONNECT){
      bufferevent_free(client->bev);
      close(client->fd);
      free(client);
   }
}

