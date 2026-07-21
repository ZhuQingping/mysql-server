#ifndef SQL_PARALLEL_H
#define SQL_PARALLEL_H

/* Copyright (c) 2020, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "my_alloc.h"
#include "sql/mem_root_array.h"
#include "sql/parallel_query/msg_queue.h"

class QEP_TAB;
class Query_block;
class THD;
class Gather_operator;
class JOIN;
class ORDER_with_src;
class Exchange;
class Filesort;
class Item;
class Item_subselect;
struct ORDER;
namespace HashJoin {
class PQHashJoinSharedContext;
}
// Defined in sql_lex.h
using Group_list_ptrs = Mem_root_array<ORDER *>;

struct PQ_REF_VAL {
  uchar *keybuff;
  uint keylen;
};

enum PQ_exec_status { SEQ_EXEC = 0, PARL_EXEC, ABORT_EXEC };

enum PQ_worker_state {
  INIT = 1,
  READY = 2,
  COMPELET = 4,
  ERROR = 8,
  OVER = 16
};

class MQ_record_gather {
 public:
  THD *m_thd;
  QEP_TAB *m_tab;
  Exchange *m_exchange;
  mem_root_deque<Item *> *m_recv_items;

 public:
  MQ_record_gather()
      : m_thd(nullptr),
        m_tab(nullptr),
        m_exchange(nullptr),
        m_recv_items(nullptr) {}

  MQ_record_gather(THD *thd, QEP_TAB *tab, mem_root_deque<Item *> *item)
      : m_thd(thd), m_tab(tab), m_exchange(nullptr), m_recv_items(item) {}

  bool mq_scan_init(Filesort *sort, int workers,
                    const std::set<const ORDER *> &desc_groups,
                    bool stab_output = false, bool index_sort = false);

  bool mq_scan_next();

  void mq_scan_end();
};

#define NO_PQ_WORKER_INDEX -1

/**
 * Parallel scan worker's manager struct
 */
class PQ_worker_manager {
 public:
  Gather_operator *m_gather;
  THD *thd_leader;             // pointer to leader thread
  THD *thd_worker;             // pointer to worker thread
  PQ_worker_state m_status;    // worker status
  my_thread_handle thread_id;  // Thread id
  bool m_active{false};  // true if this worker is created, and false otherwise.
  MQueue_handle *m_handle; /** worker's message queue handle */
  int worker_index{NO_PQ_WORKER_INDEX};

 private:
  mysql_mutex_t m_mutex;  // mutex protect previous members
  mysql_cond_t m_cond;

 public:
  PQ_worker_manager();

  ~PQ_worker_manager();

  bool wait_for_status(THD *thd, uint state);

  void signal_status(THD *thd, PQ_worker_state state);

  void set_kill_state(THD::killed_state state_to_set);
};

struct PQTab {
  TABLE *m_table;  // the table need parallel query
  void *m_pq_ctx;  // storage engine's Parallel query context
  QEP_TAB *m_tab;  // the rewrite qep_tab
  uint keyno;      // the index for paralleling read
  bool table_scan;
};

struct CODE_STATE;
/**
 * Gather operator for parallel scan
 */
class Gather_operator {
 public:
  uint m_dop;                     // Degree of parallel execution;
  JOIN *m_template_join;          // physical query plan template
  PQ_worker_manager **m_workers;  // parallel workers manager info
  PQTab pq_tabs[END_TAB];         // PQ tables array
  std::set<PQTabType> tab_set{DIV_TAB};

  int m_ha_err;
  Diagnostics_area m_stmt_da;
  CODE_STATE **m_code_state;
  // used to check clone table fields between leader and workers
  uint pq_check_fields{0};
  // used to check clone table record len between leader and workers
  uint pq_check_reclen{0};

  /// A shared storage between workers used when setting up parallel hash join.
  /// Workers need access other workers hash table, so this is a place where
  /// workers can share data during iterator construction.
  ///
  /// Note 1: Since this object is used only when constructing iterators, and
  ///         iterators are created by multiple threads in sequence, there is no
  ///         need to synchronize the access to this object. Note that workers
  ///         retain a (raw) pointer to the underlying
  ///         PQHashJoinSharedContext, which they concurrently use after
  ///         iterator setup, and synchronization is needed for that.
  /// Note 2: If not nullptr, this object is allocated on the leader THDs
  ///         pq_mem_root.
  /// Note 3: We will have at most one parallel-aware hash join in a query.
  unique_ptr_destroy_only<HashJoin::PQHashJoinSharedContext>
      m_pq_hash_join_shared_context;
  /// A list of uncorrelated subqueries which belong to the query block
  /// of this Gather_operator.
  Mem_root_array<Item_subselect *> m_uncorrelated_subqueries;

  /** groups using desc indexes */
  std::set<const ORDER *> m_desc_groups;

 public:
  Gather_operator() = delete;
  Gather_operator(uint dop, MEM_ROOT *root);

  void prepare();
  bool init();
};

Gather_operator *make_pq_gather_operator(JOIN *join, uint dop);
PQ_exec_status make_pq_leader_plan(JOIN *join, THD *thd);
PQ_exec_status make_pq_unit_plan(Query_expression *unit, THD *thd);

void *pq_worker_exec(void *arg);
bool pq_build_sum_funcs(THD *thd, Query_block *select, Ref_item_array &ref_ptr,
                        mem_root_deque<Item *> *fields,
                        nesting_map select_nest_level);
void pq_replace_avg_func(THD *thd, Query_block *select,
                         mem_root_deque<Item *> *fields,
                         nesting_map select_nest_level);
extern void add_to_list(SQL_I_List<ORDER> &list, ORDER *order);
extern bool get_table_key_fields(QEP_TAB *tab,
                                 std::vector<std::string> &res_fields);
extern bool setup_order(THD *thd, Ref_item_array ref_item_array,
                        Table_ref *tables, List<Item> &fields,
                        List<Item> &all_fields, ORDER *order);

extern void release_pq_running_threads(uint dop);

bool pq_make_join_readinfo(JOIN *join, Gather_operator *gather,
                           bool make_worker);

bool pq_check_stable_sort(JOIN *join);

bool set_key_order(QEP_TAB *tab, std::vector<std::string> &res_fields,
                   ORDER **order_ptr, Ref_item_array *ref_ptrs);

void restore_list(Group_list_ptrs *ptr, SQL_I_List<ORDER> &orig_list);
ORDER *restore_optimized_group_order(SQL_I_List<ORDER> &orig_list,
                                     std::vector<bool> &optimized_flags);
void record_optimized_group_order(Group_list_ptrs *ptr,
                                  ORDER_with_src &new_list,
                                  std::vector<bool> &optimized_flags);
void pq_free_thd(THD *thd);

void pq_free_join(THD *thd, JOIN *join);

void pq_free_gather(Gather_operator *gather);

bool check_pq_running_threads(uint dop, ulong timeout_ms);

// these two functions "pq_clone[create]_innodb_snapshot" declared here
// because we don't want to include ha_innodb.h in sql/ files.
bool pq_clone_innodb_snapshot(THD *worker_thd, THD *leader_thd);

bool pq_create_innodb_snapshot(THD *thd);

/// Estimate the costs and row count for a parallel query gather AccessPath.
void EstimatePQGatherOperatorCost(AccessPath *path, THD *thd);
#endif /* SQL_PARALLEL_H */
