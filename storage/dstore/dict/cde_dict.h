/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_dict.h
 *
 *
 * -------------------------------------------------------------------------
 */

#ifndef __CDE_DICT_H__
#define __CDE_DICT_H__

#include <atomic>
#include <boost/intrusive/list.hpp>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/cde_alloc.h"
#include "common/cde_def.h"
#include "common/cde_mutex.h"
#include "common/dstore_common_utils.h"
#include "index/dstore_scankey.h"
#include "my_alloc.h"
#include "my_base.h"
#include "systable/dstore_relation.h"

#include "ddl/cde_online_ddl.h"
#include "dict/cde_autoinc.h"

using DSTORE::CommitSeqNo;
using DSTORE::ThreadContext;
class MDL_ticket;

#define ulint unsigned long

namespace CDE {

struct TrxTable {
  TrxTable(uint64_t tableId, const std::string &name, uint64_t statNRows,
           uint64_t statModifiedCounter, bool autoRecalcEnabled)
      : m_tableId(tableId),
        m_name(name),
        m_statNRows(statNRows),
        m_statModifiedCounter(statModifiedCounter),
        m_autoRecalcEnabled(autoRecalcEnabled) {}
  /* table id of this table/relation in dstore */
  uint64_t m_tableId;
  /* Table name is combination of db_name and table_name */
  std::string m_name;
  /* Table row counts */
  uint64_t m_statNRows;
  /* Modified counter since last calc */
  uint64_t m_statModifiedCounter;
  /* switch of auto recalc table stat */
  bool m_autoRecalcEnabled;
};

typedef std::vector<TrxTable> TrxTableVec;

// A structure containing two column indexes; one index to be used in the
// MySQL server and one index to be used in DStore. The two indexes will
// always represent the same column, but the same column may have different
// index in MySQL Server and DStore. The reason is that columns that are
// dropped using ALGORITHM=INSTANT is not visible in MySQL server, while
// they are visible in DStore.
struct MysqlAndDstoreIndex {
  uint16_t m_mysqlIndex;
  uint16_t m_dstoreIndex;
};
#define CDE_DATA_MBMAX 4

extern bool dstore_stats_auto_recalc;
extern bool dstore_stats_persistent;
extern unsigned long long dstore_stats_persistent_sample_pages;
extern unsigned long long dstore_stats_transient_sample_pages;
extern unsigned long long dstore_stats_persistent_index_sample_pages;
extern unsigned long long dstore_stats_transient_index_sample_pages;
extern ulong g_dstoreTableDefinitionCache;

struct cde_dict_t;
struct cde_dict_index_t;

#define CDE_DDL_FK_RULE_DEFAULT (0)
#define CDE_DDL_FK_RULE_ON_DELETE_CASCADE (1)
#define CDE_DDL_FK_RULE_ON_DELETE_SET_NULL (2)
#define CDE_DDL_FK_RULE_ON_DELETE_NO_ACTION (4)
#define CDE_DDL_FK_RULE_ON_UPDATE_CASCADE (8)
#define CDE_DDL_FK_RULE_ON_UPDATE_SET_NULL (16)
#define CDE_DDL_FK_RULE_ON_UPDATE_NO_ACTION (32)

constexpr uint64_t TABLE_SPACE_ID_SHIFT = 32;

struct DictVirtualCol {
  /** Index of virtual column in table's columns */
  uint32_t ind;
  /** Indexes of base columns in table's columns */
  std::vector<uint32_t> m_baseColumnInd;
};

struct cde_dict_foreign_t {
  cde_dict_foreign_t() : m_mem_root(PSI_NOT_INSTRUMENTED, 100) {}

  char *unique_name{nullptr};
  uint8_t fk_rule{CDE_DDL_FK_RULE_DEFAULT};  // CDE_DDL_FK_RULE_DEFAULT or ...
  char *foreign_table_name{nullptr};
  cde_dict_t *foreign_table{nullptr};
  cde_dict_index_t *foreign_index{nullptr};

  char *referenced_table_name{nullptr};
  cde_dict_t *referenced_table{nullptr};
  cde_dict_index_t *referenced_index{nullptr};

  uint32_t col_nums{0};
  const char **foreign_col_names{nullptr};
  const char **referenced_col_names{nullptr};

