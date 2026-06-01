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

#include <type_traits>

#include "boot/cde_instance.h"
#include "catalog/dstore_fake_attribute.h"
#include "common/cde_compare_utils.h"
#include "common/cde_errorcode.h"
#include "common/cde_prototypes.h"
#include "common/cde_typecache.h"
#include "dd_helper.h"
#include "ddl/cde_dd_table.h"
#include "ddl/cde_ddl.h"
#include "ddl/cde_ddl_log.h"
#include "ddl/cde_mysql_ops.h"
#include "ddl/cde_tablespace.h"
#include "dict/cde_dict_bg_stat.h"
#include "dict/cde_dict_stat.h"
#include "dict/cde_relation.h"
#include "dml/cde_heap.h"
#include "errorcode/dstore_tablespace_error_code.h"
#include "heap/dstore_heap_struct.h"
#include "index/dstore_index_struct.h"
#include "my_sqlcommand.h"
#include "mysql/plugin.h"
#include "sql/dd/impl/properties_impl.h"
#include "sql/dd/types/foreign_key_element.h"
#include "sql/dd/types/partition.h"
#include "sql/dd/types/partition_index.h"
#include "sql/dd/types/table.h"
#include "sql/debug_sync.h"
#include "sql/field.h"
#include "sql/sql_table.h"
#include "systable/dstore_systable_interface.h"
#include "systable/systable_relation.h"
#include "tablespace/dstore_tablespace.h"
#include "tablespace/dstore_tablespace_interface.h"

#include <sql_thd_internal_api.h>

using DSTORE::AttrNumber;
using DSTORE::DSTORE_FAIL;
using DSTORE::DSTORE_INVALID_OID;
using DSTORE::DSTORE_SUCC;
using DSTORE::IndexBuildInfo;
using DSTORE::IndexInfo;
using DSTORE::PageId;
using DSTORE::RetStatus;
using DSTORE::ScanKey;
using DSTORE::SegmentType;
using DSTORE::StorageRelation;
using DSTORE::StorageRelationData;
using DSTORE::SYS_PARTTYPE_NON_PARTITIONED_RELATION;
using DSTORE::SYS_RELKIND_INDEX;
using DSTORE::SYS_RELKIND_RELATION;
using DSTORE::SYS_RELPERSISTENCE_PERMANENT;
using DSTORE::SysClassTupDef;
using DSTORE::SysIndexTupDef;
using DSTORE::TablespaceId;
using DSTORE::TBS_ID;
using DSTORE::TupleDesc;

static_assert(std::is_same<DSTORE::FileId, uint16_t>::value == true,
              "DSTORE::FileId is not uint16_t");
static_assert(std::is_same<DSTORE::BlockNumber, uint32_t>::value == true,
              "DSTORE::BlockNumber is not uint32_t");

