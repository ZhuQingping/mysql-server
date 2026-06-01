/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2026 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * ha_cdepart.cc
 *
 */

/** @file ha_cdepart.cc
Code for native partitioning in DStore. */

/* Include necessary SQL headers */

#include <debug_sync.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <log.h>
#include <my_check_opt.h>
#include <mysqld.h>
#include <scope_guard.h>
#include <sql_class.h>
#include <sql_show.h>
#include <sql_table.h>
#include <strfunc.h>
#include <algorithm>
#include <new>
#include "sql/dd/cache/dictionary_client.h"

#include "boot/cde_instance.h"
#include "common/cde_compare_utils.h"

#include "dd/dd.h"
#include "dd/dictionary.h"
#include "dd/properties.h"
#include "dd/types/partition.h"
#include "dd/types/partition_index.h"
#include "dd/types/table.h"

#include "ddl/cde_dd_table.h"
#include "ddl/cde_ddl.h"
#include "ddl/cde_ddl_log.h"
#include "ddl/cde_mysql_ops.h"
#include "ddl/cde_tablespace.h"
#include "dict/cde_dict_stat.h"
#include "dml/cde_dml.h"

#include "common/cde_errorcode.h"

#include "key.h"
#include "lex_string.h"
#include "my_byteorder.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_io.h"
#include "my_macros.h"
#include "mysql/plugin.h"
#include "partition_info.h"

#include "ddl/dd_helper.h"
#include "ha_cdepart.h"

using DSTORE::ScanKeyData;

