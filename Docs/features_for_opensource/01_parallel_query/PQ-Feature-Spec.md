# Parallel Query (PQ) Feature Specification

> Version: 2.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

MySQL executes each SELECT query using a single thread, leaving multi-core CPU resources underutilized. Large analytical queries (e.g., TPC-H) benefit significantly from parallel execution.

### 1.2 Solution

Implement a parallel query execution framework that splits the first non-const primary table into N data blocks, distributes them to worker threads, and merges results through a leader thread using message queues. The framework rewrites the single-threaded serial execution plan into a parallel execution plan after `optimize()` and before `execute()`.

### 1.3 Execution Model

```
Before parallel:
  Aggregation -> Sort -> T1 JOIN T2 JOIN T3

After parallel:
  Gather (merge)
    |            |            |
  Worker1      Worker2     WorkerN
  T1.part1     T1.part2    T1.partN
  JOIN T2,T3   JOIN T2,T3  JOIN T2,T3
```

- Only one table (the first non-const primary table) is partitioned per query block
- Each worker gets an identical copy of the execution plan but scans only its assigned block
- The leader merges worker results and performs final aggregation/sorting

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| SQL layer (`sql/parallel_query/`) | 28 | ~16,000 |
| InnoDB layer (`storage/innobase/`) | 3 | ~2,264 |
| MySQL core modifications | ~15 | ~3,000 |
| MTR tests | ~100 | ~8,000 |
| **Total** | **~46** | **~29,264** |

---

## 2. New Files

### 2.1 SQL Layer (`sql/parallel_query/`)

| File | Lines | Purpose |
|------|-------|---------|
| `sql_parallel.h/.cc` | 253/2235 | Top-level orchestration: `Gather_operator`, `PQ_worker_manager`, `MQ_record_gather`, plan rewrite entry points |
| `pq_iterators.h/.cc` | 185/764 | Iterator implementations: `ParallelScanIterator` (leader), `PQblockScanIterator` (worker), `PQRefIterator` (worker ref scan) |
| `pq_optimizer.h/.cc` | 116/1711 | Eligibility checks (`PQUnsuiteInfo` enum), `pq_support_features_switch`, pre-optimization hooks |
| `pq_clone.h/.cc` | 89/1812 | Execution plan cloning: `pq_make_join()`, `pq_dup_tabs()`, `pq_dup_order()` |
| `pq_clone_item.cc` | 4024 | Item tree cloning for worker threads |
| `pq_refix_fields_item.cc` | 532 | Fix Item references after cloning |
| `pq_replace_base_item.cc` | 111 | Replace base items in cloned query blocks |
| `pq_resolver.h/.cc` | 45/518 | Name resolution for cloned query blocks |
| `pq_handler.h/.cc` | 875/263 | Engine-agnostic data partition types: `PQ_Slice`, `PQ_Range`, `PQ_Ctx_Base`, `PQ_Scan_ctx_Base`, `Parallel_leader_Base`, `Parallel_worker`, queue/map infrastructure |
| `msg_queue.h/.cc` | 462/496 | Lock-free message queue: `MQueue`, `MQueue_handle`, `MQ_event`, batch buffer |
| `exchange.h/.cc` | 136/316 | Leader-side data exchange: `Exchange`, `Exchange_nosort` (round-robin read from MQs) |
| `exchange_sort.h/.cc` | 161/541 | Sorted exchange: `Exchange_sort`, gather merge using binary heap |
| `binary_heap.h` | 190 | Binary heap for gather merge sort |
| `query_result_mq.h/.cc` | 77/392 | Worker result sender: `Query_result_mq` (sends `record[0]` via MQ) |
| `pq_resource_stat.h/.cc` | 83/81 | Global resource tracking: thread count, memory limit, status variables |
| `barrier.h` | 142 | Thread synchronization barrier (used by hash join) |
| `bloom_filter.h` | 195 | Bloom filter (used by hash join) |
| `chunk_files_wrapper.h` | 335 | Chunk file management for on-disk hash join |
| `pq_hash_join_shared_context.h/.cc` | 153/43 | Shared context for parallel hash join |
| `explain_pq_access_path.h/.cc` | 75/376 | EXPLAIN ANALYZE timing for parallel iterators |

### 2.2 InnoDB Layer (`storage/innobase/`)

| File | Lines | Purpose |
|------|-------|---------|
| `include/row0pread_pq.h` | 289 | InnoDB parallel scan types: `Parallel_leader`, `PQ_Scan_ctx`, `PQ_Ctx`, `Iter`, `PQ_Config` |
| `row/row0pread_pq.cc` | 1178 | B+tree partitioning, parallel record reading, visibility checking |
| `handler/ha_innodb_pq.cc` | 797 | Handler integration: `pq_leader_scan_init`, `pq_worker_scan_init/next`, ref scan range building |

---

## 3. Modifications to Existing MySQL Files

### 3.1 `sql/sql_class.h` (THD class)

```c++
// New members in THD class:
enum PQ_CLONE_PHASE { PQ_PREPARE = 0, PQ_OPTIMIZE, PQ_EXECUTEION, PQ_UNKONWN };
PQ_CLONE_PHASE clone_phase{PQ_UNKONWN};
MEM_ROOT *pq_mem_root;                        // memroot for PQ allocations
THD *pq_leader;                               // non-null for worker THDs, points to leader
std::vector<Gather_operator *> pq_gathers;    // all gather operators for this query
PQ_worker_manager *pq_worker_info{nullptr};   // non-null for worker THDs
bool pq_leader_create_fake_iter;
uint pq_threads_running;                      // threads used by this statement
uint pq_dop;                                  // degree of parallelism for this statement
bool pq_error;                                // error flag
uint64 pq_current_found_rows;                 // accumulated found_rows
bool pq_executed{false};                      // has PQ been executed
bool pq_skip_fetch_ctx{false};

// Methods:
bool suite_for_parallel_query(PQUnsuiteInfo *pq_info) const;
bool is_pq_worker() const;     // return pq_leader != nullptr
bool is_pq_real_worker() const; // return pq_leader && pq_worker_info
bool is_pq_leader() const;     // return !pq_leader
bool is_pq_error() const;
bool pq_merge_status(THD *thd);
bool pq_status_reset();
void pq_copy_from(THD *thd);
bool is_in_pq_phase();
bool pq_support_features_switch_flag(ulonglong flag) const;
```

New session variables in `THD::variables`:
```
parallel_default_dop        (ulong, session)
parallel_cost_threshold     (ulong, session, hint-updateable)
parallel_queue_timeout      (ulong, session)
pq_support_features_switch  (ulonglong, session, hint-updateable)
pq_msg_queue_spin_count     (ulong, session)
parallel_batch_max_slot     (ulong, session)
parallel_batch_max_mem_size (ulong, session)
parallel_setup_cost         (double, session, hint-updateable)
parallel_tuple_cost         (double, session, hint-updateable)
parallel_rows_threshold     (ulong, session)
parallel_graceful_fallback  (bool, session)
```

### 3.2 `sql/sql_lex.h` (Query_block / Query_expression)

```c++
// In Query_block:
bool parallel_exec{false};                        // true if this block uses PQ
PQUnsuiteInfo pq_unsuite_info{INFO_NONE};        // reason PQ was disabled
bool pq_try_clone_item{false};                   // enable item cloning for PQ
bool disable_distinct_in_pq_worker{false};       // skip DISTINCT on worker
Query_block *m_pq_last_clone{nullptr};           // linked list of cloned blocks
Query_block *m_pq_is_clone_of{nullptr};          // reverse link
PQ_shared_info *m_pq_shared_info{nullptr};       // shared temp table info

// Methods:
bool suite_for_parallel_query(THD *thd);
void check_suite_for_pq_after_prepare(THD *thd);
bool pq_check_table_list();
void pq_link_clone(Query_block *clone);
void pq_unlink_clone();
bool pq_is_clone() const;
void pq_backup();
void pq_restore();

// In Query_expression:
enum PQSubqueryExecution { PQ_EXE_LEADER, PQ_EXE_WORKER, PQ_EXE_NONE };
PQSubqueryExecution subquery_suite_for_parallel_query() const;
```

### 3.3 `sql/sql_optimizer.h` (JOIN class)

```c++
// In JOIN:
bool need_tmp_pq_leader{false};       // leader needs tmp table
PQ_optimized_var saved_optimized_vars; // saved optimization state for workers
bool pq_rebuilt_group{false};
bool pq_stable_sort{false};
int pq_last_sort_idx{-1};
bool pq_pushdown_having{false};

// Methods:
bool check_pq_select_fields();
bool choose_parallel_tables(bool do_mark = false);
bool suite_for_parallel_query();
bool check_pq_support_features_switch();
bool is_suit_for_pq_based_cost();
bool pq_copy_from(JOIN *orig);
void pq_restore();
```

#### PQ_optimized_var Structure

Saved before plan rewrite, restored on leader for secondary aggregation:

