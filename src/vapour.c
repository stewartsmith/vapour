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
*/

#include "config.h"
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include <unistd.h>

#include <libmemcached/protocol_handler.h>

static int server_sockets[1024];
static int num_server_sockets= 0;
static void* socket_userdata_map[1024];

static void work(void);

#ifdef linux
/* remove optimisation that produces gcc warnings */
#undef ntohs
#undef ntohl
#undef htons
#undef htonl

#endif

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

static protocol_binary_response_status add_handler(const void *cookie,
                                                   const void *key,
                                                   uint16_t keylen,
                                                   const void *data,
                                                   uint32_t datalen,
                                                   uint32_t flags,
                                                   uint32_t exptime,
                                                   uint64_t *cas)
{
  (void)cookie;
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;

  (void)key;
  (void)keylen;
  (void)data;
  (void)datalen;
  (void)flags;
  (void)exptime;
  (void)cas;

/*  struct item* item= get_item(key, keylen);
  if (item == NULL)
  {
    item= create_item(key, keylen, data, datalen, flags, (time_t)exptime);
    if (item == 0)
    {
      rval= PROTOCOL_BINARY_RESPONSE_ENOMEM;
    }
    else
    {
      put_item(item);
      *cas= item->cas;
      release_item(item);
    }
  }
  else
  {
    rval= PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
  }
*/
  return rval;
}


static protocol_binary_response_status append_handler(const void *cookie,
                                                      const void *key,
                                                      uint16_t keylen,
                                                      const void* val,
                                                      uint32_t vallen,
                                                      uint64_t cas,
                                                      uint64_t *result_cas)
{
  (void)cookie;
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;
  (void)key;
  (void)keylen;
  (void)val;
  (void)vallen;
  (void)cas;
  (void)result_cas;
/*
  struct item *item= get_item(key, keylen);
  struct item *nitem;

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
    memcpy(nitem->data, item->data, item->size);
    memcpy(((char*)(nitem->data)) + item->size, val, vallen);
    release_item(item);
    delete_item(key, keylen);
    put_item(nitem);
    *result_cas= nitem->cas;
    release_item(nitem);
  }
*/
  return rval;
}

static protocol_binary_response_status decrement_handler(const void *cookie,
                                                         const void *key,
                                                         uint16_t keylen,
                                                         uint64_t delta,
                                                         uint64_t initial,
                                                         uint32_t expiration,
                                                         uint64_t *result,
                                                         uint64_t *result_cas) {
  (void)cookie;
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;

  (void)key;
  (void)keylen;
  (void)delta;
  (void)initial;
  (void)expiration;
  (void)result;
  (void)result_cas;
/*
  uint64_t val= initial;
  struct item *item= get_item(key, keylen);

  if (item != NULL)
  {
    if (delta > *(uint64_t*)item->data)
      val= 0;
    else
      val= *(uint64_t*)item->data - delta;

    expiration= (uint32_t)item->exp;
    release_item(item);
    delete_item(key, keylen);
  }

  item= create_item(key, keylen, NULL, sizeof(initial), 0, (time_t)expiration);
  if (item == 0)
  {
    rval= PROTOCOL_BINARY_RESPONSE_ENOMEM;
  }
  else
  {
    memcpy(item->data, &val, sizeof(val));
    put_item(item);
    *result= val;
    *result_cas= item->cas;
    release_item(item);
  }
*/
  return rval;
}


static protocol_binary_response_status delete_handler(const void *cookie,
                                                      const void *key,
                                                      uint16_t keylen,
                                                      uint64_t cas) {
  (void)cookie;
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;
  (void)key;
  (void)keylen;
  (void)cas;
/*
  if (cas != 0)
  {
    struct item *item= get_item(key, keylen);
    if (item != NULL)
    {
      if (item->cas != cas)
      {
        release_item(item);
        return PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
      }
      release_item(item);
    }
  }

  if (!delete_item(key, keylen))
  {
    rval= PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
  }
*/
  return rval;
}

