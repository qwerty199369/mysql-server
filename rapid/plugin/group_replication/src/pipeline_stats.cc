/* Copyright (c) 2016, 2017, Oracle and/or its affiliates. All rights reserved.

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

#include "pipeline_stats.h"

#include <time.h>

#include "my_dbug.h"
#include "my_systime.h"
#include "plugin.h"
#include "plugin_log.h"
#include "plugin_server_include.h"

/*
  The QUOTA based flow control tries to calculate how many
  transactions the slowest members can handle, at the certifier or
  at the applier level, by checking which members have a queue
  larger than the user-specified thresholds and, on those, checking
  which one has the lowest number of transactions certified/applied
  on the last step - let's call it MMT, which stands for Minimum
  Member Throughput. We then divide MMT by the number of writing
  members in the last step to specify how many transactions a
  member can safely send to the group (if a new member starts to
  write then the quota will be larger for one period but will be
  corrected on the next).
  About these factors:
    1. If we used MMT as the assigned quota (and if MMT represented
       well the capacity of the nodes) then the queue size would
       stabilize but would not decrease. To allow a delayed node to
       catch up on the certifier and/or queues we need to reserve
       some capacity on the slowest node, which this HOLD_FACTOR
       represents: 10% reserved to catch up.
    2. Once the queue is reduced below the user-specified threshold,
       the nodes would start to issue transactions at full speed
       even if that full speed meant pilling up many transactions
       in a single period. To avoid that we introduce the
       RELEASE_FACTOR (50%), which is enough to let the write
       capacity to grow quickly but still maintain a relation with
       the last throttled value so that the oscillation in number
       of transactions per second is not very steep, letting the
       throughput oscillate smoothly around the real cluster
       capacity.
*/
const int64 Flow_control_module::MAXTPS= INT_MAX32;


Pipeline_stats_member_message::Pipeline_stats_member_message(
    int32 transactions_waiting_certification,
    int32 transactions_waiting_apply,
    int64 transactions_certified,
    int64 transactions_applied,
    int64 transactions_local,
    int64 transactions_negative_certified,
    int64 transactions_rows_in_validation,
    const std::string& transactions_all_committed,
    const std::string& transactions_last_conflict_free,
    int64 transactions_local_rollback,
    Flow_control_mode mode)
  : Plugin_gcs_message(CT_PIPELINE_STATS_MEMBER_MESSAGE),
    m_transactions_waiting_certification(transactions_waiting_certification),
    m_transactions_waiting_apply(transactions_waiting_apply),
    m_transactions_certified(transactions_certified),
    m_transactions_applied(transactions_applied),
    m_transactions_local(transactions_local),
    m_transactions_negative_certified(transactions_negative_certified),
    m_transactions_rows_validating(transactions_rows_in_validation),
    m_transactions_committed_all_members(transactions_all_committed),
    m_transaction_last_conflict_free(transactions_last_conflict_free),
    m_transactions_local_rollback(transactions_local_rollback),
    m_flow_control_mode(mode)
{}


Pipeline_stats_member_message::Pipeline_stats_member_message(const unsigned char *buf, size_t len)
  : Plugin_gcs_message(CT_PIPELINE_STATS_MEMBER_MESSAGE),
    m_transactions_waiting_certification(0),
    m_transactions_waiting_apply(0),
    m_transactions_certified(0),
    m_transactions_applied(0),
    m_transactions_local(0),
    m_transactions_negative_certified(0),
    m_transactions_rows_validating(0),
    m_transactions_committed_all_members(""),
    m_transaction_last_conflict_free(""),
    m_transactions_local_rollback(0),
    m_flow_control_mode(FCM_QUOTA)
{
  decode(buf, len);
}


Pipeline_stats_member_message::~Pipeline_stats_member_message()
{}


int32
Pipeline_stats_member_message::get_transactions_waiting_certification()
{
  DBUG_ENTER("Pipeline_stats_member_message::get_transactions_waiting_certification");
  DBUG_RETURN(m_transactions_waiting_certification);
}