  MEM_ROOT m_mem_root;
};

struct cde_dict_foreign_compare {
  bool operator()(const cde_dict_foreign_t *lhs,
                  const cde_dict_foreign_t *rhs) const {
    return (strcmp(lhs->unique_name, rhs->unique_name) < 0);
  }
};

typedef std::set<cde_dict_foreign_t *, cde_dict_foreign_compare>
    cde_dict_foreign_set;

/** Data structure for a field in an index */
struct cde_dict_field_t {
  /** field in Heap TupleDesc's attr postion, start from 0 */
  uint32_t attId;
  /** length of field */
  uint32_t len;
  /** !< 0 or the length of the column prefix in bytes in a MySQL index of
  type, e.g., INDEX (textcol(25)); must be smaller than
  DICT_MAX_FIELD_LEN_BY_FORMAT;
  NOTE that in the UTF-8 charset, MySQL sets this to (mbmaxlen * the prefix len)
  in UTF-8 chars */
  uint32_t prefix_len : 12;
  /** !< minimum and maximum length of a character, in bytes;*/
  uint32_t mbminlen : 3;
  uint32_t mbmaxlen : 3;
  /** field in TABLE's position, start from 0 */
  uint32_t fieldId;
  /** whether field is virtual column */
  bool m_isVirtual;
  /** name of column */
  const char *m_name;
};

/** The status of online index creation */
enum OnlineIndexStatus : uint8_t {
  /** the index is complete and ready for access */
  ONLINE_INDEX_COMPLETE = 0,
  /** the index is being created, online
  (allowing concurrent modifications) */
  ONLINE_INDEX_CREATION,
  /** secondary index creation was aborted and the index
  should be dropped as soon as index->table->n_ref_count reaches 0,
  or online table rebuild was aborted and the clustered index
  of the original table should soon be restored to
  ONLINE_INDEX_COMPLETE */
  ONLINE_INDEX_ABORTED,
  /** the online index creation was aborted, the index was
  dropped from the data dictionary and the tablespace, and it
  should be dropped from the data dictionary cache as soon as
  index->table->n_ref_count reaches 0. */
  ONLINE_INDEX_ABORTED_DROPPED
};

struct cde_dict_index_t {
  cde_dict_index_t() : m_dictIndexRoot(PSI_NOT_INSTRUMENTED, 128) {}

  cde_dict_t *table{nullptr};
  // oid of this index/relation in dstore
  uint64_t oid;
  std::string name;
  /*
    Number of bytes required to store all keypart value. This may be
    different from the "length" field as it also counts
     - possible NULL-flag byte (see HA_KEY_NULL_LENGTH)
     - possible HA_KEY_BLOB_LENGTH bytes needed to store actual value length.
  */
  uint32_t key_length{0};
  DSTORE::StorageRelation rel;
  DSTORE::ScanKey scan_key;
  // array of index's column field index in TABLE
  uint32_t *index_cols{nullptr};
  // array of index's column tupledesc index in Heap TupleDesc's attr
  uint32_t *attr_cols{nullptr};
  uint32_t index_col_num{0};
  cde_dict_field_t *fields{nullptr};

  // num of pages which used to calc index stats
  uint64_t *stat_n_sample_size{nullptr};
  // diff key vals array
  uint64_t *stat_n_diff_key_vals{nullptr};
  // array of non-null key values for this index, for each column
  // use when set nulls_ignored
  uint64_t *stat_n_non_null_key_vals{nullptr};
  // num of whole index pages in index tree
  uint32_t stat_index_page_cnt{0};
  // num of leaf pages in index tree
  uint32_t stat_leaf_page_cnt{0};
  /** Fillfactor value that parsed from index comment, set to correct value
  if fillfactor specified and value is valid, else set to 0(invalid value). */
  int32_t fillfactor{0};

  /*
    Currently reserved, not used at present, waiting for dstore to provide an
    interface to obtain. Array of rec_per_key for this index, for each column.
  */
  float *rec_per_keys{nullptr};

  /** a flag that is set for indexes that have not been committed to
  the data dictionary yet. */
  bool uncommitted{false};

  /** true if the index is to be dropped. */
  bool toBeDropped{false};

  /** enum online_index_status. Transitions from ONLINE_INDEX_COMPLETE (to
  ONLINE_INDEX_CREATION) are protected by dict_operation_lock and
  dict_sys->mutex. */
  std::atomic<OnlineIndexStatus> onlineStatus{
      OnlineIndexStatus::ONLINE_INDEX_COMPLETE};

  /* whether index is corrupt. */
  std::atomic<bool> isCorrupt{false};

  // The snapshot csn when index created.
  CommitSeqNo index_csn{0};

  RowLog *m_rowLog{nullptr};

  MEM_ROOT m_dictIndexRoot;

  void destroy_from_cache();
  bool has_prefix_field() {
    if (fields == nullptr) {
      return false;
    }
    for (uint32_t i = 0; i < index_col_num; i++) {
      if ((fields + i)->prefix_len != 0) {
        return true;
      }
    }
    return false;
  }

  /** Determine if the index has been committed to the
  data dictionary.
  @return whether the index definition has been committed */
  bool IsCommitted() const { return !uncommitted; }

  /** Flag an index committed or uncommitted.
  @param[in]    committed    whether the index is committed */
  void SetCommitted(bool committed) { uncommitted = !committed; }

  void SetCorrupt() { isCorrupt = true; }

  bool IsCorrupt() { return isCorrupt; }

  OnlineIndexStatus GetOnlineStatus() { return onlineStatus; }

  void SetOnlineStatus(OnlineIndexStatus status) { onlineStatus = status; }

  bool DictStatsShouldIgnoreIndex() {
    return IsCorrupt() || toBeDropped || !IsCommitted();
  }

  /* If need to access prefixed field, need to go back to heap table
  due to prefixed field does not include the complete data. */
  bool accessColumnsHasPrefixField(const MysqlAndDstoreIndex *columns,
                                   uint32_t num) {
    if (columns == nullptr || num == 0 || fields == nullptr) {
      return false;
    }
    for (uint32_t i = 0; i < num; i++) {
      for (uint32_t j = 0; j < index_col_num; j++) {
        if ((fields + j)->prefix_len != 0 &&
            (fields + j)->attId == columns[i].m_dstoreIndex) {
          return true;
        }
      }
    }
    return false;
  }