```c++
struct PQ_optimized_var {
  bool pq_grouped;
  bool pq_group_optimized_away;
  bool pq_implicit_grouping;
  bool pq_need_tmp_before_win;
  bool pq_simple_group;
  bool pq_simple_order;
  bool pq_streaming_aggregation;
  uint pq_m_ordered_index_usage;   // cast to ORDERED_INDEX_USAGE
  bool pq_skip_sort_order;
  bool pq_select_distinct;
  std::vector<bool> optimized_group_flags;  // which GROUP BY items were optimized away
  std::vector<bool> optimized_order_flags;  // which ORDER BY items were optimized away
  Item *pq_saved_having_cond;               // saved HAVING before pushdown decision
};
```

### 3.4 `sql/handler.h` (handler class)

```c++
// In handler:
bool pq_reverse_scan{false};
bool pq_ref_depend{false};
bool pq_ref{false};
bool pq_table_scan{false};
bool do_parallel_scan{false};
uint pq_range_type{0};
key_range pq_ref_key{};
PQ_shared_info m_shared_info{};
void *pq_ctx{nullptr};

// Virtual methods (overridden by ha_innobase / ha_innopart):
virtual int pq_leader_scan_init(uint keyno, ...);
virtual int pq_worker_scan_init(uint keyno, ...);
virtual int pq_ref_build_ranges(void *scan_ctx, ...);
virtual int pq_worker_scan_next(void *scan_ctx, uchar *buf);
virtual void set_shared_info(PQ_shared_info *info);
```

### 3.5 Other Modified Files

- `sql/sql_select.cc`: Plan execution flow, parallel plan switching
- `sql/sql_union.cc`: `make_pq_unit_plan()` integration in UNION execution
- `sql/iterators/composite_iterators.cc`: MaterializeIterator and AggregateIterator PQ awareness
- `sql/join_optimizer/access_path.cc`: `PQBlockScanAccessPath`, `PQRefScanAccessPath`, `EstimatePQGatherOperatorCost()`
- `sql/opt_explain.cc`: `explain_pq_gather()` for EXPLAIN output
- `sql/opt_explain_traditional.cc`: `ET_PARALLEL_EXE` Extra tag
- `sql/sys_vars.cc`: All PQ system variable definitions
- `sql/sql_base.cc`: Table open/close hooks for PQ
- `sql/mysqld.cc`: Global variable initialization
- `sql/conn_handler/connection_handler_perthread.cc`: PQ thread creation

---

## 4. Core Data Structures

### 4.1 Gather_operator

Central coordinator for one parallel query block. Owns the template JOIN, worker managers, and exchange infrastructure.

```c++
class Gather_operator {
  uint m_dop;                                          // degree of parallelism
  JOIN *m_template_join;                               // template for worker plan cloning
  PQ_worker_manager **m_workers;                       // per-worker state
  PQTab pq_tabs[END_TAB];                              // parallel-scanned tables
  std::set<PQTabType> tab_set{DIV_TAB};               // table type set
  int m_ha_err;
  Diagnostics_area m_stmt_da;                          // worker DA
  CODE_STATE **m_code_state;
  uint pq_check_fields;                                // field count check
  uint pq_check_reclen;                                // record length check
  unique_ptr_destroy_only<HashJoin::PQHashJoinSharedContext>
      m_pq_hash_join_shared_context;                   // shared hash join state
  Mem_root_array<Item_subselect *> m_uncorrelated_subqueries;
  std::set<const ORDER *> m_desc_groups;              // descending group indexes

  // Methods:
  void prepare();    // SetupPQTab for each tab in tab_set
  bool init();       // InitPQTab: calls handler->ha_pq_init() to partition data
};
```

#### PQTab Structure

Per-table parallel scan configuration stored in `Gather_operator::pq_tabs[]`:

```c++
enum PQTabType { DIV_TAB = 0, CUT_TAB, END_TAB };

struct PQTab {
  QEP_TAB *m_tab;        // QEP_TAB of the parallel-scanned table
  TABLE *m_table;         // TABLE object
  uint keyno;             // index number used for parallel scan
  bool table_scan;        // true if full table/index scan
  void *m_pq_ctx;         // PQ scan context from handler
};
```

### 4.2 PQ_worker_manager

Per-worker state and lifecycle management.

```c++
enum PQ_worker_state {
  INIT     = 0,
  READY    = 1,      // worker finished plan setup, ready to execute
  COMPELET = 2,      // worker finished execution successfully (note: typo in original)
  ERROR    = 4,      // worker encountered error
  OVER     = 8       // worker execution ended (after COMPELET or ERROR)
};

class PQ_worker_manager {
  Gather_operator *m_gather;
  THD *thd_leader;
  THD *thd_worker;
  PQ_worker_state m_status;       // INIT/READY/COMPELET/ERROR/OVER
  my_thread_handle thread_id;
  bool m_active{false};
  MQueue_handle *m_handle;        // this worker's MQ handle
  int worker_index;
  mysql_mutex_t m_mutex;          // protects status transitions
  mysql_cond_t m_cond;

  bool wait_for_status(THD *thd, uint state);  // wait with 5s timeout
  void signal_status(THD *thd, PQ_worker_state state);
  void set_kill_state(THD::killed_state state); // only if worker is READY
};
```

**Status lifecycle:**
```
INIT → (worker signals after plan+snapshot ready) → READY
READY → (worker finishes execution) → COMPELET or ERROR
COMPELET/ERROR → (worker cleanup done) → OVER
```

### 4.3 Parallel_leader_Base / PQ_Scan_ctx_Base / PQ_Ctx_Base

Engine-agnostic data partition infrastructure. InnoDB provides concrete implementations.

```
Parallel_leader_Base           (manages B+tree partitioning, owns slices map)
  └─ Parallel_leader (InnoDB)  (adds trx_t*, makes PQ_Scan_ctx)

PQ_Scan_ctx_Base               (one per scan range, owns partition logic)
  └─ PQ_Scan_ctx (InnoDB)      (adds trx_t*, mtr, B+tree traversal)

PQ_Ctx_Base : PQ_Slice         (one per data block, reads records)
  └─ PQ_Ctx (InnoDB)           (adds btr_pcur_t*, record reading)
  └─ PQ_Ctx_intrinsic          (for intrinsic tables, range checking)
```

### 4.4 MQueue / MQueue_handle

Lock-free ring buffer for worker→leader data transfer.

```c++
class MQueue {
  MQ_event *m_sender_event;
  MQ_event *m_receiver_event;
  uint64 m_bytes_written;    // write position (atomic)
  uint64 m_bytes_read;       // read position (atomic, batch-updated)
  char *m_buffer;            // ring buffer
  uint32 m_ring_size;        // buffer size (power of 2)
  MQ_DETACHED_STATUS detached;
};

class MQueue_handle {
  MQueue *m_queue;
  char *m_buffer;
  uint32 m_consume_pending;  // batched read bytes
  uint32 m_partial_bytes;    // partial message state
  uint32 m_expected_bytes;
  bool m_length_word_complete;
  bool m_read_done;
  Batch_buffer_manager *m_ext_mgr;

  MQ_RESULT send(Field_raw_data *fm);
  MQ_RESULT send(void *datap, uint32 len, bool nowait = false);
  MQ_RESULT receive(void **datap, uint32 *, bool nowait = true);
};
```

### 4.5 Exchange (Leader-side data receiver)

```c++
class Exchange {
  THD *m_thd;
  TABLE *m_table;
  mem_root_deque<Item *> *m_recv_items;
  uint32 m_nqueues;
  MQueue_handle **mqueue_handles;   // one per worker
  bool m_stab_output;               // stable sort needed for index scan
  bool m_first_record;

  enum EXCHANGE_TYPE { EXCHANGE_NOSORT, EXCHANGE_SORT };
  virtual bool init();
  virtual bool read_mq_record() = 0;
  virtual bool convert_mq_data_to_record(uchar *data, int msg_len, uchar *row_id);
};

class Exchange_nosort : public Exchange {  // round-robin from all workers
  int m_active_readers;
  int m_next_queue;
};

class Exchange_sort : public Exchange {     // binary heap merge from workers
  Binary_heap m_heap;
  // ... merge sort state
};
```

### 4.6 Query_result_mq

Worker's result sender. Replaces the normal `Query_result_send` on worker threads.

```c++
class Query_result_mq : public Query_result {
  TABLE *m_table;
  Temp_table_param *m_param;
  JOIN *m_join;
  MQueue_handle *m_handler;
  Field_raw_data *mq_fields_data;
  bool *mq_fields_null_array;
  char *mq_fields_null_flag;
  bool m_stable_output;
};
```

### 4.7 Field_raw_data

Describes one field's data for MQ transfer. Supports NULL and CONST optimization.

```c++
struct Field_raw_data {
  uchar *m_ptr;       // raw data pointer
  uint32 m_len;       // data length to send
  uchar m_var_len;    // varstring length bytes (0 for fixed-length)
  bool m_need_send;   // false if NULL or CONST
};
```

Message format on MQ:
```
[total_len: 4B][null_len: 2B][null_flag: null_len bytes][field_data...]
```

