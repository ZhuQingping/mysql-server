/*
  Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "errorcode/dstore_common_error_code_map.h"
#include "errorcode/dstore_index_error_code.h"

#include "common/cde_error.h"
#include "common/cde_errorcode.h"

#include "ut_cde_common.h"
#include "utils/ut_mysql_mock.h"

using DSTORE::COMMON_ERROR_UNDEFINED_ERROR;
using DSTORE::g_common_error_code_map;
using DSTORE::INDEX_ERROR_INSERT_UNIQUE_CHECK;

namespace CDE {

class UtErrorCode : public CDETEST {
 protected:
  void SetUp() override {
    CDETEST::SetUp();
    CDETEST::Bootstrap();
    CDETEST::Start();
  }

  void TearDown() override {
    CDETEST::Stop();
    CDETEST::TearDown();
  }
};

TEST_F(UtErrorCode, GetDstoreErrcodeTest) {
  auto errorCode = GetDstoreErrcode();
  EXPECT_NE(errorCode, COMMON_ERROR_UNDEFINED_ERROR);

#ifndef NDEBUG
  DBUG_PUSH("+d,errcode_getctx_null_injection");
  errorCode = GetDstoreErrcode();
  EXPECT_EQ(errorCode, CDE_ERROR);
  DBUG_PUSH("-d,errcode_getctx_null_injection");
#endif /* NDEBUG */
}

TEST_F(UtErrorCode, GetDstoreErrmsgTest) {
#ifndef NDEBUG
  static const char *UNKNOWN_ERROR_MSG = "Unknown Error";
#endif /* NDEBUG */

  std::string errorMsg = GetDstoreErrmsg();
  auto unexpectMsg =
      g_common_error_code_map[ERROR_GET_CODE(COMMON_ERROR_UNDEFINED_ERROR)]
          .message;
  EXPECT_NE(errorMsg, unexpectMsg);

#ifndef NDEBUG
  DBUG_PUSH("+d,errcode_getctx_null_injection");
  errorMsg = GetDstoreErrmsg();
  std::string expectMsg = UNKNOWN_ERROR_MSG;
  EXPECT_EQ(errorMsg, expectMsg);
  DBUG_PUSH("-d,errcode_getctx_null_injection");

#endif /* NDEBUG */
}

TEST_F(UtErrorCode, ConvertDstoreErrcodeToMysqlTest) {
  HaErrorCode errorCode = ConvertDstoreErrcodeToMysql(1);
  EXPECT_EQ(errorCode, (HaErrorCode)HA_ERR_GENERIC);
  errorCode = ConvertDstoreErrcodeToMysql(-1);
  EXPECT_EQ(errorCode, (HaErrorCode)HA_ERR_GENERIC);

  errorCode = ConvertDstoreErrcodeToMysql(INDEX_ERROR_INSERT_UNIQUE_CHECK);
  EXPECT_EQ(errorCode, (HaErrorCode)HA_ERR_FOUND_DUPP_KEY);
}

TEST_F(UtErrorCode, GetAndConvertErrcodeToMysqlTest) {
  HaErrorCode errorCode = GetAndConvertDstoreErrcodeToMysql();
  EXPECT_NE(errorCode, COMMON_ERROR_UNDEFINED_ERROR);

#ifndef NDEBUG
  DBUG_PUSH("+d,errcode_GetDstoreErrcode_injection");
  errorCode = GetAndConvertDstoreErrcodeToMysql();
  EXPECT_EQ(errorCode, (HaErrorCode)STORAGE_OK);
  DBUG_PUSH("-d,errcode_GetDstoreErrcode_injection");

  DBUG_PUSH("+d,errcode_getctx_null_injection");
  errorCode = GetAndConvertDstoreErrcodeToMysql();
  EXPECT_EQ(errorCode, (HaErrorCode)HA_ERR_GENERIC);
  DBUG_PUSH("-d,errcode_getctx_null_injection");
#endif /* NDEBUG */
}

TEST_F(UtErrorCode, ConvertErrcodeToMysqlTest) {
  // Test CDE_OK case
  HaErrorCode ret = ConvertErrcodeToMysql(CDE_OK, 0, nullptr);
  EXPECT_EQ(ret, (HaErrorCode)0);

  // Test valid handler error range
  HaErrorCode handlerError = HA_ERR_FIRST;
  while (handlerError <= HA_ERR_LAST) {
    ret = ConvertErrcodeToMysql(handlerError, 0, nullptr);
    EXPECT_EQ(ret, handlerError);
    ++handlerError;
  }

  // Test errors outside handler range
  handlerError = HA_ERR_FIRST - 1;
  ret = ConvertErrcodeToMysql(handlerError, 0, nullptr);
  EXPECT_EQ(ret, (HaErrorCode)HA_ERR_GENERIC);

  handlerError = HA_ERR_LAST + 1;
  ret = ConvertErrcodeToMysql(handlerError, 0, nullptr);
  EXPECT_EQ(ret, (HaErrorCode)HA_ERR_GENERIC);

  // Test dstore specific error
  ret = ConvertErrcodeToMysql(1, 0, nullptr);
  EXPECT_EQ(ret, (HaErrorCode)HA_ERR_GENERIC);

  // Test index error case
  ret = ConvertErrcodeToMysql(INDEX_ERROR_INSERT_UNIQUE_CHECK, 0, nullptr);
  EXPECT_EQ(ret, (HaErrorCode)HA_ERR_FOUND_DUPP_KEY);
}
} /* namespace CDE */

GTEST_API_ int main(int argc, char **argv) {
  printf("Running main() from %s\n", __FILE__);

  MY_INIT("error_code-test");
  CDE::CDETEST::InitOnce();
  testing::InitGoogleTest(&argc, argv);
  int exit_code = RUN_ALL_TESTS();
  CDE::CDETEST::Destroy();

  return exit_code;
}