  /** Checks whether index covers field(column)
  @param[in] dstoreIndex  index of a column to check

  @return true  - index covers the column
          false - index does not cover the column
  */
  bool indexCoversColumn(uint16_t dstoreIndex) {
    for (uint32_t i = 0; i < index_col_num; i++) {
      if ((fields + i)->attId == dstoreIndex) return true;
    }
    return false;
  }
};

struct DictCol;

/** Save default value for columns which have been added instantly */
struct DictColDefaultVal {
  DictColDefaultVal() : m_col(nullptr), m_value(nullptr), m_len(0) {}
  ~DictColDefaultVal() {
    if (m_value) {
      delete[] m_value;
      m_value = nullptr;
    }
    m_col = nullptr;
  }
  DictColDefaultVal(DictColDefaultVal &) = delete;
  DictColDefaultVal &operator=(DictColDefaultVal &) = delete;
  /** Pointer to the column itself */
  DictCol *m_col;
  /** Columns's default value in bytes which has been added instantly */
  uint8_t *m_value;
  /** Length of default value */
  size_t m_len;
};

/** Dict column defination, include normal columns, instant add/drop columns
 */
struct DictCol {
  DictCol()
      : m_instantDefaultVal(),
        m_isVisible(true),
        m_isVirtual(false),
        m_versionAdded(0),
        m_versionDropped(0) {}
  ~DictCol() {}
  /** Column default value, value is get from dd if the col is add instantly,
  and set by engine itself if this col is dropped instantly. */
  DictColDefaultVal m_instantDefaultVal;

  /** Whether the column is visible, only set to false if col is dropped
  instantly */
  bool m_isVisible;

  /** Whether the column is generated virtual */
  bool m_isVirtual;

  /** Position of virtual column in m_vCols. Only valid for virtual columns */
  uint16_t m_virtualPos{0};

  /** Position of column on physical row. start from 0. It also represents the
   * index listed in the heap tupledesc attrs*/
  uint32_t m_phyPos{~0U};

  /** Which version this col has been added instantly */
  uint8_t m_versionAdded;

  /** Which version this col has been dropped instantly */
  uint8_t m_versionDropped;
};

/** Dict field length info defination. */
struct DictFieldLen {
  DictFieldLen()
      : m_fieldLenBytes(0), m_packLength(0), m_mbmaxlen(0), m_mbminlen(0) {}

  DictFieldLen(uint16_t fieldLenBytes, uint32_t packLength, uint32_t mbmaxlen,
               uint32_t mbminlen)
      : m_fieldLenBytes(fieldLenBytes),
        m_packLength(packLength),
        m_mbmaxlen(mbmaxlen),
        m_mbminlen(mbminlen) {}

  /** the number of bytes to represent the field length. */
  uint16_t m_fieldLenBytes;

  /** the length of packed. */
  uint32_t m_packLength;

  /** the max length of charset. */
  uint32_t m_mbmaxlen;

  /** the min length of charset. */
  uint32_t m_mbminlen;
};

constexpr uint32_t DSTORE_STATS_PERSISTENT_ON = (1 << 1);
constexpr uint32_t DSTORE_STATS_PERSISTENT_OFF = (1 << 2);

constexpr uint32_t DSTORE_STATS_AUTO_RECALC_ON = (1 << 1);
constexpr uint32_t DSTORE_STATS_AUTO_RECALC_OFF = (1 << 2);

constexpr uint16_t BG_STAT_IDLE = 0;
constexpr uint16_t BG_STAT_WORKING = (1 << 0);
constexpr uint16_t BG_STAT_QUIT = (1 << 1);

/** TEMPORARY; true for tables from CREATE TEMPORARY TABLE. */
constexpr uint32_t DICT_TABLE_TEMPORARY = 1;

/**
Initialize the dict sys object. This should be called once at startup.
*/
void DictSysInit();

/**
Uninitialize the dict sys object. This should be called once before shutdown.
*/
void DictSysDestroy();

/**
A guard class for acquiring and releasing the dict sys lock. The lock will be
acquired when the object is created and released when the object is destroyed.
This can be used to ensure that the lock is always released even if an exception
is thrown.
*/
class DictSysLockGuard {
  CdeMutexGuard m_guard;

 public:
  DictSysLockGuard();
  void Clear() { m_guard.Clear(); }
};

/** cde dict table struct, should add ref count when open handler */
struct cde_dict_t {
  /** The high 32 bits is used to store the tablespace id. The low 32 bits is
  used to store the table heap oid */
  uint64_t m_id;
  /** Table name is combination of db_name and table_name
  which passed from server */
  std::string name;
  /** The mutex to protect members of cde_dict_t, current only
  bg_stat_flag need be protected at some situaions. */
  mysql_mutex_t m_mtx;
  std::atomic<uint64_t> ref_count;
  DSTORE::StorageRelationData *dstore_relation;
  std::vector<cde_dict_index_t *> index_dict_vec;
  std::vector<std::string> col_names;
  /** vector of virtual columns */
  std::vector<DictVirtualCol> m_vCols;
  cde_dict_foreign_set foreign_dict_set;
  cde_dict_foreign_set referenced_dict_set;
  /** Attributes that include also virtual columns. They are
   * needed when creating indexes on virtual columns. Those indexes
   * store materialized values for virtual columns. In order to store
   * them we need to create index that includes virtual columns. Note
   * that attr from storage relation used for creating heap - does not
   * include attributes for virtual columns.
   */
  DSTORE::TupleDesc attrFull;