namespace CDE {

/* Error Text */
static constexpr auto PARTITION_IN_SHARED_TABLESPACE =
    "DStore : A partitioned table"
    " is not allowed in a shared tablespace.";
static constexpr auto PARTITION_IN_DIFF_TABLESPACE =
    "DStore : A partitioned table is not allowed in different tablespace.";

/** defined in ha_cde.cc */
void CdeTrxStartIfNotStarted(THD *thd, bool rw);

HaCdepartShare::HaCdepartShare(TABLE_SHARE *table_share)
    : Partition_share(),
      m_tableParts(),
      m_totParts(),
      m_refCount(),
      m_tableShare(table_share),
      m_mem_root(PSI_NOT_INSTRUMENTED, 512) {}

HaCdepartShare::~HaCdepartShare() {
  CDE_ASSERT_DEBUG(m_refCount == 0);
  if (m_tableParts != nullptr) {
    CdeFree(m_tableParts);
    m_tableParts = nullptr;
  }
  m_tableShare = nullptr;
}

/** Reads a true VARCHAR length, in the MySQL row format, and
returns a pointer to the data.
 @param[out] len    variable-length field length
 @param[in]  field  field in the MySQL format
 @param[in]  lenlen storage length of len: either 1
                    or 2 bytes
 @return pointer to the data, we skip the 1 or 2 bytes at the start
that are used to store the len */
const uchar *MysqlFieldReadTrueVarchar(ulint *len, const uchar *field,
                                       ulint lenlen) {
  if (lenlen == 2) {
    *len = CdeReadFrom2LittleEndian(field);
    return (field + 2);
  }
  CDE_ASSERT_DEBUG(lenlen == 1);

  *len = ((uint8_t)field[0]);
  return (field + 1);
}

/** Copies a cached field for MySQL from the fetch cache.
 @param[in/out] buf   row buffer
 @param[in] cache     cached row
 @param[in] table     table to which buffers belong
 @param[in] filed     mysql field to copy
*/
static void CopyCachedFieldForMysql(uchar *buf, const uchar *cache,
                                    TABLE *table, Field *field) {
  ulint len;
  ulint mysqlColOffset = (ulint)field->offset(table->record[0]);

  buf += mysqlColOffset;
  cache += mysqlColOffset;

  if (field->type() == MYSQL_TYPE_VARCHAR &&
      (field->real_type() != MYSQL_TYPE_ENUM &&
       field->real_type() != MYSQL_TYPE_SET)) {
    MysqlFieldReadTrueVarchar(&len, cache, field->get_length_bytes());
    len += field->get_length_bytes();
  } else {
    len = field->pack_length();
  }
  memcpy_s(buf, len, cache, len);
}

void haCdepart::CopyCachedFieldsForMysql(uchar *buf, const uchar *cachedRec) {
  for (size_t i = 0; i < m_dstore->m_numColumnsToDecode; ++i) {
    if (m_dstore->read_just_key &&
        (false == m_dstore->current_cde_index->shared_dict->indexCoversColumn(
                      m_dstore->m_columnsToDecode[i].m_dstoreIndex)))
      continue;

    Field *mysqlField =
        m_table->field[m_dstore->m_columnsToDecode[i].m_mysqlIndex];

    CopyCachedFieldForMysql(buf, cachedRec, m_table, mysqlField);
    /* Copy NULL bit of the current field from cachedRec to buf */
    if (mysqlField->is_nullable()) {
      ulint mysqlNullByteOffset = mysqlField->null_offset();

      buf[mysqlNullByteOffset] ^=
          (buf[mysqlNullByteOffset] ^ cachedRec[mysqlNullByteOffset]) &
          (uchar)mysqlField->null_bit;
    }
  }
}

/** Copy a cached MySQL row.
@param[out]     buf             Row in MySQL format.
@param[in]      cached_row      Which row to copy. */
inline void haCdepart::copy_cached_row(uchar *buf, const uchar *cached_row) {
  if (m_dstore->keep_other_fields_on_keyread)
    CopyCachedFieldsForMysql(buf, cached_row);
  else
    memcpy_s(buf, m_rec_length, cached_row, m_rec_length);
}

void CdeSetTableFlagsFromTableShare(cde_dict_t *table,
                                    const TABLE_SHARE *table_share);

static session_index_info *cde_index_lookup(int keynr, dstore_handler_t *dstore,
                                            TABLE *table) {
  auto key_name = table->key_info[keynr].name;
  for (session_index_info *sess_idx : dstore->rel_info->cde_index_vec) {
    if (!strcmp(sess_idx->shared_dict->name.c_str(), key_name)) {
      return sess_idx;
    }
  }
  return nullptr;
}

/** Open one partition
@param[in]    thd             Thread THD
@param[in]    table           MySQL table definition
@param[in]    dd_part         dd::Partition
@param[in]    part_name       Table name of this partition
@param[out]   part_dict_table DStore table for partition
@param[out]   dstoreHandler   dstore handler to be created and
                                initialized
@param[in/out]memRoot         memory root to allocate memory
                                from
@param[in]    tableShare      table's share
@param[in]    fistPartDstore  first partition's dstore hander.
                              Some of the member fields may be reused
                              in the subsequent partitions.
@retval       false   On success
@retval       true    On failure */
bool HaCdepartShare::openOneTablePart(
    THD *thd, TABLE *table, const dd::Partition *ddPart, const char *partName,
    cde_dict_t **partDictTable, dstore_handler_t **outDstoreHandler,
    Memory::MemHeap &memRoot, TABLE_SHARE *tableShare,
    dstore_handler_t *fistPartDstore) {
  DBUG_TRACE;

  CDE_ASSERT_DEBUG(nullptr != ddPart);

  OperationalLockGuard opWGuard;
  DictSysLockGuard lockGuard;
  uint64_t autoincSaved = 0;
  cde_dict_t *partTable = DictSysGetTableLocked(partName, false);

  DictTableRefGuard dictRefGuard(partTable);
  if (partTable != nullptr && partTable->m_discardAfterDDL) {
    /** All the handlers that were openned during DDL are now closed.
    We are under dict sys lock - to make sure we are the first
    to discard this table. No other handler for this handler
    should get open during this operation. Otherwise, we would
    not be the only onces that hold reference to this table
    and we would not be able to discard it. */
    CDE_ASSERT_DEBUG(partTable->getRefCnt() == 1);
    autoincSaved =
        partTable->autoinc_dict.GetAutoinc();  // Robert: How to handle autoInc
    dictRefGuard.Release();
    /* DictSysRemoveTableLocked will release dict sys lock */
    DictSysRemoveTableLocked(partName, lockGuard);
    partTable = nullptr;
  } else
    lockGuard.Clear();

  opWGuard.Clear();

  if (nullptr == partTable) {
    partTable = CdeDdOpenTable(thd, partName, ddPart, table);
    if (partTable == nullptr) {
      CDE_LOG_ERROR("Open partition table failed, partName = %s", partName);
      return true;
    }
    /* partTable is acquired from dict cache here, let guard own this
    reference so failure paths below can release it correctly. */
    dictRefGuard.Reset(partTable);
  }

  dstore_handler_t *dstore = new (&memRoot) dstore_handler_t;

  if (rds_use_ddl_info_replay && thd->for_ddl_info_replay) {
    DiscardAfterDDL(partTable, table);
    CdeSetTableFlagsFromTableShare(partTable, tableShare);
  } else {
    CdeSetTableFlagsFromTableShare(partTable, tableShare);
    CdeDictTableStatsInit(partTable, thd);
  }

  int ret = initDStoreHandlerPart(partTable, dstore, thd->for_ddl_info_replay,
                                  table, memRoot, fistPartDstore);
  if (CDE_OK != ret) {
    /** table_handler needs to be null, otherwise the
     * dstore handler dtor would tried to release this part.
     * It will be released by dictRefGuard.
     */
    CDE_ASSERT_DEBUG(nullptr == dstore->table_handler);
    return ret;
  }

  /* Now the duty of guard belongs to m_dstore. So reset guard here. */
  dictRefGuard.Reset(nullptr);

  if (table->found_next_number_field != nullptr) {
    if (autoincSaved != 0) {
      partTable->autoinc_dict.SetAutoinc(autoincSaved);
      partTable->autoinc_dict.SetInitialized(true);
    } else if (!thd->for_ddl_info_replay) {
      session_index_info *autoincIndex =
          cde_index_lookup(table->s->next_number_index, dstore, table);

      uint64_t ddAutoIncValue =
          CdeDdTableGetSePrivateDataAutoInc(&(ddPart->table()));
      if (InitAutoinc(&partTable->autoinc_dict, table, ddAutoIncValue,
                      autoincIndex->rel,
                      autoincIndex->shared_dict->index_col_num)) {
        return true;
      }
    }
  }

  *partDictTable = partTable;
  *outDstoreHandler = dstore;
  return (partTable == nullptr);
}

/** Increment share and DStore tables reference counters. */
void HaCdepartShare::incrementRefCounts() {
  CDE_ASSERT_DEBUG(nullptr != m_tableParts);
  CDE_ASSERT_DEBUG(m_refCount >= 1);
  CDE_ASSERT_DEBUG(m_totParts > 0);

  m_refCount++;
}

/** Decrement share and DStore tables reference counters. */
void HaCdepartShare::decrementRefCounts() {
  CDE_ASSERT_DEBUG(nullptr != m_tableParts);
  CDE_ASSERT_DEBUG(m_refCount >= 1);
  CDE_ASSERT_DEBUG(m_totParts > 0);

  m_refCount--;
}

/** Open InnoDB tables for partitions and return them as array.
@param[in,out]  thd             Thread context
@param[in]      table           MySQL table definition
@param[in]      dd_table        Global DD table object
@param[in]      part_info       Partition info (partition names to use)
@param[in]      table_name      Table name (db/table_name)
@return Array on InnoDB tables on success else nullptr. */
cde_dict_t **HaCdepartShare::openTableParts(
    THD *thd, TABLE *table, const dd::Table *dd_table,
    partition_info *part_info, const char *table_name,
    dstore_handler_t **&dstoreHandlerParts, Memory::MemHeap &memRoot,
    TABLE_SHARE *tableShare) {
  DBUG_TRACE;

  uint totParts = part_info->get_tot_partitions();
  size_t tablePartsSize = sizeof(cde_dict_t *) * totParts;
  cde_dict_t **tableParts =
      static_cast<cde_dict_t **>(CdeZalloc(tablePartsSize));
  if (nullptr == tableParts) {
    return (nullptr);
  }

  dstoreHandlerParts = static_cast<dstore_handler_t **>(
      CdeZalloc(sizeof(dstore_handler_t *) * totParts));

  dd::cache::Dictionary_client *client;
  client = dd::get_dd_client(thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(client);
  uint i = 0;

  for (const auto ddPart : dd_table->leaf_partitions()) {
    std::string partition;
    /* Build the partition name. */
    BuildPartition(ddPart, partition);
    std::string partTable;
    /* Build the partitioned table name. */
    BuildTable("", table_name, partition, false, partTable);
    CDE_ASSERT_DEBUG(partTable.length() < FN_REFLEN);

    if (openOneTablePart(thd, table, ddPart, partTable.c_str(), &tableParts[i],
                         &dstoreHandlerParts[i], memRoot, tableShare,
                         i == 0 ? nullptr : dstoreHandlerParts[0])) {
      CDE_ASSERT_DEBUG(nullptr == tableParts[i]);
      for (uint i = 0; i < totParts; ++i) {
        if (nullptr != dstoreHandlerParts[i])
          dstoreHandlerParts[i]->~dstore_handler_t();
      }
      CdeFree(dstoreHandlerParts);
      CdeFree(tableParts);
      return (nullptr);
    }
    i++;
  }
  CDE_ASSERT_DEBUG(i == totParts);

  return (tableParts);
}

void HaCdepartShare::setTablePartsAndIndexes(partition_info *partInfo,
                                             cde_dict_t **tableParts) {
  m_refCount++;

  /* Check if some other thread has managed to initialize share/open DStore
  tables for partitions concurrently, while LOCK_ha_data was free.
  In such a case table_parts array should point to same cde_dict_t entries
  as one in share, so the array can be simply discarded. There is no need to
  increment reference counters for cde_dict_t entries. */
  if (nullptr != m_tableParts) {
    CDE_ASSERT_DEBUG(m_refCount > 1);
    CDE_ASSERT_DEBUG(m_totParts == partInfo->get_tot_partitions());
    CdeFree(tableParts);
    return;
  }

  CDE_ASSERT_DEBUG(1 == m_refCount);

  m_totParts = partInfo->get_tot_partitions();
  m_tableParts = tableParts;

  CDE_ASSERT_DEBUG(partInfo->table->s->keys >=
                   m_tableParts[0]->index_dict_vec.size());

  return;
}

/** Close the table partitions.
If all instances are closed, also release the resources. */
void HaCdepartShare::closeTableParts() {
  DBUG_TRACE;

  m_refCount--;
  if (m_refCount != 0) return;

  /* Last instance closed, close all table partitions and
  free the memory. */

  if (m_tableParts != nullptr) {
    CdeFree(m_tableParts);
    m_tableParts = nullptr;
  }

  m_totParts = 0;

  /* All table partitions have been closed, autoinc initialization
  should be done again. */
  auto_inc_initialized = false;
}

/** Get explicit specified tablespace for one (sub)partition, checking
from lowest level
@param[in]      tablespace      table-level tablespace if specified
@param[in]      part            Partition to check
@param[in]      sub_part        Sub-partition to check, if no, just NULL
@return Tablespace name, if nullptr or [0] = '\0' then nothing specified */
const char *partitionGetTablespace(const char *tablespace,
                                   const partition_element *part,
                                   const partition_element *sub_part) {
  if (sub_part != nullptr) {
    if (sub_part->tablespace_name != nullptr &&
        sub_part->tablespace_name[0] != '\0') {
      return (sub_part->tablespace_name);
    }
    /* Once DATA DIRECTORY specified, it implies
    non-default tablespace, same as below */
    if (sub_part->data_file_name != nullptr &&
        sub_part->data_file_name[0] != '\0') {
      return (nullptr);
    }
  }

  CDE_ASSERT_DEBUG(nullptr != part);
  if (part->tablespace_name != nullptr && part->tablespace_name[0] != '\0') {
    return (part->tablespace_name);
  }

  if (part->data_file_name != nullptr && part->data_file_name[0] != '\0') {
    return (nullptr);
  }

  return (tablespace);
}

/** Construct ha_innopart handler.
@param[in]      hton            Handlerton.
@param[in]      table_arg       MySQL Table. */
haCdepart::haCdepart(handlerton *hton, TABLE_SHARE *table_arg)
    : ha_cde(hton, table_arg), Partition_helper(this), m_new_partitions() {
  m_int_table_flags &= ~(HA_CDE_DISABLED_TABLE_FLAGS);
}

inline int haCdepart::initialize_auto_increment(bool) {
  THD *thd = ha_thd();
  if (thd->for_ddl_info_replay) return 0;

  /* Since a table can already be "open" in DStore's internal
  data dictionary, we only init the autoinc counter once, the
  first time the table is loaded. We can safely reuse the
  autoinc value from a previous MySQL open. */

  if (m_partShare->auto_inc_initialized) {
    /* Already initialized, nothing to do. */
    return (0);
  }

  const Field *field = table->found_next_number_field;

  /* The caller should make sure that field != nullptr */
  CDE_ASSERT_DEBUG(nullptr != field);

  if (nullptr == field) {
    CDE_LOG_ERROR("Unable to determine the AUTOINC column name");
    my_error(ER_AUTOINC_READ_FAILED, MYF(0));
    return HA_ERR_AUTOINC_READ_FAILED;
  }

  uint64_t maxNextAutoInc = 0;

  if (field != nullptr) {
    for (uint part = 0; part < m_tot_parts; part++) {
      cde_dict_t *cdeTable = m_partShare->getTablePart(part);

      cdeTable->autoinc_dict.MutexEnter();
      CDE_ASSERT_DEBUG(cdeTable->autoinc_dict.GetInitialized());
      maxNextAutoInc =
          std::max(maxNextAutoInc, cdeTable->autoinc_dict.GetAutoinc());
      cdeTable->autoinc_dict.MutexExit();
    }
  }

  m_partShare->next_auto_inc_val = maxNextAutoInc;
  m_partShare->auto_inc_initialized = true;

  return 0;
}

/** Open a DStore table.
@param[in]      name            table name
@param[in]      mode            access mode
@param[in]      test_if_locked  test if the file to be opened is locked
@param[in]      table_def       dd::Table describing table to be opened
@retval 1 if error
@retval 0 if success */
int haCdepart::open(const char *name, int, uint, const dd::Table *table_def) {
  char norm_name[FN_REFLEN];
  THD *thd;

  DBUG_TRACE;
  assert(table_share == table->s);

  thd = ha_thd();
  CdeCreateOrGetSession(thd);

  if (m_part_info == nullptr) {
    /* Must be during ::clone()! */
    CDE_ASSERT_DEBUG(table->part_info != nullptr);
    m_part_info = table->part_info;
  }

  if (CDE_OK != CdeCreateTableInfo::NormalizeTableName(norm_name, name)) {
    CDE_ASSERT_DEBUG(false);
    return HA_ERR_TOO_LONG_PATH;
  }

  /* Get the Ha_innopart_share from the TABLE_SHARE. */
  lock_shared_ha_data();

  m_partShare = static_cast<HaCdepartShare *>(get_ha_share_ptr());
  if (nullptr == m_partShare) {
    m_partShare = new (std::nothrow) HaCdepartShare(table_share);
    if (nullptr == m_partShare) {
      unlock_shared_ha_data();
      return HA_ERR_INTERNAL_ERROR;
    }
    set_ha_share_ptr(static_cast<Handler_share *>(m_partShare));
  }

  if (m_partShare->hasTableParts()) {
    /* If share already has DStore tables open we just need to increment
    reference counters. */
    m_partShare->incrementRefCounts();
    unlock_shared_ha_data();
    auto decrementGuard = create_scope_guard([this] {
      lock_shared_ha_data();
      m_partShare->decrementRefCounts();
      unlock_shared_ha_data();
    });
    int error =
        initDStoreHandlerParts(thd, m_partShare->getTableParts(), m_mem_root);
    if (CDE_OK != error) {
      return error;
    }
    decrementGuard.release();
    lock_shared_ha_data();
    CDE_ASSERT_DEBUG(m_partShare->hasTableParts());
  } else {
    /* We need to open DStore tables and prepare index information.
    Since the former involves access to the data-dictionary we need
    to release TABLE_SHARE::LOCK_ha_data temporarily. */
    unlock_shared_ha_data();

    dstore_handler_t **dstoreHandlerParts;
    cde_dict_t **tableParts = HaCdepartShare::openTableParts(
        thd, table, table_def, m_part_info, norm_name, dstoreHandlerParts,
        m_mem_root, m_partShare->getTableShare());

    if (nullptr == tableParts) return HA_ERR_NO_SUCH_TABLE;

    m_dstoreHandlers = dstoreHandlerParts;
    /* Now acquire TABLE_SHARE::LOCK_ha_data again and assign table
    and index information. setTablePartsAndIndexes() will check
    if some other thread already has managed to do this concurrently,
    while lock was released. */
    lock_shared_ha_data();

    m_partShare->setTablePartsAndIndexes(m_part_info, tableParts);
  }

  /* After this point m_dstoreHandlers has been initialized. If open() exits
  with error, first release TABLE_SHARE::LOCK_ha_data if still held, then
  use close() to cleanup resources. */
  bool openFinished = false;
  bool haShareLocked = true;
  auto autoCleanUp = create_scope_guard([&]() {
    if (openFinished) {
      return;
    }
    if (haShareLocked) {
      unlock_shared_ha_data();
    }
    if (m_dstoreHandlers != nullptr) {
      close();
    }
  });

  if (m_partShare->populate_partition_name_hash(m_part_info)) {
    return HA_ERR_INTERNAL_ERROR;
  }

  if (m_partShare->auto_inc_mutex == nullptr &&
      table->found_next_number_field != nullptr) {
    if (m_partShare->init_auto_inc_mutex(table_share)) {
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  unlock_shared_ha_data();
  haShareLocked = false;

  if (open_partitioning(m_partShare)) {
    return HA_ERR_INITIALIZATION;
  }

  // block size in dstore, used by MySQL in query optimization
  stats.block_size = BLCKSZ;

  if (nullptr != table->found_next_number_field) {
    lock_auto_increment();
    int error = initialize_auto_increment(false);
    unlock_auto_increment();
    if (error != 0) {
      return error;
    }
  }

  ref_length = sizeof(m_dstore->dstore_heap_ctid);
  ref_length += PARTITION_BYTES_IN_POS;

  m_reuseColumnsDecoding = false;

  info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST);

  openFinished = true;
  return 0;
}

/** Clone this handler, used when needing more than one cursor
to the same table.
@param[in]      name            Table name.
@param[in]      mem_root        mem_root to allocate from.
@retval Pointer to clone or NULL if error. */
handler *haCdepart::clone(const char *name, MEM_ROOT *mem_root) {
  DBUG_TRACE;

  haCdepart *new_handler =
      dynamic_cast<haCdepart *>(handler::clone(name, mem_root));
  if (new_handler != nullptr) {
    for (uint i = 0; i < m_tot_parts; ++i) {
      CDE_ASSERT(new_handler->m_dstoreHandlers[i] != nullptr);
      CDE_ASSERT(m_dstoreHandlers[i] != nullptr);
      new_handler->m_dstoreHandlers[i]->select_lock_type =
          m_dstoreHandlers[i]->select_lock_type;
      new_handler->m_dstoreHandlers[i]->select_mode =
          m_dstoreHandlers[i]->select_mode;
    }
  }

  return new_handler;
}

/** Closes a handle to a dstore table.
@return 0 */
int haCdepart::close() {
  DBUG_TRACE;

  close_partitioning();

  CDE_ASSERT_DEBUG(m_partShare != nullptr);
  if (m_partShare != nullptr) {
    lock_shared_ha_data();
    m_partShare->closeTableParts();
    unlock_shared_ha_data();
    m_partShare = nullptr;
  }

  for (uint i = 0; i < m_tot_parts; i++) {
    if (m_dstoreHandlers[i] != nullptr)
      m_dstoreHandlers[i]->~dstore_handler_t();
  }

  CdeFree(m_dstoreHandlers);

  cde_perf_counters::getInstance()->_handler_mem_allocted.decrement(
      m_mem_root.allocated_size());

  m_mem_root.Clear();
  m_blob_mem_root.Clear();

  return 0;
}

int64_t HaCdepartShare::initDStoreHandlerPart(
    cde_dict_t *partTable, dstore_handler_t *dstore, bool forDDLInfoReplay,
    TABLE *table, Memory::MemHeap &memRoot, dstore_handler_t *fistPartDstore) {
  DBUG_TRACE;

  CDE_ASSERT_DEBUG(nullptr != partTable);
  CDE_ASSERT_DEBUG(nullptr != dstore);

  if (!rds_use_ddl_info_replay || !forDDLInfoReplay) {
    auto relInfo = std::make_unique<session_rel_info>(partTable->m_id);
    int64_t ret = CdeCopyRelInfoToLocalStorage(partTable, relInfo.get());
    if (ret != CDE_OK) return ret;

    /** For now the only option is to use local hander storage for relation.
     *  GlobalCache storage is to be implemented. Then the RelationStorage
     *  we create will depend on system variable.
     */
    dstore->rel_info = relInfo.release();
  }

  dstore->dstore_heap_scan = nullptr;
  uint32_t num_columns = partTable->GetTotalCols();
  dstore->table_handler = partTable;
  if (nullptr == fistPartDstore) {
    dstore->m_valuesWithVarlen = static_cast<Datum *>(
        memRoot.Alloc(num_columns * (sizeof(Datum) + sizeof(CdeVarlena))));
    dstore->m_valuesWithoutVarlen = memRoot.ArrayAlloc<Datum>(num_columns);
    dstore->m_nulls = memRoot.ArrayAlloc<bool>(num_columns);
    dstore->m_keyInfos = memRoot.ArrayAlloc<ScanKeyData>(num_columns);
    dstore->m_indexColOids = memRoot.ArrayAlloc<Oid>(num_columns);
    dstore->m_indexColCollationOids = memRoot.ArrayAlloc<Oid>(num_columns);
    dstore->m_scanAgainKeyInfos = memRoot.ArrayAlloc<ScanKeyData>(num_columns);
    dstore->m_columnsToDecode =
        memRoot.ArrayAlloc<MysqlAndDstoreIndex>(table->s->fields);
  } else {
    dstore->m_valuesWithVarlen = fistPartDstore->m_valuesWithVarlen;
    dstore->m_valuesWithoutVarlen = fistPartDstore->m_valuesWithoutVarlen;
    dstore->m_nulls = fistPartDstore->m_nulls;
    dstore->m_keyInfos = fistPartDstore->m_keyInfos;
    dstore->m_indexColOids = fistPartDstore->m_indexColOids;
    dstore->m_indexColCollationOids = fistPartDstore->m_indexColCollationOids;
    dstore->m_scanAgainKeyInfos = fistPartDstore->m_scanAgainKeyInfos;
    dstore->m_columnsToDecode = fistPartDstore->m_columnsToDecode;
  }

  return CDE_OK;
}

int64_t haCdepart::initDStoreHandlerParts(THD *thd, cde_dict_t **tableParts,
                                          Memory::MemHeap &memRoot) {
  DBUG_TRACE;

  uint totParts = m_part_info->get_tot_partitions();
  size_t dstoreHandlerPartsSize = sizeof(dstore_handler_t *) * totParts;
  dstore_handler_t **dstoreHandlers =
      static_cast<dstore_handler_t **>(CdeZalloc(dstoreHandlerPartsSize));
  DBUG_EXECUTE_IF("CdeZalloc_dstore_handler_parts_fail",
                  dstoreHandlers = nullptr;);
  if (!dstoreHandlers) {
    CDE_LOG_ERROR("CdeZalloc fail! alloc nums = %ld", dstoreHandlerPartsSize);
    return CDE_ERROR;
  }

  /** In ha_cde m_dstore is responsible for releasing cde_dict_t in its
  destructor. Here we follow this approach. First we acquire all table
  parts under the dict sys mutex. Then we assign each table part to
  the appropriate dstore object. In dstore's descrutor we release table
  part. */
  DictSysLockGuard lock;
  for (uint i = 0; i < totParts; ++i) {
    dstoreHandlers[i] = new (&memRoot) dstore_handler_t;
    tableParts[i]->acquire();
    dstoreHandlers[i]->table_handler = tableParts[i];
  }
  lock.Clear();

  lock_shared_ha_data();
  auto unlockSharedHaData =
      create_scope_guard([this] { unlock_shared_ha_data(); });
  for (uint i = 0; i < totParts; ++i) {
    cde_dict_t *partTable = tableParts[i];
    CDE_ASSERT_DEBUG(nullptr != partTable);
    dstore_handler_t *dstore = dstoreHandlers[i];
    CDE_ASSERT_DEBUG(nullptr != dstore);

    int64_t ret = HaCdepartShare::initDStoreHandlerPart(
        partTable, dstore, thd->for_ddl_info_replay, table, memRoot,
        i > 0 ? dstoreHandlers[0] : nullptr);
    DBUG_EXECUTE_IF("init_dstore_handler_parts_fail", ret = CDE_FAIL;);
    if (CDE_OK != ret) {
      // release all tables
      DictSysLockGuard lock;
      for (uint j = 0; j < totParts; ++j)
        dstoreHandlers[j]->~dstore_handler_t();
      CdeFree(dstoreHandlers);
      return ret;
    }
  }
  m_dstoreHandlers = dstoreHandlers;
  return CDE_OK;
}

/** Change active partition.
Copies needed info into m_prebuilt from the partition specific memory.
@param[in]      part_id Partition to set as active. */
void haCdepart::setPartition(uint partId) {
  DBUG_TRACE;

  DBUG_PRINT("haCdepart", ("partition id: %u", partId));
  CDE_ASSERT_DEBUG(partId < m_tot_parts);

  m_dstore = getDStorePart(partId);
}

void haCdepart::updatePartition(uint partId) {
  if (m_dstore->sql_stat_start == 0 &&
      m_dstore->keep_other_fields_on_keyread == 1) {
    m_reuseColumnsDecoding = true;
  }
  m_last_part = partId;
}

/** Write a row in specific partition.
Stores a row in an InnoDB database, to the table specified in this
handle.
@param[in]      part_id Partition to write to.
@param[in]      record  A row in MySQL format.
@return error code. */
int haCdepart::write_row_in_part(uint part_id, uchar *record) {
  int error;
  DBUG_TRACE;

  setPartition(part_id);

  m_skipUpdateAutoIncrement = true;
  error = ha_cde::write_row(record);
  m_skipUpdateAutoIncrement = false;
  updatePartition(part_id);
  return error;
}

/** Update a row in partition.
Updates a row given as a parameter to a new value.
@param[in]      part_id Partition to update row in.
@param[in]      old_row Old row in MySQL format.
@param[in]      new_row New row in MySQL format.
@return 0 or error number. */
int haCdepart::update_row_in_part(uint part_id, const uchar *old_row,
                                  uchar *new_row) {
  int error;
  DBUG_TRACE;

  setPartition(part_id);
  error = ha_cde::update_row(old_row, new_row);
  updatePartition(part_id);
  return error;
}

/** Deletes a row in partition.
@param[in]      part_id Partition to delete from.
@param[in]      record  Row to delete in MySQL format.
@return 0 or error number. */
int haCdepart::delete_row_in_part(uint partId, const uchar *record) {
  int error;
  DBUG_TRACE;
  m_err_rec = nullptr;

  m_last_part = partId;
  setPartition(partId);
  error = ha_cde::delete_row(record);
  updatePartition(partId);
  return error;
}

/**
MySQL calls this method at the end of each statement

@return 0 if success, other values if fail.
*/
int haCdepart::reset() {
  m_ds_mrr.reset();
  m_blob_mem_root.Clear();
  m_cond_handler.CleanUpResources();
  pushed_cond = nullptr;
  cde_session_t *session =
      *(cde_session_t **)thd_ha_data(ha_thd(), cde_hton_ptr);

  for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    m_dstoreHandlers[i]->keep_other_fields_on_keyread = 0;
    m_dstoreHandlers[i]->read_just_key = 0;
    m_dstoreHandlers[i]->autoincCtx.m_lastVal = 0;
    m_dstoreHandlers[i]->autoincCtx.m_noAutoincLocking = false;
    if (session) {
      session->get_trxinfo()->removeHeapScanHandler(
          &m_dstoreHandlers[i]->dstore_heap_scan);
      if (nullptr != m_dstoreHandlers[i]->dstore_heap_scan) {
        HeapInterface::EndScan(m_dstoreHandlers[i]->dstore_heap_scan);
        HeapInterface::DestroyHeapScanHandler(
            m_dstoreHandlers[i]->dstore_heap_scan);
        m_dstoreHandlers[i]->dstore_heap_scan = nullptr;
      }
    }
  }
  m_pushed_offset = m_scanned_offset = 0;
  m_pushed_aggregate = nullptr;
  return 0;
}

/** Initializes a handle to use an index.
@param[in]      keynr   Key (index) number.
@param[in]      sorted  True if result MUST be sorted according to index.
@return 0 or error number. */
int haCdepart::index_init(uint keynr, bool sorted) {
  uint partId = m_part_info->get_first_used_partition();
  DBUG_TRACE;

  active_index = keynr;
  if (partId == MY_BIT_NONE) {
    return 0;
  }

  int error = ph_index_init_setup(keynr, sorted);
  if (error != 0) {
    return error;
  }

  if (sorted) {
    error = init_record_priority_queue();
    if (error != 0) {
      /* Needs cleanup in case it returns error. */
      destroy_record_priority_queue();
      return error;
    }
  }

  DBUG_EXECUTE_IF("partition_fail_index_init", {
    destroy_record_priority_queue();
    return HA_ERR_NO_PARTITION_FOUND;
  });

  for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    setPartition(i);
    error = ha_cde::index_init(keynr, sorted);
    if (error != 0) {
      destroy_record_priority_queue();
      return error;
    }
  }

  return error;
}

/** End index cursor.
@return 0 or error code. */
int haCdepart::index_end() {
  uint partId = m_part_info->get_first_used_partition();
  DBUG_TRACE;

  if (partId == MY_BIT_NONE) {
    /* Never initialized any index. */
    active_index = MAX_KEY;
    return 0;
  }
  if (m_ordered) {
    destroy_record_priority_queue();
  }

  for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    setPartition(i);
    int error = ha_cde::index_end();
    if (error != 0) {
      return error;
    }
  }

