/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_dict.cc
 *
 *
 * IDENTIFICATION
 * src/cde_dict.cc
 *
 * -------------------------------------------------------------------------
 */
// clang-format off
#include <string>
#include <thread>
#include "debug_sync.h"

#include "mysql/thread_type.h"
#include "sql/dd/dictionary.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/types/table.h"
#include "sql/table.h"
#include "sql/dd_table_share.h"
#include "sql/sql_class.h"
#include "sql/sql_base.h"
#include "sql/sql_table.h"

#include "dict/cde_dict.h"
#include "dict/cde_dict_bg_stat.h"
#include "ddl/cde_dd_table.h"
#include "ddl/cde_ddl.h"
#include "ddl/cde_ddl_log.h"
#include "ddl/cde_mysql_ops.h"
#include "ddl/ha_cde_alter.h"
#include "ddl/dd_helper.h"
#include "perfcounters.h"
#include "common/cde_alloc.h"
#include "common/cde_perf.h"
#include "common/cde_compare_utils.h"
#include "common/cde_relation_cache.h"
#include "common/cde_session.h"

#include "page/dstore_page_struct.h"
#include "table/dstore_table_interface.h"
#include "heap/dstore_heap_struct.h"
#include "systable/dstore_relation.h"
// clang-format on

namespace CDE {

DictSys *g_dictSys = nullptr;

typedef boost::intrusive::member_hook<cde_dict_t,
                                      boost::intrusive::list_member_hook<>,
                                      &cde_dict_t::m_LRUListHook>
    LRUListHookOption;
typedef boost::intrusive::list<cde_dict_t, LRUListHookOption> CdeDictLRU;

/** LRU list of cde_dict_t. Holds cde_dict_ts that are evictable. MRU at the
 * begging of the list. LRU at the end of the list.*/
CdeDictLRU gTableLRU;
ulong g_dstoreTableDefinitionCache = 400;

bool dstore_stats_auto_recalc = true;
bool dstore_stats_persistent = true;
/**
The value of dstore_stats_persistent_sample_pages needs to be adjusted when the
dstore supports the statistics persistence feature. The value of 20 is too
small.
*/
unsigned long long dstore_stats_persistent_sample_pages = 100;
unsigned long long dstore_stats_transient_sample_pages = 100;
unsigned long long dstore_stats_persistent_index_sample_pages = 20;
unsigned long long dstore_stats_transient_index_sample_pages = 20;

// stat bg thread sleep interval in microsecond
constexpr uint32_t DICT_BG_THREAD_SLEEP_INTERVAL = 200;

void DictSysInit(void) {
  g_dictSys = new DictSys();
  CDE_ASSERT_DEBUG(g_dictSys != nullptr);
}
void DictSysDestroy(void) {
  gTableLRU.clear();
  if (g_dictSys != nullptr) {
    delete g_dictSys;
    g_dictSys = nullptr;
  }
}

DictSysLockGuard::DictSysLockGuard()
    : m_guard(&g_dictSys->m_mutex, CDE_LOCATION_HERE) {}

OperationalLockGuard::OperationalLockGuard(bool lockNow)
    : m_guard(&g_dictSys->m_dictOperationalMtx, CDE_LOCATION_HERE, lockNow) {}

void OperationalLockGuard::Clear() { m_guard.Clear(); }

/**
Removes table from gTableLRU (eviction LRU list), given table is on
this list. Complexity is O(1). Caller needs to hold lock on dict sys.

@param[in]  table   table to be removed from eviction list.
                    If table is not on the list - do nothing.
*/
void DictRemoveFromEvictLRULocked(cde_dict_t *table) {
  if (false == table->m_LRUListHook.is_linked()) return;

  CDE_ASSERT_DEBUG(CdeDictLRU::s_iterator_to(*table) != gTableLRU.end());
  gTableLRU.erase(CdeDictLRU::s_iterator_to(*table));
}

void DictRemoveFromEvictLRU(cde_dict_t *table) {
  DictSysLockGuard dictSysLockGuard;
  DictRemoveFromEvictLRULocked(table);
}

/**
Moves table to the beginning of gTableLRU (eviction LRU list) -
thus making it the MRU table in the list. Complexity is O(1).
The caller must hold lock on dict sys.

@param[in]  table   table to be made MRU in eviction list.
                    If table is not on the list - do nothing.
*/
static inline void DictMoveToMRU(cde_dict_t *table) {
  if (false == table->m_LRUListHook.is_linked()) {
    return;
  }

  DictRemoveFromEvictLRULocked(table);
  gTableLRU.push_front(*table);
}

void DictSysFreeSpaceInCache() {
  ulong numberOfTables = gTableLRU.size();
  std::vector<cde_dict_t *> toRemove;
  ulong numberOfEvicted{0};
  OperationalLockGuard writeLockGuard;
  DictSysLockGuard dictSysLockGuard;

  const ulong percentageCheck{50};
  ulong maxNumberOfChecks = (numberOfTables * percentageCheck) / 100;
  ulong numberOfChecks = 0;

  for (auto tableIt = gTableLRU.rbegin();
       tableIt != gTableLRU.rend() &&
       (numberOfTables - numberOfEvicted) > g_dstoreTableDefinitionCache &&
       numberOfChecks < maxNumberOfChecks;
       ++tableIt, ++numberOfChecks) {
    if (tableIt->getRefCnt() > 0 || tableIt->m_discardAfterDDL) continue;

    ++numberOfEvicted;
    toRemove.push_back(&(*tableIt));
  }

  const std::chrono::seconds maxRemoveLoopDuration{5};
  auto start = std::chrono::steady_clock::now();

  DBUG_EXECUTE_IF("break_too_long_loop",
                  toRemove.push_back(&(*gTableLRU.rbegin()));
                  CDE_ASSERT_DEBUG(toRemove.size() >= 1);
                  start -= 2 * maxRemoveLoopDuration;);

  for (auto tablePtr : toRemove) {
    if (std::chrono::steady_clock::now() - start >= maxRemoveLoopDuration)
      break;
    DictSysRemoveTableLocked(tablePtr->name.c_str(), dictSysLockGuard, false);
  }
  DBUG_EXECUTE_IF("break_too_long_loop_wait", {
    const char act[] =
        "break_too_long_loop_after SIGNAL break_tool_long_loop_finished "
        "WAIT_FOR break_too_long_loop_test_ready";
    CDE_ASSERT(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });
  DEBUG_SYNC_C("break_too_long_loop_after");
}

cde_dict_t *DictSysGetTable(const char *tableName, bool getByBgStat) {
  CDE_ASSERT_DEBUG(g_dictSys != nullptr);
  return g_dictSys->GetTableEntry(tableName, getByBgStat);
}

cde_dict_t *DictSysGetTableById(uint64_t tableId) {
  return g_dictSys->GetTableEntryById(tableId);
}

cde_dict_t *DictSysGetTableLocked(const char *tableName, bool getByBgStat) {
  CDE_ASSERT_DEBUG(g_dictSys != nullptr);
  return g_dictSys->GetTableEntryLocked(tableName, getByBgStat);
}

void DictSysAddTable(const char *tableName, cde_dict_t *table,
                     cde_dict_t **prtExist, bool allowEviction) {
  CDE_ASSERT_DEBUG(g_dictSys != nullptr);
  g_dictSys->AddTableEntry(tableName, table, prtExist, allowEviction);
}

void DictSysRemoveTableLocked(const char *tableName,
                              DictSysLockGuard &lockGuard,
                              bool releaseDictSys) {
  CDE_ASSERT_DEBUG(g_dictSys != nullptr);
  g_dictSys->RemoveTableEntryLocked(tableName, lockGuard, releaseDictSys);
}

void DictSysRemoveTable(const char *tableName) {
  CDE_ASSERT_DEBUG(g_dictSys != nullptr);
  g_dictSys->RemoveTableEntry(tableName);
}

void DictSysRemoveTableNoOpLock(const char *tableName) {
  CDE_ASSERT_DEBUG(g_dictSys != nullptr);
  g_dictSys->RemoveTableEntryNoOpLock(tableName);
}

int32_t DictSysRenameTable(const char *from, const char *to,
                           bool renameForeigns) {
  CDE_ASSERT_DEBUG(g_dictSys != nullptr);
  return g_dictSys->RenameTableEntry(from, to, renameForeigns);
}

/**
Removes a foreign constraint struct from the dictionary cache.

@param[in]      foreign  foreign constraint
*/
void DictForeignRemoveFromCache(cde_dict_foreign_t *foreign) {
  OperationalLockGuard opGuard;
  DictSysLockGuard lockGuard;
  CDE_ASSERT(foreign);

  if (foreign->referenced_table != nullptr) {
    foreign->referenced_table->referenced_dict_set.erase(foreign);
  }

  if (foreign->foreign_table != nullptr) {
    foreign->foreign_table->foreign_dict_set.erase(foreign);
    foreign->foreign_table->refresh_fk = true;
  }

  delete foreign;
}

DictSys::~DictSys() {
  for (auto &tableEntry : m_tables) {
    DestroyTable(tableEntry.second);
  }
  m_tables.clear();
  m_tablesId.clear();

  MutexDestroy(&m_mutex);
  MutexDestroy(&m_dictOperationalMtx);
}

cde_dict_t *DictSys::GetTableEntryLocked(const char *tableName,
                                         bool getByBgStat) {
  MutexAssertOwner(&g_dictSys->m_mutex);

  auto it = m_tables.find(tableName);
  if (it == m_tables.end()) return nullptr;

  cde_dict_t *tbl_entry = it->second;

  if (getByBgStat) {
    if (tbl_entry->MarkHoldByBgThread() == CDE_FAIL) {
      return nullptr;
    }
  }
  DictMoveToMRU(tbl_entry);

  tbl_entry->acquire();
  CDE_ASSERT_DEBUG(tbl_entry->getRefCnt() > 0);

  return tbl_entry;
}

cde_dict_t *DictSys::GetTableEntryLockedById(uint64_t tableId) {
  MutexAssertOwner(&g_dictSys->m_mutex);

  auto it = m_tablesId.find(tableId);
  if (it == m_tablesId.end()) return nullptr;

  cde_dict_t *tblEntry = it->second;

  DictMoveToMRU(tblEntry);

  tblEntry->acquire();
  CDE_ASSERT_DEBUG(tblEntry->getRefCnt() > 0);

  return tblEntry;
}

cde_dict_t *DictSys::GetTableEntry(const char *tableName, bool getByBgStat) {
  DictSysLockGuard lockGuard;
  return GetTableEntryLocked(tableName, getByBgStat);
}

cde_dict_t *DictSys::GetTableEntryById(uint64_t tableId) {
  DictSysLockGuard lockGuard;
  return GetTableEntryLockedById(tableId);
}

void DictSys::AddTableEntry(const char *tableName, cde_dict_t *table,
                            cde_dict_t **ptrExist, bool allowEviction) {
  CDE_ASSERT_DEBUG(tableName != nullptr);
  CDE_ASSERT_DEBUG(table != nullptr);

  std::size_t tblSize = 0;
  {
    // todo:handle duplicate table name with different id later
    uint64_t tableId = table->m_id;
    DictSysLockGuard lockGuard;
    auto nameMapIter = m_tables.find(tableName);
    auto idMapIter = m_tablesId.find(tableId);

    if (nameMapIter != m_tables.end()) {
      CDE_ASSERT(idMapIter != m_tablesId.end());
      if (ptrExist != nullptr) {
        cde_dict_t *tableEntry = nameMapIter->second;
        tableEntry->acquire();
        *ptrExist = tableEntry;
      }
      return;
    }

    DEBUG_SYNC_C("AddTableEntryWait");
    CDE_ASSERT(nameMapIter == m_tables.end() && idMapIter == m_tablesId.end());

    m_tables[tableName] = table;
    m_tablesId[tableId] = table;
    tblSize = m_tables.size();
    CDE_ASSERT(tblSize == m_tablesId.size());
    if (allowEviction) gTableLRU.push_front(*table);
  }

  cde_perf_counters::getInstance()->_dict_size.setValue(tblSize);
}

const char *GetDBName(const char *name, uint32_t &dbNameSize) {
  const char *slash1 = strchr(name, '/');
  if (slash1 == nullptr || slash1[1] == '\0') {
    dbNameSize = 0;
    return nullptr;
  }
  const char *slash2 = strchr(slash1 + 1, '/');
  if (slash2 == nullptr) {
    dbNameSize = 0;
    return nullptr;
  }
  dbNameSize = slash2 - slash1 - 1;
  return slash1 + 1;
}

const char *GetTableName(const char *name, uint32_t &tableNameSize) {
  const char *slash1 = strchr(name, '/');
  if (slash1 == nullptr || slash1[1] == '\0') {
    tableNameSize = 0;
    return nullptr;
  }
  const char *slash2 = strchr(slash1 + 1, '/');
  if (slash2 == nullptr || slash2[1] == '\0') {
    tableNameSize = 0;
    return nullptr;
  }

  tableNameSize = strlen(slash2 + 1);
  return slash2 + 1;
}

bool GetDbAndTableNameFromDictName(const std::string &dictName,
                                   std::string &dbNameRes,
                                   std::string &tableNameRes) {
  if (CdeIsMysqlTmpTableName(dictName.c_str())) {
    /* Parsing of temporary table's name is not support for this function. */
    CDE_LOG_ERROR("Parsing of temporary table's name: %s is not support.",
                  dictName.c_str());
    return CDE_FAIL;
  }

  char fileTableName[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char fileDbName[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  if (CdeCreateTableInfo::SplitNormalizedName(dictName.c_str(), fileTableName,
                                              fileDbName) != CDE_OK) {
    CDE_LOG_ERROR("Split normalized name:%s fail.", dictName.c_str());
    return CDE_FAIL;
  }

  if (IsPartition(fileTableName)) {
    std::string schema, partition, tableNameNoPart;
    bool isTmp;
    ParseTableName(fileTableName, false, schema, tableNameNoPart, partition,
                   isTmp);
    CDE_ASSERT_DEBUG(tableNameNoPart.length() <= strlen(fileTableName));
    strcpy_s(fileTableName, CDE_MAX_TABLE_NAME_LEN + 1,
             tableNameNoPart.c_str());
  }

  char tableName[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char dbName[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  filename_to_tablename(fileDbName, dbName, sizeof(dbName));
  filename_to_tablename(fileTableName, tableName, sizeof(tableName));
  dbNameRes = dbName;
  tableNameRes = tableName;
  return CDE_SUCC;
}

uint32_t getDBNameSizeFkStyle(const char *name) {
  const char *s;
  s = strchr(name, '/');
  if (s == nullptr) {
    return (0);
  }
  return (s - name);
}

int32_t DictSys::RenameTableEntry(const char *from, const char *to,
                                  bool renameForeigns) {
  CDE_ASSERT_DEBUG(from != nullptr);
  CDE_ASSERT_DEBUG(to != nullptr);

  DictSysLockGuard lockGuard;

  auto itFrom = m_tables.find(from);

  if (itFrom == m_tables.end()) {
    CDE_LOG_WARN("Rename table entry fail from %s to %s due to %s not exits.",
                 from, to, from);
    return HA_ERR_NO_SUCH_TABLE;
  }
  if (m_tables.find(to) != m_tables.end()) {
    CDE_LOG_WARN("Rename table entry fail from %s to %s due to %s exits.", from,
                 to, to);
    return HA_ERR_GENERIC;
  }
  DBUG_EXECUTE_IF("rename_table_entry_fail", return HA_ERR_GENERIC;);

  /** FkStyle - foreign key style means that names starts without "./""
   * for instance foreign key name might be test/t1_fk_1 for table
   * ./test/t1. I am using this naming to differentiate from table
   * names which starts with "./". For instance ./test/t1.
   * For table "./test/t1" - fkStyle name means "test/t1" */
  char oldNameFkStyle[FN_REFLEN * 2];
  const char *oldNameStartInFrom = strchr(from, '/') + 1;
  CDE_ASSERT_DEBUG(oldNameStartInFrom >= from);
  char newNameFkStyle[FN_REFLEN * 2];
  const char *newNameStartInTo = strchr(to, '/') + 1;
  CDE_ASSERT_DEBUG(newNameStartInTo >= to);
  if (0 != strcpy_s(oldNameFkStyle, sizeof(oldNameFkStyle),
                    oldNameStartInFrom) ||
      0 != strcpy_s(newNameFkStyle, sizeof(newNameFkStyle), newNameStartInTo)) {
    CDE_LOG_WARN(
        "Rename table entry fail from %s to %s due to strcpy_s failure.", from,
        to);
    return HA_ERR_GENERIC;
  }
  cde_dict_t *table = itFrom->second;
  table->name.assign(to);
  DEBUG_SYNC_C("RenameTableEntryWait");

  m_tables[to] = table;
  m_tables.erase(from);

  if (false == renameForeigns) {
    /* In ALTER TABLE we think of the rename table operation
      in the direction table -> temporary table (#sql...)
      as dropping the table with the old name and creating
      a new with the new name. Thus we kind of drop the
      constraints from the dictionary cache here. The foreign key
      constraints will be inherited to the new table from the
      system tables through a call of dict_load_foreigns.
      Thus instead of renaming foreigns - we just drop them */
    /* Remove the foreign constraints from the cache */
    for (cde_dict_foreign_t *foreign : table->foreign_dict_set) {
      cde_dict_t *referecedTable = foreign->referenced_table;
      if (referecedTable != nullptr) {
        referecedTable->referenced_dict_set.erase(foreign);
      }
      delete foreign;
    }
    table->foreign_dict_set.clear();

    for (cde_dict_foreign_t *referenced_foreign : table->referenced_dict_set) {
      referenced_foreign->referenced_table = nullptr;
      referenced_foreign->referenced_index = nullptr;
    }
    table->referenced_dict_set.clear();

    return CDE_OK;
  }

  char normName[FN_REFLEN * 2];
  CDE_ASSERT(strchr(to, '/') + 1 < to + strlen(to));
  CDE_ASSERT(strchr(from, '/') + 1 < from + strlen(from));

  /** We should not change fk unique names for temp tables */
  CDE_ASSERT(nullptr == strstr(from, "#sql"));
  CDE_ASSERT(nullptr == strstr(to, "#sql"));

  size_t newTableNameLen = strlen(to);
  const char *fkMark{"_fk_"};

  // We will be changing unique names of foreign keys. Those are the values
  // that foreign_dict_set and referenced_dict_set are sorted by.
  // That's why we use here iterators to iterate over foreign_dict_set
  // and reinsert to referenced_dict_set.
  cde_dict_foreign_set fkWithChangedNames;
  int strCpyRet [[maybe_unused]] =
      0; /** Here strcpy should always success, as the max length of the table
          * should be guarded by the server layer. So just CDE_DEBUG_ASSERT for
          * those returns to keep codecheck happy.
          */
  for (auto it = table->foreign_dict_set.begin();
       it != table->foreign_dict_set.end();) {
    cde_dict_foreign_t *foreign = *it;

    it = table->foreign_dict_set.erase(it);

    if (foreign->referenced_table) {
      foreign->referenced_table->referenced_dict_set.erase(foreign);
    }

    CDE_ASSERT_DEBUG(0 == strcmp(foreign->foreign_table_name, from));
    if (strlen(foreign->foreign_table_name) < newTableNameLen)
      foreign->foreign_table_name = safe_strdup_root(&foreign->m_mem_root, to);
    else {
      strCpyRet = strcpy_s(foreign->foreign_table_name,
                           strlen(foreign->foreign_table_name) + 1, to);
      CDE_ASSERT_DEBUG(0 == strCpyRet);
    }

    /** check if unique_name fallows pattern ./db_name./table_name_fk_. If it
     * does we assume this is a genereate name and we should change both db_name
     * (given database has changed) and table_name parts when renaming table.
     * If it does not - it means this is a name given by user and should not
     * be change - we only change the db_name if table's database changed.
     */
    if (strlen(foreign->unique_name) >
            (strlen(oldNameFkStyle) + strlen(fkMark)) &&
        !memcmp(foreign->unique_name, oldNameFkStyle, strlen(oldNameFkStyle)) &&
        !memcmp(foreign->unique_name + strlen(oldNameFkStyle), fkMark,
                strlen(fkMark))) {
      /** generated foreign key name */
      CDE_ASSERT((foreign->unique_name + strlen(strchr(from, '/') + 1)) >
                 foreign->unique_name);
      CDE_ASSERT((foreign->unique_name + strlen(strchr(from, '/') + 1)) <
                 (foreign->unique_name + strlen(foreign->unique_name)));

      snprintf_s(normName, sizeof(normName), sizeof(normName) - 1, "%s%s",
                 strchr(to, '/') + 1,
                 foreign->unique_name + strlen(strchr(from, '/') + 1));
      if (strlen(foreign->unique_name) < strlen(normName))
        foreign->unique_name = safe_strdup_root(&foreign->m_mem_root, normName);
      else
        strCpyRet = strcpy_s(foreign->unique_name,
                             strlen(foreign->unique_name) + 1, normName);
    } else {
      /* user defined foreign key name - we only need to change the database
       * part of the unique name */
      uint32_t newDBNameLen = getDBNameSizeFkStyle(newNameFkStyle);
      uint32_t oldDBNameLen = getDBNameSizeFkStyle(oldNameFkStyle);
      if (newDBNameLen != oldDBNameLen ||
          memcmp(newNameFkStyle, oldNameFkStyle, newDBNameLen)) {
        std::unique_ptr<char[]> oldUniqueName{
            new char[strlen(foreign->unique_name) + 1]};
        strcpy_s(oldUniqueName.get(), strlen(foreign->unique_name) + 1,
                 foreign->unique_name);
        /** oldId as unique name without dbname like :"/fkname" */
        uint32_t oldIdLen = strlen(oldUniqueName.get()) - oldDBNameLen;
        if (newDBNameLen > getDBNameSizeFkStyle(foreign->unique_name))
          foreign->unique_name = static_cast<char *>(
              foreign->m_mem_root.Alloc(newDBNameLen + oldIdLen + 1));
        memcpy_s(foreign->unique_name, newDBNameLen + oldIdLen + 1,
                 newNameFkStyle, newDBNameLen);
        CDE_ASSERT_DEBUG(strlen(oldUniqueName.get()) > oldDBNameLen);
        CDE_ASSERT_DEBUG(oldIdLen >=
                         strlen(oldUniqueName.get() + oldDBNameLen));
        strCpyRet = strcpy_s(foreign->unique_name + newDBNameLen, oldIdLen + 1,
                             oldUniqueName.get() + oldDBNameLen);
      }
      CDE_ASSERT_DEBUG(0 == strCpyRet);
    }

    fkWithChangedNames.insert(foreign);

    if (foreign->referenced_table) {
      foreign->referenced_table->referenced_dict_set.insert(foreign);
    }
  }

  CDE_ASSERT(table->foreign_dict_set.empty());
  table->foreign_dict_set.swap(fkWithChangedNames);

  for (auto foreign : table->referenced_dict_set) {
    CDE_ASSERT(0 == strcmp(foreign->referenced_table_name, from));
    if (strlen(foreign->referenced_table_name) < newTableNameLen)
      foreign->referenced_table_name =
          safe_strdup_root(&foreign->m_mem_root, to);
    else {
      strCpyRet = strcpy_s(foreign->referenced_table_name,
                           strlen(foreign->referenced_table_name) + 1, to);
      CDE_ASSERT_DEBUG(0 == strCpyRet);
    }
  }

  return CDE_OK;
}

void DictTableRefGuard::Release() {
  if (m_dictTable != nullptr) {
    CDE_ASSERT(m_dictTable->getRefCnt() > 0);
    m_dictTable->release();
  }
  m_dictTable = nullptr;
  if (m_mdl != nullptr) {
    CdeDdMdlRelease(m_thd, &m_mdl);
    m_mdl = nullptr;
  }
}

void DictSys::RemoveTableEntryLocked(const char *tableName,
                                     DictSysLockGuard &lockGuard,
                                     bool releaseDictSys) {
  CDE_ASSERT(tableName != nullptr);
  MutexAssertOwner(&g_dictSys->m_mutex);
  MutexAssertOwner(&g_dictSys->m_dictOperationalMtx);

  std::size_t tblSize = 0;
  cde_dict_t *cdeTable = nullptr;

  auto it = m_tables.find(tableName);
  if (it == m_tables.end()) {  // table not exist
    DEBUG_SYNC_C("RemoveTableEntryBegin");
    return;  // not an error - could have been removed by eviction thread
  }

  cdeTable = it->second;
  DEBUG_SYNC_C("RemoveTableEntryBegin");
  m_tables.erase(tableName);

  uint64_t tableId = cdeTable->m_id;
  m_tablesId.erase(tableId);

  tblSize = m_tables.size();
  CDE_ASSERT(tblSize == m_tablesId.size());

  DictRemoveFromEvictLRULocked(cdeTable);
  if (releaseDictSys) lockGuard.Clear();

  if (cdeTable->getRefCnt() > 0) {
    /* It is only possible (cdeTable->getRefCnt() > 0) if there
    is a bg_stat_service running on the table and thus
    bg_stat_service has reference on this table. It should be the
    only case why ref cout of a table is greater than 0 when we
    get here. RemoveTableEntry can be only called by:
    - DDL operation that protects the table under exclusive MDL lock.
    - open table only in case m_discardAfterDDL == true. Shared MDL lock.
    We make sure that we are only thread that releases this table during
    open operation. */
    cdeTable->WaitIfHoldByBgThread();
  }
  CDE_ASSERT(cdeTable->getRefCnt() == 0);
  CdeRemoveStatTable(cdeTable->m_id);
  DestroyTable(cdeTable);
  cde_perf_counters::getInstance()->_dict_size.setValue(tblSize);
  DEBUG_SYNC_C("RemoveTableEntryEnd");
}

void DictSys::RemoveTableEntry(const char *tableName) {
  OperationalLockGuard opWGuard;
  DictSysLockGuard lockGuard;
  RemoveTableEntryLocked(tableName, lockGuard, true);
}

void DictSys::RemoveTableEntryNoOpLock(const char *tableName) {
  DictSysLockGuard lockGuard;
  RemoveTableEntryLocked(tableName, lockGuard, true);
}

void DictSys::DestroyTable(cde_dict_t *tableDict) {
  {
    std::lock_guard<RelationCacheManager> lock(RelationCacheManager);
    relationCacheManager.freeRelation(tableDict->m_id);
  }

  tableDict->destroy_from_cache(false);
  delete tableDict;
}

cde_dict_index_t *cde_dict_t::get_index_on_name(const char *name) {
  cde_dict_index_t *index = nullptr;

  for (cde_dict_index_t *idx_dict : index_dict_vec) {
    if (!idx_dict->IsCommitted()) {
      continue;
    }

    if (strcmp(idx_dict->name.c_str(), name) == 0) {
      index = idx_dict;
      return (index);
    }
  }
  return (nullptr);
}

void cde_dict_t::set_stats_persistent(bool on, bool off) {
  // if both set, ignore off
  if (on && off) {
    off = false;
  }
  stat_persist_enable = 0;
  if (on) {
    stat_persist_enable |= DSTORE_STATS_PERSISTENT_ON;
  }
  if (off) {
    stat_persist_enable |= DSTORE_STATS_PERSISTENT_OFF;
  }
}

bool cde_dict_t::dict_stats_persist_enabled() {
  if (stat_persist_enable & DSTORE_STATS_PERSISTENT_ON) {
    return true;
  } else if (stat_persist_enable & DSTORE_STATS_PERSISTENT_OFF) {
    return false;
  }
  // not set by ddl cmd, use conf value
  return dstore_stats_persistent;
}

void cde_dict_t::set_stats_auto_recalc(bool on, bool off) {
  stat_auto_recalc = 0;
  if (on) {
    stat_auto_recalc |= DSTORE_STATS_AUTO_RECALC_ON;
  }
  if (off) {
    stat_auto_recalc |= DSTORE_STATS_AUTO_RECALC_OFF;
  }
}

bool cde_dict_t::auto_recalc_enabled() {
  if (stat_auto_recalc & DSTORE_STATS_AUTO_RECALC_ON) {
    return true;
  } else if (stat_auto_recalc & DSTORE_STATS_AUTO_RECALC_OFF) {
    return false;
  }
  // not set by ddl cmd, use conf value
  return dstore_stats_auto_recalc;
}

uint64_t cde_dict_t::get_transient_sample_pages() {
  return (stat_sample_pages == 0) ? dstore_stats_transient_sample_pages
                                  : stat_sample_pages;
}

uint64_t cde_dict_t::get_persistent_sample_pages() {
  return (stat_sample_pages == 0) ? dstore_stats_persistent_sample_pages
                                  : stat_sample_pages;
}

uint64_t cde_dict_t::get_transient_index_sample_pages() {
  return (stat_sample_pages == 0) ? dstore_stats_transient_index_sample_pages
                                  : stat_sample_pages;
}

uint64_t cde_dict_t::get_persistent_index_sample_pages() {
  return (stat_sample_pages == 0) ? dstore_stats_persistent_index_sample_pages
                                  : stat_sample_pages;
}

bool cde_dict_t::IsHoldByBgThread() {
  CdeMutexGuard dictMutexGuard(&m_mtx, CDE_LOCATION_HERE);
  MarkBgStatQuit();
  if (bg_stat_flag & BG_STAT_WORKING) {
    return true;
  }
  return false;
}

bool cde_dict_t::MarkHoldByBgThread() {
  CdeMutexGuard dictTableMutexGuard(&m_mtx, CDE_LOCATION_HERE);
  if (bg_stat_flag & BG_STAT_QUIT) {
    return CDE_FAIL;
  }
  CDE_ASSERT(bg_stat_flag == BG_STAT_IDLE);
  bg_stat_flag |= BG_STAT_WORKING;
  return CDE_SUCC;
}

void cde_dict_t::ReleaseByBgThread() {
  CdeMutexGuard dictTableMutexGuard(&m_mtx, CDE_LOCATION_HERE);
  bg_stat_flag &= !BG_STAT_WORKING;
  release();
}

void cde_dict_t::MarkBgStatQuit() { bg_stat_flag |= BG_STAT_QUIT; }

void cde_dict_t::UnMarkBgStatQuit() { bg_stat_flag &= !BG_STAT_QUIT; }

void cde_dict_t::WaitIfHoldByBgThread() {
  while (IsHoldByBgThread()) {
    std::this_thread::sleep_for(
        std::chrono::microseconds(DICT_BG_THREAD_SLEEP_INTERVAL));
  }
}

cde_dict_index_t *cde_dict_t::get_index_on_column(const char **columns,
                                                  uint32_t col_num) {
  for (cde_dict_index_t *index : index_dict_vec) {
    if (!index->IsCommitted()) {
      continue;
    }

    if (index->index_col_num < col_num) {
      continue;
    }
    uint32_t i = 0;
    for (; i < col_num; i++) {
      const char *col_name = index->fields[i].m_name;
      if (CdeStrcasecmp(columns[i], col_name) != 0) {
        break;
      }
    }
    if (i >= col_num) {
      return index;
    }
  }
  return nullptr;
}

void cde_dict_t::getIndexColNames(std::vector<std::string> &indexColsName) {
  std::set<uint32_t> colPosSet;
  for (auto index : index_dict_vec) {
    if (!index->IsCommitted()) {
      continue;
    }

    for (uint32_t i = 0; i < index->index_col_num; ++i) {
      uint32_t fieldPos = index->index_cols[i];
      if (colPosSet.count(fieldPos) == 0) {
        indexColsName.push_back(std::string(index->fields[i].m_name));
        colPosSet.insert(index->index_cols[i]);
      }
    }
  }
}

void cde_dict_t::RenameDictColumn(uint32_t colIdx, const char *from,
                                  const char *to, bool isVirtual) {
  /* Step 1. Rename the target column. */
  CDE_ASSERT(colIdx < m_totalColCount);
  std::string fromStr(from);
  std::string toStr(to);
  CDE_ASSERT(col_names[colIdx] == fromStr);
  col_names[colIdx] = toStr;

  /* Step 2. Replace the field names in every index. */
  for (cde_dict_index_t *idx_dict : index_dict_vec) {
    for (uint32_t i = 0; i < idx_dict->index_col_num; ++i) {
      if (strcmp(idx_dict->fields[i].m_name, from) == 0) {
        idx_dict->fields[i].m_name =
            safe_strdup_root(&idx_dict->m_dictIndexRoot, to);
      }
    }
  }

  /* Step 3. Replace the column names in every foreign key constraint. */
  /* Virtual columns are not allowed for foreign key */
  if (isVirtual) {
    return;
  }

  for (auto it = foreign_dict_set.begin(); it != foreign_dict_set.end(); ++it) {
    cde_dict_foreign_t *foreign = *it;
    for (unsigned f = 0; f < foreign->col_nums; f++) {
      /* These can point straight to table->col_names, because the foreign key
      constraints will be freed at the same time when the table object is freed.
    */
      auto foreignColName =
          get_col_name(static_cast<int>(foreign->foreign_index->index_cols[f]));
      foreign->foreign_col_names[f] = foreignColName;
    }
  }

  for (auto it = referenced_dict_set.begin(); it != referenced_dict_set.end();
       ++it) {
    cde_dict_foreign_t *foreign = *it;
    for (unsigned f = 0; f < foreign->col_nums; f++) {
      auto foreignColName = get_col_name(
          static_cast<int>(foreign->referenced_index->index_cols[f]));
      /* foreign->referenced_col_names[] need to be copies, because the
      constraint may become orphan when foreign_key_checks=0 and the parent
      table is dropped. */
      if (strcmp(foreign->referenced_col_names[f], foreignColName)) {
        char **rc = const_cast<char **>(foreign->referenced_col_names + f);
        size_t colNameLen = strlen(foreignColName) + 1;

        if (colNameLen <= strlen(*rc) + 1) {
          auto err = memcpy_s(*rc, colNameLen, foreignColName, colNameLen);
          CDE_ASSERT(err == EOK);
        } else {
          *rc = safe_strdup_root(&foreign->m_mem_root, foreignColName);
        }
      }
    }
  }
}

cde_dict_foreign_t *cde_dict_t::get_foreign(cde_dict_foreign_t *foreign) {
  if (foreign == nullptr) return nullptr;
  cde_dict_foreign_set::iterator it = foreign_dict_set.find(foreign);
  if (it != foreign_dict_set.end()) {
    return *it;
  }
  it = referenced_dict_set.find(foreign);
  if (it != referenced_dict_set.end()) {
    return *it;
  }
  return nullptr;
}

bool cde_dict_t::ReplaceDictForeignIndex(const char **colNames,
                                         const cde_dict_index_t *index) {
  CDE_ASSERT_DEBUG(index->toBeDropped);

  bool found = true;
  for (auto it = foreign_dict_set.begin(); it != foreign_dict_set.end(); ++it) {
    cde_dict_foreign_t *foreign = *it;

    if (foreign->foreign_index == index) {
      CDE_ASSERT_DEBUG(foreign->foreign_table == index->table);
      cde_dict_index_t *newIndex = CdeForeignKeyFindIndex(
          index->table, colNames, foreign->foreign_col_names, foreign->col_nums,
          index, true, false);
      if (newIndex != nullptr) {
        CDE_ASSERT_DEBUG(newIndex->table == index->table);
        CDE_ASSERT_DEBUG(!newIndex->toBeDropped);
      } else {
        found = false;
      }

      foreign->foreign_index = newIndex;
    }
  }

  for (auto it = referenced_dict_set.begin(); it != referenced_dict_set.end();
       ++it) {
    cde_dict_foreign_t *foreign = *it;
    if (foreign->referenced_index == index) {
      CDE_ASSERT_DEBUG(foreign->referenced_table == index->table);
      cde_dict_index_t *newIndex = CdeForeignKeyFindIndex(
          index->table, nullptr, foreign->referenced_col_names,
          foreign->col_nums, index, true, false);
      if (newIndex != nullptr) {
        CDE_ASSERT_DEBUG(newIndex->table == index->table);
        CDE_ASSERT_DEBUG(!newIndex->toBeDropped);
      } else {
        found = false;
      }

      foreign->referenced_index = newIndex;
    }
  }

  return found;
}

void cde_dict_index_t::destroy_from_cache() {
  CdeFree(rel->rel);
  CdeFree(rel->attr);
  CdeFree(rel->index->m_indexSupportProcInfo);
  CdeFree(rel->index);
  CdeFree(rel->indexInfo);
  rel->Destroy();
  CdeFree(rel);
  rel = nullptr;
  CdeFree(fields);
  fields = nullptr;
  CdeFree(scan_key);
  scan_key = nullptr;
  CdeFree(index_cols);
  index_cols = nullptr;
  CdeFree(attr_cols);
  attr_cols = nullptr;
  CdeFree(stat_n_diff_key_vals);
  stat_n_diff_key_vals = nullptr;
  CdeFree(stat_n_sample_size);
  stat_n_sample_size = nullptr;
  table = nullptr;
}

void cde_dict_t::destroy_foreign_contraints(bool lockOperationalLock) {
  OperationalLockGuard op_guard(lockOperationalLock);

  for (cde_dict_foreign_t *each_foreign : foreign_dict_set) {
    cde_dict_t *table = each_foreign->referenced_table;

    if (table != nullptr) {
      table->dict_stat_read_lock();
      table->referenced_dict_set.erase(each_foreign);
      table->dict_stat_unlock();
    }
    delete each_foreign;
  }
  foreign_dict_set.clear();

  // Reset table field in referencing constraints
  for (cde_dict_foreign_t *each_referenced_foreign : referenced_dict_set) {
    // Warnning: referenced table will destroy. Normally, SQL does not allow
    // this.
    each_referenced_foreign->referenced_table = nullptr;
    each_referenced_foreign->referenced_index = nullptr;
  }
  referenced_dict_set.clear();
}

void cde_dict_t::destroy_from_cache(bool lockOperationalLock) {
  dict_stat_wr_lock();

  destroy_foreign_contraints(lockOperationalLock);

  destroy_all_index_cache();

  if (m_dictCols) {
    delete[] m_dictCols;
    m_dictCols = nullptr;
  }

  if (m_vColCount > 0) {
    CdeFree(attrFull);
  }

  CdeFree(dstore_relation->attr);
  CdeFree(dstore_relation->rel);
  dstore_relation->Destroy();
  CdeFree(dstore_relation);
  dstore_relation = nullptr;
  dict_stat_unlock();
}

void cde_dict_t::add_index_dict(cde_dict_index_t *index_dict) {
  index_dict_vec.push_back(index_dict);
}

void cde_dict_t::OnlineRetryDropDictIndexes() {
  for (auto it = index_dict_vec.begin(); it != index_dict_vec.end();) {
    cde_dict_index_t *index = *it;
    if (index->GetOnlineStatus() ==
        OnlineIndexStatus::ONLINE_INDEX_ABORTED_DROPPED) {
      index->destroy_from_cache();
      delete index;
      it = index_dict_vec.erase(it);
    } else {
      ++it;
    }
  }
}

void cde_dict_t::MarkSecondaryIndexes() {
  for (auto it = index_dict_vec.begin(); it != index_dict_vec.end(); ++it) {
    switch ((*it)->GetOnlineStatus()) {
      case OnlineIndexStatus::ONLINE_INDEX_ABORTED_DROPPED: {
        break;
      }

      case OnlineIndexStatus::ONLINE_INDEX_COMPLETE: {
        if (!(*it)->IsCommitted()) {
          // TODO Wether we need an index lock to protect it.
          (*it)->SetOnlineStatus(
              OnlineIndexStatus::ONLINE_INDEX_ABORTED_DROPPED);
          (*it)->SetCorrupt();
        }
        break;
      }

      case OnlineIndexStatus::ONLINE_INDEX_CREATION: {
        (*it)->SetOnlineStatus(OnlineIndexStatus::ONLINE_INDEX_ABORTED);
        break;
      }

      case OnlineIndexStatus::ONLINE_INDEX_ABORTED: {
        (*it)->SetOnlineStatus(OnlineIndexStatus::ONLINE_INDEX_ABORTED_DROPPED);
        break;
      }

      default: {
        CDE_ASSERT(false);
      }
    }
  }
}

void cde_dict_t::DropSecondaryIndexes() {
  CDE_ASSERT(getRefCnt() == 1);

  for (auto it = index_dict_vec.begin(); it != index_dict_vec.end();) {
    cde_dict_index_t *index = *it;
    if (!index->IsCommitted()) {
      index->destroy_from_cache();
      delete index;
      it = index_dict_vec.erase(it);
    } else {
      it++;
    }
  }
}

bool cde_dict_t::CreateOnlineDdlMgr(THD *thd, const TABLE *mysqlTableOld,
                                    const TABLE *mysqlTableNew,
                                    const dd::Table *ddTableNew,
                                    ha_cde_inplace_ctx *ctx,
                                    OnlineDdlStatus status,
                                    DSTORE::ThreadContext *thrd) {
  CDE_ASSERT(m_onlineDdlMgr == nullptr);
  if (status == OnlineDdlStatus::INVALID) {
    /* No need to construct online ddl mgr. */
    return CDE_SUCC;
  }

  my_thread_id threadId = thd->thread_id();
  CdeRwlockGuard onlineMgrGuard(&m_onlineDdlMgrLock, CdeRwlockOp::WR_LOCK);
  OnlineDdlMgr *onlineMgr = Memory::New<OnlineDdlMgr>(
      mysqlTableOld, mysqlTableNew, ddTableNew, ctx, thrd, threadId);

  /* We must ensure only current thread referenct the current cde_dict_t,
  otherwise the check for online ddl status in further dml-threads after
  prepare_inplace will not accurate. In order to achieve this, the caller
  must own EXCLUSIVE_MDL, thus will clear all opened handlers of this table
  and prevent any other threads to open this table, and bg_stat_service
  also needs to acquire MDL before get cde_dict_t.*/
  CDE_ASSERT(thd->mdl_context.owns_equal_or_stronger_lock(
      MDL_key::TABLE, mysqlTableOld->s->db.str,
      mysqlTableOld->s->table_name.str, MDL_EXCLUSIVE));
  CDE_ASSERT(getRefCnt() == 1);

  if (onlineMgr == nullptr) {
    CDE_LOG_ERROR("alloc online ddl manager for table:%s fail.", name.c_str());
    return CDE_FAIL;
  }
  if (onlineMgr->Init() == CDE_FAIL) {
    CDE_LOG_ERROR("Init online ddl manager for table:%s fail.", name.c_str());
    Memory::Delete(onlineMgr);
    return CDE_FAIL;
  }

  onlineMgr->SetStatus(status);
  m_onlineDdlMgr = onlineMgr;
  return CDE_SUCC;
}

bool cde_dict_t::IsInOnlineDdl() {
  CdeRwlockGuard onlineMgrGuard(&m_onlineDdlMgrLock, CdeRwlockOp::RD_LOCK);
  return m_onlineDdlMgr != nullptr;
}

void cde_dict_t::DestroyOnlineDdlMgr() {
  Memory::Delete<OnlineDdlMgr>(m_onlineDdlMgr);
  m_onlineDdlMgr = nullptr;
}

static void CdeGetAllForeignTableName(cde_dict_t *dict_table,
                                      std::set<std::string> &tables) {
  for (const auto &entry : dict_table->referenced_dict_set) {
    auto foreign_tbl = entry->foreign_table;
    if (!tables.count(foreign_tbl->name)) {
      tables.emplace(foreign_tbl->name);
      CdeGetAllForeignTableName(foreign_tbl, tables);
    }
  }
}

void CdeGetAllRelativeTables(cde_dict_t *dict_table,
                             std::set<std::string> &tables) {
  // get all foreign table names cascade
  CdeGetAllForeignTableName(dict_table, tables);

  // get referenced table names not cascade
  for (const auto &entry : dict_table->foreign_dict_set) {
    tables.emplace(entry->referenced_table_name);
  }
}

/**
Add the escape character to the entered database name.
eg: test.123 -> `test.123`
    test`123 -> `test``123`

@param[in]      src            database object name.

@return str added escape char.
*/
std::string EscapeStr(const std::string &src) {
  std::string res = "`";
  for (const auto &c : src) {
    if (c == '`') res += '`';
    res += c;
  }
  res += "`";
  return res;
}

/**
Add the quote escape character to the entered database obj name.
For example, in a character field query with quotes:
select * from t where name = '$database_object_name'
eg: t'1 -> t\'1
    t\1 -> t\\1

@param[in]      src            database object name.

@return str added quote escape char.
*/
std::string quoteEscapeStr(const std::string &src) {
  static const std::unordered_set<char> escapeChars = {'\'', '\\'};

  std::string res = "";
  for (const auto &c : src) {
    if (escapeChars.find(c) != escapeChars.end()) {
      res += "\\";
    }
    res += c;
  }
  return res;
}

bool CdeDdMdlAcquire(THD *thd, MDL_ticket **mdl, const char *dictName) {
  char fileTableName[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char fileDbName[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  CdeCreateTableInfo::SplitNormalizedName(dictName, fileTableName, fileDbName);

  char tableName[CDE_MAX_TABLE_NAME_LEN + 1] = {'\0'};
  char dbName[CDE_MAX_DATABASE_NAME_LEN + 1] = {'\0'};
  filename_to_tablename(fileDbName, dbName, sizeof(dbName));
  filename_to_tablename(fileTableName, tableName, sizeof(tableName));

  int table_name_len = strlen(tableName);
  int db_name_len = strlen(dbName);

  if (lower_case_table_names == 2) {
    my_casedn_str(system_charset_info, tableName);
    my_casedn_str(system_charset_info, dbName);
  } else {
#ifndef _WIN32
    if (lower_case_table_names == 1) {
      my_casedn_str(system_charset_info, tableName);
      my_casedn_str(system_charset_info, dbName);
    }
#endif /* !_WIN32 */
  }
  if (table_name_len == 0 || db_name_len == 0 ||
      dd::acquire_shared_table_mdl(thd, dbName, tableName, false, mdl)) {
    return false;
  }

  return true;
}

void CdeDdMdlRelease(THD *thd, MDL_ticket **mdl) {
  if (*mdl == nullptr) {
    return;
  }

  dd::release_mdl(thd, *mdl);
  *mdl = nullptr;
}
} /* namespace CDE */
