/*
  Vapour: A memcached interface on top of NDB
  Copyright (C) 2010 Stewart Smith

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

  Derived from memcached_light, which is under BSD:

  Software License Agreement (BSD License)

Copyright (c) 2007, TangentOrg (Brian Aker)
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.

    * Neither the name of TangentOrg nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "config.h"
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <libmemcached/protocol_handler.h>

#include "ndb_storage.h"


/* Byte swap a 64-bit number. */
#ifndef swap64
static inline uint64_t swap64(uint64_t in)
{
#ifndef WORDS_BIGENDIAN
  /* Little endian, flip the bytes around until someone makes a faster/better
   * way to do this. */
  uint64_t rv= 0;
  for (uint8_t x= 0; x < 8; x++)
  {
    rv= (rv << 8) | (in & 0xff);
    in >>= 8;
  }
  return rv;
#else
  /* big-endian machines don't need byte swapping */
  return in;
#endif // WORDS_BIGENDIAN
}
#endif

static uint64_t ntohll(uint64_t value)
{
  return swap64(value);
}

static uint64_t htonll(uint64_t value)
{
  return swap64(value);
}

static int server_sockets[1024];
static int num_server_sockets= 0;
static void* socket_userdata_map[1024];
static bool verbose= false;

static void work(void);


static int server_socket(const char *port) {
  struct addrinfo *ai;
  struct addrinfo hints= { .ai_flags= AI_PASSIVE,
                           .ai_family= AF_UNSPEC,
                           .ai_socktype= SOCK_STREAM };

  int error= getaddrinfo("127.0.0.1", port, &hints, &ai);
  if (error != 0)
  {
    if (error != EAI_SYSTEM)
      fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
    else
      perror("getaddrinfo()");

    return 1;
  }

  struct linger ling= {0, 0};

  for (struct addrinfo *next= ai; next; next= next->ai_next)
  {
    int sock= socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock == -1)
    {
      perror("Failed to create socket");
      continue;
    }

    int flags= fcntl(sock, F_GETFL, 0);
    if (flags == -1)
    {
      perror("Failed to get socket flags");
      close(sock);
      continue;
    }
    if ((flags & O_NONBLOCK) != O_NONBLOCK)
    {
      if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1)
      {
        perror("Failed to set socket to nonblocking mode");
        close(sock);
        continue;
      }
    }

    flags= 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags)) != 0)
      perror("Failed to set SO_REUSEADDR");

    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags)) != 0)
      perror("Failed to set SO_KEEPALIVE");

    if (setsockopt(sock, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling)) != 0)
      perror("Failed to set SO_LINGER");

    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags)) != 0)
      perror("Failed to set TCP_NODELAY");

    if (bind(sock, next->ai_addr, next->ai_addrlen) == -1)
    {
      if (errno != EADDRINUSE)
      {
        perror("bind()");
        freeaddrinfo(ai);
      }
      close(sock);
      continue;
    }

    if (listen(sock, 1024) == -1)
    {
      perror("listen()");
      close(sock);
      continue;
    }

    server_sockets[num_server_sockets++]= sock;
  }

  freeaddrinfo(ai);

  return (num_server_sockets > 0) ? 0 : 1;
}

static void help(char *argv0)
{
  fprintf(stderr, "Usage: %s [-h?] [-V]\n", argv0);
  fprintf(stderr, "\n");
  fprintf(stderr, "-h -?\tHelp\n");
  fprintf(stderr, "-V\tVersion\n");
  fprintf(stderr, "\n");
}

memcached_binary_protocol_callback_st interface_v0_impl;

/**
 * Convert a command code to a textual string
 * @param cmd the comcode to convert
 * @return a textual string with the command or NULL for unknown commands
 */
static const char* comcode2str(uint8_t cmd)
{
  static const char * const text[] = {
    "GET", "SET", "ADD", "REPLACE", "DELETE",
    "INCREMENT", "DECREMENT", "QUIT", "FLUSH",
    "GETQ", "NOOP", "VERSION", "GETK", "GETKQ",
    "APPEND", "PREPEND", "STAT", "SETQ", "ADDQ",
    "REPLACEQ", "DELETEQ", "INCREMENTQ", "DECREMENTQ",
    "QUITQ", "FLUSHQ", "APPENDQ", "PREPENDQ"
  };

  if (cmd <= PROTOCOL_BINARY_CMD_PREPENDQ)
    return text[cmd];

  return NULL;
}