int64
Pipeline_stats_member_message::get_transactions_certified()
{
  DBUG_ENTER("Pipeline_stats_member_message::get_transactions_certified");
  DBUG_RETURN(m_transactions_certified);
}


int32
Pipeline_stats_member_message::get_transactions_waiting_apply()
{
  DBUG_ENTER("Pipeline_stats_member_message::get_transactions_waiting_apply");
  DBUG_RETURN(m_transactions_waiting_apply);
}


int64
Pipeline_stats_member_message::get_transactions_applied()
{
  DBUG_ENTER("Pipeline_stats_member_message::get_transactions_applied");
  DBUG_RETURN(m_transactions_applied);
}


int64
Pipeline_stats_member_message::get_transactions_local()
{
  DBUG_ENTER("Pipeline_stats_member_message::get_transactions_local");
  DBUG_RETURN(m_transactions_local);
}


int64
Pipeline_stats_member_message::get_transactions_negative_certified()
{
  DBUG_ENTER("Pipeline_stats_member_message::get_transactions_negative_certified");
  DBUG_RETURN(m_transactions_negative_certified);
}


int64
Pipeline_stats_member_message::get_transactions_rows_validating()
{
  DBUG_ENTER("Pipeline_stats_member_message::get_transactions_rows_validating");
  DBUG_RETURN(m_transactions_rows_validating);
}


int64
Pipeline_stats_member_message::get_transactions_local_rollback()
{
  DBUG_ENTER("Pipeline_stats_member_message::get_transactions_local_rollback");
  DBUG_RETURN(m_transactions_local_rollback);
}


const std::string&
Pipeline_stats_member_message::get_transaction_committed_all_members()
{
  DBUG_ENTER("Pipeline_stats_member_message::get_transaction_committed_all_members");
  DBUG_RETURN(m_transactions_committed_all_members);
}


const std::string&
Pipeline_stats_member_message::get_transaction_last_conflict_free()
{
  DBUG_ENTER("Pipeline_stats_member_message::get_transaction_last_conflict_free");
  DBUG_RETURN(m_transaction_last_conflict_free);
}


Flow_control_mode
Pipeline_stats_member_message::get_flow_control_mode()
{
  DBUG_ENTER("Pipeline_stats_member_message::get_flow_control_mode");
  DBUG_RETURN(m_flow_control_mode);
}


void
Pipeline_stats_member_message::encode_payload(std::vector<unsigned char> *buffer) const
{
  DBUG_ENTER("Pipeline_stats_member_message::encode_payload");

  uint32 transactions_waiting_certification_aux=
      (uint32)m_transactions_waiting_certification;
  encode_payload_item_int4(buffer, PIT_TRANSACTIONS_WAITING_CERTIFICATION,
                           transactions_waiting_certification_aux);

  uint32 transactions_waiting_apply_aux=
      (uint32)m_transactions_waiting_apply;
  encode_payload_item_int4(buffer, PIT_TRANSACTIONS_WAITING_APPLY,
                           transactions_waiting_apply_aux);

  uint64 transactions_certified_aux=
      (uint64)m_transactions_certified;
  encode_payload_item_int8(buffer, PIT_TRANSACTIONS_CERTIFIED,
                           transactions_certified_aux);

  uint64 transactions_applied_aux=
      (uint64)m_transactions_applied;
  encode_payload_item_int8(buffer, PIT_TRANSACTIONS_APPLIED,
                           transactions_applied_aux);

  uint64 transactions_local_aux=
      (uint64)m_transactions_local;
  encode_payload_item_int8(buffer, PIT_TRANSACTIONS_LOCAL,
                           transactions_local_aux);

  uint64 transactions_negative_certified_aux=
      (uint64)m_transactions_negative_certified;
  encode_payload_item_int8(buffer, PIT_TRANSACTIONS_NEGATIVE_CERTIFIED,
                           transactions_negative_certified_aux);

  uint64 transactions_rows_validating_aux=
      (uint64)m_transactions_rows_validating;
  encode_payload_item_int8(buffer, PIT_TRANSACTIONS_ROWS_VALIDATING,
                           transactions_rows_validating_aux);

  if (!m_transactions_committed_all_members.empty())
  {
    encode_payload_item_string(buffer, PIT_TRANSACTIONS_COMMITTED_ALL_MEMBERS,
                               m_transactions_committed_all_members.c_str(),
                               m_transactions_committed_all_members.length());
  }

  if (!m_transaction_last_conflict_free.empty())
  {
    encode_payload_item_string(buffer, PIT_TRANSACTION_LAST_CONFLICT_FREE,
                               m_transaction_last_conflict_free.c_str(),
                               m_transaction_last_conflict_free.length());
  }

  uint64 transactions_local_rollback_aux=
      (uint64)m_transactions_local_rollback;
  encode_payload_item_int8(buffer, PIT_TRANSACTIONS_LOCAL_ROLLBACK,
                           transactions_local_rollback_aux);

  char flow_control_mode_aux=
       static_cast<char>(flow_control_mode_var);
  encode_payload_item_char(buffer, PIT_FLOW_CONTROL_MODE,
                           flow_control_mode_aux);

  DBUG_VOID_RETURN;
}


