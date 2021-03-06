/* Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

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

#include "resource_group_mgr.h"

#include "sql/auth/auth_acls.h"                  // SUPER_ACL
#include "sql/current_thd.h"                     // current_thd
#include "sql/dd/cache/dictionary_client.h"      // Dictionary_client
#include "sql/dd/dd_resource_group.h"            // dd::create_resource_group
#include "sql/log.h"                             // sql_print_error
#include "sql/mysqld.h"                          // key_resource_group_mgr*
#include "sql/mysqld_thd_manager.h"              // Global_THD_manager
#include "sql/sql_class.h"                       // class THD
#include "sql_string.h"                          // to_lex_cstring
#include "sql/resourcegroups/thread_resource_control.h"

namespace resourcegroups
{
Resource_group_mgr *Resource_group_mgr::m_instance= nullptr;

void thread_create_callback(const PSI_thread_attrs *thread_attrs)
{
  auto res_grp_mgr= resourcegroups::Resource_group_mgr::instance();

  if (!res_grp_mgr->resource_group_support())
    return;

  if (thread_attrs != nullptr)
  {
    auto res_grp= thread_attrs->m_system_thread ?
      res_grp_mgr->sys_default_resource_group() :
      res_grp_mgr->usr_default_resource_group();
    res_grp_mgr->set_res_grp_in_pfs(res_grp->name().c_str(),
      res_grp->name().length(), thread_attrs->m_thread_internal_id);
  }
}


void session_disconnect_callback(const PSI_thread_attrs *)
{
  auto res_grp_mgr= resourcegroups::Resource_group_mgr::instance();

  if (!res_grp_mgr->resource_group_support())
    return;

  if (current_thd->resource_group_ctx()->m_cur_resource_group != nullptr)
    res_grp_mgr->move_resource_group(
      current_thd->resource_group_ctx()->m_cur_resource_group,
      res_grp_mgr->usr_default_resource_group());
}


Resource_group_mgr *Resource_group_mgr::instance()
{
  DBUG_ENTER("Resource_group_mgr::instance()");

  // Created during server startup. So no locking required.
  if (m_instance == nullptr)
  {
    m_instance= new (std::nothrow) Resource_group_mgr;
    DBUG_ASSERT(m_instance != nullptr);
  }
  DBUG_RETURN(m_instance);
}


/**
  Persist an in-memory resource group to Data Dictionary.

  @param thd  pointer to THD.
  @param resource_group Reference to the resource group to be persisted.
  @param update True if the resource groups exists and needs to be updated else
                     resource group is created in DD.

  @returns true if resource group persistence to DD failed else false.
*/

static inline bool persist_resource_group(
  THD *thd, const resourcegroups::Resource_group &resource_group, bool update)
{
  handlerton *ddse= ha_resolve_by_legacy_type(thd, DB_TYPE_INNODB);
  if (ddse->is_dict_readonly && ddse->is_dict_readonly())
  {
    sql_print_warning("Skipped updating resource group metadata "
                      "in InnoDB read only mode.");
    return false;
  }

  Disable_autocommit_guard autocommit_guard(thd);
  dd::String_type name(resource_group.name().c_str());
  bool res= update ? dd::update_resource_group(thd, name, resource_group) :
                     dd::create_resource_group(thd, resource_group);

  if (res)
  {
    sql_print_error("Failed to persist resource group %s to Data Dictionary",
                    resource_group.name().c_str());
    return true;
  }

  return false;
}


