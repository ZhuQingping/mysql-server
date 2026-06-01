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

#ifndef CDE_DDL_H
#define CDE_DDL_H

#include <iostream>
#include <map>
#include <vector>

#include "common/dstore_common_utils.h"
#include "framework/dstore_instance_interface.h"
#include "index/dstore_index_interface.h"
#include "systable/dstore_relation.h"
#include "table/dstore_table_interface.h"

#include "cde_tuple.h"
#include "ddl/cde_ddl_log.h"
#include "dict/cde_dict.h"
#include "sql/table.h"

using DSTORE::IndexInfo;
using DSTORE::PageId;
namespace CDE {
/* Index options */
constexpr int16 CDE_INDEX_OPTION_DESC =
    0x0001; /* values are in reverse order */
constexpr int16 CDE_INDEX_OPTION_NULLS_FIRST =
    0x0002; /* NULLs are first instead of last */

/** This is the "mbmaxlen" for my_charset_filename (defined in
strings/ctype-utf8.c), which is used to encode File and Database names. */
constexpr uint32_t CDE_FILENAME_CHARSET_MAXNAMLEN = 5;

/** The maximum length of an encode table name in bytes.
The max table and database names are NAME_CHAR_LEN (64) characters.
The max length would be NAME_CHAR_LEN (64) * CDE_FILENAME_CHARSET_MAXNAMLEN (5)
= 320 bytes. The number does not include a terminating '\0'.*/
constexpr uint32_t CDE_MAX_TABLE_NAME_LEN = 320;

/** The maximum length of a database name. Like MAX_TABLE_NAME_LEN this is
the MySQL's NAME_LEN, see check_and_convert_db_name(). */
constexpr uint32_t CDE_MAX_DATABASE_NAME_LEN = CDE_MAX_TABLE_NAME_LEN;

constexpr uint32_t CDE_MAX_INDEX_NAME_LEN = CDE_MAX_TABLE_NAME_LEN;

constexpr uint32_t CDE_MAX_FK_NAME_LEN = NAME_CHAR_LEN;

/** CDE_MAX_FULL_NAME_LEN defines the full name path including the
database name and table name. In addition, 14 bytes is added for:
        2 for surrounding quotes around table name
        1 for the separating dot (.)
        9 for the #mysql50# prefix */
constexpr uint32_t CDE_MAX_FULL_NAME_LEN =
    CDE_MAX_TABLE_NAME_LEN + CDE_MAX_DATABASE_NAME_LEN + 14;

/** DStore engine default heap fillfactor value, based on
performance and scalability considerations, choose 80 as default value. */
constexpr int32_t DEFAULT_FILLFACTOR = 80;

/** DStore engine default index fillfactor value, based on
performance and scalability considerations, choose 80 as default value. */
constexpr int32_t DEFAULT_FILLFACTOR_FOR_INDEX = 80;

constexpr const char *object_relfileid = "relfileid";
constexpr const char *object_relblknum = "relblknum";
constexpr const char *object_rellobfileid = "rellobfileid";
constexpr const char *object_rellobblknum = "rellobblknum";
constexpr const char *object_reltablespace = "reltablespace";
constexpr const char *object_indexrelid = "indexrelid";
constexpr const char *object_tablerelid = "tablerelid";
constexpr const char *object_indexcreatecsn = "indexcreatecsn";
constexpr const char *object_tablecreatecsn = "tablecreatecsn";

constexpr const char *auto_inc = "autoinc";

/** DStore engine default heap fillfactor value, used for variable
cde_default_fillfactor. */
extern int32_t g_defaultFillfactor;

/** DStore engine default index fillfactor value, used for variable
cde_default_fillfactor_for_index. */
extern int32_t g_defaultFillfactorForIndex;

/** Definition of an index being created */
struct cde_index_def {
  const char *name;       /*!< index name */
  uint32_t key_length{0}; /*!< length of key */
  bool rebuild;           /*!< whether the table is rebuilt */
  bool indisunique;
  uint32_t n_fields;     /*!< number of fields in index */
  cde_field_def *fields; /*!< field definitions */

  /** Fillfactor value that parsed from index comment, set to correct value
  if fillfactor specified and value is valid, else set to 0(invalid value). */
  int32_t fillfactor{0};

  /** MySQL key number, or ~0UL if none */
  uint32_t m_keyNumber{~0U};

  void destroy_from_cache();

