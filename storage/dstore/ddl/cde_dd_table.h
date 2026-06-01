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

#ifndef CDE_DD_TABLE_H
#define CDE_DD_TABLE_H

#include "sql/dd/string_type.h"

#include "cde_tuple.h"
#include "dict/cde_dict.h"
#include "systable/dstore_relation.h"

namespace dd {
class Table;
class Index;
class Partition;
typedef unsigned long long Object_id;
}  // namespace dd

namespace CDE {

/** Maximum number of rows version allowed when columns are added/dropped
INSTANTly. After this limit is reached, any attempt to do ADD/DROP INSTANT
column will result in error. */
const uint8_t DSTORE_MAX_ROW_VERSION = 16;

/** Class to decode or encode a stream of default value for instant table.
The decode/encode are necessary because that the default values would b
kept as dstore format stream, which is in fact byte stream. However,
to store them in the DD se_private_data, it requires text(char).
So basically, the encode will change the byte stream into char stream,
by splitting every byte into two chars, for example, 0xFF, would be split
into 0x0F and 0x0F. So the final storage space would be double. For the
decode, it's the converse process, combining two chars into one byte. */
class DDInstantColValCoder {
 public:
  /** Constructor */
  DDInstantColValCoder() : m_result(nullptr) {}

  /** Destructor */
  ~DDInstantColValCoder() { Cleanup(); }

  /**
  Encode the specified stream in format of bytes into chars
  @param[in]    stream  stream to encode in bytes
  @param[in]    inLen   length of the stream
  @param[out]   outLen  length of the encoded stream

  @return   the encoded stream, which would be destroyed if the class
  itself is destroyed */
  const char *Encode(const unsigned char *stream, size_t inLen, size_t *outLen);

  /**
  Decode the specified stream, which is encoded by encode()

  @param[in]    stream  stream to decode in chars
  @param[in]    inLen   length of the stream
  @param[out]   outLen  length of the decoded stream

  @return   the decoded stream, which would be destroyed if the class
  itself is destroyed */
  const unsigned char *Decode(const char *stream, size_t inLen, size_t *outLen);

 private:
  /** Clean-up last result */
  void Cleanup() { delete[] m_result; }

