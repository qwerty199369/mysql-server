/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "protocol_t.h"
#include "message_helpers.h"


namespace xcl {
namespace test {

using ::testing::StrEq;
using ::testing::Eq;

class Xcl_protocol_impl_tests_notices : public Xcl_protocol_impl_tests {
 public:
  using Notice      = ::Mysqlx::Notice::Frame;
  using Notice_type = ::Mysqlx::Notice::Frame::Type;

 public:
  void expect_notice_handler(
      Mock_handlers *mock,
      const Notice_type expected_type,
      const std::string &expected_payload,
      const Handler_result consume = Handler_result::Continue) {
    const bool expect_global = true;

    EXPECT_CALL(*mock, notice_handler(
        m_sut.get(),
        expect_global,
        expected_type,
        StrEq(expected_payload),
        static_cast<uint32_t>(expected_payload.size())))
      .WillOnce(Return(consume));
  }

  void expect_notice_handler_empty_payload(
      Mock_handlers *mock,
      const Notice_type expected_type,
      const Handler_result consume = Handler_result::Continue) {
    const bool expect_global = true;
    const uint32 expected_payload_size = 0;

    EXPECT_CALL(*mock, notice_handler(
        m_sut.get(),
        expect_global,
        expected_type,
        nullptr,
        expected_payload_size)).WillOnce(Return(consume));
  }

  template<typename Message_type>
  void expect_recv_message_handler(
      const Message_from_str<Message_type> &message,
      const Handler_result should_consumed = Handler_result::Continue) {
    const Message_type &m = message;
    expect_recv_message_handler(m, should_consumed);
  }

  template<typename Message_type>
  void expect_recv_message_handler(
      const Message_type &message,
      const Handler_result should_consumed = Handler_result::Continue) {
    EXPECT_CALL(m_mock_message_handler, received_message_handler(
        m_sut.get(),
        Server_message<Message_type>::get_id(),
         Cmp_msg(message))).WillOnce(Return(should_consumed));
  }