  ~cde_index_def() { destroy_from_cache(); }
};

/**
Create a dstore table.

@param[in]  thd        connection contex
@param[in]  name       Name format: ./DB_name/table_name
@param[in]  form       Table handler from sql layer
@param[in]  createInfo create info in handle layer
@param[in]  tableDef   table defination in dd

@return 0 if success
*/
int CdeCreateTable(THD *thd, const char *name, TABLE *form,
                   HA_CREATE_INFO *createInfo, dd::Table *tableDef);

/**
Drop a dstore table.

@param[in]  thd        connection contex
@param[in]  name       table name
@param[in]  tableDef   table defination in dd

@return CDE_SUCC if success, CDE_FAIL if failed.
*/
template <typename Table>
bool CdeDropTable(THD *thd, const char *name, const Table *tableDef);

template <typename Table>
int32_t CdeTruncateTable(THD *thd, const char *name, Table *tableDef);

/**
Rename a dstore table.

@param[in]  thd             connection contex
@param[in]  from            old table name
@param[in]  to              new table name
@param[in]  dictTable       dict table to be renamed
@param[in]  toTableDef      new table defination

@return 0 if success
*/
int CdeRenameTable(THD *thd, const char *from, const char *to,
                   cde_dict_t *dictTable, dd::Table *toTableDef);

/**
Marks a table to be discared on the next open. Also marks
table's dict as invalid - so the handler to this table would
not get stored in the server's Table cache.

@param[in]  dictTable       dict table to be discarded
@param[in]  table           table to be discarded
*/
void DiscardAfterDDL(cde_dict_t *dictTable, TABLE *table);

/**
Create index build info.

@param[in]  indexStorRel   index store relation
@param[in]  fullAttr       attributes including virtual columns
@param[in]  indexAttrNum   index attribute num
@param[in]  attrCols       columns belong to attributes index
@param[in]  fields         index columns info

@return IndexInfo
*/
DSTORE::IndexBuildInfo CreateIndexBuildInfo(DSTORE::StorageRelation heapStorRel,
                                            DSTORE::TupleDesc fullAttr,
                                            uint32_t indexAttrNum,
                                            uint32_t *attrCols,
                                            cde_dict_field_t *fields);

bool IsBaseStoredColumn(const TABLE *table, const char *name);

class CdeCreateTableInfo {
 public:
  /**
  construction

  @param[in]  tbsId      Table space ID.
  */
  CdeCreateTableInfo(DSTORE::TablespaceId tbsId) : m_tableSpaceId(tbsId) {}

  /**
  construction

  @param[in]  thd        connection contex
  @param[in]  name       table name
  @param[in]  form       Table handler from sql layer
  @param[in]  createInfo create info in handle layer
  @param[in]  tableDef   table defination in dd
  */
  CdeCreateTableInfo(THD *thd, const char *name, TABLE *form,
                     HA_CREATE_INFO *create_info, dd::Table *dd_tab);
  virtual ~CdeCreateTableInfo();
  /**
  Prepare table name.
  @param[in]  name        connection contex
  @return 0 if success
  */
  int PrepareCreateTable(const char *name, bool normalize = false);

  /**
  Create a dstore table and add cde_dict_t to cache.
  */
  int CreateTable();

  cde_dict_t *CdeCreateInplaceRebuildTable(THD *thd, const char *name,
                                           const TABLE *form,
                                           dd::Table *tableDef);

 private:
  /**
  Create cde dict index.

  @param[in]  heapStorRel   heap table of the index
  @param[in]  fullAttr      attributes including virtual columns
  @param[in]  key           index meta info
  @param[in]  dictCols      table columns info

  @return cde_dict_index_t
  */
  cde_dict_index_t *CreateCdeDictIndex(DSTORE::StorageRelation heapStorRel,
                                       DSTORE::TupleDesc fullAttr,
                                       const KEY *key, DictCol *dictCols,
                                       bool needCreateIndexInfo = true);

 public:
  /**
  Split normalized name.

  @param[in]    name        normalized name
  @param[out]   table_name  table Name
  @param[out]   db_name     database Name

  @return 0 success otherwise fail
  */
  static int SplitNormalizedName(const char *name, char *table_name,
                                 char *db_name);

  /**
  Normalize table name.

  @param[int, out]  norm_name   normalized_name
  @param[out]       name        Table name string

  @return 0 success otherwise fail
  */
  static int NormalizeTableName(char *norm_name, const char *name);