static bool deserialize_resource_groups(THD *thd)
{
  DBUG_ENTER("deserialize_resource_groups");

  /*
    Associate flag SYSTEM_THREAD_DD_INITIALIZE with THD context.
    This ensures we need not acquire MDL locks during initialization phase.
  */
  thd->system_thread= SYSTEM_THREAD_DD_INITIALIZE;
  std::vector<const dd::Resource_group*> resource_group_vec;
  std::vector<resourcegroups::Resource_group*> resource_group_ptr_vec;

  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  if (thd->dd_client()->fetch_global_components(&resource_group_vec))
    DBUG_RETURN(true);

  bool usr_default_in_dd= false;
  bool sys_default_in_dd= false;

  auto res_grp_mgr= Resource_group_mgr::instance();
  for (const auto &resource_group : resource_group_vec)
  {
    if (my_strcasecmp(&my_charset_utf8_general_ci,
                      resource_group->name().c_str(), "USR_default") == 0)
      usr_default_in_dd= true;
    else if (my_strcasecmp(&my_charset_utf8_general_ci,
                           resource_group->name().c_str(), "SYS_default") == 0)
      sys_default_in_dd= true;
    else
    {
      auto resource_group_ptr=
        res_grp_mgr->deserialize_resource_group(resource_group);
      if (resource_group_ptr == nullptr)
      {
        sql_print_error("Failed to deserialize resource group %s.",
                        resource_group->name().c_str());
        DBUG_RETURN(true);
      }

      resource_group_ptr_vec.push_back(resource_group_ptr);
    }
  }

  for(auto *resource_group_ptr : resource_group_ptr_vec)
  {
    // Validate the resource group and disable if validation is not successful
    auto thr_res_ctrl= resource_group_ptr->controller();
    if (thr_res_ctrl->validate(resource_group_ptr->type()))
    {
      resource_group_ptr->set_enabled(false);
      // Update the resource group on-disk.
      Disable_autocommit_guard autocommit_guard(thd);
      if (dd::update_resource_group(thd, resource_group_ptr->name().c_str(),
                                    *resource_group_ptr))
      {
        sql_print_warning("Update of resource group %s failed.",
                          resource_group_ptr->name().c_str());
        DBUG_RETURN(true);
      }
      sql_print_warning("Validation of resource group %s failed."
                        "Resource group is disabled.",
                        resource_group_ptr->name().c_str());
    }
  }

  if (persist_resource_group(thd, *res_grp_mgr->usr_default_resource_group(),
                             usr_default_in_dd))
    DBUG_RETURN(true);

  if (persist_resource_group(thd, *res_grp_mgr->sys_default_resource_group(),
                             sys_default_in_dd))
    DBUG_RETURN(true);

  DBUG_RETURN(false);
}


Resource_group *Resource_group_mgr::deserialize_resource_group(
  const dd::Resource_group *resource_group)
{
  DBUG_ASSERT(m_resource_group_support);

  LEX_CSTRING name_cstr= to_lex_cstring(resource_group->name().c_str());
  auto cpu_id_mask= resource_group->cpu_id_mask();
  auto vcpu_range_vector=
    std::unique_ptr<std::vector<Range> >(new (std::nothrow) std::vector<Range>);

  if (vcpu_range_vector.get() == nullptr)
  {
    sql_print_error("Unable to allocate memory for Resource Group %s",
                    name_cstr.str);
    return nullptr;
  }

  int bit_start= -1, bit_end= -1;
  for (uint i= 0; i < cpu_id_mask.size(); i++)
  {
    if (cpu_id_mask[i])
      bit_start == -1 ? (bit_start= bit_end= i) : (bit_end++);
    else
      if (bit_start != -1)
      {
        vcpu_range_vector->emplace_back(Range(bit_start, bit_end));
        bit_start= bit_end= -1;
      }
  }

  auto resource_group_ptr= create_and_add_in_resource_group_hash(
    name_cstr,
    resource_group->resource_group_type(),
    resource_group->resource_group_enabled(), std::move(vcpu_range_vector),
    resource_group->thread_priority());

  if (resource_group_ptr == nullptr)
  {
    sql_print_error("Failed to add resource group %s to resource group map",
                     name_cstr.str);
    return nullptr;
  }

  return resource_group_ptr;
}


bool Resource_group_mgr::acquire_shared_mdl_for_resource_group(
  THD *thd, const char *res_grp_name, enum_mdl_duration lock_duration,
  MDL_ticket **ticket, bool try_acquire)
{
  DBUG_ENTER("acquire_shared_mdl_for_resource_group");

  char lc_name[NAME_CHAR_LEN + 1];
  my_stpncpy(lc_name, res_grp_name, NAME_CHAR_LEN);
  lc_name[NAME_CHAR_LEN]= '\0';
  my_casedn_str(system_charset_info, lc_name);


  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::RESOURCE_GROUPS, "", lc_name,
                   MDL_INTENTION_EXCLUSIVE, lock_duration);

  bool res= try_acquire ? thd->mdl_context.acquire_lock(&mdl_request, 0) :
    thd->mdl_context.acquire_lock(&mdl_request,
                                  thd->variables.lock_wait_timeout);
  if (!res && ticket != nullptr)
    *ticket= mdl_request.ticket;

  DBUG_RETURN(res);
}


