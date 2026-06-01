/*
   Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "unittest/gunit/test_utils.h"

#include "sql/ddl_info.h"
#include "sql/sql_prepare.h"

#include "my_config.h"

#include <gtest/gtest.h>
#include <stddef.h>
#include <sys/types.h>
#include <algorithm>
#include <typeinfo>
#include <vector>

#include "lex_string.h"
#include "my_compiler.h"
#include "mysql/plugin.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/cache/element_map.h"
#include "sql/dd/dd.h"
#include "sql/dd/impl/cache/cache_element.h"
#include "sql/dd/impl/cache/free_list.h"
#include "sql/dd/impl/cache/shared_dictionary_cache.h"
#include "sql/dd/impl/cache/storage_adapter.h"
#include "sql/dd/impl/tables/tables.h"
#include "sql/dd/impl/types/charset_impl.h"
#include "sql/dd/impl/types/collation_impl.h"
#include "sql/dd/impl/types/column_statistics_impl.h"
#include "sql/dd/impl/types/event_impl.h"
#include "sql/dd/impl/types/procedure_impl.h"
#include "sql/dd/impl/types/schema_impl.h"
#include "sql/dd/impl/types/table_impl.h"
#include "sql/dd/impl/types/tablespace_impl.h"
#include "sql/dd/impl/types/view_impl.h"
// Avoid warning about deleting ptr to incomplete type on Win
#include "sql/dd/properties.h"
#include "sql/mdl.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "unittest/gunit/dd.h"
#include "unittest/gunit/parsertest.h"
#include "unittest/gunit/test_mdl_context_owner.h"
#include "unittest/gunit/test_utils.h"

namespace dstore_ddl_info_unittest {

using my_testing::Mock_error_handler;
using my_testing::Server_initializer;

using dd_unittest::nullp;

class Dstore_ddl_info_test : public ::testing::Test,
                             public Test_MDL_context_owner {
 public:
  dd::Schema_impl *mysql;

  void lock_object(const dd::String_type &name) {
    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, MYSQL_SCHEMA_NAME.str,
                     name.c_str(), MDL_EXCLUSIVE, MDL_TRANSACTION);
    EXPECT_FALSE(m_mdl_context.acquire_lock(
        &mdl_request, thd()->variables.lock_wait_timeout));
  }

 protected:
  Dstore_ddl_info_test() : mysql(nullptr) {}

  static void SetUpTestCase() {
    mdl_init();
    table_def_init();
  }

  static void TearDownTestCase() {
    dd::cache::Shared_dictionary_cache::shutdown();
    table_def_free();
    mdl_destroy();
  }

  void SetUp() override {
    m_init.SetUp();
    // Mark this as a dd system thread to skip MDL checks/asserts in the dd
    // cache.
    thd()->system_thread = SYSTEM_THREAD_DD_INITIALIZE;
    thd()->security_context()->skip_grants();
#ifndef NDEBUG
    dd::cache::Storage_adapter::s_use_fake_storage = true;
#endif /* !NDEBUG */
    dd::cache::Dictionary_client::Auto_releaser releaser(thd()->dd_client());
    mysql = new dd::Schema_impl();
    mysql->set_name("mysql");
    EXPECT_FALSE(thd()->dd_client()->store<dd::Schema>(mysql));
    EXPECT_LT(9999u, mysql->id());
    thd()->dd_client()->commit_modified_objects();

    mdl_locks_unused_locks_low_water = MDL_LOCKS_UNUSED_LOCKS_LOW_WATER_DEFAULT;
    max_write_lock_count = ULONG_MAX;
    m_mdl_context.init(this);
    EXPECT_FALSE(m_mdl_context.has_locks());
  }

  void TearDown() override {
    /*
      Explicit scope + auto releaser to make sure acquired objects are
      released before teardown of the thd.
    */
    {
      const dd::Schema *acquired_mysql = nullptr;
      dd::cache::Dictionary_client::Auto_releaser releaser(thd()->dd_client());
      EXPECT_FALSE(thd()->dd_client()->acquire<dd::Schema>(mysql->id(),
                                                           &acquired_mysql));
      EXPECT_NE(nullp<const dd::Schema>(), acquired_mysql);
      EXPECT_FALSE(thd()->dd_client()->drop(acquired_mysql));
      thd()->dd_client()->commit_modified_objects();
    }
    delete mysql;
    m_mdl_context.release_transactional_locks();
    m_mdl_context.destroy();
#ifndef NDEBUG
    dd::cache::Storage_adapter::s_use_fake_storage = false;
#endif /* !NDEBUG */
    m_init.TearDown();
  }

  void notify_shared_lock(MDL_context_owner *in_use,
                          bool needs_thr_lock_abort) override {
    in_use->notify_shared_lock(nullptr, needs_thr_lock_abort);
  }

  // Return dummy thd.
  THD *thd() { return m_init.thd(); }

  my_testing::Server_initializer m_init;  // Server initializer.

  MDL_context m_mdl_context;
  MDL_request m_request;

 private:
  Dstore_ddl_info_test(Dstore_ddl_info_test const &) = delete;
  Dstore_ddl_info_test &operator=(Dstore_ddl_info_test const &) = delete;
};