  DictAutoinc autoinc_dict;

  /** DictCol array added for instant add&drop, the array size is
  m_totalColCount, include normal cols, cols added/dropped instantly, the
  memory is allocted by new[], so remember to use delete[] when release. */
  DictCol *m_dictCols;

  std::vector<DictFieldLen> m_fieldLenInfos;

  /** Timestamp of the last modification of this table */
  time_t update_time;
  /** Timestamp of last recalc of the stats. */
  std::chrono::steady_clock::time_point stats_last_recalc;

  pthread_rwlock_t stat_rwlock;
  /** Is stats data persist */
  uint32_t stat_persist_enable;
  /** Is auto recalc enable */
  uint32_t stat_auto_recalc;
  bool stat_initialized;
  /** Sample page numer, passed by create table option */
  uint32_t stat_sample_pages;
  /** Block/page num of heap */
  uint64_t stat_heap_page_count;
  /** Free block/page num of heap */
  uint64_t stat_heap_free_page_count;
  /** Block/page num of lob heap allocated for storing LOB data larger than 2KB.
   * This variable is used solely to calculate the data_file_length value in the
   * info_impl() method, which in turn is used to populate the DATA_LENGTH field
   * in the information_schema.TABLES view. During page sampling and row‑count
   * estimation (stat_n_rows), these LOB pages are excluded; only
   * stat_heap_page_count (heap pages for in‑row data) is considered.*/
  uint64_t stat_heap_lob_page_count;
  /** Table row counts */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) uint64_t stat_n_rows;
  /** Sum of total index page count, used to calc index file len in ha_stats */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) uint64_t stat_all_indexs_page_count;
  /** Modified counter since last calc */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) uint64_t stat_modified_counter;
  /** Bg stat flag, to identify bg stat process and foregroud ddl op */
  uint16_t bg_stat_flag;
  /** Fillfactor value that parsed from table comment, set to correct value
  if fillfactor specified and value is valid, else set to 0(invalid value). */
  int32_t fillfactor{0};
  /** Refresh/reload FK info */
  bool refresh_fk{false};
  std::unique_ptr<char[]> oldName;
  /** Discard after ddl, right now only instant ddl will set this flag */
  bool m_discardAfterDDL{false};

  // todo: this flag need to be set when doing ALTER_COPY etc.
  unsigned skip_alter_undo{false};

  /** Current row version in case columns are added/dropped INSTANTly */
  uint32_t m_currentRowVersion{0};

  /** Current column count */
  uint32_t m_currentColCount{0};

  /** Total column count, include column which has been dropped
  instantly and generated virtual columns */
  uint32_t m_totalColCount{0};

  /** Number of generated virtual columns */
  uint32_t m_vColCount{0};

  /** Stores information about:
  1 whether the table has been created using CREATE TEMPORARY TABLE*/
  uint32_t flags{0};

  /** Csn when table is rebuilt, mainly used for table with no primary key. */
  CommitSeqNo m_rebuild_csn{0};

  /** last postion of stored column's on physical row. Convenient calculate when
   * add column by instant */
  uint32_t m_maxPos{0};

  OnlineDdlMgr *m_onlineDdlMgr{nullptr};
  /** mutex used to protect the online ddl mgr
  Todo: if we need to use the rw lock for manage online ddl, because
  CdeRowLogForRollback and CdeRowLogForIndex for rollback may hold the mutex
  too long? */
  pthread_rwlock_t m_onlineDdlMgrLock;

  /** tablespace id */
  DSTORE::TablespaceId m_spaceId;

  cde_dict_t() : cde_dict_t(nullptr) {}
  explicit cde_dict_t(DSTORE::StorageRelationData *rel)
      : ref_count(1),
        dstore_relation(rel),
        index_dict_vec(),
        autoinc_dict(),
        m_dictCols(nullptr),
        update_time(0),
        stat_persist_enable(0),
        stat_auto_recalc(0),
        stat_initialized(false),
        stat_sample_pages(0),
        stat_heap_page_count(0),
        stat_heap_free_page_count(0),
        stat_heap_lob_page_count(0),
        stat_n_rows(0),
        stat_all_indexs_page_count(0),
        stat_modified_counter(0),
        bg_stat_flag(BG_STAT_IDLE),
        fillfactor(0),
        refresh_fk(false),
        m_discardAfterDDL(false) {
    int ret = MutexInit(0, &m_mtx, MY_MUTEX_INIT_FAST);
    ret |= pthread_rwlock_init(&stat_rwlock, nullptr);
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr,
                                  PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    ret |= pthread_rwlock_init(&m_onlineDdlMgrLock, &attr);
    CDE_ASSERT(ret == 0);
  }
  ~cde_dict_t() {
    MutexDestroy(&m_mtx);
    (void)pthread_rwlock_destroy(&stat_rwlock);
    int32_t ret = pthread_rwlock_destroy(&m_onlineDdlMgrLock);
    CDE_ASSERT(ret == 0);
    /* We must have destroyed m_onlineDdlMgr before ddl transaction end.*/
    CDE_ASSERT(m_onlineDdlMgr == nullptr);
  }
  void set_stats_persistent(bool, bool);
  bool dict_stats_persist_enabled();
  void set_stats_auto_recalc(bool, bool);
  bool auto_recalc_enabled();
  uint64_t get_transient_sample_pages();
  uint64_t get_persistent_sample_pages();
  uint64_t get_transient_index_sample_pages();
  uint64_t get_persistent_index_sample_pages();
  cde_dict_index_t *get_index_on_name(const char *name);
  cde_dict_index_t *get_index_on_column(const char **columns, uint32_t col_num);
  void getIndexColNames(std::vector<std::string> &indexColsName);
  cde_dict_foreign_t *get_foreign(cde_dict_foreign_t *foreign);
  void destroy_from_cache(bool lockOperationalLock = true);
  void destroy_foreign_contraints(bool lockOperationalLock = true);
  /**
  Get dict table total columns count.

  @return total columns count.
  */
  uint32_t GetTotalCols() const { return m_totalColCount; }

  /**
  Acquire should be wrapped by g_dictSys::m_mutex to ensure
  the accuracy of refcount.
  */
  inline void acquire() { ref_count.fetch_add(1, std::memory_order_seq_cst); }

  /**
  Release has no need for any protection.
  */
  inline void release() { ref_count.fetch_sub(1, std::memory_order_seq_cst); }

  /**
  If GetRefCnt need the accurate refcount, is should be wrapped by
  g_dictSys::m_mutex. Otherwise no need.
  */
  inline uint64_t getRefCnt() {
    return ref_count.load(std::memory_order_seq_cst);
  }

  void destroy_all_index_cache() {
    for (auto dict_index : index_dict_vec) {
      dict_index->destroy_from_cache();
      delete dict_index;
    }
    index_dict_vec.clear();
  }

  void inc_n_rows() {
    if (stat_initialized) {
      uint64_t n_rows = stat_n_rows;
      if (n_rows < 0xFFFFFFFFFFFFFFFFULL) {
        stat_n_rows = n_rows + 1;
      }
    }
  }

  // not protected by latch
  void dec_n_rows() {
    if (stat_initialized) {
      uint64_t n_rows = stat_n_rows;
      if (n_rows > 0) {
        stat_n_rows = n_rows - 1;
      }
    }
  }

  /**
  Check wheter current cde_dict_t is referenced by backgroup thread.

  @return true  if referenced by backgroup thread, otherwise false
  */
  bool IsHoldByBgThread();

  /**
  Mark the current cde_dict_t is referenced by backgroup thread.

  @return CDE_SUCC if mark success, otherwise CDE_FAIL
  */
  bool MarkHoldByBgThread();

  /**
  Release cde_dict_t which is referenced by backgroup thread.
  */
  void ReleaseByBgThread();

  /**
  Mark flag to tell backgroup thread release current cde_dict_t and wait
  until it is released. Return immediately if it is not referenced by
  backgroup thread. Remember to unmark the flag at the appropriate time
  if current cde_dict_t need to be handled by backgroup thread later.
  */
  void WaitIfHoldByBgThread();

  /**
  Mark flag to tell backgroup thread release current cde_dict_t.
  */
  void MarkBgStatQuit();

  /**
  Unmark flag to tell backgroup thread release current cde_dict_t.
  */
  void UnMarkBgStatQuit();

  DSTORE::StorageRelationData *get_dstore_relation() { return dstore_relation; }

  std::vector<cde_dict_index_t *> *get_index_dict_vec() {
    return &index_dict_vec;
  }

  int get_index_num() { return index_dict_vec.size(); }

  uint64_t get_table_rows() { return stat_n_rows; }

  int get_n_attrs() { return attrFull->natts; }

  const char *get_col_name(int col_no) const {
    if (col_no >= attrFull->natts) {
      return nullptr;
    }
    return col_names[col_no].c_str();
  }

  void add_index_dict(cde_dict_index_t *index_dict);

  /** Rename a given index in the data dictionary cache.
  @param[in,out] index index to rename
  @param newName new index name */
  void RenameDictIndex(cde_dict_index_t *index, const char *newName) {
    index->name = newName;
  }

  /** Drop in-memory metadata for index (cde_dict_index_t) left from previous
  online ALTER operation. The dict sys mutex should have been locked. */
  void OnlineRetryDropDictIndexes();

  /** We will have to drop the secondary indexes later, when the table is
  in use, unless the the DDL has already been externalized. Mark the indexes
  as incomplete and corrupted, so that other threads will stop using them.
  Let DictSysRemoveTable() or crash recovery or the next invocation of
  prepare_inplace_alter_table() take care of dropping the indexes. */
  void MarkSecondaryIndexes();

  /** Drop all indexes */
  void DropSecondaryIndexes();

  /** Renames a column of a table in the data dictionary cache.
  @param[in] colIdx Column index that to be renamed
  @param[in] from Old column name
  @param[in] to New column name
  @param[in] isVirtual If this is a virtual column */
  void RenameDictColumn(uint32_t colIdx, const char *from, const char *to,
                        bool isVirtual);

  int add_foreign_dict(cde_dict_foreign_t *foreign_dict) {
    try {
      foreign_dict_set.insert(foreign_dict);
    } catch (const std::bad_alloc &) {
      return HA_ERR_OUT_OF_MEM;
    }
    return CDE_OK;
  }

  int add_referenced_dict(cde_dict_foreign_t *foreign_dict) {
    try {
      referenced_dict_set.insert(foreign_dict);
    } catch (const std::bad_alloc &) {
      return HA_ERR_OUT_OF_MEM;
    }
    return CDE_OK;
  }

  /** Replace the index passed in with another equivalent index in the
  foreign key lists of the table.
  @param[in] colNames column names
  @param[in] index    to be replaced dict index
  @return whether all replacements were found */
  bool ReplaceDictForeignIndex(const char **colNames,
                               const cde_dict_index_t *index);

  /** Gets the nth column of a table.
  @param[in] pos        position of column
  @return pointer to column object */
  DictCol *get_col(uint i) { return m_dictCols + i; }

  /** Get column by name
  @param[in]    name    column name
  @return column name if found, null otherwise */
  DictCol *get_col_by_name(const char *name) {
    int i = 0;
    for (auto &str : col_names) {
      if (strcmp(str.c_str(), name) == 0) {
        return get_col(i);
      }
      ++i;
    }

    return nullptr;
  }

  void dict_stat_read_lock() { (void)pthread_rwlock_rdlock(&stat_rwlock); }

  void dict_stat_wr_lock() { (void)pthread_rwlock_wrlock(&stat_rwlock); }

  void dict_stat_unlock() { (void)pthread_rwlock_unlock(&stat_rwlock); }

  void del_index_dict() { assert(0); }

  /** convert field index to heap tuple des attr index */
  uint32_t heapAttrIdx(uint32_t fieldIdx) {
    CDE_ASSERT(fieldIdx < m_totalColCount);
    return m_dictCols[fieldIdx].m_phyPos;
  }

  void setOldName(const char *oldName) {
    if (nullptr == oldName) {
      this->oldName.reset();
    } else {
      uint32_t oldNameLen = std::strlen(oldName) + 1;
      this->oldName = std::make_unique<char[]>(oldNameLen);
      strcpy_s(this->oldName.get(), oldNameLen, oldName);
    }
  }

  /** Determine if this is a temporary table. */
  bool is_temporary() const { return (flags & DICT_TABLE_TEMPORARY); }

  /** Get tablespace id from the high 32 bits of id
  @return tablespace id */
  Oid GetTablespaceId() {
    return static_cast<Oid>(m_id >> TABLE_SPACE_ID_SHIFT);
  }

  /** Get heap id from the low 32 bits of id
  @return heap id */
  Oid GetHeapOid() { return static_cast<Oid>(m_id & 0xFFFFFFFFULL); }

  bool CreateOnlineDdlMgr(THD *thd, const TABLE *mysqlTableOld,
                          const TABLE *mysqlTableMew,
                          const dd::Table *ddTableNew, ha_cde_inplace_ctx *ctx,
                          OnlineDdlStatus status, DSTORE::ThreadContext *thrd);

  bool IsInOnlineDdl();
  void DestroyOnlineDdlMgr();

  /** Prev/next pointer. Used by boost intrusive list. Makes cde_dict_t
  a node in this list. Removal is O(1) - which is important as we
  very often move tables to the beginning of eviction LRU list -
  making them MRU.
  */
  boost::intrusive::list_member_hook<> m_LRUListHook;
};