 private:
  /** The encoded or decoded stream */
  unsigned char *m_result;
};

constexpr uint32_t INVALID_DD_VERSION = std::numeric_limits<uint32_t>::max();

/* The prefix name of instant dropped column */
constexpr char DSTORE_DROP_PREFIX[] = "!hidden!_dropped_";

template <typename Table>
void CdeDdTableSetSePrivateData(Table *table_def,
                                DSTORE::StorageRelation table_rel,
                                cde_dict_t *dict_table);

/**
Sets auto_inc value in table's DD se_private_data

@param[in]      tableDef  The TABLE object stores the se_private_data.
@param[in]      autoInc   The auto increment value to set in se_private_data.
*/
void CdeDdTableSetSePrivateDataAutoInc(dd::Table *tableDef,
                                       const uint64_t &autoInc);
/**
Gets auto_inc value from table's DD se_private_data

@param[in]      tableDef  The TABLE object stores the se_private_data.
@return the auto increment value stored in DD Table's se_private_data.
*/
template <typename Table>
uint64_t CdeDdTableGetSePrivateDataAutoInc(const Table *tableDef);

template <typename Table>
void CdeDdIndexSetSePrivateData(Table *table_def, const cde_dict_t *dictTable);

template <typename Table>
uint16_t CdeDdTableGetSePrivateDataRelfileid(const Table *table_def);
template <typename Table>
uint32_t CdeDdTableGetSePrivateDataRelblknum(const Table *table_def);
/**
To get the lob fileid that has been persisted in se_private_dat

@param[in]      tableDef  The TABLE object stores the se_private_data
information.

@return fileid
*/
template <typename Table>
DSTORE::FileId CdeTableGetLobFileId(const Table *tableDef);
/**
To get the lob blocknumber that has been persisted in se_private_data.

@param[in]      tableDef  The TABLE object stores the se_private_data
information.

@return blocknumber
*/
template <typename Table>
DSTORE::BlockNumber CdeTableGetLobBlockNumber(const Table *tableDef);
template <typename Table>
uint32_t CdeDdTableGetTableRelOid(const Table *table_def);
template <typename Table>
uint64_t CdeDdTableGetTableId(const Table *table_def);
size_t CdeDdTableGetColumnNums(dd::Table *table_def);
const char *CdeDdTableGetName(const dd::Table *table_def);
const char *CdeDdIndexGetName(const dd::Index *index_def);

template <typename Table>
uint16_t CdeDdIndexGetSePrivateDataRelfileid(const Table *table_def,
                                             size_t key_no);
template <typename Table>
uint32_t CdeDdIndexGetSePrivateDataRelblknum(const Table *table_def,
                                             size_t key_no);
template <typename Table>
uint32_t CdeDdIndexGetTableRelOid(const Table *table_def, size_t key_no);
template <typename Table>
DSTORE::CommitSeqNo CdeDdIndexGetIndexCreateCsn(const Table *table_def,
                                                size_t key_no);
int CdeDdTableLoadForeignKey(THD *thd, const char *tbl_name,
                             cde_dict_t *m_table, const dd::Table *dd_table,
                             std::deque<const char *> &fk_names,
                             bool lockOperationalLock = true);
void CdeDdOpenForeignKeyTables(std::deque<const char *> &fk_names, THD *thd);
template <typename Table>
cde_dict_t *CdeDdOpenTable(THD *thd, const char *name, const Table *dd_table,
                           const TABLE *form_table);

/**
Open table using its name - together with child tables
(via foreign key relation).

@param[in]      name          table's name
@param[in]      thd           thread THD
@param[out]     mdl           metadata lock
@param[in]      allowEviction whether this table can be evicted by eviction
                              thread

@retval cde_dict_t on success, nullptr if failed.
*/
cde_dict_t *CdeDdOpenTableOneOnName(char *name, THD *thd, MDL_ticket **mdl,
                                    bool allowEviction);

/**
Instantiate an cde_dict_t based on a global DD object

@param[in]	thd		   thread THD
@param[in]	name	   table name
@param[in]	ddTable  Global DD table object

@retval cde_dict_t on success, nullptr if failed.
*/
template <typename Table>
cde_dict_t *CdeDdOpenTableOnDdObj(THD *thd, const char *name,
                                  const Table *ddTable);

/**
Release a metadata lock.

@param[in,out]  thd     current thread
@param[in,out]  mdl     metadata lock
*/
void CdeDdMdlRelease(THD *thd, MDL_ticket **mdl);

/**
Open uncached table definition based on a Global DD object.

@param[in]	thd		   thread THD
@param[in]	ddTable  Global DD table object
@param[in]	dbName   Database name
@param[out]	ts       MySQL table share
@param[out]	td       MySQL table definition

@retval CDE_SUCC on success, CDE_FAIL on error.
*/
bool CdeAcquireUncacheTable(THD *thd, const dd::Table *ddTable,
                            const char *dbName, TABLE_SHARE *ts, TABLE *td);

/**
Free uncached table definition.

@param[in]	ts       MySQL table share
@param[in]	td       MySQL table definition
*/
void CdeReleaseUncachedTable(TABLE_SHARE *ts, TABLE *td);

/**
Fill cde dict table struct from data dictionary.

@param[in]  dd_table   dd::Table describing the table.
@param[in]  form_table Table structure.
@param[in]  name       Table name.
@param[in]  forReplay  true if in ddl_info replay phase.

@return cde dict table struct if execute succeed, else return nullptr.
*/
template <typename Table>
cde_dict_t *CdeDdFillDictTable(const Table *dd_table, const TABLE *form_table,
                               const char *name, bool forReplay);
template <typename Table>
int CdeDdFillDictIndex(const Table &dd_table, const TABLE *form_table,
                       cde_dict_t *dict_table);

/**
Copy the engine-private parts of a table private data, eg,
rel_oid/segment_id, all alter table inplace/instant ops should call this func
@tparam         Table      dd::Table
@param[in,out]  new_table  New table definition, some private data need
                            copy from old
@param[in]      old_table  Old table definition
*/
template <typename Table>
void CdeDdTableCopyPrivate(Table &new_table, const Table &old_table);

/**
Load dict col info from dd for instant add/drop columns, and get max version.

@param[in]      ddTable    dd table
@param[in,out]  maxVersion Max version of this dict table
@param[in,out]  maxPos     Max physical postion used of this dict table

@return Dictcol arrays being build or nullptr.
*/
DictCol *BuildDictColsFromDD(const dd::Table *ddTable, uint32_t &maxVersion,
                             uint32_t &maxPos);

DictCol *BuildDictColsFromInplaceCreateDD(const dd::Table *ddTable,
                                          uint32_t &maxPos);

/**
Check wether the table has instant dropped column

@tparam         Table      dd table to process
@param[in]      table_def  table definition
@return true if the table has dropped column

@return true if the table has dropped column
*/
bool TableHasDroppedColumn(const dd::Table &table_def);

/**
Check wether the column is instant dropped column

@param[in]      dd_col  column

@return true if the column is dropped
*/
bool DDColumnIsDropped(const dd::Column *dd_col);

/**
Look up a column in a table using the system_charset_info collation.

@param[in]      dd_table        data dictionary table
@param[in]      name            column name
@return the column

@return nullptr if not found
*/
const dd::Column *DDFindColumn(const dd::Table *dd_table, const char *name);

/**
Make the name of instant dropped column.
@param[in,out]  name            the being dropped column's name.
@param[in]      version         column current version.
@param[in]      phyPos          column current physical pos.
*/
void BuildDroppedColumnName(std::string &name, uint32_t version,
                            uint32_t phyPos);

/**
Copy the instant dropped column from old table to new table.

@param[in]      old_table          old dd table.
@param[in,out]  new_table          new dd table.

@return true if success else false.
*/
bool DDCopyDroppedColumns(const dd::Table &old_table, dd::Table &new_table);

/**
Add a hidden column when creating a table.

@param[in,out]  dd_table        table containing user columns and indexes
@param[in]      old_col         the old column copy from
@param[in]      name            column name

@return the added column, or NULL if there already was a column by that name
*/
dd::Column *DDCopyHiddenColumn(dd::Table *dd_table, const dd::Column *old_col,
                               const char *name);

/**
Check wether the column is dropped via alter info.

@param[in]      ha_alter_info  alter info
@param[in]      column_name    old column name

@return true if the column is dropped
*/
bool IsFieldToDrop(const Alter_inplace_info &ha_alter_info,
                   const char *column_name);

/**
Check wether the column is renamed via alter info.

@param[in]      ha_alter_info  alter info
@param[in]      old_name       old column name
@param[out]     new_name       the renamed column name

@return true if the filed if to be renamed else false.
*/
bool IsFieldToRename(const Alter_inplace_info &ha_alter_info,
                     const char *old_name, std::string &new_name);

/**
Get the added version of a column.

@param[in]      dd_col  dd column to get version

@return   the column's added version
*/
uint32_t DDColumnGetVersionAdded(const dd::Column *dd_col);

/**
Check whether the column is added instantly.

@param[in]      dd_col  DD column to check.

@return   true if the column is added instantly.
*/
bool DDColumnIsInstantAdded(const dd::Column *dd_col);

/**
Set the default of instant add/drop column.

@param[in]      private_data  the se_private_data of dd column
@param[in]      field         field to be added or dropped
*/
void SetColumnDefaultVal(dd::Properties &private_data, Field &field);

/**
Copy the se_private_data of no changed columns

@param[in]      ha_alter_info      he being dropped column's name.
@param[in]      old_table          old dd table.
@param[in,out]  new_table          new dd table.
@param[in]      dict_table         dstore table cache
*/
void DDCopyColumnPrivate(const Alter_inplace_info &ha_alter_info,
                         const dd::Table &old_table, dd::Table &new_table,
                         cde_dict_t *dict_table);

/** Determine if a dd::Table has row versions
@param[in]      table   dd::Table
@return true if table has row versions, false otherwise */
bool DDTableHasRowVersions(const dd::Table &table);

/** Clear the instant ADD COLUMN information of a table
@param[in,out]  dd_table        dd::Table
@return CDE_OK if success else false.
*/
int32_t DDClearInstantTable(dd::Table &dd_table);

/**
Get the number of columns being added using ALTER TABLE.

@param[in]      haAlterInfo   inplace alter info
@return number of columns added using ALTER TABLE
*/
uint32_t GetNumColsAdded(const Alter_inplace_info *haAlterInfo);

/**
@brief Gets dstore table DD info.

This function gets table oid and SE private data for generating DDL info.

@param[in,out] packet the destination buffer to store the table DD info.
@param[in]     table_obj dd::Table object describing the table.
*/
void CdeGetDstoreTableDDInfo(String *packet, const dd::Table *table_obj);

/**
@brief Gets dstore index DD info.

This function gets index oid and SE private data for generating DDL info.

@param[in,out] packet the destination buffer to store the index DD info.
@param[in]     table_obj dd::Table object describing the table.
@param[in]     key_no current index number
*/
void CdeGetDstoreIndexDDInfo(String *packet, const dd::Table *table_def,
                             size_t key_no);

/**
Fill the fields' length info of table, based on dd table.

@param[in]      ddTable    dd table
@param[in]      formTable  Table structure.
@param[out]     fieldLenInfos  length infos of all fields in table
@return         number of fields
*/
struct NumberOfFields {
  uint32_t nonVirtualFieldsNum{0};
  uint32_t virtualFieldsNum{0};
};

NumberOfFields FillFieldLenFromDD(const dd::Table *ddTable,
                                  const TABLE *formTable,
                                  std::vector<DictFieldLen> &fieldLenInfos);

/** Build virtual columns
@param[in]    table       Dstore table
@param[in]    formTable   MySQL table
@param[in]    ddTable     DD table
@return virtual columns
*/
std::vector<DictVirtualCol> BuildVCols(cde_dict_t *table,
                                       const TABLE *formTable,
                                       const dd::Table *ddTable);

/** Write default value of a column to dd::Column
@param[in]      col     default value of this column to write
@param[in,out]  ddCol   where to store the default value */
void DDwriteDefaultValue(const DictCol *col, dd::Column *ddCol);

/** Find the specified dd::Index or dd::Partition_index in an DStore table
@tparam         Index                   dd::Index or dd::Partition_index
@param[in]      table                   DStore table object
@param[in]      dd_index                Index to search
@return the cde_dict_index_t object related to the index */
template <typename Index>
const cde_dict_index_t *DDFindIndex(const cde_dict_t *table, Index *dd_index);

/** Open a table from its database and table name, this is currently used by
foreign constraint parser to get the referenced table.
@param[in]      name                    foreign key table name
@param[in]      database_name           table db name
@param[in]      table_name              table db name
@param[in,out]  table                   table object or NULL
@param[in,out]  mdl                     mdl on table
@param[in,out]  heap                    heap memory
@return complete table name with database and table name, allocated from
heap memory passed in */
char *DDGetRefrencedTable(const char *name, const char *database_name,
                          const char *table_name, cde_dict_t **table,
                          MDL_ticket **mdl, MEM_ROOT *memRoot);

/** Set up base columns for virtual column
@param[in]      table   Dstore table
@param[in]      field   MySQL field
@param[in,out]  v_col   virtual column to be set up
*/
void setupBaseColumns(cde_dict_t *table, const Field *field,
                      DictVirtualCol *vCol);

/** Copy the AUTO_INCREMENT if exist.
@param[in]      src     dd::Table::se_private_data to copy from
@param[out]     dest    dd::Table::se_private_data to copy to */
void DDCopyAutoinc(const dd::Properties &src, dd::Properties &dest);
}  // namespace CDE

#endif