Each field uses 2 bits in null_flag:
- `00`: NOT_CONST, NOT_NULL (send field data)
- `01`: NOT_CONST, NULL (skip field data)
- `10`: CONST, NOT_NULL (skip, use cached value)
- `11`: CONST, NULL (skip)

---

## 5. Execution Flow

### 5.1 Eligibility Check (before optimization)

```
JOIN::optimize()
  → JOIN::suite_for_parallel_query()
      → THD::suite_for_parallel_query()     // session-level checks
      → JOIN::choose_parallel_tables()       // RBO: cost, table scan type
      → JOIN::check_pq_support_features_switch()
  → Query_block::suite_for_parallel_query()
      → TABLE-level checks (engine, temp table, etc.)
```

**THD-level checks** (`THD::suite_for_parallel_query`):
- Must be SELECT statement
- Not in stored procedure, trigger, or attachable transaction
- Not SERIALIZABLE isolation level
- Not read-only for write queries
- Not exceeding `parallel_max_threads` or `parallel_memory_limit`

**JOIN-level RBO** (`JOIN::choose_parallel_tables`):
- Query cost must exceed `parallel_cost_threshold`
- At least one non-const primary table exists
- No `zero_result_cause`
- No `SELECT DISTINCT` with certain conditions
- No ROLLUP
- No semi-join with Materialization or DuplicateWeedout strategy
- No `only_full_group_by` mode
- Field count ≤ `MAX_FIELDS`
- No `LIMIT` without `ORDER BY`
- No unsupported data types or functions

**PQUnsuiteInfo enum** provides ~40 specific reasons for PQ disqualification (see `pq_optimizer.h`).

### 5.2 Complete PQUnsuiteInfo Reason Strings

These 40 reason strings are used in optimizer trace and debugging to explain why PQ was not chosen:

```c++
static const char *PQ_UNSUITABLE_INFO[] = {
    "Internal conversion failed.",                         // INFO_NONE
    "Internal check failed.",                              // INTER_UNSUITE
    "Error happened during save properties or pq clone.",  // INTER_ERROR
    "Query with IN/ANY/ALL/SOME clause may transform to subquery which "
    "pq does not support.",                                // SUBQUERY_TRANS
    "PQ only supported SELECT/INSERT_SELECT/REPLACE_SELECT command.",  // UNSUPPORT_COMMAND
    "Query is store procedure or trigger or prepare statement or "
    "attachable transaction or serializable with locking reads.",  // PREPARE_TRIGER_PROCEDURE
    "Parallel hash_join_spill_to_disk is off and the expected number of "
    "re-fill hash table exceeds its upper limit.",         // HASH_JOIN_SPILL
    "Table list has system table/transactional tmp table/system tmp table "
    "or tables have same alias.",                          // SYS_OR_TMP_TABLE
    "Query has named view or a derived table which is outer-correlated. PQ "
    "leader could not materialize it.",                    // VIEW_OR_DERIVED_TABLE
    "Query has table function or explicit locking clause.",  // TABLE_FUNCTION_OR_LOCK
    "Query cached the result set(such as with SQL_BUFFER_RESULT or insert "
    "select into the same table) or with rollup or more than one value "
    "item of table value constructor or has windows function.",  // ROLLUP_WINDS_BUFFER
    "parallel insert_select is off or binlog isn't row based.",  // SWITCH_INSERT_SELECT
    "Query has unsupported scenarios including generated column, "
    "unsupported data type, unsupported function and so on.",  // UNSUPPORT_FUNCTION_DATATYPE
    "Item has unsupported ref type",                       // UNSUPPORT_REF_TYPE
    "Item has a merged {view or derived table}'s column which wraps a "
    "subquery, and is referenced by another subquery",     // REF_WITH_SUBQUERY
    "Item has aggregate in top query which referenced by a subquery "
    "through an alias",                                    // OUTREF_WITH_SUMITEM
    "Query has AGGREGATE(DISTINCT) and has blob in aggregation.",  // AGGR_DISTINCT
    "Query has aggregate which references outer column and is computed in "
    "the subquery.",                                       // SUMITEM_OUTREF
    "Query has unsupported subquery-type.",                // UNSUPPORTED_SUBQUERY_TYPE
    "Query's cost less than parallel_cost_threshold or query has lateral "
    "derived tables or number of fields are more than MAX_FIELDS or rollup "
    "state does not support.",                             // COST_OR_LATERAL
    "PQ message queue is saturated.",                      // QUEUE_SATURATED
    "PQ does not support for group-by with indexing scan when there are "
    "few records in each group.",                          // SCAN_RECORDS
    "Query has zero result or primary tables deemed constant.",  // ZERO_RESULT
    "Query have ORDER BY<correlated subquery> and it doesn't cached.",  // ORDER_BY_SUBQUERY
    "No table meets the conditions for splitting to parallel scanning. "
    "Possible table's fetched row less than parallel_rows_threshold or "
    "query includes unsupported scan-type or engine/multiple partitions to scan "
    "with partition table/a semi-join inner table/inner_table in BKA or "
    "outer join.",                                         // NO_DIVIDED_TABLE
    "Query does not comply with the ONLY_FULL_GROUP_BY rule which is "
    "required by PQ.",                                     // ONLY_FULL_GROUP_BY
    "Failed to execute the query using parallel query. The query was "
    "restarted without parallel query.",                   // RETRY_WITHOUT_PQ
    "Trx is rolling back",                                // ROLLBACK
    "Subquery will run single-threaded inside the parent query block's worker.",  // SUB_MTIP_WORKER
    "PQ memory size exceed parallel_memory_limit.",       // MEMORY_LIMIT
    "The number of remaining idle threads for PQ (parallel_max_threads - "
    "number of used threads) is less than parallel_default_dop.",  // IDLE_THREAD
    "Query does not meet the requirements of PQ support features switch, "
    "Check switch in pq_support_features_switch variable.",  // DISABLED_IN_PQ_SUPPORT_FEATURES
    "Query has enabled offset pushdown, so PQ cannot be enabled "
    "simultaneously.",                                     // OFFSET_PUSHDOWN_PRIO
    "The cost of the parallel query execution plan is higher than"
    " that of the serial query execution plan.",           // PQ_COST_HIGHER
    "Count(distinct) does not support PQ when JOIN::need_tmp_before_win is "
    "true.",                                               // COUNT_DISTINCT_NOT_SUPPORT_NEED_TEMP
    "Disable PQ execution when the query has a LIMIT clause but no ORDER BY"
    " clause.",                                            // LIMIT_NO_ORDERBY
    "No support INTERSECT and EXCEPT table operators.",    // UNSUPPORTED_INTERSECT_AND_EXCEPT
    "There are fields with the same name in the temporary table.",  // TMP_TABLE_FIELD_NAME_SAME
    "Query block implicitly generate aggregate function after prepare stage.",  // IMPLICIT_AGG_AFTER_PREPARE
    "The HAVING expression within the subquery references an alias from the "
    "outer query.",                                        // HAVING_WITH_OUTER_REF_IN_SUBQUERY
    "Disable PQ if access table is not InnoDB",           // ONLY_SUPPORT_INNODB
    "Query has NO_PQ hint or pq_master_enable is off or PQ is not supported in "
    "some scenarios.",                                     // NO_PQ
    "Force_parallel_execute is off or parallel_default_dop is zero.",  // ZERO_DOP
    "Use hypergraph optimizer, PQ not support yet"        // HYPERGRAPH_OPTIMIZER
};
```

### 5.3 Plan Rewrite (after optimize, before execute)

#### make_pq_unit_plan — Entry Point for UNION and Simple Queries

```c++
PQ_exec_status make_pq_unit_plan(Query_expression *unit, THD *thd) {
  if (unit->is_simple()) {
    join = unit->first_query_block()->join;
    if (join && join->suite_for_parallel_query())
      status = make_pq_leader_plan(join, thd);
  } else if (unit->is_union_all_of_simple()) {
    for (each select_lex in unit) {
      if (join && join->suite_for_parallel_query())
        status = make_pq_leader_plan(join, thd);
    }
  }
  // For INSERT SELECT: temporarily remove insert table from name resolution
  unit->create_access_paths(thd);
}
```

#### make_pq_leader_plan — Detailed Step-by-Step

This is the core function that rewrites a serial plan into a parallel plan. Called from `sql/sql_union.cc` or `sql/sql_select.cc`.