namespace CDE {
/** DStore engine default heap fillfactor value, used for variable
cde_default_fillfactor. */
int32_t g_defaultFillfactor = DEFAULT_FILLFACTOR;

/** DStore engine default index fillfactor value, used for variable
cde_default_fillfactor_for_index. */
int32_t g_defaultFillfactorForIndex = DEFAULT_FILLFACTOR_FOR_INDEX;

void cde_index_def::destroy_from_cache() {
  if (fields != nullptr) {
    CdeFree(fields);
  }
}

static void CdeDropSegmentOnDictTable(cde_dict_t *table) {
  DSTORE::PageId segmentId = {table->dstore_relation->rel->relfileid,
                              table->dstore_relation->rel->relblknum};
  CdeAllocSegmentWrapper::Drop(segmentId,
                               DSTORE::SegmentType::HEAP_TEMP_SEGMENT_TYPE,
                               CDE_TMP_TABLE_SPACE_ID);
  if (table->dstore_relation->attr->tdhaslob) {
    segmentId = {table->dstore_relation->rel->rellobfileid,
                 table->dstore_relation->rel->rellobblknum};
    CdeAllocSegmentWrapper::Drop(segmentId,
                                 DSTORE::SegmentType::HEAP_TEMP_SEGMENT_TYPE,
                                 CDE_TMP_TABLE_SPACE_ID);
  }
  for (auto index : table->index_dict_vec) {
    if (!index->IsCommitted() ||
        index->GetOnlineStatus() != OnlineIndexStatus::ONLINE_INDEX_COMPLETE) {
      continue;
    }

    segmentId = {index->rel->rel->relfileid, index->rel->rel->relblknum};
    CdeAllocSegmentWrapper::Drop(segmentId,
                                 DSTORE::SegmentType::INDEX_TEMP_SEGMENT_TYPE,
                                 CDE_TMP_TABLE_SPACE_ID);
  }
}

template <typename Table>
bool CdeDropSegmentOnDdObj(THD *thd, const Table *ddTableDef) {
  DSTORE::TablespaceId tablespaceId = CdeGetSpaceIdByDD(ddTableDef);
  bool ret = CDE_SUCC;
  PageId heapSegmentId;
  heapSegmentId.m_fileId = CdeDdTableGetSePrivateDataRelfileid(ddTableDef);
  heapSegmentId.m_blockId = CdeDdTableGetSePrivateDataRelblknum(ddTableDef);
  if (heapSegmentId.IsInvalid()) {
    CDE_LOG_ERROR("Heap segmentID[%d,%d] of table:%s is invalid.",
                  heapSegmentId.m_fileId, heapSegmentId.m_blockId,
                  ddTableDef->name().c_str());
  } else {
    ret = CdeDdlLog::GetInstance()->LogDropSegment(
        thd, heapSegmentId, DSTORE::SegmentType::HEAP_SEGMENT_TYPE,
        tablespaceId, false);
    if (ret == CDE_FAIL) {
      CDE_LOG_ERROR("Drop table: %s fail due to LogDropSegment of heap fail.",
                    ddTableDef->name().c_str());
      return CDE_FAIL;
    }
  }

  DSTORE::PageId lobSegmentId;
  lobSegmentId.m_fileId = CdeTableGetLobFileId(ddTableDef);
  lobSegmentId.m_blockId = CdeTableGetLobBlockNumber(ddTableDef);
  if (lobSegmentId.IsValid()) {
    ret = CdeDdlLog::GetInstance()->LogDropSegment(
        thd, lobSegmentId, DSTORE::SegmentType::HEAP_SEGMENT_TYPE, tablespaceId,
        false);
    if (ret == CDE_FAIL) {
      CDE_LOG_ERROR("Drop table: %s fail due to LogDropSegment of lob fail.",
                    ddTableDef->name().c_str());
      return CDE_FAIL;
    }
  }

  for (size_t idxNo = 0; idxNo < ddTableDef->indexes().size(); ++idxNo) {
    PageId idxSegmentId;
    idxSegmentId.m_fileId =
        CdeDdIndexGetSePrivateDataRelfileid(ddTableDef, idxNo);
    idxSegmentId.m_blockId =
        CdeDdIndexGetSePrivateDataRelblknum(ddTableDef, idxNo);
    if (idxSegmentId.IsInvalid()) {
      CDE_LOG_ERROR("Index:%lu's segmentID[%d,%d] of table:%s is invalid.",
                    idxNo, idxSegmentId.m_fileId, idxSegmentId.m_blockId,
                    ddTableDef->name().c_str());
    } else {
      ret = CdeDdlLog::GetInstance()->LogDropSegment(
          thd, idxSegmentId, DSTORE::SegmentType::INDEX_SEGMENT_TYPE,
          tablespaceId, false);
      if (ret == CDE_FAIL) {
        CDE_LOG_ERROR("Drop table: %s fail due to LogDropSegment of lob fail.",
                      ddTableDef->name().c_str());
        return CDE_FAIL;
      }
    }
  }
  return CDE_SUCC;
}

template bool CdeDropSegmentOnDdObj(THD *thd, const dd::Table *ddTableDef);
template bool CdeDropSegmentOnDdObj(THD *thd, const dd::Partition *ddTableDef);

int CdeCreateTable(THD *thd, const char *name, TABLE *form,
                   HA_CREATE_INFO *createInfo, dd::Table *tableDef) {
  if (name == nullptr || form == nullptr) {
    return -1;
  }

  cde_dict_t *tbl = DictSysGetTable(name);  // table name and database name
  DictTableRefGuard dictRefGuard(tbl);
  if (tbl != nullptr) {
    if (rds_dstore_enable_atomic_ddl == false && strstr(name, "#sql") != 0) {
      dictRefGuard.Release();
      DictSysRemoveTable(name);
    } else {
      return HA_ERR_TABLE_EXIST;
    }
  }

  if (form->s->fields > DSTORE::MAX_TUPLE_ATTR) {
    return HA_ERR_TOO_MANY_FIELDS;
  }

  CdeCreateTableInfo info(thd, name, form, createInfo, tableDef);
  // prepare create table
  int ret = info.PrepareCreateTable(name);
  if (ret != 0) {
    CDE_LOG_ERROR("CdeCreateTableInfo.prepare_create_table fail! name = %s",
                  name);
    return ret;
  }

  ret = info.CreateTable();
  if (ret != 0) {
    CDE_LOG_ERROR("cde_create_table_info.create_table fail! name = %s", name);
    return ret;
  }

  ret = info.CreateTableUpdateDict<dd::Table>(tableDef);
  if (ret == 0) {
    CDE_LOG_SYSTEM("Create Impl Success! table name = %s", name);
  } else {
    CDE_LOG_ERROR("Create Impl Failed! ret = %d", ret);
    return -1;
  }
  return 0;
}

template <typename Table>
bool CdeDropTable(THD *thd, const char *name, const Table *tableDef) {
  bool ret = CDE_OK;
  if (tableDef && !tableDef->is_persistent()) {
    cde_dict_t *tbl = DictSysGetTable(name);
    if (!tbl) return ret;
    {
      DictTableRefGuard dictRefGuard(tbl);
      CdeDropSegmentOnDictTable(tbl);
    }
    DictSysRemoveTable(name);
  } else {
    DictSysRemoveTable(name);
    ret = CdeDropSegmentOnDdObj(thd, tableDef);
  }
  CDE_LOG_DEBUG("Delete table %s %s.", name,
                ret == CDE_SUCC ? "success" : "fail");
  return ret;
}

template bool CdeDropTable<dd::Table>(THD *thd, const char *name,
                                      const dd::Table *tableDef);
template bool CdeDropTable<dd::Partition>(THD *thd, const char *name,
                                          const dd::Partition *tableDef);

struct CStrComparator {
  bool operator()(const char *a, const char *b) const {
    return std::strcmp(a, b) < 0;
  }
};

template <typename Table>
int32_t CdeTruncateTable(THD *thd, const char *name, Table *tableDef) {
  DBUG_TRACE;
  DBUG_EXECUTE_IF("cde_truncate_impl_injection", tableDef = nullptr;);
  if (tableDef == nullptr) {
    return HA_ERR_INTERNAL_ERROR;
  }

  cde_dict_t *dictTable = DictSysGetTable(name);
  if (dictTable == nullptr) {
    if (!tableDef->table().is_persistent()) {
      return 0;
    }

    dictTable = CdeDdOpenTableOnDdObj(thd, name, tableDef);
    if (dictTable == nullptr) {
      return HA_ERR_NO_SUCH_TABLE;
    }
  }

  DictTableRefGuard dictRefGuard(dictTable);
  bool ret = CdeTruncate::DoTruncate(tableDef, dictTable);

  if (ret != CDE_SUCC && CheckIsInterrupted()) {
    dictRefGuard.Release();
    DictSysRemoveTable(name);
    return HA_ERR_QUERY_INTERRUPTED;
  }

  DBUG_EXECUTE_IF("cde_truncate_impl_exec_injection", ret = CDE_FAIL;);
  HaErrorCode err = CDE_OK;
  if (ret != CDE_SUCC) {
    err = GetAndConvertDstoreErrcodeToMysql();
  }

  if (!dictTable->is_temporary()) {
    if (CdeDdlLog::GetInstance()->LogRemoveCache(thd, name) == CDE_FAIL) {
      dictRefGuard.Release();
      DictSysRemoveTable(name);
      CDE_LOG_ERROR("Truncate table:%s fail due to LogRemoveCache fail.", name);
      return HA_ERR_GENERIC;
    }
  }

  return err;
}

template int32_t CdeTruncateTable<dd::Table>(THD *thd, const char *name,
                                             dd::Table *tableDef);
template int32_t CdeTruncateTable<dd::Partition>(THD *thd, const char *name,
                                                 dd::Partition *tableDef);

int CdeRenameTable(THD *thd, const char *from, const char *to,
                   cde_dict_t *dictTable, dd::Table *toTableDef) {
  char norm_to[FN_REFLEN];
  char norm_from[FN_REFLEN];
  if (CdeCreateTableInfo::NormalizeTableName(norm_to, to) != 0 ||
      CdeCreateTableInfo::NormalizeTableName(norm_from, from) != 0) {
    CDE_LOG_SYSTEM("normalize_table_name fail! from = %s, to = %s", from, to);
    return HA_ERR_TOO_LONG_PATH;
  }

  if (strcmp(norm_from, norm_to) == 0) {
    CDE_LOG_SYSTEM("rename is same! from = %s, to = %s", from, to);
    return 0;
  }
  int error = CDE_OK;

  bool oldIsTmp = CdeIsMysqlTmpTableName(from);
  bool newIsTmp = CdeIsMysqlTmpTableName(to);

  if (CdeDdlLog::GetInstance()->LogRename(thd, from, to) == CDE_FAIL) {
    CDE_LOG_ERROR("Rename table from %s to %s fail due to LogRename fail.",
                  from, to);
    return CDE_ERROR;
  }

  if (oldIsTmp && !newIsTmp) {
    /** ALTER TABLE - intermediate tmp table*/
    OperationalLockGuard opGuard;

    if (dictTable->dict_stats_persist_enabled()) {
      (void)CdeDictTableStatsRecalc(thd, dictTable, true);
    }

    dictTable->refresh_fk = true;
    error = DictSysRenameTable(from, to, false);
    if (error != CDE_OK) {
      return error;
    }
    DBUG_EXECUTE_IF("cde_alter_table_error_after_rename_dict_obj1",
                    return CDE_ERROR;);
  } else {
    std::deque<const char *> fk_names;
    {
      OperationalLockGuard opGuard;
      /** For temporary tables foreign keys are not supported.
      Do not rename foreign keys constraints. Since there are no
      foreign keys defined, rest of the operations for foreign keys
      in DictSysRenameTable are no-op.*/
      error = DictSysRenameTable(from, to, !newIsTmp);
      if (error != CDE_OK) {
        return error;
      }
      DBUG_EXECUTE_IF("cde_alter_table_error_after_rename_dict_obj2",
                      return CDE_ERROR;);
      /** refresh_fk does not make sense for partitioned tables - as they do
       * not contain foreign keys nor can be referenced by foreign keys. */
      CDE_ASSERT(IsPartition(dictTable->name) ||
                 false == dictTable->refresh_fk);
      error = CdeDdTableLoadForeignKey(thd, to, dictTable, toTableDef, fk_names,
                                       false);
      if (CDE_OK != error) {
        for (auto fkName : fk_names) CdeFree(const_cast<char *>(fkName));
        return error;
      }
    }
    /** Make sure that "from" table will not be openned during foreign tables
     * load. */
    for (auto iter = fk_names.begin(); iter != fk_names.end();) {
      if (0 == std::strcmp(from, *iter)) {
        auto fkName = *iter;
        iter = fk_names.erase(iter);
        CdeFree(const_cast<char *>(fkName));
      } else
        ++iter;
    }
    CdeDdOpenForeignKeyTables(fk_names, thd);
  }

  DBUG_EXECUTE_IF("cde_alter_table_copy_rename_table_error1", {
    if (newIsTmp) {
      error = CDE_ERROR;
    }
  });

  DBUG_EXECUTE_IF("cde_alter_table_copy_rename_table_error2", {
    if (oldIsTmp) {
      error = CDE_ERROR;
    }
  });
  return error;
}

void DiscardAfterDDL(cde_dict_t *dictTable, TABLE *table) {
  dictTable->m_discardAfterDDL = true;
  /* This table handler should not be reused - it should not
  get stored inside server's level Table cache. Otherwise, there
  might be multiple references to dictTable (from different
  handlers) that we want to discard after ddl. During open, when
  we check m_discardAfterDDL and we want to reload cde_dict_t
  for this table - we need to be sure there are no more references
  to this cde_dict_t (no handlers exists). Thus, here we invalidate
  table's dict. This will cause server to close the header instead
  of putting it into Table cache for later reuse.
  */
  table->invalidate_dict();
}

/**
 * @brief Check a column is a base column for any stored
 * generated column in the table.
 *
 * @param[in] table TABLE* for the table.
 * @param[in] name  column name to check.
 *
 * @return true if this is a base column, otherwise false.
 */
bool IsBaseStoredColumn(const TABLE *table, const char *name) {
  for (uint32_t i = 0; i < table->s->fields; ++i) {
    const Field *field = table->field[i];

    if (!IsStoredGeneratedField(field)) {
      continue;
    }

    for (uint32_t j = 0; j < table->s->fields; ++j) {
      if (bitmap_is_set(&field->gcol_info->base_columns_map, j)) {
        const Field *base_field = table->field[j];
        if (CdeStrcasecmp(base_field->field_name, name) == 0) {
          return (true);
        }
      }
    }
  }
  return (false);
}

/**
 * @brief Check any cascading foreign key columns are base
 * columns for any stored generated columns in the table.
 *
 * @param[in] dd_table dd::Table for the table.
 * @param[in] table    TABLE* for the table.
 *
 * @return  true if found or false.
 */
static bool CheckFkBaseCol(const dd::Table *dd_table, const TABLE *table) {
  for (const dd::Foreign_key *key : dd_table->foreign_keys()) {
    bool upd_cascade = false;
    bool del_cascade = false;

    switch (key->update_rule()) {
      case dd::Foreign_key::RULE_CASCADE:
      case dd::Foreign_key::RULE_SET_NULL:
        upd_cascade = true;
        break;
      case dd::Foreign_key::RULE_NO_ACTION:
      case dd::Foreign_key::RULE_RESTRICT:
      case dd::Foreign_key::RULE_SET_DEFAULT:
        break;
    }

    switch (key->delete_rule()) {
      case dd::Foreign_key::RULE_CASCADE:
      case dd::Foreign_key::RULE_SET_NULL:
        del_cascade = true;
        break;
      case dd::Foreign_key::RULE_NO_ACTION:
      case dd::Foreign_key::RULE_RESTRICT:
      case dd::Foreign_key::RULE_SET_DEFAULT:
        break;
    }

    if (!upd_cascade && !del_cascade) {
      continue;
    }

    for (const dd::Foreign_key_element *key_e : key->elements()) {
      dd::String_type col_name = key_e->column().name();

      if (IsBaseStoredColumn(table, col_name.c_str())) {
        return true;
      }
    }
  }
  return false;
}

int32_t DstoreParseFillfactor(const char *str, THD *thd) {
  /* return 0 if str is empty */
  if (str == nullptr) {
    return 0;
  }

  /* locate a substring 'fillfactor=', do case-insensitive comparison, ignore
  space before and after the '=' */
  std::regex pattern(R"(fillfactor\s*=\s*(\d+))", std::regex_constants::icase);
  std::string input(str);
  std::smatch matches;

  /* return 0 if fillfactor pattern not found */
  if (!std::regex_search(input, matches, pattern)) {
    return 0;
  }

  /* do not use 'std::stoi' since it may throw exceptions */
  int32_t ff = atoi(matches[1].str().c_str());

  if (IsValidFillfactor(ff)) {
    return ff;
  }

  /* print warning to clint and return 0 if fillfactor value is invalid */
  if (thd != nullptr) {
    push_warning_printf(thd, Sql_condition::SL_WARNING,
                        ER_ILLEGAL_HA_CREATE_OPTION,
                        "DStore: The valid range for fillfactor is [10, 100]."
                        " The value is ignored.");
  }
  return 0;
}

IndexBuildInfo CreateIndexBuildInfo(StorageRelation heapStorRel,
                                    TupleDesc fullAttr, uint32_t indexAttrNum,
                                    uint32_t *attrCols,
                                    cde_dict_field_t *fields) {
  /* generate IndexBuildInfo */
  IndexBuildInfo buildInfo;
  int err = 0;
  err = memset_s(&buildInfo, sizeof(IndexBuildInfo), 0, sizeof(IndexBuildInfo));
  if (err != 0) {
    CDE_ASSERT(0);
    CDE_LOG_ERROR("memset_s fail! err = %d", err);
  }
  buildInfo.heapRelationOid = heapStorRel->relOid;
  /** attributes including generated virtual columns */
  buildInfo.heapAttributes = fullAttr;
  buildInfo.heapRels = static_cast<StorageRelation *>(
      CdeAlloc(sizeof(StorageRelation) * 1));  // todo：only one table
  buildInfo.heapRels[0] = heapStorRel;
  buildInfo.heapRels[0]->tableSmgr = heapStorRel->tableSmgr;
  buildInfo.heapRelNum = 1;
  buildInfo.allPartOids = nullptr;
  buildInfo.allPartTuples = nullptr;
  buildInfo.heapTuples = 0;
  buildInfo.indexTuples = 0;

  for (uint8_t i = 0; i < indexAttrNum; i++) {
    /* get attribute number of key */
    fields[i].attId = attrCols[i];
    if (fields[i].m_isVirtual || fields[i].prefix_len != 0) {
      buildInfo.indexAttrOffset[i] = 0;
    } else {
      buildInfo.indexAttrOffset[i] = (AttrNumber)attrCols[i] + 1;
    }
  }
  return buildInfo;
}

template <typename Table>
static void UpdateDataDictionaryInfo(Table *ddTable,
                                     const StorageRelation heapRelation,
                                     cde_dict_t *dict_table) {
  /* table se private data */
  CdeDdTableSetSePrivateData(ddTable, heapRelation, dict_table);
  /* indexes se private data */
  CdeDdIndexSetSePrivateData(ddTable, dict_table);
}

static void SetSegmemtInfo(DSTORE::SysClassTupDef *classTuple,
                           DSTORE::PageId segmentId,
                           DSTORE::PageId lobSegmentId) {
  classTuple->relfileid = segmentId.m_fileId;
  classTuple->relblknum = segmentId.m_blockId;
  if (lobSegmentId.IsValid()) {
    classTuple->rellobfileid = lobSegmentId.m_fileId;
    classTuple->rellobblknum = lobSegmentId.m_blockId;
  } else {
    classTuple->rellobfileid = DSTORE::INVALID_VFS_FILE_ID;
    classTuple->rellobblknum = DSTORE::DSTORE_INVALID_BLOCK_NUMBER;
  }
}

CdeCreateTableInfo::CdeCreateTableInfo(THD *thd, const char *name, TABLE *form,
                                       HA_CREATE_INFO *create_info,
                                       dd::Table *dd_tab)
    : m_thd(thd),
      m_name(name),
      m_form(form),
      m_create_info(create_info),
      m_dd_tab(dd_tab),
      m_dict(nullptr) {
  // StorageInstanceInterface* instance
}

CdeCreateTableInfo::~CdeCreateTableInfo() {
  if (nullptr != m_dict) m_dict->release();
  // destroy();
}

bool CdeCreateTableInfo::ValidateTableSpaceOption() {
  /** this logic should in dd/dd_table.cc invalid_tablespace_usage,
  but now we put it in engine's check. */
  if (m_create_info->tablespace &&
      strcmp(m_create_info->tablespace, dstore_global_spacename) == 0) {
    my_error(ER_RESERVED_TABLESPACE_NAME, MYF(0), m_name,
             dstore_global_spacename);
    return false;
  }

  if (m_isTmpTable) {
    bool isNotTempTblSpace =
        m_create_info->tablespace &&
        (strcmp(m_create_info->tablespace, dstore_temp_spacename) != 0);
    if (isNotTempTblSpace) {
      my_printf_error(ER_ILLEGAL_HA_CREATE_OPTION,
                      "DStore: Tablespace `%s` cannot contain"
                      " TEMPORARY tables.",
                      MYF(0), m_create_info->tablespace);
      return false;
    }
  } else {
    m_tableSpaceId = CdeGetSpaceIdByName(m_create_info->tablespace);
    if (m_tableSpaceId == CDE_TMP_TABLE_SPACE_ID) {
      my_printf_error(ER_ILLEGAL_HA_CREATE_OPTION,
                      "DStore: Tablespace `%s` can only contain"
                      " TEMPORARY tables.",
                      MYF(0), m_create_info->tablespace);
      return false;
    } else if (m_tableSpaceId == DSTORE::INVALID_TABLESPACE_ID) {
      CDE_ASSERT_DEBUG(m_create_info->tablespace);
      my_printf_error(ER_TABLESPACE_MISSING,
                      "DStore: A general tablespace named"
                      " `%s` cannot be found.",
                      MYF(0), m_create_info->tablespace);
      return false;
    }
  }

  return true;
}

int CdeCreateTableInfo::PrepareCreateTable(const char *name, bool normalize) {
  if (name == nullptr) {
    return CDE_ERROR;
  }

  if (m_create_info->options & HA_LEX_CREATE_TMP_TABLE) {
    m_isTmpTable = true;
    m_tableSpaceId = CDE_TMP_TABLE_SPACE_ID;
  }

  if (opt_initialize) {
    m_tableSpaceId = CDE_GLOBAL_TABLE_SPACE_ID;
  } else if (!ValidateTableSpaceOption()) {
    return HA_WRONG_CREATE_OPTION;
  }

  int ret = CDE_OK;

  if (normalize) {
    ret = NormalizeTableName(m_tableName, name);
  } else {
    strcpy_s(m_tableName, FN_REFLEN, name);
  }

  // todo: use norm_name
  return ret;
}

DSTORE::SysClassTupDef *CdeCreateTableInfo::CreateSysRelTuple(
    char relkind, int16_t colNum, DSTORE::PageId segmentId,
    DSTORE::PageId lobSegmentId) {
  DSTORE::SysClassTupDef *tupDef = static_cast<DSTORE::SysClassTupDef *>(
      CdeZalloc(sizeof(DSTORE::SysClassTupDef)));
  CDE_ASSERT(tupDef != nullptr);
  // not use dstore table name, relation->relname.data
  tupDef->relnamespace = 11;  // todo: ?
  tupDef->reltablespace = m_tableSpaceId;
  tupDef->reltype = DSTORE_INVALID_OID;
  tupDef->reloftype = DSTORE_INVALID_OID;
  tupDef->relowner = 10;
  tupDef->relam = 0;
  tupDef->reltoastrelid = DSTORE_INVALID_OID;
  tupDef->reltoastidxid = DSTORE_INVALID_OID;

  tupDef->reldeltarelid = DSTORE_INVALID_OID;
  tupDef->reldeltaidx = DSTORE_INVALID_OID;
  tupDef->relcudescrelid = DSTORE_INVALID_OID;
  tupDef->relcudescidx = DSTORE_INVALID_OID;
  tupDef->relhasindex = false;
  tupDef->relisshared = false;
  tupDef->relpersistence = m_isTmpTable ? DSTORE::SYS_RELPERSISTENCE_GLOBAL_TEMP
                                        : SYS_RELPERSISTENCE_PERMANENT;
  tupDef->relkind = relkind;
  tupDef->relnatts = colNum;
  tupDef->relchecks = 0;
  tupDef->relhasoids = false;
  tupDef->relhaspkey = false;
  tupDef->relhasrules = false;
  tupDef->relhastriggers = false;
  tupDef->relhassubclass = false;
  tupDef->relcmprs = 0;
  tupDef->relhasclusterkey = false;
  tupDef->relrowmovement = false;
  tupDef->parttype = SYS_PARTTYPE_NON_PARTITIONED_RELATION;
  SetSegmemtInfo(tupDef, segmentId, lobSegmentId);
  // todo
  return tupDef;
}

IndexInfo *CdeCreateTableInfo::CdeCreateEmptyIndexInfo(uint32_t indexAttrNum) {
  size_t indexInfoSize = MAXALIGN(sizeof(IndexInfo));
  size_t optionSize =
      MAXALIGN(sizeof(int16_t) * static_cast<uint32_t>(indexAttrNum));
  size_t opcintypeSize =
      MAXALIGN(sizeof(Oid) * static_cast<uint32_t>(indexAttrNum));
  char *index = static_cast<char *>(
      CdeZalloc(indexInfoSize + optionSize + opcintypeSize));
  if (unlikely(index == nullptr)) {
    // ErrLog(DSTORE_ERROR, MODULE_EMBEDDED, ErrMsg("Failed to malloc
    // IndexInfo."));
    return nullptr;
  }
  IndexInfo *indexInfo = static_cast<IndexInfo *>(static_cast<void *>(index));
  indexInfo->opcinType =
      static_cast<Oid *>(static_cast<void *>(index + indexInfoSize));
  indexInfo->indexOption = static_cast<int16_t *>(
      static_cast<void *>(index + indexInfoSize + opcintypeSize));

  /* Currently, expression indexes are not supported. */
  indexInfo->exprCallback = nullptr;

  indexInfo->m_indexSupportProcInfo =
      static_cast<DSTORE::IndexSupportProcInfo *>(
          CdeZalloc(sizeof(DSTORE::IndexSupportProcInfo)));
  indexInfo->m_indexSupportProcInfo->numSupportProc = 0;
  return indexInfo;
}

IndexInfo *CdeCreateTableInfo::CdeCreateIndexInfo(
    uint32_t indexAttrNum, const KEY *key, StorageRelation indexStorRel) {
  IndexInfo *indexInfo = CdeCreateEmptyIndexInfo(indexAttrNum);
  if (indexInfo == nullptr) {
    CDE_LOG_ERROR("CdeCreateIndexInfo fail!");
    return nullptr;
  }
  // not use dstore IndexInfo name, index_info->indexRelName
  indexInfo->relKind = indexStorRel->rel->relkind;
  indexInfo->isUnique = ((key->flags & HA_NOSAME) == 0 ? false : true);
  indexInfo->indexAttrsNum = (indexAttrNum);
  indexInfo->indexKeyAttrsNum = static_cast<uint16_t>(indexAttrNum);
  indexInfo->attributes = indexStorRel->attr;
  for (uint16_t i = 0; i < indexAttrNum; i++) {
    KEY_PART_INFO *keyPart = key->key_part + i;
    indexInfo->indexOption[i] = ((keyPart->key_part_flag & HA_REVERSE_SORT) == 0
                                     ? CDE_INDEX_OPTION_NULLS_FIRST
                                     : CDE_INDEX_OPTION_DESC);
    indexInfo->opcinType[i] = indexStorRel->attr->attrs[i]->atttypid;
  }
  return indexInfo;
}

IndexInfo *CdeCreateTableInfo::CdeCreateIndexInfo(
    const cde_index_def *indexDef, StorageRelation indexStorRel) {
  uint32_t indexAttrNum = indexDef->n_fields;
  IndexInfo *indexInfo = CdeCreateEmptyIndexInfo(indexAttrNum);
  if (indexInfo == nullptr) {
    CDE_LOG_ERROR("CdeZalloc fail!");
    return nullptr;
  }
  // not use dstore IndexInfo name, index_info->indexRelName
  indexInfo->relKind = indexStorRel->rel->relkind;
  indexInfo->isUnique = indexDef->indisunique;
  indexInfo->indexAttrsNum = (indexAttrNum);
  indexInfo->indexKeyAttrsNum = static_cast<uint16_t>(indexAttrNum);
  indexInfo->attributes = indexStorRel->attr;
  for (uint16_t i = 0; i < indexAttrNum; i++) {
    indexInfo->indexOption[i] = indexDef->fields[i].is_ascending
                                    ? CDE_INDEX_OPTION_NULLS_FIRST
                                    : CDE_INDEX_OPTION_DESC;
    indexInfo->opcinType[i] = indexStorRel->attr->attrs[i]->atttypid;
  }
  return indexInfo;
}

StorageRelation CdeCreateTableInfo::CreateIndexStorRel(
    StorageRelation heapStorRel, DSTORE::TupleDesc fullAttr, DictCol *dictCols,
    Oid indexOid, PageId segmentId, const KEY *key, uint32_t *indexCols,
    uint32_t *attrCols, ScanKey *keyInfos, cde_dict_field_t *fields,
    bool isRecovry, const int32_t fillfactor) {
  uint32_t indexAttrNum = (uint32_t)key->user_defined_key_parts;

  SysClassTupDef *classTuple = nullptr;
  DBUG_EXECUTE_IF("cde_create_index_stor_rel_fail", { return nullptr; });
  classTuple = CreateSysRelTuple(SYS_RELKIND_INDEX, indexAttrNum, segmentId,
                                 DSTORE::INVALID_PAGE_ID);
  if (classTuple == nullptr) {
    CDE_LOG_ERROR(
        "CdeCreateTableInfo::CreateSysRelTuple fail! class_tuple is "
        "nullptr");
    return nullptr;
  }

  DEBUG_SYNC(current_thd, "InterruptCreateTable");
  TupleDesc indexTupleDesc = cde_tuple_desc::build_index_tuple_desc(
      indexOid, fullAttr, key, indexCols, attrCols, dictCols);
  if (indexTupleDesc == nullptr) {
    CdeFree(classTuple);
    CDE_LOG_ERROR("build_index_tuple_desc fail!");
    return nullptr;
  }
  classTuple->relnatts = indexAttrNum;
  StorageRelation indexStorRel =
      static_cast<StorageRelation>(CdeZalloc(sizeof(StorageRelationData)));
  SysIndexTupDef *index_tup_def =
      static_cast<SysIndexTupDef *>(CdeAlloc(sizeof(SysIndexTupDef)));
  CDE_ASSERT(indexStorRel != nullptr || index_tup_def != nullptr);  // oom
  CDE_LOG_DEBUG(
      "cde_create_table_info::build_index_rel index '%s' fillfactor is %d",
      key->name, fillfactor);
  RetStatus ret;
  bool forReplay =
      (rds_use_ddl_info_replay && (key->flags & HA_USES_DSTORE_DDL_COMMENT));
  if (!(forReplay || g_guc.enableStandbyRole)) {
    ret = indexStorRel->Construct(DSTORE::g_defaultPdbId, indexOid, classTuple,
                                  indexTupleDesc, fillfactor, m_tableSpaceId,
                                  true);
    CDE_ASSERT(ret == DSTORE_SUCC);
  } else {
    indexStorRel->attr = indexTupleDesc;
    indexStorRel->relOid = indexOid;
  }
  indexStorRel->rel = classTuple;

  indexStorRel->index = CdeCreateIndexInfo(indexAttrNum, key, indexStorRel);

  indexStorRel->indexInfo = index_tup_def;
  indexStorRel->indexInfo->indnatts = indexAttrNum;
  indexStorRel->indexInfo->indexrelid = indexOid;
  indexStorRel->indexInfo->indrelid =
      heapStorRel->relOid;  // association table oid
  indexStorRel->indexInfo->indisunique = (key->flags & HA_NOSAME);
  *keyInfos = ConstructScanKey(indexTupleDesc);
  CDE_ASSERT(*keyInfos != nullptr);
  if (!isRecovry) {
    for (uint8_t i = 0; i < indexAttrNum; i++) {
      fields[i].attId = attrCols[i];
    }
    return indexStorRel;
  }
  IndexBuildInfo buildInfo = CreateIndexBuildInfo(
      heapStorRel, fullAttr, indexAttrNum, attrCols, fields);
  buildInfo.baseInfo = *indexStorRel->index;
  buildInfo.baseInfo.indexRelId = indexStorRel->relOid;
  // todo: build tree for prefix index in dstore
  if (!(forReplay || g_guc.enableStandbyRole)) {
    buildInfo.baseInfo.keyLength = CdeExpandKeyLenByTD() ? key->key_length : 0;
    ret = IndexInterface::Build(indexStorRel, *keyInfos, &buildInfo);
    if (ret == DSTORE_FAIL) {
      CDE_LOG_ERROR(
          "Create index storage relation fail for index: %s of table %s fail "
          "due to build index fail.",
          key->name, key->table->s->table_name.str);
      CdeFree(*keyInfos);
      CdeFree(classTuple);
      CdeFree(indexTupleDesc);
      CdeFree(buildInfo.heapRels);
      indexStorRel->Destroy();
      CdeFree(index_tup_def);
      CdeFree(indexStorRel->index->m_indexSupportProcInfo);
      CdeFree(indexStorRel->index);
      CdeFree(indexStorRel);
      return nullptr;
    }
  }

  CdeFree(buildInfo.heapRels);
  return indexStorRel;
}

int CdeCreateTableInfo::CreateFieldsForIndex(cde_dict_index_t *dictIndex,
                                             const KEY *key, const TABLE *table,
                                             bool isAlterOp) {
  uint32_t indexAttrNum = key->user_defined_key_parts;
  cde_dict_field_t *dictFields = static_cast<cde_dict_field_t *>(
      CdeAlloc(sizeof(cde_dict_field_t) * indexAttrNum));
  for (uint32_t i = 0; i < indexAttrNum; i++) {
    KEY_PART_INFO *keyPart = key->key_part + i;
    uint32_t charset_no = 0;
    uint32_t mbminlen = 0;
    uint32_t mbmaxlen = 0;
    cde_dict_field_t *indexField = dictFields + i;
    indexField->fieldId = isAlterOp ? keyPart->fieldnr : keyPart->fieldnr - 1;
    Field *field = table->field[indexField->fieldId];
    CDE_ASSERT(field != nullptr);
    indexField->m_isVirtual = IsVirtualGeneratedField(field);
    indexField->m_name =
        safe_strdup_root(&dictIndex->m_dictIndexRoot, field->field_name);
    if (field->type() == MYSQL_TYPE_STRING ||
        field->type() == MYSQL_TYPE_VARCHAR) {
      // innobase_get_cset_width
      charset_no = (uint32_t)field->charset()->number;
      /* max Charset Collation number (0x7fff) */
      CDE_ASSERT(charset_no <= 32767);
      CHARSET_INFO *cs;
      cs = all_charsets[charset_no];
      if (cs) {
        mbminlen = cs->mbminlen;
        mbmaxlen = cs->mbmaxlen;
      } else {
        CDE_ASSERT(charset_no == 0);
        mbminlen = mbmaxlen = 0;
      }
      CDE_ASSERT(mbminlen <= mbmaxlen);
      CDE_ASSERT(mbminlen <= CDE_DATA_MBMAX);
      CDE_ASSERT(mbmaxlen <= CDE_DATA_MBMAX);
    }
    // todo: prefix_len max length restriction

    indexField->mbminlen = mbminlen;
    indexField->mbmaxlen = mbmaxlen;
    indexField->len = field->pack_length();

    switch (field->type()) {
      case MYSQL_TYPE_VARCHAR:
        indexField->len -= field->get_length_bytes();
        [[fallthrough]];
      case MYSQL_TYPE_STRING:
        indexField->prefix_len =
            keyPart->length < indexField->len ? keyPart->length : 0;
        break;
      default:
        indexField->prefix_len = 0;
        break;
    }
  }
  dictIndex->fields = dictFields;
  return 0;
}

StorageRelation CdeCreateTableInfo::CreateIndexStorRel(
    StorageRelation heapStorRel, TupleDesc fullAttr, Oid indexOid,
    PageId segmentId, const cde_index_def *indexDef, DictCol *dictCols,
    uint32_t *indexCols, uint32_t *attrCols, ScanKey *keyInfos) {
  uint32_t indexAttrNum = indexDef->n_fields;

  SysClassTupDef *classTuple = nullptr;
  classTuple = CreateSysRelTuple(SYS_RELKIND_INDEX, indexAttrNum, segmentId,
                                 DSTORE::INVALID_PAGE_ID);
  if (classTuple == nullptr) {
    CDE_LOG_ERROR(
        "CdeCreateTableInfo::CreateSysRelTuple fail! class_tuple is "
        "nullptr");
    return nullptr;
  }

  TupleDesc indexTupleDesc = cde_tuple_desc::build_index_tuple_desc(
      indexOid, fullAttr, indexDef->fields, indexDef->n_fields, indexCols,
      attrCols, dictCols);

  DBUG_EXECUTE_IF("dstore_OOM_prepare_inplace_alter", CdeFree(indexTupleDesc);
                  indexTupleDesc = nullptr;);
  if (indexTupleDesc == nullptr) {
    CdeFree(classTuple);
    CDE_LOG_ERROR(
        "cde_tuple_desc::build_index_tuple_desc fail! index_tuple_desc is "
        "nullptr");
    return nullptr;
  }
  classTuple->relnatts = indexAttrNum;
  StorageRelation indexStorRel =
      static_cast<StorageRelation>(CdeZalloc(sizeof(StorageRelationData)));
  SysIndexTupDef *index_tup_def =
      static_cast<SysIndexTupDef *>(CdeAlloc(sizeof(SysIndexTupDef)));
  CDE_ASSERT(indexStorRel != nullptr || index_tup_def != nullptr);
  auto ff =
      indexDef->fillfactor ? indexDef->fillfactor : g_defaultFillfactorForIndex;
  CDE_LOG_DEBUG(
      "cde_create_table_info::build_index_rel index '%s' fillfactor is %d",
      indexDef->name, ff);
  RetStatus ret =
      indexStorRel->Construct(DSTORE::g_defaultPdbId, indexOid, classTuple,
                              indexTupleDesc, ff, m_tableSpaceId, true);
  CDE_ASSERT(ret == DSTORE_SUCC);
  indexStorRel->rel = classTuple;
  indexStorRel->index = CdeCreateIndexInfo(indexDef, indexStorRel);
  indexStorRel->indexInfo = index_tup_def;
  indexStorRel->indexInfo->indnatts = indexAttrNum;
  indexStorRel->indexInfo->indexrelid = indexOid;
  indexStorRel->indexInfo->indrelid =
      heapStorRel->relOid;  // association table oid
  indexStorRel->indexInfo->indisunique = indexDef->indisunique;
  *keyInfos = ConstructScanKey(indexTupleDesc);
  return indexStorRel;
}

cde_dict_index_t *CdeCreateTableInfo::CreateCdeDictIndex(
    StorageRelation heapStorRel, TupleDesc fullAttr, const KEY *key,
    DictCol *dictCols, bool needCreateIndexInfo) {
  DBUG_EXECUTE_IF("create_cde_dict_index_full_tablespace", {
    DSTORE::StorageSetErrorCodeOnly(DSTORE::TBS_ERROR_TABLESPACE_USE_UP);
    return nullptr;
  });
  uint32_t indexAttrNum = (uint32_t)key->user_defined_key_parts;
  Oid indexOid;
  PageId segmentId;

  if (rds_use_ddl_info_replay && m_thd->for_ddl_info_replay &&
      (key->flags & HA_USES_DSTORE_DDL_COMMENT)) {
    indexOid = key->dstore_index_oid;
    segmentId.m_fileId = key->dstore_index_file_id;
    segmentId.m_blockId = key->dstore_index_block_num;
    if (indexOid == DSTORE_INVALID_OID || segmentId.IsInvalid()) {
      CDE_LOG_ERROR("CDE: invalid index OID or PageId for ddl info replay");
      return nullptr;
    }
  } else {
    indexOid =
        SystableInterface::GetNewObjectId(DSTORE::g_defaultPdbId, false, false);

    /* The index_rel and heap_rel use the same tablespace. */
    segmentId = CdeAllocSegmentWrapper::Alloc(
        ChooseIndexSegmentType(m_isTmpTable), m_tableSpaceId);
    if (segmentId == DSTORE::INVALID_PAGE_ID) {
      CDE_LOG_ERROR_WITH_DSTORE_ERROR("Alloc index segment failed.");
      return nullptr;
    }
    CDE_LOG_DEBUG("alloc fileId:%d, blockId:%d, rel_oid:%d for table",
                  segmentId.m_fileId, segmentId.m_blockId, indexOid);
  }

  // get index column vector, cde_dict_index_t->index_cols need, from 0
  uint32_t *indexCols = static_cast<uint32_t *>(
      CdeZalloc(sizeof(uint32_t) * key->user_defined_key_parts));
  CDE_ASSERT(indexCols != nullptr);

  uint32_t *attrCols = static_cast<uint32_t *>(
      CdeZalloc(sizeof(uint32_t) * key->user_defined_key_parts));
  CDE_ASSERT(attrCols != nullptr);

  uint64_t *statNDiffKeyVals = static_cast<uint64_t *>(
      CdeZalloc(sizeof(uint64_t) * key->user_defined_key_parts));
  CDE_ASSERT(statNDiffKeyVals != nullptr);

  uint64_t *statNSampleSize = static_cast<uint64_t *>(
      CdeZalloc(sizeof(uint64_t) * key->user_defined_key_parts));
  CDE_ASSERT(statNSampleSize != nullptr);

  cde_dict_index_t *dictIndex = new (std::nothrow) cde_dict_index_t();
  CDE_ASSERT(dictIndex != nullptr);

  CreateFieldsForIndex(dictIndex, key, m_form);
  ScanKey keyInfos;
  dictIndex->fillfactor = DstoreParseFillfactor(key->comment.str, m_thd);
  auto ff = dictIndex->fillfactor ? dictIndex->fillfactor
                                  : g_defaultFillfactorForIndex;
  StorageRelation indexStorRel = CreateIndexStorRel(
      heapStorRel, fullAttr, dictCols, indexOid, segmentId, key, indexCols,
      attrCols, &keyInfos, dictIndex->fields, needCreateIndexInfo, ff);
  if (indexStorRel == nullptr) {
    CdeFree(indexCols);
    CdeFree(attrCols);
    CdeFree(statNDiffKeyVals);
    CdeFree(statNSampleSize);
    CdeFree(dictIndex->fields);
    dictIndex->fields = nullptr;
    delete dictIndex;
    return nullptr;
  }

  dictIndex->name.assign(key->name);
  dictIndex->key_length = key->key_length;
  dictIndex->oid = indexStorRel->relOid;
  dictIndex->rel = indexStorRel;
  dictIndex->scan_key = keyInfos;
  dictIndex->index_cols = indexCols;
  dictIndex->attr_cols = attrCols;
  dictIndex->stat_n_diff_key_vals = statNDiffKeyVals;
  dictIndex->stat_n_sample_size = statNSampleSize;
  dictIndex->index_col_num = indexAttrNum;
  dictIndex->index_csn = TransactionInterface::GetTransactionSnapshotCsn();
  return dictIndex;
}

int CdeCreateTableInfo::SplitNormalizedName(const char *name, char *table_name,
                                            char *db_name) {
  if (name == nullptr || table_name == nullptr || db_name == nullptr) {
    return CDE_ERROR;
  }
  const char *name_ptr = strend(name) - 1;
  uint table_name_len = 0;
  while (name_ptr >= name && *name_ptr != '\\' && *name_ptr != '/') {
    name_ptr--;
    table_name_len++;
  }

  if (table_name_len > CDE_MAX_TABLE_NAME_LEN) {
    return CDE_ERROR;  // table name limit 64 characters
  }
  int32_t err = memcpy_s(table_name, CDE_MAX_TABLE_NAME_LEN, name_ptr + 1,
                         table_name_len);  // get table name
  if (err != 0) {
    return CDE_ERROR;
  }
  while (name_ptr >= name &&
         (*name_ptr == '\\' ||
          *name_ptr == '/')) {  // skip any number of path separators
    name_ptr--;
  }

  CDE_ASSERT(name_ptr + 1 >= name);

  uint db_name_len = 0;
  while (name_ptr >= name && *name_ptr != '\\' && *name_ptr != '/') {
    name_ptr--;
    db_name_len++;
  }
  if (db_name_len > CDE_MAX_DATABASE_NAME_LEN) {
    return CDE_ERROR;  // db name limit 64 characters
  }
  err = memcpy_s(db_name, CDE_MAX_DATABASE_NAME_LEN, name_ptr + 1,
                 db_name_len);  // get db name
  return err != 0 ? CDE_ERROR : CDE_OK;
}

int CdeCreateTableInfo::NormalizeTableName(char *norm_name, const char *name) {
  char table_name[CDE_MAX_TABLE_NAME_LEN + 1] = {0};
  char db_name[CDE_MAX_DATABASE_NAME_LEN + 1] = {0};
  if (CdeCreateTableInfo::SplitNormalizedName(name, table_name, db_name) != 0) {
    return CDE_ERROR;
  }

  int db_len = strlen(db_name);
  int table_len = strlen(table_name);
  if ((db_len + table_len + 1) >= (FN_REFLEN - 1)) {
    CDE_LOG_SYSTEM("name is too long, name = %s", name);
    return HA_ERR_TOO_LONG_PATH;
  }
  memcpy_s(norm_name, CDE_MAX_TABLE_NAME_LEN, db_name, db_len);
  norm_name[db_len] = '/';
  /* Copy the name and null-byte. */
  memcpy_s(norm_name + db_len + 1, CDE_MAX_DATABASE_NAME_LEN, table_name,
           table_len + 1);
  if (lower_case_file_system != 0) {
    my_casedn_str(system_charset_info, norm_name);
  }
  return CDE_OK;
}

DSTORE::StorageRelation CdeCreateTableInfo::CreateHeapStorRel(
    DSTORE::Oid tableOid, DSTORE::PageId segmentId, DSTORE::PageId lobSegmentId,
    const TABLE *table, const int32_t fillfactor, const dd::Table *ddTable,
    DictCol *dictCols, bool forReplay, DSTORE::TupleDesc *attrFull) {
  TupleDesc attr = nullptr;
  if (ddTable == nullptr) {
    attr = cde_tuple_desc::build_heap_tuple_desc(tableOid, table, true);
    if (nullptr != attrFull)
      *attrFull = cde_tuple_desc::build_heap_tuple_desc(tableOid, table);
  } else {
    attr = cde_tuple_desc::BuildHeapTupleDescFromDD(tableOid, ddTable, dictCols,
                                                    true);
    if (nullptr != attrFull)
      *attrFull =
          cde_tuple_desc::BuildHeapTupleDescFromDD(tableOid, ddTable, dictCols);
  }

  if (attr == nullptr) {
    CDE_LOG_ERROR(
        "cde_tuple_desc::build_heap_tuple_desc fail! attr is nullptr");
    return nullptr;
  }

  if (nullptr != attrFull && nullptr == *attrFull) {
    CDE_LOG_ERROR(
        "cde_tuple_desc::build_heap_tuple_desc fail! attrFull is nullptr");
    return nullptr;
  }

  SysClassTupDef *classTuple = CreateSysRelTuple(
      SYS_RELKIND_RELATION, attr->natts, segmentId, lobSegmentId);
  if (classTuple == nullptr) {
    CDE_LOG_ERROR("CreateSysRelTuple fail! class_tuple is nullptr");
    return nullptr;
  }

  if (nullptr != attrFull) (*attrFull)->tdhaslob = lobSegmentId.IsValid();
  attr->tdhaslob = lobSegmentId.IsValid();

  StorageRelation storRel =
      static_cast<StorageRelation>(CdeZalloc(sizeof(StorageRelationData)));
  CDE_LOG_DEBUG(
      "cde_create_table_info::build_table_rel table '%s' fillfactor is %d",
      table->s->table_name.str, fillfactor);
  /* When replaying DDL and executing SHOW commands on a physical standby,
  the table open operation follows the same code path.
  For example, there is no need to create in-memory dstore objects,
  and skipping querying the auto-increment value. */
  if (!(forReplay || g_guc.enableStandbyRole)) {
    RetStatus ret =
        storRel->Construct(DSTORE::g_defaultPdbId, tableOid, classTuple, attr,
                           fillfactor, m_tableSpaceId, true);
    CDE_ASSERT(ret == DSTORE_SUCC);
  } else {
    storRel->attr = attr;
    storRel->relOid = tableOid;
  }

  storRel->rel = classTuple;
  storRel->indexInfo = nullptr;
  storRel->index = nullptr;
  storRel->indKey = nullptr;
  return storRel;
}

void CdeCreateTableInfo::InitializeAutoinc() {
  const bool persist = !(m_create_info->options & HA_LEX_CREATE_TMP_TABLE) &&
                       m_form->found_next_number_field;
  if (!persist && m_create_info->auto_increment_value == 0) {
    return;
  }
  CDE_ASSERT(m_dict != nullptr);
  enum_sql_command cmd = static_cast<enum_sql_command>(thd_sql_command(m_thd));
  if (m_create_info->auto_increment_value > 0 &&
      ((m_create_info->used_fields & HA_CREATE_USED_AUTO) ||
       cmd == SQLCOM_ALTER_TABLE || cmd == SQLCOM_OPTIMIZE ||
       cmd == SQLCOM_CREATE_INDEX)) {
    m_dict->autoinc_dict.SetAutoinc(m_create_info->auto_increment_value);
    CdeDdTableSetSePrivateDataAutoInc(m_dd_tab,
                                      m_create_info->auto_increment_value);
    m_dict->autoinc_dict.SetInitialized(true);
  }
}

/* parse the dstore_ddl_comment string, get the oid and segment_id */
static bool CdeExtractDstoreIdsFromDDLInfo(char *ddl_comment_str, bool hasLob,
                                           Oid &heapOid, PageId &segmentId,
                                           PageId &lobSegmentId) {
  dd::String_type tab_props_str(ddl_comment_str);
  std::unique_ptr<dd::Properties> tab_props(
      dd::Properties::parse_properties(tab_props_str));
  uint16_t fileId = 0;
  uint32_t blkNum = 0;

  if (tab_props->exists(object_tablerelid) &&
      tab_props->exists(object_relfileid) &&
      tab_props->exists(object_relblknum)) {
    tab_props->get(object_tablerelid, &heapOid);
    tab_props->get(object_relfileid, &fileId);
    tab_props->get(object_relblknum, &blkNum);

    segmentId.m_fileId = static_cast<DSTORE::FileId>(fileId);
    segmentId.m_blockId = static_cast<DSTORE::BlockNumber>(blkNum);
    if (heapOid == DSTORE_INVALID_OID || segmentId.IsInvalid()) {
      CDE_LOG_ERROR("CDE: invalid OID or PageId for ddl info replay");
      return true;
    }
  }
  if (hasLob) {
    uint16_t lobFileId = 0;
    uint32_t lobBlkNum = 0;
    tab_props->get(object_rellobfileid, &lobFileId);
    tab_props->get(object_rellobblknum, &lobBlkNum);
    lobSegmentId.m_fileId = static_cast<DSTORE::FileId>(lobFileId);
    lobSegmentId.m_blockId = static_cast<DSTORE::BlockNumber>(lobBlkNum);
    if (lobSegmentId.IsInvalid()) {
      CDE_LOG_ERROR("CDE: invalid lob PageId for ddl info replay");
      return true;
    }
  }
  return false;
}

int CdeCreateTableInfo::CreateTable() {
  int ret = CDE_OK;

  const size_t full_name_len = strlen(m_tableName);
  if (full_name_len > CDE_MAX_FULL_NAME_LEN ||
      m_tableName[full_name_len - 1] == '/') {
    CDE_LOG_ERROR("CDE: Table Name Not Support! name = %s", m_tableName);
    return HA_ERR_WRONG_TABLE_NAME;
  }

  if (CheckFkBaseCol(m_dd_tab, m_form)) {
    CDE_LOG_WARN(
        "Foreign key constraint on the base column of a stored generated "
        "column cannot use CASCADE, SET NULL, or SET DEFAULT as ON UPDATE "
        "or ON DELETE referential actions, table name '%s'.",
        m_tableName);
    return HA_ERR_CANNOT_ADD_FOREIGN;
  }

  std::unique_ptr<cde_dict_t> dict{new (std::nothrow) cde_dict_t()};
  if (dict == nullptr) {
    return HA_ERR_GENERIC;
  }
  DSTORE::Oid heapOid = DSTORE_INVALID_OID;
  DSTORE::PageId segmentId = DSTORE::INVALID_PAGE_ID;
  DSTORE::PageId lobSegmentId = DSTORE::INVALID_PAGE_ID;
  bool hasLob = IsContainLobCol(m_form);
  bool forReplay =
      (rds_use_ddl_info_replay && m_thd->for_ddl_info_replay &&
       (m_create_info->used_fields & HA_CREATE_USED_DSTORE_DDL_COMMENT));

  dict->m_spaceId = m_tableSpaceId;

  if (forReplay) {
    if (CdeExtractDstoreIdsFromDDLInfo(m_create_info->dstore_ddl_comment.str,
                                       hasLob, heapOid, segmentId,
                                       lobSegmentId)) {
      return HA_ERR_GENERIC;
    }
    /* to make the dict table reload before use it. Because in replay process,
    we do not interact with dstore so that the table storage relation is not
    built. reopen dict table can make the relation ready */
    DiscardAfterDDL(dict.get(), m_form);
  } else {
    heapOid =
        SystableInterface::GetNewObjectId(DSTORE::g_defaultPdbId, false, false);
    segmentId = CdeAllocSegmentWrapper::Alloc(
        ChooseHeapSegmentType(m_isTmpTable), m_tableSpaceId);
    DBUG_EXECUTE_IF("create_table_alloc_failed_cause_full_tablespace", {
      segmentId = DSTORE::INVALID_PAGE_ID;
      DSTORE::StorageSetErrorCodeOnly(DSTORE::TBS_ERROR_TABLESPACE_USE_UP);
    });
    if (segmentId == DSTORE::INVALID_PAGE_ID) {
      CDE_LOG_ERROR_WITH_DSTORE_ERROR("Alloc heap segment failed.");
      return GetAndConvertDstoreErrcodeToMysql();
    }
    CDE_LOG_DEBUG("alloc fileId:%d, blockId:%d, rel_oid:%d for table:%s",
                  segmentId.m_fileId, segmentId.m_blockId, heapOid,
                  m_tableName);
    if (hasLob) {
      lobSegmentId = CdeAllocSegmentWrapper::Alloc(
          ChooseHeapSegmentType(m_isTmpTable), m_tableSpaceId);
      DBUG_EXECUTE_IF("create_table_alloc_blob_failed_cause_full_tablespace", {
        lobSegmentId = DSTORE::INVALID_PAGE_ID;
        DSTORE::StorageSetErrorCodeOnly(DSTORE::TBS_ERROR_TABLESPACE_USE_UP);
      });
      if (lobSegmentId == DSTORE::INVALID_PAGE_ID) {
        CDE_LOG_ERROR_WITH_DSTORE_ERROR("Alloc lob segment failed.");
        return GetAndConvertDstoreErrcodeToMysql();
      }
      CDE_LOG_DEBUG(
          "alloc blob fileId:%d, blob blockId:%d, rel_oid:%d for table:%s",
          lobSegmentId.m_fileId, lobSegmentId.m_blockId, heapOid, m_tableName);
    }
  }

  dict->fillfactor = DstoreParseFillfactor(m_form->s->comment.str, m_thd);
  auto ff = dict->fillfactor ? dict->fillfactor : g_defaultFillfactor;

  NumberOfFields numberOfFields =
      FillFieldLenFromDD(m_dd_tab, m_form, dict->m_fieldLenInfos);
  dict->m_totalColCount =
      numberOfFields.nonVirtualFieldsNum + numberOfFields.virtualFieldsNum;
  dict->m_vColCount = numberOfFields.virtualFieldsNum;
  dict->m_dictCols = nullptr;

  /* Version should always be zero when create table. */
  dict->m_currentRowVersion = 0;

  uint32_t version = 0;
  dict->m_dictCols = BuildDictColsFromDD(m_dd_tab, version, dict->m_maxPos);
  CDE_ASSERT(version == 0);
  for (uint i = 0; i < m_form->s->fields; i++) {
    Field *mysql_field = m_form->field[i];
    std::string col_name(mysql_field->field_name);
    dict->col_names.push_back(col_name);
  }
  if (dict->m_vColCount > 0) {
    dict->m_vCols = BuildVCols(dict.get(), m_form, m_dd_tab);
  }

  TupleDesc attrFull = nullptr;
  m_heapStorRel = CreateHeapStorRel(
      heapOid, segmentId, lobSegmentId, m_form, ff, m_dd_tab, dict->m_dictCols,
      forReplay, (dict->m_vColCount > 0) ? &attrFull : nullptr);

  if (m_heapStorRel == nullptr) {
    CDE_LOG_ERROR("build_table_rel fail! m_heapStorRel is nullptr");
    return GetAndConvertDstoreErrcodeToMysql();
  }
  CDE_ASSERT_DEBUG(0 == dict->m_vColCount || nullptr != attrFull);

  /* In case there are no virtual columns attrFull is just storRel->attr*/
  dict->attrFull = (dict->m_vColCount > 0) ? attrFull : m_heapStorRel->attr;
  CDE_ASSERT_DEBUG(nullptr != dict->attrFull);

  dict->dstore_relation = m_heapStorRel;
  dict->name.assign(m_tableName);
  for (uint i = 0; i < m_form->s->fields; i++) {
    Field *mysql_field = m_form->field[i];
    std::string col_name(mysql_field->field_name);
    dict->col_names.push_back(col_name);
  }
  dict->m_id = CdeFormDictTableId(m_heapStorRel->rel->reltablespace,
                                  m_heapStorRel->relOid);

  for (uint i = 0; i < m_form->s->keys; i++) {
    const KEY *my_key = m_form->key_info + i;
    cde_dict_index_t *dictIndex = CreateCdeDictIndex(
        m_heapStorRel, dict->attrFull, my_key, dict->m_dictCols);
    if (dictIndex == nullptr) {
      dict->destroy_from_cache();
      if (CheckIsInterrupted()) {
        CDE_LOG_INFO(
            "build index %s failed for table %s, cause sql request canceled",
            my_key->name, m_name);
        return HA_ERR_QUERY_INTERRUPTED;
      }
      return GetAndConvertDstoreErrcodeToMysql();
    }
    dictIndex->table = dict.get();
    dict->add_index_dict(dictIndex);
  }
  if (m_isTmpTable) {
    dict->flags |= DICT_TABLE_TEMPORARY;
  }
  dict->m_rebuild_csn = TransactionInterface::GetTransactionSnapshotCsn();
  /** now CdeCreateTableInfo dtor is reponsible for releasing this table */
  m_dict = dict.get();
  InitializeAutoinc();
  cde_dict_t *dictPtr = dict.get();
  dict.release();
  cde_dict_t *existPtr = nullptr;
  DictSysAddTable(m_tableName, dictPtr, &existPtr, !m_isTmpTable);
  /* Under the protection of MDL-exclusive lock, no */
  CDE_ASSERT(existPtr == nullptr);
  if (m_isTmpTable) {
    return ret;
  }

  if (CdeDdlLog::GetInstance()->LogRemoveCache(m_thd, m_tableName) ==
      CDE_FAIL) {
    /** release the table before removing */
    m_dict->release();
    m_dict = nullptr;
    DictSysRemoveTable(m_tableName);
    CDE_LOG_ERROR("Create table:%s fail due to LogRemoveCache fail.",
                  m_tableName);
    return HA_ERR_GENERIC;
  }

  std::deque<const char *> fk_list_names;
  ret |= CdeDdTableLoadForeignKey(m_thd, m_tableName, dictPtr, m_dd_tab,
                                  fk_list_names);

  if (CDE_OK == ret && !fk_list_names.empty())
    CdeDdOpenForeignKeyTables(fk_list_names, m_thd);

  return ret;
}

static void set_table_flags_from_create_info(
    cde_dict_t *table, const HA_CREATE_INFO *create_info) {
  if (table->is_temporary()) {
    table->set_stats_persistent(false, true);
  } else {
    table->set_stats_persistent(
        create_info->table_options & HA_OPTION_STATS_PERSISTENT,
        create_info->table_options & HA_OPTION_NO_STATS_PERSISTENT);
  }

  table->set_stats_auto_recalc(
      create_info->table_options & HA_STATS_AUTO_RECALC_ON,
      create_info->table_options & HA_STATS_AUTO_RECALC_OFF);

  table->stat_sample_pages = create_info->stats_sample_pages;
}

template <typename Table>
int CdeCreateTableInfo::CreateTableUpdateDict(Table *ddTable) {
  if (!m_isTmpTable) {
    UpdateDataDictionaryInfo(ddTable, m_heapStorRel, m_dict);
  }
  set_table_flags_from_create_info(m_dict, m_create_info);

  int ret = CdeDictStatsReset(m_dict);

  if (m_thd != nullptr && !CdeIsDdlLogTableName(m_name) &&
      !CdeIsMysqlTmpTableName(m_name)) {
    (void)DstoreDictStatsSave(m_dict, m_thd);
  }

  (void)DstoreDictTableStatsDeinit(m_dict);

  return ret;
}

template int CdeCreateTableInfo::CreateTableUpdateDict<dd::Table>(dd::Table *);

template int CdeCreateTableInfo::CreateTableUpdateDict<dd::Partition>(
    dd::Partition *);

bool CdeTruncate::ReconstructHeapRelation(cde_dict_t *dictTable) {
  Oid rel_oid = dictTable->dstore_relation->relOid;
  StorageRelation heapRelation = dictTable->dstore_relation;
  SysClassTupDef *tupDef = heapRelation->rel;
  TupleDesc attr = dictTable->dstore_relation->attr;
  DSTORE::PageId oldSegmentId = {tupDef->relfileid, tupDef->relblknum};
  DSTORE::PageId oldLobSegmentId = {tupDef->rellobfileid, tupDef->rellobblknum};
  DSTORE::PageId lobSegmentId = DSTORE::INVALID_PAGE_ID;
  DSTORE::TablespaceId tableSpaceId = dictTable->m_spaceId;
  DSTORE::SegmentType segmentType =
      ChooseHeapSegmentType(dictTable->is_temporary());
  /* Alloc for a new segment. */
  DSTORE::PageId segmentId =
      CdeAllocSegmentWrapper::Alloc(segmentType, tableSpaceId);
  DBUG_EXECUTE_IF("recon_heap_AllocSeg_injection1",
                  { segmentId = DSTORE::INVALID_PAGE_ID; });
  if (unlikely(segmentId == DSTORE::INVALID_PAGE_ID)) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR("Reconstruct heap segment failed.");
    return CDE_FAIL;
  }
  DBUG_EXECUTE_IF("recon_heap_AllocSeg_injection2", {
    oldLobSegmentId.m_fileId = DSTORE::MAX_VFS_FILE_ID;
    oldLobSegmentId.m_blockId = DSTORE::DSTORE_MAX_BLOCK_NUMBER;
  });
  if (oldLobSegmentId.IsValid()) {
    lobSegmentId = CdeAllocSegmentWrapper::Alloc(segmentType, tableSpaceId);
    DBUG_EXECUTE_IF("recon_heap_AllocSeg_injection2",
                    { lobSegmentId = DSTORE::INVALID_PAGE_ID; });
    if (unlikely(lobSegmentId == DSTORE::INVALID_PAGE_ID)) {
      CDE_LOG_ERROR_WITH_DSTORE_ERROR("Reconstruct lob segment failed.");
      return CDE_FAIL;
    }
  }
  /* update heapRelation SysClassTupDef */
  SetSegmemtInfo(tupDef, segmentId, lobSegmentId);

