/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_dict_bg_stat.cc
 *
 *
 * IDENTIFICATION
 * src/cde_dict_bg_stat.cc
 *
 * -------------------------------------------------------------------------
 */
#define MYSQL_SERVER 1

#include <condition_variable>
#include <thread>

#include "debug_sync.h"
#include "lex_string.h"
#include "sql/sql_prepare.h"
#include "sql/sql_table.h"
#include "sql/sql_thd_internal_api.h"
#include "sql/strfunc.h"
#include "sql/thd_raii.h"

#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/dictionary.h"

#include "boot/cde_instance.h"
#include "ddl/cde_ddl.h"
#include "ddl/dd_helper.h"
#include "dict/cde_dict_bg_stat.h"
#include "dict/cde_dict_stat.h"

#ifdef HAVE_SYS_RESOURCE_H
/* include defs for CPU time priority settings */
#include <sys/resource.h>
#endif

namespace CDE {
/** Minimum time interval between stats recalc for a given table */
static constexpr std::chrono::seconds DSTORE_FAST_RECALC_INTERVAL{10};
static constexpr std::chrono::seconds DSTORE_SLOW_RECALC_INTERVAL{30};
static std::chrono::seconds dstore_auto_stats_recalc_interval();

class DelayRecalcElement {
 public:
  DelayRecalcElement(uint64_t tableId, std::string tableName,
                     std::chrono::steady_clock::time_point lastUpdateTime)
      : m_tableId(tableId),
        m_tableName(std::move(tableName)),
        m_lastUpdateTime(lastUpdateTime) {}

 public:
  uint64_t m_tableId;
  std::string m_tableName;
  std::chrono::steady_clock::time_point m_lastUpdateTime;
};

class cde_service {
 public:
  virtual int start() = 0;
  virtual int stop() = 0;
  virtual ~cde_service() {}
};

class bg_stat_service : public cde_service {
 public:
  bg_stat_service();
  virtual int start() override;
  virtual int stop() override;
  virtual ~bg_stat_service() {}
  void recalc_pool_add(uint64_t tableId, const std::string &table_name);
  void recalc_pool_del(uint64_t tableId);
  void AddElementToDelayRecalcPool(
      uint64_t tableId, const std::string &tableName,
      const std::chrono::steady_clock::time_point &lastUpdateTime);

 private:
  void bg_process();
  void get_table(uint64_t &tableId, std::string &tableName);
  bool process_table_stat(THD *thd);
  void ProcessUpdateHistogram(THD *thd, const std::string &dictTableName,
                              const std::vector<std::string> &indexColNames);
  int RecalcPoolReady();

  /* Protect recalc_pool access and change safe */
  std::mutex recalc_pool_mtx;
  /* During commit/rollback stage, changed tables were added into recalc_pool */
  std::vector<std::pair<uint64_t, std::string>> recalc_pool;
  /* Table change ratio satisfy 10% requirement, but less than
  DSTORE_FAST_RECALC_INTERVAL since last update statistics, add into this pool
  waiting for update time meets requirements. */
  std::vector<DelayRecalcElement> m_delayRecalcPool;

  std::thread worker;
  bool stop_flag{false};
  std::condition_variable m_dictStatsCV;

