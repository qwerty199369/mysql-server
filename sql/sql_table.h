/* Copyright (c) 2006, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef SQL_TABLE_INCLUDED
#define SQL_TABLE_INCLUDED

#include <map>
#include <stddef.h>
#include <sys/types.h>
#include <set>
#include <utility>
#include <vector>

#include "binary_log_types.h"  // enum_field_types
#include "dd/string_type.h"
#include "m_ctype.h"
#include "mdl.h"
#include "my_inttypes.h"
#include "my_sharedlib.h"
#include "mysql/components/services/mysql_mutex_bits.h"
#include "mysql/psi/mysql_mutex.h"

class Alter_info;
class Alter_table_ctx;
class Create_field;
class FOREIGN_KEY;
class KEY;
class THD;
class handler;
struct TABLE;
struct TABLE_LIST;
struct TABLE_SHARE;
struct handlerton;

namespace dd {
  class Schema;
  class Table;
  class Foreign_key;
}

typedef struct st_ha_check_opt HA_CHECK_OPT;
typedef struct st_ha_create_information HA_CREATE_INFO;
typedef struct charset_info_st CHARSET_INFO;
typedef struct st_mysql_mutex mysql_mutex_t;
template<typename T> class List;


enum enum_explain_filename_mode
{
  EXPLAIN_ALL_VERBOSE= 0,
  EXPLAIN_PARTITIONS_VERBOSE,
  EXPLAIN_PARTITIONS_AS_COMMENT
};

/* Maximum length of GEOM_POINT Field */
#define MAX_LEN_GEOM_POINT_FIELD   25

/* Flags for conversion functions. */
static const uint FN_FROM_IS_TMP=  1 << 0;
static const uint FN_TO_IS_TMP=    1 << 1;
static const uint FN_IS_TMP=       FN_FROM_IS_TMP | FN_TO_IS_TMP;
/** Don't check foreign key constraints while renaming table */
static const uint NO_FK_CHECKS=    1 << 2;
/**
  Don't commit transaction after updating data-dictionary while renaming
  the table.
*/
static const uint NO_DD_COMMIT=    1 << 3;


size_t filename_to_tablename(const char *from, char *to, size_t to_length
#ifndef DBUG_OFF
                           , bool stay_quiet = false
#endif /* DBUG_OFF */
                           );
size_t tablename_to_filename(const char *from, char *to, size_t to_length);
size_t build_table_filename(char *buff, size_t bufflen, const char *db,
                            const char *table, const char *ext,
                            uint flags, bool *was_truncated);
// For caller's who are mostly sure that path do not truncate
size_t inline build_table_filename(char *buff, size_t bufflen, const char *db,
                                   const char *table, const char *ext, uint flags)
{
    bool truncated_not_used;
    return build_table_filename(buff, bufflen, db, table, ext, flags,
                                &truncated_not_used);
}
size_t build_tmptable_filename(THD* thd, char *buff, size_t bufflen);
bool mysql_create_table(THD *thd, TABLE_LIST *create_table,
                        HA_CREATE_INFO *create_info,
                        Alter_info *alter_info);
bool mysql_create_table_no_lock(THD *thd, const char *db,
                                const char *table_name,
                                HA_CREATE_INFO *create_info,
                                Alter_info *alter_info,
                                uint select_field_count,
                                bool find_parent_keys,
                                bool *is_trans,
                                handlerton **post_ddl_ht);
bool mysql_discard_or_import_tablespace(THD *thd,
                                        TABLE_LIST *table_list);


/**
  Helper class for keeping track for which tables we need to invalidate
  data-dictionary cache entries and performing such invalidation.
*/
class Foreign_key_parents_invalidator
{
private:
  typedef std::map<std::pair<dd::String_type, dd::String_type>,
                   handlerton*> Parent_map;
  Parent_map m_parent_map;

public:
  void add(const char *db_name, const char *table_name,
           handlerton *hton);
  void invalidate(THD *thd);
  const Parent_map &parents() const { return m_parent_map; }
  bool is_empty() const { return m_parent_map.empty(); }
  void clear() { m_parent_map.clear(); }
};


