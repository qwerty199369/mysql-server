/* Copyright (c) 2005, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include "sql/rpl_tblmap.h"

#include <stddef.h>
#include <unordered_map>
#include <utility>

#ifdef MYSQL_SERVER
#include "sql/table.h"   // TABLE
#else
#include "sql/log_event.h" // Table_map_log_event
#endif
#include "lex_string.h"
#include "my_dbug.h"
#include "my_sys.h"
#include "mysql/psi/psi_base.h"
#include "sql/psi_memory_key.h"

#ifndef MYSQL_SERVER
#define MAYBE_TABLE_NAME(T) ("")
#else
#define MAYBE_TABLE_NAME(T) ((T) ? (T)->s->table_name.str : "<>")
#endif
#define TABLE_ID_HASH_SIZE 32
#define TABLE_ID_CHUNK 256

#ifndef MYSQL_SERVER
static const PSI_memory_key table_psi_key= PSI_NOT_INSTRUMENTED;
#else
static const PSI_memory_key table_psi_key= key_memory_table_mapping_root;
#endif

table_mapping::table_mapping()
  : m_free(0),
    m_table_ids(table_psi_key)
{
  /* We don't preallocate any block, this is consistent with m_free=0 above */
  init_alloc_root(table_psi_key,
                  &m_mem_root, TABLE_ID_HASH_SIZE*sizeof(entry), 0);
}

table_mapping::~table_mapping()
{
#ifndef MYSQL_SERVER
  clear_tables();
#endif
}

Mapped_table* table_mapping::get_table(ulonglong table_id)
{
  DBUG_ENTER("table_mapping::get_table(ulonglong)");
  DBUG_PRINT("enter", ("table_id: %llu", table_id));
  auto it= m_table_ids.find(table_id);
  if (it != m_table_ids.end())
  {
    entry *e= it->second;
    DBUG_PRINT("info", ("tid %llu -> table %p (%s)",
			table_id, e->table,
			MAYBE_TABLE_NAME(e->table)));
    DBUG_RETURN(e->table);
  }

  DBUG_PRINT("info", ("tid %llu is not mapped!", table_id));
  DBUG_RETURN(NULL);
}

/*
  Called when we are out of table id entries. Creates TABLE_ID_CHUNK
  new entries, chain them and attach them at the head of the list of free
  (free for use) entries.
*/
int table_mapping::expand()
{
  /*
    If we wanted to use "tmp= new (&m_mem_root) entry[TABLE_ID_CHUNK]",
    we would have to make "entry" derive from Sql_alloc but then it would not
    be a POD anymore and we want it to be (see rpl_tblmap.h). So we allocate
    in C.
  */
  entry *tmp= (entry *)alloc_root(&m_mem_root, TABLE_ID_CHUNK*sizeof(entry));
  if (tmp == NULL)
    return ERR_MEMORY_ALLOCATION; // Memory allocation failed

  /* Find the end of this fresh new array of free entries */
  entry *e_end= tmp+TABLE_ID_CHUNK-1;
  for (entry *e= tmp; e < e_end; e++)
    e->next= e+1;
  e_end->next= m_free;
  m_free= tmp;
  return 0;
}

int table_mapping::set_table(ulonglong table_id, Mapped_table* table)
{
  DBUG_ENTER("table_mapping::set_table(ulonglong, Mapped_table*)");
  DBUG_PRINT("enter", ("table_id: %llu  table: %p (%s)",
		       table_id, 
		       table, MAYBE_TABLE_NAME(table)));
  entry *e;
  auto it= m_table_ids.find(table_id);
  if (it == m_table_ids.end())
  {
    if (m_free == 0 && expand())
      DBUG_RETURN(ERR_MEMORY_ALLOCATION); // Memory allocation failed      
    e= m_free;
    m_free= m_free->next;
  }
  else
  {
    e= it->second;
#ifndef MYSQL_SERVER
    delete e->table;
#endif
    m_table_ids.erase(table_id);
  }
  e->table_id= table_id;
  e->table= table;
  m_table_ids.emplace(table_id, e);

  DBUG_PRINT("info", ("tid %llu -> table %p (%s)",
		      table_id, e->table,
		      MAYBE_TABLE_NAME(e->table)));
  DBUG_RETURN(0);		// All OK
}

int table_mapping::remove_table(ulonglong table_id)
{
  auto it= m_table_ids.find(table_id);
  if (it != m_table_ids.end())
  {
    /* we add this entry to the chain of free (free for use) entries */
    it->second->next= m_free;
    m_free= it->second;
    m_table_ids.erase(it);
    return 0;			// All OK
  }
  return 1;			// No table to remove
}

/*
  Puts all entries into the list of free-for-use entries (does not free any
  memory), and empties the hash.
*/
void table_mapping::clear_tables()
{
  DBUG_ENTER("table_mapping::clear_tables()");
  for (const auto &key_and_value : m_table_ids)
  {
    entry *e= key_and_value.second;
#ifndef MYSQL_SERVER
    delete e->table;
#endif
    e->next= m_free;
    m_free= e;
  }
  m_table_ids.clear();
  DBUG_VOID_RETURN;
}
