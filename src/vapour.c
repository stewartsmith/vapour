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
#include <stdio.h>
#include <poll.h>

#include <unistd.h>

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

  while ((opt= getopt(argc, argv, "h?V")) != EOF)
  {
    switch (opt)
    {
    case 'V':
      printf("%s Version 0.1\n", argv[0]);
      return 0;
    case 'h':
    case '?':
    default:
      help(argv[0]);
      return 1;
    }
  }

  printf("Cloud Vapour\n");

  return 0;
}