  /** The number of tables that can be added to "recalc_pool" before
  it is enlarged */
  static const unsigned long DSTORE_RECALC_POOL_INITIAL_SLOTS = 128;
};

bg_stat_service::bg_stat_service() {
  recalc_pool.reserve(DSTORE_RECALC_POOL_INITIAL_SLOTS);
}

int bg_stat_service::start() {
  CDE_LOG_INFO("bg stat service start");
  stop_flag = false;
  worker = std::thread(std::bind(&bg_stat_service::bg_process, this));
  return 0;
}

int bg_stat_service::stop() {
  CDE_LOG_INFO("bg stat service stop -- begin");
  stop_flag = true;

  if (worker.joinable()) {
    worker.join();
  }
  CDE_LOG_INFO("bg stat service stop -- end");
  return CDE_OK;
}

void bg_stat_service::recalc_pool_add(uint64_t tableId,
                                      const std::string &tableName) {
  if (tableName.empty()) {
    return;
  }
  std::unique_lock<std::mutex> lock(recalc_pool_mtx);

  for (const auto &table : recalc_pool) {
    if (table.first == tableId) {
      return;
    }
  }
  recalc_pool.emplace_back(tableId, tableName);
  m_dictStatsCV.notify_one();
}

void bg_stat_service::recalc_pool_del(uint64_t tableId) {
  std::unique_lock<std::mutex> lock(recalc_pool_mtx);
  if (auto it =
          std::find_if(recalc_pool.begin(), recalc_pool.end(),
                       [tableId](const auto &p) { return p.first == tableId; });
      it != recalc_pool.end()) {
    recalc_pool.erase(it);
  }
  /* Remove table from delay recalc pool, do not recalculate the table later */
  if (auto it = std::find_if(
          m_delayRecalcPool.begin(), m_delayRecalcPool.end(),
          [tableId](const auto &p) { return p.m_tableId == tableId; });
      it != m_delayRecalcPool.end()) {
    DBUG_PRINT("dstore_recalc_pool_del_print",
               ("Remove table from backend auto update delay recalculate pool, "
                "table name: %s",
                it->m_tableName.c_str()));
    m_delayRecalcPool.erase(it);
  }
}

void bg_stat_service::AddElementToDelayRecalcPool(
    uint64_t tableId, const std::string &tableName,
    const std::chrono::steady_clock::time_point &lastUpdateTime) {
  CDE_ASSERT_DEBUG(!tableName.empty());

  std::unique_lock<std::mutex> lock(recalc_pool_mtx);
  for (const auto &element : m_delayRecalcPool) {
    if (element.m_tableId == tableId) {
      return;
    }
  }
  m_delayRecalcPool.emplace_back(tableId, tableName, lastUpdateTime);
}

void bg_stat_service::get_table(uint64_t &tableId, std::string &tableName) {
  std::unique_lock<std::mutex> lock(recalc_pool_mtx);

  /* Always first get element from recalc_pool */
  if (!recalc_pool.empty()) {
    const auto iter = recalc_pool.begin();
    tableId = iter->first;
    tableName = iter->second;
    recalc_pool.erase(iter);
    return;
  }

  /* Get element from m_delayRecalcPool */
  if (!m_delayRecalcPool.empty()) {
    const auto iter = m_delayRecalcPool.begin();
    if (std::chrono::steady_clock::now() - iter->m_lastUpdateTime >=
        DSTORE_FAST_RECALC_INTERVAL) {
      tableId = iter->m_tableId;
      tableName = iter->m_tableName;
      m_delayRecalcPool.erase(iter);
      return;
    }
  }
}

/**
Concatenate the SQL statement for updating the histogram.
And then run the SQL statement.

@param[in]      thd            handle to the user thread.
@param[in]      dictTableName  table name from cde_dict_t.
@param[in]      indexColNames  Column names of indexes of table.

@return void.
*/
void bg_stat_service::ProcessUpdateHistogram(
    THD *thd, const std::string &dictTableName,
    const std::vector<std::string> &indexColNames) {
  if (indexColNames.empty()) {
    return;
  }

  char fileTableName[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char fileDbName[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  CdeCreateTableInfo::SplitNormalizedName(dictTableName.c_str(), fileTableName,
                                          fileDbName);

  char tableName[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char dbName[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  filename_to_tablename(fileDbName, dbName, sizeof(dbName));
  CDE_ASSERT_DEBUG(false == IsPartition(fileTableName));
  filename_to_tablename(fileTableName, tableName, sizeof(tableName));

  std::stringstream ssColList;
  bool firstFlag = true;
  for (const auto &col_name : indexColNames) {
    if (firstFlag) {
      firstFlag = false;
    } else {
      ssColList << ",";
    }
    ssColList << EscapeStr(col_name);
  }
  std::string dbNameS(dbName);
  std::string tableNameS(tableName);

  std::string sQuery;
  sQuery = "ANALYZE TABLE " + EscapeStr(dbNameS) + "." + EscapeStr(tableNameS) +
           " UPDATE HISTOGRAM ON " + ssColList.str();

  LEX_STRING str;
  str.length = sQuery.length();
  str.str = sQuery.data();
  Ed_connection bg_con(thd);
  // Turn off binlogging to prevent written to the binary log.
  Disable_binlog_guard disable_binlog(thd);
  Disable_sql_log_bin_guard disable_sql_log_bin(thd);
  if (bg_con.execute_direct(str)) {
    CDE_LOG_ERROR("ProcessUpdateHistogram execute_direct error : %s",
                  sQuery.c_str());
  }
}

int bg_stat_service::RecalcPoolReady() {
  /* TODO consider performance, need to remove this lock? */
  std::unique_lock<std::mutex> lock(recalc_pool_mtx);

  /* If recalc_pool include elements, recalc immediately */
  if (!recalc_pool.empty()) {
    return CDE_OK;
  }

  /* If m_delayRecalcPool include elements and first element more than
  DSTORE_FAST_RECALC_INTERVAL since last update */
  if (!m_delayRecalcPool.empty()) {
    const auto it = m_delayRecalcPool.begin();
    if (std::chrono::steady_clock::now() - it->m_lastUpdateTime >=
        DSTORE_FAST_RECALC_INTERVAL) {
      return CDE_OK;
    }
  }

  return CDE_ERROR;
}

bool IsNeedStatsUpdateByDict(cde_dict_t *table, bg_stat_service *service) {
  /* For a system with lots of small tables, update histogram every time
  if data changed more than 10%, this could become hot. Refer to InnoDB,
  only when the time interval exceed auto_stats_recalc_interval will trigger
  automatic update logic. */
  if (std::chrono::steady_clock::now() - table->stats_last_recalc <
      dstore_auto_stats_recalc_interval()) {
    /* Stats were (re)calculated not long ago. To avoid
    too frequent stats updates we put the table into delay
    recalc pool and skip this loop. */
    service->AddElementToDelayRecalcPool(table->m_id, table->name,
                                         table->stats_last_recalc);
    return false;
  }

  /* reset table modify counter */
  table->stat_modified_counter = 0;
  return true;
}

bool bg_stat_service::process_table_stat(THD *thd) {
  std::string table_name;
  uint64_t tableId;

  get_table(tableId, table_name);

  if (table_name.empty()) {
    return false;
  }
  dd::String_type tableNameToLock(table_name.c_str());

  if (IsPartition(table_name)) {
    std::string schema;
    std::string tablename;
    std::string partition;
    bool isTmp;

    ParseTableName(table_name, false, schema, tablename, partition, isTmp);
    CDE_ASSERT_DEBUG(false == schema.empty());
    CDE_ASSERT_DEBUG(false == tablename.empty());
    CDE_ASSERT_DEBUG(false == partition.empty());

    tableNameToLock = (schema + '/' + tablename).c_str();
  }

  DBUG_EXECUTE_IF("bg_stat_after_get_table_name_wait", {
    const char act[] =
        "bg_stat_after_get_table_name WAIT_FOR resume_bg_stat_sig";
    CDE_ASSERT(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });
  DEBUG_SYNC_C("bg_stat_after_get_table_name");

  // try get Shared MDL lock
  ExplicitMDLGuard mdl_guard(thd, tableNameToLock.c_str());
  if (!mdl_guard.IsLocked()) {
    return false;
  }

  // open dict table to process
  cde_dict_t *table = DictSysGetTable(table_name.c_str(), true);
  if (table == nullptr) {
    return false;
  }
  if (table->m_id != tableId) {
    table->ReleaseByBgThread();
    return false;
  }

  bool isNeedStatsUpdate = IsNeedStatsUpdateByDict(table, this);

  if (!isNeedStatsUpdate) {
    table->ReleaseByBgThread();
    return false;
  }

  DBUG_EXECUTE_IF("bg_stat_after_get_table_wait", {
    const char act[] = "bg_stat_after_get_table WAIT_FOR resume_bg_stat_sig";
    CDE_ASSERT(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });
  DEBUG_SYNC_C("bg_stat_after_get_table");

  stat_update_option option =
      table->dict_stats_persist_enabled() ? RECALC_PERSIST : RECALC_STATS;
  (void)CdeDictTableStatsUpdate(table, option, thd);
  CDE_LOG_INFO("process one table - %s", table_name.c_str());

  std::vector<std::string> indexColNames;
  table->getIndexColNames(indexColNames);

  table->stats_last_recalc = std::chrono::steady_clock::now();
  table->ReleaseByBgThread();
  table = nullptr;

  /* We should release the mdl lock before update histogram SQL is executed,
  because the ddl SQL may be blocked by the rds_dstore_ddl_max_concurrency.
  If the mdl share read lock is hold here, it may results the deadlock. */
  mdl_guard.Unlock();

  if (dstore_use_histogram_auto_update) {
    /* ProcessUpdateHistogram will acquire MDL lock while other threads may have
    owned MDL lock and wait cde_dict_t to be released. If current thread not
    release cde_dict_t before ProcessUpdateHistogram, it will cause deadlock. */
    ProcessUpdateHistogram(thd, tableNameToLock.c_str(), indexColNames);
  }
  return true;
}

void bg_stat_service::bg_process() {
  CDE_LOG_INFO("begin bg stat service");
  my_thread_init();
  cde_session_t *cde_sess = new cde_session_t();
  CDE_ASSERT(cde_sess != nullptr);
  CdeConstructSession(cde_sess);

  THD *bg_thd = create_internal_thd();
  bg_thd->set_new_thread_id();
  // set sql_mode to 0 in system background update histogram
  bg_thd->variables.sql_mode = 0;

  // dstore will set thread name, here we reset
  pthread_setname_np(pthread_self(), "cde_bg_stat");

  while (!stop_flag) {
    std::mutex dictStatsMutex;
    std::unique_lock<std::mutex> lock(dictStatsMutex);
    m_dictStatsCV.wait_for(lock, DSTORE_FAST_RECALC_INTERVAL,
                           [&] { return this->RecalcPoolReady() == CDE_OK; });
    (void)process_table_stat(bg_thd);
  }

  CdeDestorySession(cde_sess);
  destroy_internal_thd(bg_thd);
  my_thread_end();
  CDE_LOG_INFO("quit bg stat service");
}

/**
  Return adaptive recalc interval for background Dstore table stats updates.

  The value is used by Dstore's background stats thread to decide:
  - the minimum time gap between two auto recalculations of the same table,

  The implementation samples mysqld process CPU usage and maps it to either
  a fast interval (for low CPU load) or a slow interval (for high CPU load),
  with smoothing and hysteresis to avoid frequent flips.

  @return Adaptive stats recalc interval.
*/
static std::chrono::seconds dstore_auto_stats_recalc_interval() {
  std::chrono::seconds fast_interval = DSTORE_FAST_RECALC_INTERVAL;
  std::chrono::seconds slow_interval = DSTORE_SLOW_RECALC_INTERVAL;
  constexpr std::chrono::seconds refresh_interval{1};
  constexpr double cpu_load_threshold_pct = 50.0;
  DBUG_EXECUTE_IF("dstore_auto_stats_interval_test_short", {
    fast_interval = std::chrono::seconds{1};
    slow_interval = std::chrono::seconds{3};
  };);

  // This helper is currently used by a single background stats thread.
  // Static state keeps the implementation lightweight without extra locks.
  // The state stores:
  // - last refresh timestamp (to cap sampling frequency),
  // - CPU usage sampling points (for delta-based CPU utilization),
  // - current interval mode.
  struct State {
    bool use_slow_interval{false};
    std::chrono::seconds cached_interval{};
    std::chrono::steady_clock::time_point last_refresh{};
    std::chrono::steady_clock::time_point last_cpu_sample{};
    double last_cpu_total_us{0.0};
    bool has_cpu_sample{false};
#ifndef NDEBUG
    bool owner_thread_initialized{false};
    my_thread_t owner_thread{};
#endif
  };
  static State state;
  state.cached_interval = fast_interval;

#ifndef NDEBUG
  // Debug safety check: this function is expected to be called by a single
  // thread. If another thread enters, fail fast in debug builds.
  if (!state.owner_thread_initialized) {
    state.owner_thread = my_thread_self();
    state.owner_thread_initialized = true;
  } else {
    assert(my_thread_equal(state.owner_thread, my_thread_self()));
  }
#endif

  const auto now = std::chrono::steady_clock::now();
  // Sampling is intentionally rate-limited to once per second. Between two
  // refresh points we return the last computed interval.
  if (state.last_refresh != std::chrono::steady_clock::time_point{} &&
      now - state.last_refresh < refresh_interval) {
    return state.cached_interval;
  }
  state.last_refresh = now;

  rusage usage;
  // On sampling failure, choose a conservative interval to avoid creating
  // extra work under uncertain conditions.
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    state.use_slow_interval = true;
    state.cached_interval = slow_interval;
    return state.cached_interval;
  }

  const double cpu_total_us =
      static_cast<double>(usage.ru_utime.tv_sec) * 1000000.0 +
      static_cast<double>(usage.ru_utime.tv_usec) +
      static_cast<double>(usage.ru_stime.tv_sec) * 1000000.0 +
      static_cast<double>(usage.ru_stime.tv_usec);

  if (!state.has_cpu_sample) {
    // First sample initializes the baseline and keeps conservative behavior
    // until we have enough data to compute a delta.
    state.last_cpu_total_us = cpu_total_us;
    state.last_cpu_sample = now;
    state.has_cpu_sample = true;
    state.use_slow_interval = false;
    state.cached_interval = fast_interval;
    return state.cached_interval;
  }

  const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              now - state.last_cpu_sample)
                              .count();
  if (elapsed_us <= 0) {
    return state.cached_interval;
  }

  const double cpu_delta_us =
      std::max(0.0, cpu_total_us - state.last_cpu_total_us);
  state.last_cpu_total_us = cpu_total_us;
  state.last_cpu_sample = now;

  unsigned int cpu_count = 1;
  // Prefer process affinity CPU count so the normalized percentage reflects
  // the CPU set available to this mysqld process.
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  if (sched_getaffinity(0, sizeof(cpu_set), &cpu_set) == 0) {
    const int affinity_cpus = CPU_COUNT(&cpu_set);
    if (affinity_cpus > 0) {
      cpu_count = static_cast<unsigned int>(affinity_cpus);
    }
  } else {
    // Fallback for non-Linux or affinity query failures.
    const auto hw_cpus = std::thread::hardware_concurrency();
    if (hw_cpus > 0) cpu_count = hw_cpus;
  }

  double cpu_usage_pct = cpu_delta_us * 100.0 /
                         static_cast<double>(elapsed_us) /
                         static_cast<double>(cpu_count);
  cpu_usage_pct = std::clamp(cpu_usage_pct, 0.0, 100.0);
  DBUG_EXECUTE_IF("dstore_auto_stats_interval_force_high_cpu",
                  cpu_usage_pct = 100.0;);
  DBUG_EXECUTE_IF("dstore_auto_stats_interval_force_low_cpu",
                  cpu_usage_pct = 0.0;);

  // high load (>= threshold) -> slow interval, otherwise fast interval.
  state.use_slow_interval = (cpu_usage_pct >= cpu_load_threshold_pct);

  // Low CPU load -> faster stats refresh cadence.
  // High CPU load -> slower cadence to reduce background CPU pressure.
  state.cached_interval =
      state.use_slow_interval ? slow_interval : fast_interval;
  return state.cached_interval;
}

static bg_stat_service *bg_stat_srv = nullptr;

void CdeAddStatTable(const TrxTable &trxTable, bool ignoreDataChanged) {
  CDE_ASSERT(bg_stat_srv != nullptr);
  if (ignoreDataChanged) {
    bg_stat_srv->recalc_pool_add(trxTable.m_tableId, trxTable.m_name);
    return;
  }

  uint64_t counter = trxTable.m_statModifiedCounter;
  uint64_t rows = trxTable.m_statNRows;

  /* Only update statistics if table has been changed more than 10%. */
  if ((counter > 0 && (counter - 1) > (rows / 10) &&
       trxTable.m_autoRecalcEnabled)) {
    bg_stat_srv->recalc_pool_add(trxTable.m_tableId, trxTable.m_name);
  }
}

// remove stat table when table is deleted
void CdeRemoveStatTable(uint64_t tableId) {
  /** It is possible that eviction thread is still
  running and tries to remove table from stat thread.
  However, the stat thread no longer exists. This is OK
  situation. */
  if (nullptr != bg_stat_srv) bg_stat_srv->recalc_pool_del(tableId);
}

int CdeBgStatSrvInit() {
  bg_stat_srv = new bg_stat_service();
  CDE_ASSERT(bg_stat_srv != nullptr);
  bg_stat_srv->start();
  return 0;
}

int CdeBgStatSrvDeinit() {
  if (bg_stat_srv) {
    bg_stat_srv->stop();
    delete bg_stat_srv;
    bg_stat_srv = nullptr;
  }

  return 0;
}
} /* namespace CDE */