static protocol_binary_response_status flush_handler(const void *cookie,
                                                     uint32_t when) {

  (void)cookie;
  (void)when;
//  flush(when);
  return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_handler(const void *cookie,
                                                   const void *key,
                                                   uint16_t keylen,
                                                   memcached_binary_protocol_get_response_handler response_handler) {
  struct item *item= NULL; //get_item(key, keylen);
  (void)cookie;
  (void)key;
  (void)keylen;
  (void)response_handler;

  if (item == NULL)
  {
    return PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
  }
  protocol_binary_response_status rc= PROTOCOL_BINARY_RESPONSE_SUCCESS;
/*
  rc= response_handler(cookie, key, (uint16_t)keylen,
                          item->data, (uint32_t)item->size, item->flags,
                          item->cas);
  release_item(item);
*/
  return rc;
}


static protocol_binary_response_status increment_handler(const void *cookie,
                                                         const void *key,
                                                         uint16_t keylen,
                                                         uint64_t delta,
                                                         uint64_t initial,
                                                         uint32_t expiration,
                                                         uint64_t *result,
                                                         uint64_t *result_cas) {
  (void)cookie;
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;
//  uint64_t val= initial;

  (void)key;
  (void)keylen;
  (void)delta;
  (void)initial;
  (void)expiration;
  (void)result;
  (void)result_cas;

/*  struct item *item= get_item(key, keylen);

  if (item != NULL)
  {
    val= (*(uint64_t*)item->data) + delta;
    expiration= (uint32_t)item->exp;
    release_item(item);
    delete_item(key, keylen);
  }

  item= create_item(key, keylen, NULL, sizeof(initial), 0, (time_t)expiration);
  if (item == NULL)
  {
    rval= PROTOCOL_BINARY_RESPONSE_ENOMEM;
  }
  else
  {
    char buffer[1024] = {0};
    memcpy(buffer, key, keylen);
    memcpy(item->data, &val, sizeof(val));
    put_item(item);
    *result= val;
    *result_cas= item->cas;
    release_item(item);
  }
*/
  return rval;
}


static protocol_binary_response_status prepend_handler(const void *cookie,
                                                       const void *key,
                                                       uint16_t keylen,
                                                       const void* val,
                                                       uint32_t vallen,
                                                       uint64_t cas,
                                                       uint64_t *result_cas) {
  (void)cookie;
//  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;

  (void)key;
  (void)keylen;
  (void)val;
  (void)vallen;
  (void)cas;
  (void)result_cas;

/*  struct item *item= get_item(key, keylen);
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
    rval= PROTOCOL_BINARY_RESPONSE_ENOMEM;
  }
  else
  {
    memcpy(nitem->data, val, vallen);
    memcpy(((char*)(nitem->data)) + vallen, item->data, item->size);
    release_item(item);
    item= NULL;
    delete_item(key, keylen);
    put_item(nitem);
    *result_cas= nitem->cas;
  }

  if (item)
    release_item(item);

  if (nitem)
    release_item(nitem);
*/
  return rval;
}

static protocol_binary_response_status quit_handler(const void *cookie) {
  (void)cookie;
  return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}


static protocol_binary_response_status replace_handler(const void *cookie,
                                                       const void *key,
                                                       uint16_t keylen,
                                                       const void* data,
                                                       uint32_t datalen,
                                                       uint32_t flags,
                                                       uint32_t exptime,
                                                       uint64_t cas,
                                                       uint64_t *result_cas) {
  (void)cookie;
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;
  struct item* item= NULL; //get_item(key, keylen);

  (void)key;
  (void)keylen;
  (void)data;
  (void)datalen;
  (void)flags;
  (void)exptime;
  (void)cas;
  (void)result_cas;

  if (item == NULL)
  {
    rval= PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
  }
/*  else if (cas == 0 || cas == item->cas)
  {
    release_item(item);
    delete_item(key, keylen);
    item= create_item(key, keylen, data, datalen, flags, (time_t)exptime);
    if (item == 0)
    {
      rval= PROTOCOL_BINARY_RESPONSE_ENOMEM;
    }
    else
    {
      put_item(item);
      *result_cas= item->cas;
      release_item(item);
    }
  }
  else
  {
    rval= PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
    release_item(item);
  }
*/
  return rval;
}

static protocol_binary_response_status set_handler(const void *cookie,
                                                   const void *key,
                                                   uint16_t keylen,
                                                   const void* data,
                                                   uint32_t datalen,
                                                   uint32_t flags,
                                                   uint32_t exptime,
                                                   uint64_t cas,
                                                   uint64_t *result_cas) {
  (void)cookie;
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;
  (void)key;
  (void)keylen;
  (void)data;
  (void)datalen;
  (void)flags;
  (void)exptime;
  (void)cas;
  (void)result_cas;

/*
  if (cas != 0)
  {
    struct item* item= get_item(key, keylen);
    if (item != NULL && cas != item->cas)
    {
      // Invalid CAS value
      release_item(item);
      return PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
    }
  }

  delete_item(key, keylen);
  struct item* item= create_item(key, keylen, data, datalen, flags, (time_t)exptime);
  if (item == 0)
  {
    rval= PROTOCOL_BINARY_RESPONSE_ENOMEM;
  }
  else
  {
    put_item(item);
    *result_cas= item->cas;
    release_item(item);
  }
*/
  return rval;
}


static protocol_binary_response_status stat_handler(const void *cookie,
                                                    const void *key,
                                                    uint16_t keylen,
                                                    memcached_binary_protocol_stat_response_handler response_handler) {
  (void)key;
  (void)keylen;
  /* Just return an empty packet */
  return response_handler(cookie, NULL, 0, NULL, 0);
}

static protocol_binary_response_status version_handler(const void *cookie,
                                                       memcached_binary_protocol_version_response_handler response_handler) {
  const char *version= "0.1";
  return response_handler(cookie, version, (uint32_t)strlen(version));
}

static protocol_binary_response_status noop_handler(const void *cookie) {
  (void)cookie;
  return PROTOCOL_BINARY_RESPONSE_SUCCESS;
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


memcached_binary_protocol_callback_st interface_v1_impl= {
  .interface_version= MEMCACHED_PROTOCOL_HANDLER_V1,
  .interface.v1= {
    .add= add_handler,
    .append= append_handler,
    .decrement= decrement_handler,
    .delete= delete_handler,
    .flush= flush_handler,
    .get= get_handler,
    .increment= increment_handler,
    .noop= noop_handler,
    .prepend= prepend_handler,
    .quit= quit_handler,
    .replace= replace_handler,
    .set= set_handler,
    .stat= stat_handler,
    .version= version_handler
  }
};
static void pre_execute(const void *cookie __attribute__((unused)),
                        protocol_binary_request_header *header __attribute__((unused)))
{
}

/**
 * Print out the command we just executed
 */
static void post_execute(const void *cookie __attribute__((unused)),
                         protocol_binary_request_header *header __attribute__((unused)))
{
}

int main (int argc, char* argv[])
{
  int opt;
  const char *server_port= "4242";

  while ((opt= getopt(argc, argv, "h?Vp:")) != EOF)
  {
    switch (opt)
    {
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

  if (num_server_sockets == 0)
  {
    fprintf(stderr, "ERROR: Could not open any server sockets.\n\n");
    return 1;
  }

  struct memcached_protocol_st *protocol_handle;
  if ((protocol_handle= memcached_protocol_create_instance()) == NULL)
  {
    fprintf(stderr, "Failed to allocate protocol handle\n");
    return 1;
  }

  interface_v1_impl.pre_execute= pre_execute;
  interface_v1_impl.post_execute= post_execute;
  interface_v1_impl.unknown= unknown;

  memcached_binary_protocol_set_callbacks(protocol_handle, &interface_v1_impl);
  memcached_binary_protocol_set_pedantic(protocol_handle, true);

  for (int xx= 0; xx < num_server_sockets; ++xx)
    socket_userdata_map[server_sockets[xx]]= protocol_handle;

  work();

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
