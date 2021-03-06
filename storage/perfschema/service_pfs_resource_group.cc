/* Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

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

/**
  @file storage/perfschema/service_pfs_resource_group.cc
  The performance schema implementation of the resource group service.
*/

#include <mysql/plugin.h>
#include <mysql/components/service_implementation.h>
#include <mysql/components/my_service.h>
#include <mysql/components/services/pfs_resource_group.h>
#include "pfs_server.h"

extern int pfs_set_thread_resource_group_v1(const char *group_name,
                                            int group_name_len,
                                            void *user_data);
extern int pfs_set_thread_resource_group_by_id_v1(PSI_thread *thread,
                                                  ulonglong thread_id,
                                                  const char *group_name,
                                                  int group_name_len,
                                                  void *user_data);
extern int pfs_get_thread_system_attrs_v1(PSI_thread_attrs *thread_attrs);

extern int pfs_get_thread_system_attrs_by_id_v1(PSI_thread *thread,
                                                ulonglong thread_id,
                                                PSI_thread_attrs *thread_attrs);

int
impl_pfs_set_thread_resource_group(const char *group_name,
                                   int group_name_len,
                                   void *user_data)
{
  return pfs_set_thread_resource_group_v1(
    group_name, group_name_len, user_data);
}

int
impl_pfs_set_thread_resource_group_by_id(PSI_thread *thread,
                                         ulonglong thread_id,
                                         const char *group_name,
                                         int group_name_len,
                                         void *user_data)
{
  return pfs_set_thread_resource_group_by_id_v1(
    thread, thread_id, group_name, group_name_len, user_data);
}

int
impl_pfs_get_thread_system_attrs(PSI_thread_attrs *thread_attrs)
{
  return pfs_get_thread_system_attrs_v1(thread_attrs);
}

int
impl_pfs_get_thread_system_attrs_by_id(PSI_thread *thread,
                                       ulonglong thread_id,
                                       PSI_thread_attrs *thread_attrs)
{
  return pfs_get_thread_system_attrs_by_id_v1(thread, thread_id, thread_attrs);
}

SERVICE_TYPE(pfs_resource_group)
SERVICE_IMPLEMENTATION(mysql_server, pfs_resource_group) = {
  impl_pfs_set_thread_resource_group,
  impl_pfs_set_thread_resource_group_by_id,
  impl_pfs_get_thread_system_attrs,
  impl_pfs_get_thread_system_attrs_by_id};

/**
  Register the Resource Group service with the MySQL server registry.
  @return 0 if successful, 1 otherwise
*/
int
register_pfs_resource_group_service()
{
  SERVICE_TYPE(registry) * r;
  int result = 0;

  r = mysql_plugin_registry_acquire();
  if (!r)
  {
    return 1;
  }

  my_service<SERVICE_TYPE(registry_registration)> reg("registry_registration",
                                                      r);

  if (reg->register_service("pfs_resource_group.mysql_server",
                            (my_h_service)&imp_mysql_server_pfs_resource_group))
  {
    result = 1;
  }

  mysql_plugin_registry_release(r);

  return result;
}

/**
  Unregister the Resource Group service.
  @return 0 if successful, 1 otherwise
*/
int
unregister_pfs_resource_group_service()
{
  SERVICE_TYPE(registry) * r;
  int result = 0;

  r = mysql_plugin_registry_acquire();
  if (!r)
  {
    return 1;
  }

  my_service<SERVICE_TYPE(registry_registration)> reg("registry_registration",
                                                      r);

  if (reg->unregister("pfs_resource_group.mysql_server"))
  {
    result = 1;
  }

  mysql_plugin_registry_release(r);

  return result;
}
