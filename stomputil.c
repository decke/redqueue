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
#include <string.h>
#include <sys/queue.h>
#include <unistd.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include "client.h"
#include "log.h"
#include "stomp.h"
#include "stomputil.h"
#include "leveldb.h"


struct queue* stomp_add_queue(const char *queuename)
{
   struct queue *entry;
   
   if(queuename == NULL || strlen(queuename) >= MAXQUEUELEN)
      return NULL;
         
   entry = malloc(sizeof(*entry));
   entry->queuename = malloc(strlen(queuename)+1);
   strcpy(entry->queuename, queuename);
 
   TAILQ_INIT(&entry->subscribers);
   TAILQ_INSERT_TAIL(&queues, entry, entries);

#ifdef WITH_LEVELDB
   if(leveldb_load_queue(entry) != 0){
      stomp_free_queue(entry);
      return NULL;
   }
#else
   entry->read = 1;
   entry->write = 1;
#endif
       
   return entry;
}  
   
struct queue* stomp_find_queue(const char *queuename)
{
   struct queue *queue;

   TAILQ_FOREACH(queue, &queues, entries) {
      if(strcmp(queue->queuename, queuename) == 0){
         return queue;
      }
   }

   return NULL;
}

void stomp_free_queue(struct queue *queue)
{
   /* TODO: Remove subscriptions */

   TAILQ_REMOVE(&queues, queue, entries);
   free(queue);
}

void stomp_free_client(struct client *client)
{        
   /* Error/Logout */
   logwarn("Logout Client %d", client->fd);

   client->authenticated = 0;
   /* TODO: remove all subscriptions */
         
   /* Disconnect/Free */
   if(client->response_cmd == STOMP_CMD_DISCONNECT){
      /* TODO: free all allocated memory */
      TAILQ_REMOVE(&clients, client, entries);

      bufferevent_free(client->bev);
      close(client->fd);
      free(client);
   }
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

      if(line_length > MAXHEADERLEN){
         free(line);
         evbuffer_free(buffer);
         logwarn("Request exceeded maximum header length %d", MAXHEADERLEN);
         return 1;
      }

      if(strchr(line, ':') == NULL){
         free(line);
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

      if (evhttp_find_header(headers, skey) == NULL){
         if (evhttp_add_header(headers, skey, svalue) == -1){
            free(line);
            evbuffer_free(buffer);
            return 1;
         }
      }

      free(line);
   }

   evbuffer_free(buffer);

   return 0;
}