void
Pipeline_stats_member_message::decode_payload(const unsigned char *buffer,
                                              const unsigned char *end)
{
  DBUG_ENTER("Pipeline_stats_member_message::decode_payload");
  const unsigned char *slider= buffer;
  uint16 payload_item_type= 0;
  unsigned long long payload_item_length= 0;

  uint32 transactions_waiting_certification_aux= 0;
  decode_payload_item_int4(&slider,
                           &payload_item_type,
                           &transactions_waiting_certification_aux);
  m_transactions_waiting_certification=
      (int32)transactions_waiting_certification_aux;

  uint32 transactions_waiting_apply_aux= 0;
  decode_payload_item_int4(&slider,
                           &payload_item_type,
                           &transactions_waiting_apply_aux);
  m_transactions_waiting_apply=
      (int32)transactions_waiting_apply_aux;

  uint64 transactions_certified_aux= 0;
  decode_payload_item_int8(&slider,
                           &payload_item_type,
                           &transactions_certified_aux);
  m_transactions_certified=
      (int64)transactions_certified_aux;

  uint64 transactions_applied_aux= 0;
  decode_payload_item_int8(&slider,
                           &payload_item_type,
                           &transactions_applied_aux);
  m_transactions_applied=
      (int64)transactions_applied_aux;

  uint64 transactions_local_aux= 0;
  decode_payload_item_int8(&slider,
                           &payload_item_type,
                           &transactions_local_aux);
  m_transactions_local=
      (int64)transactions_local_aux;

  while (slider + Plugin_gcs_message::WIRE_PAYLOAD_ITEM_HEADER_SIZE <= end)
  {
    // Read payload item header to find payload item length.
    decode_payload_item_type_and_length(&slider,
                                        &payload_item_type,
                                        &payload_item_length);

    switch (payload_item_type)
    {
      case PIT_TRANSACTIONS_NEGATIVE_CERTIFIED:
        if (slider + payload_item_length <= end)
        {
          uint64 transactions_negative_certified_aux= *slider;
          slider += payload_item_length;
          m_transactions_negative_certified=
                 (int64)transactions_negative_certified_aux;
        }
        break;

      case PIT_TRANSACTIONS_ROWS_VALIDATING:
        if (slider + payload_item_length <= end)
        {
          uint64 transactions_rows_validating_aux= *slider;
          slider += payload_item_length;
          m_transactions_rows_validating=
                 (int64)transactions_rows_validating_aux;
        }
        break;

      case PIT_TRANSACTIONS_COMMITTED_ALL_MEMBERS:
        if (slider + payload_item_length <= end)
        {
          m_transactions_committed_all_members.assign(slider, slider + payload_item_length);
          slider += payload_item_length;
        }
        break;

      case PIT_TRANSACTION_LAST_CONFLICT_FREE:
        if (slider + payload_item_length <= end)
        {
          m_transaction_last_conflict_free.assign(slider, slider + payload_item_length);
          slider += payload_item_length;
        }
        break;

      case PIT_TRANSACTIONS_LOCAL_ROLLBACK:
        if (slider + payload_item_length <= end)
        {
          uint64 transactions_local_rollback_aux= *slider;
          slider += payload_item_length;
          m_transactions_local_rollback=
                 (int64)transactions_local_rollback_aux;
        }
        break;

      case PIT_FLOW_CONTROL_MODE:
        if (slider + payload_item_length <= end)
        {
          unsigned char flow_control_mode_aux= *slider;
          slider += payload_item_length;
          m_flow_control_mode= (Flow_control_mode)flow_control_mode_aux;
        }
        break;
    }
  }

  DBUG_VOID_RETURN;
}


