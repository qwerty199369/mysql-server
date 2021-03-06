/* Copyright (c) 2000, 2017, Oracle and/or its affiliates. All rights reserved.

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
#ifndef SQL_USER_CACHE_INCLUDED
#define SQL_USER_CACHE_INCLUDED

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <boost/graph/graph_selectors.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/properties.hpp>
#include <boost/pending/property.hpp>
#include <string.h>
#include <sys/types.h>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

#include "lex_string.h"
#include "lf.h"
#include "m_ctype.h"
#include "map_helpers.h"
#include "mem_root_fwd.h"
#include "mf_wcomp.h"                   // wild_many, wild_one, wild_prefix
#include "my_alloc.h"
#include "my_inttypes.h"
#include "my_sharedlib.h"
#include "my_sys.h"
#include "mysql/components/services/mysql_mutex_bits.h"
#include "mysql/mysql_lex_string.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/psi/mysql_statement.h"
#include "mysql/udf_registration_types.h"
#include "mysql_com.h"                  // SCRAMBLE_LENGTH
#include "mysql_time.h"                 // MYSQL_TIME
#include "prealloced_array.h"           // Prealloced_array
#include "sql/auth/auth_common.h"
#include "sql/auth/auth_internal.h" // List_of_authid, Authid
#include "sql/dd/properties.h"
#include "sql/handler.h"
#include "sql/key.h"
#include "sql/my_decimal.h"
#include "sql/sql_alloc.h"              // Sql_alloc
#include "sql/sql_connect.h"            // USER_RESOURCES
#include "sql/sql_plugin_ref.h"
#include "sql/table.h"
#include "sql_string.h"
#include "typelib.h"
#include "violite.h"                    // SSL_type

class Security_context;
/* Forward Declarations */
class String;
class THD;
struct TABLE;
template <typename Element_type, size_t Prealloc> class Prealloced_array;

/* Classes */

class ACL_HOST_AND_IP
{
  char *hostname;
  size_t hostname_length;
  long ip, ip_mask; // Used with masked ip:s

  const char *calc_ip(const char *ip_arg, long *val, char end);

public:
  const char *get_host() const { return hostname; }
  size_t get_host_len() const { return hostname_length; }

  bool has_wildcard()
  {
    return (strchr(hostname,wild_many) ||
            strchr(hostname,wild_one)  || ip_mask );
  }

  bool check_allow_all_hosts()
  {
    return (!hostname ||
            (hostname[0] == wild_many && !hostname[1]));
  }

  void update_hostname(const char *host_arg);

  bool compare_hostname(const char *host_arg, const char *ip_arg);

};

class ACL_ACCESS {
public:
  ACL_HOST_AND_IP host;
  ulong sort;
  ulong access;
};

/* ACL_HOST is used if no host is specified */

class ACL_HOST :public ACL_ACCESS
{
public:
  char *db;
};

class ACL_USER :public ACL_ACCESS
{
public:
  USER_RESOURCES user_resource;
  char *user;
  /**
    The salt variable is used as the password hash for
    native_password_authetication.
  */
  uint8 salt[SCRAMBLE_LENGTH + 1];       // scrambled password in binary form
  /**
    In the old protocol the salt_len indicated what type of autnetication
    protocol was used: 0 - no password, 4 - 3.20, 8 - 4.0,  20 - 4.1.1
  */
  uint8 salt_len;
  enum SSL_type ssl_type;
  const char *ssl_cipher, *x509_issuer, *x509_subject;
  LEX_CSTRING plugin;
  LEX_STRING auth_string;
  bool password_expired;
  bool can_authenticate;
  MYSQL_TIME password_last_changed;
  uint password_lifetime;
  bool use_default_password_lifetime;
  /**
    Specifies whether the user account is locked or unlocked.
  */
  bool account_locked;
  /**
   If this ACL_USER was used as a role id then this flag is true.
   During RENAME USER this variable is used for determining if it is safe
   to rename the user or not.
  */
  bool is_role;

  /**
    The number of old passwords to check when setting a new password
  */
  uint32 password_history_length;

  /**
    Ignore @ref password_history_length,
    use the global default @ref global_password_history
  */
  bool use_default_password_history;

  /**
    The number of days that would have to pass before a password can be reused.
  */
  uint32 password_reuse_interval;
  /**
    Ignore @ref password_reuse_interval,
    use the global default @ref global_password_reuse_interval
  */
  bool use_default_password_reuse_interval;

  ACL_USER *copy(MEM_ROOT *root);
};

