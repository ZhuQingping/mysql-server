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
#include <limits>
#include <vector>

#include "boot/cde_instance.h"
#include "cde_dd_table.h"
#include "common/cde_alloc.h"
#include "common/cde_compare_utils.h"
#include "dd_helper.h"
#include "ddl/cde_ddl.h"
#include "ddl/cde_tablespace.h"
#include "dml/cde_heap.h"
#include "securec.h"

#include "create_field.h"
#include "handler/ha_cde.h"
#include "mysql/plugin.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/dictionary.h"
#include "sql/dd/impl/types/foreign_key_impl.h"
#include "sql/dd/impl/types/index_impl.h"
#include "sql/dd/string_type.h"
#include "sql/dd/types/column_type_element.h"
#include "sql/dd/types/index.h"
#include "sql/dd/types/partition.h"
#include "sql/dd/types/partition_index.h"
#include "sql/dd/types/table.h"
#include "sql/dd_table_share.h"
#include "sql/sql_alter.h"
#include "sql/sql_class.h"
#include "sql_base.h"
#include "sql_list.h"
#include "sql_table.h"

using DSTORE::PageId;
using DSTORE::ScanKey;
using DSTORE::StorageRelation;

namespace CDE {

template <typename Table>
void CdeDdTableSetSePrivateData(Table *table_def,
                                DSTORE::StorageRelation table_rel,
                                cde_dict_t *dict_table) {
  CDE_ASSERT_DEBUG(table_def != nullptr);
  CDE_ASSERT_DEBUG(table_rel != nullptr);
  CDE_ASSERT_DEBUG(dict_table != nullptr);
  table_def->set_se_private_id(dict_table->m_id);
  table_def->se_private_data().set(
      object_relfileid, static_cast<uint>(table_rel->rel->relfileid));
  table_def->se_private_data().set(
      object_relblknum, static_cast<uint>(table_rel->rel->relblknum));
  if (table_rel->rel->rellobfileid != DSTORE::INVALID_VFS_FILE_ID &&
      table_rel->rel->rellobblknum != DSTORE::DSTORE_INVALID_BLOCK_NUMBER) {
    table_def->se_private_data().set(
        object_rellobfileid, static_cast<uint>(table_rel->rel->rellobfileid));
    table_def->se_private_data().set(
        object_rellobblknum, static_cast<uint>(table_rel->rel->rellobblknum));
  }
  table_def->se_private_data().set(
      object_reltablespace, static_cast<uint>(table_rel->rel->reltablespace));
  if (unlikely(g_guc.enableStandbyRole)) {
    /* The physical standby does not support starting transactions and has no
    visibility issues. Setting the minimum CSN is sufficient. */
    table_def->se_private_data().set(object_tablecreatecsn,
                                     DSTORE::COMMITSEQNO_FIRST_NORMAL);
  } else {
    table_def->se_private_data().set(object_tablecreatecsn,
                                     dict_table->m_rebuild_csn);
  }
}

template void CdeDdTableSetSePrivateData<dd::Table>(dd::Table *,
                                                    DSTORE::StorageRelation,
                                                    cde_dict_t *);

template void CdeDdTableSetSePrivateData<dd::Partition>(dd::Partition *,
                                                        DSTORE::StorageRelation,
                                                        cde_dict_t *);

void CdeDdTableSetSePrivateDataAutoInc(dd::Table *tableDef,
                                       const uint64_t &autoInc) {
  tableDef->se_private_data().set(auto_inc, autoInc);
}

template <typename Table>
void CdeDdIndexSetSePrivateData(Table *table_def, const cde_dict_t *dictTable) {
  CDE_ASSERT_DEBUG(table_def != nullptr);

  for (auto dd_index :
       *table_def->indexes()) {  // dd::Table and index_rel_vector must
                                 // correspond to each other
    const cde_dict_index_t *index = DDFindIndex(dictTable, dd_index);
    CDE_ASSERT(index);

    dd_index->se_private_data().set(object_indexrelid, index->rel->relOid);
    dd_index->se_private_data().set(object_relfileid,
                                    index->rel->rel->relfileid);
    dd_index->se_private_data().set(object_relblknum,
                                    index->rel->rel->relblknum);
    if (unlikely(g_guc.enableStandbyRole)) {
      dd_index->se_private_data().set(object_indexcreatecsn,
                                      DSTORE::COMMITSEQNO_FIRST_NORMAL);
    } else {
      dd_index->se_private_data().set(object_indexcreatecsn, index->index_csn);
    }
  }
}

template void CdeDdIndexSetSePrivateData<dd::Table>(
    dd::Table *, const cde_dict_t *dictTable);
template void CdeDdIndexSetSePrivateData<dd::Partition>(
    dd::Partition *, const cde_dict_t *dictTable);

template <typename Table>
uint64_t CdeDdTableGetSePrivateDataAutoInc(const Table *tableDef) {
  uint64_t autoIncVal = 0;
  if (tableDef->se_private_data().exists(auto_inc)) {
    tableDef->se_private_data().get(auto_inc, &autoIncVal);
  }
  return static_cast<uint64_t>(autoIncVal);
}

template uint64_t CdeDdTableGetSePrivateDataAutoInc<dd::Table>(
    const dd::Table *tableDef);
template uint64_t CdeDdTableGetSePrivateDataAutoInc<dd::Partition>(
    const dd::Partition *tableDef);

template <typename Table>
uint16_t CdeDdTableGetSePrivateDataRelfileid(const Table *table_def) {
  uint relfileid = 0;
  if (table_def->se_private_data().exists(object_relfileid)) {
    table_def->se_private_data().get(object_relfileid, &relfileid);
  }
  return static_cast<uint16_t>(relfileid);
}

template uint16_t CdeDdTableGetSePrivateDataRelfileid<dd::Table>(
    const dd::Table *table_def);
template uint16_t CdeDdTableGetSePrivateDataRelfileid<dd::Partition>(
    const dd::Partition *table_def);

template <typename Table>
DSTORE::FileId CdeTableGetLobFileId(const Table *tableDef) {
  DSTORE::FileId fileid = DSTORE::INVALID_VFS_FILE_ID;
  if (tableDef->se_private_data().exists(object_rellobfileid)) {
    tableDef->se_private_data().get(object_rellobfileid, &fileid);
  }
  return fileid;
}

template DSTORE::FileId CdeTableGetLobFileId<dd::Table>(
    const dd::Table *tableDef);
template DSTORE::FileId CdeTableGetLobFileId<dd::Partition>(
    const dd::Partition *tableDef);

template <typename Table>
uint32_t CdeDdTableGetSePrivateDataRelblknum(const Table *table_def) {
  uint relblknum = 0;
  if (table_def->se_private_data().exists(object_relblknum)) {
    table_def->se_private_data().get(object_relblknum, &relblknum);
  }
  return static_cast<uint32_t>(relblknum);
}

template uint32_t CdeDdTableGetSePrivateDataRelblknum<dd::Table>(
    const dd::Table *table_def);
template uint32_t CdeDdTableGetSePrivateDataRelblknum<dd::Partition>(
    const dd::Partition *table_def);

template <typename Table>
DSTORE::BlockNumber CdeTableGetLobBlockNumber(const Table *tableDef) {
  DSTORE::BlockNumber blockNumber = DSTORE::DSTORE_INVALID_BLOCK_NUMBER;
  if (tableDef->se_private_data().exists(object_rellobblknum)) {
    tableDef->se_private_data().get(object_rellobblknum, &blockNumber);
  }
  return blockNumber;
}

template DSTORE::BlockNumber CdeTableGetLobBlockNumber<dd::Table>(
    const dd::Table *tableDef);
template DSTORE::BlockNumber CdeTableGetLobBlockNumber<dd::Partition>(
    const dd::Partition *tableDef);

template <typename Table>
uint32_t CdeDdTableGetTableRelOid(const Table *table_def) {
  uint64_t tableId = static_cast<uint64_t>(table_def->se_private_id());
  return static_cast<uint32_t>(tableId & 0xFFFFFFFFULL);
}

template uint32_t CdeDdTableGetTableRelOid<dd::Table>(
    const dd::Table *table_def);
template uint32_t CdeDdTableGetTableRelOid<dd::Partition>(
    const dd::Partition *table_def);

template <typename Table>
uint64_t CdeDdTableGetTableId(const Table *table_def) {
  return static_cast<uint64_t>(table_def->se_private_id());
}

template uint64_t CdeDdTableGetTableId<dd::Table>(const dd::Table *table_def);
template uint64_t CdeDdTableGetTableId<dd::Partition>(
    const dd::Partition *table_def);

size_t CdeDdTableGetColumnNums(dd::Table *table_def) {
  return table_def->columns()->size();
}

const char *CdeDdTableGetName(const dd::Table *table_def) {
  return table_def->name().c_str();
}

const char *CdeDdIndexGetName(const dd::Index *index_def) {
  return index_def->name().c_str();
}

template <typename Table>
DSTORE::CommitSeqNo CdeDdTableGetTableCsn(const Table *table_def) {
  DSTORE::CommitSeqNo csn = 0;
  if (table_def->se_private_data().exists(object_tablecreatecsn)) {
    table_def->se_private_data().get(object_tablecreatecsn, &csn);
  }
  return csn;
}

template DSTORE::CommitSeqNo CdeDdTableGetTableCsn<dd::Table>(
    const dd::Table *table_def);
template DSTORE::CommitSeqNo CdeDdTableGetTableCsn<dd::Partition>(
    const dd::Partition *table_def);

template <typename Table>
uint16_t CdeDdIndexGetSePrivateDataRelfileid(const Table *table_def,
                                             size_t key_no) {
  uint relfileid = 0;
  using Index = std::conditional_t<std::is_same_v<Table, dd::Table>, dd::Index,
                                   dd::Partition_index>;
  const Index *index_def = table_def->indexes().at(key_no);
  if (index_def->se_private_data().exists(object_relfileid)) {
    index_def->se_private_data().get(object_relfileid, &relfileid);
  }
  return static_cast<uint16_t>(relfileid);
}

template uint16_t CdeDdIndexGetSePrivateDataRelfileid<dd::Table>(
    const dd::Table *table_def, size_t key_no);
template uint16_t CdeDdIndexGetSePrivateDataRelfileid<dd::Partition>(
    const dd::Partition *table_def, size_t key_no);

template <typename Table>
uint32_t CdeDdIndexGetSePrivateDataRelblknum(const Table *table_def,
                                             size_t key_no) {
  uint relblknum = 0;
  using Index = std::conditional_t<std::is_same_v<Table, dd::Table>, dd::Index,
                                   dd::Partition_index>;
  const Index *index_def = table_def->indexes().at(key_no);
  if (index_def->se_private_data().exists(object_relblknum)) {
    index_def->se_private_data().get(object_relblknum, &relblknum);
  }
  return static_cast<uint32_t>(relblknum);
}

template uint32_t CdeDdIndexGetSePrivateDataRelblknum<dd::Table>(
    const dd::Table *table_def, size_t key_no);

template uint32_t CdeDdIndexGetSePrivateDataRelblknum<dd::Partition>(
    const dd::Partition *table_def, size_t key_no);

template <typename Table>
uint32_t CdeDdIndexGetTableRelOid(const Table *table_def, size_t key_no) {
  uint index_relid = 0;
  using Index = std::conditional_t<std::is_same_v<Table, dd::Table>, dd::Index,
                                   dd::Partition_index>;
  const Index *index_def = table_def->indexes().at(key_no);
  if (index_def->se_private_data().exists(object_indexrelid)) {
    index_def->se_private_data().get(object_indexrelid, &index_relid);
  }
  return static_cast<uint32_t>(index_relid);
}

template uint32_t CdeDdIndexGetTableRelOid<dd::Table>(
    const dd::Table *table_def, size_t key_no);
template uint32_t CdeDdIndexGetTableRelOid<dd::Partition>(
    const dd::Partition *table_def, size_t key_no);

uint8_t CdeDdlGetForeignKeyRule(dd::Foreign_key::enum_rule update_rule,
                                dd::Foreign_key::enum_rule delete_rule) {
  uint8_t fk_rule = CDE_DDL_FK_RULE_DEFAULT;
  switch (update_rule) {
    case dd::Foreign_key::enum_rule::RULE_NO_ACTION:
    case dd::Foreign_key::enum_rule::RULE_SET_DEFAULT:
      fk_rule |= CDE_DDL_FK_RULE_ON_UPDATE_NO_ACTION;
      break;
    case dd::Foreign_key::enum_rule::RULE_CASCADE:
      fk_rule |= CDE_DDL_FK_RULE_ON_UPDATE_CASCADE;
      break;
    case dd::Foreign_key::enum_rule::RULE_SET_NULL:
      fk_rule |= CDE_DDL_FK_RULE_ON_UPDATE_SET_NULL;
      break;
    case dd::Foreign_key::enum_rule::RULE_RESTRICT:
    default:
      break;
  }
  switch (delete_rule) {
    case dd::Foreign_key::enum_rule::RULE_NO_ACTION:
    case dd::Foreign_key::enum_rule::RULE_SET_DEFAULT:
      fk_rule |= CDE_DDL_FK_RULE_ON_DELETE_NO_ACTION;
      break;
    case dd::Foreign_key::enum_rule::RULE_CASCADE:
      fk_rule |= CDE_DDL_FK_RULE_ON_DELETE_CASCADE;
      break;
    case dd::Foreign_key::enum_rule::RULE_SET_NULL:
      fk_rule |= CDE_DDL_FK_RULE_ON_DELETE_SET_NULL;
      break;
    case dd::Foreign_key::enum_rule::RULE_RESTRICT:
    default:
      break;
  }
  return fk_rule;
}

template <typename Table>
DSTORE::CommitSeqNo CdeDdIndexGetIndexCreateCsn(const Table *table_def,
                                                size_t key_no) {
  DSTORE::CommitSeqNo index_csn;
  using Index = std::conditional_t<std::is_same_v<Table, dd::Table>, dd::Index,
                                   dd::Partition_index>;
  const Index *index_def = table_def->indexes().at(key_no);
  if (index_def->se_private_data().exists(object_indexcreatecsn)) {
    index_def->se_private_data().get(object_indexcreatecsn, &index_csn);
  }
  return static_cast<DSTORE::CommitSeqNo>(index_csn);
}

template DSTORE::CommitSeqNo CdeDdIndexGetIndexCreateCsn<dd::Table>(
    const dd::Table *table_def, size_t key_no);
template DSTORE::CommitSeqNo CdeDdIndexGetIndexCreateCsn<dd::Partition>(
    const dd::Partition *table_def, size_t key_no);

int CdeDictForeignAddToCache(cde_dict_foreign_t *foreign, bool can_free_fk,
                             bool ignore_fk_nokey) {
  // set foreign_table & foreign_index, referenced_table & referenced_index
  int ret = 0;
  if (foreign == nullptr) {
    CDE_LOG_ERROR("foreign is nullptr!");
    return HA_ERR_GENERIC;
  }
  cde_dict_foreign_t *foreign_in_cache = nullptr;
  cde_dict_t *referenced_table =
      DictSysGetTable(foreign->referenced_table_name);
  cde_dict_t *foreign_table = DictSysGetTable(foreign->foreign_table_name);
  DictTableRefGuard dictRefGuardReferenced(referenced_table);
  DictTableRefGuard dictRefGuardForeign(foreign_table);

  if (foreign_table != nullptr) {
    foreign_in_cache = foreign_table->get_foreign(foreign);
  }
  if (foreign_in_cache == nullptr && referenced_table != nullptr) {
    foreign_in_cache = referenced_table->get_foreign(foreign);
  }
  if (foreign_in_cache != nullptr && foreign_in_cache != foreign) {
    // free the foreign object
    delete foreign;
  } else {
    foreign_in_cache = foreign;
  }
  bool addedToReferenceSet = false;
  // todo: Currently, all table in cache, so: foreign_table != nullptr &&
  // referenced_table != nullptr
  if (referenced_table != nullptr &&
      foreign_in_cache->referenced_table == nullptr) {
    cde_dict_index_t *referenced_index = referenced_table->get_index_on_column(
        foreign_in_cache->referenced_col_names, foreign_in_cache->col_nums);
    if (referenced_index == nullptr && false == ignore_fk_nokey) {
      CDE_LOG_ERROR(
          "Not Found Referenced Index! foreigin table = %s, ref table = %s, "
          "col_nums = %d",
          foreign_in_cache->foreign_table_name,
          foreign_in_cache->referenced_table_name, foreign_in_cache->col_nums);
      if (foreign_in_cache == foreign && can_free_fk) delete foreign;
      return HA_ERR_CANNOT_ADD_FOREIGN;
    }
    foreign_in_cache->referenced_table = referenced_table;
    foreign_in_cache->referenced_index = referenced_index;
    ret |= referenced_table->add_referenced_dict(foreign_in_cache);
    addedToReferenceSet = true;
  }

  if (foreign_table != nullptr && foreign_in_cache->foreign_table == nullptr) {
    cde_dict_index_t *foreign_index = foreign_table->get_index_on_column(
        foreign_in_cache->foreign_col_names, foreign_in_cache->col_nums);
    if (foreign_index == nullptr && false == ignore_fk_nokey) {
      CDE_LOG_ERROR(
          "Not Found Foreign Index! foreigin table = %s, ref table = %s, "
          "col_nums = %d",
          foreign_in_cache->foreign_table_name,
          foreign_in_cache->referenced_table_name, foreign_in_cache->col_nums);
      foreign_in_cache->referenced_table = nullptr;
      foreign_in_cache->referenced_index = nullptr;
      if (foreign_in_cache == foreign) {
        if (addedToReferenceSet)
          referenced_table->referenced_dict_set.erase(foreign_in_cache);
        delete foreign_in_cache;
      }
      return HA_ERR_CANNOT_ADD_FOREIGN;
    }
    foreign_in_cache->foreign_table = foreign_table;
    foreign_in_cache->foreign_index = foreign_index;
    ret |= foreign_table->add_foreign_dict(foreign_in_cache);
  }

  if (referenced_table) {
    CDE_ASSERT_DEBUG(referenced_table->getRefCnt() > 0);
    DictRemoveFromEvictLRU(referenced_table);
  }

  if (foreign_table) {
    CDE_ASSERT_DEBUG(foreign_table->getRefCnt() > 0);
    DictRemoveFromEvictLRU(foreign_table);
  }
  return ret;
}

/** Load foreign key constraint for the table. Note, it could also open the
foreign table, if this table is referenced by the foreign table
@param[in,out]	client		data dictionary client
@param[out]	m_table		CDE table handle
@return 0	if successfully load FK constraint */
int CdeDdTableCheckForChildFk(dd::cache::Dictionary_client *client,
                              cde_dict_t *dict_table,
                              std::deque<const char *> &fk_names,
                              bool ignore_fk_nokey) {
  std::vector<dd::String_type> child_schema;
  std::vector<dd::String_type> child_name;
  char table_name[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char db_name[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  int ret = 0;
  CdeCreateTableInfo::SplitNormalizedName(dict_table->name.c_str(), table_name,
                                          db_name);

  if (client->fetch_fk_children_uncached(db_name, table_name,
                                         DSTORE_ENGINE_NAME, false,
                                         &child_schema, &child_name)) {
    CDE_LOG_ERROR(
        "fetch_fk_children_uncached fail! db_name = %s, table_name = %s",
        db_name, table_name);
    return HA_ERR_GENERIC;
  }
  std::vector<dd::String_type>::iterator it = child_name.begin();
  for (auto &child_db_name : child_schema) {
    // DictSysGetTable();
    dd::String_type child_tb_name = *it;
    char buf[2 * NAME_CHAR_LEN * 5 + 2 + 1] = {0};
    bool truncated;
    build_table_filename(buf, sizeof(buf), child_db_name.c_str(),
                         child_tb_name.c_str(), nullptr, 0, &truncated);

    if (lower_case_table_names == 2) {
      my_casedn_str(system_charset_info, buf);
    } else {
#ifndef _WIN32
      if (lower_case_table_names == 1) {
        my_casedn_str(system_charset_info, buf);
      }
#endif /* !_WIN32 */
    }

    cde_dict_t *foreign_table = DictSysGetTable(buf);
    DictTableRefGuard dictRefGuard(foreign_table);
    if (foreign_table) {
      for (cde_dict_foreign_t *fk : foreign_table->foreign_dict_set) {
        if (strcmp(fk->referenced_table_name, dict_table->name.c_str())) {
          if (dict_table->refresh_fk && dict_table->oldName != nullptr &&
              strcmp(fk->referenced_table_name, dict_table->oldName.get()) ==
                  0) {
            /** When we are renaming table with ALTER TABLE RENAME COPY,
             * Note that server does it by sending those commands to SE:
             * 1 rename old table to backup name
             * 2 create a temp table
             * 3 call set_old_name on a temp table to make it aware of its
             *   old table name.
             * 4 copy old table table to temp table
             * 5 rename temp table to new name
             * 6 open new table
             * 7 remove old table
             * Let's say we have two tables t0 and t1. t1 references table t0.
             * t0 name is changed to t2. During rename in step 1 in DStore (and
             * INNODB) we drop foreign keys that belong to t0 and we reset all
             * the references to our table from each referenced table as those
             * need to be refreshed on table open. I.e. we reset in t1 (set
             * fk->refrenced_table to null in t1's referenced_dict_set) link to
             * t0, as new table for t0 will be created in steps 2 -> 6. However,
             * we leave the referenced_table_name as t0 - as in step 1 storage
             * engine doesn't know the new name of the table t2, only a temp
             * backup name. Here, in this function, we are already in step 5. We
             * open new table and refresh foreign keys. We correct
             * referenced_table_name from the oldName that was provided by the
             * server to us via set_old_name interface function - to the new
             * name (step 3). This way we change the t1's foreign key from t0 to
             * t2. During ATLER TABLE RENAME COPY server holds exclusive lock on
             * name t0. So we know that we can safely match against this name
             * here. Server also holds a lock on t1, but only a shared lock.
             * Thus, in case we encounter removed link (fk->referenced_table ==
             * nullptr) we cannot just set here referenced_table_name from t0 to
             * t2 as some other thread could have unlink this foreign key. We
             * can only correct referenced_table_name when it is set to t0 as
             * this name is protected by the server.
             */
            CDE_ASSERT_DEBUG(fk->referenced_table == nullptr);
            CDE_ASSERT_DEBUG(fk->referenced_index == nullptr);

            if (strlen(fk->referenced_table_name) < dict_table->name.length())
              fk->referenced_table_name =
                  safe_strdup_root(&fk->m_mem_root, dict_table->name.c_str());
            else {
              int strCpyRet [[maybe_unused]] =
                  strcpy_s(fk->referenced_table_name,
                           strlen(fk->referenced_table_name) + 1,
                           dict_table->name.c_str());
              DBUG_EXECUTE_IF("dd_table_fk_load_fail_on_strcpy",
                              { strCpyRet = CDE_ERROR; });
              if (0 != strCpyRet) {
                CDE_LOG_ERROR(
                    "strcpy_s failed on foreign's keys refereced_table_name!");
                return HA_ERR_INTERNAL_ERROR;
              }
            }
          } else {
            continue;
          }
        }

        if (fk->referenced_table) {
          CDE_ASSERT(fk->referenced_table == dict_table);
          continue;
        }
        int ret = CdeDictForeignAddToCache(fk, false, ignore_fk_nokey);
        if (0 != ret) {
          for (auto *fk_name : fk_names) CdeFree(const_cast<char *>(fk_name));
          return ret;
        }
      }
    } else {
      int fk_name_len = strlen(buf);
      char *fk_name = (char *)CdeZalloc(fk_name_len + 1);
      if (fk_name == nullptr) {
        CDE_LOG_ERROR("CdeZalloc fail! len = %d", fk_name_len);
        CDE_ASSERT(0);
      }
      ret = memcpy_s(fk_name, fk_name_len + 1, buf, fk_name_len);
      if (ret != 0) {
        CDE_LOG_ERROR("memcpy_s fail!");
      }
      fk_names.push_back(fk_name);
    }
    ++it;
  }
  return 0;
}

/** Load foreign key constraint for the table. Note, it could also open the
foreign table, if this table is referenced by the foreign table
@param[in]     tbl_name  Table Name
@param[in,out] m_table   CDE table handle
@param[in]     dd_table  Global DD table
@return 0	if successfully load FK constraint */
int CdeDdTableLoadForeignKeyFromDd(const char *tbl_name, cde_dict_t *m_table,
                                   const dd::Table *dd_table,
                                   bool ignore_fk_nokey) {
  int ret = 0;
  if (tbl_name == nullptr || m_table == nullptr || dd_table == nullptr) {
    CDE_LOG_ERROR("Error Input!");
    return HA_ERR_GENERIC;
  }

  CDE_ASSERT_DEBUG(m_table->getRefCnt() > 0);

  for (const dd::Foreign_key *key : dd_table->foreign_keys()) {
    dd::String_type db_name = key->referenced_table_schema_name();
    dd::String_type tb_name = key->referenced_table_name();
    char buf[FN_REFLEN + 1];
    bool truncated;
    build_table_filename(buf, sizeof(buf), db_name.c_str(), tb_name.c_str(),
                         nullptr, 0, &truncated);
    char norm_name[FN_REFLEN * 2];
    if (truncated ||
        CdeCreateTableInfo::NormalizeTableName(norm_name, buf) != 0) {
      CDE_LOG_ERROR("normalize_table_name fail! norm_name = %s, buf = %s",
                    norm_name, buf);
      return HA_ERR_GENERIC;  // DB_TOO_LONG_PATH
    }

    std::unique_ptr<cde_dict_foreign_t> foreign(new (std::nothrow)
                                                    cde_dict_foreign_t);
    if (foreign == nullptr) {
      CDE_LOG_ERROR("CdeZalloc fail!");
      return HA_ERR_INTERNAL_ERROR;
    }

    foreign->fk_rule =
        CdeDdlGetForeignKeyRule(key->update_rule(), key->delete_rule());

    foreign->foreign_table_name =
        safe_strdup_root(&foreign->m_mem_root, tbl_name);
    if (foreign->foreign_table_name == nullptr) {
      CDE_LOG_ERROR("CdeZalloc fail!");
      return HA_ERR_INTERNAL_ERROR;
    }

    if (lower_case_table_names == 2) {
      my_casedn_str(system_charset_info, foreign->foreign_table_name);
    } else {
#ifndef _WIN32
      if (lower_case_table_names == 1) {
        my_casedn_str(system_charset_info, foreign->foreign_table_name);
      }
#endif /* !_WIN32 */
    }
    foreign->referenced_table_name =
        safe_strdup_root(&foreign->m_mem_root, buf);
    if (foreign->referenced_table_name == nullptr) {
      CDE_LOG_ERROR("CdeZalloc fail!");
      return HA_ERR_INTERNAL_ERROR;
    }

    uint32_t dbNameSize = 0;
    const char *dbNamePtr = GetDBName(tbl_name, dbNameSize);
    if (nullptr == dbNamePtr || 0 == dbNameSize) {
      CDE_LOG_ERROR("GetDBName failed when creating unique_name!");
      return HA_ERR_INTERNAL_ERROR;
    }

    if (0 != strncpy_s(buf, sizeof(buf), dbNamePtr, dbNameSize)) {
      CDE_LOG_ERROR("strncpy_s failed during copying db name.");
      return HA_ERR_INTERNAL_ERROR;
    }

    snprintf(norm_name, sizeof(norm_name), "%s/%s", buf, key->name().c_str());
    foreign->unique_name = safe_strdup_root(&foreign->m_mem_root, norm_name);

    if (foreign->unique_name == nullptr) {
      CDE_LOG_ERROR("CdeZalloc fail!");
      return HA_ERR_INTERNAL_ERROR;
    }
    foreign->col_nums = key->elements().size();
    foreign->foreign_col_names =
        new (&foreign->m_mem_root) const char *[foreign->col_nums];
    foreign->referenced_col_names =
        new (&foreign->m_mem_root) const char *[foreign->col_nums];
    if (foreign->foreign_col_names == nullptr ||
        foreign->referenced_col_names == nullptr) {
      CDE_LOG_ERROR("CdeZalloc fail!");
      return HA_ERR_INTERNAL_ERROR;
    }
    int i = 0;
    for (const dd::Foreign_key_element *key_e : key->elements()) {
      dd::String_type ref_col_name = key_e->referenced_column_name();
      foreign->referenced_col_names[i] =
          safe_strdup_root(&foreign->m_mem_root, ref_col_name.c_str());

      if (nullptr == foreign->referenced_col_names[i]) {
        CDE_LOG_ERROR("CdeZalloc fail!");
        return HA_ERR_INTERNAL_ERROR;
      }
      const dd::Column *f_col = &key_e->column();
      foreign->foreign_col_names[i] =
          safe_strdup_root(&foreign->m_mem_root, f_col->name().c_str());
      if (foreign->foreign_col_names[i] == nullptr) {
        CDE_LOG_ERROR("CdeZalloc fail!");
        return HA_ERR_INTERNAL_ERROR;
      }
      i++;
    }
    ret |= CdeDictForeignAddToCache(foreign.release(), true, ignore_fk_nokey);
  }
  return ret;
}

int CdeDdTableLoadForeignKey(THD *thd, const char *tbl_name,
                             cde_dict_t *dict_table, const dd::Table *dd_table,
                             std::deque<const char *> &fk_names,
                             bool lockOperationalLock) {
  OperationalLockGuard op_guard(lockOperationalLock);

  int ret = 0;
  bool ignore_fk_nokey =
      (thd != nullptr) ? thd_test_options(thd, OPTION_NO_FOREIGN_KEY_CHECKS)
                       : true;

  ret |= CdeDdTableLoadForeignKeyFromDd(tbl_name, dict_table, dd_table,
                                        ignore_fk_nokey);

  if (thd != nullptr) {
    dd::cache::Dictionary_client *client = dd::get_dd_client(thd);
    dd::cache::Dictionary_client::Auto_releaser releaser(client);
    ret |= CdeDdTableCheckForChildFk(client, dict_table, fk_names,
                                     ignore_fk_nokey);
  }
  return ret;
}

/**
Fill cde dict index struct from data dictionary.

@param[in]       dd_table   dd::Table describing the table.
@param[in]       form_table Table structure.
@param[in,out]   dict_table CDE dict table struct.

@return CDE_OK if execute succeed, else return CDE_ERROR.
*/
template <typename Table>
int CdeDdFillDictIndex(const Table &dd_table, const TABLE *form_table,
                       cde_dict_t *dict_table) {
  for (uint key_no = 0; key_no < form_table->s->keys; key_no++) {
    const KEY *key = form_table->key_info + key_no;
    uint32_t index_attr_num = (uint32_t)key->user_defined_key_parts;
    Oid index_oid = CdeDdIndexGetTableRelOid(&dd_table, key_no);
    PageId segment_id;
    segment_id.m_fileId =
        CdeDdIndexGetSePrivateDataRelfileid(&dd_table, key_no);
    segment_id.m_blockId =
        CdeDdIndexGetSePrivateDataRelblknum(&dd_table, key_no);
    // get index column vector, cde_dict_index_t->index_cols need, from 0
    uint32_t *index_cols = static_cast<uint32_t *>(
        CdeZalloc(sizeof(uint32_t) * key->user_defined_key_parts));
    if (index_cols == nullptr) {
      CDE_LOG_ERROR("CdeZalloc fail! alloc nums = %ld",
                    sizeof(uint32_t) * key->user_defined_key_parts);
      return CDE_ERROR;
    }

    uint32_t *attr_cols = static_cast<uint32_t *>(
        CdeZalloc(sizeof(uint32_t) * key->user_defined_key_parts));
    DBUG_EXECUTE_IF("cde_dd_fill_index_attr_cols_alloc_fail", {
      CdeFree(attr_cols);
      attr_cols = nullptr;
    });
    if (attr_cols == nullptr) {
      CdeFree(index_cols);
      CDE_LOG_ERROR("CdeZalloc attr_cols fail! alloc nums = %ld",
                    sizeof(uint32_t) * key->user_defined_key_parts);
      return CDE_ERROR;
    }

    uint64_t *stat_n_diff_key_vals = static_cast<uint64_t *>(
        CdeZalloc(sizeof(uint64_t) * key->user_defined_key_parts));
    if (stat_n_diff_key_vals == nullptr) {
      CDE_LOG_ERROR("CdeZalloc fail! alloc nums = %ld",
                    sizeof(uint64_t) * key->user_defined_key_parts);
      dict_table->destroy_all_index_cache();
      CdeFree(index_cols);
      CdeFree(attr_cols);
      return CDE_ERROR;
    }

    uint64_t *stat_n_sample_size = static_cast<uint64_t *>(
        CdeZalloc(sizeof(uint64_t) * key->user_defined_key_parts));
    DBUG_EXECUTE_IF("stat_n_sample_size_alloc_fail", {
      CdeFree(stat_n_sample_size);
      stat_n_sample_size = nullptr;
    });
    if (stat_n_sample_size == nullptr) {
      CDE_LOG_ERROR("CdeZalloc fail! alloc nums = %ld",
                    sizeof(uint64_t) * key->user_defined_key_parts);
      dict_table->destroy_all_index_cache();
      CdeFree(index_cols);
      CdeFree(attr_cols);
      CdeFree(stat_n_diff_key_vals);
      return CDE_ERROR;
    }

    cde_dict_index_t *dict_index = new (std::nothrow) cde_dict_index_t();
    if (dict_index == nullptr) {
      CdeFree(index_cols);
      CdeFree(attr_cols);
      CdeFree(stat_n_diff_key_vals);
      CdeFree(stat_n_sample_size);
      return CDE_ERROR;
    }

    CdeCreateTableInfo::CreateFieldsForIndex(dict_index, key, form_table);
    ScanKey key_infos;
    dict_index->fillfactor = DstoreParseFillfactor(key->comment.str, nullptr);
    auto ff = dict_index->fillfactor ? dict_index->fillfactor
                                     : g_defaultFillfactorForIndex;
    CdeCreateTableInfo tableInfo(dict_table->m_spaceId);
    StorageRelation new_index_rel = tableInfo.CreateIndexStorRel(
        dict_table->dstore_relation, dict_table->attrFull,
        dict_table->m_dictCols, index_oid, segment_id, key, index_cols,
        attr_cols, &key_infos, dict_index->fields, false, ff);
    if (new_index_rel == nullptr) {
      CdeFree(index_cols);
      CdeFree(attr_cols);
      CdeFree(stat_n_diff_key_vals);
      CdeFree(stat_n_sample_size);
      CdeFree(dict_index->fields);
      dict_index->fields = nullptr;
      delete dict_index;
      CDE_LOG_ERROR("CdeDdFillDictIndex fail! key nums = %d, key name = %s",
                    form_table->s->keys, key->name);
      return CDE_ERROR;
    }

    dict_index->rel = new_index_rel;
    dict_index->scan_key = key_infos;
    dict_index->index_cols = index_cols;
    dict_index->key_length = key->key_length;
    dict_index->attr_cols = attr_cols;
    dict_index->stat_n_diff_key_vals = stat_n_diff_key_vals;
    dict_index->stat_n_sample_size = stat_n_sample_size;
    dict_index->index_col_num = index_attr_num;
    dict_index->name.assign(key->name);
    dict_index->oid = new_index_rel->relOid;
    dict_index->table = dict_table;
    dict_index->index_csn = CdeDdIndexGetIndexCreateCsn(&dd_table, key_no);
    dict_table->add_index_dict(dict_index);
  }
  return CDE_OK;
}

template int CdeDdFillDictIndex<dd::Table>(const dd::Table &dd_table,
                                           const TABLE *form_table,
                                           cde_dict_t *dict_table);
template int CdeDdFillDictIndex<dd::Partition>(const dd::Partition &dd_table,
                                               const TABLE *form_table,
                                               cde_dict_t *dict_table);

template <typename Table>
cde_dict_t *CdeDdFillDictTable(const Table *dd_table, const TABLE *form_table,
                               const char *name, bool forReplay) {
  const dd::Table &ddTable = dd_table->table();
  const dd::Table *ddTablePtr = &ddTable;
  uint32_t relOid = CdeDdTableGetTableRelOid(dd_table);
  PageId segmentId;
  segmentId.m_fileId = CdeDdTableGetSePrivateDataRelfileid(dd_table);
  segmentId.m_blockId = CdeDdTableGetSePrivateDataRelblknum(dd_table);

  if (segmentId.IsInvalid()) {
    CDE_LOG_ERROR(
        "segment_id is invalid! fileId = %u, blockId = %u, rel_oid = %u",
        segmentId.m_fileId, segmentId.m_blockId, relOid);
    return nullptr;
  }
  DSTORE::PageId lobSegmentId;
  lobSegmentId.m_fileId = CdeTableGetLobFileId(dd_table);
  lobSegmentId.m_blockId = CdeTableGetLobBlockNumber(dd_table);
  if (IsContainLobCol(form_table) && lobSegmentId.IsInvalid()) {
    CDE_LOG_ERROR(
        "lobSegmentId is invalid! fileId = %u, blockId = %u, rel_oid = "
        "%u",
        lobSegmentId.m_fileId, lobSegmentId.m_blockId, relOid);
    return nullptr;
  }

  cde_dict_t *dict_table = new (std::nothrow) cde_dict_t();
  if (dict_table == nullptr) {
    CDE_LOG_ERROR("new cde_dict_t fail!");
    return nullptr;
  }

  dict_table->m_spaceId = CdeGetSpaceIdByDD(dd_table);
  dict_table->fillfactor =
      DstoreParseFillfactor(form_table->s->comment.str, nullptr);
  auto ff =
      dict_table->fillfactor ? dict_table->fillfactor : g_defaultFillfactor;

  NumberOfFields numberOfFields = FillFieldLenFromDD(
      &(dd_table->table()), form_table, dict_table->m_fieldLenInfos);
  dict_table->m_totalColCount =
      numberOfFields.nonVirtualFieldsNum + numberOfFields.virtualFieldsNum;
  dict_table->m_vColCount = numberOfFields.virtualFieldsNum;

  for (uint i = 0; i < form_table->s->fields; i++) {
    Field *mysql_field = form_table->field[i];
    std::string col_name(mysql_field->field_name);
    dict_table->col_names.push_back(col_name);
  }

  /* Build DictCol must place before build table rel, because tuple init def
  values in tupledesc point to dict col's content for columns added/dropped
  instantly. */
  uint32_t version = 0;
  DictCol *dictCols =
      BuildDictColsFromDD(ddTablePtr, version, dict_table->m_maxPos);

  if (dict_table->m_vColCount > 0) {
    dict_table->m_vCols = BuildVCols(dict_table, form_table, ddTablePtr);
    CDE_ASSERT_DEBUG(dict_table->m_vCols.size() > 0);
  }

  dict_table->m_dictCols = dictCols;
  dict_table->m_currentRowVersion = version;

  DSTORE::TupleDesc attrFull = nullptr;

  CdeCreateTableInfo tableInfo(dict_table->m_spaceId);
  StorageRelation storRel = tableInfo.CreateHeapStorRel(
      relOid, segmentId, lobSegmentId, form_table, ff, ddTablePtr, dictCols,
      forReplay, (dict_table->m_vColCount > 0) ? &attrFull : nullptr);

  if (storRel == nullptr) {
    CDE_LOG_ERROR(
        "build_table_rel fail! fileId = %u, blockId = %u, rel_oid = %u",
        segmentId.m_fileId, segmentId.m_blockId, relOid);
    delete dict_table;
    return nullptr;
  }

  CDE_ASSERT_DEBUG(0 == dict_table->m_vColCount || nullptr != attrFull);

  /* In case there are no virtual columns attrFull is just storRel->attr*/
  dict_table->attrFull =
      (dict_table->m_vColCount > 0) ? attrFull : storRel->attr;
  CDE_ASSERT_DEBUG(nullptr != dict_table->attrFull);

  dict_table->dstore_relation = storRel;
  dict_table->name.assign(name);
  dict_table->m_id = CdeDdTableGetTableId(dd_table);
  dict_table->m_rebuild_csn = CdeDdTableGetTableCsn(dd_table);
  return dict_table;
}

template cde_dict_t *CdeDdFillDictTable<dd::Table>(const dd::Table *dd_table,
                                                   const TABLE *form_table,
                                                   const char *name,
                                                   bool forReplay);

template cde_dict_t *CdeDdFillDictTable<dd::Partition>(
    const dd::Partition *dd_table, const TABLE *form_table, const char *name,
    bool forReplay);

template <typename Table>
cde_dict_t *CdeDdOpenTableOne(THD *thd, const char *name, const Table *dd_table,
                              const TABLE *form_table,
                              std::deque<const char *> &fk_names,
                              bool allowEviction = true) {
  if (thd == nullptr || name == nullptr || dd_table == nullptr ||
      form_table == nullptr) {
    return nullptr;
  }

  int ret = 0;
  auto deleter = [](cde_dict_t *dict_table) {
    dict_table->destroy_from_cache(false);
    delete dict_table;
  };
  std::unique_ptr<cde_dict_t, decltype(deleter)> dict_table{
      CdeDdFillDictTable(dd_table, form_table, name, thd->for_ddl_info_replay),
      deleter};
  if (dict_table == nullptr) {
    return nullptr;
  }
  ret = CdeDdFillDictIndex(*dd_table, form_table, dict_table.get());
  if (ret != 0) return nullptr;

  cde_dict_t *existingTable = nullptr;
  OperationalLockGuard opWGuard;
  DictSysAddTable(name, dict_table.get(), &existingTable, allowEviction);
  if (existingTable != nullptr) {
    /** Seems table has been added by another thread. */
    return existingTable;
  }

  // dict_table has been added into cache, been owned by cache. So don't use
  // unique_ptr later.
  auto dict_table_ptr = dict_table.release();

  DictTableRefGuard dictRefGuard(dict_table_ptr);

  const dd::Table &ddTable = dd_table->table();
  const dd::Table *ddTablePtr = &ddTable;
  ret = CdeDdTableLoadForeignKey(thd, name, dict_table_ptr, ddTablePtr,
                                 fk_names, false);

  DBUG_EXECUTE_IF("dd_table_load_foreign_key_failed", { ret = CDE_ERROR; });
  DEBUG_SYNC_C("wait_after_load_foreign_key");
  if (ret == 0) {
    CDE_LOG_SYSTEM("open table name = %s", name);
  } else {
    dictRefGuard.Release();
    DictSysRemoveTableNoOpLock(name);
    CDE_LOG_ERROR("open table name = %s, ret = %d", name, ret);
    return nullptr;
  }
  opWGuard.Clear();
  /* Now the duty of guard belongs to caller. So reset guard here. */
  dictRefGuard.Reset(nullptr);
  return dict_table_ptr;
}

template <typename Table>
cde_dict_t *CdeDdOpenTableOnDdObj(THD *thd, const char *name,
                                  const Table *ddTable) {
  TABLE_SHARE ts;
  TABLE td;
  char tableName[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char dbName[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  CdeCreateTableInfo::SplitNormalizedName(name, tableName, dbName);
  if (CdeAcquireUncacheTable(thd, &(ddTable->table()), dbName, &ts, &td) ==
      CDE_FAIL) {
    CDE_LOG_ERROR("Get table[%s] definition based on DD object fail!", name);
    return nullptr;
  }
  cde_dict_t *dictTable = CdeDdOpenTable(thd, name, ddTable, &td);
  CdeReleaseUncachedTable(&ts, &td);
  return dictTable;
}

template cde_dict_t *CdeDdOpenTableOnDdObj<dd::Table>(THD *thd,
                                                      const char *name,
                                                      const dd::Table *ddTable);
template cde_dict_t *CdeDdOpenTableOnDdObj<dd::Partition>(
    THD *thd, const char *name, const dd::Partition *ddPart);

bool CdeAcquireUncacheTable(THD *thd, const dd::Table *ddTable,
                            const char *dbName, TABLE_SHARE *ts, TABLE *td) {
  init_tmp_table_share(thd, ts, dbName, strlen(dbName), ddTable->name().c_str(),
                       "" /* file name */, nullptr);

  if (open_table_def_suppress_invalid_meta_data(thd, ts, *ddTable) != 0) {
    CDE_LOG_ERROR("open table[%s,%s] definition fail at %s.", dbName,
                  ddTable->name().c_str(), __func__);
    return CDE_FAIL;
  }

  if (open_table_from_share(thd, ts, ddTable->name().c_str(), 0,
                            SKIP_NEW_HANDLER, 0, td, false, ddTable) != 0) {
    free_table_share(ts);
    CDE_LOG_ERROR("open table[%s,%s] from share fail at %s.", dbName,
                  ddTable->name().c_str(), __func__);
    return CDE_FAIL;
  }
  return CDE_SUCC;
}

void CdeReleaseUncachedTable(TABLE_SHARE *ts, TABLE *td) {
  closefrm(td, false);
  free_table_share(ts);
}

static cde_dict_t *CdeDdOpenTableOneOnName(char *name,
                                           std::deque<const char *> &fk_names,
                                           THD *thd, MDL_ticket **mdl,
                                           bool allowEviction) {
  CDE_ASSERT_DEBUG(mdl != nullptr && *mdl == nullptr);

  cde_dict_t *dict_table = DictSysGetTable(name);
  if (dict_table != nullptr) {
    return dict_table;
  }
  DEBUG_SYNC_C("WaitLoadReferencedTable");

  const dd::Table *dd_table = nullptr;

  char file_table_name[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char file_db_name[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  CdeCreateTableInfo::SplitNormalizedName(name, file_table_name, file_db_name);

  char table_name[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char db_name[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  filename_to_tablename(file_db_name, db_name, sizeof(db_name));
  filename_to_tablename(file_table_name, table_name, sizeof(table_name));

  int table_name_len = strlen(table_name);
  int db_name_len = strlen(db_name);

  if (lower_case_table_names == 2) {
    my_casedn_str(system_charset_info, table_name);
    my_casedn_str(system_charset_info, db_name);
  } else {
#ifndef _WIN32
    if (lower_case_table_names == 1) {
      my_casedn_str(system_charset_info, table_name);
      my_casedn_str(system_charset_info, db_name);
    }
#endif /* !_WIN32 */
  }
  if (table_name_len == 0 || db_name_len == 0 ||
      dd::acquire_shared_table_mdl(thd, db_name, table_name, false, mdl)) {
    return nullptr;
  }

  dd::cache::Dictionary_client *client = dd::get_dd_client(thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(client);

  if (client->acquire(db_name, table_name, &dd_table) || dd_table == nullptr) {
    CDE_LOG_ERROR("Dictionary_client acquire failed for table [%s] !", name);
    return nullptr;
  }

  TABLE_SHARE ts;
  TABLE td;
  if (CdeAcquireUncacheTable(thd, dd_table, db_name, &ts, &td) == CDE_FAIL) {
    CDE_LOG_ERROR("Get table[%s] definition based on DD object fail.", name);
    return nullptr;
  }

  dict_table =
      CdeDdOpenTableOne(thd, name, dd_table, &td, fk_names, allowEviction);
  CdeReleaseUncachedTable(&ts, &td);
  return dict_table;
}

void CdeDdOpenForeignKeyTables(std::deque<const char *> &fk_names, THD *thd) {
  while (!fk_names.empty()) {
    char *name = const_cast<char *>(fk_names.front());

    if (lower_case_table_names == 2) {
      my_casedn_str(system_charset_info, name);
    } else {
#ifndef _WIN32
      if (lower_case_table_names == 1) {
        my_casedn_str(system_charset_info, name);
      }
#endif /* !_WIN32 */
    }
    MDL_ticket *mdl{nullptr};
    cde_dict_t *table =
        CdeDdOpenTableOneOnName(name, fk_names, thd, &mdl, false);
    if (table) table->release();
    if (mdl) CdeDdMdlRelease(thd, &mdl);
    CdeFree(name);
    fk_names.pop_front();
  }
}

/** Set up base columns for virtual column
@param[in]      table   Dstore table
@param[in]      field   MySQL field
@param[in,out]  v_col   virtual column to be set up
*/
void setupBaseColumns(cde_dict_t *table, const Field *field,
                      DictVirtualCol *vCol) {
  for (uint i = 0; i < field->table->s->fields; ++i) {
    const Field *base_field = field->table->field[i];

    if (!base_field->is_virtual_gcol() &&
        bitmap_is_set(&field->gcol_info->base_columns_map, i)) {
      uint32_t z;

      for (z = 0; z < table->m_totalColCount; z++) {
        const char *name = table->col_names[z].c_str();
        if (!CdeStrcasecmp(name, base_field->field_name)) {
          break;
        }
      }
      CDE_ASSERT_DEBUG(z != table->m_totalColCount);
      vCol->m_baseColumnInd.push_back(z);
    }
  }
}

std::vector<DictVirtualCol> BuildVCols(cde_dict_t *table,
                                       const TABLE *formTable,
                                       const dd::Table *ddTable) {
  uint32_t i = 0;
  std::vector<DictVirtualCol> vCols;
  for (const dd::Column *ddCol : ddTable->columns()) {
    if (ddCol->is_virtual()) {
      DictVirtualCol vCol;
      vCol.ind = i;
      setupBaseColumns(table, formTable->field[i], &vCol);
      vCols.push_back(vCol);
    }
    ++i;
  }
  return vCols;
}

DictCol *BuildDictColsFromInplaceCreateDD(const dd::Table *ddTable,
                                          uint32_t &maxPos) {
  size_t nCols = ddTable->columns().size();
  DictCol *cols = new (std::nothrow) DictCol[nCols];
  if (cols == nullptr) {
    CDE_LOG_ERROR("Alloc dictcols failed for table %s.",
                  ddTable->name().c_str());
    return nullptr;
  }
  uint32_t i = 0;
  uint16_t positionOfVirtualCol = 0;
  maxPos = 0;

  for (const dd::Column *ddCol : ddTable->columns()) {
    cols[i].m_isVisible = !ddCol->is_se_hidden();
    cols[i].m_isVirtual = ddCol->is_virtual();
    if (cols[i].m_isVirtual) cols[i].m_virtualPos = positionOfVirtualCol++;
    i++;
  }

  i = 0;
  uint32_t pos = ~0U;
  for (const dd::Column *ddCol : ddTable->columns()) {
    if (ddCol->is_virtual()) {
      ++i;
      continue;
    }

    ++pos;
    cols[i].m_phyPos = pos;
    ++i;
  }
  maxPos = pos;

  i = 0;
  for (const dd::Column *ddCol : ddTable->columns()) {
    if (ddCol->is_virtual()) {
      ++pos;
      cols[i].m_phyPos = pos;
      ++i;
      continue;
    }
    ++i;
  }

  return cols;
}

DictCol *BuildDictColsFromDD(const dd::Table *ddTable, uint32_t &maxVersion,
                             uint32_t &maxPos) {
  size_t nCols = ddTable->columns().size();
  DictCol *cols = new (std::nothrow) DictCol[nCols];
  if (cols == nullptr) {
    CDE_LOG_ERROR("Alloc dictcols failed for table %s.",
                  ddTable->name().c_str());
    return nullptr;
  }
  uint32_t i = 0;
  uint16_t positionOfVirtualCol = 0;
  maxPos = 0;

  for (const dd::Column *ddCol : ddTable->columns()) {
    cols[i].m_isVisible = !ddCol->is_se_hidden();
    cols[i].m_isVirtual = ddCol->is_virtual();
    if (cols[i].m_isVirtual) cols[i].m_virtualPos = positionOfVirtualCol++;
    /* Get add&drop version from private, both may exist at the same time */
    const dd::Properties &properties = ddCol->se_private_data();
    uint32_t value{0};
    bool ret = false;
    ret = GetDDPropVal<uint32_t>(
        properties, ddColumnKeyStrings[DD_INSTANT_PHYSICAL_POS], &value);
    if (ret) {
      cols[i].m_phyPos = value;
      maxPos = std::max(maxPos, value);
    }
    ret = GetDDPropVal<uint32_t>(
        properties, ddColumnKeyStrings[DD_INSTANT_VERSION_ADDED], &value);
    cols[i].m_versionAdded = ret ? value : 0;
    if (ret) {
      maxVersion = std::max(maxVersion, value);
    }

    ret = GetDDPropVal<uint32_t>(
        properties, ddColumnKeyStrings[DD_INSTANT_VERSION_DROPPED], &value);
    cols[i].m_versionDropped = ret ? value : 0;
    if (ret) {
      maxVersion = std::max(maxVersion, value);
    }
    /* Do not process column default value if version is 0, which means that
    the column have't been added/dropped instantly */
    if (maxVersion == 0) {
      i++;
      continue;
    }

    bool isNull = false;
    ret = GetDDPropVal<bool>(properties,
                             ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT_NULL],
                             &isNull);
    if (isNull) {
      i++;
      continue;
    }

    /* Set default value only when this col has been added instantly, if it's
    been dropped instantly later, no need to set default value also. */
    bool needSetDefault = (cols[i].m_isVisible && cols[i].m_versionAdded > 0);

    if (needSetDefault) {
      dd::String_type ddDefaultVal;
      CDE_ASSERT_DEBUG(
          properties.exists(ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT]));
      properties.get(ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT],
                     &ddDefaultVal);
      CDE_ASSERT_DEBUG(ddDefaultVal.size() > 0);

      size_t outLen = 0;
      DDInstantColValCoder decoder;
      DictColDefaultVal *colDefVal = &cols[i].m_instantDefaultVal;
      auto content =
          decoder.Decode(ddDefaultVal.c_str(), ddDefaultVal.size(), &outLen);
      colDefVal->m_col = &cols[i];
      colDefVal->m_len = outLen;
      colDefVal->m_value = new (std::nothrow) uint8_t[outLen];
      if (colDefVal->m_value == nullptr) {
        delete[] cols;
        return nullptr;
      }
      (void)memcpy_s(colDefVal->m_value, colDefVal->m_len, content, outLen);
    }

    i++;
  }

  i = 0;
  uint32_t pos = maxPos;

  if (maxVersion == 0) {
    pos = ~0U;
    for (const dd::Column *ddCol : ddTable->columns()) {
      if (ddCol->is_virtual()) {
        ++i;
        continue;
      }

      ++pos;
      cols[i].m_phyPos = pos;
      ++i;
    }

    maxPos = pos;
  }

  i = 0;

  for (const dd::Column *ddCol : ddTable->columns()) {
    if (ddCol->is_virtual()) {
      ++pos;
      cols[i].m_phyPos = pos;
      ++i;
      continue;
    }
    ++i;
  }

  return cols;
}

cde_dict_t *CdeDdOpenTableOneOnName(char *name, THD *thd, MDL_ticket **mdl,
                                    bool allowEviction) {
  cde_dict_t *dict_table = nullptr;
  std::deque<const char *> fk_list_names;

  dict_table =
      CdeDdOpenTableOneOnName(name, fk_list_names, thd, mdl, allowEviction);

  if (dict_table != nullptr && !fk_list_names.empty()) {
    CdeDdOpenForeignKeyTables(fk_list_names, thd);
  }

  return dict_table;
}

template <typename Table>
cde_dict_t *CdeDdOpenTable(THD *thd, const char *name, const Table *dd_table,
                           const TABLE *form_table) {
  cde_dict_t *dict_table = nullptr;
  std::deque<const char *> fk_list_names;

  dict_table =
      CdeDdOpenTableOne(thd, name, dd_table, form_table, fk_list_names);

  if (dict_table != nullptr && !fk_list_names.empty()) {
    CdeDdOpenForeignKeyTables(fk_list_names, thd);
  }

  return dict_table;
}

template cde_dict_t *CdeDdOpenTable<dd::Table>(THD *thd, const char *name,
                                               const dd::Table *dd_table,
                                               const TABLE *form_table);

template cde_dict_t *CdeDdOpenTable<dd::Partition>(
    THD *thd, const char *name, const dd::Partition *dd_table,
    const TABLE *form_table);

template void CdeDdTableCopyPrivate<dd::Table>(dd::Table &, const dd::Table &);

template <typename Table>
void CdeDdTableCopyPrivate(Table &new_table, const Table &old_table) {
  new_table.se_private_data().clear();

  new_table.set_se_private_id(old_table.se_private_id());
  new_table.set_se_private_data(old_table.se_private_data());

  /* Note that server could provide old and new dd::Table with
  different index order in this case, so always do a double loop */
  for (const auto old_index : old_table.indexes()) {
    auto idx = new_table.indexes()->begin();
    for (; idx != new_table.indexes()->end() &&
           (*idx)->name() != old_index->name();
         ++idx)
      ;

    CDE_ASSERT(idx != new_table.indexes()->end());

    auto new_index = *idx;
    CDE_ASSERT(!old_index->se_private_data().empty());
    CDE_ASSERT(new_index != nullptr);
    CDE_ASSERT(new_index->se_private_data().empty());

    new_index->set_se_private_data(old_index->se_private_data());
  }

  new_table.table().set_row_format(old_table.table().row_format());
}

const char *DDInstantColValCoder::Encode(const unsigned char *stream,
                                         size_t inLen, size_t *outLen) {
  Cleanup();

  m_result = new (std::nothrow) unsigned char[inLen * 2];
  CDE_ASSERT_DEBUG(m_result != nullptr);
  char *result = reinterpret_cast<char *>(m_result);

  for (size_t i = 0; i < inLen; ++i) {
    uint8_t v1 = ((stream[i] & 0xF0) >> 4);
    uint8_t v2 = (stream[i] & 0x0F);

    result[i * 2] = (v1 < 10 ? '0' + v1 : 'a' + v1 - 10);
    result[i * 2 + 1] = (v2 < 10 ? '0' + v2 : 'a' + v2 - 10);
  }

  *outLen = inLen * 2;

  return result;
}

const unsigned char *DDInstantColValCoder::Decode(const char *stream,
                                                  size_t inLen,
                                                  size_t *outLen) {
  CDE_ASSERT_DEBUG(inLen % 2 == 0);

  Cleanup();

  m_result = new (std::nothrow) unsigned char[inLen * 2];
  CDE_ASSERT_DEBUG(m_result != nullptr);

  for (size_t i = 0; i < inLen / 2; ++i) {
    char c1 = stream[i * 2];
    char c2 = stream[i * 2 + 1];

    CDE_ASSERT_DEBUG(isdigit(c1) || (c1 >= 'a' && c1 <= 'f'));
    CDE_ASSERT_DEBUG(isdigit(c2) || (c2 >= 'a' && c2 <= 'f'));

    m_result[i] = ((isdigit(c1) ? c1 - '0' : c1 - 'a' + 10) << 4) +
                  ((isdigit(c2) ? c2 - '0' : c2 - 'a' + 10));
  }

  *outLen = inLen / 2;

  return m_result;
}

bool TableHasDroppedColumn(const dd::Table &ddTtable) {
  for (const auto column : ddTtable.columns()) {
    if (DDColumnIsDropped(column)) {
      return true;
    }
  }

  return false;
}

bool DdIsValidRowVersion(uint32_t version) {
  return (version != INVALID_DD_VERSION && version > 0 &&
          version <= static_cast<uint32_t>(DSTORE_MAX_ROW_VERSION));
}

bool DDColumnIsDropped(const dd::Column *ddCol) {
  const char *s = ddColumnKeyStrings[DD_INSTANT_VERSION_DROPPED];
  if (!ddCol->se_private_data().exists(s)) {
    return false;
  }

#ifdef UNIV_DEBUG
  uint32_t version = INVALID_DD_VERSION;
  ddCol->se_private_data().get(s, &version);
  ut_ad(DdIsValidRowVersion(version));
#endif

  return true;
}

const dd::Column *DDFindColumn(const dd::Table *ddTable, const char *name) {
  for (const dd::Column *c : ddTable->columns()) {
    if (!my_strcasecmp(system_charset_info, c->name().c_str(), name)) {
      return (c);
    }
  }
  return (nullptr);
}

void BuildDroppedColumnName(std::string &name, uint32_t version,
                            uint32_t phyPos) {
  std::ostringstream newName;
  newName << DSTORE_DROP_PREFIX << "v" << version << "_p" << phyPos << "_"
          << name;
  name = newName.str();
  name.resize(std::min<size_t>(name.size(), NAME_CHAR_LEN));
}

bool IsFieldToDrop(const Alter_inplace_info &haAlterInfo,
                   const char *columnName) {
  for (const Alter_drop *drop : haAlterInfo.alter_info->drop_list) {
    if (drop->type != Alter_drop::COLUMN) continue;

    if (!my_strcasecmp(system_charset_info, columnName, drop->name)) {
      return true;
    }
  }

  return false;
}

bool DDCopyDroppedColumns(const dd::Table &oldTable, dd::Table &newTable) {
  for (const auto oldCol : oldTable.columns()) {
    if (!oldCol->is_se_hidden()) {
      continue;
    }

    const char *colName = oldCol->name().c_str();
    const dd::Column *searchedColumn = DDFindColumn(&newTable, colName);
    if (searchedColumn != nullptr) {
      if (!DDColumnIsDropped(searchedColumn)) {
        /* User is trying to add column with name same as existing hidden
         * dropped column name. */

        CDE_LOG_ERROR(
            "Column name %s and internally generated INSTANT DROP column name "
            "%s is causing a conflict",
            searchedColumn->name().c_str(), colName);
        my_error(ER_WRONG_COLUMN_NAME, MYF(0), colName);
        return false;
      }
      /* Column is already present in new table. It is either already dropped
      column in previous statements or is being dropped in same statement. In
      both the cases, continue. */
      continue;
    }

    auto col = DDCopyHiddenColumn(&newTable, oldCol, colName);
    col->se_private_data().clear();
    col->set_se_private_data(oldCol->se_private_data());
    if (col == nullptr) {
      return false;
    }
  }

  return true;
}

dd::Column *DDCopyHiddenColumn(dd::Table *ddTable, const dd::Column *oldCol,
                               const char *name) {
  if (const dd::Column *c = DDFindColumn(ddTable, name)) {
    my_error(ER_WRONG_COLUMN_NAME, MYF(0), c->name().c_str());
    return (nullptr);
  }

  dd::Column *col = ddTable->add_column();
  CDE_ASSERT(col);

  col->set_name(name);
  col->set_hidden(dd::Column::enum_hidden_type::HT_HIDDEN_SE);
  col->set_nullable(oldCol->is_nullable());
  col->set_char_length(oldCol->char_length());
  col->set_numeric_scale(oldCol->numeric_scale());
  col->set_unsigned(oldCol->is_unsigned());
  col->set_collation_id(oldCol->collation_id());
  col->set_type(oldCol->type());

  /* Elements for enum columns */
  if (oldCol->type() == dd::enum_column_types::ENUM ||
      oldCol->type() == dd::enum_column_types::SET) {
    for (const auto *sourceElem : oldCol->elements()) {
      auto *elemObj = col->add_element();
      elemObj->set_name(sourceElem->name());
    }
  }

  return (col);
}

bool IsFieldToRename(const Alter_inplace_info &haAlterInfo, const char *oldName,
                     std::string &newName) {
  List_iterator_fast<Create_field> cfIt(haAlterInfo.alter_info->create_list);
  cfIt.rewind();
  Create_field *cf;
  while ((cf = cfIt++) != nullptr) {
    if (cf->field && cf->field->is_flag_set(FIELD_IS_RENAMED) &&
        !my_strcasecmp(system_charset_info, oldName, cf->change)) {
      /* This field is being renamed */
      newName = cf->field_name;
      return true;
    }
  }

  return false;
}

uint32_t DDColumnGetVersionAdded(const dd::Column *ddCol) {
  if (!DDColumnIsInstantAdded(ddCol)) {
    return INVALID_DD_VERSION;
  }

  uint32_t version = INVALID_DD_VERSION;
  ddCol->se_private_data().get(ddColumnKeyStrings[DD_INSTANT_VERSION_ADDED],
                               &version);
  CDE_ASSERT(DdIsValidRowVersion(version));
  return (version);
}

bool DDColumnIsInstantAdded(const dd::Column *ddCol) {
  const char *s = ddColumnKeyStrings[DD_INSTANT_VERSION_ADDED];
  if (!ddCol->se_private_data().exists(s)) {
    return false;
  }

#ifdef UNIV_DEBUG
  uint32_t version = INVALID_DD_VERSION;
  dd_col->se_private_data().get(s, &version);
  CDE_ASSERT_DEBUG(DdIsValidRowVersion(version));
#endif

  return true;
}

void SetColumnDefaultVal(dd::Properties &privateData, Field &field) {
  /* Set Default NULL */
  if (field.is_real_null()) {
    privateData.set(ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT_NULL], true);
    return;
  }

  /* convert the mysql field to dstore format. */
  DSTORE::Oid baseOid, expandOid;
  bool isUnsigned = false;
  int ret =
      CdeDatatypeMysqlToDstore(&field, &baseOid, &expandOid, true, &isUnsigned);
  CDE_ASSERT(ret == CDE_OK);

  size_t size = field.pack_length();
  unsigned char *mysqlData = field.field_ptr();
  std::unique_ptr<char[]> tupleData;
  if (baseOid == CDE_VARCHAR1B_OID || baseOid == CDE_VARCHAR2B_OID ||
      baseOid == CDE_COMPACT_CHAR_OID) {
    CdeVarlena varlenas;
    bool isBlob = false;
    Datum dstoreValue = 0;
    ret = StoreMysqlFieldToDstoreFormat(field.field_ptr(), baseOid, &field,
                                        false, dstoreValue, varlenas, &isBlob);
    CDE_ASSERT(ret == CDE_OK);
    auto len = CdeParseDatatypeLen(baseOid, dstoreValue);
    tupleData.reset(new (std::nothrow) char[len]);
    CDE_ASSERT(tupleData != nullptr);
    ret = CdeCopyDatumCb(tupleData.get(), len, baseOid, dstoreValue, size);
    CDE_ASSERT(ret == CDE_OK);
    mysqlData = (unsigned char *)(tupleData.get());
  }

  DDInstantColValCoder coder;
  size_t length = 0;
  const char *value = coder.Encode(mysqlData, size, &length);

  dd::String_type defaultValue;
  defaultValue.assign(dd::String_type(value, length));
  privateData.set(ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT], defaultValue);
}

void DDCopyColumnPrivate(const Alter_inplace_info &haAlterInfo,
                         const dd::Table &oldTable, dd::Table &newTable,
                         cde_dict_t *old_dict_table) {
  bool first_row_version = false;
  if (old_dict_table && (old_dict_table->m_currentRowVersion == 0)) {
    first_row_version = true;
  }

  for (const auto oldCol : oldTable.columns()) {
    if (oldCol->is_se_hidden()) {
      /* Must be an already dropped column. */
      CDE_ASSERT_DEBUG(DDColumnIsDropped(oldCol));
      continue;
    }

    /* Skip the dropped column */
    if (IsFieldToDrop(haAlterInfo, oldCol->name().c_str())) {
      continue;
    }

    dd::Column *newCol = nullptr;
    std::string newName;
    if (IsFieldToRename(haAlterInfo, oldCol->name().c_str(), newName)) {
      newCol =
          const_cast<dd::Column *>(DDFindColumn(&newTable, newName.c_str()));
    } else {
      newCol = const_cast<dd::Column *>(
          DDFindColumn(&newTable, oldCol->name().c_str()));
    }

    CDE_ASSERT_DEBUG(newCol != nullptr);

    if (!oldCol->se_private_data().empty()) {
      if (!newCol->se_private_data().empty()) newCol->se_private_data().clear();
      newCol->set_se_private_data(oldCol->se_private_data());
    }

    const char *s = ddColumnKeyStrings[DD_INSTANT_PHYSICAL_POS];
    if (old_dict_table && !newCol->is_virtual() && first_row_version) {
      /* Even the renamed column would have same phy_pos as old column */
      DictCol *col = old_dict_table->get_col_by_name(oldCol->name().c_str());
      newCol->se_private_data().set(s, col->m_phyPos);
    }
  }
}

uint32_t GetNumColsAdded(const Alter_inplace_info *haAlterInfo) {
  uint32_t colsAddedNum = 0;

  /* create_list is list of old columns (CREATE) and new columns (ALTER .. ADD)
   */
  for (const Create_field &newField : haAlterInfo->alter_info->create_list) {
    /* field contains column information for old columns (CREATE),
    and nullptr for new columns (ALTER .. ADD) */
    if (newField.field == nullptr) {
      colsAddedNum++;
    }
  }

  return colsAddedNum;
}

bool DDTableHasRowVersions(const dd::Table &table) {
  if (table.is_temporary()) {
    return false;
  }

  bool hasRowVersion = false;
  for (const auto column : table.columns()) {
    if (column->is_virtual()) {
      continue;
    }

    /* Check if non virtual column has instant added columns or instant dropped
       columns. If we support phy_pos, we can use it to check. */
    if (column->se_private_data().exists(
            ddColumnKeyStrings[DD_INSTANT_VERSION_DROPPED]) ||
        column->se_private_data().exists(
            ddColumnKeyStrings[DD_INSTANT_VERSION_ADDED])) {
      hasRowVersion = true;
      /* Checking only for one column is enough. */
      break;
    }
  }
#ifdef UNIV_DEBUG
  if (hasRowVersion) {
    bool foundInstAddOrDropCol = false;
    for (const auto column : table.columns()) {
      if (DDColumnIsInstantAdded(column) || DDColumnIsDropped(column)) {
        foundInstAddOrDropCol = true;
        break;
      }
    }
    CDE_ASSERT_DEBUG(foundInstAddOrDropCol);
  }
#endif

  return hasRowVersion;
}

int32_t DDClearInstantTable(dd::Table &dd_table) {
  int32_t ret = CDE_OK;
  auto columns = dd_table.columns();
  if (columns == nullptr) {
    return HA_ERR_GENERIC;
  }
  std::vector<std::string> colsToDrop;
  for (auto col : *columns) {
    auto fn = [&](const char *s) {
      if (col->se_private_data().exists(s)) {
        col->se_private_data().remove(s);
      }
    };
    /* Possibly an INSTANT ADD/DROP column with a version */
    if (DDColumnIsDropped(col)) {
      colsToDrop.push_back(col->name().c_str());
      continue;
    }

    fn(ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT_NULL]);
    fn(ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT]);
    fn(ddColumnKeyStrings[DD_INSTANT_VERSION_ADDED]);
    fn(ddColumnKeyStrings[DD_INSTANT_VERSION_DROPPED]);
    fn(ddColumnKeyStrings[DD_INSTANT_PHYSICAL_POS]);
  }

  if (!colsToDrop.empty()) {
    for (auto colName : colsToDrop) {
      if (!dd_table.recycle_bin_drop_column(colName.c_str())) {
        CDE_LOG_ERROR(
            "Failed to clear instant drop column metadata for table "
            "%s",
            dd_table.name().c_str());
        my_error(
            ER_INTERNAL_ERROR, MYF(0),
            "Failed to truncate table. You may drop and re-create this table.");
        ret = HA_ERR_GENERIC;
      }
      CDE_ASSERT_DEBUG(ret == CDE_OK);
    }
  }
  colsToDrop.clear();
  return ret;
}

void CdeGetDstoreTableDDInfo(String *packet, const dd::Table *table_obj) {
  /* check table engine type is dstore */
  CDE_ASSERT(CdeStrcasecmp(table_obj->engine().c_str(), DSTORE_ENGINE_NAME) ==
             0);

  uint32_t rel_oid =
      CdeDdTableGetTableRelOid(const_cast<dd::Table *>(table_obj));
  uint16_t file_id =
      CdeDdTableGetSePrivateDataRelfileid(const_cast<dd::Table *>(table_obj));
  uint32_t block_id =
      CdeDdTableGetSePrivateDataRelblknum(const_cast<dd::Table *>(table_obj));
  std::ostringstream dstore_info;
  dstore_info << " /*!80041 dstore_ddl_comment='FOR_DD_REPLAY=1;tablerelid="
              << rel_oid << ";relblknum=" << block_id
              << ";relfileid=" << file_id;

  uint16_t lobFileId = CdeTableGetLobFileId(table_obj);
  uint32_t lobBlockNum = CdeTableGetLobBlockNumber(table_obj);
  if (lobFileId != DSTORE::INVALID_VFS_FILE_ID &&
      lobBlockNum != DSTORE::DSTORE_INVALID_BLOCK_NUMBER) {
    dstore_info << ";rellobblknum=" << lobBlockNum
                << ";rellobfileid=" << lobFileId;
  }

  dstore_info << ";' */";
  packet->append(dstore_info.str().c_str(), dstore_info.str().size(),
                 system_charset_info);
}

void CdeGetDstoreIndexDDInfo(String *packet, const dd::Table *table_obj,
                             size_t key_no) {
  packet->append(' ');
  uint16_t file_id = CdeDdIndexGetSePrivateDataRelfileid(
      const_cast<dd::Table *>(table_obj), key_no);
  uint32_t block_id = CdeDdIndexGetSePrivateDataRelblknum(
      const_cast<dd::Table *>(table_obj), key_no);
  uint32_t rel_oid =
      CdeDdIndexGetTableRelOid(const_cast<dd::Table *>(table_obj), key_no);

  std::ostringstream dstore_key_info;
  dstore_key_info << "/*!80041 dstore_ddl_comment 'indexrelid=" << rel_oid
                  << ";relblknum=" << block_id << ";relfileid=" << file_id
                  << ";' */";
  packet->append(dstore_key_info.str().c_str(), dstore_key_info.str().size(),
                 system_charset_info);
}

NumberOfFields FillFieldLenFromDD(const dd::Table *ddTable,
                                  const TABLE *formTable,
                                  std::vector<DictFieldLen> &fieldLenInfos) {
  NumberOfFields numberOfFields;

  const size_t nCols = ddTable->columns().size();
  fieldLenInfos.resize(nCols);

  uint32_t fieldNr = 0;
  uint32_t fieldLenNr = 0;
  for (const dd::Column *ddCol : ddTable->columns()) {
    if (ddCol->is_se_hidden() || ddCol->is_virtual()) {
      ++fieldLenNr;
      if (ddCol->is_virtual()) {
        ++fieldNr;
        ++numberOfFields.virtualFieldsNum;
      } else {
        ++numberOfFields.nonVirtualFieldsNum;
      }
      continue;
    }

    const auto tablField = formTable->field[fieldNr];
    // MYSQL_TYPE_STRING/MYSQL_TYPE_VARCHAR can cover
    // CDE_COMPACT_CHAR_OID/CDE_VARCHAR1B_OID/CDE_VARCHAR2B_OID. if blob prefix
    // index is supported or CDE_CHAR_OID is used, need check whether need
    // modify the condition.
    if (tablField->type() == MYSQL_TYPE_STRING ||
        tablField->type() == MYSQL_TYPE_VARCHAR) {
      fieldLenInfos[fieldLenNr].m_fieldLenBytes =
          (tablField->type() == MYSQL_TYPE_STRING)
              ? 0
              : tablField->get_length_bytes();
      fieldLenInfos[fieldLenNr].m_packLength = tablField->pack_length();
      fieldLenInfos[fieldLenNr].m_mbmaxlen = tablField->charset()->mbmaxlen;
      fieldLenInfos[fieldLenNr].m_mbminlen = tablField->charset()->mbminlen;
    }

    ++fieldLenNr;
    ++fieldNr;
    ++numberOfFields.nonVirtualFieldsNum;
  }
  return numberOfFields;
}

void DDwriteDefaultValue(const DictCol *col, dd::Column *ddCol) {
  if (col->m_instantDefaultVal.m_value == nullptr) {
    ddCol->se_private_data().set(
        ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT_NULL], true);
    return;
  }

  size_t length = 0;
  DDInstantColValCoder coder;
  const char *value = coder.Encode(col->m_instantDefaultVal.m_value,
                                   col->m_instantDefaultVal.m_len, &length);

  dd::String_type defaultValue;
  defaultValue.assign(dd::String_type(value, length));
  ddCol->se_private_data().set(ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT],
                               defaultValue);
}

template <typename Index>
const cde_dict_index_t *DDFindIndex(const cde_dict_t *table, Index *dd_index) {
  /* The order could be different because all unique dd::Index(es)
  would be in front of other indexes. */
  for (auto index : table->index_dict_vec) {
    if (!index->IsCommitted()) {
      continue;
    }

    if (my_strcasecmp(system_charset_info, index->name.c_str(),
                      dd_index->name().c_str()) == 0) {
      return index;
    }
  }

  return nullptr;
}

char *DDGetRefrencedTable(const char *name, const char *database_name,
                          const char *table_name, cde_dict_t **table,
                          MDL_ticket **mdl, MEM_ROOT *memRoot) {
  const char *db_name;
  bool is_part = false;

  *table = nullptr;

  DBUG_EXECUTE_IF("dstore_inplace_alter_database_is_nullptr",
                  database_name = nullptr;);
  char dbName[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  if (!database_name) {
    /* Use the database name of the foreign key table */
    uint32_t dbNameSize = 0;
    const char *dbNamePtr = GetDBName(name, dbNameSize);
    CDE_ASSERT(dbNamePtr && dbNameSize);
    if (0 != strncpy_s(dbName, sizeof(dbName), dbNamePtr, dbNameSize)) {
      CDE_LOG_ERROR("strncpy_s failed during copying db name.");
      return nullptr;
    }
    db_name = dbName;
  } else {
    dbName[0] = '.';
    dbName[1] = '/';
    strncpy_s(dbName + 2, sizeof(dbName) - 2, database_name,
              strlen(database_name));
    db_name = dbName;
  }

  uint32_t database_name_len = strlen(db_name);
  uint32_t table_name_len = strlen(table_name);
  char buf[FN_REFLEN + 1] = {0};
  memcpy_s(buf, database_name_len, db_name, database_name_len);
  buf[database_name_len] = '/';
  memcpy_s(buf + database_name_len + 1, table_name_len + 1, table_name,
           table_name_len + 1);

  /* Values;  0 = Store and compare as given; case sensitive
              1 = Store and compare in lower; case insensitive
              2 = Store as given, compare in lower; case semi-sensitive */
  if (lower_case_table_names == 2) {
    my_casedn_str(system_charset_info, buf);
    if (!is_part) {
      *table = CdeDdOpenTableOneOnName(buf, current_thd, mdl, false);
    }
    memcpy_s(buf, database_name_len, db_name, database_name_len);
    buf[database_name_len] = '/';
    memcpy_s(buf + database_name_len + 1, table_name_len + 1, table_name,
             table_name_len + 1);
  } else {
#ifndef _WIN32
    if (lower_case_table_names == 1) {
      my_casedn_str(system_charset_info, buf);
    }
#endif /* !_WIN32 */
    if (!is_part) {
      *table = CdeDdOpenTableOneOnName(buf, current_thd, mdl, false);
    }
  }

  return safe_strdup_root(memRoot, buf);
}

/** Copy the AUTO_INCREMENT if exist.
@param[in]      src     dd::Table::se_private_data to copy from
@param[out]     dest    dd::Table::se_private_data to copy to */
void DDCopyAutoinc(const dd::Properties &src, dd::Properties &dest) {
  uint64_t autoIncVal = 0;
  if (!src.exists(auto_inc)) {
    return;
  }
  src.get(auto_inc, &autoIncVal);
  dest.set(auto_inc, autoIncVal);
}
} /* namespace CDE */