class DictSys {
 public:
  friend class DictSysLockGuard;
  friend class OperationalLockGuard;
  DictSys() {
    MutexInit(0, &m_mutex, MY_MUTEX_INIT_FAST);
    MutexInit(0, &m_dictOperationalMtx, MY_MUTEX_INIT_FAST);
  }
  ~DictSys();

  /**
  Retrieves the table entry with the specified name.

  @param[in]       tableName The name of the table to retrieve.
  @param[in]       getByBgStat Whether called by backgroud-stat thread.

  @return A pointer to the cde_dict_t object representing the table, or NULL if
  not found.
  */
  cde_dict_t *GetTableEntry(const char *tableName, bool getByBgStat = false);

  /**
  Retrieves the table entry with the given table id.

  @param[in]       tableId The table id of the table to retrieve.

  @return A pointer to the cde_dict_t object representing the table, or NULL if
  not found.
  */
  cde_dict_t *GetTableEntryById(uint64_t tableId);

  /**
  Retrieves the table entry with the specified name. The caller must hold
  lock on dictSys.

  @param[in]       tableName The name of the table to retrieve.
  @param[in]       getByBgStat Whether called by backgroud-stat thread.

  @return A pointer to the cde_dict_t object representing the table, or NULL if
  not found.
  */
  cde_dict_t *GetTableEntryLocked(const char *tableName,
                                  bool getByBgStat = false);