  /* Drop old segment_ids by post_ddl. */
  if (!dictTable->is_temporary()) {
    CDE_ASSERT(segmentType == SegmentType::HEAP_SEGMENT_TYPE);
    bool ret = CdeDdlLog::GetInstance()->LogDropSegment(
        current_thd, oldSegmentId, segmentType, tableSpaceId, false);
    if (ret == CDE_SUCC && oldLobSegmentId.IsValid()) {
      ret = CdeDdlLog::GetInstance()->LogDropSegment(
          current_thd, oldLobSegmentId, segmentType, tableSpaceId, false);
    }
    if (ret != CDE_SUCC) {
      CDE_LOG_ERROR(
          "Reconstruct heap of table:%s fail due to LogDropSegment fail.",
          dictTable->name.c_str());
      return CDE_FAIL;
    }
  }

  auto fillfactor = IsValidFillfactor(dictTable->fillfactor)
                        ? dictTable->fillfactor
                        : g_defaultFillfactor;
  CDE_LOG_DEBUG("Reconstruct heap relation of table '%s' fillfactor is %d",
                dictTable->name.c_str(), fillfactor);
  /* Reconstruct StorageRelation of heap. */
  heapRelation->Destroy();  // need destroy relation to free previous tablesmgr
  RetStatus retStatus =
      heapRelation->Construct(DSTORE::g_defaultPdbId, rel_oid, tupDef, attr,
                              fillfactor, tableSpaceId, true);
  CDE_ASSERT(retStatus == DSTORE_SUCC);
  heapRelation->rel = tupDef;
  heapRelation->indexInfo = nullptr;
  heapRelation->index = nullptr;
  heapRelation->indKey = nullptr;
  return CDE_SUCC;
}