bool fill_thd(THD *thd) {
  sql_digest_state *parent_digest;
  PSI_statement_locker *parent_locker;
  bool error;

  Parser_state parser_state;
  if (parser_state.init(thd, thd->query().str, thd->query().length))
    return true;

  parser_state.m_lip.multi_statements = false;

  Mock_error_handler handler(thd, 0);
  lex_start(thd);

  parent_digest = thd->m_digest;
  parent_locker = thd->m_statement_psi;
  thd->m_digest = nullptr;
  thd->m_statement_psi = nullptr;
  error = parse_sql(thd, &parser_state, nullptr) || thd->is_error();
  thd->m_digest = parent_digest;
  thd->m_statement_psi = parent_locker;

  if (error) goto end;

  thd->lex->set_trg_event_type_for_tables();

  parent_locker = thd->m_statement_psi;
  thd->m_statement_psi = nullptr;
  thd->m_statement_psi = parent_locker;

end:
  lex_end(thd->lex);

  return error;
}

void fake_CdeGetDstoreTableDDInfo(String *packet, const dd::Table *) {
  const char *ddl_table_info =
      " /*!80041 "
      "dstore_ddl_comment='FOR_DD_REPLAY=1;tablerelid=16384;relblknum=131;"
      "relfileid=7;' */";
  packet->append(ddl_table_info);
}

void fake_CdeGetDstoreIndexDDInfo(String *packet, const dd::Table *, size_t) {
  const char *ddl_index_info =
      " /*!80041 dstore_ddl_comment "
      "'indexrelid=16385;relblknum=139;relfileid=7;' */";
  packet->append(ddl_index_info);
}

