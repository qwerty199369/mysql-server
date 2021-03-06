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
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "auth_plain.h"
#include "mock/session.h"
#include "rapid/plugin/x/src/sql_user_require.h"

namespace xpl {

#define ER_SUCCESS 0

namespace test {
using namespace ::testing;

namespace {
const char *const EMPTY = "";
const char *const AUTH_DATA = "ALA_MA_KOTA";
const char *const MECHANISM = "MYSQL41";

AssertionResult assert_responce(
    const char *e1_expr, const char *e2_expr,
    const ngs::Authentication_interface::Response &e1,
    const ngs::Authentication_interface::Response &e2) {
  return (e1.data == e2.data && e1.status == e2.status &&
          e1.error_code == e2.error_code)
             ? ::testing::AssertionSuccess()
             : (::testing::AssertionFailure()
                << "Value of: " << e2_expr << "\nActual: {" << e2.status << ", "
                << e2.error_code << ", " << e2.data << "}\n"
                << "Expected: " << e1_expr << "\nWhich is: {" << e1.status
                << ", " << e1.error_code << ", " << e1.data << "}");
}

#define ASSERT_RESPONCE(a, b) ASSERT_PRED_FORMAT2(assert_responce, a, b);
}  // namespace

class Sasl_plain_auth_test : public Test {
 public:
  StrictMock<Mock_account_verification_handler> *mock_handler{
      ngs::allocate_object<StrictMock<Mock_account_verification_handler>>(
          nullptr)};
  Sasl_plain_auth auth{mock_handler};
  StrictMock<ngs::test::Mock_account_verification>
      mock_account_verification;

  typedef ngs::Authentication_interface::Response Response;
};

TEST_F(Sasl_plain_auth_test, handle_start_authenticate_succeeded) {
  EXPECT_CALL(*mock_handler, authenticate(_, AUTH_DATA))
      .WillOnce(Return(ngs::Success()));

  ASSERT_RESPONCE(Response(ngs::Authentication_interface::Succeeded),
                  auth.handle_start(MECHANISM, AUTH_DATA, EMPTY));
}

TEST_F(Sasl_plain_auth_test, handle_start_authenticate_failed) {
  ngs::Error_code expect_error(ER_NO_SUCH_USER, "Invalid user or password");
  EXPECT_CALL(*mock_handler, authenticate(_, AUTH_DATA))
      .WillOnce(Return(expect_error));

  ASSERT_RESPONCE(Response(ngs::Authentication_interface::Failed,
                           expect_error.error, expect_error.message),
                  auth.handle_start(MECHANISM, AUTH_DATA, EMPTY));
}

TEST_F(Sasl_plain_auth_test, handle_continue_without_previous_start) {
  ASSERT_RESPONCE(Response(ngs::Authentication_interface::Error,
                           ER_NET_PACKETS_OUT_OF_ORDER, EMPTY),
                  auth.handle_continue(AUTH_DATA));
}

TEST_F(Sasl_plain_auth_test, handle_continue_allways_failed) {
  EXPECT_CALL(*mock_handler, authenticate(_, AUTH_DATA))
      .WillOnce(Return(ngs::Success()));

  ASSERT_RESPONCE(Response(ngs::Authentication_interface::Succeeded),
                  auth.handle_start(MECHANISM, AUTH_DATA, EMPTY));

  ASSERT_RESPONCE(Response(ngs::Authentication_interface::Error,
                           ER_NET_PACKETS_OUT_OF_ORDER, EMPTY),
                  auth.handle_continue(AUTH_DATA));
}

}  // namespace test
}  // namespace xpl