  return 0;
}

/** Print error information.
@param[in]      error   Error code (MySQL).
@param[in]      errflag Flags. */
void haCdepart::print_error(int error, myf errflag) {
  DBUG_TRACE;
  if (print_partition_error(error)) {
    ha_cde::print_error(error, errflag);
  }
}

/** Can error be ignored.
@param[in]      error   Error code to check.
@return true if ignorable else false. */
bool haCdepart::is_ignorable_error(int error) {
  if (ha_cde::is_ignorable_error(error) || error == HA_ERR_NO_PARTITION_FOUND ||
      error == HA_ERR_NOT_IN_LOCK_PARTITIONS) {
    return (true);
  }
  return (false);
}

/** Return first record in index from a partition.
@param[in]      part    Partition to read from.
@param[out]     record  First record in index in the partition.
@return error number or 0. */
int haCdepart::index_first_in_part(uint partId, uchar *record) {
  DBUG_TRACE;

  setPartition(partId);
  int error = ha_cde::index_first(record);
  updatePartition(partId);
  return error;
}

/** Return next record in index from a partition.
@param[in]      part    Partition to read from.
@param[out]     record  Last record in index in the partition.
@return error number or 0. */
int haCdepart::index_next_in_part(uint partId, uchar *record) {
  DBUG_TRACE;

  setPartition(partId);
  int error = ha_cde::index_next(record);
  updatePartition(partId);
  return error;
}

