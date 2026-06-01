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

#include <cstdint>
#include <mutex>
#include <numeric>
#include <unordered_map>

#include <regex>
#include "handler/ha_cde_cond.h"
#include "mysql/plugin.h"
#include "scope_guard.h"
#include "sql/dd/dictionary.h"

#include "sql/field.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/sql_class.h"
#include "sql/sql_show.h"
#include "sql/sql_thd_internal_api.h"
#include "sql/table.h"

#include "diagnose/dstore_buf_mgr_diagnose.h"
#include "diagnose/dstore_index_diagnose.h"
#include "diagnose/dstore_lock_mgr_diagnose.h"
#include "diagnose/dstore_memory_diagnose.h"
#include "diagnose/dstore_tablespace_diagnose.h"
#include "diagnose/dstore_transaction_diagnose.h"
#include "diagnose/dstore_undo_mgr_diagnose.h"
#include "framework/dstore_instance_interface.h"

#include "cde_information_schema.h"
#include "common/cde_compare_utils.h"
#include "common/cde_mutex.h"
#include "common/cde_perf.h"
#include "common/cde_session.h"
#include "common/cde_trxmgr.h"
#include "handler/ha_cde_cond.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/dictionary.h"
#include "sql/dd/types/partition.h"
#include "sql/dd/types/partition_index.h"
#include "sql/dd/types/table.h"
#include "storage/dstore/ddl/cde_dd_table.h"
#include "storage/dstore/ddl/cde_ddl.h"
#include "storage/dstore/ddl/cde_online_ddl.h"
#include "storage/dstore/ddl/cde_tablespace.h"

namespace CDE {

static constexpr char PLUGIN_AUTHOR[] = PLUGIN_AUTHOR_HUAWEI;
static constexpr char I_S_DSTORE_TRX_TABLE_NAME[] = "DSTORE_TRX";
static constexpr char I_S_DSTORE_BUFFER_POOL_STATS_TABLE_NAME[] =
    "DSTORE_BUFFER_POOL_STATS";
static constexpr char I_S_DSTORE_LOCKS_TABLE_NAME[] = "DSTORE_LOCKS";
static constexpr char I_S_DSTORE_MEM_STATS_TABLE_NAME[] = "DSTORE_MEM";
static constexpr char I_S_DSTORE_SEGMENT_STATS_TABLE_NAME[] = "DSTORE_SEGMENT";
static constexpr char I_S_DSTORE_UNDO_TABLE_NAME[] = "DSTORE_UNDO";
static constexpr char I_S_DSTORE_INDEXES_TABLE_NAME[] = "DSTORE_INDEXES";
static constexpr char HEAP_SEGMENT[] = "HEAP";
static constexpr char INDEX_SEGMENT[] = "INDEX";
static constexpr char I_S_DSTORE_TABLESPACE_NAME[] = "DSTORE_TABLESPACES";
static constexpr char I_S_DSTORE_ONLINE_DDL_PROGRESS_TABLE_NAME[] =
    "DSTORE_ONLINE_DDL_PROGRESS";
static constexpr char NULL_STRING[] = "NULL";

static constexpr uint32_t TRX_I_S_TRX_STATE_MAX_LEN = 10;
static constexpr uint32_t TRX_I_S_ISOLATION_LEVEL_MAX_LEN = 32;
static constexpr uint32_t TRX_I_S_QUERY_STMT_MAX_LEN = 512;

static constexpr uint32_t LOCKS_I_S_STATE_MAX_LEN = 10;
static constexpr uint32_t LOCKS_I_S_DESC_MAX_LEN = DSTORE::LOCKDESCLEN;
static constexpr uint32_t SEGMENT_TYPE_MAX_LEN = 10;
static constexpr uint32_t PAGE_ID_MAX_LEN = 20;
static constexpr uint32_t UNDO_I_S_STATE_MAX_LEN = 32;
static constexpr uint32_t INDEXES_I_S_LEVEL_INFO_MAX_LEN =
    DSTORE::INDEX_DESC_MAX_LEN;

/* Characters shown for the command in 'information_schema.online_ddl_progress'
 */
static constexpr uint32_t ONLINE_DDL_I_S_PROCESS_QUERY_STMT_LEN = 65535;
/* Characters shown for the state in 'information_schema.online_ddl_progress' */
static constexpr uint32_t ONLINE_DDL_I_S_PROCESS_STATUS_LEN = 64;

static constexpr uint64_t INVALID_TABLEID = UINT64_MAX;

/**
Process a heap segment of a partitioned table via dd::Partition
*/
static int DstoreProcessPartHeapSegment(const dd::Partition *part,
                                        const dd::String_type &dbName,
                                        const dd::String_type &tableName,
                                        int fillFactor, THD *thd,
                                        TABLE *outTable);

/**
Process a partitioned table index via dd::Partition_index and populate the
IS.dstore_segment table
*/
static int DstoreProcessPartitionIndex(const dd::Partition_index *partIndex,
                                       const dd::String_type &dbName,
                                       const dd::String_type &tableName,
                                       const dd::String_type &partitionName,
                                       THD *thd, TABLE *outTable);

/**
Process a partitioned table index via dd::Partition_index and populate the
IS.dstore_indexes table
*/
static int DstoreProcessPartitionIndexForIndexTable(
    const dd::Partition_index *partIndex, const dd::String_type &dbName,
    const dd::String_type &tableName, const dd::String_type &partitionName,
    THD *thd, TABLE *outTable);

struct SegmentFilterConditions;

/**
Process a partitioned table index via dd::Table, if isDstoreIndexTable is
true, populate the IS.dstore_indexes table, else populate the
IS.dstore_segment table
*/
static int DstoreProcessSinglePartitionTable(
    THD *thd, dd::cache::Dictionary_client *dc, const dd::Table *ddTable,
    TABLE *outTable, const SegmentFilterConditions &filters,
    bool isDstoreIndexTable = false);

/**
used for IS.dstore_segment and IS.dstore_indexes
*/
void IncPerfCountForPushDown(bool isDstoreIndexTable) {
  if (isDstoreIndexTable) {
    cde_perf_counters::getInstance()->_dstore_indexes_pushdown_hit.increment();
  } else {
    cde_perf_counters::getInstance()->_dstore_segment_pushdown_hit.increment();
  }
}

#define CHECK_STORE_RET(expr) \
  if ((expr) != 0) {          \
    return CDE_ERROR;         \
  }

#define END_OF_ST_FIELD \
  { nullptr, 0, MYSQL_TYPE_NULL, 0, 0, "", 0 }

/** Extra transaction info get from server's thd */
struct ExtraArg {
  /** Session id of this connection */
  uint64_t sessionId{0};
  /** Query statement of current transaction */
  const char *queryStmt{nullptr};
};

enum MemoryContextType {
  LongLive,
  StorageManager,
  Error,
  BufferMgr,
  Transaction,
  LockMgr,
  PerQuery,
  Stack,
  StorageThreadTop,
  Others,
  MAX
};

struct MemoryStats {
  uint64_t alloced{0};
  uint64_t used{0};
};

/** Aggregate the context results based on threads. */
struct ThreadMemoryDetailAggResult {
  uint64_t os_thread_id{0};
  MemoryStats contextStats[MAX]{};
};

static const std::unordered_map<std::string, MemoryContextType> ctxMap = {
    {"LongLiveMemoryContext", LongLive},
    {"StorageManagerMemoryContext", StorageManager},
    {"ErrorMemoryContext", Error},
    {"BufferMgrMemoryContext", BufferMgr},
    {"TransactionMemoryContext", Transaction},
    {"LockMgrMemoryContext", LockMgr},
    {"PerQueryMemoryContext", PerQuery},
    {"StackMemoryContext", Stack},
    {"StorageThreadTopMemoryContext", StorageThreadTop}};

/**
Thread transaction callback, used to get extra info from thd.
Note that the memory allocted in this func is released in i_s
dstore_trx process.

@param[in]      arg      Caller's paras, right now is thd
@param[in/out]  output   Extra info get from current thd

@return 0 on success
*/
int ThreadTransactionCallback(void *arg, void **output) {
  if (!arg || !output) {
    return CDE_ERROR;
  }
  THD *thd = static_cast<THD *>(arg);

  size_t strSize = ALIGN_SIZE(sizeof(ExtraArg));
  size_t totalSize = strSize + TRX_I_S_QUERY_STMT_MAX_LEN;
  /* NOTE:Memory is released in trx i_s table process */
  void *tmp = static_cast<void *>(malloc(totalSize));
  if (!tmp) {
    return CDE_ERROR;
  }
  ExtraArg *pArg = static_cast<ExtraArg *>(tmp);

  char *ptr = static_cast<char *>(tmp);
  ptr += strSize;
  size_t stmtLen = thd_query_safe(thd, ptr, TRX_I_S_QUERY_STMT_MAX_LEN);

  pArg->queryStmt = (stmtLen > 0) ? (char *)ptr : nullptr;
  pArg->sessionId = thd->thread_id();

  *output = tmp;
  return CDE_OK;
}

/**
Store string value to mysql field

@param[in]      field  target field to store i_s value
@param[in]      str    null-terminated string, or NULL

@return 0 on success and failure for other value
*/
static int FieldStoreString(Field *field, const char *str) {
  int ret = CDE_ERROR;
  if (str) {
    ret = field->store(str, static_cast<size_t>(strlen(str)),
                       system_charset_info);
    field->set_notnull();
  } else {
    ret = 0;
    field->set_null();
  }
  return ret;
}

/** The trx i_s table field id enum, note that the order of this enum must
be the same as the array g_dstoreTrxFieldsInfo define. */
enum DstoreTrxField {
  SESSION_ID = 0,
  THREAD_ID,
  TRX_XID,
  STATE,
  TRX_DURATION,
  ISOLATION_LEVEL,
  IS_AUTONOMOUS,
  TRX_QUERY
};

static ST_FIELD_INFO g_dstoreTrxFieldsInfo[] = {
    {"session_id", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"os_thread_id", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"xid", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"state", TRX_I_S_TRX_STATE_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"trx_duration", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"isolation_level", TRX_I_S_ISOLATION_LEVEL_MAX_LEN, MYSQL_TYPE_STRING, 0,
     0, "", 0},
    {"is_autonomous", 1, MYSQL_TYPE_LONG, 0, 0, "", 0},
    {"trx_query", TRX_I_S_QUERY_STMT_MAX_LEN, MYSQL_TYPE_STRING, 0,
     MY_I_S_MAYBE_NULL, "", 0},
    END_OF_ST_FIELD};

struct DstoreTrxMgr {
  /* To protect access concurrent */
  std::mutex m_mtx;
};

static DstoreTrxMgr trxMgr;

/**
Convert isolation level to string

@param[in]      level   Isolation level from dstore

@return string of the related isolation level
*/
static const char *IsolationLevelAsString(int level) {
  static const char *names[] = {"READ_UNCOMMITED", "READ_COMMITTED",
                                "TRANSACTION_SNAPSHOT", "SERIALIZABLE",
                                "UNSUPPORTED"};
  return level >= cde_isolation_level_t::READ_UNCOMMITED &&
                 level <= cde_isolation_level_t::SERIALIZABLE
             ? names[level]
             : "UNSUPPORTED";
}

static std::unordered_map<std::string, std::string> BuildContextParentMap(
    DSTORE::ThreadMemoryDetail **detailsArray, uint32_t *detailsArrayLen) {
  std::unordered_map<std::string, std::string> parentMap;

  for (uint32_t i = 0; i < *detailsArrayLen; ++i) {
    const auto &detail = (*detailsArray)[i];
    if (*detail.contextName != '\0' && *detail.parent != '\0') {
      parentMap[std::string(detail.contextName)] = std::string(detail.parent);
    }
  }

  return parentMap;
}

static std::vector<MemoryContextType> GetAllRelatedTypes(
    const char *ctxName,
    const std::unordered_map<std::string, std::string> &parentMap) {
  std::vector<MemoryContextType> types;
  std::unordered_set<std::string> visited;

  std::string currentCtx = (ctxName != nullptr) ? std::string(ctxName) : "";

  while (!currentCtx.empty() && !visited.count(currentCtx)) {
    visited.insert(currentCtx);

    auto it = ctxMap.find(currentCtx);
    if (it != ctxMap.end()) {
      types.push_back(it->second);
    }

    auto parentIt = parentMap.find(currentCtx);
    if (parentIt != parentMap.end()) {
      currentCtx = parentIt->second;
    } else {
      break;
    }
  }

  return types;
}

static void UpdateContextStats(
    const char *ctxName,
    const std::unordered_map<std::string, std::string> &parentMap,
    ThreadMemoryDetailAggResult &result, uint64_t total, uint64_t used) {
  auto types = GetAllRelatedTypes(ctxName, parentMap);
  bool isKnownType = (ctxName != nullptr) &&
                     (ctxMap.find(std::string(ctxName)) != ctxMap.end());

  /* 1. Enumerated node: already in 'types', skip.
     2. Non-enumerated node:
        Has ancestor enums (types not empty): add only ancestors, skip Others.
        No ancestor enums (types empty): add Others (avoid data loss). */
  if (!isKnownType && ctxName != nullptr && *ctxName != '\0' && types.empty()) {
    types.push_back(Others);
  }

  for (auto type : types) {
    result.contextStats[type].alloced += total;
    result.contextStats[type].used += used;
  }
}

static void AggregateThreadMemoryDetails(
    DSTORE::ThreadMemoryDetail **detailsArray, uint32_t *detailsArrayLen,
    std::unordered_map<uint64_t, ThreadMemoryDetailAggResult> &aggMap) {
  auto parentMap = BuildContextParentMap(detailsArray, detailsArrayLen);

  for (uint32_t i = 0; i < *detailsArrayLen; ++i) {
    const auto &detail = (*detailsArray)[i];
    auto &result = aggMap[detail.tid];
    result.os_thread_id = detail.tid;

    UpdateContextStats(detail.contextName, parentMap, result, detail.totalSize,
                       detail.usedSize);
  }
}

struct FilterVal {
  bool m_isNull;      // IsNull flag of the field.
  bool m_isUnsigned;  // Unsigned flag of the field.
  Datum m_val;        // Can use directly if field is numeric type, need special
                      // process if type is varchar.
  const CHARSET_INFO *m_csetInfo;  // Pointer to charset info for varchar.
};

/* <filter column index, filter value>, only support EQ_FUNC right now */
using FilterSet = std::unordered_map<uint32_t, FilterVal>;

/**
Get filter info, note that only support cols EQ combine with AND condition,
and it depends on CdeCondHandler impl, need check whether will impact i_s
condition pushdown when modify CdeCondHandler.

@param[in]      item       The where conditions for i_s table
@param[in]      thd        Session thd pointer
@param[in]      filterCols Array of column index want to filter
@param[in/out]  outInfo    Set of filter info
*/
void FilterPushdownCond(Item *item, THD *thd,
                        const std::vector<int> &filterCols,
                        FilterSet &outInfo) {
  if (!thd->optimizer_switch_flag(OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN)) {
    return;
  }
  CdeCondHandler condHandler;
  condHandler.PrepareCondPush(item, thd->mem_root);
  auto conds = condHandler.GetCondList();
  if (!conds.is_empty()) {
    List_iterator<CondField> iter(conds);
    CondField *cond;
    while ((cond = iter++)) {
      if (cond->m_funcType != Item_func::EQ_FUNC) {
        continue;
      }
      for (auto &idx : filterCols) {
        /* Only support EQ, so can break loop directly if match any */
        if (cond->m_fieldNo == idx) {
          FilterVal val;
          val.m_isNull = cond->m_isNull;
          val.m_isUnsigned = cond->m_isUnsigned;
          val.m_val = cond->m_fieldValue;
          outInfo[idx] = val;
          break;
        }
      }
    }
    condHandler.CleanUpResources();
  }
}

static void GetFilterThreadId(FilterSet &filterInfo, uint64_t &filterThreadId) {
  /* If thread_id and session_id both set, filter thread_id is enough. */
  if (filterInfo.count(DstoreTrxField::THREAD_ID)) {
    filterThreadId = filterInfo[DstoreTrxField::THREAD_ID].m_val;
  } else if (filterInfo.count(DstoreTrxField::SESSION_ID)) {
    uint64_t sessionId = filterInfo[DstoreTrxField::SESSION_ID].m_val;
    Find_thd_with_id find_thd_with_id(sessionId);
    THD_ptr thdPtr =
        Global_THD_manager::get_instance()->find_thd(&find_thd_with_id);
    {
      if (thdPtr) {
        filterThreadId = thdPtr.get()->real_id;
      }
    }
  }
}

/**
Function to fill INFORMATION_SCHEMA.dstore_trx

@param[in]      thd
@param[in]      tables
@param[in]      item

@return 0 on success
*/
static int DstoreTrxFillTable(THD *thd, Table_ref *tables, Item *item) {
  const char *tableName = tables->table_name;

  if (CdeStrcasecmp(tableName, I_S_DSTORE_TRX_TABLE_NAME) != 0) {
    return CDE_ERROR;
  }

  /* deny access to non-superusers */
  if (check_global_access(thd, PROCESS_ACL)) {
    return CDE_OK;
  }

  (void)CdeCreateOrGetSession(thd);

  FilterSet filterInfo;
  if (item) {
    std::vector<int> filterIndex{DstoreTrxField::SESSION_ID,
                                 DstoreTrxField::THREAD_ID};
    FilterPushdownCond(item, thd, filterIndex, filterInfo);
  }

  uint64_t filterThreadId{std::numeric_limits<uint64_t>::max()};
  GetFilterThreadId(filterInfo, filterThreadId);

  std::list<DSTORE::TransactionInfo> infoList;
  auto CleanupGuard = create_scope_guard([&infoList] {
    for (const auto &node : infoList) {
      if (node.extraInfo) {
        free(node.extraInfo);
      }
    }
    infoList.clear();
  });

  DSTORE::TransactionDiagnose trxDiag;
  Field **fields = tables->table->field;
  /* Use mutex to prevent concurrent execute here. */
  std::unique_lock<std::mutex> lck(trxMgr.m_mtx);

  /* Invoke diagnose api to collect transaction info */
  DSTORE::RetStatus ret{DSTORE::DSTORE_SUCC};
  if (filterInfo.size() > 0) {
    cde_perf_counters::getInstance()
        ->_dstore_trx_thread_id_pushdown_hit.increment();
    ret = trxDiag.GetTrxInfoByThread(infoList, filterThreadId);
  } else {
    ret = trxDiag.GetAllThreadsTrxInfo(infoList);
  }

  if (ret != DSTORE::DSTORE_SUCC) {
    CDE_LOG_ERROR("Get thread trx info from dstore failed.");
    return CDE_OK;
  }

  for (const auto &info : infoList) {
    /* Process extra info get from thd, and info maybe null if trx is start
    internally in dstore, here we just set invalid value in this situation */
    uint64_t realSessId{0};
    const char *query{nullptr};

    if (info.extraInfo) {
      ExtraArg *arg = static_cast<ExtraArg *>(info.extraInfo);
      realSessId = arg->sessionId;
      query = arg->queryStmt;
    }

    CHECK_STORE_RET(fields[SESSION_ID]->store(realSessId, true));
    CHECK_STORE_RET(FieldStoreString(fields[TRX_QUERY], query));

    CHECK_STORE_RET(fields[TRX_XID]->store(info.xid, true));
    CHECK_STORE_RET(fields[THREAD_ID]->store(info.threadId, true));

    CHECK_STORE_RET(FieldStoreString(fields[STATE], info.state));
    /* Value get from dstore is transaction duration in microsecond */
    CHECK_STORE_RET(
        fields[TRX_DURATION]->store(info.duration / (1000 * 1000), true));

    CHECK_STORE_RET(FieldStoreString(
        fields[ISOLATION_LEVEL], IsolationLevelAsString(info.isolationLevel)));
    CHECK_STORE_RET(fields[IS_AUTONOMOUS]->store(info.isAutonomous, true));

    CHECK_STORE_RET(schema_table_store_record(thd, tables->table));
  }
  return CDE_OK;
}

/**
Bind the dynamic table INFORMATION_SCHEMA.dstore_trx

@param[in/out]      p   schema table to init

@return 0 on success
*/
static int DstoreTrxInit(void *p) {
  if (!p) {
    return CDE_ERROR;
  }
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;

  schema->fields_info = g_dstoreTrxFieldsInfo;
  schema->fill_table = DstoreTrxFillTable;
  return CDE_OK;
}

static struct st_mysql_information_schema g_infomationSchemaInfo = {
    MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};

struct st_mysql_plugin g_infomationSchemaDstoreTrx = {
    /* the plugin type (a MYSQL_XXX_PLUGIN value) */
    /* int */
    MYSQL_INFORMATION_SCHEMA_PLUGIN,