class ACL_DB :public ACL_ACCESS
{
public:
  char *user,*db;
};

class ACL_PROXY_USER :public ACL_ACCESS
{
  const char *user;
  ACL_HOST_AND_IP proxied_host;
  const char *proxied_user;
  bool with_grant;

  typedef enum { 
    MYSQL_PROXIES_PRIV_HOST, 
    MYSQL_PROXIES_PRIV_USER, 
    MYSQL_PROXIES_PRIV_PROXIED_HOST,
    MYSQL_PROXIES_PRIV_PROXIED_USER, 
    MYSQL_PROXIES_PRIV_WITH_GRANT,
    MYSQL_PROXIES_PRIV_GRANTOR,
    MYSQL_PROXIES_PRIV_TIMESTAMP } old_acl_proxy_users;
public:
  ACL_PROXY_USER () {};

  void init(const char *host_arg, const char *user_arg,
            const char *proxied_host_arg, const char *proxied_user_arg,
            bool with_grant_arg);

  void init(MEM_ROOT *mem, const char *host_arg, const char *user_arg,
            const char *proxied_host_arg, const char *proxied_user_arg,
            bool with_grant_arg);

  void init(TABLE *table, MEM_ROOT *mem);

  bool get_with_grant() { return with_grant; }
  const char *get_user() { return user; }
  const char *get_proxied_user() { return proxied_user; }
  const char *get_proxied_host() { return proxied_host.get_host(); }
  void set_user(MEM_ROOT *mem, const char *user_arg) 
  { 
    user= user_arg && *user_arg ? strdup_root(mem, user_arg) : NULL;
  }

  bool check_validity(bool check_no_resolve);

  bool matches(const char *host_arg, const char *user_arg, const char *ip_arg,
                const char *proxied_user_arg, bool any_proxy_user);

  inline static bool auth_element_equals(const char *a, const char *b)
  {
    return (a == b || (a != NULL && b != NULL && !strcmp(a,b)));
  }


  bool pk_equals(ACL_PROXY_USER *grant);

  bool granted_on(const char *host_arg, const char *user_arg)
  {
    return (((!user && (!user_arg || !user_arg[0])) ||
             (user && user_arg && !strcmp(user, user_arg))) &&
            ((!host.get_host() && (!host_arg || !host_arg[0])) ||
             (host.get_host() && host_arg && !strcmp(host.get_host(), host_arg))));
  }


  void print_grant(String *str);

  void set_data(ACL_PROXY_USER *grant)
  {
    with_grant= grant->with_grant;
  }

  static int store_pk(TABLE *table,
                      const LEX_CSTRING &host,
                      const LEX_CSTRING &user,
                      const LEX_CSTRING &proxied_host,
                      const LEX_CSTRING &proxied_user);

  static int store_with_grant(TABLE * table,
                              bool with_grant);

  static int store_data_record(TABLE *table,
                               const LEX_CSTRING &host,
                               const LEX_CSTRING &user,
                               const LEX_CSTRING &proxied_host,
                               const LEX_CSTRING &proxied_user,
                               bool with_grant,
                               const char *grantor);
};

class acl_entry
{
public:
  ulong access;
  uint16 length;
  char key[1];                          // Key will be stored here
};


class GRANT_COLUMN :public Sql_alloc
{
public:
  ulong rights;
  std::string column;
  GRANT_COLUMN(String &c,  ulong y);
};


class GRANT_NAME :public Sql_alloc
{
public:
  ACL_HOST_AND_IP host;
  char *db, *user, *tname;
  ulong privs;
  ulong sort;
  std::string hash_key;
  GRANT_NAME(const char *h, const char *d,const char *u,
             const char *t, ulong p, bool is_routine);
  GRANT_NAME (TABLE *form, bool is_routine);
  virtual ~GRANT_NAME() {};
  virtual bool ok() { return privs != 0; }
  void set_user_details(const char *h, const char *d,
                        const char *u, const char *t,
                        bool is_routine);
};


class GRANT_TABLE :public GRANT_NAME
{
public:
  ulong cols;
  collation_unordered_multimap
    <std::string, unique_ptr_destroy_only<GRANT_COLUMN>> hash_columns;

  GRANT_TABLE(const char *h, const char *d,const char *u,
              const char *t, ulong p, ulong c);
  explicit GRANT_TABLE(TABLE *form);
  bool init(TABLE *col_privs);
  ~GRANT_TABLE();
  bool ok() { return privs != 0 || cols != 0; }
};