/** Return next same record in index from a partition.
This routine is used to read the next record, but only if the key is
the same as supplied in the call.
@param[in]      part    Partition to read from.
@param[out]     record  Last record in index in the partition.
@param[in]      key     Key to match.
@param[in]      length  Length of key.
@return error number or 0. */
int haCdepart::index_next_same_in_part(uint partId, uchar *record,
                                       const uchar *key, uint length) {
  DBUG_TRACE;

  setPartition(partId);
  int error = ha_cde::index_next_same(record, key, length);
  updatePartition(partId);
  return error;
}

/** Return last record in index from a partition.
@param[in]      part    Partition to read from.
@param[out]     record  Last record in index in the partition.
@return error number or 0. */
int haCdepart::index_last_in_part(uint partId, uchar *record) {
  DBUG_TRACE;

  setPartition(partId);
  int error = ha_cde::index_last(record);
  updatePartition(partId);
  return error;
}

/** Return previous record in index from a partition.
@param[in]      part    Partition to read from.
@param[out]     record  Last record in index in the partition.
@return error number or 0. */
int haCdepart::index_prev_in_part(uint partId, uchar *record) {
  DBUG_TRACE;

  setPartition(partId);
  int error = ha_cde::index_next(record);
  updatePartition(partId);
  return error;
}

/** Start index scan and return first record from a partition.
This routine starts an index scan using a start key. The calling
function will check the end key on its own.
@param[in]      part            Partition to read from.
@param[out]     record          First matching record in index in the partition.
@param[in]      key             Key to match.
@param[in]      keypart_map     Which part of the key to use.
@param[in]      find_flag       Key condition/direction to use.
@return error number or 0. */
int haCdepart::index_read_map_in_part(uint partId, uchar *record,
                                      const uchar *key,
                                      key_part_map keypart_map,
                                      enum ha_rkey_function find_flag) {
  DBUG_TRACE;

  setPartition(partId);
  int error = ha_cde::index_read_map(record, key, keypart_map, find_flag);
  updatePartition(partId);
  return error;
}

/** Start index scan and return first record from a partition.
This routine starts an index scan using a start key. The calling
function will check the end key on its own.
@param[in]      part            Partition to read from.
@param[out]     record          First matching record in index in the partition.
@param[in]      index           Index to read from.
@param[in]      key             Key to match.
@param[in]      keypart_map     Which part of the key to use.
@param[in]      find_flag       Key condition/direction to use.
@return error number or 0. */
int haCdepart::index_read_idx_map_in_part(uint partId, uchar *record,
                                          uint index, const uchar *key,
                                          key_part_map keypart_map,
                                          enum ha_rkey_function find_flag) {
  DBUG_TRACE;

  setPartition(partId);
  int error =
      ha_cde::index_read_idx_map(record, index, key, keypart_map, find_flag);
  updatePartition(partId);
  return error;
}

/** Setup the ordered record buffer and the priority queue.
@param[in]    used_parts      Number of used partitions in query.
@return false for success, else true. */
int haCdepart::init_record_priority_queue_for_parts(uint) {
  DBUG_TRACE;
  m_allowBlobMemRootClearForReuse = false;
  return false;
}

/** Destroy the ordered record buffer and the priority queue. */
void haCdepart::destroy_record_priority_queue_for_parts() {
  DBUG_TRACE;
  m_allowBlobMemRootClearForReuse = true;
}

/** Return last matching record in index from a partition.
@param[in]      part            Partition to read from.
@param[out]     record          Last matching record in index in the partition.
@param[in]      key             Key to match.
@param[in]      keypart_map     Which part of the key to use.
@return error number or 0. */
int haCdepart::index_read_last_map_in_part(uint partId, uchar *record,
                                           const uchar *key,
                                           key_part_map keypart_map) {
  DBUG_TRACE;
  setPartition(partId);
  int error = ha_cde::index_read_last_map(record, key, keypart_map);
  updatePartition(partId);
  return error;
}

int CdeKeypartMapToN(key_part_map keypart_map, int keypart_max);

int haCdepart::read_range_first_in_part(uint part, uchar *record,
                                        const key_range *, const key_range *,
                                        bool) {
  DBUG_TRACE;
  int error;
  uchar *read_record = record;
  setPartition(part);
  if (read_record == nullptr) {
    read_record = table->record[0];
  }
  if (m_start_key.key != nullptr) {
    m_dstore->key_map = m_start_key.keypart_map;
    m_dstore->keys_prefix_counts = CdeKeypartMapToN(
        m_start_key.keypart_map,
        m_dstore->current_cde_index->shared_dict->index_col_num);
    error = ha_cde::index_read(read_record, m_start_key.key, m_start_key.length,
                               m_start_key.flag);
  } else {
    m_dstore->key_map = 0;
    m_dstore->keys_prefix_counts = 0;
    error = ha_cde::index_first(read_record);
  }
  if (error == HA_ERR_KEY_NOT_FOUND) {
    error = HA_ERR_END_OF_FILE;
  } else if (error == 0 && !in_range_check_pushed_down) {
    /* compare_key uses table->record[0], so we
    need to copy the data if not already there. */
    if (record != nullptr) {
      copy_cached_row(table->record[0], read_record);
    }
    if (compare_key(end_range) > 0) {
      error = HA_ERR_END_OF_FILE;
    }
  }
  updatePartition(part);
  return (error);
}

/** Return next record in index range scan from a partition.
@param[in]      part    Partition to read from.
@param[in,out]  record  First matching record in index in the partition,
if NULL use table->record[0] as return buffer.
@return error number or 0. */
int haCdepart::read_range_next_in_part(uint part, uchar *record) {
  DBUG_TRACE;
  int error;
  uchar *read_record = record;

  setPartition(part);
  if (read_record == nullptr) {
    read_record = table->record[0];
  }

  error = ha_cde::index_next(read_record);
  if (error == 0 && !in_range_check_pushed_down) {
    /* compare_key uses table->record[0], so we
    need to copy the data if not already there. */
    if (record != nullptr) {
      copy_cached_row(table->record[0], read_record);
    }
    if (compare_key(end_range) > 0) {
      error = HA_ERR_END_OF_FILE;
    }
  }
  updatePartition(part);

  return (error);
}

struct PartsSampleScanContext {
  std::vector<std::unique_ptr<DSTORE::HeapSampleScanContext>> contexts;
  std::vector<std::unique_ptr<CDESampler>> samplerContexts;
  uint currentPartition{0};
  uint partitionProcessed{0};
};

using PartsSampleScanContextUniquePtr = std::unique_ptr<PartsSampleScanContext>;

int haCdepart::sample_init(void *&scan_ctx, double sampling_percentage,
                           int sampling_seed,
                           enum_sampling_method sampling_method,
                           const bool tablesample) {
  PartsSampleScanContextUniquePtr sampleScanContextsPtr =
      std::make_unique<PartsSampleScanContext>();
  sampleScanContextsPtr->currentPartition =
      m_part_info->get_first_used_partition();

  for (auto i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    setPartition(i);
    void *scanContext;
    int error =
        ha_cde::sample_init(scanContext, sampling_percentage, sampling_seed,
                            sampling_method, tablesample);
    if (CDE_OK != error) return error;
    sampleScanContextsPtr->contexts.push_back(
        std::unique_ptr<DSTORE::HeapSampleScanContext>(
            static_cast<DSTORE::HeapSampleScanContext *>(scanContext)));
    sampleScanContextsPtr->samplerContexts.push_back(std::move(m_samplerCtx));
  }
  CDE_ASSERT_DEBUG(sampleScanContextsPtr->contexts.size() ==
                   sampleScanContextsPtr->samplerContexts.size());
  scan_ctx = static_cast<void *>(sampleScanContextsPtr.release());
  CDE_ASSERT_DEBUG(nullptr == m_samplerCtx);  // it was already moved from
  return 0;
}

int haCdepart::sample_next(void *scan_ctx, uchar *buf) {
  PartsSampleScanContext *partsContext{
      static_cast<PartsSampleScanContext *>(scan_ctx)};
  int error = CDE_OK;

  if (partsContext->currentPartition == m_tot_parts)
    return HA_ERR_END_OF_FILE;  // we have run out of partitions

  for (auto i = partsContext->currentPartition; i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    partsContext->currentPartition = i;
    setPartition(i);
    CDE_ASSERT_DEBUG(partsContext->partitionProcessed <
                     partsContext->samplerContexts.size());
    if (m_samplerCtx == nullptr)
      m_samplerCtx = std::move(
          partsContext->samplerContexts[partsContext->partitionProcessed]);
    error = ha_cde::sample_next(
        partsContext->contexts[partsContext->partitionProcessed].get(), buf);
    if (HA_ERR_END_OF_FILE != error) return error;
    // we are going to change partition
    m_samplerCtx = nullptr;
    ++partsContext->partitionProcessed;
  }
  return error;
}

int haCdepart::sample_end(void *scan_ctx) {
  DBUG_TRACE;
  PartsSampleScanContextUniquePtr partsContext{
      static_cast<PartsSampleScanContext *>(scan_ctx)};

  uint partitionContextIndex = 0;
  m_samplerCtx.reset();

  for (auto i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    setPartition(i);
    ha_cde::sample_end(
        partsContext->contexts[partitionContextIndex++].release());
  }
  return 0;
}

/** Initialize random read/scan of a specific partition.
@param[in]      part_id         Partition to initialize.
@param[in]      scan            True for scan else random access.
@return error number or 0. */
int haCdepart::rnd_init_in_part(uint partId, bool scan) {
  DBUG_TRACE;
  setPartition(partId);
  return ha_cde::rnd_init(scan);
}

int haCdepart::rnd_end_in_part(uint partId, bool) {
  DBUG_TRACE;
  setPartition(partId);
  return ha_cde::rnd_end();
}

/** Get next row during scan of a specific partition.
Also used to read the FIRST row in a table scan.
@param[in]      part_id Partition to read from.
@param[out]     buf     Next row.
@return error number or 0. */
int haCdepart::rnd_next_in_part(uint partId, uchar *buf) {
  DBUG_TRACE;

  setPartition(partId);
  int error = ha_cde::rnd_next(buf);
  updatePartition(partId);
  return error;
}

/** Get a row from a position.
Fetches a row from the table based on a row reference.
@param[out]     buf     Returns the row in this buffer, in MySQL format.
@param[in]      pos     Position, given as primary key value or DB_ROW_ID
(if no primary key) of the row in MySQL format.  The length of data in pos has
to be ref_length.
@return 0, HA_ERR_KEY_NOT_FOUND or error code. */
int haCdepart::rnd_pos(uchar *buf, uchar *pos) {
  DBUG_TRACE;
  static_assert(PARTITION_BYTES_IN_POS == 2);
  DBUG_DUMP("pos", pos, ref_length);

  ha_statistic_increment(&System_status_var::ha_read_rnd_count);

  /* Restore used partition. */
  uint partId = uint2korr(pos);

  setPartition(partId);

  int error = ha_cde::rnd_pos(buf, pos + PARTITION_BYTES_IN_POS);
  updatePartition(partId);

  return error;
}