/**
 * Print out the command we are about to execute
 */
static void pre_execute(const void *cookie __attribute__((unused)),
                        protocol_binary_request_header *header __attribute__((unused)))
{
  if (verbose)
  {
    const char *cmd= comcode2str(header->request.opcode);
    if (cmd != NULL)
      fprintf(stderr, "pre_execute from %p: %s\n", cookie, cmd);
    else
      fprintf(stderr, "pre_execute from %p: 0x%02x\n", cookie, header->request.opcode);
  }
}

/**
 * Print out the command we just executed
 */
static void post_execute(const void *cookie __attribute__((unused)),
                         protocol_binary_request_header *header __attribute__((unused)))
{
  if (verbose)
  {
    const char *cmd= comcode2str(header->request.opcode);
    if (cmd != NULL)
      fprintf(stderr, "post_execute from %p: %s\n", cookie, cmd);
    else
      fprintf(stderr, "post_execute from %p: 0x%02x\n", cookie, header->request.opcode);
  }
}

/**
 * Callback handler for all unknown commands.
 * Send an unknown command back to the client
 */
static protocol_binary_response_status unknown(const void *cookie,
                                               protocol_binary_request_header *header,
                                               memcached_binary_protocol_raw_response_handler response_handler)
{
  protocol_binary_response_no_extras response= {
    .message= {
      .header.response= {
        .magic= PROTOCOL_BINARY_RES,
        .opcode= header->request.opcode,
        .status= htons(PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND),
        .opaque= header->request.opaque
      }
    }
  };

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status noop_command_handler(const void *cookie,
                                                            protocol_binary_request_header *header,
                                                            memcached_binary_protocol_raw_response_handler response_handler)
{
  protocol_binary_response_no_extras response= {
    .message.header.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= PROTOCOL_BINARY_CMD_NOOP,
      .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
      .opaque= header->request.opaque
    }
  };

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status quit_command_handler(const void *cookie,
                                                            protocol_binary_request_header *header,
                                                            memcached_binary_protocol_raw_response_handler response_handler)
{
  protocol_binary_response_no_extras response= {
    .message.header.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= PROTOCOL_BINARY_CMD_QUIT,
      .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
      .opaque= header->request.opaque
    }
  };

  if (header->request.opcode == PROTOCOL_BINARY_CMD_QUIT)
    response_handler(cookie, header, (void*)&response);

  /* I need a better way to signal to close the connection */
  return PROTOCOL_BINARY_RESPONSE_EIO;
}