/* Data Structures */

extern MEM_ROOT global_acl_memory;
extern MEM_ROOT memex; 
const size_t ACL_PREALLOC_SIZE = 10U;
extern Prealloced_array<ACL_USER, ACL_PREALLOC_SIZE> *acl_users;
extern Prealloced_array<ACL_PROXY_USER, ACL_PREALLOC_SIZE> *acl_proxy_users;
extern Prealloced_array<ACL_DB, ACL_PREALLOC_SIZE> *acl_dbs;
extern Prealloced_array<ACL_HOST_AND_IP, ACL_PREALLOC_SIZE> *acl_wild_hosts;
extern std::unique_ptr<
  malloc_unordered_multimap<std::string, unique_ptr_destroy_only<GRANT_TABLE>>>
    column_priv_hash;
extern std::unique_ptr<
  malloc_unordered_multimap<std::string, unique_ptr_destroy_only<GRANT_NAME>>>
    proc_priv_hash, func_priv_hash;
extern collation_unordered_map<std::string, ACL_USER *> *acl_check_hosts;
extern bool allow_all_hosts;
extern uint grant_version; /* Version of priv tables */

// Search for a matching grant. Prefer exact grants before non-exact ones.

extern MYSQL_PLUGIN_IMPORT CHARSET_INFO *files_charset_info;

template<class T>
T *name_hash_search
  (const malloc_unordered_multimap<std::string, unique_ptr_destroy_only<T>>
     &name_hash,
   const char *host,const char* ip, const char *db,
   const char *user, const char *tname, bool exact, bool name_tolower)
{
  T *found= nullptr;

  std::string name= tname;
  if (name_tolower)
    my_casedn_str(files_charset_info, &name[0]);
  std::string key= user;
  key.push_back('\0');
  key.append(db);
  key.push_back('\0');
  key.append(name);
  key.push_back('\0');

  auto it_range= name_hash.equal_range(key);
  for (auto it= it_range.first; it != it_range.second; ++it)
  {
    T *grant_name= it->second.get();
    if (exact)
    {
      if (!grant_name->host.get_host() ||
          (host && !my_strcasecmp(system_charset_info, host,
                                  grant_name->host.get_host())) ||
          (ip && !strcmp(ip, grant_name->host.get_host())))
        return grant_name;
    }
    else
    {
      if (grant_name->host.compare_hostname(host, ip) &&
          (!found || found->sort < grant_name->sort))
        found=grant_name;                                       // Host ok
    }
  }
  return found;
}

inline GRANT_NAME * routine_hash_search(const char *host, const char *ip,
                                        const char *db, const char *user,
                                        const char *tname, bool proc,
                                        bool exact)
{
  return name_hash_search(proc ? *proc_priv_hash : *func_priv_hash,
                          host, ip, db, user, tname, exact, true);
}

inline GRANT_TABLE * table_hash_search(const char *host, const char *ip,
                                       const char *db, const char *user,
                                       const char *tname, bool exact)
{
  return name_hash_search(*column_priv_hash, host, ip, db,
                          user, tname, exact, false);
}

inline GRANT_COLUMN * column_hash_search(GRANT_TABLE *t, const char *cname,
                                         size_t length)
{
  return find_or_nullptr(t->hash_columns, std::string(cname, length));
}

/* Role management */

/** Tag dispatch for custom Role_properties */
namespace boost {
  enum vertex_acl_user_t { vertex_acl_user };
  BOOST_INSTALL_PROPERTY(vertex, acl_user);
}

/** 
  Custom vertex properties used in Granted_roles_graph
  TODO ACL_USER contains too much information. We only need global access,
  username and hostname. If this was a POD we don't have to hold the same
  mutex as ACL_USER.
*/
typedef boost::property<boost::vertex_acl_user_t,
                        ACL_USER,
                        boost::property<boost::vertex_name_t, std::string >
                       > Role_properties;

typedef boost::property<boost::edge_capacity_t,
                        int
                       > Role_edge_properties;

/** A graph of all users/roles privilege inheritance */
typedef boost::adjacency_list<boost::setS,      // OutEdges
                              boost::vecS,      // Vertices
                              boost::directedS,  // Directed graph
                              Role_properties,  // Vertex props
                              Role_edge_properties
                             > Granted_roles_graph;

/** The data type of a vertex in the Granted_roles_graph */
typedef boost::graph_traits<Granted_roles_graph>::vertex_descriptor
  Role_vertex_descriptor;

