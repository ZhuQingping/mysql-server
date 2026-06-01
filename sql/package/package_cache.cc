/* Copyright (c) 2023, Huawei and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "my_macros.h"
#include "mysql/components/services/bits/psi_bits.h"
#include "mysql/psi/mysql_memory.h"

#include "sql/outline/outline_proc.h"
#include "sql/package/dbms_dstore_proc.h"

#include "sql/package/package.h"
#include "sql/package/package_common.h"
#include "sql/package/package_parse.h"
#include "sql/package/proc.h"
#include "sql/package/rds_backup_proc.h"
#include "sql/package/rpl_wal_proc.h"
#include "sql/package/show_native_procedure.h"
#include "sql/recyclebin/recycle_proc.h"
#include "sql/sp_head.h"
#include "sql/sql_filter/sql_filter_proc.h"

#ifndef NDEBUG
#include "sql/package/proc_dummy.h"
#endif  // NDEBUG

namespace im {

/* All package memory usage aggregation point */
PSI_memory_key key_memory_package;

const char *PACKAGE_SCHEMA = "mysql";

static bool package_inited = false;

#ifdef HAVE_PSI_INTERFACE
static PSI_memory_info package_memory[] = {
    {&key_memory_package, "im::package", 0, 0, PSI_DOCUMENT_ME}};

static void init_package_psi_key() {
  const char *category = "sql";
  int count;

  count = static_cast<int>(array_elements(package_memory));
  mysql_memory_register(category, package_memory, count);
}
#endif

/* Register all the native package element */
template <typename K, typename T>
static void register_package(const LEX_CSTRING &schema) {
  if (package_inited) {
    Package::instance()->register_element<K>(
        std::string(schema.str), T::instance()->str(), T::instance());
  }
}

/* Template of search package element */
template <typename T>
static const T *find_package_element(const std::string &schema_name,
                                     const std::string &element_name) {
  return Package::instance()->lookup_element<T>(schema_name, element_name);
}
/* Template instantiation */
template static const Proc *find_package_element(
    const std::string &schema_name, const std::string &element_name);

/**
  whether exist native proc by schema_name and proc_name

  @retval       true              Exist
  @retval       false             Not exist
*/
bool exist_native_proc(const char *db, const char *name) {
  return find_package_element<Proc>(std::string(db), std::string(name)) ? true
                                                                        : false;
}
/**
  Find the native proc and evoke the parse tree root

  @param[in]    THD               Thread context
  @param[in]    sp_name           Proc name
  @param[in]    pt_expr_list      Parameters

  @retval       parse_tree_root   Parser structure
*/
Parse_tree_root *find_native_proc_and_evoke(THD *thd, sp_name *sp_name,
                                            PT_item_list *pt_expr_list) {
  const Proc *proc = find_package_element<Proc>(
      std::string(sp_name->m_db.str), std::string(sp_name->m_name.str));

  return proc ? proc->PT_evoke(thd, pt_expr_list, proc) : nullptr;
}

/**
  Initialize Package context.
*/
void package_context_init() {
#ifdef HAVE_PSI_INTERFACE
  init_package_psi_key();
#endif

  package_inited = true;

#ifndef NDEBUG
  register_package<Proc, Proc_dummy>(PROC_DUMMY_SCHEMA);
  register_package<Proc, Proc_dummy_3>(PROC_DUMMY_SCHEMA);
#endif

  register_package<Proc, im::Show_native_procedure_proc>(im::ADMIN_PROC_SCHEMA);
  /* rds_backup.start_full_local_backup() */
  register_package<Proc, rds_backup::Proc_start_full_local_backup>(
      rds_backup::RDS_BACKUP_PROC_SCHEMA);
  /* rds_backup.stop_full_local_backup() */
  register_package<Proc, rds_backup::Proc_stop_full_local_backup>(
      rds_backup::RDS_BACKUP_PROC_SCHEMA);
  /* rds_backup.start_log_archive() */
  register_package<Proc, rds_backup::Proc_start_log_archive>(
      rds_backup::RDS_BACKUP_PROC_SCHEMA);
  /* rds_backup.stop_log_archive() */
  register_package<Proc, rds_backup::Proc_stop_log_archive>(
      rds_backup::RDS_BACKUP_PROC_SCHEMA);

  /* Register the native procedure: rpl_wal.pdb_promote() */
  register_package<Proc, rpl_wal::Proc_pdb_promote>(
      rpl_wal::RPL_WAL_PROC_SCHEMA);
  /* Register the native procedure: rpl_wal.pdb_demote() */
  register_package<Proc, rpl_wal::Proc_pdb_demote>(
      rpl_wal::RPL_WAL_PROC_SCHEMA);
  /* Register the native procedure: dbms_dstore_maintenance.show_dstore_info()
   */
  register_package<Proc, dbms_dstore_maintenance::Proc_show_dstore_info>(
      dbms_dstore_maintenance::DBMS_DSTORE_MAINTENANCE_PROC_SCHEMA);
  /* dbms_sqlfilter.add_sql_filter(....) */
  register_package<Proc, Sqlfilter_proc_add>(SQL_FILTER_PROC_SCHEMA);
  /* dbms_sqlfilter.delete_sql_filter(...) */
  register_package<Proc, Sqlfilter_proc_del>(SQL_FILTER_PROC_SCHEMA);
  /* dbms_sqlfilter.show_sql_filter() */
  register_package<Proc, Sqlfilter_proc_show>(SQL_FILTER_PROC_SCHEMA);
  /* dbms_sqlfilter.flush_sql_filter() */
  register_package<Proc, Sqlfilter_proc_flush>(SQL_FILTER_PROC_SCHEMA);
  /* dbms_sqlfilter.update_sql_filter() */
  register_package<Proc, Sqlfilter_proc_update>(SQL_FILTER_PROC_SCHEMA);
  /* dbms_outln.add_optimizer_outline(...) */
  register_package<Proc, Outline_optimizer_proc_add>(OUTLINE_PROC_SCHEMA);
  /* dbms_outln.add_index_outline(...) */
  register_package<Proc, Outline_index_proc_add>(OUTLINE_PROC_SCHEMA);
  /* dbms_outln.del_outline(...) */
  register_package<Proc, Outline_proc_del>(OUTLINE_PROC_SCHEMA);
  /* dbms_outln.flush_outline() */
  register_package<Proc, Outline_proc_flush>(OUTLINE_PROC_SCHEMA);
  /* dbms_outln.show_outline() */
  register_package<Proc, Outline_proc_show>(OUTLINE_PROC_SCHEMA);
  /* dbms_outln.preview_outline() */
  register_package<Proc, Outline_proc_preview>(OUTLINE_PROC_SCHEMA);

  /* dbms_recyclebin.show_tables() */
  register_package<Proc, im::recycle_bin::Recycle_proc_show>(
      im::recycle_bin::RECYCLE_BIN_PROC_SCHEMA);
  /* dbms_recyclebin.purge_table() */
  register_package<Proc, im::recycle_bin::Recycle_proc_purge>(
      im::recycle_bin::RECYCLE_BIN_PROC_SCHEMA);
  /* dbms_recycle.restore_table(...) */
  register_package<Proc, im::recycle_bin::Recycle_proc_restore>(
      im::recycle_bin::RECYCLE_BIN_PROC_SCHEMA);
  /* dbms_recycle.restore_database(...) */
  register_package<Proc, im::recycle_bin::Recycle_proc_restore_db>(
      im::recycle_bin::RECYCLE_BIN_PROC_SCHEMA);
}

} /* namespace im */