  StrictMock<Mock_handlers> m_mock_message_handler;
  StrictMock<Mock_handlers> m_mock_notice_handlers[2];
  const Message_from_str<Notice> m_msg_notice1{
    "type: 2 "
  };
  const Message_from_str<Notice> m_msg_notice2{
    "type: 3 "
    "payload: \"0123456789\""
  };
  const std::string m_notice1_payload = "";
  const std::string m_notice2_payload = "0123456789";
};

TEST_F(Xcl_protocol_impl_tests_notices, recv_notice_when_no_handler_installed) {
  XProtocol::Server_message_type_id out_id;
  XError out_error;

  expect_read_message(m_msg_notice1);
  auto message = m_sut->recv_single_message(&out_id, &out_error);

  EXPECT_THAT(*message, Cmp_msg(m_msg_notice1));
}

TEST_F(Xcl_protocol_impl_tests_notices, recv_notice_handler) {
  XProtocol::Server_message_type_id out_id;
  XError                out_error;

  expect_read_message(m_msg_notice1);
  expect_notice_handler_empty_payload(
      &m_mock_notice_handlers[0],
      Notice_type::Frame_Type_SESSION_VARIABLE_CHANGED);

  m_sut->add_notice_handler(
      m_mock_notice_handlers[0].get_mock_lambda_notice_handler());

  auto message = m_sut->recv_single_message(&out_id, &out_error);

  EXPECT_THAT(*message, Cmp_msg(m_msg_notice1));
}

TEST_F(Xcl_protocol_impl_tests_notices,
       recv_msg_handler_called_before_notice_handler) {
  XProtocol::Server_message_type_id out_id;
  XError                out_error;
  InSequence            sequence;

  expect_read_message(m_msg_notice1);
  expect_recv_message_handler(m_msg_notice1);
  expect_notice_handler_empty_payload(
      &m_mock_notice_handlers[0],
      Notice_type::Frame_Type_SESSION_VARIABLE_CHANGED);

  m_sut->add_received_message_handler(
      m_mock_message_handler.get_mock_lambda_received_message_handler());
  m_sut->add_notice_handler(
      m_mock_notice_handlers[0].get_mock_lambda_notice_handler());

  auto message = m_sut->recv_single_message(&out_id, &out_error);

  EXPECT_THAT(*message, Cmp_msg(m_msg_notice1));
}

TEST_F(Xcl_protocol_impl_tests_notices,
       recv_msg_handler_consumed_notice_recv_next_msg) {
  XProtocol::Server_message_type_id out_id;
  XError                out_error;
  ::Mysqlx::Ok          msg_ok;
  InSequence            sequence;

  expect_read_message(m_msg_notice1);
  expect_recv_message_handler(m_msg_notice1, Handler_result::Consumed);
  expect_read_message_without_payload(msg_ok);
  expect_recv_message_handler(msg_ok);

  m_sut->add_received_message_handler(
      m_mock_message_handler.get_mock_lambda_received_message_handler());
  m_sut->add_notice_handler(
      m_mock_notice_handlers[0].get_mock_lambda_notice_handler());

  auto message = m_sut->recv_single_message(&out_id, &out_error);

  EXPECT_THAT(*message, Cmp_msg(msg_ok));
}


TEST_F(Xcl_protocol_impl_tests_notices,
       recv_other_msg_notice_handler_not_called) {
  XProtocol::Server_message_type_id out_id;
  XError                out_error;
  auto                  msg_error =
      Server_message<::Mysqlx::Error>::make_required();

  expect_read_message(msg_error);

  m_sut->add_notice_handler(
      m_mock_notice_handlers[0].get_mock_lambda_notice_handler());

  auto message = m_sut->recv_single_message(&out_id, &out_error);

  EXPECT_THAT(*message, Cmp_msg(msg_error));
}

TEST_F(Xcl_protocol_impl_tests_notices, recv_multiple_notice_handlers) {
  XProtocol::Server_message_type_id out_id;
  XError                out_error;
  InSequence            sequence;

  expect_read_message(m_msg_notice1);
  expect_notice_handler_empty_payload(
      &m_mock_notice_handlers[1],
      Notice_type::Frame_Type_SESSION_VARIABLE_CHANGED);
  expect_notice_handler_empty_payload(
      &m_mock_notice_handlers[0],
      Notice_type::Frame_Type_SESSION_VARIABLE_CHANGED);

  m_sut->add_notice_handler(
      m_mock_notice_handlers[0].get_mock_lambda_notice_handler());
  m_sut->add_notice_handler(
      m_mock_notice_handlers[1].get_mock_lambda_notice_handler());

  auto message = m_sut->recv_single_message(&out_id, &out_error);

  EXPECT_THAT(*message, Cmp_msg(m_msg_notice1));
}

TEST_F(Xcl_protocol_impl_tests_notices,
       recv_pushed_multiple_notice_handler_and_poped_some_of_them) {
  XProtocol::Server_message_type_id out_id;
  XError                out_error;

  expect_read_message(m_msg_notice1);

  // The handlers are held in "stack" container,
  // thus after calling "pop" should leave
  // in the container first handler
  expect_notice_handler_empty_payload(
      &m_mock_notice_handlers[0],
      Notice_type::Frame_Type_SESSION_VARIABLE_CHANGED);

  m_sut->add_notice_handler(
      m_mock_notice_handlers[0].get_mock_lambda_notice_handler());
  const auto id = m_sut->add_notice_handler(
      m_mock_notice_handlers[1].get_mock_lambda_notice_handler());
  m_sut->remove_notice_handler(id);

  auto message = m_sut->recv_single_message(&out_id, &out_error);

  EXPECT_THAT(*message, Cmp_msg(m_msg_notice1));
}

TEST_F(Xcl_protocol_impl_tests_notices,
       recv_multiple_notice_handlers_consume1) {
  XProtocol::Server_message_type_id out_id;
  XError                out_error;
  InSequence            sequence;

  expect_read_message(m_msg_notice1);
  // First handler returns true, and consumes the notice.
  // Because of that second handler isn't called
  // for this notice, and `recv_single_message` must receive
  // next message.
  expect_notice_handler_empty_payload(
      &m_mock_notice_handlers[1],
      Notice_type::Frame_Type_SESSION_VARIABLE_CHANGED,
      Handler_result::Consumed);

  expect_read_message(m_msg_notice2);
  expect_notice_handler(
      &m_mock_notice_handlers[1],
      Notice_type::Frame_Type_SESSION_STATE_CHANGED,
      m_notice2_payload);
  expect_notice_handler(
      &m_mock_notice_handlers[0],
      Notice_type::Frame_Type_SESSION_STATE_CHANGED,
      m_notice2_payload);

  m_sut->add_notice_handler(
      m_mock_notice_handlers[0].get_mock_lambda_notice_handler());
  m_sut->add_notice_handler(
      m_mock_notice_handlers[1].get_mock_lambda_notice_handler());

  auto message = m_sut->recv_single_message(&out_id, &out_error);

  EXPECT_THAT(*message, Cmp_msg(m_msg_notice2));
}

TEST_F(Xcl_protocol_impl_tests_notices,
       recv_multiple_notice_handlers_consume2) {
  XProtocol::Server_message_type_id out_id;
  XError                out_error;
  InSequence            sequence;

  expect_read_message(m_msg_notice1);
  // Second handler returns true, and consumes the notice.
  // Because of that second handler isn't called
  // for this notice, and `recv_single_message` must receive
  // next message.
  expect_notice_handler_empty_payload(
      &m_mock_notice_handlers[1],
      Notice_type::Frame_Type_SESSION_VARIABLE_CHANGED);
  expect_notice_handler_empty_payload(
      &m_mock_notice_handlers[0],
      Notice_type::Frame_Type_SESSION_VARIABLE_CHANGED,
      Handler_result::Consumed);

  expect_read_message(m_msg_notice2);
  expect_notice_handler(
      &m_mock_notice_handlers[1],
      Notice_type::Frame_Type_SESSION_STATE_CHANGED,
      m_notice2_payload);
  expect_notice_handler(
      &m_mock_notice_handlers[0],
      Notice_type::Frame_Type_SESSION_STATE_CHANGED,
      m_notice2_payload);

  m_sut->add_notice_handler(
      m_mock_notice_handlers[0].get_mock_lambda_notice_handler());
  m_sut->add_notice_handler(
      m_mock_notice_handlers[1].get_mock_lambda_notice_handler());

  auto message = m_sut->recv_single_message(&out_id, &out_error);

  EXPECT_THAT(*message, Cmp_msg(m_msg_notice2));
}

}  // namespace test
}  // namespace xcl

