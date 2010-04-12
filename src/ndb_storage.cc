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

struct item* create_item(const void* key, size_t nkey, const void* data,
                         size_t size, uint32_t flags, time_t exp)
{
  struct item* ret= static_cast<struct item*>(calloc(1, sizeof(*ret)));
  if (ret != NULL)
  {
    ret->cas= 1;
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
  free(item);
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

static bool decode_key(char* key, char **db, char **table, char **col, char **val_column, char **key_value)
{
  *db= key;
  *table= strchr(key, '/');
  if (*table == NULL)
    return false;

  **table= '\0';
  (*table)++;

  *col= strchr(*table, '/');
  **col='\0';
  (*col)++;

  *val_column= strchr(*col, '/');
  **val_column='\0';
  (*val_column)++;

  *key_value= strchr(*val_column, '/');
  **key_value='\0';
  (*key_value)++;

  return true;
}
void set_ndb_op(const NdbDictionary::Column* ndb_column, NdbOperation *myOperation, char* key_value);
void make_ndb_varchar(char *buffer, char *str);

void make_ndb_varchar(char *buffer, char *str)
{
  size_t len = strlen(str);
  int hlen = (len > 255) ? 2 : 1;
  buffer[0] = char(len & 0xff);
  if( len > 255 )
    buffer[1] = char(len / 256);
  strcpy(buffer+hlen, str);
}

void set_ndb_op(const NdbDictionary::Column* ndb_column, NdbOperation *myOperation, char* key_value)
{
  switch(ndb_column->getType())
  {
  case NdbDictionary::Column::Tinyint:
  case NdbDictionary::Column::Tinyunsigned:
  case NdbDictionary::Column::Smallint:
  case NdbDictionary::Column::Smallunsigned:
  case NdbDictionary::Column::Mediumint:
  case NdbDictionary::Column::Mediumunsigned:
  case NdbDictionary::Column::Int:
  case NdbDictionary::Column::Unsigned:
    myOperation->equal(ndb_column->getName(), atoi(static_cast<const char*>(key_value)));
    break;
  case NdbDictionary::Column::Bigint:
  case NdbDictionary::Column::Bigunsigned:
    myOperation->equal(ndb_column->getName(), strtoll(static_cast<const char*>(key_value), NULL, 0));
    break;
  case NdbDictionary::Column::Varchar:
  case NdbDictionary::Column::Longvarchar:
  {
    char *ndbchar= static_cast<char*>(malloc(strlen(key_value)+2));
    make_ndb_varchar(ndbchar, key_value);
    myOperation->equal(ndb_column->getName(), ndbchar);
    free(ndbchar);
  }
    break;
  default:
    break;
  }
}

struct item* get_item(const void* key, size_t nkey)
{
  char *keycopy= static_cast<char*>(malloc(nkey));
  char *db;
  char *table;
  char *col;
  char *val_column;
  char *key_value;
  memcpy(keycopy, key, nkey);

  if (! decode_key(keycopy, &db, &table, &col, &val_column, &key_value))
  {
    free(keycopy);
    return NULL;
  }

  Ndb myNdb( ndb_connection, db );
  if (myNdb.init())
    FATAL_NDBAPI_ERROR(myNdb.getNdbError());

  NdbDictionary::Dictionary* myDict= myNdb.getDictionary();
retry:
  const NdbDictionary::Table *myTable= myDict->getTable(table);

  if (myTable == NULL)
    FATAL_NDBAPI_ERROR(myDict->getNdbError());

  NdbTransaction *myTransaction= myNdb.startTransaction();
  if (myTransaction == NULL) FATAL_NDBAPI_ERROR(myNdb.getNdbError());

  NdbOperation *myOperation= myTransaction->getNdbOperation(myTable);
  if (myOperation == NULL) FATAL_NDBAPI_ERROR(myTransaction->getNdbError());

  const NdbDictionary::Column* ndb_column= myTable->getColumn(col);

  myOperation->readTuple(NdbOperation::LM_Read);

  set_ndb_op(ndb_column, myOperation, key_value);


  NdbRecAttr *myRecAttr= myOperation->getValue(val_column, NULL);
  if (myRecAttr == NULL) FATAL_NDBAPI_ERROR(myTransaction->getNdbError());

  if(myTransaction->execute( NdbTransaction::Commit ) == -1)
    FATAL_NDBAPI_ERROR(myTransaction->getNdbError());

  if (myTransaction->getNdbError().classification == NdbError::NoDataFound)
  {
    myNdb.closeTransaction(myTransaction);
    free(keycopy);
    return NULL;
  }
  if (myTransaction->getNdbError().classification == NdbError::SchemaError)
  {
    myDict->invalidateTable(table);
    myDict->removeCachedTable(table);
    goto retry;
  }

  const NdbDictionary::Column* get_ndb_column= myTable->getColumn(val_column);

  char *buf;
  char numberbuf[50];
  size_t bufsz= 0;

  switch(get_ndb_column->getType())
  {
  case NdbDictionary::Column::Tinyint:
  case NdbDictionary::Column::Tinyunsigned:
  case NdbDictionary::Column::Smallint:
  case NdbDictionary::Column::Smallunsigned:
  case NdbDictionary::Column::Mediumint:
  case NdbDictionary::Column::Mediumunsigned:
  case NdbDictionary::Column::Int:
  case NdbDictionary::Column::Unsigned:
    buf= numberbuf;
    snprintf(buf, sizeof(buf), "%2d\n", myRecAttr->u_32_value());
    break;
  case NdbDictionary::Column::Varchar:
    bufsz= size_t(*(myRecAttr->aRef()));
    buf= myRecAttr->aRef()+1;
    break;
  case NdbDictionary::Column::Longvarchar:
    bufsz= size_t(*(myRecAttr->aRef()));
    bufsz+= size_t(*(myRecAttr->aRef()+1)) * 256;
    buf= myRecAttr->aRef()+1;
    break;

  default:
    break;
  }

  struct item *ret_item= create_item(key, nkey, buf, strlen(buf)+1, 0, 0);

  myNdb.closeTransaction(myTransaction);
  free(keycopy);

  return ret_item;
}

bool delete_item(const void* key, size_t nkey)
{
  char *keycopy= static_cast<char*>(malloc(nkey));
  char *db;
  char *table;
  char *col;
  char *key_value;
  char *val_column;
  memcpy(keycopy, key, nkey);

  if (! decode_key(keycopy, &db, &table, &col, &val_column, &key_value))
  {
    free(keycopy);
    return NULL;
  }

  Ndb myNdb( ndb_connection, db );
  if (myNdb.init())
    FATAL_NDBAPI_ERROR(myNdb.getNdbError());

  NdbDictionary::Dictionary* myDict= myNdb.getDictionary();

retry:
  const NdbDictionary::Table *myTable= myDict->getTable(table);

  if (myTable == NULL)
    FATAL_NDBAPI_ERROR(myDict->getNdbError());

  NdbTransaction *myTransaction= myNdb.startTransaction();
  if (myTransaction == NULL) FATAL_NDBAPI_ERROR(myNdb.getNdbError());

  NdbOperation *myOperation= myTransaction->getNdbOperation(myTable);
  if (myOperation == NULL) FATAL_NDBAPI_ERROR(myTransaction->getNdbError());

  const NdbDictionary::Column* ndb_column= myTable->getColumn(col);

  myOperation->deleteTuple();
  set_ndb_op(ndb_column, myOperation, key_value);

  if(myTransaction->execute( NdbTransaction::Commit ) == -1)
    FATAL_NDBAPI_ERROR(myTransaction->getNdbError());

  if (myTransaction->getNdbError().classification == NdbError::SchemaError)
  {
    myDict->invalidateTable(table);
    myDict->removeCachedTable(table);
    goto retry;
  }

  if (myTransaction->getNdbError().classification == NdbError::NoDataFound)
  {
    myNdb.closeTransaction(myTransaction);
    free(keycopy);
    return false;
  }

  myNdb.closeTransaction(myTransaction);
  free(keycopy);

  return true;
}

void flush(uint32_t when __attribute__((unused)))
{
}

void put_item(struct item* item)
{
  (void)item;
}
