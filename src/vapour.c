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
#include <poll.h>

#include <unistd.h>

#include <libmemcached/protocol_handler.h>

static int server_sockets[1024];
static int num_server_sockets= 0;

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

int main (int argc, char* argv[])
{
  int opt;
  char *server_port= "4242";

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

  return 0;
}