```c++
PQ_exec_status make_pq_leader_plan(JOIN *join, THD *thd) {
  // Step 1: Backup serial plan for graceful fallback
  backup_leader_plan(thd, graceful_fallback, join);
  //   → join->query_block->pq_backup()
  //   → In debug: save thd->mem_root digest for verification

  // Step 2: Switch to PQ mem_root for all subsequent allocations
  thd->mem_root = thd->pq_mem_root;

  // Step 3: Mark parallel-scanned tables
  for (i = const_tables..primary_tables) {
    if (qep_tab[i].pq_div_tab) { idx_div_tab = i; do_parallel_scan = true; }
    if (qep_tab[i].pq_cut_tab) { idx_cut_tab = i; do_parallel_scan = true; }
  }

  // Step 4: Check stable sort requirement
  join->pq_stable_sort = pq_check_stable_sort(join);

  // Step 5: Create Gather_operator (includes template JOIN clone)
  gather = make_pq_gather_operator(join, thd->pq_dop);

  // Step 6: For EXPLAIN, rewrite access path costs on template join
  if (thd->lex->is_explain())
    pq_rewrite_full_access_path_cost(thd, join, gather->m_template_join->m_root_access_path);

  // Step 7: After this point, no graceful fallback (see note below)
  graceful_fallback = false;
  exec_code = PQ_exec_status::ABORT_EXEC;

  // Step 8: Create leader's rewritten QEP_TAB (the <gatherN> pseudo-table)
  join->make_leader_rewritten_tab();

  // Step 9: Attach gather to leader's first QEP_TAB
  tab = join->qep_tab;
  tab->gather = gather;
  thd->pq_gathers.push_back(gather);

  // Step 10: Mark descending groups for merge sort
  mark_desc_groups(join, gather);

  // Step 11: Restore optimized vars and create leader's tmp table
  join->restore_optimized_vars();  // restore GROUP BY/ORDER BY, decide HAVING pushdown
  join->make_leader_tmp_table();
  join->make_tmp_tables_info();

  // Step 12: Update m_accum_properties on PQ tmp fields
  for (item : join->tmp_fields[REF_SLICE_PQ_TMP])
    item->update_used_tables();

  // Step 13: Create leader's access paths
  join->create_access_paths();

  // Step 14: Invalidate plan cache (PQ wins over plan cache)
  plan_cache::invalidate_cached_plan(join->query_block);
  plan_cache::set_uncacheable(join->query_block);

  return PQ_exec_status::PARL_EXEC;

err:
  // Error: free gather, restore leader plan from backup
  pq_free_gather(gather);
  restore_leader_plan(thd, graceful_fallback, orig_digests, join);
  //   → join->qep_tab = join->qep_tab0 (serial plan)
  //   → join->pq_restore()
  //   → In debug: verify thd->mem_root digest unchanged
}
```

**Note on graceful fallback**: Fallback only covers the clone/refix_fields phase because:
1. RBO rules in clone/refix_fields make fallback valuable
2. Leader plan modifications after that point involve many dynamic data structures (vector/deque) that cannot be reliably backed up
3. Memory consistency checks (`CalcMemDigest()`) can only verify `MEM_ROOT` allocations, not dynamic containers

#### make_pq_gather_operator — Template JOIN Creation

```c++
Gather_operator *make_pq_gather_operator(JOIN *join, uint dop) {
  // 1. Create a new THD for template join (pq_new_thd)
  THD *new_thd = pq_new_thd(join->thd);
  new_thd->pq_leader = thd;
  new_thd->has_pq = true;

  // 2. Clone JOIN as template
  template_join = pq_make_join(new_thd, join);
  pq_dup_tabs(template_join, join, true);
  template_join->need_tmp_pq = true;

  // 3. For each cloned query block, set up tmp table info
  query_blocks_to_clone = list_query_blocks_to_clone(template_join->query_block);
  for (auto query_block : query_blocks_to_clone) {
    join2->setup_tmp_table_info(orig_join2);
    join2->make_tmp_tables_info();
  }

  // 4. Restore old THD as current
  thd->store_globals();

  // 5. Create Gather_operator
  gather_opr = new (thd->pq_mem_root) Gather_operator(dop, thd->pq_mem_root);

  // 6. Collect uncorrelated subqueries for later execution by leader
  WalkQueryBlock(join->query_block, [&gather_opr](Item *item) {
    if (item->type() == Item::SUBSELECT_ITEM) {
      auto subq_item = down_cast<Item_subselect *>(item);
      if (!subq_item->is_uncacheable())
        gather_opr->m_uncorrelated_subqueries.push_back(subq_item);
    }
  });

  // 7. Set up PQTab entries for each parallel-scanned table
  gather_opr->m_template_join = template_join;
  for (auto tabType : gather_opr->tab_set) {
    auto idx = (tabType == DIV_TAB) ? join->idx_div_tab : join->idx_cut_tab;
    gather_opr->pq_tabs[tabType].m_tab = &join->qep_tab[idx];
    gather_opr->pq_tabs[tabType].m_table = join->qep_tab[idx].table();
  }

  // 8. Allocate worker manager array
  gather_opr->m_workers = thd->pq_mem_root->ArrayAlloc<PQ_worker_manager *>(dop);
  for (i = 0..dop) {
    gather_opr->m_workers[i] = new PQ_worker_manager();
    gather_opr->m_workers[i]->m_gather = gather_opr;
    gather_opr->m_workers[i]->thd_leader = thd;
  }

  // 9. Prepare PQTab (SetupPQTab per table type)
  gather_opr->prepare();

  // 10. Generate access paths for template join
  for (auto query_block : query_blocks_to_clone) {
    pq_make_join_readinfo(join2, (join2 == template_join) ? gather_opr : nullptr, false);
    query_block->pq_unlink_clone();
  }
}
```

#### SetupPQTab — Per-Table Scan Configuration

Determines the index key and scan mode for each parallel-scanned table:

```c++
void SetupPQTab(PQTab *pqTab, QEP_TAB *tab, JOIN *join) {
  join_type type = tab->type();
  switch (type) {
    case JT_ALL:
      // InnoDB: table scan = primary key scan
      pqTab->keyno = table->s->primary_key;
      if (tab->range_scan() && ordered_index_usage != VOID)
        pqTab->keyno = used_index(tab->range_scan());  // e.g., GROUP BY uses index
      pqTab->table_scan = true;
      break;
    case JT_RANGE:
      pqTab->keyno = used_index(tab->range_scan());
      break;
    case JT_REF:
      table->file->pq_ref_key = {key_buff, keypart_map, key_length, HA_READ_KEY_OR_NEXT};
      table->file->pq_ref = true;
      pqTab->keyno = tab->ref().key;
      break;
    case JT_INDEX_SCAN:
      pqTab->keyno = tab->index();
      break;
  }
}
```

#### InitPQTab — Handler-Level Data Partitioning

Called during `Gather_operator::init()` at execution time to partition the table data:

```c++
int InitPQTab(PQTab *pqTab, uint dop) {
  // For range scan: set up MRR and reverse scan flags
  if (range_scan && type == JT_RANGE) {
    table->file->pq_range_type = PQ_RANGE_SELECT;
    if (reverse) {
      ReverseIndexRangeScanIterator->shared_reset();
      table->file->ha_set_reverse_scan(true);
    } else {
      IndexRangeScanIterator->shared_reset();
    }
  }

  // Call handler to partition data
  int error = table->file->ha_pq_init(dop, pqTab->keyno);
  pqTab->m_pq_ctx = table->file->pq_ctx;
}
```

#### make_leader_rewritten_tab — Creating the <gatherN> QEP_TAB

Creates a new QEP_TAB for the leader that replaces the parallel-scanned table with a message queue receiver:

```c++
bool JOIN::make_leader_rewritten_tab() {
  // 1. Allocate new qep_tab1 and ref_items1 on pq_mem_root
  alloc_qep1(tables);
  alloc_indirection_slices1();

  // 2. Set up the gather pseudo-table entry
  QEP_TAB *tab = &qep_tab[0];
  tab->pq_div_tab = true;
  tab->set_position(pq_copy from original div_tab position);
  tab->set_type(JT_ALL);

  // 3. Create Table_ref with <gatherN> alias for EXPLAIN
  Table_ref *tbl = new Table_ref();
  snprintf(buff, 64, "<gather%u>", query_block->select_number);
  tbl->alias = buff;
  tbl->table_name = div_table->s->table_name;
  tbl->db = div_table->s->db;

  // 4. Mark as message-receiving tmp table
  tab->set_split_table(div_table);
  tab->op_type = QEP_TAB::OT_RECV_MSG_TMP_TABLE;

  // 5. Reset ref_items and table counts
  ref_items[REF_SLICE_ACTIVE] = query_block->base_ref_items;
  tmp_tables = primary_tables = const_tables = 0;
  need_tmp_pq_leader = true;
}
```

### 5.4 Worker Execution — Complete Lifecycle