/** Return position for cursor in last used partition.
Stores a reference to the current row to 'ref' field of the handle. Note
that in the case where we have generated the clustered index for the
table, the function parameter is illogical: we MUST ASSUME that 'record'
is the current 'position' of the handle, because if row ref is actually
the row id internally generated in InnoDB, then 'record' does not contain
it. We just guess that the row id must be for the record where the handle
was positioned the last time.
@param[out]     ref_arg Pointer to buffer where to write the position.
@param[in]      record  Record to position for. */
void haCdepart::position_in_last_part(uchar *ref_arg, const uchar *record) {
  DBUG_TRACE;
  CDE_ASSERT_DEBUG(m_last_part < m_tot_parts);
  CDE_ASSERT((table->file->ha_table_flags() & HA_PRIMARY_KEY_IN_READ_INDEX) ==
             false);
  uint len = sizeof(DSTORE::ItemPointer);
  errno_t err = memcpy_s(ref_arg, len,
                         &m_dstoreHandlers[m_last_part]->dstore_heap_ctid, len);
  CDE_ASSERT(err == 0);
  /* We assume that the 'ref' value len is always fixed for the same table. */
  if (len + PARTITION_BYTES_IN_POS != ref_length) {
    CDE_LOG_ERROR("len + 2 and ref_length should be same: %d,  %d ", len,
                  ref_length);
  }
  (void)record;
}

/** Check partition's tablespace is same with table's tablespace name/
@param[in]      partitionTableSpace Name of the partition's tablespace
@param[in]      tableSpace Name of the table's tablespace
@return true if equal. */
static inline bool isSameTableSpace(const char *partitionTableSpace,
                                    const char *tableSpace) {
  if (partitionTableSpace == nullptr || partitionTableSpace == tableSpace ||
      (strcmp(partitionTableSpace, tableSpace) == 0)) {
    return true;
  }

  return false;
}

/** Creates a new table in DStore database.
@param[in]      name            Table name (in filesystem charset).
@param[in]      form            MySQL Table containing information of
partitions, columns and indexes etc.
@param[in]      create_info     Additional create information, like
create statement string.
@param[in,out]  table_def       dd::Table object for table to be created.
Can be adjusted by this call. Changes to the table definition will be
persisted in the data-dictionary at statement commit time.
@return 0 or error number. */
int haCdepart::create(const char *name, TABLE *form, HA_CREATE_INFO *createInfo,
                      dd::Table *tableDef) {
  int error;
  char table_data_file_name[FN_REFLEN];
  char table_level_tablespace_name[NAME_LEN + 1] = {'\0'};
  const char *table_index_file_name;
  uint created = 0;
  THD *thd = ha_thd();
  DBUG_TRACE;

  CdeTrxStartIfNotStarted(thd, true);

  if (thd_sql_command(thd) == SQLCOM_TRUNCATE) {
    return CdeTruncatePartitions(name, form, tableDef);
  }

  CdeCreateTableInfo info(thd, name, form, createInfo, tableDef);

  CDE_ASSERT_DEBUG(m_part_info == form->part_info);
  CDE_ASSERT_DEBUG(table_share != nullptr);

  /* Not allowed to create temporary partitioned tables. */
  if ((createInfo->options & HA_LEX_CREATE_TMP_TABLE) != 0) {
    my_error(ER_PARTITION_NO_TEMPORARY, MYF(0));
    CDE_ASSERT_DEBUG(false);
    return HA_ERR_INTERNAL_ERROR;
  }

  CdeTrxStartIfNotStarted(thd, true);

  /* Setup and check table level options. */
  error = info.PrepareCreateTable(name, true);
  if (error != 0) {
    return error;
  }

  /* Save the original table name before adding partition information. */
  const std::string savedTableName(name);

  table_index_file_name = createInfo->index_file_name;

  table_data_file_name[0] = '\0';

  if (createInfo->tablespace != nullptr) {
    CDE_ASSERT(strcpy_s(table_level_tablespace_name, NAME_LEN + 1,
                        createInfo->tablespace) == EOK);
  } else {
    CDE_ASSERT(strcpy_s(table_level_tablespace_name, NAME_LEN + 1,
                        dstore_default_spacename) == EOK);
  }

  /* It's also doable to get tablespace names by accessing
  dd::Tablespace::name according to dd_part->tablespace_id().
  However, it costs more. So as long as partition_element contains
  tablespace name, it's easier to check it */
  std::vector<const char *> tablespace_names;
  List_iterator_fast<partition_element> part_it(form->part_info->partitions);
  partition_element *part_elem;
  for (part_elem = part_it++; part_elem != nullptr; part_elem = part_it++) {
    const char *tablespace;
    if (form->part_info->is_sub_partitioned()) {
      List_iterator_fast<partition_element> sub_it(part_elem->subpartitions);
      partition_element *sub_elem;
      for (sub_elem = sub_it++; sub_elem != nullptr; sub_elem = sub_it++) {
        tablespace = partitionGetTablespace(table_level_tablespace_name,
                                            part_elem, sub_elem);
        if (!isSameTableSpace(tablespace, table_level_tablespace_name)) {
          tablespace_names.clear();
          error = HA_ERR_INTERNAL_ERROR;
          break;
        }
        tablespace_names.push_back(tablespace);
      }
    } else {
      tablespace = partitionGetTablespace(table_level_tablespace_name,
                                          part_elem, nullptr);
      if (!isSameTableSpace(tablespace, table_level_tablespace_name)) {
        tablespace_names.clear();
        error = HA_ERR_INTERNAL_ERROR;
        break;
      }
      tablespace_names.push_back(tablespace);
    }
  }

  if (error) {
    my_printf_error(ER_ILLEGAL_HA_CREATE_OPTION, PARTITION_IN_DIFF_TABLESPACE,
                    MYF(0));
    return error;
  }

  for (const auto ddPart : *tableDef->leaf_partitions()) {
    std::string partition;
    /* Build the partition name. */
    BuildPartition(ddPart, partition);

    std::string partTable;
    /* Build the partitioned table name. */
    BuildTable("", savedTableName, partition, false, partTable);

    if (partTable.length() + 1 >= FN_REFLEN - 1) {
      error = HA_ERR_INTERNAL_ERROR;
      my_error(ER_PATH_LENGTH, MYF(0), partTable.c_str());
      break;
    }

    dd::String_type index_file_name;
    dd::String_type data_file_name;

    createInfo->data_file_name = nullptr;
    createInfo->index_file_name = nullptr;
    createInfo->tablespace = table_level_tablespace_name;

    CdeCreateTableInfo info(thd, name, form, createInfo, tableDef);

    if ((error = info.PrepareCreateTable(partTable.c_str(), true)) != 0) {
      break;
    }

    if ((error = info.CreateTable()) != 0) {
      break;
    }

    if ((error = info.CreateTableUpdateDict<dd::Partition>(
             const_cast<dd::Partition *>(ddPart))) != 0) {
      break;
    }

    ++created;
    createInfo->data_file_name = table_data_file_name;
    createInfo->index_file_name = table_index_file_name;
  }

  createInfo->data_file_name = nullptr;
  createInfo->index_file_name = nullptr;
  createInfo->tablespace = nullptr;

  return error;
}

/** Drop a table.
@param[in]      name            table name
@param[in,out]  dd_table        data dictionary table
@return error number
@retval 0 on success */
int haCdepart::delete_table(const char *name, const dd::Table *table_def) {
  DBUG_TRACE;
  THD *thd = ha_thd();
  CdeCreateOrGetSession(thd);

  CDE_ASSERT_DEBUG(table_def != nullptr);
  CDE_ASSERT_DEBUG(table_def->partition_type() != dd::Table::PT_NONE);
  CDE_ASSERT_DEBUG(table_def->is_persistent());

  CdeTrxStartIfNotStarted(thd, true);

  char norm_name[FN_REFLEN];

  int error = CdeCreateTableInfo::NormalizeTableName(norm_name, name);
  if (error) return error;

  TABLE_SHARE ts;
  TABLE td;
  error = CdeAcquireUncacheTable(thd, table_def, norm_name, &ts, &td);
  if (error != 0) {
    return (error);
  }

  for (const dd::Partition *ddPart : table_def->leaf_partitions()) {
    std::string partition;
    /* Build the partition name. */
    BuildPartition(ddPart, partition);

    std::string partTable;
    /* Build the partitioned table name. */
    BuildTable("", norm_name, partition, false, partTable);

    if (partTable.length() >= FN_REFLEN) {
      CdeReleaseUncachedTable(&ts, &td);
      return HA_ERR_INTERNAL_ERROR;
    }

    const char *partitionName = partTable.c_str();

    if (thd->for_ddl_info_replay) {
      DictSysRemoveTable(partitionName);
      error = 0;
    }
    error = CdeDropTable(thd, partitionName, ddPart);

    if (error != 0) {
      break;
    }
    DstoreDictStatDropTable(partitionName, thd);
  }
  CdeReleaseUncachedTable(&ts, &td);
  return error;
}

/** Rename a table.
@param[in]      from            table name before rename
@param[in]      to              table name after rename
@param[in]      from_table      data dictionary table before rename
@param[in,out]  to_table        data dictionary table after rename
@return error number
@retval 0 on success */
int haCdepart::rename_table(const char *from, const char *to,
                            const dd::Table *fromTable, dd::Table *toTable) {
  THD *thd = ha_thd();
  char normFrom[FN_REFLEN];
  char normTo[FN_REFLEN];
  int error = 0;

  DBUG_TRACE;

  CDE_ASSERT_DEBUG(fromTable != nullptr);
  CDE_ASSERT_DEBUG(toTable != nullptr);
  CDE_ASSERT_DEBUG(fromTable->se_private_id() == toTable->se_private_id());
  CDE_ASSERT_DEBUG(fromTable->se_private_data().raw_string() ==
                   toTable->se_private_data().raw_string());
  CDE_ASSERT_DEBUG(fromTable->partition_type() == toTable->partition_type());

  if (CDE_OK != CdeCreateTableInfo::NormalizeTableName(normFrom, from) ||
      CDE_OK != CdeCreateTableInfo::NormalizeTableName(normTo, to))
    return HA_ERR_TOO_LONG_PATH;

  /* Get the transaction associated with the current thd, or create one
  if not yet created */
  CdeTrxStartIfNotStarted(thd, true);

  auto toPart = toTable->leaf_partitions()->begin();

  for (const auto fromPart : fromTable->leaf_partitions()) {
    CDE_ASSERT_DEBUG((*toPart) != nullptr);

    std::string partition;
    /* Build the old partition name. */
    BuildPartition(fromPart, partition);

    /* Build the old partitioned table name. */
    std::string fromName;
    BuildTable("", normFrom, partition, false, fromName);

    /* Build the new partition name. */
    BuildPartition(*toPart, partition);

    /* Build the new partitioned table name. */
    std::string toName;
    BuildTable("", normTo, partition, false, toName);

    if (fromName.length() >= FN_REFLEN || toName.length() >= FN_REFLEN) {
      CDE_ASSERT_DEBUG(false);
      return HA_ERR_INTERNAL_ERROR;
    }

    cde_dict_t *dictFromPart = DictSysGetTable(fromName.c_str());
    if (nullptr == dictFromPart) {
      dictFromPart = CdeDdOpenTableOnDdObj(thd, fromName.c_str(), fromPart);
      if (nullptr == dictFromPart) {
        return HA_ERR_NO_SUCH_TABLE;
      }
    }
    DictTableRefGuard dictRefGuard(dictFromPart);
    error = CdeRenameTable(thd, fromName.c_str(), toName.c_str(), dictFromPart,
                           toTable);
    if (error != 0) {
      CDE_ASSERT_DEBUG(false);
      break;
    }

    if (!CdeIsMysqlTmpTableName(from) && !CdeIsMysqlTmpTableName(to)) {
      (void)DstoreDictStatRenameTable(thd, fromName.c_str(), toName.c_str());
    }

    ++toPart;
  }
  return error;
}