  /**
  Retrieves the table entry with the specified name. The caller must hold
  lock on dictSys.

  @param[in]       tableName The name of the table to retrieve.
  @param[in]       getByBgStat Whether called by backgroud-stat thread.

  @return A pointer to the cde_dict_t object representing the table, or NULL if
  not found.
  */
  cde_dict_t *GetTableEntryLockedById(uint64_t tableId);

  /**
  Adds a new table entry to the dictionary cache.

  @param[in]       tableName The name of the table to add.
  @param[in]       table     Pointer to the cde_dict_t object to add.
  @param[out]      ptrExist  If dict_table with same name already exists
                             in dict_cache, use this to point to it.
  */
  void AddTableEntry(const char *tableName, cde_dict_t *table,
                     cde_dict_t **ptrExist, bool allowEviction = true);

  /**
  Removes a table entry from the dictionary cache.

  @param[in]       tableName The name of the table to remove.
  */
  void RemoveTableEntry(const char *tableName);

  /**
  Removes a table entry from the dictionary cache. The caller needs to hold
  Operational Lock already.

  @param[in]       tableName The name of the table to remove.
  */
  void RemoveTableEntryNoOpLock(const char *tableName);

  /**
  Removes a table entry from the dictionary cache. The caller needs to hold
  lock on dictSys.

  @param[in]       tableName The name of the table to remove.
  @param[in]       lockGuard Locked guard on dictSys lock.
  */
  void RemoveTableEntryLocked(const char *tableName,
                              DictSysLockGuard &lockGuard,
                              bool releaseDictSys = true);