TEST_F(Dstore_ddl_info_test, DDLInfoGenerate) {
  THD *main_thd = thd();

  std::string sql_text = "create table test.t2(id int)";

  auto res = alloc_query(main_thd, sql_text.c_str(), sql_text.length());

  fill_thd(main_thd);
  EXPECT_EQ(res, false);

  dd::cache::Dictionary_client &dc = *thd()->dd_client();
  dd::cache::Dictionary_client::Auto_releaser releaser(&dc);

  std::unique_ptr<dd::Schema_impl> s(new dd::Schema_impl());
  s->set_name("test");
  EXPECT_FALSE(dc.store<dd::Schema>(s.get()));
  EXPECT_LT(9999u, s->id());

  std::unique_ptr<dd::Table> tab(new dd::Table_impl());
  EXPECT_FALSE(dc.store<dd::Table>(tab.get()));
  EXPECT_LT(9999u, tab->id());

  dd::Table *obj = tab.get();
  obj->set_name("t2");
  obj->set_engine("Dstore");
  obj->set_collation_id(1);
  obj->set_tablespace_id(1);
  obj->set_schema_id(s->id());

  //
  // Create a new column
  //
  dd::Column *col_obj1 = obj->add_column();
  col_obj1->set_name("col2");
  col_obj1->set_default_value_null(true);
  col_obj1->set_type(dd::enum_column_types::LONG);
  col_obj1->set_collation_id(1);

  //
  // Create a new indexes
  //
  dd::Index *idx_obj = obj->add_index();
  idx_obj->set_name("idx2");
  idx_obj->set_comment("Index2 comment");
  idx_obj->set_engine("Dstore");
  idx_obj->set_tablespace_id(1);
  idx_obj->add_element(col_obj1);

  MDL_REQUEST_INIT(&m_request, MDL_key::TABLE, "test", "t2", MDL_EXCLUSIVE,
                   MDL_TRANSACTION);

  const char *name = "t2";

  Mock_field_long t2_x("col2", /*is_nullable=*/false, /*is_unsigned=*/true);
  unique_ptr_destroy_only<Fake_TABLE> t2(new (main_thd->mem_root)
                                             Fake_TABLE(&t2_x));
  const int t2_idx =
      t2->create_index(t2->field[0], /*column2=*/nullptr, /*unique=*/false);

  auto hton = new (main_thd->mem_root) Fake_handlerton;
  hton->db_type = DB_TYPE_DSTORE;
  hton->get_table_dd_info = fake_CdeGetDstoreTableDDInfo;
  hton->get_index_dd_info = fake_CdeGetDstoreIndexDDInfo;
  NiceMock<Mock_HANDLER> m_handler(hton, t2->get_share());
  t2->set_handler(&m_handler);

  t2->field[0]->field_name = "col2";
  t2->alias = name;
  t2->get_share()->table_name.str = name;
  t2->get_share()->table_name.length = std::strlen(name);
  t2->get_share()->db.str = "test";
  t2->get_share()->db.length = 4;

  EXPECT_EQ(t2_idx, 0);

  TABLE *form = t2.get();

  ON_CALL(m_handler, table_type()).WillByDefault(testing::Return("Dstore"));

  generate_dstore_create_ddl_info(main_thd, main_thd->lex->create_info, form,
                                  obj);

  const char *target_gen_query =
      "USE test; DROP TABLE IF EXISTS `t2` /*!80041 "
      "dstore_ddl_comment='FOR_DD_REPLAY=1;' */; CREATE TABLE `test`.`t2` (  "
      "`col2` int unsigned NOT NULL DEFAULT '0',  KEY `unittest_index` "
      "(`col2`) /*!80000 INVISIBLE */ /*!80041 dstore_ddl_comment "
      "'indexrelid=16385;relblknum=139;relfileid=7;' */) ENGINE=Dstore "
      "/*!80041 "
      "dstore_ddl_comment='FOR_DD_REPLAY=1;tablerelid=16384;relblknum=131;"
      "relfileid=7;' */";
  EXPECT_STREQ(target_gen_query, main_thd->ddl_info_sql().ptr());
  dc.commit_modified_objects();
}

TEST_F(Dstore_ddl_info_test, DDLInfoParse) {
  THD *main_thd = thd();
  main_thd->for_ddl_info_replay = true;
  rds_use_ddl_info_replay = true;

  std::string sql_text =
      "drop table if exists test.t0  /*!80041 "
      "dstore_ddl_comment='FOR_DD_REPLAY=1;' */";

  auto res = alloc_query(main_thd, sql_text.c_str(), sql_text.length());

  fill_thd(main_thd);
  EXPECT_EQ(res, false);
  EXPECT_EQ(thd_sql_command(main_thd), SQLCOM_DROP_TABLE);

  std::string sql_text2 =
      "CREATE TABLE `test`.`t2` (  "
      "`col2` int unsigned NOT NULL DEFAULT '0',  KEY `unittest_index` "
      "(`col2`) /*!80000 INVISIBLE */ /*!80041 dstore_ddl_comment "
      "'indexrelid=16385;relblknum=139;relfileid=7;' */) /*!80041 "
      "dstore_ddl_comment='FOR_DD_REPLAY=1;tablerelid=16384;relblknum=131;"
      "relfileid=7;' */";
  res = alloc_query(main_thd, sql_text2.c_str(), sql_text2.length());

  fill_thd(main_thd);
  EXPECT_EQ(res, false);
  EXPECT_EQ(thd_sql_command(main_thd), SQLCOM_CREATE_TABLE);

  std::string sql_text3 = "drop table if exists test.t0";
  res = alloc_query(main_thd, sql_text3.c_str(), sql_text3.length());
  res = fill_thd(main_thd);
  // in replay process, drop table should have ddl_info_comment
  EXPECT_EQ(res, true);

  main_thd->for_ddl_info_replay = false;
  rds_use_ddl_info_replay = false;
}

}  // namespace dstore_ddl_info_unittest