void Resource_group_mgr::deinit()
{
  if (m_resource_group_support && m_notify_svc != nullptr)
  {
    m_notify_svc->unregister_notification(m_notify_handle);
    m_registry_svc->release(m_h_res_grp_svc);
    m_h_res_grp_svc= nullptr;

    m_registry_svc->release(m_h_notification_svc);
    m_h_notification_svc= nullptr;

    mysql_plugin_registry_release(m_registry_svc);
    delete m_resource_group_hash;
    mysql_rwlock_destroy(&m_map_rwlock);
  }
}


bool Resource_group_mgr::post_init()
{
  DBUG_ENTER("Resource_group_mgr::post_init");

  if (!m_resource_group_support)
    DBUG_RETURN(false);

  // Create temporary THD to read Resource groups from disk.
  std::unique_ptr<THD> thd(new (std::nothrow) THD());
  if (thd.get() == nullptr)
    DBUG_RETURN(true);

  thd->thread_stack= reinterpret_cast<char*>(&thd);
  thd->store_globals();

  thd->security_context()->set_master_access(SUPER_ACL);
  thd->variables.transaction_read_only= false;
  thd->tx_read_only= false;

  bool res= deserialize_resource_groups(thd.get());

  DBUG_RETURN(res);
}


bool Resource_group_mgr::init()
{
  DBUG_ENTER("Resource_group_mgr::init");

#ifndef WITH_PERFSCHEMA_STORAGE_ENGINE
  // WITH_PERFSCHEMA_STORAGE_ENGINE is always set.
  static_assert(0, "WITH_PERFSCHEMA_STORAGE_ENGINE not defined.");
#endif

  if (!platform::is_platform_supported())
  {
    m_unsupport_reason= "Platform Unsupported";
    DBUG_RETURN(false);
  }

#ifdef DISABLE_PSI_THREAD
  // Resource group not supported with DISABLE_PSI_THREAD.
  m_resource_group_support= false;
  sql_print_information("Resource group feature is disabled. "
                        "(Server compiled with DISABLE_PSI_THREAD.)");
  m_unsupport_reason= "Server compiled with DISABLE_PSI_THREAD";
  DBUG_RETURN(false);
#endif

  m_resource_group_support= true;
  mysql_rwlock_init(key_rwlock_resource_group_mgr_map_lock, &m_map_rwlock);
  m_thread_priority_available= platform::can_thread_priority_be_set();

  m_registry_svc= mysql_plugin_registry_acquire();
  if (!m_registry_svc)
  {
    sql_print_warning("mysql_plugin_registry_acquire() failed");
    DBUG_RETURN(true);
  }

  if (m_registry_svc->acquire("pfs_resource_group", &m_h_res_grp_svc))
  {
    sql_print_warning("Can't find pfs_resource_group service");
    DBUG_RETURN(true);
  }
  m_resource_group_svc= reinterpret_cast<SERVICE_TYPE(pfs_resource_group) *>(
                          m_h_res_grp_svc);

  if (m_registry_svc->acquire("pfs_notification", &m_h_notification_svc))
  {
    sql_print_warning("Unable to get handle for pfs notification service.");
    DBUG_RETURN(true);
  }

  m_notify_svc = reinterpret_cast<SERVICE_TYPE(pfs_notification) *>(
                   m_h_notification_svc);

  // Register thread creation notification callbacks.
  PSI_notification callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.thread_create= &thread_create_callback;
  callbacks.session_disconnect= &session_disconnect_callback;

  m_notify_handle= m_notify_svc->register_notification(&callbacks, false);
  if (m_notify_handle == 0)
  {
    sql_print_warning("register_notify failed");
    DBUG_RETURN(true);
  }

  m_resource_group_hash= new collation_unordered_map<std::string,
    std::unique_ptr<Resource_group> >(&my_charset_utf8_tolower_ci,
                                      PSI_INSTRUMENT_ME);
  if (m_resource_group_hash == nullptr)
  {
    sql_print_error("Failed to allocate memory for resource group hash");
    DBUG_RETURN(true);
  }

  m_usr_default_resource_group= new (std::nothrow) Resource_group(
    "USR_default", resourcegroups::Type::USER_RESOURCE_GROUP, true);

  if (m_usr_default_resource_group == nullptr)
  {
    sql_print_error("Failed to allocate memory for user Resource Group.");
    delete m_resource_group_hash;
    m_resource_group_hash= nullptr;
    DBUG_RETURN(true);
  }

  m_sys_default_resource_group=new (std::nothrow) Resource_group(
    "SYS_default", resourcegroups::Type::SYSTEM_RESOURCE_GROUP, true);

  if (m_sys_default_resource_group == nullptr)
  {
    sql_print_error("Failed to allocate memory for user Resource Group.");
    delete m_resource_group_hash;
    m_resource_group_hash= nullptr;
    delete m_usr_default_resource_group;
    m_usr_default_resource_group= nullptr;
    DBUG_RETURN(true);
  }

  add_resource_group(
    std::unique_ptr<Resource_group>(m_usr_default_resource_group));
  add_resource_group(
    std::unique_ptr<Resource_group>(m_sys_default_resource_group));

  // Initialize number of VCPUs.
  m_num_vcpus= platform::num_vcpus();
  DBUG_RETURN(false);
}