static protocol_binary_response_status get_command_handler(const void *cookie,
                                                           protocol_binary_request_header *header,
                                                           memcached_binary_protocol_raw_response_handler response_handler)
{
  uint8_t opcode= header->request.opcode;
  union {
    protocol_binary_response_get response;
    char buffer[4096];
  } msg= {
    .response.message.header.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= opcode,
      .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
      .opaque= header->request.opaque
    }
  };

  struct item *item= get_item(header + 1, ntohs(header->request.keylen));
  if (item)
  {
    msg.response.message.body.flags= htonl(item->flags);
    char *ptr= (char*)(msg.response.bytes + sizeof(*header) + 4);
    uint32_t bodysize= 4;
    msg.response.message.header.response.cas= htonll(item->cas);
    if (opcode == PROTOCOL_BINARY_CMD_GETK || opcode == PROTOCOL_BINARY_CMD_GETKQ)
    {
      memcpy(ptr, item->key, item->nkey);
      msg.response.message.header.response.keylen= htons((uint16_t)item->nkey);
      ptr += item->nkey;
      bodysize += (uint32_t)item->nkey;
    }
    memcpy(ptr, item->data, item->size);
    bodysize += (uint32_t)item->size;
    msg.response.message.header.response.bodylen= htonl(bodysize);
    msg.response.message.header.response.extlen= 4;

    release_item(item);
    return response_handler(cookie, header, (void*)&msg);
  }
  else if (opcode == PROTOCOL_BINARY_CMD_GET || opcode == PROTOCOL_BINARY_CMD_GETK)
  {
    msg.response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
    return response_handler(cookie, header, (void*)&msg);
  }

  /* Q shouldn't report a miss ;-) */
  return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status delete_command_handler(const void *cookie,
                                                              protocol_binary_request_header *header,
                                                              memcached_binary_protocol_raw_response_handler response_handler)
{
  size_t keylen= ntohs(header->request.keylen);
  char *key= ((char*)header) + sizeof(*header);
  protocol_binary_response_no_extras response= {
    .message.header.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= header->request.opcode,
      .opaque= header->request.opaque
    }
  };

  if (!delete_item(key, keylen))
  {
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
    return response_handler(cookie, header, (void*)&response);
  }
  else if (header->request.opcode == PROTOCOL_BINARY_CMD_DELETE)
  {
    /* DELETEQ doesn't want success response */
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS);
    return response_handler(cookie, header, (void*)&response);
  }

  return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status flush_command_handler(const void *cookie,
                                                             protocol_binary_request_header *header,
                                                             memcached_binary_protocol_raw_response_handler response_handler)
{
  uint8_t opcode= header->request.opcode;

  /* @fixme sett inn when! */
  flush(0);

  if (opcode == PROTOCOL_BINARY_CMD_FLUSH)
  {
    protocol_binary_response_no_extras response= {
      .message.header.response= {
        .magic= PROTOCOL_BINARY_RES,
        .opcode= opcode,
        .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
        .opaque= header->request.opaque
      }
    };
    return response_handler(cookie, header, (void*)&response);
  }

  return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status arithmetic_command_handler(const void *cookie,
                                                                  protocol_binary_request_header *header,
                                                                  memcached_binary_protocol_raw_response_handler response_handler)
{
  protocol_binary_request_incr *req= (void*)header;
  protocol_binary_response_incr response= {
    .message.header.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= header->request.opcode,
      .opaque= header->request.opaque,
    },
  };

  uint16_t keylen= ntohs(header->request.keylen);
  uint64_t initial= ntohll(req->message.body.initial);
  uint64_t delta= ntohll(req->message.body.delta);
  uint32_t expiration= ntohl(req->message.body.expiration);
  uint32_t flags= 0;
  void *key= req->bytes + sizeof(req->bytes);
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;

  uint64_t value= initial;

  struct item *item= get_item(key, keylen);
  if (item != NULL)
  {
    if (header->request.opcode == PROTOCOL_BINARY_CMD_INCREMENT ||
        header->request.opcode == PROTOCOL_BINARY_CMD_INCREMENTQ)
    {
      value= (*(uint64_t*)item->data) + delta;
    }
    else
    {
      if (delta > *(uint64_t*)item->data)
      {
        value= 0;
      }
      else
      {
        value= *(uint64_t*)item->data - delta;
      }
    }
    expiration= (uint32_t)item->exp;
    flags= item->flags;

    release_item(item);
    delete_item(key, keylen);
  }

  item= create_item(key, keylen, NULL, sizeof(value), flags, (time_t)expiration);
  if (item == NULL)
  {
    rval= PROTOCOL_BINARY_RESPONSE_ENOMEM;
  }
  else
  {
    memcpy(item->data, &value, sizeof(value));
    put_item(item);
  }

  response.message.header.response.status= htons(rval);
  if (rval == PROTOCOL_BINARY_RESPONSE_SUCCESS)
  {
    response.message.header.response.bodylen= ntohl(8);
    response.message.body.value= ntohll((*(uint64_t*)item->data));
    response.message.header.response.cas= ntohll(item->cas);

    release_item(item);
    if (header->request.opcode == PROTOCOL_BINARY_CMD_INCREMENTQ ||
        header->request.opcode == PROTOCOL_BINARY_CMD_DECREMENTQ)
    {
      return PROTOCOL_BINARY_RESPONSE_SUCCESS;
    }
  }

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status version_command_handler(const void *cookie,
                                                               protocol_binary_request_header *header,
                                                               memcached_binary_protocol_raw_response_handler response_handler)
{
  const char *versionstring= "1.0.0";
  union {
    protocol_binary_response_header packet;
    char buffer[256];
  } response= {
    .packet.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= PROTOCOL_BINARY_CMD_VERSION,
      .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
      .opaque= header->request.opaque,
      .bodylen= htonl((uint32_t)strlen(versionstring))
    }
  };

  memcpy(response.buffer + sizeof(response.packet), versionstring, strlen(versionstring));

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status concat_command_handler(const void *cookie,
                                                              protocol_binary_request_header *header,
                                                              memcached_binary_protocol_raw_response_handler response_handler)
{
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;
  uint16_t keylen= ntohs(header->request.keylen);
  uint64_t cas= ntohll(header->request.cas);
  void *key= header + 1;
  uint32_t vallen= ntohl(header->request.bodylen) - keylen;
  void *val= (char*)key + keylen;

  struct item *item= get_item(key, keylen);
  struct item *nitem= NULL;

  if (item == NULL)
  {
    rval= PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
  }
  else if (cas != 0 && cas != item->cas)
  {
    rval= PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
  }
  else if ((nitem= create_item(key, keylen, NULL, item->size + vallen,
                               item->flags, item->exp)) == NULL)
  {
    release_item(item);
    rval= PROTOCOL_BINARY_RESPONSE_ENOMEM;
  }
  else
  {
    if (header->request.opcode == PROTOCOL_BINARY_CMD_APPEND ||
        header->request.opcode == PROTOCOL_BINARY_CMD_APPENDQ)
    {
      memcpy(nitem->data, item->data, item->size);
      memcpy(((char*)(nitem->data)) + item->size, val, vallen);
    }
    else
    {
      memcpy(nitem->data, val, vallen);
      memcpy(((char*)(nitem->data)) + vallen, item->data, item->size);
    }
    release_item(item);
    delete_item(key, keylen);
    put_item(nitem);
    cas= nitem->cas;
    release_item(nitem);

    if (header->request.opcode == PROTOCOL_BINARY_CMD_APPEND ||
        header->request.opcode == PROTOCOL_BINARY_CMD_PREPEND)
    {
      protocol_binary_response_no_extras response= {
        .message= {
          .header.response= {
            .magic= PROTOCOL_BINARY_RES,
            .opcode= header->request.opcode,
            .status= htons(rval),
            .opaque= header->request.opaque,
            .cas= htonll(cas),
          }
        }
      };
      return response_handler(cookie, header, (void*)&response);
    }
  }

  return rval;
}

static protocol_binary_response_status set_command_handler(const void *cookie,
                                                           protocol_binary_request_header *header,
                                                           memcached_binary_protocol_raw_response_handler response_handler)
{
  size_t keylen= ntohs(header->request.keylen);
  size_t datalen= ntohl(header->request.bodylen) - keylen - 8;
  protocol_binary_request_replace *request= (void*)header;
  uint32_t flags= ntohl(request->message.body.flags);
  time_t timeout= (time_t)ntohl(request->message.body.expiration);
  char *key= ((char*)header) + sizeof(*header) + 8;
  char *data= key + keylen;

  protocol_binary_response_no_extras response= {
    .message= {
      .header.response= {
        .magic= PROTOCOL_BINARY_RES,
        .opcode= header->request.opcode,
        .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
        .opaque= header->request.opaque
      }
    }
  };

  if (header->request.cas != 0)
  {
    /* validate cas */
    struct item* item= get_item(key, keylen);
    if (item != NULL)
    {
      if (item->cas != ntohll(header->request.cas))
      {
        release_item(item);
        response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS);
        return response_handler(cookie, header, (void*)&response);
      }
      release_item(item);
    }
  }

  delete_item(key, keylen);
  struct item* item= create_item(key, keylen, data, datalen, flags, timeout);
  if (item == NULL)
  {
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_ENOMEM);
  }
  else
  {
    put_item(item);
    /* SETQ shouldn't return a message */
    if (header->request.opcode == PROTOCOL_BINARY_CMD_SET)
    {
      response.message.header.response.cas= htonll(item->cas);
      release_item(item);
      return response_handler(cookie, header, (void*)&response);
    }
    release_item(item);

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
  }

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status add_command_handler(const void *cookie,
                                                           protocol_binary_request_header *header,
                                                           memcached_binary_protocol_raw_response_handler response_handler)
{
  size_t keylen= ntohs(header->request.keylen);
  size_t datalen= ntohl(header->request.bodylen) - keylen - 8;
  protocol_binary_request_add *request= (void*)header;
  uint32_t flags= ntohl(request->message.body.flags);
  time_t timeout= (time_t)ntohl(request->message.body.expiration);
  char *key= ((char*)header) + sizeof(*header) + 8;
  char *data= key + keylen;

  protocol_binary_response_no_extras response= {
    .message= {
      .header.response= {
        .magic= PROTOCOL_BINARY_RES,
        .opcode= header->request.opcode,
        .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
        .opaque= header->request.opaque
      }
    }
  };

  struct item* item= get_item(key, keylen);
  if (item == NULL)
  {
    item= create_item(key, keylen, data, datalen, flags, timeout);
    if (item == NULL)
      response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_ENOMEM);
    else
    {
      put_item(item);
      /* ADDQ shouldn't return a message */
      if (header->request.opcode == PROTOCOL_BINARY_CMD_ADD)
      {
        response.message.header.response.cas= htonll(item->cas);
        release_item(item);
        return response_handler(cookie, header, (void*)&response);
      }
      release_item(item);
      return PROTOCOL_BINARY_RESPONSE_SUCCESS;
    }
  }
  else
  {
    release_item(item);
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS);
  }

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status replace_command_handler(const void *cookie,
                                                               protocol_binary_request_header *header,
                                                               memcached_binary_protocol_raw_response_handler response_handler)
{
  size_t keylen= ntohs(header->request.keylen);
  size_t datalen= ntohl(header->request.bodylen) - keylen - 8;
  protocol_binary_request_replace *request= (void*)header;
  uint32_t flags= ntohl(request->message.body.flags);
  time_t timeout= (time_t)ntohl(request->message.body.expiration);
  char *key= ((char*)header) + sizeof(*header) + 8;
  char *data= key + keylen;

  protocol_binary_response_no_extras response= {
    .message= {
      .header.response= {
        .magic= PROTOCOL_BINARY_RES,
        .opcode= header->request.opcode,
        .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
        .opaque= header->request.opaque
      }
    }
  };

  struct item* item= get_item(key, keylen);
  if (item == NULL)
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
  else if (header->request.cas == 0 || ntohll(header->request.cas) == item->cas)
  {
    release_item(item);
    delete_item(key, keylen);
    item= create_item(key, keylen, data, datalen, flags, timeout);
    if (item == NULL)
      response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_ENOMEM);
    else
    {
      put_item(item);
      /* REPLACEQ shouldn't return a message */
      if (header->request.opcode == PROTOCOL_BINARY_CMD_REPLACE)
      {
        response.message.header.response.cas= htonll(item->cas);
        release_item(item);
        return response_handler(cookie, header, (void*)&response);
      }
      release_item(item);
      return PROTOCOL_BINARY_RESPONSE_SUCCESS;
    }
  }
  else
  {
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS);
    release_item(item);
  }

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status stat_command_handler(const void *cookie,
                                                            protocol_binary_request_header *header,
                                                            memcached_binary_protocol_raw_response_handler response_handler)
{
  /* Just send the terminating packet*/
  protocol_binary_response_no_extras response= {
    .message= {
      .header.response= {
        .magic= PROTOCOL_BINARY_RES,
        .opcode= PROTOCOL_BINARY_CMD_STAT,
        .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
        .opaque= header->request.opaque
      }
    }
  };

  return response_handler(cookie, header, (void*)&response);
}