/*
  Reload the foreign key parent information of the referenced
  tables and for the table itself.

  @param thd                 Thread handle.
  @param db                  Table schema name.
  @param name                Table name.
  @param reload_self         Reload FK parent info also for the
                             table itself.
  @param fk_invalidator      Object keeping track of which dd::Table
                             objects to invalidate. If submitted, use this
                             to restrict which FK parents should have their
                             FK parent information reloaded.

  @retval operation outcome, false if no error.
*/
bool
adjust_fk_parents(THD *thd, const char *db, const char *name, bool reload_self,
                  const Foreign_key_parents_invalidator *fk_invalidator);


/**
  Update the unique constraint name for the referencing tables.

  @param thd                  Thread handle.
  @param parent_table_db      Parent table schema name.
  @param parent_table_name    Parent table name.
  @param hton                 Handlerton for table's storage engine.
  @param parent_table_def     Table object representing the referenced table.
  @param invalidate_tdc       Indicates whether we need to invalidate TDC for
                              referencing tables after updating their
                              definitions.

  @retval operation outcome, false if no error.
*/
bool
adjust_fk_children_after_parent_def_change(THD *thd,
                                           const char *parent_table_db,
                                           const char *parent_table_name,
                                           handlerton *hton,
                                           const dd::Table *parent_table_def,
                                           bool invalidate_tdc)
                                           MY_ATTRIBUTE((warn_unused_result));


/**
  Update the unique constraint name for the referencing tables with
  mandatory TDC invalidation.
*/
inline bool
adjust_fk_children_after_parent_def_change(THD *thd,
                                           const char *parent_table_db,
                                           const char *parent_table_name,
                                           handlerton *hton,
                                           const dd::Table *parent_table_def)
{
  return adjust_fk_children_after_parent_def_change(thd,
                                                    parent_table_db,
                                                    parent_table_name,
                                                    hton, parent_table_def,
                                                    true);
}


/**
  Add MDL requests for exclusive lock on all tables referencing the given
  schema qualified table name to the list.

  @param          thd           Thread handle.
  @param          schema        Schema name.
  @param          table_name    Table name.
  @param          hton          Handlerton for table's storage engine.
  @param[in,out]  mdl_requests  List to which MDL requests are to be added.

  @retval operation outcome, false if no error.
*/
bool
collect_fk_children(THD *thd,
                    const char *schema,
                    const char *table_name,
                    handlerton *hton,
                    MDL_request_list *mdl_requests)
                    MY_ATTRIBUTE((warn_unused_result));


/**
  Add MDL requests for lock of specified type on tables referenced by the
  foreign keys to be added by the CREATE TABLE or ALTER TABLE operation.
  Also add the referenced table names to the foreign key invalidator,
  to be used at a later stage to invalidate the dd::Table objects.

  @param          thd             Thread handle.
  @param          db_name         Table's database name.
  @param          table_name      Table name.
  @param          alter_info      Alter_info object with the list of FKs
                                  to be added.
  @param          lock_type       Type of metadata lock to be requested.
  @param          hton            Handlerton for table's storage engine.
  @param[in,out]  mdl_requests    List to which MDL requests are to be added.
  @param[in,out]  fk_invalidator  Object keeping track of which dd::Table
                                  objects to invalidate.

  @retval operation outcome, false if no error.
*/
bool
collect_fk_parents_for_new_fks(THD *thd,
                               const char *db_name, const char *table_name,
                               const Alter_info *alter_info,
                               enum_mdl_type lock_type,
                               handlerton *hton,
                               MDL_request_list *mdl_requests,
                               Foreign_key_parents_invalidator *fk_invalidator)
                               MY_ATTRIBUTE((warn_unused_result));


/**
  Acquire exclusive metadata locks on tables which definitions need to
  be updated or invalidated since they are related through foreign keys
  to the table to be renamed,
  Also add the referenced table names for the FKs on this table to the
  foreign key invalidator, to be used at a later stage to invalidate the
  dd::Table objects.

  @param          thd             Thread handle.
  @param          db              Table's old schema.
  @param          table_name      Table's old name.
  @param          table_def       Table definition of table being RENAMEd.
  @param          new_db          Table's new schema.
  @param          new_table_name  Table's new name.
  @param          hton            Table's SE.
  @param[in,out]  fk_invalidator  Object keeping track of which dd::Table
                                  objects to invalidate.

  @retval operation outcome, false if no error.
*/
bool
collect_and_lock_fk_tables_for_rename_table(THD *thd,
      const char *db, const char *table_name,
      const dd::Table *table_def,
      const char *new_db, const char *new_table_name,
      handlerton *hton,
      Foreign_key_parents_invalidator *fk_invalidator)
      MY_ATTRIBUTE((warn_unused_result));