bool CdeTruncate::ReconstructIndexRelation(cde_dict_t *dictTable) {
  DSTORE::TablespaceId tableSpaceId = dictTable->m_spaceId;
  DSTORE::SegmentType segmentType =
      ChooseIndexSegmentType(dictTable->is_temporary());
  for (auto eachIndex : dictTable->index_dict_vec) {
    if (!eachIndex->IsCommitted()) {
      continue;
    }

    Oid indexOid = eachIndex->rel->relOid;
    StorageRelation indexRelation = eachIndex->rel;
    SysClassTupDef *tupDef = indexRelation->rel;
    TupleDesc attr = indexRelation->attr;
    PageId oldSegmentId = {tupDef->relfileid, tupDef->relblknum};

    /* Obtains the segment ID of the index table. */
    PageId segmentId = CdeAllocSegmentWrapper::Alloc(segmentType, tableSpaceId);
    DBUG_EXECUTE_IF("recon_idx_AllocSeg_injection", {
      segmentId = DSTORE::INVALID_PAGE_ID;
      DSTORE::StorageSetErrorCodeOnly(DSTORE::TBS_ERROR_TABLESPACE_USE_UP);
    });
    if (segmentId == DSTORE::INVALID_PAGE_ID) {
      CDE_LOG_ERROR_WITH_DSTORE_ERROR(
          "Alloc semgent for index truncate failed.");
      return CDE_FAIL;
    }

    /* Update the data dictionary table. */
    SetSegmemtInfo(tupDef, segmentId, DSTORE::INVALID_PAGE_ID);
    /* Drop old segments by post_ddl. */
    if (!dictTable->is_temporary()) {
      CDE_ASSERT(segmentType == SegmentType::INDEX_SEGMENT_TYPE);
      bool retVal = CdeDdlLog::GetInstance()->LogDropSegment(
          current_thd, oldSegmentId, segmentType, tableSpaceId, false);
      if (retVal != CDE_SUCC) {
        CDE_LOG_ERROR(
            "Reconstruct index:%s of table:%s fail due to LogDropSegment fail.",
            eachIndex->name.c_str(), dictTable->name.c_str());
        return CDE_FAIL;
      }
    }

    DEBUG_SYNC(current_thd, "InterruptTruncateTable");
    auto fillfactor = IsValidFillfactor(eachIndex->fillfactor)
                          ? eachIndex->fillfactor
                          : g_defaultFillfactorForIndex;
    CDE_LOG_DEBUG("Reconstruct index relation of index '%s' fillfactor is %d",
                  eachIndex->name.c_str(), fillfactor);
    /* Reconstruct StorageRelation of each index */
    // need destroy relation to free previous btreesmgr
    indexRelation->Destroy();
    RetStatus ret =
        indexRelation->Construct(DSTORE::g_defaultPdbId, indexOid, tupDef, attr,
                                 fillfactor, tableSpaceId, true);
    indexRelation->rel = tupDef;

    /* build index btreeSmg */
    IndexBuildInfo build_info = CreateIndexBuildInfo(
        dictTable->dstore_relation, dictTable->attrFull,
        eachIndex->index_col_num, eachIndex->attr_cols, eachIndex->fields);

    build_info.baseInfo = *indexRelation->index;
    build_info.baseInfo.keyLength =
        CdeExpandKeyLenByTD() ? eachIndex->key_length : 0;
    build_info.baseInfo.indexRelId = indexRelation->relOid;
    ret =
        IndexInterface::Build(indexRelation, eachIndex->scan_key, &build_info);
    if (ret == DSTORE_FAIL) {
      CDE_LOG_ERROR(
          "Reconstruct index:%s of table:%s fail due to build index fail.",
          eachIndex->name.c_str(), dictTable->name.c_str());
      CdeFree(build_info.heapRels);
      return CDE_FAIL;
    }

    CdeFree(build_info.heapRels);
  }
  return CDE_SUCC;
}

