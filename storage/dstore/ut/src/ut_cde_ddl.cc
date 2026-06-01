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
#include "ddl/cde_dd_table.h"
#include "ddl/cde_ddl.h"
#include "dict/cde_dict.h"

#include "transaction/dstore_transaction_interface.h"
#include "ut_cde_ddl.h"
#include "utils/ut_mysql_mock.h"

#include "index/dstore_scankey.h"

#include "catalog/dstore_function.h"

class THD {
 public:
  explicit THD(bool enable_plugins)
      : for_ddl_info_replay(false), m_enable_plugins(enable_plugins) {}

  bool for_ddl_info_replay;

 private:
  bool m_enable_plugins;
};

namespace CDE {

template <typename Table>
cde_dict_t *CdeDdOpenTableOne(THD *thd, const char *name, const Table *dd_table,
                              const TABLE *form_table,
                              std::deque<const char *> &fk_names,
                              bool allowEviction = true);

class ut_cde_ddl_api : public CDETEST {
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

TEST_F(ut_cde_ddl_api,
       cde_ddl_create_table_test_01)  // case: create table no index
{
  const char table_name[] = "t1";
  TABLE *form = nullptr;
  HA_CREATE_INFO *create_info = new HA_CREATE_INFO();
  dd::Table *table_def = ut_mysql_dd_init();

  TransactionInterface::StartTrxCommand();
  TransactionInterface::SetSnapShot();
  int ret = CdeCreateTable(nullptr, table_name, form, create_info, table_def);
  if (ret == 0) {
    TransactionInterface::CommitTrxCommand();
  } else {
    TransactionInterface::AbortTrx();
  }
  EXPECT_NE(ret, 0);
  form = new ut_mysql_table(123456, "t1");
  form->s->primary_key = MAX_KEY;  // no primary key
  TransactionInterface::StartTrxCommand();
  TransactionInterface::SetSnapShot();
  ret = CdeCreateTable(nullptr, table_name, form, create_info, table_def);
  if (ret == 0) {
    TransactionInterface::CommitTrxCommand();
  } else {
    TransactionInterface::AbortTrx();
  }
  EXPECT_EQ(ret, 0);

  TransactionInterface::StartTrxCommand();
  TransactionInterface::SetSnapShot();
  ret = CdeCreateTable(nullptr, table_name, form, create_info, table_def);
  if (ret == 0) {
    TransactionInterface::CommitTrxCommand();
  } else {
    TransactionInterface::AbortTrx();
  }
  EXPECT_NE(ret, 0);

  delete form;
  ut_mysql_dd_release(table_def);
  delete create_info;
}

TEST_F(ut_cde_ddl_api, cde_ddl_rename_table_test)  // case: rename table
{
  const char oldname[] = "./test/t_oldname";
  const char newname[] = "./test/t_newname";
  TABLE *form = nullptr;
  HA_CREATE_INFO *create_info = new HA_CREATE_INFO();
  dd::Table *table_def = ut_mysql_dd_init();
  form = new ut_mysql_table(123456, "./test/t_oldname");
  form->s->primary_key = MAX_KEY;  // no primary key
  TransactionInterface::StartTrxCommand();
  TransactionInterface::SetSnapShot();
  int ret = CdeCreateTable(nullptr, oldname, form, create_info,
                           table_def);  // create table no index
  if (ret == 0) {
    TransactionInterface::CommitTrxCommand();
  } else {
    TransactionInterface::AbortTrx();
  }
  EXPECT_EQ(ret, 0);
  cde_dict_t *dictTable = DictSysGetTable(oldname);
  EXPECT_NE(dictTable, nullptr);
  ret = CdeRenameTable(nullptr, oldname, newname, dictTable, table_def);
  EXPECT_EQ(ret, 0);
  delete form;
  ut_mysql_dd_release(table_def);
  delete create_info;
}

TEST_F(ut_cde_ddl_api, cde_ddl_open_existing_table_test) {
  dd::Table *table_def = ut_mysql_dd_init();
  TABLE *form = new ut_mysql_table(123456, "leos_natek_table");
  HA_CREATE_INFO *create_info = new HA_CREATE_INFO();
  const char *table_name = "./test/leos_natek_table";
  /** No primary key. */
  form->s->primary_key = MAX_KEY;

  TransactionInterface::StartTrxCommand();
  TransactionInterface::SetSnapShot();
  int ret = CdeCreateTable(nullptr, table_name, form, create_info, table_def);
  EXPECT_EQ(ret, 0);
  TransactionInterface::CommitTrxCommand();

  cde_dict_t *tableT1FromDict = DictSysGetTable(table_name);
  std::deque<const char *> fk_names;
  /** THD should not be dereferenced by CdeDdOpenTableOne, so it is safe. */
  THD *thd = new THD(false);
  cde_dict_t *tableT1 =
      CdeDdOpenTableOne(thd, table_name, table_def, form, fk_names);
  EXPECT_EQ(tableT1FromDict, tableT1);

  delete form;
  ut_mysql_dd_release(table_def);
  delete create_info;
  delete thd;
}

int CdeStrcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }

TEST_F(ut_cde_ddl_api, cde_ddl_info_generate) {
  const char table_name[] = "t2";
  TABLE *form = nullptr;
  HA_CREATE_INFO *create_info = new HA_CREATE_INFO();
  dd::Table *table_def = ut_mysql_dd_init();
  table_def->set_engine("Dstore");

  form = new ut_mysql_table(123456, "t2");
  form->s->primary_key = MAX_KEY;  // no primary key
  form->key_info = new KEY();
  form->s->keys = 1;
  form->key_info->user_defined_key_parts = 1;
  form->key_info->name = "id_index";
  form->key_info->key_part = new KEY_PART_INFO();
  form->key_info->key_part->field = form->field[0];
  form->key_info->key_part->fieldnr = 1;
  dd::Index *idx_obj = table_def->add_index();
  idx_obj->set_name(form->key_info->name);
  idx_obj->set_type(dd::Index::IT_MULTIPLE);
  dd::Column *dd_column = table_def->add_column();
  dd_column->set_name("id");
  dd_column->set_type(dd::enum_column_types::LONG);

  TransactionInterface::StartTrxCommand();
  TransactionInterface::SetSnapShot();
  int ret = CdeCreateTable(nullptr, table_name, form, create_info, table_def);
  if (ret == 0) {
    TransactionInterface::CommitTrxCommand();
  } else {
    TransactionInterface::AbortTrx();
  }
  EXPECT_EQ(ret, 0);

  const char *ddl_table_info =
      " /*!80041 "
      "dstore_ddl_comment='FOR_DD_REPLAY=1;tablerelid=16384;relblknum=131;"
      "relfileid=5121;' */";

  String packet;
  CdeGetDstoreTableDDInfo(&packet, table_def);
  EXPECT_STREQ(packet.ptr(), ddl_table_info);

  const char *ddl_index_info =
      " /*!80041 dstore_ddl_comment "
      "'indexrelid=16385;relblknum=139;relfileid=5121;' */";

  String packet1;
  CdeGetDstoreIndexDDInfo(&packet1, table_def, 0);
  EXPECT_STREQ(packet1.ptr(), ddl_index_info);

  delete form->key_info->key_part;
  delete form->key_info;
  delete form;
  ut_mysql_dd_release(table_def);
  delete create_info;
}

} /* namespace CDE */