Pipeline_stats_member_collector::Pipeline_stats_member_collector()
  : m_transactions_waiting_apply(0), m_transactions_certified(0),
    m_transactions_applied(0), m_transactions_local(0),
    m_transactions_local_rollback(0), send_transaction_identifiers(false)
{
  mysql_mutex_init(key_GR_LOCK_pipeline_stats_transactions_waiting_apply,
                   &m_transactions_waiting_apply_lock,
                   MY_MUTEX_INIT_FAST);
}


Pipeline_stats_member_collector::~Pipeline_stats_member_collector()
{
  mysql_mutex_destroy(&m_transactions_waiting_apply_lock);
}


void
Pipeline_stats_member_collector::increment_transactions_waiting_apply()
{
  mysql_mutex_lock(&m_transactions_waiting_apply_lock);
  DBUG_ASSERT(m_transactions_waiting_apply.load() >= 0);
  ++m_transactions_waiting_apply;
  mysql_mutex_unlock(&m_transactions_waiting_apply_lock);
}


void
Pipeline_stats_member_collector::decrement_transactions_waiting_apply()
{
  mysql_mutex_lock(&m_transactions_waiting_apply_lock);
  if (m_transactions_waiting_apply.load() > 0)
    --m_transactions_waiting_apply;
  DBUG_ASSERT(m_transactions_waiting_apply.load() >= 0);
  mysql_mutex_unlock(&m_transactions_waiting_apply_lock);
}


void
Pipeline_stats_member_collector::increment_transactions_certified()
{
  ++m_transactions_certified;
}


void
Pipeline_stats_member_collector::increment_transactions_applied()
{
  ++m_transactions_applied;
}


void
Pipeline_stats_member_collector::increment_transactions_local()
{
  ++m_transactions_local;
}


void
Pipeline_stats_member_collector::increment_transactions_local_rollback()
{
  ++m_transactions_local_rollback;
}


int32
Pipeline_stats_member_collector::get_transactions_waiting_apply()
{
  return m_transactions_waiting_apply.load();
}


int64
Pipeline_stats_member_collector::get_transactions_certified()
{
  return m_transactions_certified.load();
}


int64
Pipeline_stats_member_collector::get_transactions_applied()
{
  return m_transactions_applied.load();
}


int64
Pipeline_stats_member_collector::get_transactions_local()
{
  return m_transactions_local.load();
}


int64
Pipeline_stats_member_collector::get_transactions_local_rollback()
{
  return m_transactions_local_rollback.load();
}


void
Pipeline_stats_member_collector::set_send_transaction_identifiers()
{
  send_transaction_identifiers= true;
}