template <typename Table>
bool CdeTruncate::DoTruncate(Table *ddTable, cde_dict_t *dictTable) {
  dictTable->WaitIfHoldByBgThread();
  CDE_ASSERT_DEBUG(dictTable->getRefCnt() == 1);
  if (ReconstructHeapRelation(dictTable) == CDE_FAIL) {
    return CDE_FAIL;
  }

  /* Index Storage Relation vector*/
  if (ReconstructIndexRelation(dictTable) == CDE_FAIL) {
    return CDE_FAIL;
  }

  DSTORE::CommitSeqNo csn = TransactionInterface::GetTransactionSnapshotCsn();
  dictTable->m_rebuild_csn = csn;
  for (cde_dict_index_t *index : dictTable->index_dict_vec) {
    if (!index->IsCommitted()) {
      continue;
    }
    index->index_csn = csn;
  }

  /* Truncate table can happen without exisiting m_dstore. Server will close
  all the handlers to a table before truncating - thus it is safe to use
  rd_storage_relation directly here. When we implement global relation cache
  we will take storage relation from that cache anyways. */
  if (!dictTable->is_temporary()) {
    UpdateDataDictionaryInfo(ddTable, dictTable->dstore_relation, dictTable);
  }
  dictTable->UnMarkBgStatQuit();

  (void)CdeDictTableStatsUpdate(dictTable, EMPTY_TABLE, current_thd);

  /* In protection of MDL_EXCLUSIVE of the table, we don't
  need acquire the lock of autoinc. */
  dictTable->autoinc_dict.SetAutoinc(1);
  return CDE_SUCC;
}