  /**
  Create storage relation for index.

  @param[in]  heapStorRel   heap table of the index
  @param[in]  fullAttr      attributes including virtual columns
  @param[in]  dictCols      table columns info
  @param[in]  indexOid      index oid
  @param[in]  segmentId     segment id
  @param[in]  key           index meta info
  @param[out] indexCols     columns belong to index
  @param[out] attrCols      columns belong to attributes index
  @param[out] keyInfos      scan key
  @param[in]  fields        index columns info
  @param[in]  isRecovery    is recover process
  @param[in]  fillfactor    fill factor

  @return cde_dict_index_t
  */
  DSTORE::StorageRelation CreateIndexStorRel(
      DSTORE::StorageRelation heapStorRel, DSTORE::TupleDesc fullAttr,
      DictCol *dictCols, DSTORE::Oid indexOid, DSTORE::PageId segmentId,
      const KEY *key, uint32_t *indexCols, uint32_t *attrCols,
      DSTORE::ScanKey *keyInfos, cde_dict_field_t *fields, bool isRecovery,
      const int32_t fillfactor);
  /**
  Create storage relation for index.

  @param[in]  heapStorRel   heap table of the index
  @param[in]  fullAttr      attributes including virtual columns
  @param[in]  indexOid      index oid
  @param[in]  segmentId     segment id
  @param[in]  indexDef      index meta info
  @param[in]  dictCols      table columns info
  @param[out] indexCols     columns belong to index
  @param[out] attrCols      columns belong to attributes index
  @param[out] keyInfos      scan key

  @return cde_dict_index_t
  */
  DSTORE::StorageRelation CreateIndexStorRel(
      DSTORE::StorageRelation heapStorRel, DSTORE::TupleDesc fullAttr,
      DSTORE::Oid indexOid, DSTORE::PageId segmentId,
      const cde_index_def *indexDef, DictCol *dictCols, uint32_t *indexCols,
      uint32_t *attrCols, DSTORE::ScanKey *keyInfos);
  /**
  Create sys relation tuple.

  @param[in]  relkind       relation kind
  @param[in]  colNum        column number
  @param[in]  segmentId     segment id
  @param[in]  lobSegmentId  index segment id

  @return SysClassTupDef
  */
  DSTORE::SysClassTupDef *CreateSysRelTuple(char relkind, int16_t colNum,
                                            DSTORE::PageId segmentId,
                                            DSTORE::PageId lobSegmentId);
  /**
  Create dstore index info.

  @param[in]  indexAttrNum   index attribute num
  @param[in]  key            index key info
  @param[in]  indexStorRel   index store relation

  @return IndexInfo
  */
  static DSTORE::IndexInfo *CdeCreateIndexInfo(
      uint32_t indexAttrNum, const KEY *key,
      DSTORE::StorageRelation indexStorRel);

  /**
  Create dstore index info.

  @param[in]  indexDef       cde index definition
  @param[in]  indexStorRel   index store relation

  @return IndexInfo
  */
  static DSTORE::IndexInfo *CdeCreateIndexInfo(
      const cde_index_def *indexDef, DSTORE::StorageRelation indexStorRel);

 private:
  /**
  Create empty dstore index info.

  @param[in]  indexAttrNum   index attribute num

  @return IndexInfo
  */
  static DSTORE::IndexInfo *CdeCreateEmptyIndexInfo(uint32_t indexAttrNum);

 public:
  /**
  Create storage relation for heap.

  @param[in]   tableOid      table oid
  @param[in]   segmentId     segment id
  @param[in]   lobSegmentId  log segment id
  @param[in]   table         table handler
  @param[out]  fillfactor    fill factor
  @param[out]  ddTable       dd table info
  @param[out]  dictCols      dict column defination
  @param[out]  attrFull      attributes including virtual columns

  @return storage relation.
  */

  DSTORE::StorageRelation CreateHeapStorRel(
      DSTORE::Oid tableOid, DSTORE::PageId segmentId,
      DSTORE::PageId lobSegmentId, const TABLE *table, const int32_t fillfactor,
      const dd::Table *ddTable = nullptr, DictCol *dictCols = nullptr,
      bool forReplay = false, DSTORE::TupleDesc *attrFull = nullptr);

  /**
  Create fields for index.

  @param[in]  dictIndex      cde dict index
  @param[in]  key            key info
  @param[in]  table          table handler
  @param[in]  isAlterOp      whether alter add index op

  @return 0 if success.
  */
  static int CreateFieldsForIndex(cde_dict_index_t *dictIndex, const KEY *key,
                                  const TABLE *table, bool isAlterOp = false);

  /**
  Create table update dict.
  */
  template <typename Table>
  int CreateTableUpdateDict(Table *ddTable);

  /**
  Initialize autoinc.
  */
  void InitializeAutoinc();

  /**
  valid create info tablespace option

  @return true if success.
  */
  bool ValidateTableSpaceOption();

