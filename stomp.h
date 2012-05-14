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

#ifndef _STOMP_H_
#define _STOMP_H_

#include <event2/buffer.h>
#include <event2/http.h>

enum stomp_direction {
   STOMP_IN = 1,
   STOMP_OUT
};

enum stomp_cmd {
   STOMP_CMD_NONE = 0,
   STOMP_CMD_CONNECT,
   STOMP_CMD_CONNECTED,
   STOMP_CMD_SEND,
   STOMP_CMD_MESSAGE,
   STOMP_CMD_SUBSCRIBE,
   STOMP_CMD_UNSUBSCRIBE,
   STOMP_CMD_ACK,
   STOMP_CMD_RECEIPT,
   STOMP_CMD_DISCONNECT,
   STOMP_CMD_ERROR
};

extern int stomp_connect(struct client *client);
extern int stomp_disconnect(struct client *client);
extern int stomp_subscribe(struct client *client);

extern int stomp_handle_request(struct client *client);
extern int stomp_handle_response(struct client *client);
extern int stomp_parse_headers(struct evkeyvalq *headers, char *request);

 
#endif /* _STOMP_H_ */