DSTORE::PageId CdeAllocSegmentWrapper::Alloc(
    DSTORE::SegmentType type, DSTORE::TablespaceId tableSpaceId) {
  DSTORE::PageId segmentId = TableSpace_Interface::AllocSegment(
      DSTORE::g_defaultPdbId, tableSpaceId, type);
  if (unlikely(segmentId == DSTORE::INVALID_PAGE_ID) ||
      (tableSpaceId == CDE_TMP_TABLE_SPACE_ID)) {
    return segmentId;
  }

  bool writeLogFail = false;
  bool ret = CdeDdlLog::GetInstance()->LogDropSegment(
      current_thd, segmentId, type, tableSpaceId, true, &writeLogFail);
  if (ret == CDE_FAIL) {
    CDE_LOG_ERROR("Log drop segment fail, id:[%hu,%u] type:%hhu.",
                  segmentId.m_fileId, segmentId.m_blockId,
                  static_cast<uint8_t>(type));
    if (writeLogFail == true) {
      CDE_LOG_WARN(
          "Drop the newly allocated segment due to write log fail, id:[%hu,%u] "
          "type:%hhu.",
          segmentId.m_fileId, segmentId.m_blockId, static_cast<uint8_t>(type));
      CdeAllocSegmentWrapper::Drop(segmentId, type, tableSpaceId);
    }
    return DSTORE::INVALID_PAGE_ID;
  }
  return segmentId;
}