bool Resource_group_mgr::move_resource_group(
  Resource_group *from_res_grp, Resource_group *to_res_grp)
{
  DBUG_ASSERT(m_resource_group_support);

  if (to_res_grp == nullptr)
    to_res_grp= m_usr_default_resource_group;

  if (from_res_grp == to_res_grp)
    return false;

  if (to_res_grp->controller()->apply_control())
  {
    sql_print_warning("Unable to apply resource group controller %s",
                      to_res_grp->name().c_str());
    return false;
  }

  // Set resource group name in PFS.
  ulonglong pfs_thread_id= 0;

#ifdef HAVE_PSI_THREAD_INTERFACE
  ulonglong unused_event_id MY_ATTRIBUTE((unused));

  PSI_THREAD_CALL(get_thread_event_id)(&pfs_thread_id, &unused_event_id);
#endif

  m_resource_group_svc->set_thread_resource_group_by_id(nullptr, pfs_thread_id,
    to_res_grp->name().c_str(), to_res_grp->name().length(), nullptr);

  if (from_res_grp != nullptr && !is_resource_group_default(from_res_grp))
    from_res_grp->remove_pfs_thread_id(pfs_thread_id);

  if (!is_resource_group_default(to_res_grp))
    to_res_grp->add_pfs_thread_id(pfs_thread_id);
  return true;
}


void Resource_group_mgr::destroy_instance()
{
  if (m_instance != nullptr)
  {
    m_instance->deinit();
    delete m_instance;
    m_instance= nullptr;
  }
}


Resource_group *Resource_group_mgr::get_resource_group(
  const std::string &resource_group_name)
{
  DBUG_ENTER("Resource_group_mgr::get_resource_group");

  DBUG_ASSERT(m_resource_group_support);

  Resource_group *resource_group= nullptr;

  mysql_rwlock_rdlock(&m_map_rwlock);
  auto resource_group_iter= m_resource_group_hash->find(resource_group_name);
  if (resource_group_iter != m_resource_group_hash->end())
    resource_group= resource_group_iter->second.get();
  mysql_rwlock_unlock(&m_map_rwlock);

  DBUG_RETURN(resource_group);
}


bool Resource_group_mgr::add_resource_group(
  std::unique_ptr<Resource_group> resource_group_ptr)
{
  DBUG_ENTER("Resource_group_mgr::add_resource_group");
  DBUG_ASSERT(m_resource_group_support);

  mysql_rwlock_wrlock(&m_map_rwlock);
  m_resource_group_hash->emplace(resource_group_ptr->name(),
                                 std::move(resource_group_ptr));
  mysql_rwlock_unlock(&m_map_rwlock);
  DBUG_RETURN(false);
}


void Resource_group_mgr::remove_resource_group(const std::string &name)
{
  DBUG_ENTER("Resource_group_mgr::remove_resource_group");
  DBUG_ASSERT(m_resource_group_support);

  mysql_rwlock_wrlock(&m_map_rwlock);
  m_resource_group_hash->erase(name);
  mysql_rwlock_unlock(&m_map_rwlock);

  DBUG_VOID_RETURN;
}