memcached_binary_protocol_callback_st interface_v0_impl= {
  .interface_version= MEMCACHED_PROTOCOL_HANDLER_V0,
};

static void initialize_interface_v0_handler(void)
{
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_GET]= get_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_SET]= set_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_ADD]= add_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_REPLACE]= replace_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_DELETE]= delete_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_INCREMENT]= arithmetic_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_DECREMENT]= arithmetic_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_QUIT]= quit_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_FLUSH]= flush_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_GETQ]= get_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_NOOP]= noop_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_VERSION]= version_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_GETK]= get_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_GETKQ]= get_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_APPEND]= concat_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_PREPEND]= concat_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_STAT]= stat_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_SETQ]= set_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_ADDQ]= add_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_REPLACEQ]= replace_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_DELETEQ]= delete_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_INCREMENTQ]= arithmetic_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_DECREMENTQ]= arithmetic_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_QUITQ]= quit_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_FLUSHQ]= flush_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_APPENDQ]= concat_command_handler;
  interface_v0_impl.interface.v0.comcode[PROTOCOL_BINARY_CMD_PREPENDQ]= concat_command_handler;
}

int main (int argc, char* argv[])
{
  int opt;
  const char *server_port= "4242";
  memcached_binary_protocol_callback_st *interface= &interface_v0_impl;

  initialize_interface_v0_handler();

  while ((opt= getopt(argc, argv, "h?Vvp:")) != EOF)
  {
    switch (opt)
    {
    case 'v':
      verbose= true;
      break;
    case 'V':
      printf("%s Version 0.1\n", argv[0]);
      return 0;
    case 'p':
      server_port= optarg;
      break;
    case 'h':
    case '?':
    default:
      help(argv[0]);
      return 1;
    }
  }

  printf("Cloud Vapour\n");
  printf("------------\n\n");
  printf("Port %s\n", server_port);

  server_socket(server_port);

  printf("\nConnecting to NDB...");
  init_ndb();
  printf("Done\n");

  if (num_server_sockets == 0)
  {
    fprintf(stderr, "ERROR: Could not open any server sockets.\n\n");
    return 1;
  }

  interface->pre_execute= pre_execute;
  interface->post_execute= post_execute;
  interface->unknown= unknown;

  struct memcached_protocol_st *protocol_handle;
  if ((protocol_handle= memcached_protocol_create_instance()) == NULL)
  {
    fprintf(stderr, "Failed to allocate protocol handle\n");
    return 1;
  }

  memcached_binary_protocol_set_callbacks(protocol_handle, interface);
  memcached_binary_protocol_set_pedantic(protocol_handle, true);

  for (int xx= 0; xx < num_server_sockets; ++xx)
    socket_userdata_map[server_sockets[xx]]= protocol_handle;

  work();
  // never reached

  cleanup_ndb();
  return 0;
}