 private:
  THD *m_thd{nullptr};
  DSTORE::StorageInstanceInterface *m_instance{nullptr};
  const char *m_name{nullptr};
  char m_tableName[FN_REFLEN];
  TABLE *m_form{nullptr};
  HA_CREATE_INFO *m_create_info{nullptr};
  dd::Table *m_dd_tab{nullptr};
  DSTORE::StorageRelation m_heapStorRel{nullptr};
  cde_dict_t *m_dict{nullptr};
  bool m_isTmpTable = false;
  DSTORE::TablespaceId m_tableSpaceId{CDE_INVALID_SPACE_ID};
};

class CdeTruncate {
 public:
  CdeTruncate() = delete;
  template <typename Table>
  static bool DoTruncate(Table *ddTable, cde_dict_t *dictTable);

 private:
  /* Update segments of dict_table in memory. */
  static void UpdateDictTableSegmemt(cde_dict_t *dictTbl,
                                     DSTORE::SysClassTupDef *classTuple,
                                     DSTORE::PageId segmentId,
                                     DSTORE::PageId lobSegmentId);

  /* Rebuild heap storage relation */
  static bool ReconstructHeapRelation(cde_dict_t *dictTable);

  /* Rebuild Indexes storage relation */
  static bool ReconstructIndexRelation(cde_dict_t *dictTable);
};

class CdeAllocSegmentWrapper {
 public:
  static DSTORE::PageId Alloc(DSTORE::SegmentType type,
                              DSTORE::TablespaceId tableSpaceId);
  static void Drop(const DSTORE::PageId &segmentId, DSTORE::SegmentType type,
                   DSTORE::TablespaceId tableSpaceId);
};

/**
To determine whether the table contains lob column(column type is either blob or
json).

@param[in]      table   The TABLE object contains information about the columns.

@return true if the table contains lob column
*/
bool IsContainLobCol(const TABLE *table);

/**
Parse fillfactor value from the string and push warning to user if fillfactor
value is invalid and thd is not nullptr. For DDL query(CREATE, ALTER) need to
push warning to user if fillfactor is invalid, for parse info from DD
scenario(open table process), no need to push warning since user already
got a warning before(when execute DDL query).

@param[in]  str string which might include 'fillfactor=(\d+)' pattern
@param[in]  thd connection, thd is nullptr for parse info from DD scenario

@return value parsed. 0 means not found or invalid value.

@note In the common situation where we're processing a CREATE TABLE from the
user and the fillfactor is not specified in this statement, it is important to
distinguish the return value (0) from the default (the value of SQL variable
global.cde_default_fillfactor, for example 80): "0" will be stored in the
table's definition, meaning "fillfactor was not specified by the user, so use
the default", and "80" will be used when making pages; the key point is that
if later the default is changed, and the table is re-opened, the table's pages
will switch to the new default. On the opposite, if the user specified "80" in
CREATE TABLE, "80" will always be used.
*/
int32_t DstoreParseFillfactor(const char *str, THD *thd);

/**
  Check fillfactor is valid or not. Valid values are between 10 and 100.

  @param[int32_t]  val  fillfactor value

  @return true if val is valid fillfactor value, otherwise false.
*/
inline bool IsValidFillfactor(int32_t val) { return (val >= 10 && val <= 100); }

/**
  Check if the current session is interrupted by Kill Statement or Ctrl + C

  @return true if it is interrupted, otherwise false.
*/
bool CheckIsInterrupted();

/**
  Choose which type heap segment created
  @param[bool]  isTempTable  fillfactor value
  @return HEAP_TEMP_SEGMENT_TYPE if temporary table, otherwise
  HEAP_SEGMENT_TYPE.
*/
inline DSTORE::SegmentType ChooseHeapSegmentType(bool isTempTable) {
  return (isTempTable ? DSTORE::SegmentType::HEAP_TEMP_SEGMENT_TYPE
                      : DSTORE::SegmentType::HEAP_SEGMENT_TYPE);
}

/**
  Choose which type index segment created
  @param[bool]  isTempTable  fillfactor value
  @return INDEX_TEMP_SEGMENT_TYPE if temporary table, otherwise
  INDEX_SEGMENT_TYPE.
*/
inline DSTORE::SegmentType ChooseIndexSegmentType(bool isTempTable) {
  return (isTempTable ? DSTORE::SegmentType::INDEX_TEMP_SEGMENT_TYPE
                      : DSTORE::SegmentType::INDEX_SEGMENT_TYPE);
}
} /* namespace CDE */
#endif