/**
  Update referenced table names and the unique constraint name for FKs
  affected by RENAME TABLE operation.

  @param  thd             Thread handle.
  @param  db              Table's old schema.
  @param  table_name      Table's old name.
  @param  new_db          Table's new schema.
  @param  new_table_name  Table's new name.
  @param  hton            Table's SE.

  @retval operation outcome, false if no error.
*/
bool
adjust_fks_for_rename_table(THD *thd,
                            const char *db, const char *table_name,
                            const char *new_db, const char *new_table_name,
                            handlerton *hton)
                            MY_ATTRIBUTE((warn_unused_result));


/**
  Find name of unique constraint in parent table which is referenced by
  foreign key.

  @param parent_table_def Object describing the parent table.
  @param fk               Object describing the foreign key.

  @retval non-"" - unique constraint name if matching constraint is found.
  @retval ""     - if no matching unique constraint is found.
*/
const char *
find_fk_parent_key(const dd::Table *parent_table_def,
                   const dd::Foreign_key *fk)
                   MY_ATTRIBUTE((warn_unused_result));


/**
  Prepare Create_field and Key_spec objects for ALTER and upgrade.
  @param[in,out]  thd          thread handle. Used as a memory pool
                               and source of environment information.
  @param[in]      src_table    DD table object. Will be nullptr for temporary
                               tables and during upgrade.
  @param[in]      table        the source table, open and locked
                               Used as an interface to the storage engine
                               to acquire additional information about
                               the original table.
  @param[in,out]  create_info  A blob with CREATE/ALTER TABLE
                               parameters
  @param[in,out]  alter_info   Another blob with ALTER/CREATE parameters.
                               Originally create_info was used only in
                               CREATE TABLE and alter_info only in ALTER TABLE.
                               But since ALTER might end-up doing CREATE,
                               this distinction is gone and we just carry
                               around two structures.
  @param[in,out]  alter_ctx    Runtime context for ALTER TABLE.
  @param[in]      used_fields  used_fields from HA_CREATE_INFO.

  @retval TRUE   error, out of memory or a semantical error in ALTER
                 TABLE instructions
  @retval FALSE  success

*/
bool prepare_fields_and_keys(THD *thd,
                             const dd::Table *src_table,
                             TABLE *table,
                             HA_CREATE_INFO *create_info,
                             Alter_info *alter_info,
                             Alter_table_ctx *alter_ctx,
                             const uint &used_fields);

bool mysql_prepare_alter_table(THD *thd, const dd::Table *src_table,
                               TABLE *table, HA_CREATE_INFO *create_info,
                               Alter_info *alter_info,
                               Alter_table_ctx *alter_ctx);
bool mysql_trans_prepare_alter_copy_data(THD *thd);
bool mysql_trans_commit_alter_copy_data(THD *thd);
bool mysql_alter_table(THD *thd, const char *new_db, const char *new_name,
                       HA_CREATE_INFO *create_info,
                       TABLE_LIST *table_list,
                       Alter_info *alter_info);
bool mysql_compare_tables(TABLE *table,
                          Alter_info *alter_info,
                          HA_CREATE_INFO *create_info,
                          bool *metadata_equal);
bool mysql_recreate_table(THD *thd, TABLE_LIST *table_list, bool table_copy);
bool mysql_create_like_table(THD *thd, TABLE_LIST *table,
                             TABLE_LIST *src_table,
                             HA_CREATE_INFO *create_info);
bool mysql_rename_table(THD *thd, handlerton *base, const char *old_db,
                        const char * old_name, const dd::Schema &new_schema,
                        const char *new_db, const char * new_name, uint flags);

bool mysql_checksum_table(THD* thd, TABLE_LIST* table_list,
                          HA_CHECK_OPT* check_opt);
bool mysql_rm_table(THD *thd,TABLE_LIST *tables, bool if_exists,
                    bool drop_temporary);
bool mysql_rm_table_no_locks(THD *thd, TABLE_LIST *tables, bool if_exists,
                             bool drop_temporary, bool drop_database,
                             bool *dropped_non_atomic_flag,
                             std::set<handlerton*> *post_ddl_htons,
                             Foreign_key_parents_invalidator *fk_invalidator,
                             std::vector<MDL_ticket*> *safe_to_release_mdl);