Resource_group *Resource_group_mgr::create_and_add_in_resource_group_hash(
  const LEX_CSTRING &name, Type type, bool enabled,
  std::unique_ptr<std::vector<Range>> vcpu_range_vector,
  int priority)
{
  DBUG_ENTER("resourcegroups::create_in_memory_resource_group");
  DBUG_ASSERT(m_resource_group_support);

  auto resource_group_ptr=
    new (std::nothrow) Resource_group(std::string(name.str),
                                      type, enabled);
  if (resource_group_ptr == nullptr)
  {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), 0);
    DBUG_RETURN(nullptr);
  }

  auto thr_res_ctrl= resource_group_ptr->controller();
  thr_res_ctrl->set_priority(priority);
  thr_res_ctrl->set_vcpu_vector(*vcpu_range_vector);

  // add to in-memory hash
  Resource_group_mgr::instance()->add_resource_group(
    std::unique_ptr<Resource_group>(resource_group_ptr));

  DBUG_RETURN(resource_group_ptr);
}


#ifndef DBUG_OFF
bool Resource_group_mgr::disable_pfs_notification()
{
  if (!m_resource_group_support || m_notify_svc == nullptr)
    return false;

  m_notify_svc->unregister_notification(m_notify_handle);
  m_notify_handle= 0;

  return false;
}
#endif


bool Resource_group_mgr::switch_resource_group_if_needed(
  THD *thd, resourcegroups::Resource_group **src_res_grp,
  resourcegroups::Resource_group **dest_res_grp, MDL_ticket **ticket,
  MDL_ticket **cur_ticket)
{
  bool switched= false;
  auto res_grp_name=
    thd->resource_group_ctx()->m_switch_resource_group_str;

  if (!opt_initialize && res_grp_name[0] != '\0')
  {
    resourcegroups::Resource_group_mgr *mgr_instance=
      resourcegroups::Resource_group_mgr::instance();

    if (mgr_instance->acquire_shared_mdl_for_resource_group(thd, res_grp_name,
                                                            MDL_EXPLICIT,
                                                            ticket, false))
    {
      sql_print_warning("Unable to acquire lock. Resource group hint to switch"
                        " resource group %s shall be ignored", res_grp_name);
      res_grp_name[0]= '\0';
      return false;
    }

    auto resource_group= mgr_instance->get_resource_group(res_grp_name);

    if (resource_group == nullptr)
    {
      thd->resource_group_ctx()->m_warn= WARN_RESOURCE_GROUP_NOT_EXISTS;
      return false;
    }

    Security_context *sctx= thd->security_context();
    if (!(sctx->check_access(SUPER_ACL) ||
          sctx->has_global_grant(STRING_WITH_LEN("RESOURCE_GROUP_ADMIN")).first ||
          sctx->has_global_grant(STRING_WITH_LEN("RESOURCE_GROUP_USER")).first))
    {
      thd->resource_group_ctx()->m_warn= WARN_RESOURCE_GROUP_ACCESS_DENIED;
    }

    // Do not allow SYSTEM resource group to bind with a session thread.
    if (resource_group->type() == resourcegroups::Type::SYSTEM_RESOURCE_GROUP)
    {
      thd->resource_group_ctx()->m_warn= WARN_RESOURCE_GROUP_TYPE_MISMATCH;
      return false;
    }

    mysql_mutex_lock(&thd->LOCK_thd_data);
    *src_res_grp= thd->resource_group_ctx()->m_cur_resource_group;
    *dest_res_grp= resource_group;

    if (*src_res_grp == *dest_res_grp)
    {
      mysql_mutex_unlock(&thd->LOCK_thd_data);
      return false;
    }

    const char *src_res_grp_str= *src_res_grp != nullptr ?
      (*src_res_grp)->name().c_str() : nullptr;

    if (src_res_grp_str != nullptr &&
        mgr_instance->acquire_shared_mdl_for_resource_group(thd, src_res_grp_str,
                                                            MDL_EXPLICIT,
                                                            cur_ticket, true))
    {
      sql_print_warning("Unable to acquire lock on current resource group %s."
                        "Hint shall be ignored for the query. ",
                        src_res_grp_str);
      mysql_mutex_unlock(&thd->LOCK_thd_data);
      res_grp_name[0]= '\0';
      return false;
    }
    DBUG_ASSERT(*dest_res_grp != nullptr);
    switched= mgr_instance->move_resource_group(*src_res_grp, *dest_res_grp);
    mysql_mutex_unlock(&thd->LOCK_thd_data);
    DBUG_EXECUTE_IF("pause_after_rg_switch",
                    {
                      const char act[]= "now SIGNAL execute_pfs_select "
                                        "WAIT_FOR signal_to_continue";
                      DBUG_ASSERT(!debug_sync_set_action(thd,
                                  STRING_WITH_LEN(act)));

                    };);
  }

  return switched;
}

} // resourcegroups