void
Pipeline_stats_member_collector::send_stats_member_message(Flow_control_mode mode)
{
  if (local_member_info == NULL)
    return; /* purecov: inspected */
  Group_member_info::Group_member_status member_status=
      local_member_info->get_recovery_status();
  if (member_status != Group_member_info::MEMBER_ONLINE &&
      member_status != Group_member_info::MEMBER_IN_RECOVERY)
    return;

  std::string last_conflict_free_transaction;
  std::string committed_transactions;

  Certifier_interface * cert_interface=
       (applier_module && applier_module->get_certification_handler()) ?
        applier_module->get_certification_handler()->get_certifier() : NULL;

  if (send_transaction_identifiers &&
      cert_interface != NULL)
  {
    char *committed_transactions_buf= NULL;
    size_t committed_transactions_buf_length= 0;
    int get_group_stable_transactions_set_string_outcome= cert_interface->
          get_group_stable_transactions_set_string(&committed_transactions_buf,
                                                   &committed_transactions_buf_length);
    if (!get_group_stable_transactions_set_string_outcome &&
        committed_transactions_buf_length > 0)
    {
      committed_transactions.assign(committed_transactions_buf);
    }
    my_free(committed_transactions_buf);
    cert_interface->get_last_conflict_free_transaction(&last_conflict_free_transaction);
    send_transaction_identifiers= false;
  }
  else
  {
    last_conflict_free_transaction.clear();
    committed_transactions.clear();
  }

  Pipeline_stats_member_message message(
      static_cast<int32>(applier_module->get_message_queue_size()),
      m_transactions_waiting_apply.load(),
      m_transactions_certified.load(),
      m_transactions_applied.load(),
      m_transactions_local.load(),
      (cert_interface != NULL)?cert_interface->get_negative_certified():0,
      (cert_interface != NULL)?cert_interface->get_certification_info_size():0,
      committed_transactions,
      last_conflict_free_transaction,
      m_transactions_local_rollback.load(),
      mode);

  enum_gcs_error msg_error= gcs_module->send_message(message, true);
  if (msg_error != GCS_OK)
  {
    log_message(MY_INFORMATION_LEVEL,
                "Error while sending stats message"); /* purecov: inspected */
  }
}


Pipeline_member_stats::Pipeline_member_stats()
  : m_transactions_waiting_certification(0),
    m_transactions_waiting_apply(0),
    m_transactions_certified(0),
    m_delta_transactions_certified(0),
    m_transactions_applied(0),
    m_delta_transactions_applied(0),
    m_transactions_local(0),
    m_delta_transactions_local(0),
    m_transactions_negative_certified(0),
    m_transactions_rows_validating(0),
    m_transactions_committed_all_members(),
    m_transaction_last_conflict_free(),
    m_transactions_local_rollback(0),
    m_flow_control_mode(FCM_QUOTA),
    m_stamp(0)
{}


Pipeline_member_stats::Pipeline_member_stats(Pipeline_stats_member_message &msg)
  : m_transactions_waiting_certification(msg.get_transactions_waiting_certification()),
    m_transactions_waiting_apply(msg.get_transactions_waiting_apply()),
    m_transactions_certified(msg.get_transactions_certified()),
    m_delta_transactions_certified(0),
    m_transactions_applied(msg.get_transactions_applied()),
    m_delta_transactions_applied(0),
    m_transactions_local(msg.get_transactions_local()),
    m_delta_transactions_local(0),
    m_transactions_negative_certified(msg.get_transactions_negative_certified()),
    m_transactions_rows_validating(msg.get_transactions_rows_validating()),
    m_transactions_committed_all_members(msg.get_transaction_committed_all_members()),
    m_transaction_last_conflict_free(msg.get_transaction_last_conflict_free()),
    m_transactions_local_rollback(msg.get_transactions_local_rollback()),
    m_flow_control_mode(msg.get_flow_control_mode()),
    m_stamp(0)
{}


Pipeline_member_stats::~Pipeline_member_stats()
{}


void
Pipeline_member_stats::update_member_stats(Pipeline_stats_member_message &msg,
                                           uint64 stamp)
{
  m_transactions_waiting_certification=
      msg.get_transactions_waiting_certification();

  m_transactions_waiting_apply=
      msg.get_transactions_waiting_apply();

  int64 previous_transactions_certified= m_transactions_certified;
  m_transactions_certified= msg.get_transactions_certified();
  m_delta_transactions_certified=
      m_transactions_certified - previous_transactions_certified;

  int64 previous_transactions_applied= m_transactions_applied;
  m_transactions_applied= msg.get_transactions_applied();
  m_delta_transactions_applied=
      m_transactions_applied - previous_transactions_applied;

  int64 previous_transactions_local= m_transactions_local;
  m_transactions_local= msg.get_transactions_local();
  m_delta_transactions_local=
      m_transactions_local - previous_transactions_local;

  m_transactions_negative_certified=
      msg.get_transactions_negative_certified();

  m_transactions_rows_validating=
      msg.get_transactions_rows_validating();

  if (!msg.get_transaction_committed_all_members().empty())
  {
    m_transactions_committed_all_members=
        msg.get_transaction_committed_all_members();
  }

  if (!msg.get_transaction_last_conflict_free().empty())
  {
    m_transaction_last_conflict_free=
      msg.get_transaction_last_conflict_free();
  }

  m_transactions_local_rollback=
      msg.get_transactions_local_rollback();

  m_flow_control_mode= msg.get_flow_control_mode();

  m_stamp= stamp;
}