```
ParallelScanIterator::Init()
  → para_exec_init()                   // materialize shared temp tables
  → exec_scalar_uncorrelated_subquery()  // leader executes uncorrelated subqueries
  → pq_init_record_gather()            // create Exchange, set worker MQ handles
  → gather->init()                     // InnoDB data partitioning
  → pq_create_innodb_snapshot()        // leader's read view
  → pq_launch_worker()                 // create and launch worker threads

pq_launch_worker():
  for each worker i:
    mysql_thread_create(pq_worker_exec, workers[i])
    workers[i]->wait_for_status(READY | COMPELET | ERROR | OVER)

pq_worker_exec(void *arg):
  // Phase 1: Plan creation
  1. make_pq_worker_plan(mngr)
     → pq_new_thd(template_join->thd)        // clone THD
     → pq_make_join(new_thd, template_join)   // clone JOIN
     → pq_dup_tabs()                          // clone QEP_TABs
     → setup_tmp_table_info + make_tmp_tables_info
     → pq_make_join_readinfo(join, gather, true)
     → Create Query_result_mq as worker's result sink

  // Phase 2: Snapshot setup
  2. pq_clone_innodb_snapshot(thd, leader_thd)  // share leader's read view
     Only if query block contains InnoDB non-temp tables

  // Phase 3: Signal ready
  3. mngr->signal_status(thd, PQ_worker_state::READY)

  // Phase 4: Execute
  4. join->query_expression()->ExecuteIteratorQuery(thd)

  // Phase 5: Signal completion
  5. mngr->signal_status(thd, PQ_worker_state::OVER)

  // Error path:
  err:
    if (res):  // execution failed
      leader_thd->pq_error = true
      msg_handler->send_exception_msg(ERROR_MSG)

    // Cleanup
    msg_handler->set_detached_status(MQ_HAVE_DETACHED)
    result->cleanup(); destroy(result)
    pq_free_join(thd, join)

    // Merge error info to template_join's THD
    temp_thd->pq_merge_status(thd)
    mngr->m_gather->m_template_join->examined_rows += thd->examined_rows

    // Propagate errors
    if (thd->is_error())
      temp_thd->raise_condition(...)

    pq_free_thd(thd)
    mngr->signal_status(NULL, COMPELET or ERROR)
    my_thread_exit(0)
```

Worker sends each row:
```
Query_result_mq::send_data()
  → collect Field_raw_data from record[0]
  → MQueue_handle::send(Field_raw_data[])
```

### 5.5 Leader Data Collection

```
ParallelScanIterator::Read()
  → MQ_record_gather::mq_scan_next()
      → Exchange::read_mq_record()
          → Exchange_nosort: round-robin read from worker MQs
          → Exchange_sort: binary heap merge from worker MQs
      → Exchange::convert_mq_data_to_record()
          → Parse null_flag, copy field data into leader's record[0]

ParallelScanIterator::End()
  → pq_wait_workers_finished()
     → Detach all MQ handles (leader stops reading)
     → Wait for each worker: wait_for_status(COMPELET | ERROR)
     → my_thread_join for each worker
  → pq_error_code()
     → Check HA_ERR_TABLE_DEF_CHANGED
     → Merge template THD's diagnostic area into leader
     → Update examined_rows from template_join
     → If error: raise_condition on leader
     → If no explicit error but pq_error flag: ER_PARALLEL_QUERY_ERROR
```

### 5.6 Aggregation

```
Worker: partial aggregation on data partition
  → SUM/COUNT: computed partially
  → AVG: stored as extended format [sum_bytes][8-byte count]
  → Inter-aggregation expressions (SUM(a)-SUM(b)): excluded from worker's fields_list
  → HAVING: set to NULL on worker if it contains aggregation

Leader: secondary aggregation on merged results
  → pq_build_sum_funcs(): rebuild aggregation function pointers from tmp table fields
  → SUM/COUNT: re-aggregate by summing partial results
  → AVG: compute from merged SUM and COUNT
  → pq_replace_avg_func(): switch AVG items to PQ_LEADER mode (extended format)
  → HAVING: evaluate after secondary aggregation
```

**AVG handling detail**: `Item_sum_avg::pq_avg_type` is set to `PQ_LEADER` on the leader. This causes the AVG to:
1. Read the extended format from worker output (sum + count)
2. `init_store_info()` adjusts the field length to accommodate the extended format
3. `resolve_type()` recalculates the result type for the extended storage

