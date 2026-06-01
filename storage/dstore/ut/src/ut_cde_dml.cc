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

#include <gtest/gtest.h>
#include "securec.h"

#include "sql/field.h"
#include "sql/table.h"

#undef MAX_FILE_SIZE
#include "transaction/dstore_transaction_interface.h"

#include "ddl/cde_ddl.h"
#include "dict/cde_dict.h"
#include "dml/cde_dml.h"
#include "dml/cde_heap.h"

#include <signal.h>
#include "catalog/dstore_fake_type.h"
#include "ut_cde_dml.h"
#include "utils/ut_common.h"
#include "utils/ut_mysql_mock.h"

using DSTORE::StorageRelation;
namespace CDE {

#define DEREF_TAG (0x1111000000000000ULL)
#define DEREF_TAG_MASK (0xFFFF000000000000ULL)

void signalHandler(int signal) { exit(signal); }

class test_cde_dml_api : public CDETEST {
 protected:
  void SetUp() override {
    CDETEST::SetUp();
    CDETEST::Bootstrap();
    CDETEST::Start();
    CDETEST::StartSession();
  }

  void TearDown() override {
    CDETEST::StopSession();
    CDETEST::Stop();
    CDETEST::TearDown();
  }

  static void SetUpTestCase() {
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sa.sa_flags = 0;
    sigaction(SIGABRT, &sa, nullptr);
  }
  static void TearDownTestCase() {}
};

TEST_F(test_cde_dml_api, test_case_01) {
  return;
  int ret = 0;
  const char *table_name = "./test/ikun";
  // Oid col_oids[] = {CDE_INT4OID, CDE_VARCHAROID};
  // int col_num = sizeof(col_oids) / sizeof(col_oids[0]);

  TABLE *form = nullptr;
  HA_CREATE_INFO *create_info = new HA_CREATE_INFO();
  dd::Table *table_def = ut_mysql_dd_init();

  constexpr int index_col_num = 1;
  uint32_t index_cols[index_col_num] = {0};

  form = new ut_mysql_table(123456, "t2");
  form->key_info = new KEY();
  form->s->keys = 1;
  form->key_info->user_defined_key_parts = 1;
  form->key_info->name = "id_index";
  form->key_info->key_part = new KEY_PART_INFO();
  form->key_info->key_part->field = form->field[0];
  TransactionInterface::StartTrxCommand();
  TransactionInterface::SetSnapShot();
  ret = CdeCreateTable(nullptr, table_name, form, create_info, table_def);
  if (ret == 0) {
    TransactionInterface::CommitTrxCommand();
  } else {
    TransactionInterface::AbortTrx();
  }
  EXPECT_EQ(ret, 0);

  cde_dict_t *d_heap = DictSysGetTable(table_name);
  StorageRelation heap_rel = d_heap->dstore_relation;
  EXPECT_NE(heap_rel, nullptr);

  dstore_handler_t *dstore_handler = new dstore_handler_t;
  dstore_handler->table_handler = DictSysGetTable(table_name);
  void *mem = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfo(new (mem) session_rel_info(0));
  relInfo->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  ut_mysql_table *table = new ut_mysql_table(123456, "test", 114514, "ikun");

  TransactionInterface::StartTrxCommand();
  TransactionInterface::SetSnapShot();

  cde_session_t session;
  session.trxinfo.check_foreigns = false;
  auto colNum = dstore_handler->table_handler->GetTotalCols();
  dml_ins_ctx ins_ctx(session.get_trxinfo(), d_heap, relInfo, colNum, 0,
                      nullptr);
  ins_ctx.init();
  ret = CdeDmlWriteRow(table, table->record[0], &ins_ctx);
  EXPECT_EQ(ret, 0);

  TransactionInterface::IncreaseCommandCounter();

  dml_upd_ctx upd_ctx(session.get_trxinfo(), d_heap, relInfo, colNum, 0, true,
                      nullptr, nullptr, nullptr);
  upd_ctx.init();
  upd_ctx.set_old_ctid(&ins_ctx.tuple()->ctid);
  ret = CdeDmlUpdateRow(table, table->record[0], table->record[1], &upd_ctx);
  EXPECT_EQ(ret, 0);

  TransactionInterface::IncreaseCommandCounter();

  // todo:新开用例
  constexpr int mysql_key_len = 1 + sizeof(int);
  char *mysql_key_ptr = new char[mysql_key_len];
  mysql_key_ptr[0] = 0;
  int key_val = 114514;
  errno_t err = memcpy_s(mysql_key_ptr + 1, sizeof(int), &key_val, sizeof(int));
  EXPECT_EQ(err, 0);
  Datum *index_values = static_cast<Datum *>(
      CdeAlloc(index_col_num * (sizeof(Datum) + sizeof(CdeVarlena))));
  bool *index_is_nulls =
      static_cast<bool *>(CdeAlloc(index_col_num * sizeof(bool)));
  session_index_info *d_index =
      static_cast<session_index_info *>(CdeAlloc(sizeof(session_index_info)));
  d_index->shared_dict = new (std::nothrow) cde_dict_index_t();
  d_index->shared_dict->index_cols = index_cols;
  cde_dict_field_t *idx_fields = static_cast<cde_dict_field_t *>(
      CdeAlloc(sizeof(cde_dict_field_t) * index_col_num));
  for (uint32_t i = 0; i < index_col_num; i++) {
    cde_dict_field_t *idx_field = idx_fields + i;
    idx_field->prefix_len = 0;
    idx_field->len = 0;
    idx_field->mbminlen = 1;
    idx_field->mbmaxlen = 4;
    idx_field->attId = index_cols[i];
  }
  d_index->shared_dict->fields = idx_fields;
  // unsigned char** tmp_ptr = (unsigned char**)malloc(sizeof(unsigned char*) *
  // index_col_num);
  ret = key_mysql_to_dstore(table, d_index, index_col_num,
                            (unsigned char *)mysql_key_ptr, mysql_key_len,
                            index_values, index_is_nulls,
                            relInfo->rd_storage_releation->attr);
  EXPECT_EQ(ret, 0);

  // todo:新开用例
  unsigned char *mysql_buf = new unsigned char[1024];
  mysql_buf[0] = (unsigned char)0b11111111;
  Datum *output_values =
      static_cast<Datum *>(CdeAlloc(2 * (sizeof(Datum) + sizeof(CdeVarlena))));
  bool *output_is_nulls = static_cast<bool *>(CdeAlloc(2 * sizeof(bool)));

  ret = MysqlRecordToDstoreForInsert(
      table, table->record[0], upd_ctx.tuple_old(), &upd_ctx,
      upd_ctx.get_rel()->rd_storage_releation->attr, false);
  EXPECT_EQ(ret, 0);
  // remove DEREF_TAG in varchar
  output_values[1] =
      static_cast<Datum>((uint64_t)output_values[1] & (~DEREF_TAG));

  std::vector<MysqlAndDstoreIndex> columnsToDecode{{0, 0}, {1, 1}};
  DstoreHeapDataToMysql(table, mysql_buf, output_values, output_is_nulls,
                        nullptr, true, columnsToDecode.data(),
                        columnsToDecode.size(),
                        relInfo->rd_storage_releation->attr);

  CdeDropTable(nullptr, table_name, table_def);

  TransactionInterface::CommitTrxCommand();

  // for (uint32_t i = 0; i < index_col_num; i++) {
  //     if(tmp_ptr[i] != 0) CdeFree(tmp_ptr[i]);
  // }
  // CdeFree(tmp_ptr);

  CdeFree(output_is_nulls);
  CdeFree(output_values);
  CdeFree(idx_fields);

  delete d_index->shared_dict;
  CdeFree(d_index);
  CdeFree(index_is_nulls);
  delete[] mysql_buf;

  CdeFree(index_values);

  delete[] mysql_key_ptr;
  delete table;
  delete dstore_handler;
  delete form->key_info->key_part;
  delete form->key_info;
  delete form;

  ut_mysql_dd_release(table_def);
  delete create_info;

  std::free(mem);
}

// This testcase is used to verify set_varlena_datum, and can be erased when
// foreign key constraint is opened, because MTR testcases can covery the
// function very well.
TEST_F(test_cde_dml_api, test_set_varlena_datum) {
  std::unique_ptr<cde_dict_t> table = std::make_unique<cde_dict_t>();
  table->m_fieldLenInfos.emplace_back(0, 0, 0, 0);  // data for CDE_SHORT_OID
  table->m_fieldLenInfos.emplace_back(0, 3, 3,
                                      3);  // data for CDE_COMPACT_CHAR_OID
  table->m_fieldLenInfos.emplace_back(1, 0, 0,
                                      0);  // data for CDE_VARCHAR1B_OID
  table->m_fieldLenInfos.emplace_back(2, 0, 0,
                                      0);  // data for CDE_VARCHAR2B_OID

  dml_upd_ctx updCtx(nullptr, table.get(), nullptr,
                     table->m_fieldLenInfos.size(), 0, true, nullptr, nullptr,
                     nullptr);

  Datum value0 = 0;
  auto dataLen = 3;
  Datum value1 = (Datum)malloc(dataLen + 1);
  *(uint8_t *)value1 = dataLen;
  dataLen = 10;
  Datum value2 = (Datum)malloc(dataLen + 1);
  *(uint8_t *)value2 = dataLen;
  dataLen = 20;
  Datum value3 = (Datum)malloc(dataLen + 2);
  memset_s((void *)value3, dataLen + 2, 0, dataLen + 2);
  *(uint8_t *)value3 = dataLen;

  Datum values[4] = {value0, value1, value2, value3};
  CdeVarlena varlenas[4];
  updCtx.set_varlena_datum(INT2OID, 0, 0, values, varlenas);
  updCtx.set_varlena_datum(CDE_COMPACT_CHAR_OID, 1, 1, values, varlenas);
  EXPECT_NE(((uint64_t)values[1] & DEREF_TAG_MASK), (uint64_t)0);
  EXPECT_EQ(varlenas[1].len, (uint32_t)3);

  updCtx.set_varlena_datum(CDE_VARCHAR1B_OID, 2, 2, values, varlenas);
  EXPECT_NE(((uint64_t)values[2] & DEREF_TAG_MASK), (uint64_t)0);
  EXPECT_EQ(varlenas[2].len, (uint32_t)10);

  updCtx.set_varlena_datum(CDE_VARCHAR2B_OID, 3, 3, values, varlenas);
  EXPECT_NE(((uint64_t)values[3] & DEREF_TAG_MASK), (uint64_t)0);
  EXPECT_EQ(varlenas[3].len, (uint32_t)20);
}

TEST_F(test_cde_dml_api, test_cde_char_oid) {
  Oid type_oid = CDE_CHAR_OID;
  Field *field =
      new Field_string(3, true, "char_col", &my_charset_utf8mb4_general_ci);
  Datum dstore_ptr;
  CdeVarlena varlena;
  bool isAllocBlobMem;
  int ret = StoreMysqlFieldToDstoreFormat(
      reinterpret_cast<const unsigned char *>("abc"), type_oid, field, false,
      dstore_ptr, varlena, &isAllocBlobMem);
  EXPECT_EQ(ret, CDE_OK);
  delete field;
}

TEST_F(test_cde_dml_api, test_cde_compact_char_oid) {
  Oid type_oid = CDE_COMPACT_CHAR_OID;
  std::unique_ptr<Field> field(
      new Field_string(3, true, "char_col", &my_charset_utf8mb4_general_ci));
  Datum dstore_ptr;
  CdeVarlena varlena;
  bool isAllocBlobMem;
  EXPECT_DEATH(StoreMysqlFieldToDstoreFormat(
                   reinterpret_cast<const unsigned char *>("abc"), type_oid,
                   field.get(), false, dstore_ptr, varlena, &isAllocBlobMem),
               ".*");
}

} /* namespace CDE */