/**
When the table name is changed, the statistics table is refreshed.
This function is currently used in the rename copy algorithm scenario.
After the DDL rename operation is complete, the statistics name is changed.

@param[in]      from            table name before rename
@param[in]      to              table name after rename
@param[in]      from_table      data dictionary table before rename
@param[in,out]  to_table        data dictionary table after rename
@retval 0 on success */
int haCdepart::post_rename_table_statistics(const char *from, const char *to,
                                            const dd::Table *fromTable,
                                            const dd::Table *toTable) {
  THD *thd = ha_thd();
  char normFrom[FN_REFLEN];
  char normTo[FN_REFLEN];
  int error = 0;

  DBUG_TRACE;

  CDE_ASSERT_DEBUG(fromTable != nullptr);
  CDE_ASSERT_DEBUG(toTable != nullptr);
  CDE_ASSERT_DEBUG(fromTable->se_private_id() == toTable->se_private_id());
  CDE_ASSERT_DEBUG(fromTable->se_private_data().raw_string() ==
                   toTable->se_private_data().raw_string());
  CDE_ASSERT_DEBUG(fromTable->partition_type() == toTable->partition_type());

  if (CDE_OK != CdeCreateTableInfo::NormalizeTableName(normFrom, from) ||
      CDE_OK != CdeCreateTableInfo::NormalizeTableName(normTo, to))
    return HA_ERR_TOO_LONG_PATH;

  /* Get the transaction associated with the current thd, or create one
  if not yet created */
  CdeTrxStartIfNotStarted(thd, true);

  auto toPart = toTable->leaf_partitions().begin();

  for (const auto fromPart : fromTable->leaf_partitions()) {
    CDE_ASSERT_DEBUG((*toPart) != nullptr);

    std::string partition;
    /* Build the old partition name. */
    BuildPartition(fromPart, partition);

    /* Build the old partitioned table name. */
    std::string fromName;
    BuildTable("", normFrom, partition, false, fromName);

    /* Build the new partition name. */
    BuildPartition(*toPart, partition);

    /* Build the new partitioned table name. */
    std::string toName;
    BuildTable("", normTo, partition, false, toName);

    if (fromName.length() >= FN_REFLEN || toName.length() >= FN_REFLEN) {
      CDE_ASSERT_DEBUG(false);
      return HA_ERR_INTERNAL_ERROR;
    }

    (void)ha_cde::post_rename_table_statistics(fromName.c_str(), toName.c_str(),
                                               fromTable, toTable);

    ++toPart;
  }
  return error;
}

/** Compare key and rowid.
Helper function for sorting records in the priority queue.
a/b points to table->record[0] rows which must have the
key fields set. The bytes before a and b store the rowid.
This is used for comparing/sorting rows first according to
KEY and if same KEY, by rowid (ref).
@param[in]      key_info        Null terminated array of index information.
@param[in]      a               Pointer to record+ref in first record.
@param[in]      b               Pointer to record+ref in second record.
@return Return value is SIGN(first_rec - second_rec)
@retval 0       Keys are equal.
@retval -1      second_rec is greater than first_rec.
@retval +1      first_rec is greater than second_rec. */
int haCdepart::keyAndRowidCmp(KEY **key_info, uchar *a, uchar *b) {
  int cmp = key_rec_cmp(key_info, a, b);
  if (cmp != 0) {
    return (cmp);
  }
  /* We must compare by rowid, which is added before the record,
  in the priority queue. */
  auto refLength = sizeof(m_dstore->dstore_heap_ctid);
  return ha_cde::cmpRef(a - refLength, b - refLength);
}

/** Extra hints from MySQL.
@param[in]      operation       Operation hint.
@return 0 or error number. */
int haCdepart::extra(enum ha_extra_function operation) {
  if (operation == HA_EXTRA_SECONDARY_SORT_ROWID) {
    /* index_init(sorted=true) must have been called! */
    if (m_part_info->num_partitions_used() != 0) {
      CDE_ASSERT_DEBUG(m_ordered);
      CDE_ASSERT_DEBUG(m_ordered_rec_buffer != nullptr);
      /* No index_read call must have been done! */
      CDE_ASSERT_DEBUG(m_queue->empty());

      /* If not PK is set as secondary sort, do secondary sort by
      rowid/ref. */

      if (m_curr_key_info[1] == nullptr) {
        m_ref_usage = Partition_helper::REF_USED_FOR_SORT;
        m_queue->m_fun = keyAndRowidCmp;
      }
    }
    return (0);
  }

  if (operation == HA_EXTRA_BEGIN_ALTER_COPY ||
      operation == HA_EXTRA_END_ALTER_COPY) {
    return (0);
  }

  for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    setPartition(i);
    int error = ha_cde::extra(operation);
    if (0 != error) return error;
  }
  return 0;
}

/* Get partition row type
@param[in] partition_table partition table
@param[in] part_id Id of partition for which row type to be retrieved
@return Partition row type. */
enum row_type haCdepart::get_partition_row_type(const dd::Table *, uint) {
  return table_share->real_row_type;
}

int haCdepart::CdeTruncatePartitions(const char *name, TABLE *form,
                                     dd::Table *table_def) {
  (void)form;
  DBUG_TRACE;

  CDE_ASSERT_DEBUG(table_def != nullptr);
  CDE_ASSERT_DEBUG(table_def->partition_type() != dd::Table::PT_NONE);
  CDE_ASSERT_DEBUG(table_def->is_persistent());
  int32_t error = 0;

  THD *thd = ha_thd();
  CdeTrxStartIfNotStarted(thd, true);

  for (const auto ddPart : *table_def->leaf_partitions()) {
    char norm_name[FN_REFLEN];

    std::string partition;
    /* Build the partition name. */
    BuildPartition(ddPart, partition);

    std::string partition_name;
    /* Build the partitioned table name. */
    BuildTable("", name, partition, false, partition_name);
    CDE_ASSERT_DEBUG(partition_name.length() < FN_REFLEN);

    if (CDE_OK != CdeCreateTableInfo::NormalizeTableName(
                      norm_name, partition_name.c_str())) {
      return HA_ERR_TOO_LONG_PATH;
    }

    error = CdeTruncateTable(thd, norm_name, ddPart);

    if (error != 0) {
      return error;
    }
  }

  CDE_ASSERT_DEBUG(0 == error);

  /** after truncation all parts are going to be closed and
  auto_inc_initialized will be set to false in closeTableParts
  which will force auto inc to be reloaded to the higest remain autoinc
  or auto inc from DD or 0. */

  return error;
}

/** Delete all rows in the requested partitions.
Done by deleting the partitions and recreate them again.
@param[in,out]  dd_table        dd::Table object for partitioned table
which partitions need to be truncated. Can be adjusted by this call.
Changes to the table definition will be persisted in the data-dictionary
at statement commit time.
@return 0 or error number. */
// int ha_innopart::truncate_partition_low(dd::Table *dd_table) {
int haCdepart::truncate_partition_low(dd::Table *ddTable) {
  int error = 0;
  const char *tableName = table->s->normalized_path.str;
  THD *thd = ha_thd();
  uint partNum = 0;
  DBUG_TRACE;

  CdeTrxStartIfNotStarted(thd, true);

  for (const auto ddPart : *ddTable->leaf_partitions()) {
    if (!m_part_info->is_partition_used(partNum++)) continue;

    char normName[FN_REFLEN];

    std::string partition;
    /* Build the partition name. */
    BuildPartition(ddPart, partition);

    std::string partitionName;

    /* Build the partitioned table name. */
    BuildTable("", tableName, partition, false, partitionName);
    CDE_ASSERT_DEBUG(partitionName.length() < FN_REFLEN);

    if (CDE_OK != CdeCreateTableInfo::NormalizeTableName(
                      normName, partitionName.c_str())) {
      return HA_ERR_TOO_LONG_PATH;
    }

    error = CdeTruncateTable(thd, normName, ddPart);

    if (error != 0) return error;
  }

  CDE_ASSERT_DEBUG(0 == error);
  return CDE_OK;
}

/** Total number of rows in all used partitions.
Returns the exact number of records that this client can see using this
handler object.
@param[out]     num_rows        Number of rows.
@return 0 or error number. */
int haCdepart::records(ha_rows *num_rows) {
  DBUG_TRACE;

  if (ha_table_flags() & HA_COUNT_ROWS_INSTANT) {
    *num_rows = stats.records;
    return 0;
  }

  for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    setPartition(i);

    int error = 0;
    ha_rows rows = 0;

    start_psi_batch_mode();

    if (!(error = ha_cde::rnd_init(true))) {
      while (!table->in_use->killed) {
        DBUG_EXECUTE_IF("bug28079850",
                        table->in_use->killed = THD::KILL_QUERY;);
        if ((error = ha_cde::rnd_next(table->record[0]))) {
          if (error == HA_ERR_RECORD_DELETED)
            continue;
          else
            break;
        }
        ++rows;
      }
    }

    end_psi_batch_mode();
    int ha_rnd_end_error = 0;

    if (error != HA_ERR_END_OF_FILE) *num_rows = HA_POS_ERROR;

    // Call ha_rnd_end() only if only if handler has been initialized.
    if (inited && (ha_rnd_end_error = ha_cde::rnd_end()))
      *num_rows = HA_POS_ERROR;

    if ((error != 0 && error != HA_ERR_END_OF_FILE) || ha_rnd_end_error != 0) {
      return (error != HA_ERR_END_OF_FILE) ? error : ha_rnd_end_error;
    }
    *num_rows += rows;

    updatePartition(i);
  }

  return 0;
}

ha_rows haCdepart::records_in_range(uint keynr, key_range *min_key,
                                    key_range *max_key) {
  if (keynr > table->s->keys) {
    return HA_POS_ERROR;
  }
  if (min_key == nullptr && max_key == nullptr) {
    return m_rowsInAllPartitions == 0 ? 1 : m_rowsInAllPartitions;
  }

  if (current_thd->optimizer_switch_flag(
          OPTIMIZER_SWITCH_DSTORE_USE_HISTOGRAM_RANGE_ESTIMATION)) {
    int64_t records = records_in_range_by_histogram(keynr, min_key, max_key,
                                                    m_rowsInAllPartitions);
    if (records >= 0) {
      return records == 0 ? 1 : static_cast<uint64_t>(records);
    }
  }

  uint32_t numRows = 0;
  // ha_cde::records_in_range shouldn't correct 0 rows to 1
  m_returnExactNrowsInRange = true;
  m_dontUseHistogram = true;
  for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    active_index = keynr;
    setPartition(i);  // set partition should do the mapping between mysql keynr
                      // and partition’s index
    uint32_t currentRows = ha_cde::records_in_range(keynr, min_key, max_key);
    if (HA_POS_ERROR == currentRows) return HA_POS_ERROR;
    numRows += currentRows;
  }
  m_returnExactNrowsInRange = false;
  m_dontUseHistogram = false;
  /* The MySQL optimizer seems to believe an estimate of 0 rows is
  always accurate and may return the result 'Empty set' based on that.
  Add 1 to the value to make sure MySQL does not make the assumption! */
  return numRows == 0 ? 1 : numRows;
}

/** Gives an UPPER BOUND to the number of rows in a table.
This is used in filesort.cc.
@return upper bound of rows. */
ha_rows haCdepart::estimate_rows_upper_bound() {
  DBUG_TRACE;
  uint32_t numRows = 0;
  for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    setPartition(i);
    numRows += ha_cde::estimate_rows_upper_bound();
  }
  return numRows;
}

/** Time estimate for full table scan.
How many seeks it will take to read through the table. This is to be
comparable to the number returned by records_in_range so that we can
decide if we should scan the table or use keys.
@return estimated time measured in disk seeks. */
double haCdepart::scan_time() {
  double scanTime = 0.0;
  DBUG_TRACE;

  for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    setPartition(i);
    scanTime += ha_cde::scan_time();
  }
  return scanTime;
}

// defined in ha_cde.cc
uint64_t dstore_rec_per_key(cde_dict_index_t *dict_index, const TABLE *table,
                            uint32_t key_no, uint32_t i,
                            ha_rows *prefix_records, bool isSampleScan);