  /**
  Renames a table entry in the dictionary cache.

  @param[in]       form The current table name.
  @param[in]       to The new table name.
  @param[in]       renameForeigns Whether to rename foreign key object.

  @return 0 if success, errno if fail.
  */
  int32_t RenameTableEntry(const char *form, const char *to,
                           bool renameForeigns);

 private:
  /** locked for modyfing foreign contraints
   * (i.e. table open, create, drop)
   */
  mysql_mutex_t m_dictOperationalMtx;

  /**
  Destroy the table dict object from memory
  @param[in]      tableDict       the table dict object will be destoried from
  memory
  */
  void DestroyTable(cde_dict_t *tableDict);

  // mutex to protect sys dict operations
  mysql_mutex_t m_mutex;

  /* Map of the tables, base on name */
  std::unordered_map<std::string, cde_dict_t *> m_tables;

  /* Map of the tables, base on id */
  std::unordered_map<uint64_t, cde_dict_t *> m_tablesId;
  cde_dict_t *m_ddlLog = nullptr;
};

/** Helper class to guard locking/unlocking lock on
operational lock */
class OperationalLockGuard {
  CdeMutexGuard m_guard;

 public:
  explicit OperationalLockGuard(bool lockNow = true);
  void Clear();
};

class DictTableRefGuard {
  cde_dict_t *m_dictTable = nullptr;
  MDL_ticket *m_mdl = nullptr;
  THD *m_thd = nullptr;

 public:
  explicit DictTableRefGuard(cde_dict_t *dictTable) : m_dictTable(dictTable) {}
  DictTableRefGuard(cde_dict_t *dictTable, MDL_ticket *mdl, THD *thd)
      : m_dictTable(dictTable), m_mdl(mdl), m_thd(thd) {
    CDE_ASSERT_DEBUG(nullptr == m_mdl || nullptr != m_thd);
  }

  void Release();
  void Reset(cde_dict_t *dictTable) { m_dictTable = dictTable; }
  void Reset(cde_dict_t *dictTable, MDL_ticket *mdl, THD *thd) {
    m_dictTable = dictTable;
    m_mdl = mdl;
    m_thd = thd;
    CDE_ASSERT_DEBUG(nullptr == m_mdl || nullptr != m_thd);
  }

  ~DictTableRefGuard() { Release(); }
};

void DictRemoveFromEvictLRULocked(cde_dict_t *table);

/**
Removes table from gTableLRU (eviction LRU list), given table is on
this list. Complexity is O(1).

@param[in]  table   table to be removed from eviction list.
                    If table is not on the list - do nothing.
*/
void DictRemoveFromEvictLRU(cde_dict_t *table);

/**
Retrieves the table entry with the specified name.

@param[in]       tableName The name of the table to retrieve.
@param[in]       getByBgStat Whether is called by backgroud-stat thread.

@return A pointer to the cde_dict_t object representing the table, or NULL if
not found.
*/

cde_dict_t *DictSysGetTable(const char *tableName, bool getByBgStat = false);

/**
Retrieves the table entry with the specified name. The caller must hold
lock on dictSys.

@param[in]       tableName The name of the table to retrieve.
@param[in]       getByBgStat Whether is called by backgroud-stat thread.

@return A pointer to the cde_dict_t object representing the table, or NULL if
not found.
*/
cde_dict_t *DictSysGetTableLocked(const char *tableName,
                                  bool getByBgStat = false);

/**
Retrieves the table entry with the specified table id. The caller must hold
lock on dictSys.

@param[in]       tableId The id of the table to retrieve.

@return A pointer to the cde_dict_t object representing the table, or NULL if
not found.
*/
cde_dict_t *DictSysGetTableById(uint64_t tableId);