template bool CdeTruncate::DoTruncate<dd::Table>(dd::Table *ddTable,
                                                 cde_dict_t *dictTable);
template bool CdeTruncate::DoTruncate<dd::Partition>(dd::Partition *ddTable,
                                                     cde_dict_t *dictTable);

void CdeAllocSegmentWrapper::Drop(const DSTORE::PageId &segmentId,
                                  DSTORE::SegmentType type,
                                  DSTORE::TablespaceId tableSpaceId) {
  DSTORE::RetStatus ret = TableSpace_Interface::DropSegment(
      DSTORE::g_defaultPdbId, tableSpaceId, type, segmentId);
  if (ret == DSTORE::DSTORE_FAIL) {
    CDE_LOG_WARN(
        "Drop segment fail and it may cause leak, id:[%hu,%u] type:%hhu.",
        segmentId.m_fileId, segmentId.m_blockId, static_cast<uint8_t>(type));
  }
}

bool IsContainLobCol(const TABLE *table) {
  bool isLob = false;
  if (table == nullptr || table->s == nullptr) {
    return isLob;
  }
  for (uint i = 0; i < table->s->fields; i++) {
    Field *mysqlField = table->field[i];
    if (is_blob(mysqlField->type()) || mysqlField->type() == MYSQL_TYPE_JSON) {
      isLob = true;
      break;
    }
  }
  return isLob;
}

