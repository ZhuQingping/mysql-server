#ifndef PQ_CONTEXT_INCLUDED
#define PQ_CONTEXT_INCLUDED

/* Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

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
   GNU General Public License for more details. */

#include <atomic>
#include <vector>

#include "include/map_helpers.h"
#include "my_inttypes.h"
#include "sql/parallel_query/pq_optimizer.h"

class Item;
class JOIN;
class Gather_operator;
class MQueue_handle;
class QEP_TAB;
class Query_block;
class THD;
class Temp_table_param;
class PQ_worker_manager;
struct ORDER;
struct MEM_ROOT;
template <class T>
class Bounds_checked_array;
template <class T>
class mem_root_deque;
template <class T>
class Mem_root_array;

enum class PQ_clone_phase {
  PREPARE = 0,
  OPTIMIZE,
  EXECUTION,
  UNKNOWN,
};

/** PQ-owned session, worker, and allocation state for one THD. */
class PQ_thd_context {
 public:
  void initialize_mem_root();
  void destroy_mem_root();
  void cleanup_statement();
  bool suite_for_parallel_query(const THD &thd, PQUnsuiteInfo *pq_info) const;
  bool is_worker() const { return leader != nullptr; }
  bool is_real_worker() const { return leader != nullptr && worker_info != nullptr; }
  bool is_leader() const { return leader == nullptr; }
  /**
    Error is a worker-to-leader cancellation signal. Relaxed ordering is enough:
    the flag does not publish diagnostics or plan state; those retain their
    existing synchronization.
  */
  bool has_error() const { return m_error.load(std::memory_order_relaxed); }
  void set_error() { m_error.store(true, std::memory_order_relaxed); }
  /** Call only after all workers observing this statement have joined. */
  void clear_error_after_workers_join() {
    m_error.store(false, std::memory_order_relaxed);
  }
  bool merge_status(THD &owner, THD &other);
  void copy_from(const THD &source);

  PQ_clone_phase clone_phase{PQ_clone_phase::UNKNOWN};
  /** Dedicated allocation root for PQ plans, clones, and workers. */
  MEM_ROOT *mem_root{nullptr};
  /** Worker-to-leader topology and per-leader gather state. */
  THD *leader{nullptr};
  std::vector<Gather_operator *> gathers;
  PQ_worker_manager *worker_info{nullptr};
  bool retry_without_pq{false};
  bool has_pq{false};
  bool leader_create_fake_iter{false};
  uint threads_running{0};
  uint dop{0};
  bool no_pq{false};
  uint64 current_found_rows{0};
  bool executed{false};
#ifndef NDEBUG
  // One worker skips fetching scan context; test-only Temptable coverage.
  bool skip_fetch_ctx{false};
#endif  // NDEBUG
  bool explain_analyze{false};

 private:
  std::atomic<bool> m_error{false};
};

/** Optimizer state saved before the PQ plan rewrites its host JOIN. */
struct PQ_optimized_var {
  bool pq_grouped;
  bool pq_implicit_grouping;
  bool pq_simple_group;
  bool pq_simple_order;
  bool pq_streaming_aggregation;
  bool pq_group_optimized_away;
  bool pq_need_tmp_before_win;
  bool pq_skip_sort_order;
  int pq_m_ordered_index_usage;
  std::vector<bool> optimized_group_flags;
  std::vector<bool> optimized_order_flags;
  bool pq_select_distinct;
  Item *pq_saved_having_cond{nullptr};
};

/** PQ-owned semantic, clone, and fallback state for one Query_block. */
class PQ_query_block_context {
 public:
  Query_block *last_clone(const Query_block &owner) const;
  Query_block *clone_of(const Query_block &owner) const;
  void link_clone(Query_block &owner, Query_block &clone);
  void unlink_clone(Query_block &clone);
  bool is_clone(const Query_block &owner) const;
  void backup(Query_block &owner);
  void restore(Query_block &owner);
  void check_after_prepare(Query_block &owner, THD &thd);
  bool suite_for_parallel_query(Query_block &owner, THD &thd);
  bool check_table_list(Query_block &owner);

  bool parallel_exec{false};
  bool m_suite_for_pq{true};
  PQUnsuiteInfo pq_unsuite_info{PQUnsuiteInfo::INFO_NONE};
  Item *saved_where_cond{nullptr};
  Item *saved_having_cond{nullptr};
  bool pq_try_clone_item{false};
  bool disable_distinct_in_pq_worker{false};
  Query_block *m_pq_last_clone{nullptr};
  Query_block *m_pq_is_clone_of{nullptr};
  uint saved_windows_elements{0};
  Mem_root_array<ORDER *> *saved_group_list_ptrs{nullptr};
  Mem_root_array<ORDER *> *saved_order_list_ptrs{nullptr};
  mem_root_unordered_map<uint, uint> *check_map_group_to_base{nullptr};
  mem_root_unordered_map<uint, uint> *check_map_order_to_base{nullptr};
};

/** PQ-owned execution-plan state for one JOIN. */
class PQ_join_context {
 public:
  bool restore_optimized_vars(JOIN &join);
  void save_optimized_vars(JOIN &join);
  void set_push_down_having(JOIN &join);
  void restore_plan(JOIN &join);
  bool allocate_indirection_slices(JOIN &join);
  bool allocate_qep(JOIN &join, uint n);
  bool setup_tmp_table_info(JOIN &join, JOIN &orig);

  Mem_root_array<ORDER *> *saved_join_order{nullptr};
  Mem_root_array<ORDER *> *saved_join_group_list{nullptr};
  QEP_TAB *qep_tab0{nullptr};
  QEP_TAB *qep_tab1{nullptr};
  Temp_table_param *saved_tmp_table_param{nullptr};
  bool need_tmp_pq{false};
  bool need_tmp_pq_leader{false};
  mem_root_deque<Item *> *tmp_fields0{nullptr};
  mem_root_deque<Item *> *tmp_fields1{nullptr};
  PQ_optimized_var saved_optimized_vars;
  int idx_div_tab{-1};
  int idx_cut_tab{-1};
  bool pq_rebuilt_group{false};
  bool pq_stable_sort{false};
  int pq_last_sort_idx{-1};
  Bounds_checked_array<Item *> *ref_items0{nullptr};
  Bounds_checked_array<Item *> *ref_items1{nullptr};
  MQueue_handle *m_msg_handler{nullptr};
  bool pq_pushdown_having{false};
  bool has_count_distinct{false};
};

#endif  // PQ_CONTEXT_INCLUDED