bool
Pipeline_member_stats::is_flow_control_needed()
{
  return (m_flow_control_mode == FCM_QUOTA) &&
    (m_transactions_waiting_certification > flow_control_certifier_threshold_var
     || m_transactions_waiting_apply > flow_control_applier_threshold_var);
}


int32
Pipeline_member_stats::get_transactions_waiting_certification()
{
  return m_transactions_waiting_certification;
}


int32
Pipeline_member_stats::get_transactions_waiting_apply()
{
  return m_transactions_waiting_apply;
}


int64
Pipeline_member_stats::get_delta_transactions_certified()
{
  return m_delta_transactions_certified;
}


int64
Pipeline_member_stats::get_delta_transactions_applied()
{
  return m_delta_transactions_applied;
}


int64
Pipeline_member_stats::get_delta_transactions_local()
{
  return m_delta_transactions_local;
}


int64
Pipeline_member_stats::get_transactions_certified()
{
  return m_transactions_certified;
}


int64
Pipeline_member_stats::get_transactions_applied()
{
  return m_transactions_applied;
}


int64
Pipeline_member_stats::get_transactions_local()
{
  return m_transactions_local;
}


int64
Pipeline_member_stats::get_transactions_negative_certified()
{
  return m_transactions_negative_certified;
}


int64
Pipeline_member_stats::get_transactions_rows_validating()
{
  return m_transactions_rows_validating;
}


int64
Pipeline_member_stats::get_transactions_local_rollback()
{
  return m_transactions_local_rollback;
}


const std::string&
Pipeline_member_stats::get_transaction_committed_all_members()
{
  return m_transactions_committed_all_members;
}


const std::string&
Pipeline_member_stats::get_transaction_last_conflict_free()
{
  return m_transaction_last_conflict_free;
}

Flow_control_mode
Pipeline_member_stats::get_flow_control_mode()
{
  return m_flow_control_mode;
}

uint64
Pipeline_member_stats::get_stamp()
{
  return m_stamp;
}


#ifndef DBUG_OFF
void
Pipeline_member_stats::debug(const char *member, int64 quota_size,
                             int64 quota_used)
{
  log_message(MY_INFORMATION_LEVEL, "Flow control - update member stats: "
      "%s stats: certifier_queue %d, applier_queue %d,"
      " certified %ld (%ld), applied %ld (%ld), local %ld (%ld), quota %ld (%ld) "
      "mode=%d",
      member, m_transactions_waiting_certification,
      m_transactions_waiting_apply, m_transactions_certified,
      m_delta_transactions_certified, m_transactions_applied,
      m_delta_transactions_applied, m_transactions_local,
      m_delta_transactions_local, quota_size, quota_used,
      m_flow_control_mode); /* purecov: inspected */
}
#endif


Flow_control_module::Flow_control_module()
  : m_holds_in_period(0), m_quota_used(0), m_quota_size(0), m_stamp(0), seconds_to_skip(1)
{
  mysql_mutex_init(key_GR_LOCK_pipeline_stats_flow_control, &m_flow_control_lock, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_GR_COND_pipeline_stats_flow_control, &m_flow_control_cond);
}


Flow_control_module::~Flow_control_module()
{
  mysql_mutex_destroy(&m_flow_control_lock);
  mysql_cond_destroy(&m_flow_control_cond);
}


