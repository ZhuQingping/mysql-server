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

#ifndef HA_CDE_ALTER_H
#define HA_CDE_ALTER_H

#include "common/cde_alloc.h"
#include "common/cde_srv.h"
#include "ddl/cde_heap_builder.h"
#include "dict/cde_dict.h"
#include "handler.h"

#include "catalog/dstore_fake_type.h"
#include "catalog/dstore_function_struct.h"
namespace CDE {

struct dml_tuple_t;

/** Flags indicating if alter table ops can be done instantly */
enum class dstore_instant_type : uint16_t {
  /** Impossible to alter table instantly */
  INSTANT_IMPOSSIBLE,

  /** Alter table instantly without any change */
  INSTANT_NO_CHANGE,

  /** Rename column instantly */
  INSTANT_RENAME_COLUMN,

  /** Varchar resize instantly */
  INSTANT_VARCHAR_RESIZE,

  /** TODO: no need to enumerate each instant op, refactor it later */
  /** Rename column and varchar resize can be done instantly at the same time */
  INSTANT_RENAME_COLUMN_WITH_VARCHAR_RESIZE,

  /** Adding or dropping virtual columns only */
  INSTANT_VIRTUAL_ONLY,

  /** ADD/DROP COLUMN which can be done instantly, including adding/dropping
  stored column only (or along with adding/dropping virtual columns) */
  INSTANT_ADD_DROP_COLUMN
};

class ha_cde_inplace_ctx : public inplace_alter_handler_ctx {
 public:
  /** CDE indexes being created */
  cde_dict_index_t **add_index;
  /** number of CDE indexes being created */
  uint32_t num_to_add_index;
  /** MySQL key numbers for the indexes that are being created */
  uint32_t *add_key_numbers;
  /** CDE indexes being dropped */
  cde_dict_index_t **drop_index;
  /** number of CDE indexes being dropped */
  uint32_t num_to_drop_index;
  /** CDE indexes being renamed */
  cde_dict_index_t **rename;
  /** number of CDE indexes being renamed */
  uint32_t num_to_rename;
  /** CDE foreign key constraints being dropped */
  cde_dict_foreign_t **drop_fk;
  /** number of CDE foreign key constraints being dropped */
  uint32_t num_to_drop_fk;
  /** CDE foreign key constraints being added */
  cde_dict_foreign_t **add_fk;
  /** number of CDE foreign key constraints being dropped */
  uint32_t num_to_add_fk;
  /** original table (if rebuilt, differs from indexed_table) */
  cde_dict_t *old_table;
  /** table where the indexes are being created or dropped */
  cde_dict_t *new_table;
  /** new column names, or nullptr if nothing was renamed */
  const char **col_names;
  /** new added virtual column map */
  std::unordered_map<uint32_t, DictVirtualCol> new_addedVirtualCols;
  /** min added virtual column field pos */
  uint32_t minAddedVirtualInd;
  /** old existed virtual column map */
  std::unordered_map<uint32_t, DictVirtualCol> old_existVirtualCols;
  /** whether heap reconstruct in this inplace operation */
  bool needRebuild = false;
  /** rebuild case, every old column corresponding new column pos */
  std::vector<uint32_t> colMap;
  /** rebuild case, old column used in new table and can't be null */
  std::vector<uint32_t> colNotNull;
  /** rebuild new table added column value */
  AddedFieldInfos addedFieldsInfos;
  /** added AUTO_INCREMENT column position, or UIN32_MAX if not */
  uint32_t addAutoinc;
  /** autoinc sequence to use */
  AutoIncreSequence sequence;
  /** maximum auto-increment value */
  ulonglong maxAutoinc;

  ha_cde_inplace_ctx(dstore_handler_t *dstore_ht_args,
                     cde_dict_index_t **drop_arg, uint32_t num_to_drop_arg,
                     cde_dict_index_t **rename_arg, uint32_t num_to_rename_arg,
                     cde_dict_foreign_t **drop_fk_arg,
                     uint32_t num_to_drop_fk_arg,
                     cde_dict_foreign_t **add_fk_arg,
                     uint32_t num_to_add_fk_arg, cde_dict_t *new_table_arg,
                     const char **col_names_args, uint32_t addAutoincArg,
                     uint64_t autoincColMinValueArg,
                     uint64_t autoincColMaxValueArg, THD *thd)
      : inplace_alter_handler_ctx(),
        sequence(thd, autoincColMinValueArg, autoincColMaxValueArg) {
    add_index = nullptr;
    num_to_add_index = 0;
    drop_index = drop_arg;
    num_to_drop_index = num_to_drop_arg;
    rename = rename_arg;
    num_to_rename = num_to_rename_arg;
    drop_fk = drop_fk_arg;
    num_to_drop_fk = num_to_drop_fk_arg;
    add_fk = add_fk_arg;
    num_to_add_fk = num_to_add_fk_arg;
    old_table = dstore_ht_args->table_handler;
    new_table = new_table_arg;
    col_names = col_names_args;
    add_key_numbers = nullptr;
    addAutoinc = addAutoincArg;
    maxAutoinc = 0;
  }