/**
Adds a new table entry to the dictionary cache.

@param[in]       tableName The name of the table to add.
@param[in]       table     Pointer to the cde_dict_t object to add.
@param[out]      ptrExist  If dict_table with same name already exists
                           in dict_cache, use this to point to it.
@param[in]       allowEviction - whether to put this on LRU eviction
                                 list.
*/
void DictSysAddTable(const char *tableName, cde_dict_t *table,
                     cde_dict_t **prtExist, bool allowEviction = true);

/**
Removes a table entry from the dictionary cache.

@param[in]       tableName The name of the table to remove.
*/
void DictSysRemoveTable(const char *tableName);

/**
Removes a table entry from the dictionary cache. The caller needs to hold
Operational Lock on dictSys.

@param[in]       tableName The name of the table to remove.
*/
void DictSysRemoveTableNoOpLock(const char *tableName);

/**
Removes a table entry from the dictionary cache. The caller needs to hold
lock on dictSys.

@param[in]       tableName The name of the table to remove.
@param[in]       lockGuard Locked guard on dictSys lock.
@param[in]       releaseDictSys whether this function can release lock on dict
sys.
@param[in]       lockOperationalLock - should be true if the caller hasn't
locked operational lock.
*/
void DictSysRemoveTableLocked(const char *tableName,
                              DictSysLockGuard &lockGuard,
                              bool releaseDictSys = true);

/**
Renames a table entry in the dictionary cache.

@param[in]       form The current table name.
@param[in]       to The new table name.
@param[in]       renameForeigns Whether to rename foreign key object.

@return 0 if success, errno if fail.
*/
int32_t DictSysRenameTable(const char *from, const char *to,
                           bool renameForeigns = true);

/**
Removes a foreign constraint struct from the dictionary cache.

@param[in]      foreign  foreign constraint
*/
void DictForeignRemoveFromCache(cde_dict_foreign_t *foreign);

void CdeGetAllRelativeTables(cde_dict_t *dict_table,
                             std::set<std::string> &tables);

/**
Retrieve database name from name.
@param[in]       name - name in format /database/table
@param[out]      dbNameSize - size of table's name or 0 if error
@return pointer to database name inside the name.
*/
const char *GetDBName(const char *name, uint32_t &dbNameSize);

/**
Retrieve table name from name.
@param[in]       name - name in format /database/table
@param[out]      tableNameSize - size of table's name or 0 if error
@return pointer to table name inside the name.
*/
const char *GetTableName(const char *name, uint32_t &tableNameSize);

[[maybe_unused]] bool GetDbAndTableNameFromDictName(const std::string &dictName,
                                                    std::string &dbNameRes,
                                                    std::string &tableNameRes);

uint32_t getDBNameSizeFkStyle(const char *name);

/**
Add the escape character to the entered database name.
eg: test.123 -> `test.123`
    test`123 -> `test``123`

@param[in]      src            database object name.

@return str added escape char.
*/
std::string EscapeStr(const std::string &src);

/**
Add the quote escape character to the entered database name.
eg: t'1 -> t''1

@param[in]      src            database object name.

@return str added quote escape char.
*/
std::string quoteEscapeStr(const std::string &src);

/**
Removes tables from cache. It removes up to 50% of the current
eviction LRU list (gTableLRU) - and only till g_dstoreTableDefitionCache
threshold is exceeded.
*/
void DictSysFreeSpaceInCache();

/** Acquire a shared metadata lock.
@param[in,out]  thd       current thread
@param[out]     mdl       metadata lock
@param[in]      dictName  dict table name
@retval true if acquired
@retval false if failed (my_error() will have been called) */
bool CdeDdMdlAcquire(THD *thd, MDL_ticket **mdl, const char *dictName);

/** Release a metadata lock.
@param[in,out]  thd     current thread
@param[in,out]  mdl     metadata lock */
void CdeDdMdlRelease(THD *thd, MDL_ticket **mdl);

/**
A guard class for acquiring and releasing the explicit MDL lock.
*/
class ExplicitMDLGuard {
  THD *m_thd;
  std::string m_tableName;

  MDL_ticket *m_mdl = nullptr;

 public:
  ExplicitMDLGuard(THD *thd, std::string tableName, bool lockNow = true) {
    m_thd = thd;
    m_tableName = tableName;
    if (lockNow) {
      Lock();
    }
  }

  ~ExplicitMDLGuard() { Unlock(); }

  bool Lock() {
    if (!CdeDdMdlAcquire(m_thd, &m_mdl, m_tableName.c_str())) {
      CDE_LOG_WARN("Get Explicit MDL lock fail for table- %s",
                   m_tableName.c_str());
      return CDE_FAIL;
    }
    return CDE_SUCC;
  }

  void Unlock() { CdeDdMdlRelease(m_thd, &m_mdl); }

  bool IsLocked() { return m_mdl != nullptr; }
};

/**
Form the table id which the high 32 bit is used to store tablespace id and the
low 32 bit is used to store heap oid.
@param[in]      tablespaceId  tablespace id
@param[in]      heapOid       heap id
@retval table id
*/
inline uint64_t CdeFormDictTableId(Oid tablespaceId, Oid heapOid) {
  return (static_cast<uint64_t>(tablespaceId) << TABLE_SPACE_ID_SHIFT) |
         heapOid;
}

} /* namespace CDE */
#endif /*__CDE_DICT_H__*/