    /* pointer to type-specific plugin descriptor */
    /* void* */
    &g_infomationSchemaInfo,

    /* plugin name */
    /* const char* */
    I_S_DSTORE_TRX_TABLE_NAME,

    /* plugin author (for SHOW PLUGINS) */
    /* const char* */
    PLUGIN_AUTHOR,

    /* general descriptive text (for SHOW PLUGINS) */
    /* const char* */
    "DStore transactions",

    /* the plugin license (PLUGIN_LICENSE_XXX) */
    /* int */
    PLUGIN_LICENSE_GPL,

    /* the function to invoke when plugin is loaded */
    /* int (*)(void*); */
    DstoreTrxInit,

    /* the function to invoke when plugin is un installed */
    /* int (*)(void*); */
    nullptr,

    /* the function to invoke when plugin is unloaded */
    /* int (*)(void*); */
    nullptr,

    /* plugin version (for SHOW PLUGINS) */
    /* unsigned int */
    1,

    /* SHOW_VAR* */
    nullptr,

    /* SYS_VAR** */
    nullptr,

    /* reserved for dependency checking */
    /* void* */
    nullptr,

    /* Plugin flags */
    /* unsigned long */
    0UL,
};

/** The DSTORE_BUFFER_POOL_STATS i_s table field id enum, note that the order of
this enum must be the same as the array g_dstoreBufferPoolStatsFieldsInfo
define. */
enum DstoreBufferPoolStatsField {
  LRU_PARTITION_SIZE = 0,
  POOL_SIZE,
  CANDIDATE_BUFFERS,
  HOT_DATABASE_PAGES,
  LRU_DATABASE_PAGES,
  NUMBER_DIRTY_PAGES,
  NUMBER_LAST_DIRTY_PAGES_REMOVE,
  NUMBER_LAST_DIRTY_PAGES_FLUSH,
  NUMBER_PUSH_QUEUE_TOTAL,
  NUMBER_REMOVE_QUEUE_TOTAL,
  DIRTY_PAGES_LOW_WATERMARK,
  DIRTY_PAGES_HIGH_WATERMARK,
  CURRENT_DISK_SLAVE_WRITER_NUM,
  CURRENT_FLUSH_BATCH_NUM,
  CURRENT_DISK_MASTER_WRITER_SLEEP_MS,
  PAGES_MADE_HOT,
  PAGES_REMOVE_HOT,
  PAGES_MADE_HOT_RATE,
  PAGES_REMOVE_HOT_RATE,
  HOT_MAKE_PER_THOUSAND_GETS,
  HOT_REMOVE_PER_THOUSAND_GETS,
  PAGES_LRU_ADD,
  PAGES_LRU_REMOVE,
  PAGES_LRU_MOVE,
  PAGES_LRU_ADD_RATE,
  PAGES_LRU_REMOVE_RATE,
  PAGES_LRU_MOVE_RATE,
  PAGES_CANDIDATE_ADD,
  PAGES_CANDIDATE_REMOVE,
  PAGES_CANDIDATE_MISS,
  PAGES_CANDIDATE_ADD_RATE,
  PAGES_CANDIDATE_REMOVE_RATE,
  PAGES_CANDIDATE_MISS_RATE,
  NUMBER_PAGES_READ,
  NUMBER_PAGES_WRITTEN,
  NUMBER_BUFFERS_ALLOC,
  NUMBER_BUFFERS_REMOVE,
  PAGES_READ_RATE,
  PAGES_WRITTEN_RATE,
  BUFFERS_ALLOC_RATE,
  BUFFERS_REMOVE_RATE,
  NUMBER_PAGES_GET,
  NUMBER_PAGES_HIT,
  HIT_RATE,
  NUMBER_CR_BUFFERS_ALLOC,
  NUMBER_CR_BUFFERS_BUILD,
  NUMBER_CR_BUFFERS_REUSE,
  NUMBER_CR_BUFFERS_GET,
  NUMBER_CR_BUFFERS_HIT,
  CR_HIT_RATE,
  TIME_AFTER_COUNTER_RESET,
  NUMBER_PAGES_READ_AFTER_COUNTER_RESET,
  AVERAGE_READ_LATENCY_AFTER_COUNTER_RESET,
  MIN_READ_LATENCY_AFTER_COUNTER_RESET,
  MAX_READ_LATENCY_AFTER_COUNTER_RESET,
  NUMBER_PAGES_WRITE_AFTER_COUNTER_RESET,
  AVERAGE_WRITE_LATENCY_AFTER_COUNTER_RESET,
  MIN_WRITE_LATENCY_AFTER_COUNTER_RESET,
  MAX_WRITE_LATENCY_AFTER_COUNTER_RESET
};

static ST_FIELD_INFO g_dstoreBufferPoolStatsFieldsInfo[] = {
    {"lru_partition_size", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"pool_size", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"candidate_buffers", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"hot_database_pages", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"lru_database_pages", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"number_dirty_pages", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"number_last_dirty_pages_remove", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"number_last_dirty_pages_flush", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"number_push_queue_total", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"number_remove_queue_total", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"dirty_pages_low_watermark", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG,
     0, MY_I_S_UNSIGNED, "", 0},
    {"dirty_pages_high_watermark", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG,
     0, MY_I_S_UNSIGNED, "", 0},
    {"current_disk_slave_writer_num", MY_INT32_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"current_flush_batch_num", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"current_disk_master_writer_sleep_ms", MY_INT32_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"pages_made_hot", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"pages_remove_hot", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"pages_made_hot_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0, "",
     0},
    {"pages_remove_hot_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0, "",
     0},
    {"hot_make_per_thousand_gets", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"hot_remove_per_thousand_gets", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"pages_lru_add", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"pages_lru_remove", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"pages_lru_move", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"pages_lru_add_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0, "", 0},
    {"pages_lru_remove_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0, "",
     0},
    {"pages_lru_move_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0, "",
     0},
    {"pages_candidate_add", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"pages_candidate_remove", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
     0, MY_I_S_UNSIGNED, "", 0},
    {"pages_candidate_miss", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
     0, MY_I_S_UNSIGNED, "", 0},
    {"pages_candidate_add_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0,
     "", 0},
    {"pages_candidate_remove_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0,
     0, "", 0},
    {"pages_candidate_miss_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0,
     "", 0},
    {"number_pages_read", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"number_pages_written", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
     0, MY_I_S_UNSIGNED, "", 0},
    {"numbers_buffers_alloc", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
     0, MY_I_S_UNSIGNED, "", 0},
    {"numbers_buffers_remove", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
     0, MY_I_S_UNSIGNED, "", 0},
    {"pages_read_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0, "", 0},
    {"pages_written_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0, "", 0},
    {"buffers_alloc_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0, "", 0},
    {"buffers_remove_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0, "",
     0},
    {"number_pages_get", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"number_pages_hit", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"hit_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0, "", 0},
    {"number_cr_buffers_alloc", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"number_cr_buffers_build", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"number_cr_buffers_reuse", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"number_cr_buffers_get", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
     0, MY_I_S_UNSIGNED, "", 0},
    {"number_cr_buffers_hit", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
     0, MY_I_S_UNSIGNED, "", 0},
    {"cr_hit_rate", MAX_FLOAT_STR_LENGTH, MYSQL_TYPE_FLOAT, 0, 0, "", 0},
    {"time_after_counter_reset", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG,
     0, MY_I_S_UNSIGNED, "", 0},
    {"number_pages_read_after_counter_reset", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"average_read_latency_after_counter_reset_us", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"min_read_latency_after_counter_reset_us", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"max_read_latency_after_counter_reset_us", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"number_pages_write_after_counter_reset", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"average_write_latency_after_counter_reset_us",
     MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "",
     0},
    {"min_write_latency_after_counter_reset_us", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"max_write_latency_after_counter_reset_us", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    END_OF_ST_FIELD};

struct DstoreBufferPoolMgr {
  /* To protect access concurrent */
  std::mutex stats_mtx;
};

static DstoreBufferPoolMgr bufferPoolMgr;

/**
Function to fill INFORMATION_SCHEMA.dstore_buffer_pool_stats

@param[in]      thd
@param[in]      tables
@param[in]      item

@return 0 on success
*/
static int DstoreBufferPoolStatsFillTable(THD *thd, Table_ref *tables,
                                          Item *item) {
  (void)item;

  DSTORE::BufMgrDiagnose bufMgrDiag;
  Field **fields = tables->table->field;
  const char *tableName = tables->table_name;

  if (CdeStrcasecmp(tableName, I_S_DSTORE_BUFFER_POOL_STATS_TABLE_NAME) != 0) {
    return CDE_ERROR;
  }

  /* deny access to non-superusers */
  if (check_global_access(thd, PROCESS_ACL)) {
    return CDE_OK;
  }

  (void)CdeCreateOrGetSession(thd);
  DSTORE::BufferPoolStatsInfo bufferPoolStatsInfo;

  /* Use mutex to prevent concurrent execute here. */
  std::unique_lock<std::mutex> lck(bufferPoolMgr.stats_mtx);
  /* Invoke diagnose api to collect buffpool statistic info */
  DSTORE::RetStatus ret =
      bufMgrDiag.GetBufferPoolStatsInfo(&bufferPoolStatsInfo);

  if (ret != DSTORE::DSTORE_SUCC) {
    return CDE_ERROR;
  }

  CHECK_STORE_RET(fields[LRU_PARTITION_SIZE]->store(
      bufferPoolStatsInfo.lruPartitionSize, true));
  CHECK_STORE_RET(fields[POOL_SIZE]->store(bufferPoolStatsInfo.poolSize, true));
  CHECK_STORE_RET(fields[CANDIDATE_BUFFERS]->store(
      bufferPoolStatsInfo.candidateBuffers, true));
  CHECK_STORE_RET(fields[HOT_DATABASE_PAGES]->store(
      bufferPoolStatsInfo.hotDatabasePages, true));
  CHECK_STORE_RET(fields[LRU_DATABASE_PAGES]->store(
      bufferPoolStatsInfo.lruDatabasePages, true));
  CHECK_STORE_RET(fields[NUMBER_DIRTY_PAGES]->store(
      bufferPoolStatsInfo.modifiedDatabasePages, true));
  CHECK_STORE_RET(fields[NUMBER_LAST_DIRTY_PAGES_REMOVE]->store(
      bufferPoolStatsInfo.lastRemoveCnt, true));
  CHECK_STORE_RET(fields[NUMBER_LAST_DIRTY_PAGES_FLUSH]->store(
      bufferPoolStatsInfo.lastFlushCnt, true));
  CHECK_STORE_RET(fields[NUMBER_PUSH_QUEUE_TOTAL]->store(
      bufferPoolStatsInfo.pushQueueTotalCnt, true));
  CHECK_STORE_RET(fields[NUMBER_REMOVE_QUEUE_TOTAL]->store(
      bufferPoolStatsInfo.removeTotalCnt, true));
  CHECK_STORE_RET(fields[DIRTY_PAGES_LOW_WATERMARK]->store(
      bufferPoolStatsInfo.dirtyPageLowWatermark, true));
  CHECK_STORE_RET(fields[DIRTY_PAGES_HIGH_WATERMARK]->store(
      bufferPoolStatsInfo.dirtyPageHighWatermark, true));
  CHECK_STORE_RET(fields[CURRENT_DISK_SLAVE_WRITER_NUM]->store(
      bufferPoolStatsInfo.currentDiskSlaveWriterNum, true));
  CHECK_STORE_RET(fields[CURRENT_FLUSH_BATCH_NUM]->store(
      bufferPoolStatsInfo.currentFlushBatchNum, true));
  CHECK_STORE_RET(fields[CURRENT_DISK_MASTER_WRITER_SLEEP_MS]->store(
      bufferPoolStatsInfo.currentDiskMasterWriterSleepMs, true));
  CHECK_STORE_RET(
      fields[PAGES_MADE_HOT]->store(bufferPoolStatsInfo.pagesMadeHot, true));
  CHECK_STORE_RET(fields[PAGES_REMOVE_HOT]->store(
      bufferPoolStatsInfo.pagesRemoveHot, true));
  CHECK_STORE_RET(
      fields[PAGES_MADE_HOT_RATE]->store(bufferPoolStatsInfo.pagesMadeHotRate));
  CHECK_STORE_RET(fields[PAGES_REMOVE_HOT_RATE]->store(
      bufferPoolStatsInfo.pagesRemoveHotRate));
  CHECK_STORE_RET(fields[HOT_MAKE_PER_THOUSAND_GETS]->store(
      bufferPoolStatsInfo.hotMakePerThousandGets, true));
  CHECK_STORE_RET(fields[HOT_REMOVE_PER_THOUSAND_GETS]->store(
      bufferPoolStatsInfo.hotRemovePerThousandGets, true));
  CHECK_STORE_RET(
      fields[PAGES_LRU_ADD]->store(bufferPoolStatsInfo.pagesLruAdd, true));
  CHECK_STORE_RET(fields[PAGES_LRU_REMOVE]->store(
      bufferPoolStatsInfo.pagesLruRemove, true));
  CHECK_STORE_RET(
      fields[PAGES_LRU_MOVE]->store(bufferPoolStatsInfo.pagesLruMove, true));
  CHECK_STORE_RET(
      fields[PAGES_LRU_ADD_RATE]->store(bufferPoolStatsInfo.pagesLruAddRate));
  CHECK_STORE_RET(fields[PAGES_LRU_REMOVE_RATE]->store(
      bufferPoolStatsInfo.pagesLruRemoveRate));
  CHECK_STORE_RET(
      fields[PAGES_LRU_MOVE_RATE]->store(bufferPoolStatsInfo.pagesLruMoveRate));
  CHECK_STORE_RET(fields[PAGES_CANDIDATE_ADD]->store(
      bufferPoolStatsInfo.pagesCandidateAdd, true));
  CHECK_STORE_RET(fields[PAGES_CANDIDATE_REMOVE]->store(
      bufferPoolStatsInfo.pagesCandidateRemove, true));
  CHECK_STORE_RET(fields[PAGES_CANDIDATE_MISS]->store(
      bufferPoolStatsInfo.pagesCandidateMiss, true));
  CHECK_STORE_RET(fields[PAGES_CANDIDATE_ADD_RATE]->store(
      bufferPoolStatsInfo.pagesCandidateAddRate));
  CHECK_STORE_RET(fields[PAGES_CANDIDATE_REMOVE_RATE]->store(
      bufferPoolStatsInfo.pagesCandidateRemoveRate));
  CHECK_STORE_RET(fields[PAGES_CANDIDATE_MISS_RATE]->store(
      bufferPoolStatsInfo.pagesCandidateMissRate));
  CHECK_STORE_RET(fields[NUMBER_PAGES_READ]->store(
      bufferPoolStatsInfo.numberPagesRead, true));
  CHECK_STORE_RET(fields[NUMBER_PAGES_WRITTEN]->store(
      bufferPoolStatsInfo.numberPagesWriten, true));
  CHECK_STORE_RET(fields[NUMBER_BUFFERS_ALLOC]->store(
      bufferPoolStatsInfo.numberBuffersAlloc, true));
  CHECK_STORE_RET(fields[NUMBER_BUFFERS_REMOVE]->store(
      bufferPoolStatsInfo.numberBuffersRemove, true));
  CHECK_STORE_RET(
      fields[PAGES_READ_RATE]->store(bufferPoolStatsInfo.pagesReadRate));
  CHECK_STORE_RET(
      fields[PAGES_WRITTEN_RATE]->store(bufferPoolStatsInfo.pagesWritenRate));
  CHECK_STORE_RET(
      fields[BUFFERS_ALLOC_RATE]->store(bufferPoolStatsInfo.buffersAllocRate));
  CHECK_STORE_RET(fields[BUFFERS_REMOVE_RATE]->store(
      bufferPoolStatsInfo.buffersRemoveRate));
  CHECK_STORE_RET(fields[NUMBER_PAGES_GET]->store(
      bufferPoolStatsInfo.numberPagesGet, true));
  CHECK_STORE_RET(fields[NUMBER_PAGES_HIT]->store(
      bufferPoolStatsInfo.numberPagesHit, true));
  CHECK_STORE_RET(fields[HIT_RATE]->store(bufferPoolStatsInfo.hitRate));
  CHECK_STORE_RET(fields[NUMBER_CR_BUFFERS_ALLOC]->store(
      bufferPoolStatsInfo.numberCrBuffersAlloc, true));
  CHECK_STORE_RET(fields[NUMBER_CR_BUFFERS_BUILD]->store(
      bufferPoolStatsInfo.numberCrBuffersBuild, true));
  CHECK_STORE_RET(fields[NUMBER_CR_BUFFERS_REUSE]->store(
      bufferPoolStatsInfo.numberCrBuffersReuse, true));
  CHECK_STORE_RET(fields[NUMBER_CR_BUFFERS_GET]->store(
      bufferPoolStatsInfo.numberCrBuffersGet, true));
  CHECK_STORE_RET(fields[NUMBER_CR_BUFFERS_HIT]->store(
      bufferPoolStatsInfo.numberCrBuffersHit, true));
  CHECK_STORE_RET(fields[CR_HIT_RATE]->store(bufferPoolStatsInfo.crHitRate));
  CHECK_STORE_RET(fields[TIME_AFTER_COUNTER_RESET]->store(
      bufferPoolStatsInfo.timeAfterCounterLastReset, true));
  CHECK_STORE_RET(fields[NUMBER_PAGES_READ_AFTER_COUNTER_RESET]->store(
      bufferPoolStatsInfo.numPagesReadAfterReset, true));
  CHECK_STORE_RET(fields[AVERAGE_READ_LATENCY_AFTER_COUNTER_RESET]->store(
      bufferPoolStatsInfo.avgReadLatency, true));
  CHECK_STORE_RET(fields[MAX_READ_LATENCY_AFTER_COUNTER_RESET]->store(
      bufferPoolStatsInfo.maxReadLatency, true));
  CHECK_STORE_RET(fields[MIN_READ_LATENCY_AFTER_COUNTER_RESET]->store(
      bufferPoolStatsInfo.minReadLatency, true));
  CHECK_STORE_RET(fields[NUMBER_PAGES_WRITE_AFTER_COUNTER_RESET]->store(
      bufferPoolStatsInfo.numPagesWriteAfterReset, true));
  CHECK_STORE_RET(fields[AVERAGE_WRITE_LATENCY_AFTER_COUNTER_RESET]->store(
      bufferPoolStatsInfo.avgWriteLatency, true));
  CHECK_STORE_RET(fields[MAX_WRITE_LATENCY_AFTER_COUNTER_RESET]->store(
      bufferPoolStatsInfo.maxWriteLatency, true));
  CHECK_STORE_RET(fields[MIN_WRITE_LATENCY_AFTER_COUNTER_RESET]->store(
      bufferPoolStatsInfo.minWriteLatency, true));

  CHECK_STORE_RET(schema_table_store_record(thd, tables->table));

  return CDE_OK;
}

/**
Bind the dynamic table INFORMATION_SCHEMA.dstore_buffer_pool_stats

@param[in/out]      p   schema table to init

@return 0 on success
*/
static int DstoreBufferPoolStatsInit(void *p) {
  if (!p) {
    return CDE_ERROR;
  }
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;

  schema->fields_info = g_dstoreBufferPoolStatsFieldsInfo;
  schema->fill_table = DstoreBufferPoolStatsFillTable;
  return CDE_OK;
}

struct st_mysql_plugin g_infomationSchemaDstoreBufferPoolStats = {
    /* the plugin type (a MYSQL_XXX_PLUGIN value) */
    /* int */
    MYSQL_INFORMATION_SCHEMA_PLUGIN,

    /* pointer to type-specific plugin descriptor */
    /* void* */
    &g_infomationSchemaInfo,

    /* plugin name */
    /* const char* */
    I_S_DSTORE_BUFFER_POOL_STATS_TABLE_NAME,

    /* plugin author (for SHOW PLUGINS) */
    /* const char* */
    PLUGIN_AUTHOR,

    /* general descriptive text (for SHOW PLUGINS) */
    /* const char* */
    "DStore bufferpool statistic infomation",

    /* the plugin license (PLUGIN_LICENSE_XXX) */
    /* int */
    PLUGIN_LICENSE_GPL,

    /* the function to invoke when plugin is loaded */
    /* int (*)(void*); */
    DstoreBufferPoolStatsInit,

    /* the function to invoke when plugin is un installed */
    /* int (*)(void*); */
    nullptr,

    /* the function to invoke when plugin is unloaded */
    /* int (*)(void*); */
    nullptr,

    /* plugin version (for SHOW PLUGINS) */
    /* unsigned int */
    1,

    /* SHOW_VAR* */
    nullptr,

    /* SYS_VAR** */
    nullptr,

    /* reserved for dependency checking */
    /* void* */
    nullptr,

    /* Plugin flags */
    /* unsigned long */
    0UL,
};

/** The Dstore Locks i_s table field id enum, note that the order of this enum
must be the same as the array g_dstoreLocksFieldsInfo define. */
enum DstoreLocksField {
  LOCK_OS_THREAD_ID,
  LOCKTAG_TYPE,
  LOCK_MODE,
  LOCK_FIELD1,
  LOCK_FIELD2,
  LOCK_FIELD3,
  LOCK_FIELD4,
  LOCK_FIELD5,
  LOCK_STATE,
  LOCK_GRANTED_COUNT,
  LOCK_DESC
};

static ST_FIELD_INFO g_dstoreLocksFieldsInfo[] = {
    {"OS_THREAD_ID", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"LOCKTAG_TYPE", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"LOCK_MODE", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"FIELD1", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, MY_I_S_UNSIGNED,
     "", 0},
    {"FIELD2", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, MY_I_S_UNSIGNED,
     "", 0},
    {"FIELD3", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, MY_I_S_UNSIGNED,
     "", 0},
    {"FIELD4", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, MY_I_S_UNSIGNED,
     "", 0},
    {"FIELD5", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, MY_I_S_UNSIGNED,
     "", 0},
    {"STATE", LOCKS_I_S_STATE_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"GRANTED_COUNT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"DESC", LOCKS_I_S_DESC_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    END_OF_ST_FIELD};

struct DstoreLocksMgr {
  /* To protect access concurrent */
  std::mutex m_mtx;
};

static DstoreLocksMgr dstoreLockMgr;

/**
Function to fill INFORMATION_SCHEMA.dstore_locks

@param[in]      thd
@param[in]      tables
@param[in]      item

@return 0 on success
*/
static int DstoreLocksFillTable(THD *thd, Table_ref *tables, Item *item) {
  (void)item;

  Field **fields = tables->table->field;
  const char *tableName = tables->table_name;

  if (CdeStrcasecmp(tableName, I_S_DSTORE_LOCKS_TABLE_NAME) != 0) {
    return CDE_ERROR;
  }

  /* deny access to non-superusers */
  if (check_global_access(thd, PROCESS_ACL)) {
    return CDE_OK;
  }

  FilterSet filterInfo;
  if (item) {
    std::vector<int> filterIndex{DstoreLocksField::LOCK_OS_THREAD_ID};
    FilterPushdownCond(item, thd, filterIndex, filterInfo);
  }

  /* Default value 0 means get all thread's locks. */
  uint64_t filterThreadId{0};
  if (filterInfo.count(DstoreLocksField::LOCK_OS_THREAD_ID)) {
    filterThreadId = filterInfo[DstoreLocksField::LOCK_OS_THREAD_ID].m_val;
    /* For dstore api GetLocksByThread, pid para zero means get all thread's
    info. But if select with thread_id condition equal zero, no entry will
    match this condition, so here we just return directly. */
    if (filterThreadId == 0) {
      return CDE_OK;
    }
    cde_perf_counters::getInstance()
        ->_dstore_locks_thread_id_pushdown_hit.increment();
  }

  (void)CdeCreateOrGetSession(thd);

  /* Use mutex to prevent concurrent execute here. */
  std::unique_lock<std::mutex> lck(dstoreLockMgr.m_mtx);
  int32_t lockNum = 0;
  /* Invoke diagnose api to collect transaction info */
  DSTORE::LockStatus **lockStatus =
      DSTORE::LockMgrDiagnose::GetLocksByThread(lockNum, filterThreadId);
  if (lockNum == 0) {
    return CDE_OK;
  }

  auto CleanupGuard = create_scope_guard([lockStatus, lockNum] {
    DSTORE::LockMgrDiagnose::FreeLockStatusArr(lockStatus, lockNum);
  });

  for (uint32_t i = 0; i < static_cast<uint32_t>(lockNum); ++i) {
    DSTORE::LockStatus *lock = lockStatus[i];
    CHECK_STORE_RET(fields[LOCK_OS_THREAD_ID]->store(lock->tid, true));
    CHECK_STORE_RET(fields[LOCKTAG_TYPE]->store(lock->lockTagType, true));
    CHECK_STORE_RET(fields[LOCK_MODE]->store(lock->lockMode, true));
    CHECK_STORE_RET(fields[LOCK_FIELD1]->store(lock->field1, true));
    CHECK_STORE_RET(fields[LOCK_FIELD2]->store(lock->field2, true));
    CHECK_STORE_RET(fields[LOCK_FIELD3]->store(lock->field3, true));
    CHECK_STORE_RET(fields[LOCK_FIELD4]->store(lock->field4, true));
    CHECK_STORE_RET(fields[LOCK_FIELD5]->store(lock->field5, true));
    CHECK_STORE_RET(FieldStoreString(fields[LOCK_STATE],
                                     lock->isWaiting ? "WAITING" : "GRANTED"));
    CHECK_STORE_RET(fields[LOCK_GRANTED_COUNT]->store(lock->grantedCnt, true));
    CHECK_STORE_RET(
        FieldStoreString(fields[LOCK_DESC], lock->lockTagDescription));
    CHECK_STORE_RET(schema_table_store_record(thd, tables->table));
  }

  return CDE_OK;
}

/**
Bind the dynamic table INFORMATION_SCHEMA.dstore_locks

@param[in/out]      p   schema table to init

@return 0 on success
*/
static int DstoreLocksInit(void *p) {
  if (!p) {
    return CDE_ERROR;
  }
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;

  schema->fields_info = g_dstoreLocksFieldsInfo;
  schema->fill_table = DstoreLocksFillTable;
  return CDE_OK;
}

struct st_mysql_plugin g_infomationSchemaDstoreLocks = {
    /* the plugin type (a MYSQL_XXX_PLUGIN value) */
    /* int */
    MYSQL_INFORMATION_SCHEMA_PLUGIN,

    /* pointer to type-specific plugin descriptor */
    /* void* */
    &g_infomationSchemaInfo,

    /* plugin name */
    /* const char* */
    I_S_DSTORE_LOCKS_TABLE_NAME,

    /* plugin author (for SHOW PLUGINS) */
    /* const char* */
    PLUGIN_AUTHOR,

    /* general descriptive text (for SHOW PLUGINS) */
    /* const char* */
    "DStore locks",

    /* the plugin license (PLUGIN_LICENSE_XXX) */
    /* int */
    PLUGIN_LICENSE_GPL,

    /* the function to invoke when plugin is loaded */
    /* int (*)(void*); */
    DstoreLocksInit,

    /* the function to invoke when plugin is un installed */
    /* int (*)(void*); */
    nullptr,

    /* the function to invoke when plugin is unloaded */
    /* int (*)(void*); */
    nullptr,

    /* plugin version (for SHOW PLUGINS) */
    /* unsigned int */
    1,

    /* SHOW_VAR* */
    nullptr,

    /* SYS_VAR** */
    nullptr,

    /* reserved for dependency checking */
    /* void* */
    nullptr,

    /* Plugin flags */
    /* unsigned long */
    0UL,
};

/** The DSTORE_MEM_STATS i_s table field id enum, note that the order of
this enum must be the same as the array g_dstoreMemStatsFieldsInfo
define. */
enum DstoreMemStatsField {
  OS_THREAD_ID,
  ROOT_ALLOCATED,
  ROOT_USED,
  LONGLIVE_ALLOCATED,
  LONGLIVE_USED,
  SMGR_ALLOCATED,
  SMGR_USED,
  ERROR_ALLOCATED,
  ERROR_USED,
  BUFFER_ALLOCATED,
  BUFFER_USED,
  TRANSACTION_ALLOCATED,
  TRANSACTION_USED,
  LOCK_ALLOCATED,
  LOCK_USED,
  QUERY_ALLOCATED,
  QUERY_USED,
  STACK_ALLOCATED,
  STACK_USED,
  OTHERS_ALLOCATED,
  OTHERS_USED
};

static ST_FIELD_INFO g_dstoreMemStatsFieldsInfo[] = {
    {"os_thread_id", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"root_allocated", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"root_used", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"longlive_allocated", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"longlive_used", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"smgr_allocated", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"smgr_used", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"error_allocated", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"error_used", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"buffer_allocated", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"buffer_used", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"transaction_allocated", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
     0, MY_I_S_UNSIGNED, "", 0},
    {"transaction_used", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"lock_allocated", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"lock_used", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"query_allocated", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"query_used", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"stack_allocated", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"stack_used", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"others_allocated", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"others_used", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    END_OF_ST_FIELD};

struct DstoreMemMgr {
  /* To protect access concurrent */
  std::mutex stats_mtx;
};

static DstoreMemMgr memMgr;

/**
Function to fill INFORMATION_SCHEMA.dstore_mem_stats

@param[in]      thd
@param[in]      tables
@param[in]      item

@return 0 on success
*/
static int DstoreMemStatsFillTable(THD *thd, Table_ref *tables, Item *item) {
  (void)item;

  [[maybe_unused]] Field **fields = tables->table->field;
  const char *tableName = tables->table_name;

  if (CdeStrcasecmp(tableName, I_S_DSTORE_MEM_STATS_TABLE_NAME) != 0) {
    return CDE_ERROR;
  }

  /* deny access to non-superusers */
  if (check_global_access(thd, PROCESS_ACL)) {
    return CDE_OK;
  }

  (void)CdeCreateOrGetSession(thd);

  /* Use mutex to prevent concurrent execute here. */
  std::unique_lock<std::mutex> lck(memMgr.stats_mtx);

  /* Invoke diagnose api to collect memory info */
  DSTORE::ThreadMemoryDetail *memDetails = nullptr;
  uint32_t memDetailsLen = 0;
  DSTORE::MemoryDiagnose memMgrDiag;
  DSTORE::RetStatus ret =
      memMgrDiag.GetAllThreadsMemoryDetails(&memDetails, &memDetailsLen);

  /* Get failed also return OK to avoid server crash. */
  if (ret != DSTORE::DSTORE_SUCC) {
    return CDE_OK;
  }

  auto CleanupGuard = create_scope_guard([&memDetails, &memDetailsLen] {
    DSTORE::MemoryDiagnose::FreeMemoryStatusArr(&memDetails, memDetailsLen);
  });

  std::unordered_map<uint64_t, ThreadMemoryDetailAggResult> aggMap;
  AggregateThreadMemoryDetails(&memDetails, &memDetailsLen, aggMap);

  for (const auto &pair : aggMap) {
    uint64_t tid = pair.first;
    const ThreadMemoryDetailAggResult &result = pair.second;
    CHECK_STORE_RET(fields[OS_THREAD_ID]->store(tid, true));
    CHECK_STORE_RET(fields[ROOT_ALLOCATED]->store(
        result.contextStats[StorageThreadTop].alloced, true));
    CHECK_STORE_RET(fields[ROOT_USED]->store(
        result.contextStats[StorageThreadTop].used, true));
    CHECK_STORE_RET(fields[LONGLIVE_ALLOCATED]->store(
        result.contextStats[LongLive].alloced, true));
    CHECK_STORE_RET(
        fields[LONGLIVE_USED]->store(result.contextStats[LongLive].used, true));
    CHECK_STORE_RET(fields[SMGR_ALLOCATED]->store(
        result.contextStats[StorageManager].alloced, true));
    CHECK_STORE_RET(fields[SMGR_USED]->store(
        result.contextStats[StorageManager].used, true));
    CHECK_STORE_RET(fields[ERROR_ALLOCATED]->store(
        result.contextStats[Error].alloced, true));
    CHECK_STORE_RET(
        fields[ERROR_USED]->store(result.contextStats[Error].used, true));
    CHECK_STORE_RET(fields[BUFFER_ALLOCATED]->store(
        result.contextStats[BufferMgr].alloced, true));
    CHECK_STORE_RET(
        fields[BUFFER_USED]->store(result.contextStats[BufferMgr].used, true));
    CHECK_STORE_RET(fields[TRANSACTION_ALLOCATED]->store(
        result.contextStats[Transaction].alloced, true));
    CHECK_STORE_RET(fields[TRANSACTION_USED]->store(
        result.contextStats[Transaction].used, true));
    CHECK_STORE_RET(fields[LOCK_ALLOCATED]->store(
        result.contextStats[LockMgr].alloced, true));
    CHECK_STORE_RET(
        fields[LOCK_USED]->store(result.contextStats[LockMgr].used, true));
    CHECK_STORE_RET(fields[QUERY_ALLOCATED]->store(
        result.contextStats[PerQuery].alloced, true));
    CHECK_STORE_RET(
        fields[QUERY_USED]->store(result.contextStats[PerQuery].used, true));
    CHECK_STORE_RET(fields[STACK_ALLOCATED]->store(
        result.contextStats[Stack].alloced, true));
    CHECK_STORE_RET(
        fields[STACK_USED]->store(result.contextStats[Stack].used, true));
    CHECK_STORE_RET(fields[OTHERS_ALLOCATED]->store(
        result.contextStats[Others].alloced, true));
    CHECK_STORE_RET(
        fields[OTHERS_USED]->store(result.contextStats[Others].used, true));
    CHECK_STORE_RET(schema_table_store_record(thd, tables->table));
  }

  return CDE_OK;
}

/**
Bind the dynamic table INFORMATION_SCHEMA.dstore_mem_stats

@param[in/out]      p   schema table to init

@return 0 on success
*/
static int DstoreMemStatsInit(void *p) {
  if (!p) {
    return CDE_ERROR;
  }
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;

  schema->fields_info = g_dstoreMemStatsFieldsInfo;
  schema->fill_table = DstoreMemStatsFillTable;
  return CDE_OK;
}

struct st_mysql_plugin g_infomationSchemaDstoreMemStats = {
    /* the plugin type (a MYSQL_XXX_PLUGIN value) */
    /* int */
    MYSQL_INFORMATION_SCHEMA_PLUGIN,

    /* pointer to type-specific plugin descriptor */
    /* void* */
    &g_infomationSchemaInfo,

    /* plugin name */
    /* const char* */
    I_S_DSTORE_MEM_STATS_TABLE_NAME,

    /* plugin author (for SHOW PLUGINS) */
    /* const char* */
    PLUGIN_AUTHOR,

    /* general descriptive text (for SHOW PLUGINS) */
    /* const char* */
    "DStore memory statistic infomation",

    /* the plugin license (PLUGIN_LICENSE_XXX) */
    /* int */
    PLUGIN_LICENSE_GPL,

    /* the function to invoke when plugin is loaded */
    /* int (*)(void*); */
    DstoreMemStatsInit,

    /* the function to invoke when plugin is un installed */
    /* int (*)(void*); */
    nullptr,

    /* the function to invoke when plugin is unloaded */
    /* int (*)(void*); */
    nullptr,

    /* plugin version (for SHOW PLUGINS) */
    /* unsigned int */
    1,

    /* SHOW_VAR* */
    nullptr,

    /* SYS_VAR** */
    nullptr,

    /* reserved for dependency checking */
    /* void* */
    nullptr,

    /* Plugin flags */
    /* unsigned long */
    0UL,
};

/** The DSTORE_SEGMENT i_s table field id enum, note that the order of
this enum must be the same as the array g_dstoreSegmentStatsFieldsInfo
define. */
enum DstoreSegStatsField {
  SEGMENT_ID,
  TYPE,
  DB_NAME,
  TABLE_NAME,
  INDEX_NAME,
  PARTITION_NAME,
  TABLESPACE,
  FILL_FACTOR,
  PAGE_COUNT,
  FREE_LEVEL_64B_PAGE_COUNT,
  FREE_LEVEL_128B_PAGE_COUNT,
  FREE_LEVEL_512B_PAGE_COUNT,
  FREE_LEVEL_1K_PAGE_COUNT,
  FREE_LEVEL_2K_PAGE_COUNT,
  FREE_LEVEL_4K_PAGE_COUNT,
  FREE_LEVEL_8K_PAGE_COUNT,
};

/** The Dstore INDEXES i_s table field id enum, note that the order of this enum
must be the same as the array g_dstoreIndexesFieldsInfo define. */
enum DstoreIndexesField {
  INDEX_DB_NAME,
  INDEX_TABLE_NAME,
  INDEX_INDEX_NAME,
  INDEX_PARTITION_NAME,
  INDEX_SEGMENT_ID,
  INDEX_META_PAGE,
  ROOT_PAGE,
  ROOT_LEVEL,
  FAST_ROOT_PAGE,
  FAST_ROOT_LEVEL,
  RELATION_KIND,
  KEY_NUM,
  ATTRS_NUM,
  TABLE_OID_ATT,
  SPLIT_TIMES_PER_LEVEL,
  RECYCLE_TIMES_PER_LEVEL
};

static ST_FIELD_INFO g_dstoreSegmentStatsFieldsInfo[] = {
    {"SEGMENT_ID", PAGE_ID_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"TYPE", SEGMENT_TYPE_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"DB_NAME", CDE_MAX_DATABASE_NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"TABLE_NAME", CDE_MAX_TABLE_NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"INDEX_NAME", CDE_MAX_INDEX_NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"PARTITION_NAME", CDE_MAX_TABLE_NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"TABLESPACE", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"FILL_FACTOR", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"PAGE_COUNT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"FREE_LEVEL_64B_PAGE_COUNT", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"FREE_LEVEL_128B_PAGE_COUNT", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"FREE_LEVEL_512B_PAGE_COUNT", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"FREE_LEVEL_1K_PAGE_COUNT", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"FREE_LEVEL_2K_PAGE_COUNT", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"FREE_LEVEL_4K_PAGE_COUNT", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"FREE_LEVEL_8K_PAGE_COUNT", MY_INT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    END_OF_ST_FIELD};

static ST_FIELD_INFO g_dstoreIndexesFieldsInfo[] = {
    {"DB_NAME", CDE_MAX_DATABASE_NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"TABLE_NAME", CDE_MAX_TABLE_NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"INDEX_NAME", CDE_MAX_INDEX_NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"PARTITION_NAME", CDE_MAX_TABLE_NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"SEGMENT_ID", PAGE_ID_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"META_PAGE", PAGE_ID_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"ROOT_PAGE", PAGE_ID_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"ROOT_LEVEL", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"FAST_ROOT_PAGE", PAGE_ID_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"FAST_ROOT_LEVEL", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"RELATION_KIND", PAGE_ID_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"KEY_NUM", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"ATTRS_NUM", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"TABLE_OID_ATT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"SPLIT_TIMES_PER_LEVEL", INDEXES_I_S_LEVEL_INFO_MAX_LEN, MYSQL_TYPE_STRING,
     0, 0, "", 0},
    {"RECYCLE_TIMES_PER_LEVEL", INDEXES_I_S_LEVEL_INFO_MAX_LEN,
     MYSQL_TYPE_STRING, 0, 0, "", 0},
    END_OF_ST_FIELD};

/**
 * @brief Parse a string in the format "fieldId,blockId" into unsigned integers
 *
 * Parses strings like "5121,1" where:
 * - fieldId is a 16-bit unsigned integer (uint16_t)
 * - blockId is a 32-bit unsigned integer (uint32_t)
 *
 * @param input The input string to parse, must be in format "number,number"
 * @param fieldId[out] Parsed field ID (16-bit unsigned integer)
 * @param blockId[out] Parsed block ID (32-bit unsigned integer)
 * @return true if parsing succeeded, false if input format is invalid or values
 * are out of range
 */

static bool parseSegmentId(const std::string &input, uint16_t &fileId,
                           uint32_t &blockId) {
  std::regex pattern(R"((\d+)\,(\d+))");
  std::smatch matches;

  if (std::regex_match(input, matches, pattern) && matches.size() == 3) {
    try {
      /* parse fileId */
      unsigned long tmpFileId = std::stoul(matches[1].str());
      if (tmpFileId > std::numeric_limits<uint16_t>::max()) {
        return false;
      }
      fileId = static_cast<uint16_t>(tmpFileId);

      /* parse blockId */
      unsigned long tmpBlockId = std::stoul(matches[2].str());
      if (tmpBlockId > std::numeric_limits<uint32_t>::max()) {
        return false;
      }
      blockId = static_cast<uint32_t>(tmpBlockId);

      return true;
    } catch (const std::exception &e) {
      return false;
    }
  }
  return false;
}

/**
When `fetch_global_components` retrieves all ddTables, it reads from
`mysql.tables` and `mysql.partitions` using separate transactions.
Consequently, a scenario may arise where `ddTable.partition_type != PT_NONE`
(indicating a partitioned table), but `ddTable.partitions().size() == 0`.
In this case, the ddTable object lacks the corresponding partition metadata.

@param[in]      ddTable
@return true if success
*/
inline static bool IsPartitionInfoConsistent(const dd::Table *ddTable) {
  // LCOV_EXCL_START
  if (ddTable == nullptr) {
    return false;
  }
  // For partitioned tables, the partitions list cannot be empty.
  if (ddTable->partition_type() != dd::Table::PT_NONE) {
    return ddTable->partitions().size() != 0;
  }
  // LCOV_EXCL_STOP
  return true;
}

/*  SegmentTableInfo - Stores segment metadata information extracted from
 * dd::Table */
struct SegmentTableInfo {
  std::string segmentIdStr;
  std::string dbName;
  std::string tableName;
  std::string indexName;
  std::string partitionName;
  std::string segmentType;
  int fillFactor{0};
};

/**
 * SegmentFilterConditions - Stores segment statistics filter conditions parsed
 * from queries Contains filter conditions for segment_id, db_name, table_name,
 * index_name and their corresponding boolean flags
 */
struct SegmentFilterConditions {
  bool hasSegmentId = false;
  DSTORE::PageId segmentId = {DSTORE::INVALID_VFS_FILE_ID,
                              DSTORE::DSTORE_INVALID_BLOCK_NUMBER};

  bool hasDbName = false;
  std::string dbName;

  bool hasTableName = false;
  std::string tableName;

  bool hasIndexName = false;
  std::string indexName;

  SegmentFilterConditions() = default;

  /* Get filterCondition from FilterSet, for INFORMATION_SCHEMA.dstore_segment
   * table */
  static bool SegParseFromFilterSet(const FilterSet &out,
                                    SegmentFilterConditions &conditions) {
    if (out.count(DstoreSegStatsField::SEGMENT_ID)) {
      conditions.hasSegmentId = true;
      Datum datumSegmentIdStr = out.at(DstoreSegStatsField::SEGMENT_ID).m_val;
      const std::string segmentIdStr((const char *)datumSegmentIdStr);
      uint16_t fileId;
      uint32_t blockId;
      if (!parseSegmentId(segmentIdStr, fileId, blockId)) {
        CDE_LOG_WARN("SegParseFromFilterSet Failed to parse segment ID: %s",
                     segmentIdStr.c_str());
        return false;
      }
      conditions.segmentId.m_fileId = fileId;
      conditions.segmentId.m_blockId = blockId;
    }

    if (out.count(DstoreSegStatsField::DB_NAME)) {
      conditions.hasDbName = true;
      Datum dbName = out.at(DstoreSegStatsField::DB_NAME).m_val;
      conditions.dbName = (const char *)dbName;
    }

    if (out.count(DstoreSegStatsField::TABLE_NAME)) {
      conditions.hasTableName = true;
      Datum tableName = out.at(DstoreSegStatsField::TABLE_NAME).m_val;
      conditions.tableName = (const char *)(tableName);
    }

    if (out.count(DstoreSegStatsField::INDEX_NAME)) {
      conditions.hasIndexName = true;
      Datum indexName = out.at(DstoreSegStatsField::INDEX_NAME).m_val;
      conditions.indexName = (const char *)indexName;
    }

    return true;
  }

  /* Get filterCondition from FilterSet, for INFORMATION_SCHEMA.dstore_indexes
   * table */
  static bool IndexParseFromFilterSet(const FilterSet &out,
                                      SegmentFilterConditions &conditions) {
    if (out.count(DstoreIndexesField::INDEX_SEGMENT_ID)) {
      conditions.hasSegmentId = true;
      Datum datumSegmentIdStr =
          out.at(DstoreIndexesField::INDEX_SEGMENT_ID).m_val;
      const std::string segmentIdStr((const char *)datumSegmentIdStr);
      uint16_t fileId;
      uint32_t blockId;
      if (!parseSegmentId(segmentIdStr, fileId, blockId)) {
        CDE_LOG_WARN("IndexParseFromFilterSet Failed to parse segment ID: %s",
                     segmentIdStr.c_str());
        return false;
      }
      conditions.segmentId.m_fileId = fileId;
      conditions.segmentId.m_blockId = blockId;
    }

    if (out.count(DstoreIndexesField::INDEX_DB_NAME)) {
      conditions.hasDbName = true;
      Datum dbName = out.at(DstoreIndexesField::INDEX_DB_NAME).m_val;
      conditions.dbName = (const char *)dbName;
    }

    if (out.count(DstoreIndexesField::INDEX_TABLE_NAME)) {
      conditions.hasTableName = true;
      Datum tableName = out.at(DstoreIndexesField::INDEX_TABLE_NAME).m_val;
      conditions.tableName = (const char *)(tableName);
    }

    if (out.count(DstoreIndexesField::INDEX_INDEX_NAME)) {
      conditions.hasIndexName = true;
      Datum indexName = out.at(DstoreIndexesField::INDEX_INDEX_NAME).m_val;
      conditions.indexName = (const char *)indexName;
    }

    return true;
  }
};

/**
 * This function populates the INFORMATION_SCHEMA.dstore_indexes table
 * based on the input information provided.
 */
static int DstoreIndexStatsFillTableInternal(
    const std::unique_ptr<DSTORE::IndexStatus> &indexInfo,
    const SegmentTableInfo &segTbInfo, THD *thd, TABLE *table) {
  Field **fields = table->field;
  CHECK_STORE_RET(
      FieldStoreString(fields[INDEX_DB_NAME], segTbInfo.dbName.c_str()));
  CHECK_STORE_RET(
      FieldStoreString(fields[INDEX_TABLE_NAME], segTbInfo.tableName.c_str()));
  CHECK_STORE_RET(
      FieldStoreString(fields[INDEX_INDEX_NAME], segTbInfo.indexName.c_str()));
  CHECK_STORE_RET(FieldStoreString(fields[INDEX_PARTITION_NAME],
                                   segTbInfo.partitionName.c_str()));
  CHECK_STORE_RET(FieldStoreString(fields[INDEX_SEGMENT_ID],
                                   segTbInfo.segmentIdStr.c_str()));

  std::string metaPageIdStr = std::to_string(indexInfo->metaPageId.m_fileId) +
                              "," +
                              std::to_string(indexInfo->metaPageId.m_blockId);
  CHECK_STORE_RET(
      FieldStoreString(fields[INDEX_META_PAGE], metaPageIdStr.c_str()));

  std::string rootPageIdStr = std::to_string(indexInfo->rootPageId.m_fileId) +
                              "," +
                              std::to_string(indexInfo->rootPageId.m_blockId);
  CHECK_STORE_RET(FieldStoreString(fields[ROOT_PAGE], rootPageIdStr.c_str()));
  CHECK_STORE_RET(fields[ROOT_LEVEL]->store(indexInfo->rootLevel, true));

  std::string fastRootPageIdStr =
      std::to_string(indexInfo->fastRootPageId.m_fileId) + "," +
      std::to_string(indexInfo->fastRootPageId.m_blockId);
  CHECK_STORE_RET(
      FieldStoreString(fields[FAST_ROOT_PAGE], fastRootPageIdStr.c_str()));
  CHECK_STORE_RET(
      fields[FAST_ROOT_LEVEL]->store(indexInfo->fastRootLevel, true));

  CHECK_STORE_RET(FieldStoreString(fields[RELATION_KIND], &indexInfo->relKind));

  CHECK_STORE_RET(fields[KEY_NUM]->store(indexInfo->keyAttsNum, true));
  CHECK_STORE_RET(fields[ATTRS_NUM]->store(indexInfo->attrsNum, true));
  CHECK_STORE_RET(fields[TABLE_OID_ATT]->store(indexInfo->tableOidAtt, true));
  CHECK_STORE_RET(FieldStoreString(fields[SPLIT_TIMES_PER_LEVEL],
                                   indexInfo->splitTimesPerTimes));
  CHECK_STORE_RET(FieldStoreString(fields[RECYCLE_TIMES_PER_LEVEL],
                                   indexInfo->recycleTimesPerTimes));
  CHECK_STORE_RET(schema_table_store_record(thd, table));
  return CDE_OK;
}

/**
 * This function populates the INFORMATION_SCHEMA.dstore_segment table
 * based on the input information provided.
 */
static int DstoreSegmentStatsFillTableInternal(
    const DSTORE::PageUseCondition &puc, const SegmentTableInfo &segTbInfo,
    THD *thd, TABLE *table) {
  Field **fields = table->field;
  CHECK_STORE_RET(
      FieldStoreString(fields[SEGMENT_ID], segTbInfo.segmentIdStr.c_str()));
  CHECK_STORE_RET(
      FieldStoreString(fields[TYPE], segTbInfo.segmentType.c_str()));
  CHECK_STORE_RET(FieldStoreString(fields[DB_NAME], segTbInfo.dbName.c_str()));
  CHECK_STORE_RET(
      FieldStoreString(fields[TABLE_NAME], segTbInfo.tableName.c_str()));
  CHECK_STORE_RET(
      FieldStoreString(fields[INDEX_NAME], segTbInfo.indexName.c_str()));
  CHECK_STORE_RET(FieldStoreString(fields[PARTITION_NAME],
                                   segTbInfo.partitionName.c_str()));
  CHECK_STORE_RET(fields[TABLESPACE]->store(puc.tablespaceId, true));
  CHECK_STORE_RET(fields[FILL_FACTOR]->store(segTbInfo.fillFactor, true));
  CHECK_STORE_RET(fields[PAGE_COUNT]->store(puc.totalBlocks, true));
  CHECK_STORE_RET(
      fields[FREE_LEVEL_64B_PAGE_COUNT]->store(puc.freeSpace64B, true));
  CHECK_STORE_RET(
      fields[FREE_LEVEL_128B_PAGE_COUNT]->store(puc.freeSpace128B, true));
  CHECK_STORE_RET(
      fields[FREE_LEVEL_512B_PAGE_COUNT]->store(puc.freeSpace512B, true));
  CHECK_STORE_RET(
      fields[FREE_LEVEL_1K_PAGE_COUNT]->store(puc.freeSpace1K, true));
  CHECK_STORE_RET(
      fields[FREE_LEVEL_2K_PAGE_COUNT]->store(puc.freeSpace2K, true));
  CHECK_STORE_RET(
      fields[FREE_LEVEL_4K_PAGE_COUNT]->store(puc.freeSpace4K, true));
  CHECK_STORE_RET(
      fields[FREE_LEVEL_8K_PAGE_COUNT]->store(puc.freeSpace8K, true));
  CHECK_STORE_RET(schema_table_store_record(thd, table));
  return CDE_OK;
}
struct DstoreSegmentStatsMgr {
  /* To protect access concurrent */
  std::mutex m_mtx;
};

static DstoreSegmentStatsMgr segStatsMgr;

/**
 * Segment processing context structure
 * Used to pass segment processing related parameters between functions
 */
struct SegmentProcessContext {
  THD *thd{nullptr};
  dd::cache::Dictionary_client *dc{nullptr};
  const dd::Table *ddTable{nullptr};
  DSTORE::PageId targetSegmentId{DSTORE::INVALID_PAGE_ID};
  TABLE *outTable{nullptr};
  dd::String_type dbName;
  dd::String_type tableName;

  SegmentProcessContext(THD *thdPtr, dd::cache::Dictionary_client *dcPtr,
                        const dd::Table *tablePtr,
                        const DSTORE::PageId segmentId, TABLE *outputTable,
                        dd::String_type inputDbName,
                        dd::String_type inputTableName)
      : thd(thdPtr),
        dc(dcPtr),
        ddTable(tablePtr),
        targetSegmentId(segmentId),
        outTable(outputTable),
        dbName(inputDbName),
        tableName(inputTableName) {}
};

/**
Get segmentId by Properties, if isLobSegment is true, return lobSegment

@param[in]      pro
@param[in]      isLobSegment

@return DSTORE::PageId
*/
static DSTORE::PageId GetSegmentIdByProperties(const dd::Properties &pro,
                                               bool isLobSegment) {
  uint16_t fileId = DSTORE::INVALID_VFS_FILE_ID;
  uint32_t blockId = DSTORE::DSTORE_INVALID_BLOCK_NUMBER;
  if (isLobSegment) {
    if (pro.exists(object_rellobfileid) && pro.exists(object_rellobblknum)) {
      pro.get(object_rellobfileid, &fileId);
      pro.get(object_rellobblknum, &blockId);
    }
  } else {
    if (pro.exists(object_relfileid) && pro.exists(object_relblknum)) {
      pro.get(object_relfileid, &fileId);
      pro.get(object_relblknum, &blockId);
    }
  }

  DSTORE::PageId id = {fileId, blockId};
  return id;
}

using GetNameFuncPtr = bool (dd::cache::Dictionary_client::*)(
    const dd::String_type &, dd::Object_id, dd::String_type *,
    dd::String_type *);

template <typename EntityType>
static int GetTableNameInternal(dd::cache::Dictionary_client *dc,
                                const EntityType *entity,
                                dd::String_type &dbName,
                                dd::String_type &tableName,
                                GetNameFuncPtr lookupFunc) {
  /* Get tableId */
  dd::Object_id tableId = CDE::CdeDdTableGetTableId(entity);

  // LCOV_EXCL_START
  if (tableId == INVALID_TABLEID) {
    CDE_LOG_WARN("Get invalid tableId");
    return CDE_ERROR;
  }
  // LCOV_EXCL_STOP

  const dd::String_type engineName(DSTORE_ENGINE_NAME);

  if ((dc->*lookupFunc)(engineName, tableId, &dbName, &tableName)) {
    CDE_LOG_WARN("Failed to get table name for tableId %llu",
                 (unsigned long long)tableId);
    return CDE_ERROR;
  }

  return CDE_OK;
}

static int GetTableNameByDDTable(dd::cache::Dictionary_client *dc,
                                 const dd::Table *ddTable,
                                 dd::String_type &dbName,
                                 dd::String_type &tableName) {
  return GetTableNameInternal(
      dc, ddTable, dbName, tableName,
      &dd::cache::Dictionary_client::get_table_name_by_se_private_id);
}

static int GetTableNameByDDPartition(dd::cache::Dictionary_client *dc,
                                     const dd::Partition *ddPartition,
                                     dd::String_type &dbName,
                                     dd::String_type &tableName) {
  return GetTableNameInternal(
      dc, ddPartition, dbName, tableName,
      &dd::cache::Dictionary_client::get_table_name_by_partition_se_private_id);
}

/**
Get the database name and table name for a partitioned table.
If secondary partitions exist, retrieve from a secondary partition;
otherwise, retrieve from a primary partition.
*/
static int GetPartTableNameAndDBName(dd::cache::Dictionary_client *dc,
                                     const dd::Table *ddTable,
                                     dd::String_type &tableName,
                                     dd::String_type &dbName) {
  if (ddTable == nullptr) {
    return CDE_ERROR;
  }

  const auto &partitions = ddTable->partitions();
  if (partitions.size() == 0) {
    return CDE_ERROR;
  }

  const dd::Partition *targetPart = nullptr;
  const auto &subPartitions = partitions[0]->subpartitions();
  if (subPartitions.size() != 0) {
    /* Has secondary partitions, use the first secondary partition */
    targetPart = subPartitions[0];
  } else {
    /* No secondary partitions, use the first primary partition */
    targetPart = partitions[0];
  }

  if (GetTableNameByDDPartition(dc, targetPart, dbName, tableName) != CDE_OK ||
      dbName.size() == 0 || tableName.size() == 0) {
    return CDE_ERROR;
  }
  return CDE_OK;
}

struct PartitionProcessContext {
  THD *thd{nullptr};
  TABLE *outTable{nullptr};
  dd::String_type dbName;
  dd::String_type tableName;
  int fillFactor{DEFAULT_FILLFACTOR};
  bool isDstoreIndexTable{false};

  PartitionProcessContext(THD *t, TABLE *table, const dd::String_type &db,
                          const dd::String_type &tbl, int factor,
                          bool isIndex = false)
      : thd(t),
        outTable(table),
        dbName(db),
        tableName(tbl),
        fillFactor(factor),
        isDstoreIndexTable(isIndex) {}
};

struct PartitionTableSearchContext {
  THD *thd{nullptr};
  dd::cache::Dictionary_client *dc{nullptr};
  const dd::Table *ddTable{nullptr};
  TABLE *outTable{nullptr};
  DSTORE::PageId targetSegmentId{DSTORE::INVALID_VFS_FILE_ID,
                                 DSTORE::DSTORE_INVALID_BLOCK_NUMBER};
  bool foundTargetSegment{false};
  bool isDstoreIndexTable{false};

  PartitionTableSearchContext(THD *thdPtr, dd::cache::Dictionary_client *dcPtr,
                              const dd::Table *ddTablePtr, TABLE *outputTable,
                              const DSTORE::PageId &targetId,
                              bool isIndexTable = false)
      : thd(thdPtr),
        dc(dcPtr),
        ddTable(ddTablePtr),
        outTable(outputTable),
        targetSegmentId(targetId),
        isDstoreIndexTable(isIndexTable) {}
};

/**
Process a single partition, including the indexes on that partition
*/
static int ProcessSinglePartition(const dd::Partition *part,
                                  PartitionProcessContext &ctx) {
  if (!part) return CDE_OK;

  dd::String_type partitionName = part->name();
  DSTORE::PageId segmentId =
      GetSegmentIdByProperties(part->se_private_data(), false);

  if (!segmentId.IsValid()) {
    CDE_LOG_WARN("Invalid segment ID for partition %s", partitionName.c_str());
    return CDE_ERROR;
  }

  /* IS.dstore_indexes does not need to process heap segments */
  if (!ctx.isDstoreIndexTable) {
    if (DstoreProcessPartHeapSegment(part, ctx.dbName, ctx.tableName,
                                     ctx.fillFactor, ctx.thd,
                                     ctx.outTable) != CDE_OK) {
      CDE_LOG_WARN("Failed to process heap segment for partition %s",
                   partitionName.c_str());
      return CDE_ERROR;
    }
  }

  /* IS.dstore_indexes does not need to process heap segments */
  const auto &partitionIndexes = part->indexes();
  for (const dd::Partition_index *partIndex : partitionIndexes) {
    if (ctx.isDstoreIndexTable) {
      if (DstoreProcessPartitionIndexForIndexTable(
              partIndex, ctx.dbName, ctx.tableName, partitionName, ctx.thd,
              ctx.outTable) != CDE_OK) {
        CDE_LOG_WARN(
            "Failed to process index for IS.dstore_indexes, partition %s",
            partitionName.c_str());
        return CDE_ERROR;
      }
    } else {
      if (DstoreProcessPartitionIndex(partIndex, ctx.dbName, ctx.tableName,
                                      partitionName, ctx.thd,
                                      ctx.outTable) != CDE_OK) {
        CDE_LOG_WARN(
            "Failed to process index for IS.dstore_segment, partition %s",
            partitionName.c_str());
        return CDE_ERROR;
      }
    }
  }

  return CDE_OK;
}

/**
Find and process the target segment within a partition
*/
static int FindAndProcessPartitionBySegmentId(
    PartitionTableSearchContext &context, const dd::Partition *part,
    const dd::String_type &dbName, const dd::String_type &tableName,
    int fillFactor) {
  if (!part) return CDE_OK;
  /* Check heap segment */
  DSTORE::PageId segmentId =
      GetSegmentIdByProperties(part->se_private_data(), false);
  if (!context.isDstoreIndexTable && segmentId == context.targetSegmentId) {
    // IS.dstore_indexes do not need process heap segment
    context.foundTargetSegment = true;
    return DstoreProcessPartHeapSegment(part, dbName, tableName, fillFactor,
                                        context.thd, context.outTable);
  }

  /* Check index segment */
  for (const dd::Partition_index *partIndex : part->indexes()) {
    DSTORE::PageId indexSegmentId =
        GetSegmentIdByProperties(partIndex->se_private_data(), false);
    if (indexSegmentId == context.targetSegmentId) {
      context.foundTargetSegment = true;
      if (context.isDstoreIndexTable) {
        return DstoreProcessPartitionIndexForIndexTable(
            partIndex, dbName, tableName, part->name(), context.thd,
            context.outTable);
      } else {
        return DstoreProcessPartitionIndex(partIndex, dbName, tableName,
                                           part->name(), context.thd,
                                           context.outTable);
      }
    }
  }

  return CDE_OK;
}

/**
Process a partitioned table to find the partition (could be heap or index)
that matches context.targetSegmentId.
1. If secondary partitions exist, check secondary partitions.
2. Otherwise, only check primary partitions.
*/
static int DstoreProcessPartitionBySegmentId(
    PartitionTableSearchContext &context) {
  if (!context.ddTable) return CDE_ERROR;

  dd::String_type tableName, dbName;
  if (GetPartTableNameAndDBName(context.dc, context.ddTable, tableName,
                                dbName) != CDE_OK ||
      dbName.empty() || tableName.empty()) {
    return CDE_ERROR;
  }

  int fillFactor =
      DstoreParseFillfactor(context.ddTable->comment().c_str(), nullptr);
  if (fillFactor == 0) {
    fillFactor = g_defaultFillfactor;
  }

  /* Search target segment */
  const auto &partitions = context.ddTable->partitions();
  for (const dd::Partition *part : partitions) {
    if (!part) continue;

    const auto &subPartitions = part->subpartitions();

    /* Search secondary partition */
    if (subPartitions.size() != 0) {
      for (const dd::Partition *subPart : subPartitions) {
        int ret = FindAndProcessPartitionBySegmentId(context, subPart, dbName,
                                                     tableName, fillFactor);
        if (ret != CDE_OK) {
          return ret;
        }
        /* The segmentId of subPart corresponds to targetSegmentId */
        if (context.foundTargetSegment) {
          return CDE_OK;
        }
      }
    } else {
      /* Search primary partition */
      int ret = FindAndProcessPartitionBySegmentId(context, part, dbName,
                                                   tableName, fillFactor);
      if (ret != CDE_OK) {
        return ret;
      }
      /* The segmentId of part corresponds to targetSegmentId */
      if (context.foundTargetSegment) {
        return CDE_OK;
      }
    }
  }

  return CDE_OK;
}

/**
 * Populates INFORMATION_SCHEMA.dstore_segment statistical information related
 * to index segments.
 * 1. Retrieves page usage information for index segments from DSTORE
 * 2. Fills the INFORMATION_SCHEMA.dstore_segment table
 */
static int DstoreFillIndexSegment(SegmentProcessContext &segProCtx,
                                  SegmentTableInfo &segTbInfo) {
  /* only collect Dstore table infomation */
  if (segProCtx.ddTable->engine() != DSTORE_ENGINE_NAME) {
    return CDE_OK;
  }
  /* step 1.Get indexSegment pageUseCondition */
  bool isValid = true;
  DSTORE::PageUseCondition puc;
  puc.init();
  DSTORE::TableSpaceDiagnose::GetSegmentPageUseCondition(
      segProCtx.targetSegmentId, &isValid, &puc);
  if (!isValid) {
    CDE_LOG_WARN(
        "DstoreFillIndexSegment get indexSegment page use condition failed!");
    return CDE_ERROR;
  }

  /* step 2.Fill INFORMATION_SCHEMA.dstore_segment table */
  if (DstoreSegmentStatsFillTableInternal(puc, segTbInfo, segProCtx.thd,
                                          segProCtx.outTable) != CDE_OK) {
    CDE_LOG_ERROR("DstoreSegmentStatsFillTableInternal failed ");
    return CDE_ERROR;
  }
  return CDE_OK;
}

/**
 * Populates INFORMATION_SCHEMA.dstore_indexes statistical information.
 * 1. Retrieves index diagnose from DSTORE
 * 2. Fills the INFORMATION_SCHEMA.dstore_indexes table
 */
static int DstoreFillIndexTable(SegmentProcessContext &segProCtx,
                                SegmentTableInfo &segTbInfo) {
  /* only collect Dstore table infomation */
  if (segProCtx.ddTable->engine() != DSTORE_ENGINE_NAME) {
    return CDE_OK;
  }

  auto indexInfo = std::make_unique<DSTORE::IndexStatus>();

  DSTORE::RetStatus ret = DSTORE::IndexDiagnose::PrintIndexInfo(
      DSTORE::g_defaultPdbId, segProCtx.targetSegmentId.m_fileId,
      segProCtx.targetSegmentId.m_blockId, indexInfo.get());
  if (ret != DSTORE::DSTORE_SUCC) {
    CDE_LOG_ERROR("Get index diagnose from dstore failed, segmentId %hu,%u.",
                  segProCtx.targetSegmentId.m_fileId,
                  segProCtx.targetSegmentId.m_blockId);
    return CDE_ERROR;
  }

  if (DstoreIndexStatsFillTableInternal(indexInfo, segTbInfo, segProCtx.thd,
                                        segProCtx.outTable) != CDE_OK) {
    CDE_LOG_ERROR("DstoreIndexStatsFillTableInternal failed");
    return CDE_ERROR;
  }
  return CDE_OK;
}

/**
 * Populates INFORMATION_SCHEMA.dstore_segment statistical information related
 * to heap segments.
 * 1. Retrieves page usage information for heap segment from DSTORE
 * 2. Retrieves page usage information for lob segment from DSTORE
 * 3. Fills the INFORMATION_SCHEMA.dstore_segment table
 */
static int DstoreFillHeapSegment(SegmentProcessContext &segProCtx,
                                 SegmentTableInfo &segTbInfo) {
  /* only collect Dstore table infomation */
  if (segProCtx.ddTable->engine() != DSTORE_ENGINE_NAME) {
    return CDE_OK;
  }
  /* step 1.Get PageUseCondition for heapSegment */
  bool isValid = true;
  DSTORE::PageUseCondition puc;
  puc.init();
  DSTORE::TableSpaceDiagnose::GetSegmentPageUseCondition(
      segProCtx.targetSegmentId, &isValid, &puc);
  if (!isValid) {
    CDE_LOG_ERROR(
        "DstoreFillHeapSegment heapSegmentId for table %s is invalid, "
        "heapSegmentId is %hu,%u",
        segTbInfo.tableName.c_str(), segProCtx.targetSegmentId.m_fileId,
        segProCtx.targetSegmentId.m_blockId);
    return CDE_ERROR;
  }

  /* step 2.If table has LOB column , get PageUseCondition for lobSegment */
  DSTORE::PageId lobSegmentId;
  lobSegmentId.m_fileId = CDE::CdeTableGetLobFileId(segProCtx.ddTable);
  lobSegmentId.m_blockId = CDE::CdeTableGetLobBlockNumber(segProCtx.ddTable);
  if (lobSegmentId.IsValid()) {
    /*
      Reusing the puc parameter from the heapSegment collection,
      add lobSegment page usage into the heapSegment's total.
    */
    DSTORE::TableSpaceDiagnose::GetSegmentPageUseCondition(lobSegmentId,
                                                           &isValid, &puc);
    if (!isValid) {
      CDE_LOG_ERROR(
          "DstoreFillHeapSegment get page use condition for lobSegmentId "
          "failed.");
      return CDE_ERROR;
    }
  }
  /* step 3.Fills the INFORMATION_SCHEMA.dstore_segment table */
  if (DstoreSegmentStatsFillTableInternal(puc, segTbInfo, segProCtx.thd,
                                          segProCtx.outTable) != CDE_OK) {
    CDE_LOG_ERROR("DstoreSegmentStatsFillTableInternal failed.");
    return CDE_ERROR;
  }
  return CDE_OK;
}

/**
 * This function is used to populate the INFORMATION_SCHEMA.dstore_segment
 * table.
 * 1. Retrieves database name, table name, fillFactor, etc. for the heap segment
 * from SegmentProcessContext, and populates the SegmentTableInfo structure
 * 2. Calls DstoreFillHeapSegment to get the heap segment's page usage
 * condition, then fills the INFORMATION_SCHEMA.dstore_segment table
 */
static int DstoreProcessHeapSegment(SegmentProcessContext &segProCtx) {
  /* step 1.Get TableStructure */
  TABLE_SHARE ts;
  TABLE tableStructure;
  if (CdeAcquireUncacheTable(segProCtx.thd, segProCtx.ddTable,
                             segProCtx.dbName.c_str(), &ts,
                             &tableStructure) == CDE_FAIL) {
    CDE_LOG_WARN(
        "DstoreSegmentStatsFillTable get TABLE/TABLE_SHARE for %s failed",
        segProCtx.tableName.c_str());
    return CDE_ERROR;
  }

  auto table_guard = create_scope_guard(
      [&]() { CdeReleaseUncachedTable(&ts, &tableStructure); });

  /* step 2. Get fillFactor and Fill SegmentTableInfo struct*/
  SegmentTableInfo segTbInfo;
  segTbInfo.segmentIdStr = std::to_string(segProCtx.targetSegmentId.m_fileId) +
                           "," +
                           std::to_string(segProCtx.targetSegmentId.m_blockId);
  segTbInfo.segmentType = HEAP_SEGMENT;
  segTbInfo.indexName = NULL_STRING;
  segTbInfo.partitionName = NULL_STRING;
  segTbInfo.dbName = segProCtx.dbName.c_str();
  segTbInfo.tableName = segProCtx.tableName.c_str();

  int fillFactor =
      DstoreParseFillfactor(tableStructure.s->comment.str, nullptr);
  segTbInfo.fillFactor = fillFactor ? fillFactor : g_defaultFillfactor;

  /* step 3.Get the heap segment's page usage condition and fills the
   * INFORMATION_SCHEMA.dstore_segment table*/
  return DstoreFillHeapSegment(segProCtx, segTbInfo);
}

/**
This function is used to populate the INFORMATION_SCHEMA.dstore_segment for
partition table.
1. Retrieves database name, table name, fillFactor, etc. for the heap segment
from dd::Partition, and populates the SegmentTableInfo structure
2. Calls GetSegmentPageUseCondition to get the heap segment's page usage
3. Calls DstoreSegmentStatsFillTableInternal to populate the
INFORMATION_SCHEMA.dstore_segment
*/
static int DstoreProcessPartHeapSegment(const dd::Partition *part,
                                        const dd::String_type &dbName,
                                        const dd::String_type &tableName,
                                        int fillFactor, THD *thd,
                                        TABLE *outTable) {
  dd::String_type partitionName = part->name();

  DSTORE::PageId segmentId = {DSTORE::INVALID_VFS_FILE_ID,
                              DSTORE::DSTORE_INVALID_BLOCK_NUMBER};

  segmentId = GetSegmentIdByProperties(part->se_private_data(), false);

  if (!segmentId.IsValid()) {
    return CDE_ERROR;
  }

  bool isValid = true;
  DSTORE::PageUseCondition puc;
  puc.init();
  DSTORE::TableSpaceDiagnose::GetSegmentPageUseCondition(segmentId, &isValid,
                                                         &puc);
  if (!isValid) {
    CDE_LOG_ERROR(
        "DstoreProcessPartHeapSegment for table %s is invalid, "
        "heapSegmentId is %hu,%u",
        tableName.c_str(), segmentId.m_fileId, segmentId.m_blockId);
    return CDE_ERROR;
  }

  /* Process lob segment */
  DSTORE::PageId lobSegmentId = {DSTORE::INVALID_VFS_FILE_ID,
                                 DSTORE::DSTORE_INVALID_BLOCK_NUMBER};

  lobSegmentId = GetSegmentIdByProperties(part->se_private_data(), true);
  if (lobSegmentId.IsValid()) {
    /* Reusing the puc parameter from the heapSegment collection,
      add lobSegment page usage into the heapSegment's total. */
    DSTORE::TableSpaceDiagnose::GetSegmentPageUseCondition(lobSegmentId,
                                                           &isValid, &puc);
    if (!isValid) {
      CDE_LOG_ERROR(
          "DstoreProcessPartHeapSegment get page use condition for lobSegment "
          "%hu,%u "
          "failed.",
          lobSegmentId.m_fileId, lobSegmentId.m_blockId);
      return CDE_ERROR;
    }
  }

  SegmentTableInfo segTbInfo;
  segTbInfo.segmentIdStr = std::to_string(segmentId.m_fileId) + "," +
                           std::to_string(segmentId.m_blockId);
  segTbInfo.segmentType = HEAP_SEGMENT;
  segTbInfo.indexName = NULL_STRING;
  segTbInfo.partitionName = partitionName.c_str();
  segTbInfo.dbName = dbName.c_str();
  segTbInfo.tableName = tableName.c_str();
  segTbInfo.fillFactor = fillFactor;

  if (DstoreSegmentStatsFillTableInternal(puc, segTbInfo, thd, outTable) !=
      CDE_OK) {
    CDE_LOG_ERROR(
        "DstoreProcessPartHeapSegment call DstoreSegmentStatsFillTableInternal "
        "for segment %hu,%u"
        "failed.",
        segmentId.m_fileId, segmentId.m_blockId);
    return CDE_ERROR;
  }
  return CDE_OK;
}

/**
This function is used to populate the INFORMATION_SCHEMA.dstore_segment for
partition table by PartitionIndex.
1. Calls GetSegmentPageUseCondition to get the heap segment's page usage
2. Calls DstoreSegmentStatsFillTableInternal to populate the
INFORMATION_SCHEMA.dstore_segment
*/
static int DstoreProcessPartitionIndex(const dd::Partition_index *partIndex,
                                       const dd::String_type &dbName,
                                       const dd::String_type &tableName,
                                       const dd::String_type &partitionName,
                                       THD *thd, TABLE *outTable) {
  dd::String_type partitionIndexName = partIndex->name();

  DSTORE::PageId indexSegmentId = {DSTORE::INVALID_VFS_FILE_ID,
                                   DSTORE::DSTORE_INVALID_BLOCK_NUMBER};

  indexSegmentId =
      GetSegmentIdByProperties(partIndex->se_private_data(), false);

  if (!indexSegmentId.IsValid()) {
    CDE_LOG_ERROR(
        "get indexSegmentId failed, tableName is %s, partitionIndexName is %s",
        tableName.c_str(), partitionIndexName.c_str());
    return CDE_ERROR;
  }
  int indexFillFactor =
      DstoreParseFillfactor(partIndex->index().comment().c_str(), nullptr);
  SegmentTableInfo segTbInfo;
  segTbInfo.segmentIdStr = std::to_string(indexSegmentId.m_fileId) + "," +
                           std::to_string(indexSegmentId.m_blockId);
  segTbInfo.segmentType = INDEX_SEGMENT;
  segTbInfo.indexName = partitionIndexName.c_str();
  segTbInfo.partitionName = partitionName.c_str();
  segTbInfo.dbName = dbName.c_str();
  segTbInfo.tableName = tableName.c_str();
  segTbInfo.fillFactor =
      indexFillFactor ? indexFillFactor : g_defaultFillfactor;

  bool isValid = true;
  DSTORE::PageUseCondition puc;
  puc.init();
  DSTORE::TableSpaceDiagnose::GetSegmentPageUseCondition(indexSegmentId,
                                                         &isValid, &puc);
  if (!isValid) {
    CDE_LOG_ERROR(
        "indexSegmentId for table %s is invalid, "
        "indexSegmentId is %hu,%u",
        tableName.c_str(), indexSegmentId.m_fileId, indexSegmentId.m_blockId);
    return CDE_ERROR;
  }
  if (DstoreSegmentStatsFillTableInternal(puc, segTbInfo, thd, outTable) !=
      CDE_OK) {
    CDE_LOG_ERROR(
        "DstoreSegmentStatsFillTableInternal failed, tableName is %s,indexName "
        "is %s",
        tableName.c_str(), partitionIndexName.c_str());
    return CDE_ERROR;
  }
  return CDE_OK;
}

/**
This function is used to populate the INFORMATION_SCHEMA.dstore_indexes for
partition table by PartitionIndex.
1. Calls DSTORE::IndexDiagnose::PrintIndexInfo
2. Calls DstoreIndexStatsFillTableInternal to populate the
INFORMATION_SCHEMA.dstore_indexes
*/
static int DstoreProcessPartitionIndexForIndexTable(
    const dd::Partition_index *partIndex, const dd::String_type &dbName,
    const dd::String_type &tableName, const dd::String_type &partitionName,
    THD *thd, TABLE *outTable) {
  dd::String_type partitionIndexName = partIndex->name();

  DSTORE::PageId indexSegmentId = {DSTORE::INVALID_VFS_FILE_ID,
                                   DSTORE::DSTORE_INVALID_BLOCK_NUMBER};

  indexSegmentId =
      GetSegmentIdByProperties(partIndex->se_private_data(), false);

  if (!indexSegmentId.IsValid()) {
    CDE_LOG_ERROR("get indexSegmentId failed, table %s index %s",
                  tableName.c_str(), partitionIndexName.c_str());
    return CDE_ERROR;
  }

  auto indexInfo = std::make_unique<DSTORE::IndexStatus>();

  DSTORE::RetStatus ret = DSTORE::IndexDiagnose::PrintIndexInfo(
      DSTORE::g_defaultPdbId, indexSegmentId.m_fileId, indexSegmentId.m_blockId,
      indexInfo.get());
  if (ret != DSTORE::DSTORE_SUCC) {
    CDE_LOG_ERROR("Get index diagnose from dstore failed, segmentId %hu,%u.",
                  indexSegmentId.m_fileId, indexSegmentId.m_blockId);
    return CDE_ERROR;
  }

  SegmentTableInfo segTbInfo;
  segTbInfo.segmentIdStr = std::to_string(indexSegmentId.m_fileId) + "," +
                           std::to_string(indexSegmentId.m_blockId);
  segTbInfo.indexName = partitionIndexName.c_str();
  segTbInfo.partitionName = partitionName.c_str();
  segTbInfo.dbName = dbName.c_str();
  segTbInfo.tableName = tableName.c_str();

  if (DstoreIndexStatsFillTableInternal(indexInfo, segTbInfo, thd, outTable) !=
      CDE_OK) {
    CDE_LOG_ERROR("DstoreIndexStatsFillTableInternal for table %s failed",
                  tableName.c_str());
    return CDE_ERROR;
  }
  return CDE_OK;
}

/**
 * @brief Processes index segment for INFORMATION_SCHEMA.dstore_indexes OR
 * INFORMATION_SCHEMA.dstore_segment
 *
 * This function performs two main operations:
 * 1. Retrieves index segment metadata (database name, table name, fill factor,
 * etc.) from the SegmentProcessContext and populates the SegmentTableInfo
 * structure
 * 2. Calls DstoreFillIndexSegment OR DstoreFillIndexTable to obtain the index
 * diagnose, then updates either INFORMATION_SCHEMA.dstore_indexes or
 * dstore_segment system table based on the parameter
 *
 * @param[in] SegmentProcessContext Segment processing context, containing
 * target segment ID, dictionary client, and other information
 * @param[in] idxNo Index number identifying the index to be processed
 * @param[in] isDstoreIndexTable Processing flag
 *        true: Fill INFORMATION_SCHEMA.dstore_indexes system table
 *        false: Fill INFORMATION_SCHEMA.dstore_segment system table
 *
 * @retval 0 Success
 * @retval Non-zero Error code indicating the specific failure type
 *
 * @see DstoreFillIndexSegment, SegmentTableInfo
 */
static int DstoreProcessIndexSegment(SegmentProcessContext &segProCtx,
                                     size_t idxNo,
                                     bool isDstoreIndexTable = false) {
  TABLE_SHARE ts;
  TABLE tableStructure;
  if (CdeAcquireUncacheTable(segProCtx.thd, segProCtx.ddTable,
                             segProCtx.dbName.c_str(), &ts,
                             &tableStructure) == CDE_FAIL) {
    CDE_LOG_WARN(
        "DstoreSegmentStatsFillTable get TABLE/TABLE_SHARE for %s failed",
        segProCtx.tableName.c_str());
    return CDE_ERROR;
  }

  auto table_guard = create_scope_guard(
      [&]() { CdeReleaseUncachedTable(&ts, &tableStructure); });

  /* step 2. Get fillFactor, indexName and Fill SegmentTableInfo struct */
  const KEY *key = tableStructure.key_info + idxNo;
  int indexFillFactor = DstoreParseFillfactor(key->comment.str, nullptr);

  SegmentTableInfo segTbInfo;
  segTbInfo.segmentIdStr = std::to_string(segProCtx.targetSegmentId.m_fileId) +
                           "," +
                           std::to_string(segProCtx.targetSegmentId.m_blockId);
  segTbInfo.segmentType = INDEX_SEGMENT;
  segTbInfo.dbName = segProCtx.dbName.c_str();
  segTbInfo.tableName = segProCtx.tableName.c_str();
  segTbInfo.partitionName = NULL_STRING;

  const dd::Index *index_def = segProCtx.ddTable->indexes().at(idxNo);
  segTbInfo.indexName = CDE::CdeDdIndexGetName(index_def);
  segTbInfo.fillFactor =
      indexFillFactor ? indexFillFactor : g_defaultFillfactor;

  /* INFORAMTION_SCHEMA.dstore_segment table */
  if (!isDstoreIndexTable) {
    return DstoreFillIndexSegment(segProCtx, segTbInfo);
  } else {
    /* INFORAMTION_SCHEMA.dstore_indexes table */
    return DstoreFillIndexTable(segProCtx, segTbInfo);
  }
}

/* Get heapSegmentId by ddTable */
static DSTORE::PageId GetHeapSegmentId(const dd::Table *table) {
  DSTORE::PageId id = {DSTORE::INVALID_VFS_FILE_ID,
                       DSTORE::DSTORE_INVALID_BLOCK_NUMBER};
  if (table) {
    id.m_fileId = CDE::CdeDdTableGetSePrivateDataRelfileid(table);
    id.m_blockId = CDE::CdeDdTableGetSePrivateDataRelblknum(table);
  }
  return id;
}

/* Get indexSegmentId by ddTable */
static DSTORE::PageId GetIndexSegmentId(const dd::Table *table,
                                        size_t indexIdx) {
  DSTORE::PageId id = {DSTORE::INVALID_VFS_FILE_ID,
                       DSTORE::DSTORE_INVALID_BLOCK_NUMBER};
  if (table && indexIdx < table->indexes().size()) {
    id.m_fileId = CdeDdIndexGetSePrivateDataRelfileid(table, indexIdx);
    id.m_blockId = CdeDdIndexGetSePrivateDataRelblknum(table, indexIdx);
  }
  return id;
}

/**
 * This function is used to populate the INFORMATION_SCHEMA.dstore_segment
 * table.
 * 1. Get heapSegmentId by ddTable
 * 2. Calls DstoreProcessHeapSegment to get the heap segment's page usage
 * condition, then fills the INFORMATION_SCHEMA.dstore_segment table
 */
static int DstoreProcessTableHeapSegment(SegmentProcessContext &ctx) {
  /* fill segmentId */
  ctx.targetSegmentId = GetHeapSegmentId(ctx.ddTable);
  return DstoreProcessHeapSegment(ctx);
}

/**
 * Processes index segments of a table and populates either the
 * INFORMATION_SCHEMA.dstore_segment or INFORMATION_SCHEMA.dstore_indexes system
 * table.
 *
 * This function iterates through all indexes of the specified table, optionally
 * filtered by index name. For each matching index, it retrieves the
 * corresponding segment ID, processes the index segment to obtain index
 * diagnose, and writes the results to the output system table.
 *
 * @param thd                 Thread handler containing execution context
 * @param dc                 Dictionary client for accessing data dictionary
 * @param ddTable            Pointer to the data dictionary table object
 * @param outTable           Output table (either dstore_segment or
 * dstore_indexes)
 * @param indexNameFilter    Optional filter for processing only a specific
 * index by name. Empty string indicates no filtering.
 * @param isDstoreIndexTable If true, populates
 * INFORMATION_SCHEMA.dstore_indexes; if false, populates
 * INFORMATION_SCHEMA.dstore_segment
 *
 * @return CDE_OK on success, error code on failure. When indexNameFilter is
 * specified, processing stops after the first matching index. Without filter,
 * all indexes are processed.
 */
static int DstoreProcessTableIndexSegments(
    SegmentProcessContext &inputCtx, const std::string &indexNameFilter = "",
    bool isDstoreIndexTable = false) {
  bool hasIndexFilter = !indexNameFilter.empty();

  const dd::Table *ddTable = inputCtx.ddTable;
  for (size_t idxNo = 0; idxNo < ddTable->indexes().size(); ++idxNo) {
    if (hasIndexFilter) {
      const dd::Index *indexDef = ddTable->indexes().at(idxNo);
      std::string indexName = CDE::CdeDdIndexGetName(indexDef);
      if (CdeStrcasecmp(indexName.c_str(), indexNameFilter.c_str()) != 0) {
        continue;
      }
    }

    /* fill inputCtx.targetSegmentId */
    inputCtx.targetSegmentId = GetIndexSegmentId(ddTable, idxNo);
    int ret = DstoreProcessIndexSegment(inputCtx, idxNo, isDstoreIndexTable);
    if (ret != CDE_OK) {
      CDE_LOG_WARN("Failed to process index segment %lu", idxNo);
      return ret;
    }

    if (hasIndexFilter) {
      IncPerfCountForPushDown(isDstoreIndexTable);
      break;
    }
  }

  return CDE_OK;
}

/**
 * Processes a specific segment by its ID and populates
 * INFORMATION_SCHEMA.dstore_segment OR INFORMATION_SCHEMA.dstore_indexes.
 *
 * This function searches through all data dictionary tables to find the table
 * containing the specified segment ID (which can be either a heap segment or an
 * index segment). When a matching table is found, it acquires the necessary
 * metadata locks, reacquires the table with the lock, and processes either the
 * heap segment or the matching index segment to populate the output system
 * table.
 *
 * The function iterates through all tables in the data dictionary, filtering
 * for DSTORE engine tables. For each candidate table, it compares the target
 * segment ID against both the heap segment ID and all index segment IDs. When a
 * match is found, the corresponding processing function is called to populate
 * the output table.
 *
 * @param thd                Thread handler containing execution context
 * @param dc                 Dictionary client for accessing data dictionary
 * @param outTable           Output system table to populate with segment
 * information
 * @param targetSegmentId    The segment ID to search for (can be heap or index
 * segment)
 * @param isDstoreIndexTable If true, processes for
 * INFORMATION_SCHEMA.dstore_indexes; if false, processes for
 * INFORMATION_SCHEMA.dstore_segment
 *
 * @return CDE_OK on success or if no matching segment is found (treated as
 * success), CDE_ERROR on dictionary access failures, lock acquisition failures,
 * or segment processing failures.
 *
 * @note The function acquires a shared metadata lock on the table before
 * processing to ensure consistency. The lock is automatically released when the
 * function returns via a scope guard.
 * @note The function get dd::Table by Dictionary_client, it will be
 * automatically released by caller.
 */
static int DstoreProcessBySegmentId(THD *thd, dd::cache::Dictionary_client *dc,
                                    TABLE *outTable,
                                    const DSTORE::PageId &targetSegmentId,
                                    bool isDstoreIndexTable = false) {
  std::vector<const dd::Table *> tableVector;
  if (dc->fetch_global_components(&tableVector)) {
    CDE_LOG_ERROR("Failed to get all DD tables in DstoreProcessBySegmentId");
    return CDE_ERROR;
  }

  for (const dd::Table *ddTable : tableVector) {
    if (ddTable->engine() != DSTORE_ENGINE_NAME) {
      continue;
    }

    // LCOV_EXCL_START
    /* Check ddTable info if the table is partition Table */
    if (!IsPartitionInfoConsistent(ddTable)) {
      CDE_LOG_WARN("Partition table information is wrong");
      continue;
    }
    // LCOV_EXCL_STOP

    /* not partition table */
    if (ddTable->partition_type() == dd::Table::PT_NONE) {
      dd::String_type dbName, tableName;
      if (GetTableNameByDDTable(dc, ddTable, dbName, tableName) != CDE_OK) {
        CDE_LOG_ERROR("Failed to get tableName for segment %d,%u",
                      targetSegmentId.m_fileId, targetSegmentId.m_blockId);
        return CDE_ERROR;
      }

      /* If processes for INFORMATION_SCHEMA.dstore_segment, Check whether
       * targetSegmentId corresponds to a heapSegment */
      if (!isDstoreIndexTable) {
        DSTORE::PageId heapId = GetHeapSegmentId(ddTable);
        if (heapId == targetSegmentId) {
          IncPerfCountForPushDown(isDstoreIndexTable);
          SegmentProcessContext ctx(thd, dc, ddTable, targetSegmentId, outTable,
                                    dbName, tableName);
          return DstoreProcessHeapSegment(ctx);
        }
      }

      /* Check whether targetSegmentId corresponds to an indexSegment */
      for (size_t idxNo = 0; idxNo < ddTable->indexes().size(); ++idxNo) {
        DSTORE::PageId indexId = GetIndexSegmentId(ddTable, idxNo);
        if (indexId == targetSegmentId) {
          IncPerfCountForPushDown(isDstoreIndexTable);
          SegmentProcessContext ctx(thd, dc, ddTable, targetSegmentId, outTable,
                                    dbName, tableName);
          return DstoreProcessIndexSegment(ctx, idxNo, isDstoreIndexTable);
        }
      }
      /* not found targetSegmentId, continue to process next table */
    } else {
      /* Iterate through all partitions and partition indexes in the current
      partitioned table to check if they match the targetSegmentId */
      PartitionTableSearchContext context(thd, dc, ddTable, outTable,
                                          targetSegmentId, isDstoreIndexTable);
      if (DstoreProcessPartitionBySegmentId(context) != CDE_OK) {
        return CDE_ERROR;
      }
      /* find targetSegment，return CDE_OK */
      if (context.foundTargetSegment) {
        return CDE_OK;
      }
      /* not found targetSegmentId, continue to process next table */
    }
  }

  CDE_LOG_DEBUG("No segment found in DstoreProcessBySegmentId with id %d,%u",
                targetSegmentId.m_fileId, targetSegmentId.m_blockId);
  return CDE_OK;
}

/**
 * Processes heap and index segments for a specified table and populates system
 * table information.
 *
 * This function retrieves a table by its database and table name, acquires
 * necessary metadata locks, and processes its segments to populate the output
 * system table. Depending on the mode flag, it either processes both heap and
 * index segments (for INFORMATION_SCHEMA.dstore_segment) or only index segments
 * (for INFORMATION_SCHEMA.dstore_indexes).
 *
 * The function performs the following steps:
 * 1. Acquires a shared metadata lock on the specified table
 * 2. Reacquires the table object with the lock held for consistency
 * 3. Verifies the table uses the DSTORE engine
 * 4. Processes the heap segment (if not for INFORMATION_SCHEMA.dstore_indexes)
 * 5. Processes all index segments, with optional index name filtering
 *
 * @param thd                 Thread handler containing execution context
 * @param dc                  Dictionary client for accessing data dictionary
 * @param outTable            Output system table to populate with segment
 * information
 * @param filters             Filter conditions containing database name, table
 * name, and optional index name for filtering
 * @param isDstoreIndexTable  If true, processes only index segments for
 *                           INFORMATION_SCHEMA.dstore_indexes; if false,
 * processes both heap and index segments for INFORMATION_SCHEMA.dstore_segment
 *
 * @return CDE_OK on successful processing of all required segments,
 *         CDE_ERROR on any failure including:
 *         - Metadata lock acquisition failure
 *         - Table acquisition failure
 *         - Heap segment processing failure
 *         - Index segment processing failure
 *
 * @note Returns CDE_OK (not error) if the table does not use DSTORE engine
 * @note The metadata lock is automatically released when the function returns
 *       via a scope guard
 * @note When isDstoreIndexTable is true, heap segment processing is skipped
 * entirely
 * @note The filters.indexName parameter allows filtering to a specific index;
 *       if empty, all indexes are processed
 * @note The function get dd::Table by Dictionary_client, it will be
 * automatically released by caller.
 */
static int DstoreProcessByTableName(THD *thd, dd::cache::Dictionary_client *dc,
                                    TABLE *outTable,
                                    const SegmentFilterConditions &filters,
                                    bool isDstoreIndexTable = false) {
  dd::String_type targetDb(filters.dbName.c_str()),
      targetTable(filters.tableName.c_str());
  std::vector<const dd::Table *> tableVector;
  if (dc->fetch_global_components(&tableVector)) {
    CDE_LOG_ERROR("Failed to get all DD tables in DstoreProcessByTableName");
    return CDE_ERROR;
  }
  /*
   * Using dc->acquire<dd::Table>(dbName, tableName) to retrieve the ddTable is
   * incorrect here, as the returned pointer may become invalid due to
   * concurrent DDL operations. In contrast, the ddTable obtained via
   * fetch_global_components is a fully independent copy (snapshot), ensuring
   * its content remains unaffected by concurrent DDL. Therefore, we iterate
   * through tableVector to find the corresponding ddTable.
   */
  const dd::Table *targetDDTable = nullptr;
  for (const dd::Table *ddTable : tableVector) {
    if (ddTable->engine() != DSTORE_ENGINE_NAME) {
      continue;
    }

    // LCOV_EXCL_START
    /* Check ddTable info if the table is partition Table */
    if (!IsPartitionInfoConsistent(ddTable)) {
      CDE_LOG_WARN("Partition table information is wrong");
      continue;
    }
    // LCOV_EXCL_STOP

    dd::String_type dbName, tableName;
    /* ddTable points to a Partition table */
    if (ddTable->partition_type() != dd::Table::PT_NONE) {
      if (GetPartTableNameAndDBName(dc, ddTable, tableName, dbName) != CDE_OK) {
        CDE_LOG_ERROR("Failed to get tableName for partition table");
        return CDE_ERROR;
      }
    } else {
      if (GetTableNameByDDTable(dc, ddTable, dbName, tableName) != CDE_OK) {
        CDE_LOG_ERROR("Failed to get tableName");
        return CDE_ERROR;
      }
    }

    if (CdeStrcasecmp(dbName.c_str(), targetDb.c_str()) != 0 ||
        CdeStrcasecmp(tableName.c_str(), targetTable.c_str()) != 0) {
      /* The ddTable does not match */
      continue;
    }
    /* Get taregtDDTable */
    targetDDTable = ddTable;
    break;
  }

  if (targetDDTable == nullptr) {
    CDE_LOG_INFO("Not find ddTable for [%s,%s]", filters.dbName.c_str(),
                 filters.tableName.c_str());
    return CDE_OK;
  }

  bool isParititonTable = targetDDTable->partition_type() != dd::Table::PT_NONE;
  if (isParititonTable) {
    return DstoreProcessSinglePartitionTable(thd, dc, targetDDTable, outTable,
                                             filters, isDstoreIndexTable);
  } else {
    SegmentProcessContext ctx(thd, dc, targetDDTable, DSTORE::INVALID_PAGE_ID,
                              outTable, targetDb, targetTable);
    /* INFORMATION_SCHEMA.dstore_indexes do not need process heapSegment */
    if (!isDstoreIndexTable) {
      int ret = DstoreProcessTableHeapSegment(ctx);
      if (ret != CDE_OK) {
        CDE_LOG_WARN("Failed to process heap segment for %s.%s",
                     filters.dbName.c_str(), filters.tableName.c_str());
        return CDE_ERROR;
      }
    }

    int ret = DstoreProcessTableIndexSegments(ctx, filters.indexName,
                                              isDstoreIndexTable);
    if (ret != CDE_OK) {
      CDE_LOG_WARN("Failed to process index segments for %s.%s",
                   filters.dbName.c_str(), filters.tableName.c_str());
      return CDE_ERROR;
    }
  }

  return CDE_OK;
}

/**
 * Processes heap and index segments for a single DSTORE table and populates
 * system table information.
 *
 * This function processes a single table's segments to populate either
 * INFORMATION_SCHEMA.dstore_segment or INFORMATION_SCHEMA.dstore_indexes. It
 * first extracts the table's database and table names, applies optional
 * filters, acquires necessary metadata locks, and then processes the table's
 * heap and index segments accordingly.
 *
 * The function performs the following steps:
 * 1. Extracts database and table names from the data dictionary table object
 * 2. Applies optional database and table name filters (if specified in filters)
 * 3. Acquires a shared metadata lock on the table
 * 4. Reacquires the table object with the lock held for consistency
 * 5. Verifies the table uses the DSTORE engine
 * 6. Processes the heap segment (if not in index-only mode)
 * 7. Processes index segments with optional index name filtering
 *
 * @param thd                 Thread handler containing execution context
 * @param dc                  Dictionary client for accessing data dictionary
 * @param ddTable             Data dictionary table object to process
 * @param outTable            Output system table to populate with segment
 * information
 * @param filters             Filter conditions containing optional database
 * name, table name, and index name filters
 * @param isDstoreIndexTable  If true, processes only index segments for
 *                           INFORMATION_SCHEMA.dstore_indexes; if false,
 * processes both heap and index segments for INFORMATION_SCHEMA.dstore_segment
 *
 * @return CDE_OK on successful processing of the table's segments,
 *         CDE_ERROR on any failure
 *
 * @note The metadata lock is automatically released when the function returns
 * via a scope guard
 * @note When isDstoreIndexTable is true, heap segment processing is skipped
 * entirely
 * @note The filters.indexName parameter allows filtering to a specific index;
 *       if empty, all indexes are processed
 * @note The function is designed to be called from a loop that iterates over
 * multiple tables
 */
static int DstoreProcessSingleTable(THD *thd, dd::cache::Dictionary_client *dc,
                                    const dd::Table *ddTable, TABLE *outTable,
                                    const SegmentFilterConditions &filters,
                                    bool isDstoreIndexTable = false) {
  /* Get dbName and tableName */
  dd::String_type dbName, tableName;
  if (GetTableNameByDDTable(dc, ddTable, dbName, tableName) != CDE_OK) {
    CDE_LOG_WARN(
        "Failed to get dbName and tableName in DstoreProcessSingleTable");
    return CDE_ERROR;
  }

  std::string dbNameStr(dbName.c_str());
  std::string tableNameStr(tableName.c_str());

  /* Filter based on dbName and tableName */
  if (filters.hasDbName &&
      CdeStrcasecmp(dbName.c_str(), filters.dbName.c_str()) != 0) {
    return CDE_OK;
  }
  if (filters.hasTableName &&
      CdeStrcasecmp(tableName.c_str(), filters.tableName.c_str()) != 0) {
    return CDE_OK;
  }
  if (filters.hasTableName) {
    IncPerfCountForPushDown(isDstoreIndexTable);
  }
  if (filters.hasDbName) {
    IncPerfCountForPushDown(isDstoreIndexTable);
  }

  SegmentProcessContext ctx(thd, dc, ddTable, DSTORE::INVALID_PAGE_ID, outTable,
                            dbName, tableName);
  /* INFORMATION_SCHEMA.dstore_indexes do not need process heapSegment */
  if (!isDstoreIndexTable) {
    /* Process heap segment */
    if (DstoreProcessTableHeapSegment(ctx) != CDE_OK) {
      CDE_LOG_WARN("Failed to processTableHeapSegment for table %s,%s",
                   dbName.c_str(), tableName.c_str());
      return CDE_ERROR;
    }
  }
  /* Process index segment */
  if (DstoreProcessTableIndexSegments(ctx, filters.indexName,
                                      isDstoreIndexTable) != CDE_OK) {
    CDE_LOG_WARN("Failed to processTableIndexSegments for table %s,%s",
                 dbName.c_str(), tableName.c_str());
    return CDE_ERROR;
  }

  return CDE_OK;
}

/**
This function processes all partitions and partition indexes (including
subpartitions) of a partitioned table and populates the corresponding
INFORMATION_SCHEMA table (dstore_segment or dstore_indexes). It handles both
primary and secondary partitions, applying filter conditions as needed.
*/
static int DstoreProcessSinglePartitionTable(
    THD *thd, dd::cache::Dictionary_client *dc, const dd::Table *ddTable,
    TABLE *outTable, const SegmentFilterConditions &filters,
    bool isDstoreIndexTable) {
  if (!ddTable || !thd || !outTable) {
    return CDE_ERROR;
  }

  /* Get partitioned table's database and table names */
  dd::String_type tableName, dbName;
  if (GetPartTableNameAndDBName(dc, ddTable, tableName, dbName) != CDE_OK ||
      dbName.empty() || tableName.empty()) {
    CDE_LOG_WARN("Failed to get partition table name for table");
    return CDE_ERROR;
  }

  /* Apply filter conditions */
  std::string dbNameStr(dbName.c_str());
  std::string tableNameStr(tableName.c_str());

  if (filters.hasDbName &&
      CdeStrcasecmp(dbName.c_str(), filters.dbName.c_str()) != 0) {
    return CDE_OK;
  }
  if (filters.hasTableName &&
      CdeStrcasecmp(tableName.c_str(), filters.tableName.c_str()) != 0) {
    return CDE_OK;
  }

  if (filters.hasTableName) {
    IncPerfCountForPushDown(isDstoreIndexTable);
  }
  if (filters.hasDbName) {
    IncPerfCountForPushDown(isDstoreIndexTable);
  }

  /* Get fill factor */
  int fillFactor = DstoreParseFillfactor(ddTable->comment().c_str(), nullptr);
  if (fillFactor == 0) {
    fillFactor = g_defaultFillfactor;
  }

  PartitionProcessContext ctx(thd, outTable, dbName, tableName, fillFactor,
                              isDstoreIndexTable);

  /* Process all partitions */
  const auto &partitions = ddTable->partitions();
  for (const dd::Partition *part : partitions) {
    if (!part) continue;

    const auto &subPartitions = part->subpartitions();

    /* Process secondary partitions */
    if (subPartitions.size() != 0) {
      for (const dd::Partition *subPart : subPartitions) {
        if (ProcessSinglePartition(subPart, ctx) != CDE_OK) {
          return CDE_ERROR;
        }
      }
    } else {
      /* Process primary partition  */
      if (ProcessSinglePartition(part, ctx) != CDE_OK) {
        return CDE_ERROR;
      }
    }
  }
  return CDE_OK;
}

/**
1. Obtain the ddTable for all tables via fetch_global_components
2. Process all ddTables obtained in the previous step by calling
DstoreProcessSingleTable
3. All ddTable will be release by Auto_releaser when the function finish
*/
static int DstoreProcessAllTables(THD *thd, TABLE *outTable,
                                  const SegmentFilterConditions &filters,
                                  bool isDstoreIndexTable = false) {
  dd::cache::Dictionary_client *dc = thd->dd_client();
  dd::cache::Dictionary_client::Auto_releaser releaser(dc);

  std::vector<const dd::Table *> tableVector;
  if (dc->fetch_global_components(&tableVector)) {
    CDE_LOG_ERROR("Failed to get all DD tables");
    return CDE_ERROR;
  }

  for (const dd::Table *ddTable : tableVector) {
    if (ddTable->engine() != DSTORE_ENGINE_NAME) {
      continue;
    }

    // LCOV_EXCL_START
    /* Check ddTable info if the table is partition Table */
    if (!IsPartitionInfoConsistent(ddTable)) {
      CDE_LOG_WARN(
          "Partition table information is wrong, skip this partition table");
      continue;
    }
    /*Traversing and processing all tables can be time-consuming; if a KILL
    signal is received, the process will terminate early.*/
    if (thd->killed) {
      thd->send_kill_message();
      return CDE_ERROR;
    }
    // LCOV_EXCL_STOP
    /* Release memory immediately after processing each table, rather than
     * waiting for all tables to finish */
    dd::cache::Dictionary_client::Auto_releaser localReleaser(dc);
    /* process partition table */
    if (ddTable->partition_type() != dd::Table::PT_NONE) {
      int ret = DstoreProcessSinglePartitionTable(thd, dc, ddTable, outTable,
                                                  filters, isDstoreIndexTable);
      if (ret != CDE_OK) {
        /* An error occurred, log it and continue processing the next table */
        CDE_LOG_WARN(
            "DstoreProcessSinglePartitionTable Failed to process table");
      }
    } else {
      int ret = DstoreProcessSingleTable(thd, dc, ddTable, outTable, filters,
                                         isDstoreIndexTable);
      if (ret != CDE_OK) {
        /* An error occurred, log it and continue processing the next table */
        CDE_LOG_WARN("DstoreProcessSingleTable Failed to process table");
      }
    }
  }
  return CDE_OK;
}

/**
 * @brief Populates the INFORMATION_SCHEMA.dstore_segment table with segment
 * usage statistics.
 *
 * @details
 * Function Purpose:
 * This function populates the INFORMATION_SHCEMA.dstore_segment table
 * by querying and processing segment information from DSTORE engine tables.
 *
 * Main Processing Flow:
 * 1. Validation and permission checks
 * 2. Parse filter conditions from the query
 * 3. Route to appropriate processing function based on filters
 *
 * Function Call Hierarchy:
 * ┌─────────────────────────────────────────────────────────────┐
 * │  dstoreSegmentStatsFillTable (Main Entry Point)            │
 * ├─────────────────────────────────────────────────────────────┤
 * │  Case 1: SEGMENT_ID filter present                         │
 * │  │   └── DstoreProcessBySegmentId()                        │
 * │  │       ├── Fetch all DD tables without lock             │
 * │  │       ├── For each DD table:                           │
 * │  │       │   ├── Get table names (db/table)              │
 * │  │       │   ├── Check heap segment match                │
 * │  │       │   └── Check index segment matches             │
 * │  │                                                       │
 * │  Case 2: DB_NAME + TABLE_NAME filters present            │
 * │  │   └── DstoreProcessByTableName()                      │
 * │  │       ├── Get DD table object from dc                 │
 * │  │       ├── Process heap segment                        │
 * │  │       └── Process index segments                      │
 * │  │                                                       │
 * │  Case 3: No filter or partial filter only                │
 * │      └── DstoreProcessAllTables()                        │
 * │          └── Iterate through all DD tables               │
 * │              └── DstoreProcessSingleTable()                  │
 * │                  ├── getTableNames()                          │
 * │                  ├── Process heap segment                       │
 * │                  └── Process index segments                     │
 * └─────────────────────────────────────────────────────────────────┘
 * @param[in] thd    Thread context
 * @param[in] tables Table list structure
 * @param[in] item   Additional query item
 * @return 0 on success, non-zero on error
 */
static int DstoreSegmentStatsFillTable(THD *thd, Table_ref *tables,
                                       Item *item) {
  [[maybe_unused]] Field **fields = tables->table->field;
  const char *tableName = tables->table_name;

  if (CdeStrcasecmp(tableName, I_S_DSTORE_SEGMENT_STATS_TABLE_NAME) != 0) {
    return CDE_ERROR;
  }

  /* deny access to non-superusers */
  if (check_global_access(thd, PROCESS_ACL)) {
    return CDE_OK;
  }

  (void)CdeCreateOrGetSession(thd);

  /* Use mutex to prevent concurrent execute here. */
  std::unique_lock<std::mutex> lock(segStatsMgr.m_mtx);

  /* use item to do filter pushdown */
  FilterSet out;
  SegmentFilterConditions filters;
  if (item) {
    std::vector<int> filterIndex{
        DstoreSegStatsField::SEGMENT_ID, DstoreSegStatsField::DB_NAME,
        DstoreSegStatsField::TABLE_NAME, DstoreSegStatsField::INDEX_NAME};
    FilterPushdownCond(item, thd, filterIndex, out);

    if (!SegmentFilterConditions::SegParseFromFilterSet(out, filters)) {
      CDE_LOG_WARN("Failed to SegParseFromFilterSet");
      return CDE_OK;
    }
  }

  dd::cache::Dictionary_client *dc = thd->dd_client();
  dd::cache::Dictionary_client::Auto_releaser releaser(dc);

  /* Case 1: Filtering by SEGMENT_ID */
  if (filters.hasSegmentId) {
    cde_perf_counters::getInstance()->_dstore_segment_pushdown_hit.increment();
    if (DstoreProcessBySegmentId(thd, dc, tables->table, filters.segmentId) !=
        CDE_OK) {
      CDE_LOG_WARN("Failed to DstoreProcessBySegmentId for segmentId %hu,%u",
                   filters.segmentId.m_fileId, filters.segmentId.m_blockId);
    }
    return CDE_OK;
  }

  /* Case 2: Filtering by DB_NAME and TABLE_NAME */
  if (filters.hasDbName && filters.hasTableName) {
    cde_perf_counters::getInstance()->_dstore_segment_pushdown_hit.increment();
    if (DstoreProcessByTableName(thd, dc, tables->table, filters) != CDE_OK) {
      CDE_LOG_WARN("Failed to DstoreProcessByTableName for table %s,%s",
                   filters.dbName.c_str(), filters.tableName.c_str());
    }
    return CDE_OK;
  }

  /* Case 3: No filtering or partial filtering, iterate through all tables */
  if (DstoreProcessAllTables(thd, tables->table, filters) != CDE_OK) {
    CDE_LOG_WARN("Failed to DstoreProcessAllTables");
    /* return CDE_OK rather than returning CDE_ERROR to prevent service crashes
     */
    return CDE_OK;
  }
  return CDE_OK;
}

/**
Bind the dynamic table INFORMATION_SCHEMA.dstore_segment_stats

@param[in/out]      p   schema table to init

@return 0 on success
*/
static int DstoreSegmentStatsInit(void *p) {
  if (!p) {
    return CDE_ERROR;
  }
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;

  schema->fields_info = g_dstoreSegmentStatsFieldsInfo;
  schema->fill_table = DstoreSegmentStatsFillTable;
  return CDE_OK;
}

struct st_mysql_plugin g_infomationSchemaDstoreSegStats = {
    /* the plugin type (a MYSQL_XXX_PLUGIN value) */
    /* int */
    MYSQL_INFORMATION_SCHEMA_PLUGIN,

    /* pointer to type-specific plugin descriptor */
    /* void* */
    &g_infomationSchemaInfo,

    /* plugin name */
    /* const char* */
    I_S_DSTORE_SEGMENT_STATS_TABLE_NAME,

    /* plugin author (for SHOW PLUGINS) */
    /* const char* */
    PLUGIN_AUTHOR,

    /* general descriptive text (for SHOW PLUGINS) */
    /* const char* */
    "DStore segment statistic infomation",

    /* the plugin license (PLUGIN_LICENSE_XXX) */
    /* int */
    PLUGIN_LICENSE_GPL,

    /* the function to invoke when plugin is loaded */
    /* int (*)(void*); */
    DstoreSegmentStatsInit,

    /* the function to invoke when plugin is un installed */
    /* int (*)(void*); */
    nullptr,

    /* the function to invoke when plugin is unloaded */
    /* int (*)(void*); */
    nullptr,

    /* plugin version (for SHOW PLUGINS) */
    /* unsigned int */
    1,

    /* SHOW_VAR* */
    nullptr,

    /* SYS_VAR** */
    nullptr,

    /* reserved for dependency checking */
    /* void* */
    nullptr,

    /* Plugin flags */
    /* unsigned long */
    0UL,
};

/** The DSTORE_UNDO i_s table field id enum, note that the order of
this enum must be the same as the array g_dstoreUndoFieldsInfo
define. */
enum DstoreUndoField {
  UNDO_ZONE_ID,
  UNDO_ZONE_STATUS,
  PAGE_NUM,
  FREE_SLOT_NUM,
  RECYCLE_LOGICAL_SLOTID,
  NEXT_FREE_LOGICAL_SLOTID
};

static ST_FIELD_INFO g_dstoreUndoFieldsInfo[] = {
    {"undo_zone_id", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"undo_zone_status", UNDO_I_S_STATE_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "",
     0},
    {"page_num", MY_UINT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"free_slot_num", MY_UINT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"recycle_logical_slotid", MY_UINT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"next_free_logical_slotid", MY_UINT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    END_OF_ST_FIELD};

struct DstoreUndoMgr {
  /* To protect access concurrent */
  std::mutex m_mtx;
};

static DstoreUndoMgr undoMgr;

/**
Function to fill INFORMATION_SCHEMA.dstore_undo

@param[in]      thd
@param[in]      tables
@param[in]      item

@return 0 on success
*/
static int DstoreUndoFillTable(THD *thd, Table_ref *tables, Item *item) {
  (void)item;

  [[maybe_unused]] Field **fields = tables->table->field;
  const char *tableName = tables->table_name;

  if (CdeStrcasecmp(tableName, I_S_DSTORE_UNDO_TABLE_NAME) != 0) {
    return CDE_ERROR;
  }

  /** deny access to non-superusers:
  When permissions are insufficient, the system does not directly throw an error
  but instead employs security degradation. If check_global_access returns TRUE,
  it indicates that the current thread lacks the PROCESS_ACL permission for
  accessing system-level information, which is considered a normal and expected
  interception. We return CDE_OK (value 0) to indicate that the request has been
  processed normally (but no data is returned when permissions are
  insufficient). This approach aligns with InnoDB's handling.
  */
  if (check_global_access(thd, PROCESS_ACL)) {
    return CDE_OK;
  }

  (void)CdeCreateOrGetSession(thd);

  FilterSet filterInfo;
  DSTORE::ZoneId filterZoneId{-1};
  if (item) {
    std::vector<int> filterIndex{DstoreUndoField::UNDO_ZONE_ID};
    FilterPushdownCond(item, thd, filterIndex, filterInfo);
    if (filterInfo.count(DstoreUndoField::UNDO_ZONE_ID)) {
      auto zoneId = filterInfo[DstoreUndoField::UNDO_ZONE_ID].m_val;
      /* Zone id data type is unsigned, server will check whether condition
      value is negative, here we just check maximum value. */
      if (zoneId >= DSTORE::UNDO_ZONE_COUNT) {
        return CDE_OK;
      }
      filterZoneId = zoneId;
      cde_perf_counters::getInstance()
          ->_dstore_undo_zone_id_pushdown_hit.increment();
    }
  }

  auto FillEntry = [&](DSTORE::ZoneId zoneId, DSTORE::UndoZoneStatus &status) {
    CHECK_STORE_RET(fields[UNDO_ZONE_ID]->store(zoneId, true));
    CHECK_STORE_RET(FieldStoreString(
        fields[UNDO_ZONE_STATUS],
        status.isAsyncRollbacking ? "In Rolling Back" : "Not in Rolling Back"));
    CHECK_STORE_RET(fields[PAGE_NUM]->store(status.pageNum, true));
    CHECK_STORE_RET(fields[FREE_SLOT_NUM]->store(status.freeSlotNum, true));
    CHECK_STORE_RET(
        fields[RECYCLE_LOGICAL_SLOTID]->store(status.recycleLogicSlotId, true));
    CHECK_STORE_RET(fields[NEXT_FREE_LOGICAL_SLOTID]->store(
        status.nextFreeLogicSlotId, true));
    CHECK_STORE_RET(schema_table_store_record(thd, tables->table));
    return CDE_OK;
  };

  /* Use mutex to prevent concurrent execute here. */
  std::unique_lock<std::mutex> lck(undoMgr.m_mtx);

  /* Invoke diagnose api to collect undo zone info */
  DSTORE::UndoMgrDiagnose undoMgrDiag;
  DSTORE::UndoZoneStatus undoZoneStatus;
  DSTORE::PdbId pdbId = DSTORE::g_defaultPdbId;

  if (filterZoneId != -1) {
    DSTORE::RetStatus retStatus = undoMgrDiag.GetUndoZoneStatusByZid(
        pdbId, filterZoneId, &undoZoneStatus);
    if (retStatus != DSTORE::DSTORE_SUCC) {
      CDE_LOG_ERROR("GetUndoZoneStatusByZid failed.");
      return CDE_OK;
    }
    return FillEntry(filterZoneId, undoZoneStatus);
  }

  for (DSTORE::ZoneId zoneid = 0; zoneid < DSTORE::UNDO_ZONE_COUNT; ++zoneid) {
    DSTORE::RetStatus retStatus =
        undoMgrDiag.GetUndoZoneStatusByZid(pdbId, zoneid, &undoZoneStatus);

    if (retStatus != DSTORE::DSTORE_SUCC) {
      /* The process iterates through all undo zones, skipping any that are
       * temporarily unavailable. */
      continue;
    }
    if (CDE_OK != FillEntry(zoneid, undoZoneStatus)) {
      return CDE_ERROR;
    }
  }

  return CDE_OK;
}

/**
Bind the dynamic table INFORMATION_SCHEMA.dstore_mem_stats

@param[in/out]      p   schema table to init

@return 0 on success
*/

static int DstoreUndoInit(void *p) {
  if (!p) {
    return CDE_ERROR;
  }
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;

  schema->fields_info = g_dstoreUndoFieldsInfo;
  schema->fill_table = DstoreUndoFillTable;
  return CDE_OK;
}

struct st_mysql_plugin g_infomationSchemaDstoreUndo = {
    /* the plugin type (a MYSQL_XXX_PLUGIN value) */
    /* int */
    MYSQL_INFORMATION_SCHEMA_PLUGIN,

    /* pointer to type-specific plugin descriptor */
    /* void* */
    &g_infomationSchemaInfo,

    /* plugin name */
    /* const char* */
    I_S_DSTORE_UNDO_TABLE_NAME,

    /* plugin author (for SHOW PLUGINS) */
    /* const char* */
    PLUGIN_AUTHOR,

    /* general descriptive text (for SHOW PLUGINS) */
    /* const char* */
    "DStore undo infomation",

    /* the plugin license (PLUGIN_LICENSE_XXX) */
    /* int */
    PLUGIN_LICENSE_GPL,

    /* the function to invoke when plugin is loaded */
    /* int (*)(void*); */
    DstoreUndoInit,

    /* the function to invoke when plugin is un installed */
    /* int (*)(void*); */
    nullptr,

    /* the function to invoke when plugin is unloaded */
    /* int (*)(void*); */
    nullptr,

    /* plugin version (for SHOW PLUGINS) */
    /* unsigned int */
    1,

    /* SHOW_VAR* */
    nullptr,

    /* SYS_VAR** */
    nullptr,

    /* reserved for dependency checking */
    /* void* */
    nullptr,

    /* Plugin flags */
    /* unsigned long */
    0UL,
};

struct DstoreIndexesMgr {
  /* To protect access concurrent */
  std::mutex m_mtx;
};

static DstoreIndexesMgr dstoreIndexMgr;

/**
 * @brief Populates the INFORMATION_SCHEMA.dstore_indexes table
 *
 * @details
 * Function Purpose:
 * This function populates the INFORMATION_SHCEMA.dstore_indexes table
 * by querying and processing segment information from DSTORE engine tables.
 *
 * Main Processing Flow:
 * 1. Validation and permission checks
 * 2. Parse filter conditions from the query
 * 3. Route to appropriate processing function based on filters
 *
 * Function Call Hierarchy:
 * ┌─────────────────────────────────────────────────────────────┐
 * │  DstoreIndexesFillTable (Main Entry Point)            │
 * ├─────────────────────────────────────────────────────────────┤
 * │  Case 1: SEGMENT_ID filter present                         │
 * │  │   └── DstoreProcessBySegmentId(isDstoreIndexTable = true) │
 * │  │       ├── Fetch all DD tables                    │
 * │  │       ├── For each DD table:                           │
 * │  │       │   ├── Get table names (db/table)              │
 * │  │       │   └── Check index segment matche              │
 * │  │                                                       │
 * │  Case 2: DB_NAME + TABLE_NAME filters present            │
 * │  │   └── DstoreProcessByTableName(isDstoreIndexTable = true) |
 * │  │       ├── Get DD table object from dc                 │
 * │  │       └── Process index segments                      │
 * │  │                                                       │
 * │  Case 3: No filter or partial filter only                │
 * │      └── DstoreProcessAllTables(isDstoreIndexTable = true)  |
 * │          └── Iterate through all DD tables               │
 * │              └── DstoreProcessSingleTable()                  │
 * │                  ├── getTableNames()                          │
 * │                  └── Process index segments                     │
 * └─────────────────────────────────────────────────────────────────┘
 * @param[in] thd    Thread context
 * @param[in] tables Table list structure
 * @param[in] item   Additional query item
 * @return 0 on success, non-zero on error
 */
static int DstoreIndexesFillTable(THD *thd, Table_ref *tables, Item *item) {
  [[maybe_unused]] Field **fields = tables->table->field;
  const char *tableName = tables->table_name;

  if (CdeStrcasecmp(tableName, I_S_DSTORE_INDEXES_TABLE_NAME) != 0) {
    return CDE_ERROR;
  }

  /* deny access to non-superusers */
  if (check_global_access(thd, PROCESS_ACL)) {
    return CDE_OK;
  }

  (void)CdeCreateOrGetSession(thd);

  /* Use mutex to prevent concurrent execute here. */
  std::unique_lock<std::mutex> lck(dstoreIndexMgr.m_mtx);
  FilterSet out;
  SegmentFilterConditions filters;
  /* Use item to do filter */
  if (item) {
    std::vector<int> filterIndex{DstoreIndexesField::INDEX_DB_NAME,
                                 DstoreIndexesField::INDEX_TABLE_NAME,
                                 DstoreIndexesField::INDEX_INDEX_NAME,
                                 DstoreIndexesField::INDEX_SEGMENT_ID};
    FilterPushdownCond(item, thd, filterIndex, out);

    if (!SegmentFilterConditions::IndexParseFromFilterSet(out, filters)) {
      CDE_LOG_WARN("Failed to IndexParseFromFilterSet");
      return CDE_OK;
    }
  }

  dd::cache::Dictionary_client *dc = thd->dd_client();
  dd::cache::Dictionary_client::Auto_releaser releaser(dc);

  /* Case 1: Filtering by SEGMENT_ID */
  if (filters.hasSegmentId) {
    cde_perf_counters::getInstance()->_dstore_indexes_pushdown_hit.increment();
    if (DstoreProcessBySegmentId(thd, dc, tables->table, filters.segmentId,
                                 true) != CDE_OK) {
      CDE_LOG_WARN("Failed to DstoreProcessBySegmentId for segmentId %d,%u",
                   filters.segmentId.m_fileId, filters.segmentId.m_blockId);
    }
    return CDE_OK;
  }

  /* Case 2: Filtering by DB_NAME and TABLE_NAME */
  if (filters.hasDbName && filters.hasTableName) {
    cde_perf_counters::getInstance()->_dstore_indexes_pushdown_hit.increment();
    if (DstoreProcessByTableName(thd, dc, tables->table, filters, true) !=
        CDE_OK) {
      CDE_LOG_WARN("Failed to DstoreProcessByTableName for table %s,%s",
                   filters.dbName.c_str(), filters.tableName.c_str());
    }
    return CDE_OK;
  }

  /* Case 3: No filtering or partial filtering, iterate through all tables */
  if (DstoreProcessAllTables(thd, tables->table, filters, true) != CDE_OK) {
    CDE_LOG_WARN("Failed to DstoreProcessAllTables");
    /* return CDE_OK rather than returning CDE_ERROR to prevent service crashes
     */
    return CDE_OK;
  }
  return CDE_OK;
}

/**
Bind the dynamic table INFORMATION_SCHEMA.dstore_indexes

@param[in/out]      p   schema table to init

@return 0 on success
*/
static int DstoreIndexesInit(void *p) {
  if (!p) {
    return CDE_ERROR;
  }
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;

  schema->fields_info = g_dstoreIndexesFieldsInfo;
  schema->fill_table = DstoreIndexesFillTable;
  return CDE_OK;
}

struct st_mysql_plugin g_infomationSchemaDstoreIndexes = {
    /* the plugin type (a MYSQL_XXX_PLUGIN value) */
    /* int */
    MYSQL_INFORMATION_SCHEMA_PLUGIN,

    /* pointer to type-specific plugin descriptor */
    /* void* */
    &g_infomationSchemaInfo,

    /* plugin name */
    /* const char* */
    I_S_DSTORE_INDEXES_TABLE_NAME,

    /* plugin author (for SHOW PLUGINS) */
    /* const char* */
    PLUGIN_AUTHOR,

    /* general descriptive text (for SHOW PLUGINS) */
    /* const char* */
    "DStore indexes information",

    /* the plugin license (PLUGIN_LICENSE_XXX) */
    /* int */
    PLUGIN_LICENSE_GPL,

    /* the function to invoke when plugin is loaded */
    /* int (*)(void*); */
    DstoreIndexesInit,

    /* the function to invoke when plugin is un installed */
    /* int (*)(void*); */
    nullptr,

    /* the function to invoke when plugin is unloaded */
    /* int (*)(void*); */
    nullptr,

    /* plugin version (for SHOW PLUGINS) */
    /* unsigned int */
    1,

    /* SHOW_VAR* */
    nullptr,

    /* SYS_VAR** */
    nullptr,

    /* reserved for dependency checking */
    /* void* */
    nullptr,

    /* Plugin flags */
    /* unsigned long */
    0UL,
};

/** The tablespaces table field id enum, note that the order of this enum must
be the same as the array g_dstoreTableSpacesFieldsInfo define. */
enum DstoreTableSpacesField {
  SPACE_ID = 0,
  SPACE_NAME,
  MAX_SIZE,
  SPACE_TYPE,
  SPACE_FILES
};

/**  INNODB_TABLESPACES    ********************************************/
/* Fields of the dynamic table INFORMATION_SCHEMA.INNODB_TABLESPACES
Every time any column gets changed, added or removed, please remember
to change i_s_innodb_plugin_version_postfix accordingly, so that
the change can be propagated to server */
static ST_FIELD_INFO g_dstoreTableSpacesFieldsInfo[] = {
    {"SPACE", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, MY_I_S_UNSIGNED,
     "", 0},
    {"NAME", TRX_I_S_QUERY_STMT_MAX_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"MAX_SIZE", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"SPACE_TYPE", 10, MYSQL_TYPE_STRING, 0, MY_I_S_MAYBE_NULL, "", 0},
    {"FILES", ONLINE_DDL_I_S_PROCESS_QUERY_STMT_LEN, MYSQL_TYPE_STRING, 0, 0,
     "", 0},
    END_OF_ST_FIELD};

/** Function to populate INFORMATION_SCHEMA.DSTORE_TABLESPACES table.
Loop through each record in , and extract the column
information and fill the INFORMATION_SCHEMA.DSTORE_TABLESPACES table.
@param[in]      thd             thread
@param[in,out]  tables          tables to fill
@return 0 on success */
static int DstoreTableSpacesFillTable(THD *thd, Table_ref *tables, Item *) {
  DBUG_TRACE;

  /* deny access to user without PROCESS_ACL privilege */
  if (check_global_access(thd, PROCESS_ACL)) {
    return 0;
  }

  (void)CdeCreateOrGetSession(thd);

  Field **fields = tables->table->field;
  DSTORE::FileId *fileIds = nullptr;
  uint32_t fileCount = 0;

  auto saveToTable = [&](const char *spaceName, uint64_t maxSize,
                         DSTORE::TablespaceId spaceId) -> TableSpace * {
    const char *spaceType;
    if (spaceId ==
        (DSTORE::TablespaceId)DSTORE::TBS_ID::DEFAULT_TABLE_SPACE_ID) {
      spaceType = "General";
    } else if ((spaceId ==
                (DSTORE::TablespaceId)DSTORE::TBS_ID::TEMP_TABLE_SPACE_ID) ||
               (spaceId ==
                (DSTORE::TablespaceId)DSTORE::TBS_ID::GLOBAL_TABLE_SPACE_ID)) {
      spaceType = "System";
    } else {
      spaceType = "Single";
    }

    fields[SPACE_ID]->store(spaceId, true);
    FieldStoreString(fields[SPACE_NAME], spaceName);
    fields[MAX_SIZE]->store(maxSize, true);
    FieldStoreString(fields[SPACE_TYPE], spaceType);
    fileCount = 0;
    TableSpace_Interface::GetFileIdsByTablespaceId(
        DSTORE::g_defaultPdbId, spaceId, &fileIds, fileCount);
    /** temporary tablespace may not have files */
    if (fileCount == 0) {
      FieldStoreString(fields[SPACE_FILES], "");
    } else {
      std::string fileList =
          std::accumulate(fileIds + 1, fileIds + fileCount,
                          std::to_string(fileIds[0]), [](auto a, uint32_t b) {
                            return std::move(a) + "," + std::to_string(b);
                          });
      FieldStoreString(fields[SPACE_FILES], fileList.c_str());
    }
    schema_table_store_record(thd, tables->table);
    return nullptr;
  };
  LoadAllCreatedSpace(thd, saveToTable);

  return 0;
}

/** Bind the dynamic table INFORMATION_SCHEMA.DSTORE_TABLESPACES
@param[in,out]  p       table schema object
@return 0 on success */
static int DstoreTableSpacesInit(void *p) {
  ST_SCHEMA_TABLE *schema;

  DBUG_TRACE;

  schema = (ST_SCHEMA_TABLE *)p;

  schema->fields_info = g_dstoreTableSpacesFieldsInfo;
  schema->fill_table = DstoreTableSpacesFillTable;

  return 0;
}

struct st_mysql_plugin g_infomationSchemaDstoreTableSpace = {
    /* the plugin type (a MYSQL_XXX_PLUGIN value) */
    /* int */
    MYSQL_INFORMATION_SCHEMA_PLUGIN,

    /* pointer to type-specific plugin descriptor */
    /* void* */
    &g_infomationSchemaInfo,

    /* plugin name */
    /* const char* */
    I_S_DSTORE_TABLESPACE_NAME,

    /* plugin author (for SHOW PLUGINS) */
    /* const char* */
    PLUGIN_AUTHOR,

    /* general descriptive text (for SHOW PLUGINS) */
    /* const char* */
    "DStore DSTORE_TABLESPACES",

    /* the plugin license (PLUGIN_LICENSE_XXX) */
    /* int */
    PLUGIN_LICENSE_GPL,

    /* the function to invoke when plugin is loaded */
    /* int (*)(void*); */
    DstoreTableSpacesInit,

    /* the function to invoke when plugin is un installed */
    /* int (*)(void*); */
    nullptr,

    /* the function to invoke when plugin is unloaded */
    /* int (*)(void*); */
    nullptr,

    /* plugin version (for SHOW PLUGINS) */
    /* unsigned int */
    1,

    /* SHOW_VAR* */
    nullptr,

    /* SYS_VAR** */
    nullptr,

    /* reserved for dependency checking */
    /* void* */
    nullptr,

    /* Plugin flags */
    /* unsigned long */
    0UL,
};

/** The dstore_online_ddl_progress i_s table field id enum, note that the order
of this enum must be the same as the array g_dstoreOnlineDdlProgressFieldsInfo
define. */
enum DstoreOnlineDdlProgressField {
  ONLINE_DDL_THREAD_ID = 0,
  ONLINE_DDL_TABLE,
  ONLINE_DDL_INDEX,
  ONLINE_DDL_STATUS,
  REPLAYED_ROW_LOG_ITEMS,
  TOTAL_ROW_LOG_ITEMS,
};

static ST_FIELD_INFO g_dstoreOnlineDdlProgressFieldsInfo[] = {
    {"THREAD_ID", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_UNSIGNED, "", 0},
    {"TABLE_NAME", CDE_MAX_FULL_NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"INDEX_NAME", CDE_MAX_INDEX_NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "", 0},
    {"STATUS", ONLINE_DDL_I_S_PROCESS_STATUS_LEN, MYSQL_TYPE_STRING, 0, 0, "",
     0},
    {"REPLAYED_ROW_LOG_ITEMS", MY_UINT64_NUM_DECIMAL_DIGITS,
     MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
    {"TOTAL_ROW_LOG_ITEMS", MY_UINT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
     0, MY_I_S_UNSIGNED, "", 0},
    END_OF_ST_FIELD};

/**
Function to fill INFORMATION_SCHEMA.dstore_undo

@param[in]      thd
@param[in]      tables
@param[in]      item

@return 0 on success
*/
static int DstoreOnlineDdlProgressFillTable(THD *thd, Table_ref *tables,
                                            Item *item) {
  (void)item;
  [[maybe_unused]] Field **fields = tables->table->field;
  const char *tableName = tables->table_name;

  if (CdeStrcasecmp(tableName, I_S_DSTORE_ONLINE_DDL_PROGRESS_TABLE_NAME) !=
      0) {
    return CDE_ERROR;
  }

  /* deny access to non-superusers */
  if (check_global_access(thd, PROCESS_ACL)) {
    return CDE_OK;
  }

  RowLogProgressMgr *progressMgr = RowLogProgressMgr::GetInstance();
  CdeMutexGuard progressMutexGuard(progressMgr->GetProgressLock(),
                                   CDE_LOCATION_HERE);
  const Memory::Map<RowLog *, cde_dict_index_t *> *progressMap =
      RowLogProgressMgr::GetInstance()->GetProgressMap();
  for (const auto &progress : *progressMap) {
    RowLog *rowLog = progress.first;
    OnlineDdlMgr *onlineDdlMgr = rowLog->GetOnlineDdlMgr();
    cde_dict_index_t *dictIndex = progress.second;
    CHECK_STORE_RET(
        fields[ONLINE_DDL_THREAD_ID]->store(onlineDdlMgr->GetThreadId(), true));
    CHECK_STORE_RET(FieldStoreString(fields[ONLINE_DDL_TABLE],
                                     dictIndex->table->name.c_str()));
    CHECK_STORE_RET(
        FieldStoreString(fields[ONLINE_DDL_INDEX], dictIndex->name.c_str()));
    CHECK_STORE_RET(FieldStoreString(fields[ONLINE_DDL_STATUS],
                                     onlineDdlMgr->GetStatusString().c_str()));
    CHECK_STORE_RET(fields[REPLAYED_ROW_LOG_ITEMS]->store(
        rowLog->GetReplayedRowLogCount(), true));
    CHECK_STORE_RET(
        fields[TOTAL_ROW_LOG_ITEMS]->store(rowLog->GetRowLogCount(), true));
    CHECK_STORE_RET(schema_table_store_record(thd, tables->table));
  }
  return CDE_OK;
}

/**
Bind the dynamic table INFORMATION_SCHEMA.dstore_online_ddl_progress

@param[in/out]      p   schema table to init

@return 0 on success
*/
static int DstoreOnlineDdlProgressInit(void *p) {
  if (!p) {
    return CDE_ERROR;
  }
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;

  schema->fields_info = g_dstoreOnlineDdlProgressFieldsInfo;
  schema->fill_table = DstoreOnlineDdlProgressFillTable;
  return CDE_OK;
}

struct st_mysql_plugin g_infomationSchemaDstoreOnlineDdlProgress = {
    /* the plugin type (a MYSQL_XXX_PLUGIN value) */
    /* int */
    MYSQL_INFORMATION_SCHEMA_PLUGIN,

    /* pointer to type-specific plugin descriptor */
    /* void* */
    &g_infomationSchemaInfo,

    /* plugin name */
    /* const char* */
    I_S_DSTORE_ONLINE_DDL_PROGRESS_TABLE_NAME,

    /* plugin author (for SHOW PLUGINS) */
    /* const char* */
    PLUGIN_AUTHOR,

    /* general descriptive text (for SHOW PLUGINS) */
    /* const char* */
    "DStore online ddl progress",

    /* the plugin license (PLUGIN_LICENSE_XXX) */
    /* int */
    PLUGIN_LICENSE_GPL,

    /* the function to invoke when plugin is loaded */
    /* int (*)(void*); */
    DstoreOnlineDdlProgressInit,

    /* the function to invoke when plugin is un installed */
    /* int (*)(void*); */
    nullptr,

    /* the function to invoke when plugin is unloaded */
    /* int (*)(void*); */
    nullptr,

    /* plugin version (for SHOW PLUGINS) */
    /* unsigned int */
    1,

    /* SHOW_VAR* */
    nullptr,

    /* SYS_VAR** */
    nullptr,

    /* reserved for dependency checking */
    /* void* */
    nullptr,

    /* Plugin flags */
    /* unsigned long */
    0UL,
};

} /* namespace CDE */