  ~ha_cde_inplace_ctx() override {
    if (add_index != nullptr) {
      CdeFree(add_index);
    }

    if (add_key_numbers != nullptr) {
      CdeFree(add_key_numbers);
    }

    if (drop_index != nullptr) {
      CdeFree(drop_index);
    }

    if (rename != nullptr) {
      CdeFree(rename);
    }

    if (col_names != nullptr) {
      CdeFree(col_names);
    }

    if (drop_fk != nullptr) CdeFree(drop_fk);

    for (uint32_t i = 0; i < num_to_add_fk; ++i) {
      if (add_fk[i]) {
        delete add_fk[i];
      }
    }

    if (add_fk) {
      CdeFree(add_fk);
    }

    for (auto &fieldInfo : addedFieldsInfos) {
      if (((fieldInfo.m_typeOid == BLOBOID) ||
           (fieldInfo.m_typeOid == CLOBOID)) &&
          fieldInfo.m_defaultValueWithVarlen.data) {
        CdeFree(const_cast<unsigned char *>(
            fieldInfo.m_defaultValueWithVarlen.data));
      }
    }
  }

 private:
  // Disable copying
  ha_cde_inplace_ctx(const ha_cde_inplace_ctx &);
  ha_cde_inplace_ctx &operator=(const ha_cde_inplace_ctx &);
};

struct FieldExtraInfo {
  uint32_t indexPos;
  uint32_t heapPos;
  DictVirtualCol *virtualCol = nullptr;
  size_t numColumnsToDecode;
  MysqlAndDstoreIndex *columnsToDecode = nullptr;
};

class ConvertHeapValueToIndexValue {
 public:
  ConvertHeapValueToIndexValue(
      const TABLE *oldTable, TABLE *alteredTable, cde_dict_t *dictTable,
      DSTORE::TupleDesc oldHeapAttr, cde_dict_t *newDictTable,
      const cde_dict_index_t *newIndex, MEM_ROOT *blobMemRoot,
      std::unordered_map<uint32_t, DictVirtualCol> &new_addedVirtualCols,
      std::unordered_map<uint32_t, DictVirtualCol> &old_existedVirtualCols,
      bool isRebuild);
  void init();
  void destroy();
  Datum operator()(DSTORE::FunctionCallInfo fcinfo);
  uint16_t getFiledSQLIndexInNewTable(uint16_t oldSqlIndex);

  const TABLE *m_oldTable;
  TABLE *m_newTable;
  cde_dict_t *m_oldDictTable;
  DSTORE::TupleDesc m_oldHeapAttr;
  cde_dict_t *m_newDictTable;
  const cde_dict_index_t *m_newIndex;
  MEM_ROOT *m_blobMemRoot;
  bool m_isRebuild;
  DSTORE::StorageRelationData *m_relation;
  uint32_t m_deformTupleEndIndex = 0;
  uint32_t m_maxHeapPos = 0;
  std::vector<FieldExtraInfo> m_fieldExtraInfos;
  dml_tuple_t *m_fullTuple{nullptr};
  unsigned char *m_record{nullptr};
  bool m_hasBlob{false};
  bool m_canMultiThread{true};
};

/**
Check if INSTANT ADD/DROP can be done.

@param[in]      haAlterInfo   the alter info
@param[in]      table         the old table.
@param[in]      alteredTable  the altered table.

@return true if succeed
*/
bool IsInstantAddDropPossible(const Alter_inplace_info *haAlterInfo,
                              const TABLE *table, const TABLE *alteredTable);

/** Tries to find an index whose first fields are the columns in the array,
in the same order and is not marked for deletion and is not the same
as types_idx.
 @param[in] table table to find index
 @param[in] colNames column names if we rename column in alter table statement,
                     or NULL to use table->col_names
 @param[in] fkColumns array of column names
 @param[in] nCols number of columns
 @param[in] typesIdx NULL or an index whose types the column types must match
 @param[in] check_charsets whether to check charsets.  only has an effect if
typesIdx != NULL
 @param[in] check_null true if none of the columns must be declared NOT NULL

 @return matching index, NULL if not found
*/
cde_dict_index_t *CdeForeignKeyFindIndex(cde_dict_t *table,
                                         const char **colNames,
                                         const char **fkColumns, uint32_t nCols,
                                         const cde_dict_index_t *typesIdx,
                                         bool check_charsets, bool check_null);

void CdeInplaceAlterReportDupKey(TABLE *alteredTable,
                                 cde_dict_index_t *dictIndex,
                                 DSTORE::IndexTuple *dupIndexTuple,
                                 DSTORE::TupleDesc attrFull, KEY *dupKey,
                                 const char *msg);

} /* namespace CDE */
#endif