void
Flow_control_module::flow_control_step(Pipeline_stats_member_collector* member)
{
  if (--seconds_to_skip > 0)
    return;

  int32 holds= m_holds_in_period.exchange(0);
  Flow_control_mode fcm= static_cast<Flow_control_mode>(flow_control_mode_var);
  seconds_to_skip= flow_control_period_var;
  m_stamp++;

  /*
    Send statistics to other members
  */
  member->send_stats_member_message(fcm);

  DBUG_EXECUTE_IF("flow_control_simulate_delayed_members",
                  {
                    Pipeline_stats_member_message message(1000, 100000, 500,
                                                          500, 500, 0, 0, "",
                                                          "", 0, FCM_QUOTA);
                    Pipeline_member_stats stats;
                    stats.update_member_stats(message, 1);

                    m_info.clear();
                    m_info["m1"]= stats;
                    m_info["m2"]= stats;

                    holds= 2;
                  });

  switch(fcm)
  {
    case FCM_QUOTA:
    {
      double HOLD_FACTOR=
        1.0 - static_cast<double>(flow_control_hold_percent_var) / 100.0;
      double RELEASE_FACTOR=
        1.0 + static_cast<double>(flow_control_release_percent_var) / 100.0;
      double TARGET_FACTOR=
        static_cast<double>(flow_control_member_quota_percent_var) / 100.0;
      int64 max_quota= static_cast<int64>(flow_control_max_quota_var);

      /*
        Postponed transactions
      */
      int64 quota_size= m_quota_size.exchange(0);
      int64 quota_used= m_quota_used.exchange(0);
      int64 extra_quota=
          (quota_size > 0 && quota_used > quota_size) ? quota_used - quota_size : 0;

      /*
        Release waiting transactions on do_wait().
      */
      if (extra_quota > 0)
      {
        mysql_mutex_lock(&m_flow_control_lock);
        mysql_cond_broadcast(&m_flow_control_cond);
        mysql_mutex_unlock(&m_flow_control_lock);
      }

      if (holds > 0)
      {
        uint num_writing_members= 0, num_non_recovering_members= 0;
        int64 min_certifier_capacity= MAXTPS, min_applier_capacity= MAXTPS,
              safe_capacity= MAXTPS;

        Flow_control_module_info::iterator it= m_info.begin();
        while (it != m_info.end())
        {
          if (it->second.get_stamp() < (m_stamp - 10))
          {
            /*
              Purge member stats that were not updated on the last
              10 flow control steps.
            */
            m_info.erase(it++);
          }
          else
          {
            if (it->second.get_flow_control_mode() == FCM_QUOTA)
            {
              if (flow_control_certifier_threshold_var > 0
                  && it->second.get_delta_transactions_certified() > 0
                  && it->second.get_transactions_waiting_certification()
                     - flow_control_certifier_threshold_var > 0
                  && min_certifier_capacity
                     > it->second.get_delta_transactions_certified())
              {
                min_certifier_capacity= it->second.get_delta_transactions_certified();
              }

              if (it->second.get_delta_transactions_certified() > 0)
                safe_capacity= std::min(safe_capacity,
                                        it->second.get_delta_transactions_certified());

              if (flow_control_applier_threshold_var > 0
                  && it->second.get_delta_transactions_applied() > 0
                  && it->second.get_transactions_waiting_apply()
                     - flow_control_applier_threshold_var > 0)
              {
               if (min_applier_capacity > it->second.get_delta_transactions_applied())
                  min_applier_capacity= it->second.get_delta_transactions_applied();

                if (it->second.get_delta_transactions_applied() > 0)
                  num_non_recovering_members++;
              }

              if (it->second.get_delta_transactions_applied() > 0)
                safe_capacity= std::min(safe_capacity,
                                        it->second.get_delta_transactions_applied());

              if (it->second.get_delta_transactions_local() > 0)
                num_writing_members++;
            }
            ++it;
          }
        }

        // Avoid division by zero.
        num_writing_members= num_writing_members > 0 ? num_writing_members : 1;
        int64 min_capacity= (min_certifier_capacity > 0
                             && min_certifier_capacity < min_applier_capacity)
                             ? min_certifier_capacity : min_applier_capacity;

        // Minimum capacity will never be less than lim_throttle.
        int64 lim_throttle= static_cast<int64>
                            (0.05 * std::min(flow_control_certifier_threshold_var,
                                             flow_control_applier_threshold_var));
        if (flow_control_min_recovery_quota_var > 0 && num_non_recovering_members == 0)
          lim_throttle= flow_control_min_recovery_quota_var;
        if (flow_control_min_quota_var > 0)
          lim_throttle= flow_control_min_quota_var;

        min_capacity= std::max(std::min(min_capacity, safe_capacity), lim_throttle);
        quota_size= static_cast<int64>(min_capacity * HOLD_FACTOR);

        if (max_quota > 0)
          quota_size= std::min(quota_size, max_quota);

        if (num_writing_members > 1)
        {
          if (flow_control_member_quota_percent_var == 0)
            quota_size /= num_writing_members;
          else
            quota_size= static_cast<int64>
                        (static_cast<double>(quota_size) * TARGET_FACTOR);
        }

        quota_size = (quota_size - extra_quota > 1) ? quota_size - extra_quota : 1;

#ifndef DBUG_OFF
        log_message(MY_INFORMATION_LEVEL,
                    "Flow control: throttling to %ld commits per %ld sec, "
                    "with %d writing and %d non-recovering members, "
                    "min capacity %lld, lim throttle %lld", quota_size,
                    flow_control_period_var, num_writing_members,
                     num_non_recovering_members,
                    min_capacity, lim_throttle);
#endif
      }
      else
      {
        if (quota_size > 0 && flow_control_release_percent_var > 0
            && (quota_size * RELEASE_FACTOR) < MAXTPS)
        {
          int64 quota_size_next= static_cast<int64>(quota_size * RELEASE_FACTOR);
          quota_size= quota_size_next > quota_size ? quota_size_next : quota_size + 1;
        }
        else
          quota_size= 0;
      }

      if (max_quota > 0)
        quota_size= std::min(quota_size > 0 ? quota_size : max_quota, max_quota);

      m_quota_size.store(quota_size);
      m_quota_used.store(0);
      break;
    }

    case FCM_DISABLED:
      m_quota_size.store(0);
      m_quota_used.store(0);
      break;

    default:
      DBUG_ASSERT(0);
  }
}