bool CheckIsInterrupted() {
  DSTORE::ThreadContextInterface *thrd =
      DSTORE::ThreadContextInterface::GetCurrentThreadContext();
  if (!thrd) {
    return true;
  }

  DSTORE::ThreadContext *threadContext =
      dynamic_cast<DSTORE::ThreadContext *>(thrd);

  if (threadContext->GetErrorCode() ==
      DSTORE::SQL_WARNING_REQUEST_ARE_CANCELED) {
    return true;
  }

  return false;
}

cde_dict_t *CdeCreateTableInfo::CdeCreateInplaceRebuildTable(
    THD *thd, const char *name, const TABLE *form, dd::Table *tableDef) {
  if (CheckFkBaseCol(tableDef, form)) {
    CDE_LOG_WARN(
        "Foreign key constraint on the base column of a stored generated "
        "column cannot use CASCADE, SET NULL, or SET DEFAULT as ON UPDATE "
        "or ON DELETE referential actions, table name '%s'.",
        name);
    return nullptr;
  }

  std::unique_ptr<cde_dict_t> dict{new (std::nothrow) cde_dict_t()};
  if (dict == nullptr) {
    return nullptr;
  }

  if (m_create_info->tablespace) {
    if (strcmp(m_create_info->tablespace, dstore_global_spacename) == 0) {
      my_error(ER_RESERVED_TABLESPACE_NAME, MYF(0), m_name,
               dstore_global_spacename);
      return nullptr;
    } else if (strcmp(m_create_info->tablespace, dstore_temp_spacename) == 0) {
      my_printf_error(ER_ILLEGAL_HA_CREATE_OPTION,
                      "DStore: Tablespace `%s` can only contain"
                      " TEMPORARY tables.",
                      MYF(0), m_create_info->tablespace);
      return nullptr;
    }
  }

  dict->m_spaceId = CdeGetSpaceIdByName(m_create_info->tablespace);
  if (dict->m_spaceId == DSTORE::INVALID_TABLESPACE_ID) {
    my_printf_error(ER_TABLESPACE_MISSING,
                    "DStore: A general tablespace named"
                    " `%s` cannot be found.",
                    MYF(0), m_create_info->tablespace);
    return nullptr;
  }

  m_tableSpaceId = dict->m_spaceId;

  DSTORE::Oid heapOid = DSTORE_INVALID_OID;
  DSTORE::PageId segmentId = DSTORE::INVALID_PAGE_ID;
  DSTORE::PageId lobSegmentId = DSTORE::INVALID_PAGE_ID;
  bool hasLob = IsContainLobCol(form);
  heapOid =
      SystableInterface::GetNewObjectId(DSTORE::g_defaultPdbId, false, false);
  segmentId = CdeAllocSegmentWrapper::Alloc(ChooseHeapSegmentType(false),
                                            dict->m_spaceId);
  if (segmentId == DSTORE::INVALID_PAGE_ID) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR("Alloc heap segment failed.");
    return nullptr;
  }
  CDE_LOG_DEBUG("alloc fileId:%d, blockId:%d, rel_oid:%d for table:%s",
                segmentId.m_fileId, segmentId.m_blockId, heapOid, name);
  if (hasLob) {
    lobSegmentId = CdeAllocSegmentWrapper::Alloc(ChooseHeapSegmentType(false),
                                                 dict->m_spaceId);
    if (lobSegmentId == DSTORE::INVALID_PAGE_ID) {
      CDE_LOG_ERROR_WITH_DSTORE_ERROR("Alloc lob segment failed.");
      return nullptr;
    }
    CDE_LOG_DEBUG(
        "alloc blob fileId:%d, blob blockId:%d, rel_oid:%d for table:%s",
        lobSegmentId.m_fileId, lobSegmentId.m_blockId, heapOid, name);
  }

  dict->fillfactor = DstoreParseFillfactor(form->s->comment.str, thd);
  auto ff = dict->fillfactor ? dict->fillfactor : g_defaultFillfactor;

  NumberOfFields numberOfFields =
      FillFieldLenFromDD(tableDef, form, dict->m_fieldLenInfos);
  dict->m_totalColCount =
      numberOfFields.nonVirtualFieldsNum + numberOfFields.virtualFieldsNum;
  dict->m_vColCount = numberOfFields.virtualFieldsNum;
  dict->m_dictCols = nullptr;

  dict->m_currentRowVersion = 0;
  uint32_t version = 0;
  dict->m_dictCols = BuildDictColsFromInplaceCreateDD(tableDef, dict->m_maxPos);
  CDE_ASSERT(version == 0);
  for (uint i = 0; i < form->s->fields; i++) {
    Field *mysql_field = form->field[i];
    std::string col_name(mysql_field->field_name);
    dict->col_names.push_back(col_name);
  }
  if (dict->m_vColCount > 0) {
    dict->m_vCols = BuildVCols(dict.get(), form, tableDef);
  }

  TupleDesc attrFull = nullptr;
  DSTORE::StorageRelation heapStorRel = CreateHeapStorRel(
      heapOid, segmentId, lobSegmentId, form, ff, tableDef, dict->m_dictCols,
      false, (dict->m_vColCount > 0) ? &attrFull : nullptr);

  if (heapStorRel == nullptr) {
    CDE_LOG_ERROR("build_table_rel fail! m_heapStorRel is nullptr");
    return nullptr;
  }
  CDE_ASSERT_DEBUG(0 == dict->m_vColCount || nullptr != attrFull);

  /* In case there are no virtual columns attrFull is just storRel->attr*/
  dict->attrFull = (dict->m_vColCount > 0) ? attrFull : heapStorRel->attr;
  CDE_ASSERT_DEBUG(nullptr != dict->attrFull);

  dict->dstore_relation = heapStorRel;
  dict->name.assign(name);
  dict->m_id =
      CdeFormDictTableId(heapStorRel->rel->reltablespace, heapStorRel->relOid);

  for (uint i = 0; i < m_form->s->keys; i++) {
    const KEY *my_key = m_form->key_info + i;
    cde_dict_index_t *dictIndex = CreateCdeDictIndex(
        heapStorRel, dict->attrFull, my_key, dict->m_dictCols, false);
    if (dictIndex == nullptr) {
      dict->destroy_from_cache();
      if (CheckIsInterrupted()) {
        CDE_LOG_INFO(
            "build index %s failed for table %s, cause sql request canceled",
            my_key->name, m_name);
        return nullptr;
      }
      return nullptr;
    }
    dictIndex->table = dict.get();
    dict->add_index_dict(dictIndex);
  }
  dict->m_rebuild_csn = TransactionInterface::GetTransactionSnapshotCsn();
  cde_dict_t *dictPtr = dict.get();
  dict.release();
  return dictPtr;
}
} /* namespace CDE */