int haCdepart::info_impl(uint flag, bool is_analyze) {
  DBUG_TRACE;

  THD *thd = ha_thd();
  int ret = CDE_OK;
  cde_session_t *session = CdeCreateOrGetSession(thd);

  if (is_analyze) {
    /* If it's ANALYZE TABLE, then we register a rw handlerton to
    trigger a 2PC commit in Server layer. */
    CdeTrxStartIfNotStarted(thd, true);
  }

  // need recalc dict stats
  if (flag & HA_STATUS_TIME && !(ha_thd()->for_ddl_info_replay)) {
    cde_perf_counters::getInstance()->_info_flag_time.increment();

    for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
         i = m_part_info->get_next_used_partition(i)) {
      cde_dict_t *dictTable = m_partShare->getTablePart(i);
      if (nullptr == dictTable) return HA_ERR_INTERNAL_ERROR;
      if (is_analyze) {
        if (dictTable->dict_stats_persist_enabled()) {
          CdeDictTableStatsUpdate(dictTable, RECALC_PERSIST, ha_thd());
        } else {
          ret = CdeDictTableStatsUpdate(dictTable, RECALC_STATS, ha_thd());
        }
      } else {
        if (dictTable->dict_stats_persist_enabled()) {
          CdeDictStatsFetchFromPs(dictTable, ha_thd());
        } else {
          ret = CdeDictTableStatsUpdate(dictTable, RECALC_STATS, ha_thd());
        }
      }
      if (ret != CDE_OK) {
        return HA_ERR_GENERIC;
      }
    }
    // todo: need track trx modified tables to update time
    stats.update_time = 0;
  }  // HA_STATUS_TIME

  uint biggestPartition = 0;
  uint maxRows = 0;
  // sync stats data from dict to ha_stats, no need to recalc
  if (flag & HA_STATUS_VARIABLE) {
    cde_perf_counters::getInstance()->_info_flag_variable.increment();
    uint64_t heap_pages = 0, index_pages = 0, n_rows = 0, free_pages = 0;

    // server can request HA_STATUS_VARIABLE with some partitions pruned away.
    // This affect stats.records. For instance if all partitions get pruned
    // away, but one -
    // only records from this partition will get counted - although whole table
    // can have
    // many more records. In DStore we use histogram to estimate
    // records_in_range.
    // Histogram uses information from whole table and takes into account all
    // the records
    // in the table. It has to use m_rowsInAllPartiton to get accurate number -
    // instead
    // of using stats.records.
    uint32_t rowsInAllPartitions = 0;
    for (uint i = 0; i < m_tot_parts; ++i) {
      cde_dict_t *dictTable = m_partShare->getTablePart(i);
      if (nullptr == dictTable) return HA_ERR_INTERNAL_ERROR;

      rowsInAllPartitions += dictTable->stat_n_rows;
    }
    // 2 == fallback estimate - check fetch_number_of_rows
    m_rowsInAllPartitions = rowsInAllPartitions == 0 ? 2 : rowsInAllPartitions;

    for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
         i = m_part_info->get_next_used_partition(i)) {
      cde_dict_t *dictTable = m_partShare->getTablePart(i);
      if (nullptr == dictTable) return HA_ERR_INTERNAL_ERROR;

      if (!(flag & HA_STATUS_NO_LOCK)) {
        dictTable->dict_stat_read_lock();
      }

      if (dictTable->stat_n_rows > maxRows) {
        maxRows = dictTable->stat_n_rows;
        biggestPartition = i;
      }

      heap_pages += dictTable->stat_heap_page_count;
      index_pages += dictTable->stat_all_indexs_page_count;
      n_rows += dictTable->stat_n_rows;
      free_pages += dictTable->stat_heap_free_page_count;
      if (!(flag & HA_STATUS_NO_LOCK)) {
        dictTable->dict_stat_unlock();
      }
    }

    stats.records = n_rows;
    stats.data_file_length = heap_pages * BLCKSZ;
    stats.index_file_length = index_pages * BLCKSZ;
    stats.delete_length = free_pages * BLCKSZ;
    if (stats.records == 0) {
      stats.mean_rec_length = 0;
    } else {
      stats.mean_rec_length = stats.data_file_length / stats.records;
    }

    stats.check_time = 0;
    stats.deleted = 0;
  }

  if (flag & HA_STATUS_CONST) {
    cde_perf_counters::getInstance()->_info_flag_const.increment();
    cde_dict_t *dictTable = nullptr;
    /* Find max rows and biggest partition. */
    for (uint i = 0; i < m_tot_parts; i++) {
      /* Skip partitions from above. */
      if ((flag & HA_STATUS_VARIABLE) == 0 ||
          !bitmap_is_set(&(m_part_info->read_partitions), i)) {
        dictTable = m_partShare->getTablePart(i);
        if (nullptr == dictTable) return HA_ERR_INTERNAL_ERROR;

        // handle index mem estimate in table_share if HA_STATUS_NO_LOCK
        if (!(flag & HA_STATUS_NO_LOCK)) dictTable->dict_stat_read_lock();

        if (dictTable->stat_n_rows > maxRows) {
          maxRows = dictTable->stat_n_rows;
          biggestPartition = i;
        }

        if (!(flag & HA_STATUS_NO_LOCK)) dictTable->dict_stat_unlock();
      }
    }

    dictTable = m_partShare->getTablePart(biggestPartition);
    if (nullptr == dictTable) return HA_ERR_INTERNAL_ERROR;

    if (!(flag & HA_STATUS_NO_LOCK)) dictTable->dict_stat_read_lock();

    ref_length = sizeof(m_dstore->dstore_heap_ctid);
    ref_length += PARTITION_BYTES_IN_POS;
    stats.mrr_length_per_rec =
        ref_length + sizeof(void *) - PARTITION_BYTES_IN_POS;
    bool isSampleScan = g_guc.enableIndexSample;

    Histogram_rdlock_guard s_histo_guard(&table->s->LOCK_m_histograms);
    for (uint i = 0; i < table->s->keys; i++) {
      KEY *key = &table->key_info[i];
      ha_rows prefix_records = maxRows;
      for (uint j = 0; j < key->actual_key_parts; ++j) {
        cde_dict_index_t *index = dictTable->get_index_on_name(key->name);
        if (index == nullptr && isSampleScan) {
          CDE_LOG_ERROR(
              "empty index dict when cal rec per key, table : %s index %s.",
              dictTable->name.c_str(), key->name);
          break;
        }
        key->rec_per_key[j] = dstore_rec_per_key(index, table, i, j,
                                                 &prefix_records, isSampleScan);
      }
    }

    if (!(flag & HA_STATUS_NO_LOCK)) {
      dictTable->dict_stat_unlock();
    }

    // todo: not used in CBO
    stats.create_time = 0;
    stats.max_data_file_length = 0;
    stats.max_index_file_length = 0;
  }

  if (flag & HA_STATUS_ERRKEY) {
    const cde_dict_index_t *err_dict_index = session->get_trxinfo()->err_index;
    if (err_dict_index) {
      errkey = cde_index_lookup(err_dict_index->name.c_str());
    } else {
      errkey = (~0);
    }
  }

  // handle auto inc
  if ((flag & HA_STATUS_AUTO) && !(ha_thd()->for_ddl_info_replay)) {
    CDE_ASSERT_DEBUG(table_share->next_number_keypart == 0);
    DBUG_PRINT("info", ("HA_STATUS_AUTO"));
    cde_perf_counters::getInstance()->_info_flag_auto.increment();

    if (table->found_next_number_field == nullptr) {
      stats.auto_increment_value = 0;
    } else {
      /* Lock to avoid two concurrent initializations. */
      lock_auto_increment();
      if (m_partShare->auto_inc_initialized) {
        stats.auto_increment_value = m_partShare->next_auto_inc_val;
      } else {
        /* The auto-inc mutex in the table_share is
        locked, so we do not need to have the handlers
        locked. */

        ret = initialize_auto_increment((flag & HA_STATUS_NO_LOCK) != 0);
        stats.auto_increment_value = m_partShare->next_auto_inc_val;
      }
      unlock_auto_increment();
    }
  }

  return ret;
}

int haCdepart::optimize(THD *, HA_CHECK_OPT *) {
  DBUG_TRACE;
  return (HA_ADMIN_TRY_ALTER);
}

/** Checks a partitioned table.
Tries to check that an InnoDB table is not corrupted. If corruption is
noticed, prints to stderr information about it. In case of corruption
may also assert a failure and crash the server. Also checks for records
in wrong partition.
@param[in]      thd             MySQL THD object/thread handle.
@param[in]      check_opt       Check options.
@return HA_ADMIN_CORRUPT or HA_ADMIN_OK. */
int haCdepart::check(THD *thd, HA_CHECK_OPT *check_opt) {
  uint error = HA_ADMIN_OK;
  uint i;

  DBUG_TRACE;
  /* TODO: Enhance this to:
  - Every partition has the same structure.
  - The names are correct (partition names checked in ::open()?)
  Currently it only does normal DStore check of each partition. */

  if (set_altered_partitions()) {
    CDE_ASSERT_DEBUG(false);  // Already checked by set_part_state()!
    return HA_ADMIN_INVALID;
  }
  for (i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    if ((check_opt->flags & (T_MEDIUM | T_EXTEND)) != 0) {
      error = Partition_helper::check_misplaced_rows(i, false);
      if (error != 0) {
        break;
      }
    }
  }
  if (error != 0) {
    print_admin_msg(thd, 256, "error", table_share->db.str, table->alias,
                    "check",
                    m_is_sub_partitioned ? "Subpartition %s returned error"
                                         : "Partition %s returned error",
                    m_partShare->get_partition_name(i));
  }

  return error;
}

/** Repair a partitioned table.
Only repairs records in wrong partitions (moves them to the correct
partition or deletes them if not in any partition).
@param[in]      thd             MySQL THD object/thread handle.
@param[in]      repair_opt      Repair options.
@return 0 or error code. */
int haCdepart::repair(THD *thd, HA_CHECK_OPT *repair_opt) {
  uint error = HA_ADMIN_OK;

  DBUG_EXECUTE_IF("haCdepart_repair_admin_internal_error",
                  return HA_ADMIN_INTERNAL_ERROR;);
  DBUG_TRACE;

  /* Only repair partitions for MEDIUM or EXTENDED options. */
  if ((repair_opt->flags & (T_MEDIUM | T_EXTEND)) == 0) {
    return HA_ADMIN_OK;
  }
  if (set_altered_partitions()) {
    CDE_ASSERT_DEBUG(false);  // Already checked by set_part_state()!
    return HA_ADMIN_INVALID;
  }
  for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    /* TODO: Implement and use haCde::repair()! */
    error = Partition_helper::check_misplaced_rows(i, true);
    if (error != 0) {
      print_admin_msg(thd, 256, "error", table_share->db.str, table->alias,
                      "repair",
                      m_is_sub_partitioned ? "Subpartition %s returned error"
                                           : "Partition %s returned error",
                      m_partShare->get_partition_name(i));
      break;
    }
  }

  return error;
}

/** Start statement.
MySQL calls this function at the start of each SQL statement inside LOCK
TABLES. Inside LOCK TABLES the "::external_lock" method does not work to
mark SQL statement borders. Note also a special case: if a temporary table
is created inside LOCK TABLES, MySQL has not called external_lock() at all
on that table.
MySQL-5.0 also calls this before each statement in an execution of a stored
procedure. To make the execution more deterministic for binlogging, MySQL-5.0
locks all tables involved in a stored procedure with full explicit table
locks (thd_in_lock_tables(thd) holds in store_lock()) before executing the
procedure.
@param[in]      thd             Handle to the user thread.
@param[in]      lock_type       Lock type.
@return 0 or error code. */
int haCdepart::start_stmt(THD *thd, thr_lock_type lock_type) {
  DBUG_TRACE;
  int error = 0;

  if (m_part_info->get_first_used_partition() == MY_BIT_NONE) {
    /* All partitions pruned away, do nothing! */
    return (error);
  }

  for (uint i = 0; i < m_tot_parts; i++) {
    setPartition(i);
    error = ha_cde::start_stmt(thd, lock_type);
    if (CDE_OK != error) return error;
  }

  m_reuseColumnsDecoding = false;
  return error;
}

