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

#include <iostream>
#include "common/cde_typecache.h"
#include "dict/cde_dict.h"
#include "dml/cde_heap.h"
#include "handler/ha_cde.h"

#include "ut_cde_common.h"
#include "utils/ut_mysql_mock.h"

#include "index/dstore_scankey.h"

#include "catalog/dstore_fake_type.h"
#include "catalog/dstore_function.h"

using DSTORE::DatumGetInt32;
using DSTORE::FunctionCall2Coll;
using DSTORE::Int32GetDatum;
using DSTORE::MAINTAIN_ORDER;
using DSTORE::ScanKey;
using DSTORE::ScanKeyData;

namespace CDE {

class ut_cde_common_api : public CDETEST {
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

TEST_F(ut_cde_common_api, cde_common_api_test) {
  std::cout << "test in" << std::endl;
  int32_t ret;
  ScanKey scan_key = new ScanKeyData();

  /* BtreeInt4Cmp */
  const DSTORE::FuncCache *cache =
      CdeGetDstoreFuncCache(INT4OID, INT4OID, MAINTAIN_ORDER);
  EXPECT_NE(cache->fnAddr, nullptr);
  scan_key->skFunc.fnAddr = cache->fnAddr;
  scan_key->skFunc.fnOid = cache->fnOid;
  scan_key->skFunc.fnNargs = 2;
  scan_key->skFunc.fnStrict = true;
  scan_key->skFunc.fnRetset = false;
  scan_key->skFunc.fnMcxt = nullptr;
  scan_key->skCollation = 100;  // DEFAULT_COLLATION_OID;

  std::cout << "BtreeInt4Cmp in" << std::endl;
  ret = DatumGetInt32(FunctionCall2Coll(
      &scan_key->skFunc, scan_key->skCollation, Int32GetDatum((int32_t)0),
      Int32GetDatum((int32_t)1024)));
  EXPECT_EQ(ret, -1);
  ret = DatumGetInt32(FunctionCall2Coll(
      &scan_key->skFunc, scan_key->skCollation, Int32GetDatum((int32_t)6666),
      Int32GetDatum((int32_t)1024)));
  EXPECT_EQ(ret, 1);
  ret = DatumGetInt32(FunctionCall2Coll(
      &scan_key->skFunc, scan_key->skCollation, Int32GetDatum((int32_t)6666),
      Int32GetDatum((int32_t)6666)));
  EXPECT_EQ(ret, 0);
  std::cout << "BtreeInt4Cmp out" << std::endl;

  /* CharCmp */
  cache = CdeGetFuncCache(CDE_BIN_START_OID + 1, CDE_BIN_START_OID + 1,
                          MAINTAIN_ORDER);
  EXPECT_NE(cache->fnAddr, nullptr);
  scan_key->skFunc.fnAddr = cache->fnAddr;
  scan_key->skFunc.fnOid = cache->fnOid;
  scan_key->skFunc.fnNargs = 2;
  scan_key->skFunc.fnStrict = true;
  scan_key->skFunc.fnRetset = false;
  scan_key->skFunc.fnMcxt = nullptr;
  scan_key->skFunc.fnExtra = DSTORE::FnExtraMake(CDE_BIN_START_OID + 1, 0);
  scan_key->skCollation = 100;  // DEFAULT_COLLATION_OID;
  Datum *values0 = static_cast<Datum *>(CdeAlloc(2 * sizeof(Datum)));
  const char *c01 = "z";
  const char *c02 = "a";
  values0[0] = (Datum)c01;
  values0[1] = (Datum)c02;

  std::cout << "charcmp in" << std::endl;
  ret = DatumGetInt32(FunctionCall2Coll(
      &scan_key->skFunc, scan_key->skCollation, values0[0], values0[1]));
  EXPECT_EQ(ret, 1);
  std::cout << "charcmp out" << std::endl;
  free(values0);
  values0 = nullptr;

  /* TextEq (varchar)*/
  DSTORE::Oid varchar_oid = CDE_VARCHAR1B_OID;
  varchar_oid = (varchar_oid) | (255 << OID_COLL_SHIFT);
  cache = CdeGetFuncCache(varchar_oid, varchar_oid, MAINTAIN_ORDER);
  EXPECT_NE(cache->fnAddr, nullptr);
  scan_key->skFunc.fnAddr = cache->fnAddr;
  scan_key->skFunc.fnOid = cache->fnOid;
  scan_key->skFunc.fnNargs = 2;
  scan_key->skFunc.fnStrict = true;
  scan_key->skFunc.fnRetset = false;
  scan_key->skFunc.fnMcxt = nullptr;
  scan_key->skFunc.fnExtra = DSTORE::FnExtraMake(varchar_oid, 0);
  scan_key->skCollation = 100;  // DEFAULT_COLLATION_OID;
  const char *c1 = "aaa";
  const char *c2 = "aaa";
  Datum *values1 =
      static_cast<Datum *>(CdeAlloc(2 * (sizeof(Datum) + sizeof(CdeVarlena))));
  int len = 3;
  CdeVarlena *varlenas1 = (CdeVarlena *)&values1[2];
  varlenas1[0].len = len;
  varlenas1[0].data = (const unsigned char *)c1;
  values1[0] = static_cast<Datum>((uint64_t)&varlenas1[0]);
  varlenas1[1].len = len;
  varlenas1[1].data = (const unsigned char *)c2;
  values1[1] = static_cast<Datum>((uint64_t)&varlenas1[1]);

  values1[0] = static_cast<Datum>((uint64_t)&varlenas1[0] | DEREF_TAG);
  values1[1] = static_cast<Datum>((uint64_t)&varlenas1[1] | DEREF_TAG);
  std::cout << "TextEq in" << std::endl;
  ret = DatumGetInt32(FunctionCall2Coll(
      &scan_key->skFunc, scan_key->skCollation, values1[0], values1[1]));
  EXPECT_EQ(ret, 0);
  std::cout << "TextEq out" << std::endl;
  free(values1);
  values1 = nullptr;
  varlenas1 = nullptr;

  /* TextLt (varchar)*/
  cache = CdeGetFuncCache(varchar_oid, varchar_oid, MAINTAIN_ORDER);
  EXPECT_NE(cache->fnAddr, nullptr);
  scan_key->skFunc.fnAddr = cache->fnAddr;
  scan_key->skFunc.fnOid = cache->fnOid;
  scan_key->skFunc.fnNargs = 2;
  scan_key->skFunc.fnStrict = true;
  scan_key->skFunc.fnRetset = false;
  scan_key->skFunc.fnMcxt = nullptr;
  scan_key->skFunc.fnExtra = DSTORE::FnExtraMake(varchar_oid, 0);
  scan_key->skCollation = 100;  // DEFAULT_COLLATION_OID;
  const char *c3 = "aaa";
  const char *c4 = "bbb";
  Datum *values2 =
      static_cast<Datum *>(CdeAlloc(2 * (sizeof(Datum) + sizeof(CdeVarlena))));
  CdeVarlena *varlenas2 = (CdeVarlena *)&values2[2];
  varlenas2[0].len = len;
  varlenas2[0].data = (const unsigned char *)c3;
  values2[0] = static_cast<Datum>((uint64_t)&varlenas2[0]);
  varlenas2[1].len = len;
  varlenas2[1].data = (const unsigned char *)c4;
  values2[1] = static_cast<Datum>((uint64_t)&varlenas2[1]);

  values2[0] = static_cast<Datum>((uint64_t)&varlenas2[0] | DEREF_TAG);
  values2[1] = static_cast<Datum>((uint64_t)&varlenas2[1] | DEREF_TAG);

  std::cout << "TextLt in" << std::endl;
  ret = DatumGetInt32(FunctionCall2Coll(
      &scan_key->skFunc, scan_key->skCollation, values2[0], values2[1]));
  EXPECT_EQ(ret, -1);
  std::cout << "TextLt out" << std::endl;
  free(values2);
  values2 = nullptr;
  varlenas2 = nullptr;

  /* TextGt (varchar)*/
  cache = CdeGetFuncCache(varchar_oid, varchar_oid, MAINTAIN_ORDER);
  EXPECT_NE(cache->fnAddr, nullptr);
  scan_key->skFunc.fnAddr = cache->fnAddr;
  scan_key->skFunc.fnOid = cache->fnOid;
  scan_key->skFunc.fnNargs = 2;
  scan_key->skFunc.fnStrict = true;
  scan_key->skFunc.fnRetset = false;
  scan_key->skFunc.fnMcxt = nullptr;
  scan_key->skFunc.fnExtra = DSTORE::FnExtraMake(varchar_oid, 0);
  scan_key->skCollation = 100;  // DEFAULT_COLLATION_OID;
  const char *c5 = "zzz";
  const char *c6 = "aaa";
  Datum *values3 =
      static_cast<Datum *>(CdeAlloc(2 * (sizeof(Datum) + sizeof(CdeVarlena))));
  CdeVarlena *varlenas3 = (CdeVarlena *)&values3[2];
  varlenas3[0].len = len;
  varlenas3[0].data = (const unsigned char *)c5;
  values3[0] = static_cast<Datum>((uint64_t)&varlenas3[0]);
  varlenas3[1].len = len;
  varlenas3[1].data = (const unsigned char *)c6;
  values3[1] = static_cast<Datum>((uint64_t)&varlenas3[1]);

  values3[0] = static_cast<Datum>((uint64_t)&varlenas3[0] | DEREF_TAG);
  values3[1] = static_cast<Datum>((uint64_t)&varlenas3[1] | DEREF_TAG);
  std::cout << "TextGt in" << std::endl;
  ret = DatumGetInt32(FunctionCall2Coll(
      &scan_key->skFunc, scan_key->skCollation, values3[0], values3[1]));
  EXPECT_EQ(ret, 1);
  std::cout << "TextGt out" << std::endl;
  free(values3);
  values3 = nullptr;
  varlenas3 = nullptr;
}
} /* namespace CDE */
