/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_dict_stat.cc
 *
 *
 * IDENTIFICATION
 * dict/cde_dict_stat.cc
 *
 * -------------------------------------------------------------------------
 */

#include <string>
#include <tuple>
#include "handler.h"
#include "include/scope_guard.h"

#include "key.h"
#include "m_ctype.h"
#include "my_base.h"
#include "my_bitmap.h"
#include "sql/dd/impl/utils.h"
#include "sql/sql_prepare.h"
#include "sql/sql_table.h"
#include "sql/sql_thd_internal_api.h"
#include "sql/thd_raii.h"
#include "sql/transaction.h"

#include "boot/cde_instance.h"
#include "common/cde_alloc.h"
#include "common/cde_trxmgr.h"
#include "common/dstore_common_utils.h"
#include "ddl/cde_ddl.h"
#include "ddl/dd_helper.h"
#include "dict/cde_dict_bg_stat.h"
#include "dict/cde_dict_stat.h"
#include "dict/cde_stats_sampler.h"

#include "heap/dstore_heap_interface.h"
#include "index/dstore_btree_sample_scan.h"
#include "page/dstore_itemptr.h"
#include "sql_base.h"
#include "sql_const.h"
#include "sql_string.h"
#include "thr_lock.h"
#include "transaction/dstore_transaction_interface.h"
#include "tuple/dstore_tuple_interface.h"
namespace CDE {

/**
 * Index position of the fields in the dstore_table_stats table
 */
namespace DstoreTableStatColIdx {
static constexpr size_t N_DATABASE_NAME = 0;
static constexpr size_t N_TABLE_NAME = 1;
static constexpr size_t N_LAST_UPDATE = 2;
static constexpr size_t N_ROWS = 3;
static constexpr size_t N_HEAP_TABLE_PAGES = 4;
static constexpr size_t N_SUM_OF_INDEX_PAGES = 5;
/* Free page num of heap table */
static constexpr size_t N_HEAP_TABLE_FREE_PAGES = 6;
/* lob page num of heap table */
static constexpr size_t N_HEAP_TABLE_LOB_PAGES = 7;
}  // namespace DstoreTableStatColIdx

/**
 * Index position of the fields in the dstore_index_stats table
 */
namespace DstoreIndexStatColIdx {
static constexpr size_t N_DATABASE_NAME = 0;
static constexpr size_t N_TABLE_NAME = 1;
static constexpr size_t N_INDEX_NAME = 2;
static constexpr size_t N_LAST_UPDATE = 3;
static constexpr size_t N_STAT_NAME = 4;
static constexpr size_t N_STAT_VALUE = 5;
static constexpr size_t N_SAMPLE_SIZE = 6;
static constexpr size_t N_STAT_DESCRIPTION = 7;
}  // namespace DstoreIndexStatColIdx

/** Because the algorithm used by the innodb analyze table is used, the number
 * of rows collected is relatively stable. Currently, the dstore randomly
 * samples data based on heap pages. If the random seed is not fixed, the row
 * value fluctuates each time the analyze table is executed. Therefore, the seed
 * value needs to be fixed to ensure that the row value is relatively stable
 * after the analyze table is executed. 42 is a random fixed value without
 * special meaning.  */
constexpr uint32_t CDE_STAT_SEED = 42;

// persist stats data
int CdeDictStatsPersist(cde_dict_t *table) {
  // lock and clone a new table to avoid persist occupy too much time
  table->dict_stat_wr_lock();

  if (!table->stat_initialized) {
    table->dict_stat_unlock();
    return CDE_ERROR;
  }

  table->dict_stat_unlock();
  return CDE_OK;
}

int CdeDictStatsResetIndex(cde_dict_index_t *index) {
  for (uint32_t i = 0; i < index->index_col_num; i++) {
    index->stat_n_diff_key_vals[i] = 0;
    index->stat_n_sample_size[i] = 0;
  }

  index->stat_index_page_cnt = 1;
  index->stat_leaf_page_cnt = 1;
  return CDE_OK;
}

// reset table and relative index stats data
int CdeDictStatsReset(cde_dict_t *table) {
  CDE_ASSERT(table != nullptr);

  table->dict_stat_wr_lock();

  // reset table stats
  table->stat_sample_pages = 0;
  table->stat_heap_page_count = 1;
  table->stat_heap_free_page_count = 0;
  table->stat_heap_lob_page_count = 0;
  table->stat_n_rows = 0;
  table->stat_modified_counter = 0;
  // table->data_file_length = 0;

  // reset all related index stats
  auto idx_vec = table->index_dict_vec;
  for (auto idx : idx_vec) {
    if (idx->DictStatsShouldIgnoreIndex()) {
      continue;
    }

    CdeDictStatsResetIndex(idx);
  }

  table->stat_initialized = true;

  table->dict_stat_unlock();

  return CDE_OK;
}

void CdeDictTableStatsInit(cde_dict_t *table, THD *thd) {
  if (table->stat_initialized) {
    return;
  }

  if (table->dict_stats_persist_enabled()) {
    CdeDictStatsFetchFromPs(table, thd);
  } else {
    CdeDictTableStatsUpdate(table, RECALC_STATS, thd);
  }
}

/**
Since no dict_table_close(), deinitialize it explicitly.
Do same with innodb.

@param[in,out]  table Pointer to the cde_dict_t object.
*/
void DstoreDictTableStatsDeinit(cde_dict_t *table) {
  if (!table->stat_initialized) {
    return;
  }

  table->dict_stat_wr_lock();
  table->stat_initialized = false;
  table->dict_stat_unlock();
}

bool CdeDictIndexStatsCalc(cde_dict_index_t *index, uint64_t samplePageNums) {
  auto index_rel = index->rel;
  if (index_rel == nullptr || index_rel->btreeSmgr == nullptr) {
    return false;
  }
  uint64_t page_cnt =
      StorageTableInterface::GetIndexBlockCount(index_rel->btreeSmgr);
  index->stat_index_page_cnt = page_cnt;

  uint64_t leaf_page_cnt =
      StorageTableInterface::GetIndexLeafPageCount(index_rel->btreeSmgr);
  index->stat_leaf_page_cnt = leaf_page_cnt;

  if (!CdeEnableIndexSample()) {
    return true;
  }

  DSTORE::BtrSampleScanContext context;
  DSTORE::RetStatus status = context.Init(index->index_col_num, samplePageNums);
  if (status == DSTORE::DSTORE_FAIL) {
    return true;
  }
  context.statLeafPageCnt = leaf_page_cnt;
  status =
      IndexInterface::IndexSampleScan(index_rel, index_rel->index, &context);
  if (status == DSTORE::DSTORE_FAIL) {
    context.Clean();
    return true;
  }
  for (uint32_t i = 0; i < index->index_col_num; i++) {
    index->stat_n_diff_key_vals[i] = context.statDiffKeyVals[i];
    index->stat_n_sample_size[i] = context.statLeafPagesAnalyze[i];
  }
  context.Clean();
  return true;
}

// just recalc stats and update mem, no need to persist
int CdeDictTableStatsRecalc(THD *thd, cde_dict_t *table, bool skipSample) {
  CDE_ASSERT(table != nullptr);
  // Do not support recalc stats for physical standby node
  if (g_guc.enableStandbyRole) {
    return CDE_OK;
  }
  table->dict_stat_wr_lock();

  // scan heap page to calc num of record
  DSTORE::StorageRelationData *table_rel = table->get_dstore_relation();
  CDE_ASSERT(table_rel->tableSmgr != nullptr);
  table->stat_heap_page_count =
      StorageTableInterface::GetTableBlockCount(table_rel->tableSmgr);
  if (table_rel->lobTableSmgr != nullptr) {
    table->stat_heap_lob_page_count =
        StorageTableInterface::GetTableBlockCount(table_rel->lobTableSmgr);
  }
  if (!skipSample) {
    DSTORE::HeapScanHandler *heap_table_handler =
        HeapInterface::CreateHeapScanHandler(table->dstore_relation);
    if (!heap_table_handler) {
      table->dict_stat_unlock();
      return CDE_ERROR;
    }

    if (thd->system_thread == SYSTEM_THREAD_BACKGROUND) {
      // background thread scan dstore tuple use READ_COMMITTED isoLevel.
      SingleStmtTrxStart(READ_COMMITTED);
    }

    DSTORE::SnapshotData dstore_snapshot;
    CDESetSnapshotByTrans(dstore_snapshot);

    HeapInterface::BeginScan(heap_table_handler, &dstore_snapshot);
    DSTORE::HeapSampleScanContext context;

    uint64_t samplePageNums = table->dict_stats_persist_enabled()
                                  ? table->get_persistent_sample_pages()
                                  : table->get_transient_sample_pages();
    uint64_t totalLiveTups = 0;
    uint64_t validPageCnt = 0;
    uint64_t validFreePageCnt = 0;

    BlockSamplerData data;
    OptstatsBlockSamplerInit(&data, table->stat_heap_page_count, samplePageNums,
                             CDE_STAT_SEED);
    uint32_t sampleCnt = std::min(table->stat_heap_page_count, samplePageNums);

    for (uint32_t i = 0; i < sampleCnt; i++) {
      /* quit bg stat process if delete/alter table set quit */
      /* bg_stat_flag is not required to be accurate at here, so we don't
      need to owen table->m_mtx. */
      if (table->bg_stat_flag & BG_STAT_QUIT) {
        break;
      }
      /* The internal implementation of the SampleScan interface depends on the
      value of SetSampleBlockNum. The num range cannot exceed the number of
      pages. */
      BlockNumber num = OptstatsBlockSamplerNext(&data);
      context.numTuples = 0;
      context.SetSampleBlockNum(num);
      auto status = HeapInterface::SampleScan(heap_table_handler, &context);
      if (DSTORE::DSTORE_SUCC == status) {
        totalLiveTups += context.numLiveTuples;
        validPageCnt++;
        uint32_t tuplesNum =
            std::min(context.numTuples, (int)DSTORE::MAX_ITEM_OFFSET_NUMBER);
        for (uint32_t j = 0; j < tuplesNum; j++) {
          TupleInterface::DestroyTuple(context.tuples[j], CdeFreeMemForDtuple);
        }
        /* Dstore do not provide interface to get free page information,
        we use sample interface to estimate free pages for heap table. */
        if (context.numLiveTuples == 0) {
          validFreePageCnt++;
        }
      }
    }
    HeapInterface::EndScan(heap_table_handler);
    HeapInterface::DestroyHeapScanHandler(heap_table_handler);

    if (samplePageNums < table->stat_heap_page_count && validPageCnt > 0) {
      table->stat_n_rows =
          ((uint64_t)totalLiveTups * (uint64_t)table->stat_heap_page_count) /
          validPageCnt;
      table->stat_heap_free_page_count =
          (validFreePageCnt * table->stat_heap_page_count) / validPageCnt;
    } else {
      table->stat_n_rows = totalLiveTups;
      table->stat_heap_free_page_count = validFreePageCnt;
    }
  }

  uint64_t indexSamplePageNums =
      table->dict_stats_persist_enabled()
          ? table->get_persistent_index_sample_pages()
          : table->get_transient_index_sample_pages();
  // calc all index stats
  uint64_t index_page_cnt = 0;
  for (auto idx : table->index_dict_vec) {
    if (idx == nullptr) {
      continue;
    }
    /* bg_stat_flag is not required to be accurate at here, so we don't
    need to owen table->m_mtx. */
    if (table->bg_stat_flag & BG_STAT_QUIT) {
      break;
    }

    if (idx->DictStatsShouldIgnoreIndex()) {
      continue;
    }

    auto index_rel = idx->rel;
    if (index_rel == nullptr || index_rel->btreeSmgr == nullptr) {
      continue;
    }
    uint64_t page_cnt =
        StorageTableInterface::GetIndexBlockCount(index_rel->btreeSmgr);
    index_page_cnt += page_cnt;
    CdeDictIndexStatsCalc(idx, indexSamplePageNums);

    // keep consistency with init
    if (idx->stat_leaf_page_cnt == 0) {
      idx->stat_leaf_page_cnt = 1;
    }
  }
  table->stat_all_indexs_page_count = index_page_cnt;

  if (thd->system_thread == SYSTEM_THREAD_BACKGROUND) {
    DSTORE::RetStatus ret = TransactionInterface::CommitTrxCommand();
    if (unlikely(ret != DSTORE::DSTORE_SUCC)) {
      table->dict_stat_unlock();
      return CDE_ERROR;
    }
  }

  // When initializing an empty table, the value of stat_heap_page_count is set
  // to 1 to maintain consistency.
  if (table->stat_heap_page_count == 0) {
    table->stat_heap_page_count = 1;
  }

  table->stat_initialized = true;
  table->stat_modified_counter = 0;

  table->dict_stat_unlock();
  return CDE_OK;
}

enum class DstoreStatTableEnum { TableStat, IndexStat };

/**
 * fetch schema name and table name from normalized table filename fullName.

  @param[in]  fullName normalized table filename fullName.

  @return a tuple <dbName, tableName>
 */
static std::tuple<std::string, std::string> NormalizeTableAndSchemaName(
    const std::string &fullName) {
  char fileTableName[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char fileDbName[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  CdeCreateTableInfo::SplitNormalizedName(fullName.c_str(), fileTableName,
                                          fileDbName);

  char tableName[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char dbName[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  filename_to_tablename(fileDbName, dbName, sizeof(dbName));
  PartsAwareFileNameToTableName(fileTableName, tableName, sizeof(tableName));

  return std::make_pair(std::string(dbName), std::string(tableName));
}

static bool ForEachStatRow(const std::string &schemaName,
                           const std::string &tableName, TABLE *table,
                           DstoreStatTableEnum whichTable,
                           const std::function<bool()> &callback) {
  if (table->file->ha_index_init(/*idx=*/0, /*sorted=*/false)) return true;

  auto index_cleanup =
      create_scope_guard([table]() { table->file->ha_index_end(); });

  table->use_all_columns();
  table
      ->field[whichTable == DstoreStatTableEnum::TableStat
                  ? DstoreTableStatColIdx::N_DATABASE_NAME
                  : DstoreIndexStatColIdx::N_DATABASE_NAME]
      ->store(schemaName.c_str(), schemaName.size(), system_charset_info);
  table
      ->field[whichTable == DstoreStatTableEnum::TableStat
                  ? DstoreTableStatColIdx::N_TABLE_NAME
                  : DstoreIndexStatColIdx::N_TABLE_NAME]
      ->store(tableName.c_str(), tableName.size(), system_charset_info);

  uchar user_key[MAX_KEY_LENGTH];
  key_copy(user_key, table->record[0], table->key_info,
           table->key_info->key_length);

  key_part_map key_part = HA_WHOLE_KEY;
  if (whichTable == DstoreStatTableEnum::IndexStat) {
    key_part = 3;
  }

  int res = table->file->ha_index_read_map(table->record[0], user_key, key_part,
                                           HA_READ_KEY_EXACT);
  while (res == 0) {
    if (callback()) {
      return true;
    }

    res = table->file->ha_index_next_same(table->record[0], user_key,
                                          table->key_info->key_length);
  }

  return false;
}

/**
rename persist statistical information into the stat table.

@param[in]      oldSchemaName oldSchemaName.
@param[in]      oldTableName oldTableName.
@param[in]      newSchemaName newSchemaName.
@param[in]      newTableName newTableName.
@param[in]      table Pointer to a stats table.
@param[in]      whichTable enum type of DstoreStatTable.

@return false on success, true on error.
*/
static bool RenameDstoreTableStats(const std::string &oldSchemaName,
                                   const std::string &oldTableName,
                                   const std::string &newSchemaName,
                                   const std::string &newTableName,
                                   TABLE *table,
                                   DstoreStatTableEnum whichTable) {
  return ForEachStatRow(oldSchemaName, oldTableName, table, whichTable, [&]() {
    store_record(table, record[1]);

    table
        ->field[whichTable == DstoreStatTableEnum::TableStat
                    ? DstoreTableStatColIdx::N_DATABASE_NAME
                    : DstoreIndexStatColIdx::N_DATABASE_NAME]
        ->store(newSchemaName.c_str(), newSchemaName.size(),
                system_charset_info);
    table
        ->field[whichTable == DstoreStatTableEnum::TableStat
                    ? DstoreTableStatColIdx::N_TABLE_NAME
                    : DstoreIndexStatColIdx::N_TABLE_NAME]
        ->store(newTableName.c_str(), newTableName.size(), system_charset_info);
    return table->file->ha_update_row(table->record[1], table->record[0]) != 0;
  });
}

/**
delete persist statistical information into the stat table.

@param[in]      schemaName dbName.
@param[in]      tableName tableName.
@param[in]      dstoreTableStats table Pointer to a stats table.
@param[in]      whichTable enum type of DstoreStatTable.

@return false on success, true on error.
*/
static bool DeleteDstoreTableStats(const std::string &schemaName,
                                   const std::string &tableName, TABLE *table,
                                   DstoreStatTableEnum whichTable) {
  return ForEachStatRow(schemaName, tableName, table, whichTable, [&]() {
    return table->file->ha_delete_row(table->record[0]) != 0;
  });
}

/**
delete index persist statistical information into the index stat table.

@param[in]      schemaName dbName.
@param[in]      tableName  tableName.
@param[in]      table      table Pointer to the index stats table.

@return false on success, true on error.
*/
static bool DeleteDstoreIndexStats(const std::string &schemaName,
                                   const std::string &tableName,
                                   const std::string &indexName, TABLE *table) {
  auto indexCleanup = create_scope_guard([table]() {
    if (table->file->inited) {
      table->file->ha_index_end();
    }
  });

  if (!table->file->inited &&
      table->file->ha_index_init(/*idx=*/0, /*sorted=*/false)) {
    return true;
  }

  table->use_all_columns();
  table->field[DstoreIndexStatColIdx::N_DATABASE_NAME]->store(
      schemaName.c_str(), schemaName.size(), system_charset_info);
  table->field[DstoreIndexStatColIdx::N_TABLE_NAME]->store(
      tableName.c_str(), tableName.size(), system_charset_info);
  table->field[DstoreIndexStatColIdx::N_INDEX_NAME]->store(
      indexName.c_str(), indexName.size(), system_charset_info);

  uchar userKey[MAX_KEY_LENGTH];
  key_copy(userKey, table->record[0], table->key_info,
           table->key_info->key_length);

  // We use the frist 3 column as key, so the key_map is set to 7.
  int ret = table->file->ha_index_read_map(table->record[0], userKey, 7,
                                           HA_READ_KEY_EXACT);

  if (ret == HA_ERR_KEY_NOT_FOUND || ret == HA_ERR_END_OF_FILE) {
    return false;
  }

  if (ret != 0) {
    table->file->print_error(ret, MYF(0));
    return true;
  }

  while (ret == 0) {
    store_record(table, record[1]);
    if (table->file->ha_delete_row(table->record[1]) != 0) {
      return true;
    }

    ret = table->file->ha_index_next_same(table->record[0], userKey,
                                          table->key_info->key_length);
  }

  return false;
}

/**
rename index persist statistical information into the index stat table.

@param[in]      schemaName oldSchemaName.
@param[in]      tableName oldTableName.
@param[in]      oldIndexName newSchemaName.
@param[in]      newIndexName newTableName.
@param[in]      table        Pointer to a index stats table.

@return false on success, true on error.
*/
static bool RenameDstoreIndexStats(const std::string &schemaName,
                                   const std::string &tableName,
                                   const std::string &oldIndexName,
                                   const std::string &newIndexName,
                                   TABLE *table) {
  auto indexCleanup = create_scope_guard([table]() {
    if (table->file->inited) {
      table->file->ha_index_end();
    }
  });

  if (!table->file->inited &&
      table->file->ha_index_init(/*idx=*/0, /*sorted=*/false)) {
    return true;
  }

  table->use_all_columns();
  table->field[DstoreIndexStatColIdx::N_DATABASE_NAME]->store(
      schemaName.c_str(), schemaName.size(), system_charset_info);
  table->field[DstoreIndexStatColIdx::N_TABLE_NAME]->store(
      tableName.c_str(), tableName.size(), system_charset_info);
  table->field[DstoreIndexStatColIdx::N_INDEX_NAME]->store(
      oldIndexName.c_str(), oldIndexName.size(), system_charset_info);

  uchar userKey[MAX_KEY_LENGTH];
  key_copy(userKey, table->record[0], table->key_info,
           table->key_info->key_length);

  // We use the frist 3 column as key, so the key_map is set to 7.
  int ret = table->file->ha_index_read_map(table->record[0], userKey, 7,
                                           HA_READ_KEY_EXACT);

  if (ret == HA_ERR_KEY_NOT_FOUND || ret == HA_ERR_END_OF_FILE) {
    return false;
  }

  if (ret != 0) {
    table->file->print_error(ret, MYF(0));
    return true;
  }

  while (ret == 0) {
    store_record(table, record[1]);
    table->field[DstoreIndexStatColIdx::N_INDEX_NAME]->store(
        newIndexName.c_str(), newIndexName.size(), system_charset_info);
    ret = table->file->ha_update_row(table->record[1], table->record[0]);
    if (ret != 0 && ret != HA_ERR_RECORD_IS_THE_SAME) {
      table->file->print_error(ret, MYF(0));
      return true;
    }

    ret = table->file->ha_index_next_same(table->record[0], userKey,
                                          table->key_info->key_length);
  }

  return false;
}

/**
write/update persist statistical information into the stat table.

@param[in]      schemaName dbName.
@param[in]      tableName tableName.
@param[in]      dstoreTableStats table Pointer to a table:dstore_table_stats.
@param[in]      dictTable table Pointer to the cde_dict_t object.

@return false on success, true on error.
*/
static bool UpdateDstoreTableStats(THD *thd, const std::string &schemaName,
                                   const std::string &tableName,
                                   TABLE *dstoreTableStats,
                                   cde_dict_t *dictTable) {
  bool rowExists = false;
  bool ret = ForEachStatRow(schemaName, tableName, dstoreTableStats,
                            DstoreStatTableEnum::TableStat, [&]() {
                              rowExists = true;
                              return false;
                            });
  if (ret) return true;

  if (rowExists) {
    // Row exists, so we must make a copy of the old row into record[1]
    store_record(dstoreTableStats, record[1]);
  }

  auto now = static_cast<uint32_t>(time(nullptr));
  MYSQL_TIME currentTime;
  thd->variables.time_zone->gmt_sec_to_TIME(&currentTime,
                                            static_cast<my_time_t>(now));

  dstoreTableStats->field[DstoreTableStatColIdx::N_LAST_UPDATE]->store_time(
      &currentTime);  // last update
  dstoreTableStats->field[DstoreTableStatColIdx::N_ROWS]->store(
      static_cast<longlong>(dictTable->stat_n_rows),
      /*unsigned_val=*/true);
  dstoreTableStats->field[DstoreTableStatColIdx::N_HEAP_TABLE_PAGES]->store(
      static_cast<longlong>(dictTable->stat_heap_page_count),
      /*unsigned_val=*/true);  // Heap table size
  dstoreTableStats->field[DstoreTableStatColIdx::N_SUM_OF_INDEX_PAGES]->store(
      static_cast<longlong>(dictTable->stat_all_indexs_page_count),
      /*unsigned_val=*/true);  // sum of indexes
  dstoreTableStats->field[DstoreTableStatColIdx::N_HEAP_TABLE_FREE_PAGES]
      ->store(static_cast<longlong>(dictTable->stat_heap_free_page_count),
              /*unsigned_val=*/true);  // Heap table size
  dstoreTableStats->field[DstoreTableStatColIdx::N_HEAP_TABLE_LOB_PAGES]->store(
      static_cast<longlong>(dictTable->stat_heap_lob_page_count),
      /*unsigned_val=*/true);  // Heap table lob size

  int error = 0;
  if (rowExists) {
    // The row already exists, so update it
    if ((error = dstoreTableStats->file->ha_update_row(
             dstoreTableStats->record[1], dstoreTableStats->record[0])) != 0 &&
        error != HA_ERR_RECORD_IS_THE_SAME) {
      CDE_LOG_ERROR(
          "UpdateDstoreTableStats ha_update_row error, table:%s error:%d",
          tableName.c_str(), error);
      return true;
    }
  } else {
    // Row does not exists
    if ((error = dstoreTableStats->file->ha_write_row(
             dstoreTableStats->record[0])) != 0) {
      CDE_LOG_ERROR(
          "UpdateDstoreTableStats ha_write_row error, table:%s error:%d",
          tableName.c_str(), error);
      return true;
    }
  }

  return false;
}

/**
write/update persist statistical information into the index stat table.

@param[in]      schemaName dbName.
@param[in]      tableName tableName.
@param[in]      dstoreIndexStats table Pointer to a table:dstore_index_stats.
@param[in]      dictTable table Pointer to the cde_dict_t object.
@param[in]      index    Only update the index statistics if not nullptr.

@return false on success, true on error.
*/
static bool UpdateDstoreIndexStats(THD *thd, const std::string &schemaName,
                                   const std::string &tableName,
                                   TABLE *dstoreIndexStats,
                                   cde_dict_t *dictTable,
                                   cde_dict_index_t *index = nullptr) {
  if (dstoreIndexStats->file->ha_index_init(/*idx=*/0, /*sorted=*/false))
    return true;

  auto index_cleanup = create_scope_guard(
      [dstoreIndexStats]() { dstoreIndexStats->file->ha_index_end(); });
  dstoreIndexStats->use_all_columns();

  const auto upsert = [&](const std::string &index_name,
                          const std::string &stat_name, longlong stat_value,
                          longlong sample_size,
                          const std::string &statDescription) {
    dstoreIndexStats->field[DstoreIndexStatColIdx::N_DATABASE_NAME]->store(
        schemaName.c_str(), schemaName.size(), system_charset_info);
    dstoreIndexStats->field[DstoreIndexStatColIdx::N_TABLE_NAME]->store(
        tableName.c_str(), tableName.size(), system_charset_info);
    dstoreIndexStats->field[DstoreIndexStatColIdx::N_INDEX_NAME]->store(
        index_name.c_str(), index_name.size(), system_charset_info);
    dstoreIndexStats->field[DstoreIndexStatColIdx::N_STAT_NAME]->store(
        stat_name.c_str(), stat_name.size(), system_charset_info);
    uchar user_key[MAX_KEY_LENGTH];

    key_copy(user_key, dstoreIndexStats->record[0], dstoreIndexStats->key_info,
             dstoreIndexStats->key_info->key_length);
    int res = dstoreIndexStats->file->ha_index_read_map(
        dstoreIndexStats->record[0], user_key, HA_WHOLE_KEY, HA_READ_KEY_EXACT);

    if (res == 0) {
      // Store the old row in record[1]
      store_record(dstoreIndexStats, record[1]);
    }

    auto now = static_cast<uint32_t>(time(nullptr));
    MYSQL_TIME currentTime;
    thd->variables.time_zone->gmt_sec_to_TIME(&currentTime,
                                              static_cast<my_time_t>(now));

    dstoreIndexStats->field[DstoreIndexStatColIdx::N_LAST_UPDATE]->store_time(
        &currentTime);  // last update
    dstoreIndexStats->field[DstoreIndexStatColIdx::N_STAT_VALUE]->store(
        stat_value,
        /*unsigned_val=*/true);  // stat_value
    dstoreIndexStats->field[DstoreIndexStatColIdx::N_SAMPLE_SIZE]->store(
        sample_size,
        /*unsigned_val=*/true);  // sample_size
    dstoreIndexStats->field[DstoreIndexStatColIdx::N_STAT_DESCRIPTION]->store(
        statDescription.c_str(), statDescription.size(), system_charset_info);
    int error = 0;
    DBUG_EXECUTE_IF("simulate_persist_updateIndexstats_index_read_map_error",
                    res = HA_ERR_GENERIC;);
    switch (res) {
      case 0:
        if ((error = dstoreIndexStats->file->ha_update_row(
                 dstoreIndexStats->record[1], dstoreIndexStats->record[0])) !=
                0 &&
            error != HA_ERR_RECORD_IS_THE_SAME) {
          CDE_LOG_ERROR(
              "UpdateDstoreIndexStats ha_update_row error, table:%s error:%d",
              tableName.c_str(), error);
          return true;
        }
        break;
      case HA_ERR_END_OF_FILE:
      case HA_ERR_KEY_NOT_FOUND:
        if ((error = dstoreIndexStats->file->ha_write_row(
                 dstoreIndexStats->record[0])) != 0) {
          CDE_LOG_ERROR(
              "UpdateDstoreIndexStats ha_write_row error, table:%s error:%d",
              tableName.c_str(), error);
          return true;
        }
        break;
      default:
        // Some kind of error.
        CDE_LOG_ERROR("UpdateDstoreIndexStats other error, table:%s error:%d",
                      tableName.c_str(), res);
        return true;
    }

    return false;
  };

  for (cde_dict_index_t *indexDict : *dictTable->get_index_dict_vec()) {
    if (indexDict->DictStatsShouldIgnoreIndex()) {
      continue;
    }

    if (index != nullptr && indexDict->oid != index->oid) {
      continue;
    }

    for (uint32_t i = 0; i < indexDict->index_col_num; ++i) {
      std::string stat_name = "n_diff_pfx";
      if (i < 9) {
        stat_name += "0";
      }
      stat_name += std::to_string(i + 1);

      std::string statDescription;

      const char *headColName = indexDict->fields[0].m_name;
      statDescription += headColName;
      for (uint32_t j = 1; j <= i; ++j) {
        statDescription += ",";
        statDescription += indexDict->fields[j].m_name;
      }

      uint64_t n_diff_keys = indexDict->stat_n_diff_key_vals == nullptr
                                 ? 0
                                 : indexDict->stat_n_diff_key_vals[i];
      uint64_t n_sample_size = indexDict->stat_n_sample_size == nullptr
                                   ? 0
                                   : indexDict->stat_n_sample_size[i];
      if (upsert(indexDict->name, stat_name, n_diff_keys, n_sample_size,
                 statDescription)) {
        return true;
      }
    }

    if (upsert(indexDict->name, "n_leaf_pages", indexDict->stat_leaf_page_cnt,
               0, "Number of leaf pages in the index") ||
        upsert(indexDict->name, "size", indexDict->stat_index_page_cnt, 0,
               "Number of pages in the index")) {
      return true;
    }
  }

  return false;
}

/**
write/update persist statistical information into the table.

@param[in]  table Pointer to the cde_dict_t object.
@param[in]      thd Connection contex.
@param[in]      index Only update the index statistics if not nullptr.

@return false on success, true on error.
*/
static bool UpdateDstoreStatisticsTables(cde_dict_t *table, THD *thd,
                                         cde_dict_index_t *index = nullptr) {
  Table_ref dstoreStatsTables[2] = {
      Table_ref("mysql", "dstore_table_stats", TL_WRITE),
      Table_ref("mysql", "dstore_index_stats", TL_WRITE)};
  dstoreStatsTables[0].next_global = dstoreStatsTables[0].next_local =
      dstoreStatsTables[0].next_name_resolution_table = &dstoreStatsTables[1];

  if (open_trans_system_tables_for_write(thd, dstoreStatsTables)) {
    return true;
  }

  /* when bin log is disabled. this attachable transaction run the sub-statment.
    when setting next-gtid thiscommit it will invoke to save gtid. and then
    commit will invoke dstore commit. but the system table is innodb. and it
    cause conflict. and this statistics table is system table. it could be skip
    to save gtid. so in this sub-statement set this variable.
  */
  bool savedOperSubstatementimplicitly =
      thd->is_operating_substatement_implicitly;
  if (!opt_bin_log) {
    thd->is_operating_substatement_implicitly = true;
  }

  const auto close_tables =
      create_scope_guard([thd, savedOperSubstatementimplicitly]() {
        close_trans_system_tables(thd);
        if (!opt_bin_log) {
          thd->is_operating_substatement_implicitly =
              savedOperSubstatementimplicitly;
        }
      });

  auto [dbNameS, tableNameS] = NormalizeTableAndSchemaName(table->name);

  if (UpdateDstoreTableStats(thd, dbNameS, tableNameS,
                             dstoreStatsTables[0].table, table) ||
      DBUG_EVALUATE_IF("simulate_persist_dictstat_updatetable_failed", true,
                       false)) {
    ha_commit_trans(thd, false, true);
    return true;
  }

  if (index == nullptr &&
      DeleteDstoreTableStats(dbNameS, tableNameS, dstoreStatsTables[1].table,
                             DstoreStatTableEnum::IndexStat)) {
    ha_commit_trans(thd, false, true);
    return true;
  }

  if (UpdateDstoreIndexStats(thd, dbNameS, tableNameS,
                             dstoreStatsTables[1].table, table, index)) {
    ha_commit_trans(thd, false, true);
    return true;
  }

  if (ha_commit_trans(thd, false, true) != 0) {
    CDE_LOG_ERROR("UpdateDstoreStatisticsTables commit error, table:%s",
                  table->name.c_str());
    return true;
  }

  return false;
}

/**
Persist statistical information into the table.

@param[in,out]  table Pointer to the cde_dict_t object.
@param[in]      thd Connection contex.

@return 0 on success, -1 on error.
*/
int DstoreDictStatsSave(cde_dict_t *table, THD *thd, cde_dict_index_t *index) {
  CDE_ASSERT(table != nullptr);
  if (!table->dict_stats_persist_enabled() || table->is_temporary()) {
    return CDE_OK;
  }

  table->dict_stat_wr_lock();
  auto cleanup_guard = create_scope_guard([&] { table->dict_stat_unlock(); });

  if (UpdateDstoreStatisticsTables(table, thd, index)) return CDE_ERROR;
  return CDE_OK;
}

/**
Fetch table-level statistics from mysql.dstore_table_stats.

@param[in,out]  table Pointer to the cde_dict_t object.
@param[in]      dstoreStatsTable table Pointer to table:dstore_table_stats.
@param[in]      dbName Database name.
@param[in]      tableName Table name.

@return 0 on success, -1 on error.
*/
int FetchTableStats(cde_dict_t *table, TABLE *dstoreStatsTable,
                    const std::string &dbName, const std::string &tableName) {
  [[maybe_unused]] bool seen_row = false;
  return ForEachStatRow(
      dbName, tableName, dstoreStatsTable, DstoreStatTableEnum::TableStat, [&] {
        CDE_ASSERT(!seen_row);
        seen_row = true;
        table->stat_n_rows =
            dstoreStatsTable->field[DstoreTableStatColIdx::N_ROWS]->val_int();
        table->stat_heap_page_count =
            dstoreStatsTable->field[DstoreTableStatColIdx::N_HEAP_TABLE_PAGES]
                ->val_int();
        table->stat_all_indexs_page_count =
            dstoreStatsTable->field[DstoreTableStatColIdx::N_SUM_OF_INDEX_PAGES]
                ->val_int();
        table->stat_heap_free_page_count =
            dstoreStatsTable
                ->field[DstoreTableStatColIdx::N_HEAP_TABLE_FREE_PAGES]
                ->val_int();
        table->stat_heap_lob_page_count =
            dstoreStatsTable
                ->field[DstoreTableStatColIdx::N_HEAP_TABLE_LOB_PAGES]
                ->val_int();

        CDE_LOG_DEBUG(
            "FetchTableStats table:%s stat_n_rows:%lu page_count:%lu "
            "index_page_cnt:%lu",
            table->name.c_str(), table->stat_n_rows,
            table->stat_heap_page_count, table->stat_all_indexs_page_count);

        return false;
      });
}

/**
Obtain the suffix index value of the composite index n_diff_pfx0x.
e.g: in:n_diff_pfx01 -> out:1

@param[in]      str Index statistics name character with
                the prefix n_diff_pfx.
@param[in]      thd Connection contex.
@param[in]      dbName Database name.
@param[in]      tableName Table name.

@return -1 on error, other value on succ.
*/
int32_t ExtractIndexStatSuffixValue(const char *str) {
  const char *prefix = "n_diff_pfx";
  const size_t prefix_len = strlen(prefix);

  if (strlen(str) != prefix_len + 2) {
    return -1;
  }

  if (strncmp(str, prefix, prefix_len) != 0) {
    return -1;
  }

  char suffix[3] = {0};
  suffix[0] = str[prefix_len];
  suffix[1] = str[prefix_len + 1];

  if (!isdigit(suffix[0]) || !isdigit(suffix[1])) {
    return -1;
  }

  return std::stoi(std::string(suffix));
}

/**
Fetch index-level statistics from mysql.dstore_index_stats.

@param[in,out]  table Pointer to the cde_dict_t object.
@param[in]      dstoreStatsTable table Pointer to table:dstore_stats_index.
@param[in]      dbName Database name.
@param[in]      tableName Table name.

@return 0 on success, -1 on error.
*/
int FetchIndexStats(cde_dict_t *table, TABLE *dstoreStatsTable,
                    const std::string &dbName, const std::string &tableName) {
  StringBuffer<STRING_BUFFER_USUAL_SIZE> indexNameBuf;
  StringBuffer<STRING_BUFFER_USUAL_SIZE> statNameBuf;
  return ForEachStatRow(
      dbName, tableName, dstoreStatsTable, DstoreStatTableEnum::IndexStat, [&] {
        String *indexName =
            dstoreStatsTable->field[DstoreIndexStatColIdx::N_INDEX_NAME]
                ->val_str(&indexNameBuf);

        std::string indexNameStr =
            std::string(indexName->ptr(), indexName->length());
        cde_dict_index_t *index =
            table->get_index_on_name(indexNameStr.c_str());
        if (!index) {
          return true;
        }
        String *statName =
            dstoreStatsTable->field[DstoreIndexStatColIdx::N_STAT_NAME]
                ->val_str(&statNameBuf);
        std::string statNameStr(statName->ptr(), statName->length());

        const longlong statValue =
            dstoreStatsTable->field[DstoreIndexStatColIdx::N_STAT_VALUE]
                ->val_int();
        const longlong sample_size =
            dstoreStatsTable->field[DstoreIndexStatColIdx::N_SAMPLE_SIZE]
                ->val_int();
        if (std::strncmp(statNameStr.c_str(), "n_diff_pfx", 10) == 0) {
          int32_t suffix = ExtractIndexStatSuffixValue(statNameStr.c_str());
          CDE_LOG_DEBUG("FetchIndexStats n_diff_pfx suffix:%d", suffix);
          if (suffix == -1 ||
              (suffix - 1) >= static_cast<int32_t>(index->index_col_num)) {
            return true;
          }
          index->stat_n_diff_key_vals[suffix - 1] = statValue;
          index->stat_n_sample_size[suffix - 1] = sample_size;
        } else if (std::strcmp(statNameStr.c_str(), "n_leaf_pages") == 0) {
          index->stat_leaf_page_cnt = statValue;
        } else if (std::strcmp(statNameStr.c_str(), "size") == 0) {
          index->stat_index_page_cnt = statValue;
        } else {
          CDE_LOG_ERROR("invalid index str in FetchIndexStats");
          return true;
        }

        // keep consistency with init
        if (index->stat_leaf_page_cnt == 0) {
          index->stat_leaf_page_cnt = 1;
        }

        CDE_LOG_DEBUG("FetchIndexStats indexName:%s statName:%s statValue:%llu",
                      indexNameStr.c_str(), statNameStr.c_str(), statValue);

        return false;
      });
}

/**
The first time the dictionary statistics information is initialized.
It is retrieved from the persistent table.

@param[in,out]  table Pointer to the cde_dict_t object.
@param[in]      thd Connection contex.

@return 0 on success, -1 on error.
*/
int CdeDictStatsFetchFromPs(cde_dict_t *table, THD *thd) {
  CDE_ASSERT(table != nullptr);
  if (!table->dict_stats_persist_enabled() || table->is_temporary()) {
    return CDE_OK;
  }

  if (table->stat_initialized) {
    return CDE_OK;
  }

  table->dict_stat_wr_lock();
  auto cleanup_guard = create_scope_guard([&] { table->dict_stat_unlock(); });

  auto [dbNameS, tableNameS] = NormalizeTableAndSchemaName(table->name);

  if (tableNameS == "dstore_table_stats" ||
      tableNameS == "dstore_index_stats") {
    return CDE_OK;
  }

  Table_ref dstoreStatsTables[2] = {
      Table_ref("mysql", "dstore_table_stats", TL_READ),
      Table_ref("mysql", "dstore_index_stats", TL_READ)};
  dstoreStatsTables[0].next_global = dstoreStatsTables[0].next_local =
      dstoreStatsTables[0].next_name_resolution_table = &dstoreStatsTables[1];

  if (open_trans_system_tables_for_read(thd, dstoreStatsTables)) {
    CDE_LOG_ERROR("CdeDictStatsFetchFromPs open error, table:%s",
                  table->name.c_str());
    return CDE_ERROR;
  }

  const auto close_tables =
      create_scope_guard([thd]() { close_trans_system_tables(thd); });

  int ret =
      FetchTableStats(table, dstoreStatsTables[0].table, dbNameS, tableNameS);
  if (ret != CDE_OK) {
    return ret;
  }
  ret = FetchIndexStats(table, dstoreStatsTables[1].table, dbNameS, tableNameS);
  if (ret != CDE_OK) {
    return ret;
  }

  if (table->stat_heap_page_count == 0) {
    table->stat_heap_page_count = 1;
  }

  table->stat_initialized = true;
  table->stat_modified_counter = 0;
  return ret;
}

/**
Delete the record from the dstore statistical information persistence table.

@param[in]      dbTableName The name of the table to drop.
@param[in]      thd Connection contex.

@return 0 on success, -1 on error.
*/
int DstoreDictStatDropTable(const std::string &dbTableName, THD *thd) {
  if (dbTableName.empty()) {
    return CDE_ERROR;
  }

  Table_ref dstoreStatsTables[2] = {
      Table_ref("mysql", "dstore_table_stats", TL_WRITE),
      Table_ref("mysql", "dstore_index_stats", TL_WRITE)};
  dstoreStatsTables[0].next_global = dstoreStatsTables[0].next_local =
      dstoreStatsTables[0].next_name_resolution_table = &dstoreStatsTables[1];

  if (open_trans_system_tables_for_write(thd, dstoreStatsTables)) {
    CDE_LOG_ERROR("DstoreDictStatDropTable open error, table:%s",
                  dbTableName.c_str());
    return CDE_ERROR;
  }

  /* when bin log is disabled. this attachable transaction run the sub-statment.
    when setting next-gtid thiscommit it will invoke to save gtid. and then
    commit will invoke dstore commit. but the system table is innodb. and it
    cause conflict. and this statistics table is system table. it could be skip
    to save gtid. so in this sub-statement set this variable.
  */
  bool savedOperSubstatementimplicitly =
      thd->is_operating_substatement_implicitly;
  if (!opt_bin_log) {
    thd->is_operating_substatement_implicitly = true;
  }
  const auto close_tables =
      create_scope_guard([thd, savedOperSubstatementimplicitly]() {
        close_trans_system_tables(thd);
        if (!opt_bin_log) {
          thd->is_operating_substatement_implicitly =
              savedOperSubstatementimplicitly;
        }
      });

  auto [dbNameS, tableNameS] = NormalizeTableAndSchemaName(dbTableName);

  if (DeleteDstoreTableStats(dbNameS, tableNameS, dstoreStatsTables[0].table,
                             DstoreStatTableEnum::TableStat) ||
      DeleteDstoreTableStats(dbNameS, tableNameS, dstoreStatsTables[1].table,
                             DstoreStatTableEnum::IndexStat) ||
      DBUG_EVALUATE_IF("simulate_persist_dictstat_droptable_failed", true,
                       false)) {
    CDE_LOG_ERROR("DstoreDictStatDropTable error, table:%s",
                  dbTableName.c_str());
    ha_commit_trans(thd, false, true);
    return CDE_ERROR;
  }

  if (ha_commit_trans(thd, false, true) != 0) {
    CDE_LOG_ERROR("DstoreDictStatDropTable commit error, table:%s",
                  dbTableName.c_str());
  }

  return CDE_OK;
}

/**
Modify the table name in the statistical information persistence table.

@param[in]      thd Connection contex.
@param[in]      form The current table name.
@param[in]      to The new table name.

@return 0 on success, -1 on error.
*/
int DstoreDictStatRenameTable(THD *thd, const std::string &from,
                              const std::string &to) {
  if (from.empty() || to.empty()) {
    return CDE_ERROR;
  }

  Table_ref dstoreStatsTables[2] = {
      Table_ref("mysql", "dstore_table_stats", TL_WRITE),
      Table_ref("mysql", "dstore_index_stats", TL_WRITE)};
  dstoreStatsTables[0].next_global = dstoreStatsTables[0].next_local =
      dstoreStatsTables[0].next_name_resolution_table = &dstoreStatsTables[1];

  if (open_trans_system_tables_for_write(thd, dstoreStatsTables)) {
    CDE_LOG_ERROR("DstoreDictStatRenameTable open error, from:%s to:%s",
                  from.c_str(), to.c_str());
    return CDE_ERROR;
  }

  /* when bin log is disabled. this attachable transaction run the sub-statment.
  when setting next-gtid thiscommit it will invoke to save gtid. and then commit
  will invoke dstore commit. but the system table is innodb. and it cause
  conflict. and this statistics table is system table. it could be skip to save
  gtid. so in this sub-statement set this variable.
*/
  bool savedOperSubstatementimplicitly =
      thd->is_operating_substatement_implicitly;
  if (!opt_bin_log) {
    thd->is_operating_substatement_implicitly = true;
  }

  const auto close_tables =
      create_scope_guard([thd, savedOperSubstatementimplicitly]() {
        close_trans_system_tables(thd);
        if (!opt_bin_log) {
          thd->is_operating_substatement_implicitly =
              savedOperSubstatementimplicitly;
        }
      });

  auto [fDbNameS, fTableNameS] = NormalizeTableAndSchemaName(from);

  auto [tDbNameS, tTableNameS] = NormalizeTableAndSchemaName(to);

  if (RenameDstoreTableStats(fDbNameS, fTableNameS, tDbNameS, tTableNameS,
                             dstoreStatsTables[0].table,
                             DstoreStatTableEnum::TableStat) ||
      RenameDstoreTableStats(fDbNameS, fTableNameS, tDbNameS, tTableNameS,
                             dstoreStatsTables[1].table,
                             DstoreStatTableEnum::IndexStat) ||
      DBUG_EVALUATE_IF("simulate_persist_dictstat_renametable_failed", true,
                       false)) {
    ha_commit_trans(thd, false, true);
    return CDE_ERROR;
  }

  if (ha_commit_trans(thd, false, true) != 0) {
    CDE_LOG_ERROR("DstoreDictStatRenameTable error, from:%s to:%s",
                  from.c_str(), to.c_str());
    return CDE_ERROR;
  }

  return CDE_OK;
}

int DictStatDropIndex(THD *thd, cde_dict_t *dictTable,
                      const std::string &indexName) {
  if (!dictTable->dict_stats_persist_enabled()) {
    return CDE_OK;
  }

  const std::string &tableName = dictTable->name.c_str();

  if (tableName.empty() || indexName.empty()) {
    return CDE_ERROR;
  }

  Table_ref dstoreStatsTables[2] = {
      Table_ref("mysql", "dstore_table_stats", TL_WRITE),
      Table_ref("mysql", "dstore_index_stats", TL_WRITE)};
  dstoreStatsTables[0].next_global = dstoreStatsTables[0].next_local =
      dstoreStatsTables[0].next_name_resolution_table = &dstoreStatsTables[1];

  auto [dbName, tbName] = NormalizeTableAndSchemaName(tableName);

  if (open_trans_system_tables_for_write(thd, dstoreStatsTables)) {
    CDE_LOG_ERROR("DictStatDropIndex open error, tableName:%s indexName:%s",
                  tableName.c_str(), indexName.c_str());
    return CDE_ERROR;
  }

  /* when bin log is disabled. this attachable transaction run the sub-statment.
  when setting next-gtid thiscommit it will invoke to save gtid. and then commit
  will invoke dstore commit. but the system table is innodb. and it cause
  conflict. and this statistics table is system table. it could be skip to save
  gtid. so in this sub-statement set this variable.
*/
  bool savedOperSubstatementimplicitly =
      thd->is_operating_substatement_implicitly;
  if (!opt_bin_log) {
    thd->is_operating_substatement_implicitly = true;
  }

  const auto close_tables =
      create_scope_guard([thd, savedOperSubstatementimplicitly]() {
        close_trans_system_tables(thd);
        if (!opt_bin_log) {
          thd->is_operating_substatement_implicitly =
              savedOperSubstatementimplicitly;
        }
      });

  if (dictTable->auto_recalc_enabled()) {
    if (UpdateDstoreTableStats(thd, dbName, tbName, dstoreStatsTables[0].table,
                               dictTable)) {
      ha_commit_trans(thd, false, true);
      return true;
    }
  }

  if (DeleteDstoreIndexStats(dbName, tbName, indexName,
                             dstoreStatsTables[1].table)) {
    ha_commit_trans(thd, false, true);
    return CDE_ERROR;
  }

  if (ha_commit_trans(thd, false, true) != 0) {
    CDE_LOG_ERROR("DictStatDropIndex commit error, tableName:%s indexName:%s",
                  tableName.c_str(), indexName.c_str());
    return CDE_ERROR;
  }

  return CDE_OK;
}

int DictStatRenameIndex(THD *thd, const std::string &tableName,
                        const std::string &oldIndexName,
                        const std::string &newIndexName) {
  if (tableName.empty() || oldIndexName.empty() || newIndexName.empty()) {
    return CDE_ERROR;
  }

  Table_ref indexStatTable = Table_ref("mysql", "dstore_index_stats", TL_WRITE);
  if (open_trans_system_tables_for_write(thd, &indexStatTable)) {
    CDE_LOG_ERROR(
        "DictStatRenameIndex open error, tableName:%s oldIndexName:%s "
        "newIndexName:%s",
        tableName.c_str(), oldIndexName.c_str(), newIndexName.c_str());
    return CDE_ERROR;
  }

  /* when bin log is disabled. this attachable transaction run the sub-statment.
  when setting next-gtid thiscommit it will invoke to save gtid. and then commit
  will invoke dstore commit. but the system table is innodb. and it cause
  conflict. and this statistics table is system table. it could be skip to save
  gtid. so in this sub-statement set this variable.
*/
  bool savedOperSubstatementimplicitly =
      thd->is_operating_substatement_implicitly;
  if (!opt_bin_log) {
    thd->is_operating_substatement_implicitly = true;
  }

  const auto close_tables =
      create_scope_guard([thd, savedOperSubstatementimplicitly]() {
        close_trans_system_tables(thd);
        if (!opt_bin_log) {
          thd->is_operating_substatement_implicitly =
              savedOperSubstatementimplicitly;
        }
      });

  auto [dbName, tbName] = NormalizeTableAndSchemaName(tableName);

  if (RenameDstoreIndexStats(dbName, tbName, oldIndexName, newIndexName,
                             indexStatTable.table)) {
    ha_commit_trans(thd, false, true);
    return CDE_ERROR;
  }

  if (ha_commit_trans(thd, false, true) != 0) {
    CDE_LOG_ERROR(
        "DictStatRenameIndex error, tableName:%s oldIndexName:%s "
        "newIndexName:%s",
        tableName.c_str(), oldIndexName.c_str(), newIndexName.c_str());
    return CDE_ERROR;
  }

  return CDE_OK;
}

int CdeDictTableStatsUpdate(cde_dict_t *table, stat_update_option opt,
                            THD *thd) {
  if (table == nullptr) {
    return CDE_ERROR;
  }

  switch (opt) {
    case EMPTY_TABLE: {
      int err = CdeDictStatsReset(table);
      if (err != CDE_OK) {
        return err;
      }
      // UT scenario create table not persist
      if (thd == nullptr) {
        return CDE_OK;
      }
      return DstoreDictStatsSave(table, thd);
    }
    case RECALC_STATS:
      return CdeDictTableStatsRecalc(thd, table);
    case RECALC_PERSIST: {
      int err = CdeDictTableStatsRecalc(thd, table);
      if (err != CDE_OK) {
        return err;
      }
      return DstoreDictStatsSave(table, thd);
    }
    case FETCH_STATS_FROM_PERSIST: {
      return CdeDictStatsFetchFromPs(table, thd);
    }
    default:
      return CDE_ERROR;
  }
  return CDE_OK;
}

void DstoreDictStatsUpdateForIndex(cde_dict_t *table, cde_dict_index_t *index,
                                   THD *thd) {
  table->dict_stat_wr_lock();
  uint64_t indexSamplePageNums =
      table->dict_stats_persist_enabled()
          ? table->get_persistent_index_sample_pages()
          : table->get_transient_index_sample_pages();
  if (CdeDictIndexStatsCalc(index, indexSamplePageNums) == false) {
    CDE_LOG_ERROR("index stats calc fail for index: %s", index->name.c_str());
  }

  table->stat_all_indexs_page_count += index->stat_index_page_cnt;
  table->dict_stat_unlock();
  if (!table->dict_stats_persist_enabled()) {
    return;
  }

  if (DstoreDictStatsSave(table, thd, index) != CDE_OK) {
    CDE_LOG_ERROR("Dict stats save fail for index: %s", index->name.c_str());
  }
}

} /* namespace CDE */