/**
  Discover missing tables in SE and acquire locks on tables which participate
  in FKs on tables to be dropped by DROP TABLES/DATABASE and which definitions
  will have to be updated or invalidated during this operation.

  @param  thd     Thread context.
  @param  tables  Tables to be dropped by DROP TABLES/DATABASE.

  @retval False - Success.
  @retval True  - Failure.
*/
bool rm_table_do_discovery_and_lock_fk_tables(THD *thd, TABLE_LIST *tables)
  MY_ATTRIBUTE((warn_unused_result));

bool quick_rm_table(THD *thd, handlerton *base, const char *db,
                    const char *table_name, uint flags);
bool prepare_sp_create_field(THD *thd,
                             Create_field *field_def);
bool prepare_pack_create_field(THD *thd, Create_field *sql_field,
                               longlong table_flags);

const CHARSET_INFO* get_sql_field_charset(const Create_field *sql_field,
                                          const HA_CREATE_INFO *create_info);
bool validate_comment_length(THD *thd, const char *comment_str,
                             size_t *comment_len, uint max_len,
                             uint err_code, const char *comment_name);
int write_bin_log(THD *thd, bool clear_error,
                  const char *query, size_t query_length,
                  bool is_trans= FALSE);
void promote_first_timestamp_column(List<Create_field> *column_definitions);


/**
  Prepares the column definitions for table creation.

  @param thd                       Thread object.
  @param create_info               Create information.
  @param[in,out] create_list       List of columns to create.
  @param[in,out] select_field_pos  Position where the SELECT columns start
                                   for CREATE TABLE ... SELECT.
  @param file                      The handler for the new table.
  @param[in,out] sql_field         Create_field to populate.
  @param field_no                  Column number.

  @retval false   OK
  @retval true    error
*/

bool prepare_create_field(THD *thd, HA_CREATE_INFO *create_info,
                          List<Create_field> *create_list,
                          int *select_field_pos, handler *file,
                          Create_field *sql_field, int field_no);


/**
  Prepares the table and key structures for table creation.

  @param thd                       Thread object.
  @param error_schema_name         Schema name of the table to create/alter, only
                                   error reporting.
  @param error_table_name          Name of table to create/alter, only used for
                                   error reporting.
  @param create_info               Create information (like MAX_ROWS).
  @param alter_info                List of columns and indexes to create
  @param file                      The handler for the new table.
  @param[out] key_info_buffer      An array of KEY structs for the indexes.
  @param[out] key_count            The number of elements in the array.
  @param[out] fk_key_info_buffer   An array of FOREIGN_KEY structs for the
                                   foreign keys.
  @param[out] fk_key_count         The number of elements in the array.
  @param[in] existing_fks          An array of pre-existing FOREIGN KEYS
                                   (in case of ALTER).
  @param[in] existing_fks_count    The number of pre-existing foreign keys.
  @param select_field_count        The number of fields coming from a select table.
  @param find_parent_keys          Indicates whether we need to lookup name of
                                   unique constraint in parent table for foreign
                                   keys.

  @retval false   OK
  @retval true    error
*/

bool mysql_prepare_create_table(THD *thd,
                                const char* error_schema_name,
                                const char* error_table_name,
                                HA_CREATE_INFO *create_info,
                                Alter_info *alter_info,
                                handler *file, KEY **key_info_buffer,
                                uint *key_count,
                                FOREIGN_KEY **fk_key_info_buffer,
                                uint *fk_key_count,
                                FOREIGN_KEY *existing_fks,
                                uint existing_fks_count,
                                int select_field_count,
                                bool find_parent_keys);


size_t explain_filename(THD* thd, const char *from, char *to, size_t to_length,
                        enum_explain_filename_mode explain_mode);

void parse_filename(const char *filename, size_t filename_length,
                    const char ** schema_name, size_t *schema_name_length,
                    const char ** table_name, size_t *table_name_length,
                    const char ** partition_name, size_t *partition_name_length,
                    const char ** subpartition_name, size_t *subpartition_name_length);

extern MYSQL_PLUGIN_IMPORT const char *primary_key_name;


/**
  Acquire metadata lock on triggers associated with a list of tables.

  @param[in] thd     Current thread context
  @param[in] tables  Tables for that associated triggers have to locked.

  @return Operation status.
    @retval false Success
    @retval true  Failure
*/

bool lock_trigger_names(THD *thd, TABLE_LIST *tables);

#endif /* SQL_TABLE_INCLUDED */