**pq_build_sum_funcs** iterates over the leader's temp table fields. For each field that has `item_sum_ref` (pointing to the original aggregate), it:
1. Calls `pq_rebuild_sum_func()` to create a new aggregate Item
2. Sets `sum_func->orig_func = item_ref` (links to worker's partial result)
3. Calls `refix_fields()` to fix field references in the new aggregate
4. Replaces the temp table field with the new aggregate in the fields list

### 5.7 ORDER BY / Gather Merge

```
Worker: sort local partition using SortingIterator
  → Copy ORDER list, resolve, generate optimized sort
  → Send sorted results to MQ

Leader: merge sort (gather merge) using binary heap
  → Binary_heap with one slot per worker
  → Pop smallest, read next from that worker, sift down
  → For GROUP BY + ORDER BY: if group-by uses sorting, gather merge
    produces globally sorted group results

pq_make_filesort(): construct sort fields on leader for:
  - GROUP BY with index scan → restore saved_join_group_list
  - ORDER BY with index scan → restore saved_join_order or extract from index key
  - Implicit index scan (covering index) → set_key_order() from index key fields

mark_desc_groups(): detect GROUP BY items that use descending index
  → Only needed when pq_rebuilt_group is true (streaming aggregation on leader)
  → For each group item: if direction not already set, check if corresponding
    key part has HA_REVERSE_SORT flag
  → Skip items that also appear in ORDER BY (their direction follows ORDER BY)
  → Store in gather->m_desc_groups for Exchange_sort to handle correctly
```

### 5.8 HAVING Pushdown

HAVING can be conditionally pushed to workers to reduce data transfer:

```c++
void JOIN::set_push_down_having() {
  // Cannot push if:
  // 1. HAVING contains aggregation (workers produce partial results, not final)
  // 2. HAVING contains grouping functions
  if (!having_cond || having_cond->has_aggregation() ||
      having_cond->has_grouping_func()) {
    pq_pushdown_having = false;
    return;
  }

  // Cannot push if: select has aggregation but no GROUP BY
  // Reason: AggregateIterator outputs a row even with 0 input rows.
  // If HAVING is pushed, leader wouldn't filter this empty-row case.
  if (has_agg && !grouped && !group_optimized_away) {
    pq_pushdown_having = false;
    return;
  }

  pq_pushdown_having = true;
}
```

When pushed:
- **Leader**: `having_cond = nullptr` (leader relies on workers having filtered)
- **Worker**: uses `query_block->having_cond()` (the original HAVING condition)

When not pushed:
- **Leader**: uses `pq_saved_having_cond` (evaluated after secondary aggregation)
- **Worker**: `having_cond = nullptr` (workers send all rows to leader)

### 5.9 CBO Cost-Based PQ Decision

After RBO passes, a cost-based check determines whether parallel execution is actually beneficial:

```c++
bool JOIN::is_suit_for_pq_based_cost() {
  // 1. If plan has dedup operators with unreliable row estimates, prefer PQ
  if (is_untrust_full_access_path_row_nums(m_root_access_path)) return true;

  // 2. Get serial plan cost and row count
  double seq_cost = get_full_access_path_cost(m_root_access_path);
  double seq_rows_num = get_full_access_path_row_nums(m_root_access_path);

  // 3. Quick check: if serial cost < threshold, use PQ anyway
  //    (the parallel scan cost already exceeded threshold, so serial cost
  //     being lower means it's distorted)
  if (check_serial_execution_cost(thd, seq_cost, seq_rows_num)) return true;

  // 4. Rewrite access path costs for parallel plan
  PathCosts orig_costs;
  pq_rewrite_full_access_path_cost(thd, this, m_root_access_path, &orig_costs);
  //   → Replaces TABLE_SCAN/REF/etc. with PQ_BLOCK_SCAN/PQ_REF_SCAN
  //   → Recalculates costs for each AccessPath type

  // 5. Get parallel plan cost
  double exchange_rows_num = get_full_access_path_row_nums(m_root_access_path) * thd->pq_dop;
  double parl_cost = get_full_access_path_cost(m_root_access_path);

  // 6. Restore original serial access path costs
  pq_resore_full_access_path_cost(m_root_access_path, orig_costs);

  // 7. Handle edge cases: untrustworthy costs → prefer PQ
  if (parl_cost < 0.0 || seq_cost < 0.0 || parl_cost > seq_cost) return true;

  // 8. Add PQ overhead: tuple transfer cost + setup cost
  parl_cost += exchange_rows_num * parallel_tuple_cost + parallel_setup_cost;

  // 9. Final decision
  return seq_cost >= parl_cost;
}
```

**check_serial_execution_cost formula**:

```
min_rows_num = (parallel_cost_threshold - (parallel_setup_cost +
                parallel_cost_threshold / parallel_default_dop)) /
               parallel_tuple_cost

If min_rows_num < 0: PQ not beneficial → return false
If rows_num < min_rows_num: PQ beneficial → return true
Otherwise: continue with full cost comparison
```

### 5.10 Access Path Cost Rewriting for PQ

When evaluating PQ cost, access paths are rewritten to reflect parallel scan costs:

```c++
bool pq_rewrite_full_access_path_cost(THD *thd, JOIN *join, AccessPath *path,
                                      PathCosts *orig_costs) {
  // Walk all access paths, backup original costs, and rewrite
  WalkAccessPathsProxy(path, [...](AccessPath *p) {
    // Step 1: Backup original cost
    orig_costs->push_back(*p);

    // Step 2: Replace scan paths with PQ variants
    replace_with_pq_table_access_path(thd, p);
    //   TABLE_SCAN → PQ_BLOCK_SCAN
    //   INDEX_SCAN → PQ_BLOCK_SCAN
    //   REF (non-constant lookup) → PQ_REF_SCAN
    //   REF (constant lookup) → PQ_BLOCK_SCAN
    //   INDEX_RANGE_SCAN → PQ_BLOCK_SCAN
    //   DYNAMIC_INDEX_RANGE_SCAN → PQ_BLOCK_SCAN
    //   FOLLOW_TAIL → PQ_BLOCK_SCAN

    // Step 3: Recalculate costs per type
    switch (p->type) {
      case PQ_BLOCK_SCAN:  SetCostOnTableAccessPath(..., is_after_filter=false)
      case PQ_REF_SCAN:    SetCostOnTableAccessPath(..., is_after_filter=false)
      case FILTER:         rewrite_filter_access_path_cost(thd, p)
      case NESTED_LOOP_JOIN / NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
                           rewrite_nestloop_join_access_path_cost(p, thd, div_table_map)
      case HASH_JOIN:      rewrite_hash_join_access_path_cost(thd, join, div_table_map, p)
      case LIMIT_OFFSET:   EstimateLimitOffsetCost(p, true)
      case MATERIALIZE:    EstimateMaterializeCost(thd, p)
                           // If inner is PQ_BLOCK_SCAN, adjust rows by dividing by pq_dop
      case STREAM:         EstimateStreamCost(p)
      case SORT:           EstimateSortCost(p)
      case WEEDOUT / CACHE_INVALIDATOR:
                           rewrite_access_path_cost_by_copy(p)
      case AGGREGATE:      p->set_num_output_rows(kUnknownRowCount)
                           EstimateAggregateCost(p, join->query_block)
    }
  }, post_order_traversal=true);
}
```

**Hash join cost rewriting detail**:
```c++
rewrite_hash_join_access_path_cost(thd, join, div_table_map, path):
  // Recalculate joined rows
  joined_rows = outer->num_output_rows * inner->num_output_rows

  // If hash table is shared (probe table is divided), multiply back by pq_dop
  if (IsPartialResultsInputHashJoin(join, path))
    joined_rows *= thd->pq_dop

  // If div table is in inner or outer path, divide read_cost by pq_dop
  if (GetUsedTableMap(inner/outer, true) & div_table_map)
    outer_read_cost /= thd->pq_dop

  path->cost = inner->cost + outer_read_cost + row_evaluate_cost(joined_rows)
```

**replace_with_pq_table_access_path mapping**:

| Original Path | Condition | PQ Path |
|---------------|-----------|---------|
| TABLE_SCAN | JT_ALL, not dynamic range, not recursive | PQ_BLOCK_SCAN |
| INDEX_SCAN | JT_INDEX_SCAN | PQ_BLOCK_SCAN |
| REF | Non-constant lookup (`key_copy[part_no]` != null) | PQ_REF_SCAN |
| REF | Constant lookup | PQ_BLOCK_SCAN |
| INDEX_RANGE_SCAN | JT_RANGE | PQ_BLOCK_SCAN |
| DYNAMIC_INDEX_RANGE_SCAN | - | PQ_BLOCK_SCAN |
| FOLLOW_TAIL | - | PQ_BLOCK_SCAN |

### 5.11 Graceful Fallback Mechanism

When `parallel_graceful_fallback` is enabled, PQ failure can fall back to serial execution:

```c++
// Before plan rewrite:
backup_leader_plan():
  join->query_block->pq_backup()  // save query block state
  // In debug: calc and save thd->mem_root digest

// After failure:
restore_leader_plan():
  join->qep_tab = join->qep_tab0        // switch to serial QEP_TAB
  join->ref_items = join->ref_items0
  join->tmp_fields = join->tmp_fields0
  join->query_block->pq_restore()        // restore query block state
  join->pq_restore()                     // reset do_parallel_scan, pq_ref, etc.
  // In debug: verify thd->mem_root digest unchanged
  //   (ensures serial plan was not modified during PQ attempt)
```

**Fallback scope**: Only covers the clone/refix_fields phase. After `make_leader_rewritten_tab()`, fallback is disabled because:
- Leader plan modifications involve dynamic containers (vector/deque) that can't be backed up
- Memory digest checks only work for `MEM_ROOT` allocations
- Most failures after this point are OOM, which are rare

### 5.12 Hash Join Parallel

```
In-memory hash join (Case 1 - divide build table):
  Worker_i: t1.part_i → build hash map → probe with full t2
  Each worker has own hash map, no contention

In-memory hash join (Case 2 - divide probe table):
  Worker_i: full t1 → shared hash map → probe with t2.part_i
  One worker builds hash map preemptively, others wait at barrier

On-disk hash join:
  PQHashJoinSharedContext manages:
  - Barrier for build/probe phase synchronization
  - Shared chunk files (ChunkFilesWrapper)
  - Hash map registry for cross-worker probe
  - Memory budget tracking
```

### 5.13 UNION / Subquery

```
UNION ALL:
  make_pq_unit_plan() → parallelize each eligible query block independently

UNION DISTINCT:
  setup_materialization() → parallelize each block, then dedup

Materialized subquery (derived table, CTE):
  Two-phase execution:
    Phase 1: inner subquery executes in parallel, materializes results
    Phase 2: outer query executes in parallel, reads from materialized table
  Inner materialization triggered by para_exec_init() during ParallelScanIterator::Init()

Shared temp table:
  PQ_shared_info stores shared storage data
  Each worker creates own temp table, swaps storage pointer with leader's
  Supports Temptable engine, InnoDB temp table, and MEMORY engine
```

### 5.14 Partition Table

```
Leader: ha_innopart::pq_leader_scan_init()
  → for each used partition: split_used_partition()

Two-level map: PQ_slices_map
  Level 1: join ref key → partition map
  Level 2: partition id → data block collection

Worker: ha_innopart::pq_worker_scan_next()
  Index unordered: sequential read from each partition, switch on exhaustion
  Index ordered: priority queue merge across partitions (reuses Partition_helper)

Multi-partition cursor: m_prebuilt modified to use arrays indexed by partition
```

---

## 6. InnoDB Data Partition Algorithm

### 6.1 First-Level Partition

Leader traverses the B+tree root node. Each root record corresponds to a leaf page range, forming one scan context (Ctx).

```
Scan_ctx::create_ranges(root_page, scan_borders, level=0, ranges):
  for each record in root page within scan range:
    create_persistent_cursor(record) → Iter
    create PQ_Range with [start_iter, end_iter)
  number of Ctxs = number of root page records in range
```

Scan types supported:
- **Full table scan**: empty start/end dtuple
- **Range scan**: each KEY_MULTI_RANGE → one range, partitioned independently
- **Ref scan (independent)**: treated as single-range scan
- **Ref scan (dependent)**: partition deferred to workers (ref key depends on outer table)

### 6.2 Second-Level Split

First-level Ctxs may not divide evenly among workers. Dynamic split subdivides remaining Ctxs.

```
Split decision:
  if #Ctxs > #threads:  split only tail Ctxs
  if B+tree height < 2: no split (smallest unit is a page)
  otherwise:            all Ctxs marked for second split

Worker-performed split:
  PQ_Range::split():
    ranges = scan_ctx->partition(scan_range, depth=1)
    wait for this range's turn (preserve order via m_split_cur)
    enqueue child Ctxs
```

### 6.3 Worker Data Collection (Dual Queue)

Each worker uses a `PQ_dual_queue` to ensure it scans the same data blocks across multiple rounds (e.g., nested-loop join inner table).

```
Worker_get_next_ctx:
  if public_queue not empty:   dequeue → put in standby
  elif current_queue not empty: dequeue → put in standby
  else: rotate (standby becomes current), get next ctx
```

### 6.4 Read View Consistency

All workers share the leader's MVCC read view for consistent visibility checking.

```
pq_clone_innodb_snapshot(worker_thd, leader_thd):
  Copy leader's read_view to worker
```

Visibility check:
- Read-only mode: view is empty
- Non-read-only: check via MVCC (primary key) or page LSN (secondary key)

### 6.5 Reverse Index Scan

- Ctxs enqueue in reverse order
- Workers scan each Ctx in reverse
- Boundary tuples repositioned for reverse direction
- Second-split child Ctxs maintain reverse order via `m_split_cur--`

---

## 7. Iterator Architecture

### 7.1 ParallelScanIterator (Leader Side)

The leader-side iterator that replaces the original table scan. It manages worker threads and collects results via message queues.

```c++
class ParallelScanIterator : public TableRowIterator {
  uchar *m_record;
  double m_expected_rows;
  ha_rows *m_examined_rows;
  uint m_dop;
  JOIN *m_join;
  Gather_operator *m_gather;
  MQ_record_gather *m_record_gather;
  ORDER *m_order;
  QEP_TAB *m_tab;
  bool m_stable_sort;
  AccessPath *m_root_access_path;  // saved for subquery materialization walk

  // Lifecycle:
  bool Init() override;  // materialize subqueries → init exchange → init data → launch workers
  int Read() override;   // read from MQ via Exchange
  int End() override;    // wait workers → collect errors
  ~ParallelScanIterator();  // cleanup MQ

  // Internal methods:
  bool pq_make_filesort(Filesort **sort);      // construct sort for gather merge
  bool pq_init_record_gather();                 // create Exchange + set worker MQ handles
  bool pq_launch_worker();                      // create and start worker threads
  void pq_wait_workers_finished();              // wait + join all workers
  int pq_error_code();                          // merge worker errors
  bool para_exec_init(AccessPath *path);        // materialize shared temp tables
  bool exec_scalar_uncorrelated_subquery();     // execute scalar uncorrelated subqueries
};
```

**Init() detailed flow**:
1. `para_exec_init()` — Walk access paths, materialize temp tables for sharing
2. `exec_scalar_uncorrelated_subquery()` — Execute uncorrelated subqueries on leader, share results with workers
3. `pq_init_record_gather()` — Create Exchange (sort or nosort), set each worker's MQ handle
4. `m_gather->init()` — Call `InitPQTab` → `handler->ha_pq_init()` to partition data
5. `pq_create_innodb_snapshot()` — Create leader's read view if not already done
6. `pq_launch_worker()` — Create and start all worker threads, wait for each to signal READY

### 7.2 PQblockScanIterator (Worker Side — Table/Block Scan)

Worker-side iterator that replaces the original table scan, reading only the assigned data blocks.

```c++
class PQblockScanIterator : public TableRowIterator {
  PQTabType m_tabType;       // DIV_TAB or CUT_TAB
  bool m_need_rowid;          // true if leader needs row IDs for stable sort
  MQueue_handle *m_handler;   // for checking MQ detached status
  Gather_operator *m_gather;
  QEP_TAB *m_tab;
  void *m_pq_ctx;             // scan context from Gather_operator::pq_tabs
  uint keyno;                 // index to scan

  bool Init() override;  // pq_worker_scan_init + record buffer setup
  int Read() override;   // ha_pq_next(record, pq_ctx) until EOF or MQ detached
  int End() override;    // pq_worker_scan_end
};
```

**Read() logic**:
```c++
int PQblockScanIterator::Read() {
  while (!m_handler->is_detached() && (tmp = ha_pq_next(m_record, m_pq_ctx))) {
    if (tmp == HA_ERR_RECORD_DELETED) continue;     // MyISAM concurrency
    if (tmp == HA_ERR_KEY_NOT_FOUND && mvi_filter) continue;  // multi-value index
    return HandleError(tmp);
  }
  if (m_handler->is_detached()) return -1;  // leader stopped reading
  if (m_need_rowid) table()->file->position(m_record);
  return 0;
}
```

### 7.3 PQRefIterator (Worker Side — Ref/Key Lookup)

Worker-side iterator for dependent ref scans, where the lookup key depends on the outer table.

```c++
class PQRefIterator : public TableRowIterator {
  PQTabType m_tabType;
  Gather_operator *m_gather;
  QEP_TAB *m_tab;
  Index_lookup *m_ref;
  void *m_pq_ctx;
  bool m_first_record_since_init;

  bool Init() override;  // pq_worker_scan_init + construct lookup key
  int Read() override;   // first: pq_ref_build_ranges + ha_pq_next; subsequent: ha_pq_next
  int End() override;    // pq_worker_scan_end
};
```

**Read() logic**:
```c++
int PQRefIterator::Read() {
  if (m_first_record_since_init) {
    // Construct lookup key from outer row
    construct_lookup(thd(), table(), m_ref);
    table()->file->pq_ref_key = {key, key_buff, keypart_map};
    table()->file->pq_ref_depend = true;
    // Build scan ranges for this specific ref key
    table()->file->pq_ref_build_ranges(m_pq_ctx, key_ref);
    error = table()->file->ha_pq_next(table()->record[0], m_pq_ctx);
  } else {
    // Read next matching record
    error = table()->file->ha_pq_next(table()->record[0], m_pq_ctx);
  }
}
```

### 7.4 Iterator Creation in Access Path

The access path types `PQ_BLOCK_SCAN` and `PQ_REF_SCAN` are created during `replace_with_pq_table_access_path()` and result in `PQblockScanIterator` or `PQRefIterator` respectively:

```c++
// In create_iterator_from_accesspath():
case AccessPath::PQ_BLOCK_SCAN:
  return NewPQblockScanIterator(thd, table, expected_rows, examined_rows,
                                tabType, gather, qep_tab, need_rowid, handler);
case AccessPath::PQ_REF_SCAN:
  return NewPQrefScanIterator(thd, table, expected_rows, examined_rows,
                              tabType, gather, qep_tab, ref, need_rowid, handler);
```

---

## 8. System Variables

| Variable | Type | Scope | Default | Description |
|----------|------|-------|---------|-------------|
| `parallel_default_dop` | ulong | session | 4 | Default degree of parallelism |
| `parallel_cost_threshold` | ulong | session | 10000 | Minimum query cost to trigger PQ |
| `parallel_memory_limit` | ulonglong | global | 256MB | Max memory for all PQ executions |
| `parallel_max_threads` | ulong | global | 128 | Max concurrent PQ threads |
| `parallel_queue_timeout` | ulong | session | 0 | Timeout for PQ thread queue (ms) |
| `pq_support_features_switch` | flagset | session | `simple_agg,correlated_subquery` | Feature enable/disable flags |
| `pq_msg_queue_spin_count` | ulong | session | - | Spin count before MQ wait |
| `parallel_batch_max_slot` | ulong | session | - | Max batch buffer slots |
| `parallel_batch_max_mem_size` | ulong | session | - | Max batch buffer memory |
| `parallel_setup_cost` | double | session | 1000 | Cost of setting up parallel execution |
| `parallel_tuple_cost` | double | session | 0.1 | Cost of transferring one tuple between workers and leader |
| `parallel_rows_threshold` | ulong | session | 10000 | Minimum rows in table to consider PQ |
| `parallel_graceful_fallback` | bool | session | false | Enable graceful fallback to serial on PQ failure |

### pq_support_features_switch Flags

| Flag | Bit | Default | Description |
|------|-----|---------|-------------|
| `simple_agg` | 0 | ON | Enable parallel simple aggregation |
| `count_distinct` | 1 | OFF | Enable parallel COUNT(DISTINCT) |
| `correlated_subquery` | 2 | ON | Enable PQ for correlated subqueries |
| `hash_join_spill_to_disk` | 3 | OFF | Enable spill to disk for parallel hash join |
| `insert_select` | 4 | OFF | Enable PQ for INSERT SELECT |

---

## 9. Status Variables

| Variable | Description |
|----------|-------------|
| `parallel_threads_running` | Current number of PQ threads running |
| `parallel_threads_refused` | Count of PQ refusals due to thread limit |
| `parallel_memory_refused` | Count of PQ refusals due to memory limit |

---

## 10. EXPLAIN Output

### Traditional EXPLAIN

A `<gatherN>` pseudo-table row is added showing the number of workers and the parallel-scanned table:

```
| id | select_type | table     | type | key     | Extra                                    |
|----|-------------|-----------|------|---------|------------------------------------------|
|  1 | SIMPLE      | <gather1> | ALL  | NULL    | Parallel execute (4 workers, test.t1)    |
|  1 | SIMPLE      | t1        | range| idx_abc | Using where; Using index                 |
```

Implementation:
- `opt_explain_traditional.cc`: `ET_PARALLEL_EXE` → "Parallel execute"
- `opt_explain.cc`: `Explain_join::explain_pq_gather(QEP_TAB *tab)` renders the gather row

### EXPLAIN FORMAT=TREE

Shows `ParallelScanIterator` and `PQblockScanIterator` nodes in the iterator tree.

### EXPLAIN ANALYZE

Timing data collected per-worker via `ParallelIterTimingInfo` and aggregated on leader. Workers collect timing via `CollectWorkerIterTimingInfo()` called in `pq_worker_exec()`.

---

## 11. Error Handling

### Worker Error

- Worker sets `m_status = ERROR` and sends `ERROR_MSG` via MQ
- Leader detects error in `pq_error_code()`:
  - MQ detached: worker crashed
  - Worker `is_killed()`: KILL query
  - Worker `is_error()`: SQL error
- Leader calls `pq_merge_status()` to propagate worker error to client
- All workers are signaled to stop via `set_kill_state()`

### Leader Error

- Leader checks `is_pq_error()` which includes worker errors
- On error, leader aborts remaining workers and falls back if possible

### MQ Error

- `MQueue_handle::send_exception_msg()` sends error message
- If MQ send itself fails, `my_error(ER_PARALLEL_QUERY_ERROR)` is raised
- DBUG test hooks: `pq_mq_error1` through `pq_mq_error6`

### Auto-Retry

If PQ fails at runtime, the query can be re-executed without PQ:
- `PQUnsuiteInfo::RETRY_WITHOUT_PQ` flag
- Leader falls back to serial execution plan (qep_tab0)

### Worker Launch Failure

If some workers fail to start:
- `pq_launch_worker()` checks `m_active` after `wait_for_status(READY)`
- If `!m_active`, leader detaches the failed worker's MQ
- If ALL workers fail to start, `launch_workers == 0`, return error
- For hash join: non-launched workers' barrier participants are reduced via `ArriveAndDrop()`

---

## 12. Memory Management

- All PQ allocations use `THD::pq_mem_root` (a separate MEM_ROOT per PQ statement)
- `pq_mem_root` is freed after query completes
- Workers share leader's `pq_mem_root` for certain structures
- Global memory tracking: `add_pq_memory()` / `sub_pq_memory()` with bucket-based accounting
- If `parallel_memory_limit` is exceeded, PQ is refused (`MEMORY_LIMIT` unsuite info)
- In debug mode: `MEM_ROOT::CalcMemDigest()` verifies serial plan integrity during graceful fallback

### THD Creation for Workers and Template

```c++
THD *pq_new_thd(THD *thd) {
  THD *new_thd = new (thd->pq_mem_root) THD();
  new_thd->set_new_thread_id();
  new_thd->init_cost_model();
  new_thd->store_globals();
  new_thd->want_privilege = 0;
  new_thd->set_db(thd->db());
  new_thd->pq_copy_from(thd);   // copy session variables, charset, etc.
  // For EXPLAIN ANALYZE, set flag on worker THD
  if (thd->pq_explain_analyze || (thd->lex->is_explain() && iterator_based))
    new_thd->pq_explain_analyze = true;
}
```

---

## 13. Thread Management

```
Global counters:
  parallel_threads_running  (protected by LOCK_pq_threads_running)
  parallel_max_threads      (configured limit)

Before creating workers:
  check_pq_running_threads(dop, timeout)
    → if parallel_threads_running + dop > parallel_max_threads:
        wait up to parallel_queue_timeout, then refuse if still saturated
    → parallel_threads_running += dop

After query:
  release_pq_running_threads(dop)
    → parallel_threads_running -= dop
    → signal COND_pq_threads_running for waiting sessions
```

---

## 14. Key Integration Points in MySQL Codebase

| File | Integration Point | What Happens |
|------|-------------------|-------------|
| `sql/sql_optimizer.cc` | `JOIN::optimize()` end | Check PQ eligibility, save optimized vars, call `make_pq_leader_plan()` |
| `sql/sql_select.cc` | Plan execution | Switch between qep_tab0 (serial) and qep_tab1 (parallel) |
| `sql/sql_union.cc` | `Query_expression::execute()` | Call `make_pq_unit_plan()` for UNION |
| `sql/sql_union.cc` | `setup_materialization()` | Parallelize UNION DISTINCT blocks |
| `sql/iterators/composite_iterators.cc` | `MaterializeIterator` | PQ-aware subquery materialization |
| `sql/join_optimizer/access_path.cc` | AccessPath creation | Create `PQBlockScanAccessPath`, cost estimation |
| `sql/opt_explain.cc` | EXPLAIN | `explain_pq_gather()` renders gather row |
| `sql/sys_vars.cc` | System variables | Define all PQ variables |
| `sql/CMakeLists.txt` | Build | Add all `parallel_query/*.cc` files |
| `storage/innobase/CMakeLists.txt` | Build | Add `ha_innodb_pq.cc`, `row0pread_pq.cc` |

### Key Integration Sequence

1. **After `JOIN::optimize()`**: `JOIN::save_optimized_vars()` saves GROUP BY/ORDER BY state
2. **After `Query_expression::optimize()`**: `make_pq_unit_plan()` rewrites plan for PQ
3. **During `ParallelScanIterator::Init()`**: Materialize subqueries, create exchange, partition data, launch workers
4. **After PQ execution**: `pq_free_gather()` cleans up, `release_pq_running_threads()` returns threads to pool

---

## 15. Test Coverage

98 MTR test cases in `mysql-test/suite/parallel_query/t/`:

| Category | Test Cases |
|----------|-----------|
| Core functionality | `pq_fullscan`, `pq_demon`, `pq_variables`, `pq_fallback`, `pq_master_disable` |
| Aggregation | `pq_group_by`, `pq_agg_distinct`, `pq_aggr_no_record`, `pq_autoinc` |
| ORDER BY | `pq_order_by`, `pq_order_const`, `pq_stable_sort` (via coverage_index) |
| Range scan | `pq_range_clust`, `pq_range_sec`, `pq_range_scan_reverse`, `pq_range_exception` |
| Ref scan | `pq_depend_ref`, `pq_ref_build_range`, `pq_ref_reverse_scan`, `pq_jt_ref` |
| Index scan | `pq_coverage_index`, `pq_index_scan_desc`, `pq_reverse_index_scan` |
| Hash join | `pq_hash_join`, `pq_hash_join_error` |
| UNION/Subquery | `pq_union`, `pq_subquery`, `pq_subquery_correlated`, `pq_derived_view` |
| Partition table | `pq_partition` |
| EXPLAIN | `pq_explain`, `pq_explain_json`, `pq_explain_tree`, `pq_explain_analyze` |
| Error handling | `pq_worker_error`, `pq_leader_exception`, `pq_mq_error`, `pq_abort`, `pq_kill`, `pq_kill_query` |
| Resource control | `pq_memory_limit`, `pq_mdl_lock`, `pq_readonly` |
| INSERT SELECT | `parallel_insert_select`, `parallel_insert_select_behavior_changes` |
| Special cases | `pq_blob`, `pq_charset`, `pq_icp`, `pq_semijoin`, `pq_distinct`, `pq_multi_value` |
| Bug fixes | `pq_bugfix`, `pq_dev_bugs`, `pq_read_record_crash`, `pq_left_join_zero_rows` |
| Optimizer trace | `pq_optimizer_trace`, `pq_opt_trace`, `pq_optimizer_trace_bugfix` |
| Misc | `pq_prepare`, `pq_found_rows`, `pq_examined_rows`, `pq_slow_log`, `pq_flush`, `pq_instant_add_column`, `pq_innodb_intrinsic` |

### DBUG Sync Points for Testing

Key debug sync points used in test cases:
- `pq_wait_launch_worker` — before launching workers
- `pq_wait_kill` — during worker launch (for kill testing)
- `pq_launch_worker_1` — after first worker launched
- `after_pq_leader_plan` — after leader plan creation

Key DBUG evaluate-if hooks:
- `pq_gather_error1..4` — force errors in gather operator creation
- `pq_leader_abort1..3` — force errors in leader plan creation
- `pq_worker_abort1..2` — force errors in worker plan creation
- `pq_worker_error1..9` — force various worker errors
- `pq_msort_error1..4` — force errors in merge sort setup
- `pq_range_scan_reset` — force range scan reset error
- `no_available_idle_threads` — simulate thread exhaustion

---

## 16. Limitations

- Only InnoDB storage engine is supported
- Only the first non-const primary table is parallelized per query block
- SERIALIZABLE isolation level not supported
- Temporary tables, fulltext search not supported
- Semi-join with Materialization or DuplicateWeedout not parallelized
- ROLLUP not supported
- LIMIT without ORDER BY not parallelized
- Dependent (correlated) subqueries: only with `pq_support_features_switch=correlated_subquery`
- Group reshuffle (advanced aggregation) not in current implementation
- Maximum 256 parallel reader threads per InnoDB instance
- Hypergraph optimizer not supported
- Plan cache is invalidated when PQ is chosen (PQ and plan cache are mutually exclusive)

---

## 17. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **pq_resource_stat** — Global resource tracking (memory, threads, status variables)
2. **pq_handler** — Engine-agnostic data partition types (base classes, queues, maps)
3. **msg_queue** — Lock-free message queue infrastructure
4. **binary_heap** — Binary heap for gather merge
5. **exchange** / **exchange_nosort** — Leader-side data collection (unsorted)
6. **exchange_sort** — Gather merge (sorted)
7. **query_result_mq** — Worker result sender
8. **InnoDB data partition** — `row0pread_pq.h`, `row0pread_pq.cc`, `ha_innodb_pq.cc`
9. **pq_clone** — Execution plan cloning (JOIN, QEP_TAB)
10. **pq_clone_item** — Item tree deep cloning
11. **pq_refix_fields_item** — Reference fixing after clone
12. **pq_replace_base_item** — Base item replacement
13. **pq_resolver** — Name resolution for cloned blocks
14. **pq_optimizer** — Eligibility checks, RBO rules, PQ_optimized_var
15. **sql_parallel** — Core orchestration (Gather_operator, worker management, plan rewrite)
16. **pq_iterators** — ParallelScanIterator, PQblockScanIterator, PQRefIterator
17. **THD/JOIN/Query_block modifications** — Integrate PQ state into existing classes
18. **System variables** — Define all PQ variables in sys_vars.cc
19. **EXPLAIN support** — explain_pq_gather, ET_PARALLEL_EXE
20. **explain_pq_access_path** — EXPLAIN ANALYZE timing
21. **Hash join parallel** — PQHashJoinSharedContext, barrier, chunk files
22. **UNION/Subquery** — make_pq_unit_plan, materialization, shared temp table
23. **Partition table** — ha_innopart extension, ordered/unordered partition scan
24. **Test suite** — Port MTR test cases
