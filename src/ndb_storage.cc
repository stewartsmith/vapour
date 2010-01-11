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
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <storage/ndb/ndbapi/NdbApi.hpp>
#include "ndb_storage.h"

#define FATAL_NDBAPI_ERROR(err) \
  fprintf(stderr, "Error in %s, line: %d, code: %d, msg: %s.\n", \
          __FILE__,  __LINE__, err.code, err.message);


Ndb_cluster_connection* ndb_connection;

static struct item* create_item(const void* key, size_t nkey, const void* data,
                         size_t size, uint32_t flags, time_t exp)
{
  struct item* ret= new item();
  if (ret != NULL)
  {
    ret->key= malloc(nkey);
    if (size > 0)
    {
      ret->data= malloc(size);
    }

    if (ret->key == NULL || (size > 0 && ret->data == NULL))
    {
      free(ret->key);
      free(ret->data);
      free(ret);
      return NULL;
    }

    memcpy(ret->key, key, nkey);
    if (data != NULL)
    {
      memcpy(ret->data, data, size);
    }

    ret->nkey= nkey;
    ret->size= size;
    ret->flags= flags;
    ret->exp= exp;
  }

  return ret;
}


void release_item(struct item* item)
{
  free(item->key);
  free(item->data);
  delete item;
}


int init_ndb()
{
  ndb_init();

  ndb_connection= new Ndb_cluster_connection();

  if( ndb_connection->connect(4, 5, 1) )
  {
    fprintf(stderr, "Unable to connect to cluster within 30 seconds.");
    exit(EXIT_FAILURE);
  }

  if(ndb_connection->wait_until_ready(30, 0) < 0)
  {
    fprintf(stderr, "Cluster was not ready within 30 seconds.\n");
    exit(EXIT_FAILURE);
  }

  return 0;
}

int cleanup_ndb()
{
  delete ndb_connection;

  ndb_end(0);

  return 0;
}

struct item* get_item(const void* key, size_t nkey)
{
  Ndb myNdb( ndb_connection, "test" );
  if (myNdb.init())
    FATAL_NDBAPI_ERROR(myNdb.getNdbError());

  const NdbDictionary::Dictionary* myDict= myNdb.getDictionary();
  const NdbDictionary::Table *myTable= myDict->getTable("t1");

  if (myTable == NULL)
    FATAL_NDBAPI_ERROR(myDict->getNdbError());

  NdbTransaction *myTransaction= myNdb.startTransaction();
  if (myTransaction == NULL) FATAL_NDBAPI_ERROR(myNdb.getNdbError());

  NdbOperation *myOperation= myTransaction->getNdbOperation(myTable);
  if (myOperation == NULL) FATAL_NDBAPI_ERROR(myTransaction->getNdbError());

  myOperation->readTuple(NdbOperation::LM_Read);
  myOperation->equal("a", 1);

  NdbRecAttr *myRecAttr= myOperation->getValue("b", NULL);
  if (myRecAttr == NULL) FATAL_NDBAPI_ERROR(myTransaction->getNdbError());

  if(myTransaction->execute( NdbTransaction::Commit ) == -1)
    FATAL_NDBAPI_ERROR(myTransaction->getNdbError());

  if (myTransaction->getNdbError().classification == NdbError::NoDataFound)
    abort();//return NULL; // FIXME

  char buf[50];
  snprintf(buf, sizeof(buf), "%2d", myRecAttr->u_32_value());

  printf("%s\n",buf);

  return create_item(key, nkey, buf, strlen(buf), 0, 0);
}