/** Function to store lock for all partitions in native partitioned table. Also
look at ha_innobase::store_lock for more details.
@param[in]      thd             user thread handle
@param[in]      to              pointer to the current element in an array of
pointers to lock structs
@param[in]      lock_type       lock type to store in 'lock'; this may also be
TL_IGNORE
@retval to      pointer to the current element in the 'to' array */
THR_LOCK_DATA **haCdepart::store_lock(THD *thd, THR_LOCK_DATA **to,
                                      thr_lock_type lock_type) {
  DBUG_TRACE;
  for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    setPartition(i);
    ha_cde::store_lock(thd, to, lock_type);
  }
  return to;
}

/** Lock/prepare to lock table.
As MySQL will execute an external lock for every new table it uses when it
starts to process an SQL statement (an exception is when MySQL calls
start_stmt for the handle) we can use this function to store the pointer to
the THD in the handle. We will also use this function to communicate
to DStore that a new SQL statement has started and that we must store a
savepoint to our transaction handle, so that we are able to roll back
the SQL statement in case of an error.
@param[in]      thd             Handle to the user thread.
@param[in]      lock_type       Lock type.
@return 0 or error number. */
int haCdepart::external_lock(THD *thd, int lock_type) {
  DBUG_TRACE;
  int error = CDE_OK;

  if (m_part_info->get_first_used_partition() == MY_BIT_NONE &&
      !(m_mysql_has_locked && lock_type == F_UNLCK)) {
    /* All partitions pruned away, do nothing! */
    CDE_ASSERT_DEBUG(!m_mysql_has_locked);
    return (error);
  }
  CDE_ASSERT_DEBUG(m_mysql_has_locked || lock_type != F_UNLCK);

  for (uint i = m_part_info->get_first_used_partition(); i < m_tot_parts;
       i = m_part_info->get_next_used_partition(i)) {
    setPartition(i);
    error = ha_cde::external_lock(thd, lock_type);
    if (CDE_OK != error) {
      /** We can only fail on locking */
      CDE_ASSERT_DEBUG(HA_ERR_LOCK_TABLE_FULL == error);
      /** Previous successfull call to ha_cde::external_lock might
       * have set m_mysql_has_locked to true. Set it back to false.
       */
      m_mysql_has_locked = false;
      return error;
    }
  }

  m_reuseColumnsDecoding = false;
  return error;
}

/** Exchange partition.
Low-level primitive which implementation is provided here.
@param[in]      part_id                 The id of the partition to be exchanged
@param[in]      part_table              partitioned table to be exchanged
@param[in]      swap_table              table to be exchanged
@return error number
@retval 0       on success */
int haCdepart::exchange_partition_low(uint partId, dd::Table *partTable,
                                      dd::Table *swapTable) {
  DBUG_TRACE;

  CDE_ASSERT_DEBUG(nullptr != partTable);
  CDE_ASSERT_DEBUG(nullptr != swapTable);
  CDE_ASSERT_DEBUG(nullptr != m_partShare);
  CDE_ASSERT_DEBUG(partId < m_tot_parts);
  std::vector<dd::Partition_index *> partIndexes;
  std::vector<dd::Partition_index *>::iterator pIter;
  std::vector<dd::Index *> swapIndexes;
  std::vector<dd::Index *>::iterator sIter;
  std::vector<dd::Index *> part_table_indexes;
  std::vector<dd::Index *>::iterator pt_iter;

  /* Find the specified dd::Partition object */
  uint id = 0;
  dd::Partition *ddPart = nullptr;
  for (auto part : *partTable->leaf_partitions()) {
#ifndef NDEBUG
    uint64_t refCount = m_partShare->getTablePart(id)->ref_count.load();
    CDE_ASSERT_DEBUG(1 == refCount);
#endif

    if (++id > partId) {
      ddPart = part;
      break;
    }
  }
  CDE_ASSERT_DEBUG(nullptr != ddPart);

  THD *thd = ha_thd();
  /* Get the dstore table objects of part_table and swap_table */
  cde_dict_t *part = m_partShare->getTablePart(partId);

  dd::cache::Dictionary_client *dc = dd::get_dd_client(thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(dc);

  dd::String_type schema;
  dd::String_type tablename;
  if (dc->get_table_name_by_se_private_id(DSTORE_ENGINE_NAME,
                                          swapTable->se_private_id(), &schema,
                                          &tablename)) {
    CDE_LOG_ERROR("Unable to determine the schema name for table %s",
                  tablename.c_str());
    return HA_ERR_INTERNAL_ERROR;
  }

  dd::String_type swapTableFullName{"./" + schema + '/' + tablename};

  cde_dict_t *swap = DictSysGetTable(swapTableFullName.c_str());
  CDE_ASSERT_DEBUG(nullptr != swap);
  CDE_ASSERT_DEBUG(2 == swap->ref_count);
  DictTableRefGuard dictRefGuard(swap);

  /* Store and sort part_table indexes */
  std::copy(partTable->indexes()->begin(), partTable->indexes()->end(),
            std::back_inserter(part_table_indexes));
  std::sort(part_table_indexes.begin(), part_table_indexes.end(),
            [](dd::Index *a, dd::Index *b) { return (a->name() < b->name()); });
  /* Try to rename files. Tablespace checking ensures that
  both partition and table are of implicit tablespace. The plan is:
  1. Rename the swap table to the intermediate file
  2. Rename the partition to the swap table file
  3. Rename the intermediate file of swap table to the partition file */
  /* Define the temporary table name, by appending TMP_POSTFIX */
  const std::string tmpName{swap->name + TMP_POSTFIX};
  const std::string swapName{swap->name};
  const std::string partName{part->name};

  int32_t error = DictSysRenameTable(swapName.c_str(), tmpName.c_str(), false);
  if (0 != error) {
    CDE_LOG_ERROR(
        "exchange_partition_low: error when renaming table from %s to %s.",
        swapName.c_str(), tmpName.c_str());
    return HA_ERR_INTERNAL_ERROR;
  }
  error = DictSysRenameTable(partName.c_str(), swapName.c_str(), false);
  if (0 != error) {
    CDE_LOG_ERROR(
        "exchange_partition_low: error when renaming table from %s to %s.",
        partName.c_str(), swapName.c_str());
    return HA_ERR_INTERNAL_ERROR;
  }

  error = DictSysRenameTable(tmpName.c_str(), partName.c_str(), false);
  if (0 != error) {
    CDE_LOG_ERROR(
        "exchange_partition_low: error when renaming table from %s to %s.",
        tmpName.c_str(), partName.c_str());
    return HA_ERR_INTERNAL_ERROR;
  }

  std::copy(ddPart->indexes()->begin(), ddPart->indexes()->end(),
            std::back_inserter(partIndexes));
  std::copy(swapTable->indexes()->begin(), swapTable->indexes()->end(),
            std::back_inserter(swapIndexes));

  /* Sort the index pointers according to the index names because the index
  ordanility of the partition being exchanged may be different than the
  table being swapped */
  std::sort(partIndexes.begin(), partIndexes.end(),
            [](dd::Partition_index *a, dd::Partition_index *b) {
              return (a->name() < b->name());
            });
  std::sort(swapIndexes.begin(), swapIndexes.end(),
            [](dd::Index *a, dd::Index *b) { return (a->name() < b->name()); });

  /* Swap the se_private_data and options between indexes.
  The se_private_data should be swapped between every index of
  dd_part and swap_table; however, options should be swapped(checked)
  between part_table and swap_table */
  CDE_ASSERT_DEBUG(partIndexes.size() == swapIndexes.size());
  for (pIter = partIndexes.begin(), sIter = swapIndexes.begin();
       pIter < partIndexes.end() && sIter < swapIndexes.end();
       pIter++, sIter++) {
    auto partIndex = *pIter;
    auto swapIndex = *sIter;
    dd::Object_id p_tablespace_id = partIndex->tablespace_id();
    partIndex->set_tablespace_id(swapIndex->tablespace_id());
    swapIndex->set_tablespace_id(p_tablespace_id);

    CDE_ASSERT_DEBUG(partIndex->se_private_data().empty() ==
                     swapIndex->se_private_data().empty());
    CDE_ASSERT_DEBUG(partIndex->se_private_data().size() ==
                     swapIndex->se_private_data().size());

    if (!partIndex->se_private_data().empty()) {
      std::unique_ptr<dd::Properties> p_se_data(
          dd::Properties::parse_properties(""));
      p_se_data->insert_values(partIndex->se_private_data());
      partIndex->se_private_data().clear();
      partIndex->set_se_private_data(swapIndex->se_private_data());
      swapIndex->se_private_data().clear();
      swapIndex->set_se_private_data(*p_se_data);
    }
  }

  /* Swap the se_private_data and options of the two tables.
  Only the max autoinc should be set to both tables */
  if (m_partShare->getTableShare()->found_next_number_field) {
    uint64_t partAutoinc = part->autoinc_dict.GetAutoinc();
    uint64_t swapAutoinc = swap->autoinc_dict.GetAutoinc();
    uint64_t maxAutoinc = std::max(partAutoinc, swapAutoinc);

    part->autoinc_dict.MutexEnter();
    part->autoinc_dict.SetAutoinc(maxAutoinc);
    part->autoinc_dict.MutexExit();

    if (m_partShare->next_auto_inc_val < swapAutoinc) {
      lock_auto_increment();
      m_partShare->next_auto_inc_val = swapAutoinc;
      unlock_auto_increment();
    }
  }

  /* Swap the se_private_id between partition and table */
  dd::Object_id pSeId = ddPart->se_private_id();

  CDE_ASSERT_DEBUG(pSeId == part->m_id);
  CDE_ASSERT_DEBUG(swapTable->se_private_id() == swap->m_id);

  ddPart->set_se_private_id(swapTable->se_private_id());
  swapTable->set_se_private_id(pSeId);

  std::unique_ptr<dd::Properties> partTableSeData(
      dd::Properties::parse_properties(""));
  partTableSeData->insert_values(ddPart->se_private_data());
  ddPart->se_private_data().clear();
  ddPart->set_se_private_data(swapTable->se_private_data());
  swapTable->se_private_data().clear();
  swapTable->set_se_private_data(*partTableSeData);

  return error;
}

void haCdepart::get_auto_increment(ulonglong, ulonglong increment,
                                   ulonglong nb_desired_values,
                                   ulonglong *first_value,
                                   ulonglong *nb_reserved_values) {
  DBUG_TRACE;
  if (table_share->next_number_keypart != 0) {
    /* Only first key part allowed as autoinc for DStore tables! */
    *first_value = ULLONG_MAX;
    CDE_ASSERT_DEBUG(false);
    return;
  }
  get_auto_increment_first_field(increment, nb_desired_values, first_value,
                                 nb_reserved_values);
}

/** Compares two 'refs'.
A 'ref' is the (internal) primary key value of the row.
If there is no explicitly declared non-null unique key or a primary key, then
DStore internally uses the row id as the primary key.
It will use the partition id as secondary compare.
@param[in]      ref1    An (internal) primary key value in the MySQL key value
format.
@param[in]      ref2    Reference to compare with (same type as ref1).
@return < 0 if ref1 < ref2, 0 if equal, else > 0. */
int haCdepart::cmp_ref(const uchar *ref1, const uchar *ref2) const {
  int cmp;

  cmp = ha_cde::cmp_ref(ref1 + PARTITION_BYTES_IN_POS,
                        ref2 + PARTITION_BYTES_IN_POS);

  if (cmp != 0) {
    return (cmp);
  }

  cmp = static_cast<int>(uint2korr(ref1)) - static_cast<int>(uint2korr(ref2));

  return (cmp);
}

}  // namespace CDE

/****************************************************************************
DS-MRR implementation
 ***************************************************************************/

/* TODO: move the default implementations into the base handler class! */
/* TODO: See if it could be optimized for partitioned tables? */
/* Use default ha_innobase implementation for now... */
