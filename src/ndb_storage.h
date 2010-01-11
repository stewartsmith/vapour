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

#ifndef NDB_STORAGE_H
#define NDB_STORAGE_H

#ifdef __cplusplus
extern "C"
#endif
int init_ndb(void);

#ifdef __cplusplus
extern "C"
#endif
int cleanup_ndb(void);

struct item {
  uint64_t cas;
  void* key;
  size_t nkey;
  void* data;
  size_t size;
  uint32_t flags;
  time_t exp;
};

#ifdef __cplusplus
extern "C"
#endif
struct item* get_item(const void* key, size_t nkey);

#ifdef __cplusplus
extern "C"
#endif
void release_item(struct item* item);


#endif