/** The data type of an edge in the Granted_roles_graph */
typedef boost::graph_traits<Granted_roles_graph>::edge_descriptor
  Role_edge_descriptor;

/** The datatype of the map between authids and graph vertex descriptors */
typedef std::unordered_map<std::string, Role_vertex_descriptor >
  Role_index_map;

/** Container for global, schema, table/view and routine ACL maps */
class Acl_map : public Sql_alloc
{
public:
  Acl_map(Security_context *sctx, uint64 ver);
  Acl_map(const Acl_map &map);
  Acl_map(const Acl_map &&map);
  ~Acl_map();
private:
  Acl_map &operator=(const Acl_map &map);
public:
  void *operator new(size_t size);
  void operator delete(void* p);
  Acl_map &operator=(Acl_map &&map);
  void increase_reference_count();
  void decrease_reference_count();

  ulong global_acl();
  Db_access_map *db_acls();
  Db_access_map *db_wild_acls();
  Table_access_map *table_acls();
  SP_access_map *sp_acls();
  SP_access_map *func_acls();
  Grant_acl_set *grant_acls();
  Dynamic_privileges *dynamic_privileges();
  uint64 version() { return m_version; }
  uint32 reference_count()
  {
    return m_reference_count.load();
  }
private:
  std::atomic<int32> m_reference_count;
  uint64 m_version;
  Db_access_map m_db_acls;
  Db_access_map m_db_wild_acls;
  Table_access_map m_table_acls;
  ulong m_global_acl;
  SP_access_map m_sp_acls;
  SP_access_map m_func_acls;
  Grant_acl_set m_with_admin_acls;
  Dynamic_privileges m_dynamic_privileges;
};

typedef LF_HASH Acl_cache_internal;

class Acl_cache
{
public:
  Acl_cache();
  ~Acl_cache();

  /**
    When ever the role graph is modified we must flatten the privileges again.
    This is done by increasing the role graph version counter. Next time
    a security context is created for an authorization id (aid) a request is
    also sent to the acl_cache to checkout a flattened acl_map for this
    particular aid. If a previous acl_map exists the version of this map is
    compared to the role graph version. If they don't match a new acl_map
    is calculated and inserted into the cache.
  */
  void increase_version();
  /**
    Returns a pointer to an acl map to the caller and increase the reference
    count on the object, iff the object version is the same as the global
    graph version.
    If no acl map exists which correspond ot the current authorization id of
    the security context, a new acl map is calculated, inserted into the cache
    and returned to the user.
    A new object will also be created if the role graph version counter is
    different than the acl map object's version.
  
    @param uid
    @return
  */
  Acl_map *checkout_acl_map(Security_context *sctx, Auth_id_ref &uid,
                            List_of_auth_id_refs &active_roles);
  /**
    When the security context is done with the acl map it calls the cache
    to decrease the reference count on that object.
    @param map
  */
  void return_acl_map(Acl_map *map);
  /**
    Removes all acl map objects with a references count of zero.
  */
  void flush_cache();
  /**
    Return a lower boundary to the current version count.
  */
  uint64 version();
  /**
    Return a snapshot of the number of items in the cache
  */
  int32 size();
  
private:
  /**
    Creates a new acl map for the authorization id of the security context.

    @param version The version of the new map
    @param sctx The associated security context
    @return
  */
  Acl_map *create_acl_map(uint64 version, Security_context *sctx);
  /** Role graph version counter */
  std::atomic<uint64> m_role_graph_version;
  Acl_cache_internal m_cache;
  mysql_mutex_t m_cache_flush_mutex;
};

Acl_cache *get_global_acl_cache();


/**
  Enum for specifying lock type over Acl cache
*/

enum class Acl_cache_lock_mode
{
  READ_MODE=1,
  WRITE_MODE
};

/**
  Lock guard for ACL Cache.
  Destructor automatically releases the lock.
*/

class Acl_cache_lock_guard
{
public:
  Acl_cache_lock_guard(THD *thd,
                       Acl_cache_lock_mode mode);

  /**
    Acl_cache_lock_guard destructor.

    Release lock(s) if taken
  */
  ~Acl_cache_lock_guard()
  {
    unlock();
  }

  bool lock(bool raise_error= true);
  void unlock();

private:
  bool already_locked();

private:
  /** Handle to THD object */
  THD *m_thd;
  /** Lock mode */
  Acl_cache_lock_mode m_mode;
  /** Lock status */
  bool m_locked;
};

#endif /* SQL_USER_CACHE_INCLUDED */