static void work(void)
{
#define MAX_SERVERS_TO_POLL 100
  struct pollfd fds[MAX_SERVERS_TO_POLL];
  int max_poll;

  for (max_poll= 0; max_poll < num_server_sockets; ++max_poll)
  {
    fds[max_poll].events= POLLIN;
    fds[max_poll].revents= 0;
    fds[max_poll].fd= server_sockets[max_poll];
  }

  while (true)
  {
    int err= poll(fds, (nfds_t)max_poll, -1);

    if (err == 0 || (err == -1 && errno != EINTR))
    {
      perror("poll() failed");
      abort();
    }

    /* find the available filedescriptors */
    for (int x= max_poll - 1; x > -1 && err > 0; --x)
    {
      if (fds[x].revents != 0)
      {
        --err;
        if (x < num_server_sockets)
        {
          /* accept new client */
          struct sockaddr_storage addr;
          socklen_t addrlen= sizeof(addr);
          int sock= accept(fds[x].fd, (struct sockaddr *)&addr,
                           &addrlen);

          if (sock == -1)
          {
            perror("Failed to accept client");
            continue;
          }
          struct memcached_protocol_st *protocol;
          protocol= socket_userdata_map[fds[x].fd];

          struct memcached_protocol_client_st* c;
          c= memcached_protocol_create_client(protocol, sock);
          if (c == NULL)
          {
            fprintf(stderr, "Failed to create client\n");
            close(sock);
          }
          else
          {
            socket_userdata_map[sock]= c;
            fds[max_poll].events= POLLIN;
            fds[max_poll].revents= 0;
            fds[max_poll].fd= sock;
            ++max_poll;
          }
        }
        else
        {
          /* drive the client */
          struct memcached_protocol_client_st* c;
          c= socket_userdata_map[fds[x].fd];
          assert(c != NULL);
          fds[max_poll].events= 0;

          memcached_protocol_event_t events= memcached_protocol_client_work(c);
          if (events & MEMCACHED_PROTOCOL_WRITE_EVENT)
            fds[max_poll].events= POLLOUT;

          if (events & MEMCACHED_PROTOCOL_READ_EVENT)
            fds[max_poll].events= POLLIN;

          if (!(events & MEMCACHED_PROTOCOL_PAUSE_EVENT ||
                fds[max_poll].events != 0))
          {
            memcached_protocol_client_destroy(c);
            close(fds[x].fd);
            fds[x].events= 0;

            if (x != max_poll - 1)
              memmove(fds + x, fds + x + 1, (size_t)(max_poll - x));

            --max_poll;
          }
        }
      }
    }
  }
}