int
Flow_control_module::handle_stats_data(const uchar *data,
                                       size_t len,
                                       const std::string& member_id)
{
  DBUG_ENTER("Flow_control_module::handle_stats_data");
  int error= 0;
  Pipeline_stats_member_message message(data, len);

  /*
    This method is called synchronously by communication layer, so
    we do not need concurrency control.
  */
  Flow_control_module_info::iterator it= m_info.find(member_id);
  if (it == m_info.end())
  {
    Pipeline_member_stats stats;

    std::pair<Flow_control_module_info::iterator, bool> ret=
      m_info.insert(std::pair<std::string, Pipeline_member_stats>
                    (member_id, stats));
    error= !ret.second;
    it= ret.first;
  }
  it->second.update_member_stats(message, m_stamp);

  /*
    Verify if flow control is required.
  */
  if (it->second.is_flow_control_needed())
  {
    ++m_holds_in_period;
#ifndef DBUG_OFF
    it->second.debug(it->first.c_str(),
                     m_quota_size.load(),
                     m_quota_used.load());
#endif
  }

  DBUG_RETURN(error);
}


Pipeline_member_stats *
Flow_control_module::get_pipeline_stats(const std::string& member_id)
{
  Pipeline_member_stats * member_pipeline_stats= NULL;
  Flow_control_module_info::iterator it= m_info.find(member_id);
  if (it != m_info.end())
  {
    member_pipeline_stats= new Pipeline_member_stats(it->second);
  }
  return member_pipeline_stats;
}


int32
Flow_control_module::do_wait()
{
  DBUG_ENTER("Flow_control_module::do_wait");
  int64 quota_size= m_quota_size.load();
  int64 quota_used= ++m_quota_used;

  if (quota_used > quota_size && quota_size != 0)
  {
    struct timespec delay;
    set_timespec(&delay, 1);

    mysql_mutex_lock(&m_flow_control_lock);
    mysql_cond_timedwait(&m_flow_control_cond, &m_flow_control_lock, &delay);
    mysql_mutex_unlock(&m_flow_control_lock);
  }

  DBUG_RETURN(0);
}
