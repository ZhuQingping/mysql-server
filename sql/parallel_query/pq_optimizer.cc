/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in license.xml
   elsewhere in this distribution.  You may use this software under
   the terms of the GNU General Public License, version 2.0,
   or the terms of any other license that is available in this distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/parallel_query/pq_optimizer.cc
  Parallel Query V1-MVP: Conservative eligibility check and fallback skeleton.

  Phase 1 implementation: pq_check_query_block_eligible() and helpers.
  All uncertain cases default to serial fallback.
*/

#include "sql/parallel_query/pq_optimizer.h"

#include <vector>

#include "include/thr_lock.h"     // Lock_descriptor, thr_lock_type
#include "my_dbug.h"
#include "sql/join_optimizer/access_path.h"  // AccessPath
#include "sql/filesort.h"         // Filesort
#include "sql/parallel_query/exchange_sort.h"  // Exchange_sort
#include "sql/parallel_query/pq_aggregate.h"  // pq_check_agg_supported
#include "sql/parallel_query/sql_parallel.h"  // pq_set_execution_state
#include "sql/range_optimizer/range_optimizer.h"  // QUICK_RANGE
#include "sql/sql_class.h"        // THD
#include "sql/sql_lex.h"          // Query_block, LEX, Table_ref
#include "sql/sql_opt_exec_shared.h"  // JOIN_TAB, join_type, JT_ALL
#include "sql/sql_optimizer.h"    // JOIN
#include "sql/table.h"            // TABLE, TABLE_SHARE, Table_ref

/**
  String representation of each PQUnsuiteReason value.
  Indexed by (int)reason, terminated by PQ_UNSUITED_REASON_COUNT sentinel.
*/
static const char *pq_unsuite_reason_names[] = {
    "NONE",                      // PQUnsuiteReason::NONE
    "DISABLED",                  // PQUnsuiteReason::DISABLED
    "WORKER_THD",                // PQUnsuiteReason::WORKER_THD
    "HYPERGRAPH_OPTIMIZER",      // PQUnsuiteReason::HYPERGRAPH_OPTIMIZER
    "NOT_SELECT",                // PQUnsuiteReason::NOT_SELECT
    "MULTI_QUERY_BLOCK",         // PQUnsuiteReason::MULTI_QUERY_BLOCK
    "NO_TABLE",                  // PQUnsuiteReason::NO_TABLE
    "MULTI_TABLE",               // PQUnsuiteReason::MULTI_TABLE
    "NON_INNODB",                // PQUnsuiteReason::NON_INNODB
    "TEMPORARY_TABLE",           // PQUnsuiteReason::TEMPORARY_TABLE
    "PARTITIONED_TABLE",         // PQUnsuiteReason::PARTITIONED_TABLE
    "FULLTEXT",                  // PQUnsuiteReason::FULLTEXT
    "LOCKING_READ",              // PQUnsuiteReason::LOCKING_READ
    "SERIALIZABLE",              // PQUnsuiteReason::SERIALIZABLE
    "HAS_SUBQUERY",              // PQUnsuiteReason::HAS_SUBQUERY
    "HAS_UNION",                 // PQUnsuiteReason::HAS_UNION
    "HAS_DERIVED_OR_VIEW",       // PQUnsuiteReason::HAS_DERIVED_OR_VIEW
    "HAS_WINDOW",                // PQUnsuiteReason::HAS_WINDOW
    "HAS_DISTINCT",              // PQUnsuiteReason::HAS_DISTINCT
    "HAS_ORDER_BY",              // PQUnsuiteReason::HAS_ORDER_BY
    "HAS_HAVING",                // PQUnsuiteReason::HAS_HAVING
    "HAS_GROUP_BY",              // PQUnsuiteReason::HAS_GROUP_BY
    "GROUP_BY_ROLLUP",           // PQUnsuiteReason::GROUP_BY_ROLLUP
    "GROUP_BY_HAVING",           // PQUnsuiteReason::GROUP_BY_HAVING
    "GROUP_BY_UNSUPPORTED_EXPR", // PQUnsuiteReason::GROUP_BY_UNSUPPORTED_EXPR
    "GROUP_BY_UNSUPPORTED_AGGREGATE",  // PQUnsuiteReason::GROUP_BY_UNSUPPORTED_AGGREGATE
    "GROUP_BY_PARTIAL_AGG_UNSUPPORTED",  // PQUnsuiteReason::GROUP_BY_PARTIAL_AGG_UNSUPPORTED
    "HAS_ROLLUP",                // PQUnsuiteReason::HAS_ROLLUP
    "HAS_SEMIJOIN",              // PQUnsuiteReason::HAS_SEMIJOIN
    "NON_FULL_TABLE_SCAN",       // PQUnsuiteReason::NON_FULL_TABLE_SCAN
    "COST_BELOW_THRESHOLD",      // PQUnsuiteReason::COST_BELOW_THRESHOLD
    "UNSUPPORTED_AGGREGATE",     // PQUnsuiteReason::UNSUPPORTED_AGGREGATE
    "UNSUPPORTED_BY_PHASE1",     // PQUnsuiteReason::UNSUPPORTED_BY_PHASE1
};

const char *pq_unsuite_reason_to_string(PQUnsuiteReason reason) {
  int idx = static_cast<int>(reason);
  if (idx >= 0 && idx < static_cast<int>(PQUnsuiteReason::PQ_UNSUITED_REASON_COUNT))
    return pq_unsuite_reason_names[idx];
  return "UNKNOWN";
}

const char *pq_v1_explain_eligible_label() {
  return "eligible, execution disabled, serial fallback";
}

bool pq_build_saved_order_group_contract(
    Query_block *query_block, JOIN *join,
    PQSavedOrderGroupContract *contract) {
  if (contract == nullptr) return false;

  contract->reset();
  if (query_block == nullptr || join == nullptr) {
    contract->status =
        PQSavedOrderGroupContractStatus::UNSUPPORTED_NULL_INPUT;
    contract->detail = "missing Query_block or JOIN";
    return false;
  }

  contract->has_order = query_block->is_ordered() || !join->order.empty();
  contract->has_group =
      query_block->is_explicitly_grouped() || !join->group_list.empty();
  contract->has_having =
      query_block->having_cond() != nullptr || join->having_cond != nullptr;
  contract->grouped = join->grouped;
  contract->group_optimized_away = join->group_optimized_away;
  contract->implicit_grouping = join->implicit_grouping;
  contract->need_tmp_before_win = join->need_tmp_before_win;
  contract->simple_group = join->simple_group;
  contract->simple_order = join->simple_order;
  contract->streaming_aggregation = join->streaming_aggregation;
  contract->skip_sort_order = join->skip_sort_order;
  contract->select_distinct = join->select_distinct;
  contract->ordered_index_usage = static_cast<int>(join->m_ordered_index_usage);

  /*
    E5d-S1 deliberately fails closed. The current branch does not expose the
    commercial saved ORDER/GROUP helper layer (`saved_join_order`,
    `saved_join_group_list`, optimized ORDER/GROUP flags, and restore helpers).
    Captured scalar flags are diagnostics only until S2/S3 define ownership.
  */
  contract->status =
      PQSavedOrderGroupContractStatus::UNSUPPORTED_MISSING_SAVED_HELPERS;
  contract->detail = "missing saved ORDER/GROUP helper layer";
  return false;
}

bool pq_copy_saved_order_group_contract(
    const PQSavedOrderGroupContract &src,
    PQSavedOrderGroupContract *dst) {
  if (dst == nullptr) return false;
  *dst = src;
  return dst->status != PQSavedOrderGroupContractStatus::READY;
}

bool pq_build_orderby_filesort_contract(
    Query_block *query_block, JOIN *join,
    PQOrderByFilesortContract *contract) {
  if (contract == nullptr) return false;

  contract->reset();
  if (query_block == nullptr || join == nullptr) {
    contract->status = PQOrderByFilesortContractStatus::UNSUPPORTED_NULL_INPUT;
    contract->detail = "missing Query_block or JOIN";
    return false;
  }

  PQSavedOrderGroupContract saved_contract;
  (void)pq_build_saved_order_group_contract(query_block, join, &saved_contract);

  contract->has_order = saved_contract.has_order;
  contract->stable_sort_requested = saved_contract.has_order;
  contract->ordered_index_usage = saved_contract.ordered_index_usage;
  contract->saved_order_group_ready = saved_contract.ready();

  if (!saved_contract.ready()) {
    contract->status =
        PQOrderByFilesortContractStatus::UNSUPPORTED_MISSING_SAVED_HELPERS;
    contract->detail = "saved ORDER/GROUP helper contract is not ready";
    return false;
  }

  contract->status = PQOrderByFilesortContractStatus::READY;
  contract->detail = "ready";
  return true;
}

static bool pq_orderby_contract_access_is_full_scan(JOIN *join) {
  if (join == nullptr) return false;

  join_type access_type = JT_UNKNOWN;
  if (join->best_ref != nullptr && join->primary_tables > 0 &&
      join->best_ref[0] != nullptr) {
    access_type = join->best_ref[0]->type();
  }

  if (access_type == JT_UNKNOWN && join->qep_tab != nullptr &&
      join->primary_tables > 0) {
    access_type = join->qep_tab[0].type();
  }

  return access_type == JT_ALL;
}

static bool pq_orderby_contract_asc_only(const JOIN *join) {
  if (join == nullptr || join->order.empty()) return false;

  for (ORDER *order = join->order.order; order != nullptr;
       order = order->next) {
    if (order->direction == ORDER_DESC) return false;
  }
  return true;
}

bool pq_build_orderby_eligibility_contract(
    THD *thd, Query_block *query_block, JOIN *join,
    PQOrderByEligibilityContract *contract) {
  if (contract == nullptr) return false;

  contract->reset();
  if (thd == nullptr || query_block == nullptr || join == nullptr) {
    contract->status =
        PQOrderByEligibilityContractStatus::UNSUPPORTED_NULL_INPUT;
    contract->detail = "missing THD, Query_block, or JOIN";
    return false;
  }

  contract->has_order = query_block->is_ordered() || !join->order.empty();
  if (!contract->has_order) {
    contract->status =
        PQOrderByEligibilityContractStatus::UNSUPPORTED_NOT_ORDERED;
    contract->detail = "query has no ORDER BY";
    return false;
  }

  contract->single_table = query_block->table_count() == 1;
  contract->simple_order = !join->order.empty() && join->simple_order &&
                           is_simple_order(join->order.order);
  contract->asc_only = pq_orderby_contract_asc_only(join);
  contract->has_limit = query_block->has_limit();
  contract->select_distinct =
      query_block->is_distinct() || join->select_distinct;
  contract->has_group =
      query_block->is_explicitly_grouped() || !join->group_list.empty();
  contract->has_having =
      query_block->having_cond() != nullptr || join->having_cond != nullptr;
  contract->has_window = query_block->has_wfs();
  contract->filesort_required =
      join->m_ordered_index_usage == JOIN::ORDERED_INDEX_VOID &&
      !join->skip_sort_order;
  contract->full_scan = pq_orderby_contract_access_is_full_scan(join);

  if (thd->lex->sql_command == SQLCOM_SELECT &&
      !thd->lex->using_hypergraph_optimizer() &&
      query_block->is_simple_query_block() && contract->single_table &&
      contract->simple_order && contract->asc_only && !contract->has_limit &&
      !contract->select_distinct && !contract->has_group &&
      !contract->has_having && !contract->has_window &&
      contract->filesort_required && contract->full_scan) {
    contract->status =
        PQOrderByEligibilityContractStatus::
            FUTURE_CANDIDATE_EXECUTION_DISABLED;
    contract->detail = "future ORDER BY candidate, execution disabled";
    contract->execution_disabled = true;
    return true;
  }

  contract->status = PQOrderByEligibilityContractStatus::UNSUPPORTED_SHAPE;
  contract->detail = "ORDER BY shape is not in the first candidate subset";
  return false;
}

bool pq_build_orderby_execution_preflight(
    THD *thd, Query_block *query_block, JOIN *join,
    PQOrderByExecutionPreflight *preflight) {
  if (preflight == nullptr) return false;

  preflight->reset();
  if (thd == nullptr || query_block == nullptr || join == nullptr) {
    preflight->status =
        PQOrderByExecutionPreflightStatus::UNSUPPORTED_NULL_INPUT;
    preflight->detail = "missing THD, Query_block, or JOIN";
    return false;
  }

  if (!pq_build_orderby_eligibility_contract(thd, query_block, join,
                                             &preflight->eligibility) ||
      !preflight->eligibility.future_candidate_disabled()) {
    preflight->status =
        PQOrderByExecutionPreflightStatus::BLOCKED_ELIGIBILITY;
    preflight->detail = "ORDER BY eligibility contract did not select a "
                        "disabled future candidate";
    return false;
  }

  preflight->eligibility_candidate_disabled = true;

  /*
    M11-E5d-5e-2 centralizes the visible ORDER BY execution blocker. The
    prerequisite flags below intentionally stay false until their real runtime
    owners are wired into the default execution path.
  */
  preflight->saved_order_group_runtime_ready = false;
  preflight->filesort_runtime_ready = false;
  preflight->sort_param_runtime_ready = false;
  preflight->worker_order_frame_producer_ready = false;
  preflight->exchange_sort_heap_read_ready = false;
  preflight->leader_materialization_ready = false;
  preflight->rowid_tiebreak_ready = false;
  preflight->default_ordered_read_ready = false;
  preflight->kill_detach_error_diagnostics_ready = false;
  preflight->execution_disabled = true;
  preflight->status =
      PQOrderByExecutionPreflightStatus::BLOCKED_EXECUTION_DISABLED;
  preflight->detail = "ORDER BY execution prerequisites are not ready";
  return false;
}

struct PQ_owned_order_chain_sidecar {
  std::vector<ORDER> nodes;
  std::vector<ORDER> restored_nodes;
  std::vector<const ORDER *> source_nodes;
  std::vector<bool> optimized_flags;
  Explain_sort_clause src{ESC_none};
  bool const_optimized{false};

  void reset() {
    nodes.clear();
    restored_nodes.clear();
    source_nodes.clear();
    optimized_flags.clear();
    src = ESC_none;
    const_optimized = false;
  }

  ORDER *head() { return nodes.empty() ? nullptr : &nodes[0]; }
  const ORDER *head() const { return nodes.empty() ? nullptr : &nodes[0]; }
  ORDER *restored_head() {
    return restored_nodes.empty() ? nullptr : &restored_nodes[0];
  }
  const ORDER *restored_head() const {
    return restored_nodes.empty() ? nullptr : &restored_nodes[0];
  }
};

struct PQ_order_chain_identity_snapshot {
  const ORDER *head{nullptr};
  Explain_sort_clause src{ESC_none};
  bool const_optimized{false};
  std::vector<const ORDER *> node_addresses;
};

static uint pq_count_order_chain(const ORDER *order) {
  uint count = 0;
  for (const ORDER *cur = order; cur != nullptr && count < 1024;
       cur = cur->next) {
    ++count;
  }
  return count;
}

static bool pq_copy_order_chain(const ORDER_with_src &src,
                                PQ_owned_order_chain_sidecar *dst) {
  if (dst == nullptr) return false;

  dst->reset();
  dst->src = src.src;
  dst->const_optimized = src.is_const_optimized();

  const uint count = pq_count_order_chain(src.order);
  if (count == 0) return true;

  dst->nodes.reserve(count);
  dst->source_nodes.reserve(count);
  uint copied_count = 0;
  for (const ORDER *cur = src.order; cur != nullptr && copied_count < count;
       cur = cur->next, ++copied_count) {
    dst->nodes.push_back(*cur);
    dst->source_nodes.push_back(cur);
  }

  if (dst->nodes.size() != count || dst->source_nodes.size() != count) {
    return false;
  }
  for (uint i = 0; i < dst->nodes.size(); ++i) {
    dst->nodes[i].next =
        (i + 1 < dst->nodes.size()) ? &dst->nodes[i + 1] : nullptr;
  }
  return true;
}

static bool pq_order_chain_copy_matches(
    const ORDER_with_src &src, const PQ_owned_order_chain_sidecar &copy) {
  if (copy.src != src.src ||
      copy.const_optimized != src.is_const_optimized()) {
    return false;
  }

  const ORDER *source = src.order;
  const ORDER *copied = copy.head();
  uint count = 0;
  while (source != nullptr && copied != nullptr && count < 1024) {
    if (source == copied ||
        source->item_initial != copied->item_initial ||
        source->item != copied->item ||
        source->rollup_item != copied->rollup_item ||
        source->direction != copied->direction ||
        source->in_field_list != copied->in_field_list ||
        source->used_alias != copied->used_alias ||
        source->field_in_tmp_table != copied->field_in_tmp_table ||
        source->buff != copied->buff || source->used != copied->used ||
        source->depend_map != copied->depend_map ||
        source->is_explicit != copied->is_explicit) {
      return false;
    }

    source = source->next;
    copied = copied->next;
    ++count;
  }

  return source == nullptr && copied == nullptr &&
         count == copy.nodes.size();
}

static bool pq_capture_order_chain_identity(
    const ORDER_with_src &src, PQ_order_chain_identity_snapshot *snapshot) {
  if (snapshot == nullptr) return false;

  snapshot->head = src.order;
  snapshot->src = src.src;
  snapshot->const_optimized = src.is_const_optimized();
  snapshot->node_addresses.clear();

  for (const ORDER *cur = src.order;
       cur != nullptr && snapshot->node_addresses.size() < 1024;
       cur = cur->next) {
    snapshot->node_addresses.push_back(cur);
  }
  return true;
}

static bool pq_order_chain_identity_matches(
    const ORDER_with_src &src,
    const PQ_order_chain_identity_snapshot &snapshot) {
  if (src.order != snapshot.head || src.src != snapshot.src ||
      src.is_const_optimized() != snapshot.const_optimized) {
    return false;
  }

  uint pos = 0;
  for (const ORDER *cur = src.order; cur != nullptr && pos < 1024;
       cur = cur->next, ++pos) {
    if (pos >= snapshot.node_addresses.size() ||
        cur != snapshot.node_addresses[pos]) {
      return false;
    }
  }
  return pos == snapshot.node_addresses.size();
}

static bool pq_order_chain_contains_source(const ORDER *head,
                                           const ORDER *needle) {
  uint count = 0;
  for (const ORDER *cur = head; cur != nullptr && count < 1024;
       cur = cur->next, ++count) {
    if (cur == needle) return true;
  }
  return false;
}

static bool pq_record_order_chain_optimized_flags(
    const ORDER_with_src &optimized, PQ_owned_order_chain_sidecar *sidecar) {
  if (sidecar == nullptr || sidecar->nodes.size() != sidecar->source_nodes.size()) {
    return false;
  }

  sidecar->optimized_flags.clear();
  sidecar->optimized_flags.reserve(sidecar->source_nodes.size());
  for (const ORDER *source_node : sidecar->source_nodes) {
    sidecar->optimized_flags.push_back(
        pq_order_chain_contains_source(optimized.order, source_node));
  }
  return sidecar->optimized_flags.size() == sidecar->source_nodes.size();
}

static bool pq_restore_order_chain_from_flags(
    PQ_owned_order_chain_sidecar *sidecar) {
  if (sidecar == nullptr ||
      sidecar->nodes.size() != sidecar->optimized_flags.size()) {
    return false;
  }

  sidecar->restored_nodes.clear();
  sidecar->restored_nodes.reserve(sidecar->nodes.size());
  for (uint i = 0; i < sidecar->nodes.size(); ++i) {
    if (sidecar->optimized_flags[i]) {
      sidecar->restored_nodes.push_back(sidecar->nodes[i]);
    }
  }

  for (uint i = 0; i < sidecar->restored_nodes.size(); ++i) {
    sidecar->restored_nodes[i].next =
        (i + 1 < sidecar->restored_nodes.size())
            ? &sidecar->restored_nodes[i + 1]
            : nullptr;
  }
  return !sidecar->restored_nodes.empty();
}

static bool pq_restored_order_chain_matches_optimized(
    const ORDER_with_src &optimized,
    const PQ_owned_order_chain_sidecar &sidecar) {
  const ORDER *expected = optimized.order;
  const ORDER *restored = sidecar.restored_head();
  uint count = 0;
  while (expected != nullptr && restored != nullptr && count < 1024) {
    if (expected == restored ||
        expected->item_initial != restored->item_initial ||
        expected->item != restored->item ||
        expected->direction != restored->direction ||
        expected->in_field_list != restored->in_field_list ||
        expected->used_alias != restored->used_alias ||
        expected->field_in_tmp_table != restored->field_in_tmp_table ||
        expected->buff != restored->buff || expected->used != restored->used ||
        expected->depend_map != restored->depend_map ||
        expected->is_explicit != restored->is_explicit) {
      return false;
    }
    expected = expected->next;
    restored = restored->next;
    ++count;
  }

  return expected == nullptr && restored == nullptr &&
         count == sidecar.restored_nodes.size();
}

static void pq_rewire_order_nodes(std::vector<ORDER> *nodes) {
  if (nodes == nullptr) return;

  for (uint i = 0; i < nodes->size(); ++i) {
    (*nodes)[i].next = (i + 1 < nodes->size()) ? &(*nodes)[i + 1] : nullptr;
  }
}

static bool pq_clone_order_chain_sidecar(
    const PQ_owned_order_chain_sidecar &src,
    PQ_owned_order_chain_sidecar *dst) {
  if (dst == nullptr) return false;

  *dst = src;
  pq_rewire_order_nodes(&dst->nodes);
  pq_rewire_order_nodes(&dst->restored_nodes);
  return dst->nodes.size() == src.nodes.size() &&
         dst->restored_nodes.size() == src.restored_nodes.size() &&
         dst->source_nodes.size() == src.source_nodes.size() &&
         dst->optimized_flags.size() == src.optimized_flags.size();
}

static bool pq_order_nodes_equivalent(const ORDER &lhs, const ORDER &rhs) {
  return lhs.item_initial == rhs.item_initial && lhs.item == rhs.item &&
         lhs.rollup_item == rhs.rollup_item && lhs.direction == rhs.direction &&
         lhs.in_field_list == rhs.in_field_list &&
         lhs.used_alias == rhs.used_alias &&
         lhs.field_in_tmp_table == rhs.field_in_tmp_table &&
         lhs.buff == rhs.buff && lhs.used == rhs.used &&
         lhs.depend_map == rhs.depend_map && lhs.is_explicit == rhs.is_explicit;
}

static bool pq_order_vector_clone_matches(
    const std::vector<ORDER> &src, const std::vector<ORDER> &clone) {
  if (src.size() != clone.size()) return false;

  for (uint i = 0; i < src.size(); ++i) {
    if (&src[i] == &clone[i] || !pq_order_nodes_equivalent(src[i], clone[i])) {
      return false;
    }

    const ORDER *expected_next =
        (i + 1 < clone.size()) ? &clone[i + 1] : nullptr;
    if (clone[i].next != expected_next) return false;
  }
  return true;
}

static bool pq_order_chain_sidecar_clone_matches(
    const PQ_owned_order_chain_sidecar &src,
    const PQ_owned_order_chain_sidecar &clone) {
  if (src.src != clone.src || src.const_optimized != clone.const_optimized ||
      src.source_nodes.size() != clone.source_nodes.size() ||
      src.optimized_flags.size() != clone.optimized_flags.size()) {
    return false;
  }

  for (uint i = 0; i < src.source_nodes.size(); ++i) {
    if (src.source_nodes[i] != clone.source_nodes[i]) return false;
  }
  for (uint i = 0; i < src.optimized_flags.size(); ++i) {
    if (src.optimized_flags[i] != clone.optimized_flags[i]) return false;
  }

  return pq_order_vector_clone_matches(src.nodes, clone.nodes) &&
         pq_order_vector_clone_matches(src.restored_nodes,
                                       clone.restored_nodes);
}

static bool pq_runtime_saved_order_attach_smoke(
    const ORDER_with_src &join_order,
    const ORDER_with_src &optimized_order) {
  PQ_order_chain_identity_snapshot before;
  if (!pq_capture_order_chain_identity(join_order, &before)) return false;

  bool attached_to_owner = false;
  {
    PQ_owned_order_chain_sidecar owner_sidecar;
    PQ_owned_order_chain_sidecar owner_clone;
    attached_to_owner =
        pq_copy_order_chain(join_order, &owner_sidecar) &&
        pq_order_chain_copy_matches(join_order, owner_sidecar) &&
        pq_record_order_chain_optimized_flags(optimized_order,
                                              &owner_sidecar) &&
        pq_restore_order_chain_from_flags(&owner_sidecar) &&
        pq_restored_order_chain_matches_optimized(optimized_order,
                                                  owner_sidecar) &&
        pq_clone_order_chain_sidecar(owner_sidecar, &owner_clone) &&
        pq_order_chain_sidecar_clone_matches(owner_sidecar, owner_clone);
  }

  return attached_to_owner && pq_order_chain_identity_matches(join_order, before);
}

static bool pq_build_orderby_filesort_restored_order_contract(
    Query_block *query_block, JOIN *join,
    PQOrderByFilesortContract *contract) {
  if (contract == nullptr) return false;

  contract->reset();
  if (query_block == nullptr || join == nullptr) {
    contract->status = PQOrderByFilesortContractStatus::UNSUPPORTED_NULL_INPUT;
    contract->detail = "missing Query_block or JOIN";
    return false;
  }

  contract->has_order = query_block->is_ordered() || !join->order.empty();
  contract->stable_sort_requested = contract->has_order;

  PQSavedOrderGroupContract saved_contract;
  (void)pq_build_saved_order_group_contract(query_block, join, &saved_contract);
  contract->ordered_index_usage = saved_contract.ordered_index_usage;
  contract->saved_order_group_ready = saved_contract.ready();

  if (join->order.empty() || join->order.order->next == nullptr) {
    contract->status =
        PQOrderByFilesortContractStatus::UNSUPPORTED_MISSING_RESTORED_ORDER;
    contract->detail = "restored ORDER sidecar is not available";
    return false;
  }

  PQ_owned_order_chain_sidecar leader_sidecar;
  PQ_owned_order_chain_sidecar clone_sidecar;
  ORDER_with_src optimized_without_first(join->order.order->next,
                                         join->order.src,
                                         join->order.is_const_optimized());
  if (!pq_copy_order_chain(join->order, &leader_sidecar) ||
      !pq_order_chain_copy_matches(join->order, leader_sidecar) ||
      !pq_record_order_chain_optimized_flags(optimized_without_first,
                                             &leader_sidecar) ||
      !pq_restore_order_chain_from_flags(&leader_sidecar) ||
      !pq_restored_order_chain_matches_optimized(optimized_without_first,
                                                 leader_sidecar) ||
      !pq_clone_order_chain_sidecar(leader_sidecar, &clone_sidecar) ||
      !pq_order_chain_sidecar_clone_matches(leader_sidecar, clone_sidecar) ||
      !pq_restored_order_chain_matches_optimized(optimized_without_first,
                                                 clone_sidecar)) {
    contract->status =
        PQOrderByFilesortContractStatus::UNSUPPORTED_MISSING_RESTORED_ORDER;
    contract->detail = "restored ORDER sidecar contract failed";
    return false;
  }

  contract->restored_order_ready = true;
  contract->restored_order_count = clone_sidecar.restored_nodes.size();
  contract->sidecar_clone_ready = true;
  contract->status = PQOrderByFilesortContractStatus::READY;
  contract->detail = "ready";
  return true;
}

static TABLE *pq_find_orderby_filesort_smoke_table(Query_block *query_block,
                                                   JOIN *join) {
  if (join != nullptr && join->best_ref != nullptr && join->primary_tables > 0 &&
      join->best_ref[0] != nullptr && join->best_ref[0]->table() != nullptr) {
    return join->best_ref[0]->table();
  }

  if (join != nullptr && join->qep_tab != nullptr && join->primary_tables > 0 &&
      join->qep_tab[0].table() != nullptr) {
    return join->qep_tab[0].table();
  }

  if (query_block != nullptr) {
    Table_ref *table_ref = query_block->get_table_list();
    if (table_ref != nullptr) return table_ref->table;
  }
  return nullptr;
}

static bool pq_run_orderby_filesort_construct_smoke(Query_block *query_block,
                                                    JOIN *join) {
  if (query_block == nullptr || join == nullptr || join->thd == nullptr ||
      join->order.empty() || join->order.order->next == nullptr) {
    return false;
  }

  TABLE *table = pq_find_orderby_filesort_smoke_table(query_block, join);
  if (table == nullptr) return false;

  PQ_owned_order_chain_sidecar leader_sidecar;
  PQ_owned_order_chain_sidecar clone_sidecar;
  ORDER_with_src optimized_without_first(join->order.order->next,
                                         join->order.src,
                                         join->order.is_const_optimized());
  if (!pq_copy_order_chain(join->order, &leader_sidecar) ||
      !pq_order_chain_copy_matches(join->order, leader_sidecar) ||
      !pq_record_order_chain_optimized_flags(optimized_without_first,
                                             &leader_sidecar) ||
      !pq_restore_order_chain_from_flags(&leader_sidecar) ||
      !pq_restored_order_chain_matches_optimized(optimized_without_first,
                                                 leader_sidecar) ||
      !pq_clone_order_chain_sidecar(leader_sidecar, &clone_sidecar) ||
      !pq_order_chain_sidecar_clone_matches(leader_sidecar, clone_sidecar) ||
      !pq_restored_order_chain_matches_optimized(optimized_without_first,
                                                 clone_sidecar)) {
    return false;
  }

  const uint restored_count = clone_sidecar.restored_nodes.size();
  if (restored_count == 0) return false;

  Filesort *filesort = new (join->thd->mem_root)
      Filesort(join->thd, {table}, /*keep_buffers=*/false,
               clone_sidecar.restored_head(), HA_POS_ERROR,
               /*remove_duplicates=*/false, /*force_sort_rowids=*/false,
               /*unwrap_rollup=*/false);
  return filesort != nullptr && filesort->sort_order_length() == restored_count;
}

static bool pq_run_orderby_sort_param_init_smoke(Query_block *query_block,
                                                 JOIN *join) {
  if (query_block == nullptr || join == nullptr || join->thd == nullptr ||
      join->order.empty() || join->order.order->next == nullptr) {
    return false;
  }

  TABLE *table = pq_find_orderby_filesort_smoke_table(query_block, join);
  if (table == nullptr) return false;

  PQ_owned_order_chain_sidecar leader_sidecar;
  PQ_owned_order_chain_sidecar clone_sidecar;
  ORDER_with_src optimized_without_first(join->order.order->next,
                                         join->order.src,
                                         join->order.is_const_optimized());
  if (!pq_copy_order_chain(join->order, &leader_sidecar) ||
      !pq_order_chain_copy_matches(join->order, leader_sidecar) ||
      !pq_record_order_chain_optimized_flags(optimized_without_first,
                                             &leader_sidecar) ||
      !pq_restore_order_chain_from_flags(&leader_sidecar) ||
      !pq_restored_order_chain_matches_optimized(optimized_without_first,
                                                 leader_sidecar) ||
      !pq_clone_order_chain_sidecar(leader_sidecar, &clone_sidecar) ||
      !pq_order_chain_sidecar_clone_matches(leader_sidecar, clone_sidecar) ||
      !pq_restored_order_chain_matches_optimized(optimized_without_first,
                                                 clone_sidecar)) {
    return false;
  }

  const uint restored_count = clone_sidecar.restored_nodes.size();
  if (restored_count == 0) return false;

  Filesort *filesort = new (join->thd->mem_root)
      Filesort(join->thd, {table}, /*keep_buffers=*/false,
               clone_sidecar.restored_head(), HA_POS_ERROR,
               /*remove_duplicates=*/false, /*force_sort_rowids=*/false,
               /*unwrap_rollup=*/false);
  if (filesort == nullptr || filesort->sort_order_length() != restored_count ||
      filesort->sortorder == nullptr) {
    return false;
  }

  Sort_param sort_param;
  const Bounds_checked_array<st_sort_field> sort_fields(
      filesort->sortorder, filesort->sort_order_length());
  sort_param.init_for_filesort(
      filesort, sort_fields,
      sortlength(join->thd, filesort->sortorder,
                 filesort->sort_order_length()),
      filesort->tables, HA_POS_ERROR, /*remove_duplicates=*/false);
  return sort_param.local_sortorder.size() == filesort->sort_order_length() &&
         sort_param.max_record_length() > 0;
}

static bool pq_run_orderby_sort_state_handoff_smoke(Query_block *query_block,
                                                    JOIN *join) {
  if (query_block == nullptr || join == nullptr || join->thd == nullptr ||
      join->order.empty() || join->order.order->next == nullptr) {
    return false;
  }

  TABLE *table = pq_find_orderby_filesort_smoke_table(query_block, join);
  if (table == nullptr) return false;

  const size_t filesorts_to_cleanup_size = join->filesorts_to_cleanup.size();
  const ORDER *join_order_head = join->order.order;
  const Explain_sort_clause join_order_src = join->order.src;
  const bool join_order_const_optimized = join->order.is_const_optimized();
  JOIN_TAB **best_ref = join->best_ref;
  QEP_TAB *qep_tab = join->qep_tab;
  TABLE *sort_by_table = join->sort_by_table;

  PQ_owned_order_chain_sidecar leader_sidecar;
  PQ_owned_order_chain_sidecar clone_sidecar;
  ORDER_with_src optimized_without_first(join->order.order->next,
                                         join->order.src,
                                         join->order.is_const_optimized());
  if (!pq_copy_order_chain(join->order, &leader_sidecar) ||
      !pq_order_chain_copy_matches(join->order, leader_sidecar) ||
      !pq_record_order_chain_optimized_flags(optimized_without_first,
                                             &leader_sidecar) ||
      !pq_restore_order_chain_from_flags(&leader_sidecar) ||
      !pq_restored_order_chain_matches_optimized(optimized_without_first,
                                                 leader_sidecar) ||
      !pq_clone_order_chain_sidecar(leader_sidecar, &clone_sidecar) ||
      !pq_order_chain_sidecar_clone_matches(leader_sidecar, clone_sidecar) ||
      !pq_restored_order_chain_matches_optimized(optimized_without_first,
                                                 clone_sidecar)) {
    return false;
  }

  const uint restored_count = clone_sidecar.restored_nodes.size();
  if (restored_count == 0) return false;

  Filesort *filesort = new (join->thd->mem_root)
      Filesort(join->thd, {table}, /*keep_buffers=*/false,
               clone_sidecar.restored_head(), HA_POS_ERROR,
               /*remove_duplicates=*/false, /*force_sort_rowids=*/false,
               /*unwrap_rollup=*/false);
  if (filesort == nullptr || filesort->sort_order_length() != restored_count ||
      filesort->sortorder == nullptr) {
    return false;
  }

  Sort_param sort_param;
  const Bounds_checked_array<st_sort_field> sort_fields(
      filesort->sortorder, filesort->sort_order_length());
  sort_param.init_for_filesort(
      filesort, sort_fields,
      sortlength(join->thd, filesort->sortorder,
                 filesort->sort_order_length()),
      filesort->tables, HA_POS_ERROR, /*remove_duplicates=*/false);
  if (sort_param.local_sortorder.size() != filesort->sort_order_length() ||
      sort_param.max_record_length() == 0) {
    return false;
  }

  const bool join_state_unchanged_before_handoff =
      join->filesorts_to_cleanup.size() == filesorts_to_cleanup_size &&
      join->order.order == join_order_head && join->order.src == join_order_src &&
      join->order.is_const_optimized() == join_order_const_optimized &&
      join->best_ref == best_ref && join->qep_tab == qep_tab &&
      join->sort_by_table == sort_by_table;
  if (!join_state_unchanged_before_handoff) return false;

  const uint32 workers =
      std::max<uint32>(1, static_cast<uint32>(join->thd->variables.parallel_default_dop));
  Exchange_sort sort_exchange;
  const bool first_handoff =
      !sort_exchange.run_orderby_sort_state_shape_handoff_smoke(
          workers, /*stable_output=*/true, /*index_sort=*/false,
          static_cast<uint32>(filesort->sort_order_length()),
          static_cast<uint32>(sort_param.max_record_length()),
          /*ref_length=*/1);
  const bool repeat_handoff =
      !sort_exchange.run_orderby_sort_state_shape_handoff_smoke(
      workers, /*stable_output=*/true, /*index_sort=*/false,
      static_cast<uint32>(filesort->sort_order_length()),
      static_cast<uint32>(sort_param.max_record_length()),
      /*ref_length=*/1);
  const bool join_state_unchanged_after_handoff =
      join->filesorts_to_cleanup.size() == filesorts_to_cleanup_size &&
      join->order.order == join_order_head && join->order.src == join_order_src &&
      join->order.is_const_optimized() == join_order_const_optimized &&
      join->best_ref == best_ref && join->qep_tab == qep_tab &&
      join->sort_by_table == sort_by_table;

  return first_handoff && repeat_handoff && join_state_unchanged_after_handoff;
}

struct PQ_copied_key_endpoint {
  key_range range{};
  std::vector<uchar> key;
  bool present{false};
};

static bool pq_copy_key_endpoint(const key_range &src,
                                 PQ_copied_key_endpoint *dst) {
  if (dst == nullptr) return false;
  dst->range = src;
  dst->key.clear();
  dst->present = src.keypart_map != 0;
  if (!dst->present) {
    dst->range.key = nullptr;
    dst->range.length = 0;
    return true;
  }
  if (src.length > 0 && src.key == nullptr) return false;
  dst->key.assign(src.key, src.key + src.length);
  dst->range.key = dst->key.empty() ? nullptr : dst->key.data();
  return true;
}

static bool pq_secondary_range_key_has_unsupported_parts(const TABLE *table,
                                                         uint keyno) {
  if (table == nullptr || table->s == nullptr || table->key_info == nullptr ||
      keyno >= table->s->keys) {
    return true;
  }
  const KEY &key = table->key_info[keyno];
  if (key.flags & (HA_SPATIAL | HA_MULTI_VALUED_KEY)) {
    return true;
  }
  for (uint i = 0; i < key.user_defined_key_parts; ++i) {
    if (key.key_part[i].key_part_flag & HA_REVERSE_SORT) {
      return true;
    }
  }
  return false;
}

static bool pq_secondary_covering_field_type_is_safe(const Field *field) {
  if (field == nullptr || field->is_nullable() || field->is_gcol() ||
      field->is_hidden() || field->is_field_for_functional_index()) {
    return false;
  }

  switch (field->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_YEAR:
      return true;
    default:
      return false;
  }
}

static bool pq_secondary_covering_read_set_is_safe(const TABLE *table,
                                                   uint keyno) {
  if (table == nullptr || table->s == nullptr || table->key_info == nullptr ||
      table->field == nullptr || table->read_set == nullptr ||
      keyno >= table->s->keys || keyno == table->s->primary_key) {
    return false;
  }

  const KEY &key = table->key_info[keyno];
  if (key.flags & (HA_SPATIAL | HA_MULTI_VALUED_KEY)) {
    return false;
  }

  bool saw_read_field = false;
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    if (field == nullptr || !bitmap_is_set(table->read_set, i)) {
      continue;
    }

    saw_read_field = true;
    if (!pq_secondary_covering_field_type_is_safe(field)) {
      return false;
    }

    bool found_full_keypart = false;
    for (uint part = 0; part < key.user_defined_key_parts; ++part) {
      const KEY_PART_INFO &key_part = key.key_part[part];
      if (key_part.field != field) {
        continue;
      }

      if ((key_part.key_part_flag &
           (HA_REVERSE_SORT | HA_PART_KEY_SEG | HA_VAR_LENGTH_PART |
            HA_BLOB_PART | HA_BIT_PART)) != 0 ||
          key_part.length != field->key_length()) {
        return false;
      }

      found_full_keypart = true;
      break;
    }

    if (!found_full_keypart) {
      return false;
    }
  }

  return saw_read_field;
}

static bool pq_secondary_ref_key_parts_are_safe(const TABLE *table, uint keyno,
                                                uint key_parts) {
  if (table == nullptr || table->s == nullptr || table->key_info == nullptr ||
      keyno >= table->s->keys || keyno == table->s->primary_key ||
      key_parts == 0) {
    return false;
  }

  const KEY &key = table->key_info[keyno];
  if (key_parts > key.user_defined_key_parts ||
      key.flags & (HA_SPATIAL | HA_MULTI_VALUED_KEY)) {
    return false;
  }

  for (uint part = 0; part < key_parts; ++part) {
    const KEY_PART_INFO &key_part = key.key_part[part];
    Field *field = key_part.field;
    if (!pq_secondary_covering_field_type_is_safe(field)) {
      return false;
    }
    if ((key_part.key_part_flag &
         (HA_REVERSE_SORT | HA_PART_KEY_SEG | HA_VAR_LENGTH_PART |
          HA_BLOB_PART | HA_BIT_PART)) != 0 ||
        key_part.length != field->key_length()) {
      return false;
    }
  }

  return true;
}

static void pq_maybe_run_saved_order_group_contract_smoke(
    Query_block *query_block, JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_saved_order_group_contract_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.saved_order_group_contract_attempts.fetch_add(
      1, std::memory_order_relaxed);

  PQSavedOrderGroupContract contract;
  if (!pq_build_saved_order_group_contract(query_block, join, &contract)) {
    pq_global_stats.saved_order_group_contract_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  }
}

static bool pq_saved_order_group_scalar_subset_matches(
    const PQSavedOrderGroupContract &lhs,
    const PQSavedOrderGroupContract &rhs) {
  return lhs.simple_order == rhs.simple_order &&
         lhs.simple_group == rhs.simple_group &&
         lhs.skip_sort_order == rhs.skip_sort_order &&
         lhs.need_tmp_before_win == rhs.need_tmp_before_win &&
         lhs.select_distinct == rhs.select_distinct;
}

static bool pq_saved_order_group_contract_matches(
    const PQSavedOrderGroupContract &lhs,
    const PQSavedOrderGroupContract &rhs) {
  return lhs.status == rhs.status && lhs.detail == rhs.detail &&
         lhs.has_order == rhs.has_order && lhs.has_group == rhs.has_group &&
         lhs.has_having == rhs.has_having && lhs.grouped == rhs.grouped &&
         lhs.group_optimized_away == rhs.group_optimized_away &&
         lhs.implicit_grouping == rhs.implicit_grouping &&
         lhs.need_tmp_before_win == rhs.need_tmp_before_win &&
         lhs.simple_group == rhs.simple_group &&
         lhs.simple_order == rhs.simple_order &&
         lhs.streaming_aggregation == rhs.streaming_aggregation &&
         lhs.skip_sort_order == rhs.skip_sort_order &&
         lhs.select_distinct == rhs.select_distinct &&
         lhs.ordered_index_usage == rhs.ordered_index_usage;
}

static void pq_maybe_run_saved_order_group_restore_smoke(
    Query_block *query_block, JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_saved_order_group_restore_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.saved_order_group_restore_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.saved_order_group_restore_smoke_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (query_block == nullptr || join == nullptr) {
    mark_unsupported();
    return;
  }

  PQSavedOrderGroupContract initial;
  (void)pq_build_saved_order_group_contract(query_block, join, &initial);
  if (initial.status !=
      PQSavedOrderGroupContractStatus::UNSUPPORTED_MISSING_SAVED_HELPERS) {
    mark_unsupported();
    return;
  }

  bool perturbation_changed_values = false;
  {
    struct Scalar_restore_guard {
      JOIN *join{nullptr};
      bool simple_order{false};
      bool simple_group{false};
      bool skip_sort_order{false};
      bool need_tmp_before_win{false};
      bool select_distinct{false};

      explicit Scalar_restore_guard(JOIN *join_arg)
          : join(join_arg),
            simple_order(join_arg->simple_order),
            simple_group(join_arg->simple_group),
            skip_sort_order(join_arg->skip_sort_order),
            need_tmp_before_win(join_arg->need_tmp_before_win),
            select_distinct(join_arg->select_distinct) {}

      ~Scalar_restore_guard() {
        join->simple_order = simple_order;
        join->simple_group = simple_group;
        join->skip_sort_order = skip_sort_order;
        join->need_tmp_before_win = need_tmp_before_win;
        join->select_distinct = select_distinct;
      }
    } restore_guard(join);

    join->simple_order = !restore_guard.simple_order;
    join->simple_group = !restore_guard.simple_group;
    join->skip_sort_order = !restore_guard.skip_sort_order;
    join->need_tmp_before_win = !restore_guard.need_tmp_before_win;
    join->select_distinct = !restore_guard.select_distinct;

    perturbation_changed_values =
        join->simple_order != restore_guard.simple_order &&
        join->simple_group != restore_guard.simple_group &&
        join->skip_sort_order != restore_guard.skip_sort_order &&
        join->need_tmp_before_win != restore_guard.need_tmp_before_win &&
        join->select_distinct != restore_guard.select_distinct;
  }

  PQSavedOrderGroupContract restored;
  (void)pq_build_saved_order_group_contract(query_block, join, &restored);
  if (perturbation_changed_values &&
      restored.status ==
          PQSavedOrderGroupContractStatus::UNSUPPORTED_MISSING_SAVED_HELPERS &&
      pq_saved_order_group_scalar_subset_matches(initial, restored)) {
    pq_global_stats.saved_order_group_restore_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    mark_unsupported();
  }
}

static void pq_maybe_run_saved_order_group_clone_copy_smoke(
    Query_block *query_block, JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_saved_order_group_clone_copy_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.saved_order_group_clone_copy_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.saved_order_group_clone_copy_smoke_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  PQSavedOrderGroupContract source;
  (void)pq_build_saved_order_group_contract(query_block, join, &source);
  if (source.status !=
      PQSavedOrderGroupContractStatus::UNSUPPORTED_MISSING_SAVED_HELPERS) {
    mark_unsupported();
    return;
  }

  PQSavedOrderGroupContract copied;
  if (pq_copy_saved_order_group_contract(source, &copied) &&
      copied.status ==
          PQSavedOrderGroupContractStatus::UNSUPPORTED_MISSING_SAVED_HELPERS &&
      pq_saved_order_group_contract_matches(source, copied)) {
    pq_global_stats.saved_order_group_clone_copy_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    mark_unsupported();
  }
}

static void pq_maybe_run_orderby_filesort_contract_smoke(
    Query_block *query_block, JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_orderby_filesort_contract_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.orderby_filesort_contract_attempts.fetch_add(
      1, std::memory_order_relaxed);

  PQOrderByFilesortContract contract;
  if (!pq_build_orderby_filesort_contract(query_block, join, &contract) &&
      contract.status ==
          PQOrderByFilesortContractStatus::UNSUPPORTED_MISSING_SAVED_HELPERS) {
    pq_global_stats.orderby_filesort_contract_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  }
}

static void pq_maybe_run_orderby_eligibility_contract_smoke(
    THD *thd, Query_block *query_block, JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_orderby_eligibility_contract_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.orderby_eligibility_contract_attempts.fetch_add(
      1, std::memory_order_relaxed);

  PQOrderByEligibilityContract contract;
  if (pq_build_orderby_eligibility_contract(thd, query_block, join, &contract) &&
      contract.future_candidate_disabled()) {
    pq_global_stats.orderby_eligibility_contract_candidate_disabled.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    pq_global_stats.orderby_eligibility_contract_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  }
}

static void pq_maybe_run_orderby_execution_preflight_smoke(
    THD *thd, Query_block *query_block, JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_orderby_execution_preflight_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.orderby_execution_preflight_attempts.fetch_add(
      1, std::memory_order_relaxed);

  PQOrderByExecutionPreflight preflight;
  if (pq_build_orderby_execution_preflight(thd, query_block, join,
                                           &preflight)) {
    pq_global_stats.orderby_execution_preflight_ready.fetch_add(
        1, std::memory_order_relaxed);
  } else if (preflight.blocked_by_execution_disabled()) {
    pq_global_stats.orderby_execution_preflight_blocked.fetch_add(
        1, std::memory_order_relaxed);
    if (!preflight.saved_order_group_runtime_ready) {
      pq_global_stats.orderby_execution_preflight_missing_saved_order_state
          .fetch_add(1, std::memory_order_relaxed);
    }
    if (!preflight.filesort_runtime_ready) {
      pq_global_stats.orderby_execution_preflight_missing_filesort_runtime
          .fetch_add(1, std::memory_order_relaxed);
    }
    if (!preflight.sort_param_runtime_ready) {
      pq_global_stats.orderby_execution_preflight_missing_sort_param_runtime
          .fetch_add(1, std::memory_order_relaxed);
    }
    if (!preflight.worker_order_frame_producer_ready) {
      pq_global_stats
          .orderby_execution_preflight_missing_worker_frame_producer.fetch_add(
              1, std::memory_order_relaxed);
    }
    if (!preflight.exchange_sort_heap_read_ready) {
      pq_global_stats.orderby_execution_preflight_missing_exchange_heap_read
          .fetch_add(1, std::memory_order_relaxed);
    }
    if (!preflight.leader_materialization_ready) {
      pq_global_stats
          .orderby_execution_preflight_missing_leader_materialization.fetch_add(
              1, std::memory_order_relaxed);
    }
    if (!preflight.rowid_tiebreak_ready) {
      pq_global_stats.orderby_execution_preflight_missing_rowid_tiebreak
          .fetch_add(1, std::memory_order_relaxed);
    }
    if (!preflight.default_ordered_read_ready) {
      pq_global_stats.orderby_execution_preflight_missing_default_ordered_read
          .fetch_add(1, std::memory_order_relaxed);
    }
    if (!preflight.kill_detach_error_diagnostics_ready) {
      pq_global_stats.orderby_execution_preflight_missing_error_diagnostics
          .fetch_add(1, std::memory_order_relaxed);
    }
  }
}

static void pq_maybe_run_orderby_filesort_restored_order_contract_smoke(
    Query_block *query_block, JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_orderby_filesort_restored_order_contract_smoke",
                  enabled = true;);
  if (!enabled) return;

  pq_global_stats.orderby_filesort_restored_order_contract_attempts.fetch_add(
      1, std::memory_order_relaxed);

  PQOrderByFilesortContract contract;
  if (pq_build_orderby_filesort_restored_order_contract(query_block, join,
                                                        &contract) &&
      contract.ready() && contract.restored_order_ready &&
      contract.restored_order_count > 0 && contract.sidecar_clone_ready) {
    pq_global_stats.orderby_filesort_restored_order_contract_success.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    pq_global_stats.orderby_filesort_restored_order_contract_unsupported
        .fetch_add(1, std::memory_order_relaxed);
  }
}

static void pq_maybe_run_orderby_filesort_construct_smoke(
    Query_block *query_block, JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_orderby_filesort_construct_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.orderby_filesort_construct_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (pq_run_orderby_filesort_construct_smoke(query_block, join)) {
    pq_global_stats.orderby_filesort_construct_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    pq_global_stats.orderby_filesort_construct_smoke_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  }
}

static void pq_maybe_run_orderby_sort_param_init_smoke(Query_block *query_block,
                                                       JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_orderby_sort_param_init_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.orderby_sort_param_init_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (pq_run_orderby_sort_param_init_smoke(query_block, join)) {
    pq_global_stats.orderby_sort_param_init_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    pq_global_stats.orderby_sort_param_init_smoke_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  }
}

static void pq_maybe_run_orderby_sort_state_handoff_smoke(
    Query_block *query_block, JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_orderby_sort_state_handoff_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.orderby_sort_state_handoff_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (pq_run_orderby_sort_state_handoff_smoke(query_block, join)) {
    pq_global_stats.orderby_sort_state_handoff_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    pq_global_stats.orderby_sort_state_handoff_smoke_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  }
}

static void pq_maybe_run_saved_order_chain_copy_smoke(
    Query_block *query_block [[maybe_unused]], JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_saved_order_chain_copy_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.saved_order_chain_copy_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.saved_order_chain_copy_smoke_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (join == nullptr || join->order.empty()) {
    mark_unsupported();
    return;
  }

  PQ_owned_order_chain_sidecar copied;
  if (pq_copy_order_chain(join->order, &copied) &&
      !copied.nodes.empty() &&
      pq_order_chain_copy_matches(join->order, copied)) {
    pq_global_stats.saved_order_chain_copy_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    mark_unsupported();
  }
}

static void pq_maybe_run_saved_order_chain_flags_smoke(
    Query_block *query_block [[maybe_unused]], JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_saved_order_chain_flags_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.saved_order_chain_flags_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.saved_order_chain_flags_smoke_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (join == nullptr || join->order.empty() || join->order.order->next == nullptr) {
    mark_unsupported();
    return;
  }

  PQ_owned_order_chain_sidecar copied;
  ORDER_with_src optimized_without_first(join->order.order->next,
                                         join->order.src,
                                         join->order.is_const_optimized());
  if (pq_copy_order_chain(join->order, &copied) &&
      pq_order_chain_copy_matches(join->order, copied) &&
      pq_record_order_chain_optimized_flags(optimized_without_first, &copied) &&
      copied.optimized_flags.size() >= 2 && !copied.optimized_flags[0] &&
      copied.optimized_flags[1]) {
    pq_global_stats.saved_order_chain_flags_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    mark_unsupported();
  }
}

static void pq_maybe_run_saved_order_chain_restore_smoke(
    Query_block *query_block [[maybe_unused]], JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_saved_order_chain_restore_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.saved_order_chain_restore_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.saved_order_chain_restore_smoke_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (join == nullptr || join->order.empty() ||
      join->order.order->next == nullptr) {
    mark_unsupported();
    return;
  }

  PQ_owned_order_chain_sidecar sidecar;
  ORDER_with_src optimized_without_first(join->order.order->next,
                                         join->order.src,
                                         join->order.is_const_optimized());
  if (pq_copy_order_chain(join->order, &sidecar) &&
      pq_order_chain_copy_matches(join->order, sidecar) &&
      pq_record_order_chain_optimized_flags(optimized_without_first, &sidecar) &&
      pq_restore_order_chain_from_flags(&sidecar) &&
      pq_restored_order_chain_matches_optimized(optimized_without_first,
                                                sidecar)) {
    pq_global_stats.saved_order_chain_restore_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    mark_unsupported();
  }
}

static void pq_maybe_run_saved_order_chain_clone_copy_smoke(
    Query_block *query_block [[maybe_unused]], JOIN *join) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_saved_order_chain_clone_copy_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.saved_order_chain_clone_copy_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.saved_order_chain_clone_copy_smoke_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (join == nullptr || join->order.empty() ||
      join->order.order->next == nullptr) {
    mark_unsupported();
    return;
  }

  PQ_owned_order_chain_sidecar leader_sidecar;
  PQ_owned_order_chain_sidecar clone_sidecar;
  ORDER_with_src optimized_without_first(join->order.order->next,
                                         join->order.src,
                                         join->order.is_const_optimized());
  if (pq_copy_order_chain(join->order, &leader_sidecar) &&
      pq_order_chain_copy_matches(join->order, leader_sidecar) &&
      pq_record_order_chain_optimized_flags(optimized_without_first,
                                            &leader_sidecar) &&
      pq_restore_order_chain_from_flags(&leader_sidecar) &&
      pq_clone_order_chain_sidecar(leader_sidecar, &clone_sidecar) &&
      pq_order_chain_sidecar_clone_matches(leader_sidecar, clone_sidecar) &&
      pq_order_chain_copy_matches(join->order, clone_sidecar) &&
      pq_restored_order_chain_matches_optimized(optimized_without_first,
                                                clone_sidecar) &&
      pq_runtime_saved_order_attach_smoke(join->order,
                                          optimized_without_first)) {
    pq_global_stats.saved_order_chain_clone_copy_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    mark_unsupported();
  }
}

static void pq_maybe_run_secondary_range_partition_smoke(
    THD *thd, TABLE *table, AccessPath *range_scan, uint keyno) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_secondary_range_partition_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.secondary_range_clone_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_failed = []() {
    pq_global_stats.secondary_range_clone_failed.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (thd == nullptr || table == nullptr || table->s == nullptr ||
      table->file == nullptr || range_scan == nullptr ||
      range_scan->type != AccessPath::INDEX_RANGE_SCAN ||
      keyno >= table->s->keys || keyno == table->s->primary_key ||
      table->part_info != nullptr || table->file->pushed_idx_cond != nullptr) {
    mark_failed();
    return;
  }

  const auto &param = range_scan->index_range_scan();
  if (param.reverse || param.geometry || param.num_ranges != 1 ||
      param.ranges == nullptr ||
      pq_secondary_range_key_has_unsupported_parts(table, keyno)) {
    mark_failed();
    return;
  }

  QUICK_RANGE *quick_range = param.ranges[0];
  if (quick_range == nullptr) {
    mark_failed();
    return;
  }

  key_range start_key{};
  key_range end_key{};
  quick_range->make_min_endpoint(&start_key);
  quick_range->make_max_endpoint(&end_key);

  PQ_copied_key_endpoint copied_start;
  PQ_copied_key_endpoint copied_end;
  if (!pq_copy_key_endpoint(start_key, &copied_start) ||
      !pq_copy_key_endpoint(end_key, &copied_end)) {
    mark_failed();
    return;
  }

  uint ranges_built = 0;
  uint requested_dop = thd->variables.parallel_default_dop;
  if (requested_dop == 0) requested_dop = 1;

  const int error = table->file->pq_secondary_range_partition_smoke(
      thd, keyno, copied_start.present ? &copied_start.range : nullptr,
      copied_end.present ? &copied_end.range : nullptr, requested_dop,
      &ranges_built);
  if (error != 0) {
    mark_failed();
    return;
  }

  pq_global_stats.secondary_ranges_built.fetch_add(
      ranges_built, std::memory_order_relaxed);
}

static void pq_maybe_run_secondary_visibility_smoke(THD *thd, TABLE *table,
                                                    AccessPath *range_scan,
                                                    uint keyno) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_secondary_visibility_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.secondary_visibility_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.secondary_visibility_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (thd == nullptr || table == nullptr || table->s == nullptr ||
      table->file == nullptr || range_scan == nullptr ||
      range_scan->type != AccessPath::INDEX_RANGE_SCAN ||
      keyno >= table->s->keys || keyno == table->s->primary_key ||
      table->part_info != nullptr || table->file->pushed_idx_cond != nullptr) {
    mark_unsupported();
    return;
  }

  const auto &param = range_scan->index_range_scan();
  if (param.reverse || param.geometry || param.num_ranges != 1 ||
      param.ranges == nullptr ||
      pq_secondary_range_key_has_unsupported_parts(table, keyno)) {
    mark_unsupported();
    return;
  }

  if (table->file->pq_secondary_visibility_smoke(thd, keyno) != 0) {
    mark_unsupported();
  } else {
    pq_global_stats.secondary_visibility_supported.fetch_add(
        1, std::memory_order_relaxed);
  }
}

static void pq_maybe_run_secondary_visibility_one_record_smoke(
    THD *thd, TABLE *table, AccessPath *range_scan, uint keyno) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_secondary_visibility_one_record_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.secondary_visibility_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.secondary_visibility_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (thd == nullptr || table == nullptr || table->s == nullptr ||
      table->file == nullptr || range_scan == nullptr ||
      range_scan->type != AccessPath::INDEX_RANGE_SCAN ||
      keyno >= table->s->keys || keyno == table->s->primary_key ||
      table->part_info != nullptr || table->file->pushed_idx_cond != nullptr) {
    mark_unsupported();
    return;
  }

  const auto &param = range_scan->index_range_scan();
  if (param.reverse || param.geometry || param.num_ranges != 1 ||
      param.ranges == nullptr ||
      pq_secondary_range_key_has_unsupported_parts(table, keyno)) {
    mark_unsupported();
    return;
  }

  QUICK_RANGE *quick_range = param.ranges[0];
  if (quick_range == nullptr) {
    mark_unsupported();
    return;
  }

  key_range start_key{};
  key_range end_key{};
  quick_range->make_min_endpoint(&start_key);
  quick_range->make_max_endpoint(&end_key);

  PQ_copied_key_endpoint copied_start;
  PQ_copied_key_endpoint copied_end;
  if (!pq_copy_key_endpoint(start_key, &copied_start) ||
      !pq_copy_key_endpoint(end_key, &copied_end)) {
    mark_unsupported();
    return;
  }

  const int error = table->file->pq_secondary_visibility_one_record_smoke(
      thd, keyno, copied_start.present ? &copied_start.range : nullptr,
      copied_end.present ? &copied_end.range : nullptr);
  if (error != 0) {
    mark_unsupported();
  } else {
    pq_global_stats.secondary_visibility_supported.fetch_add(
        1, std::memory_order_relaxed);
  }
}

static void pq_maybe_run_secondary_covering_one_row_smoke(
    THD *thd, TABLE *table, AccessPath *range_scan, uint keyno) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_secondary_covering_one_row_smoke", enabled = true;);
  if (!enabled) return;

  pq_global_stats.secondary_visibility_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.secondary_visibility_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (thd == nullptr || table == nullptr || table->s == nullptr ||
      table->file == nullptr || range_scan == nullptr ||
      range_scan->type != AccessPath::INDEX_RANGE_SCAN ||
      keyno >= table->s->keys || keyno == table->s->primary_key ||
      table->part_info != nullptr || table->file->pushed_idx_cond != nullptr) {
    mark_unsupported();
    return;
  }

  const auto &param = range_scan->index_range_scan();
  if (param.reverse || param.geometry || param.num_ranges != 1 ||
      param.ranges == nullptr ||
      pq_secondary_range_key_has_unsupported_parts(table, keyno) ||
      !pq_secondary_covering_read_set_is_safe(table, keyno)) {
    mark_unsupported();
    return;
  }

  QUICK_RANGE *quick_range = param.ranges[0];
  if (quick_range == nullptr) {
    mark_unsupported();
    return;
  }

  key_range start_key{};
  key_range end_key{};
  quick_range->make_min_endpoint(&start_key);
  quick_range->make_max_endpoint(&end_key);

  PQ_copied_key_endpoint copied_start;
  PQ_copied_key_endpoint copied_end;
  if (!pq_copy_key_endpoint(start_key, &copied_start) ||
      !pq_copy_key_endpoint(end_key, &copied_end)) {
    mark_unsupported();
    return;
  }

  const int error = table->file->pq_secondary_covering_one_row_smoke(
      thd, keyno, copied_start.present ? &copied_start.range : nullptr,
      copied_end.present ? &copied_end.range : nullptr);
  if (error != 0) {
    mark_unsupported();
  } else {
    pq_global_stats.secondary_rows_materialized_smoke.fetch_add(
        1, std::memory_order_relaxed);
  }
}

static void pq_maybe_run_secondary_covering_range_smoke(
    THD *thd, TABLE *table, AccessPath *range_scan, uint keyno) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_secondary_covering_range_materialize_smoke",
                  enabled = true;);
  if (!enabled) return;

  pq_global_stats.secondary_visibility_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.secondary_visibility_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (thd == nullptr || table == nullptr || table->s == nullptr ||
      table->file == nullptr || range_scan == nullptr ||
      range_scan->type != AccessPath::INDEX_RANGE_SCAN ||
      keyno >= table->s->keys || keyno == table->s->primary_key ||
      table->part_info != nullptr || table->file->pushed_idx_cond != nullptr) {
    mark_unsupported();
    return;
  }

  const auto &param = range_scan->index_range_scan();
  if (param.reverse || param.geometry || param.num_ranges != 1 ||
      param.ranges == nullptr ||
      pq_secondary_range_key_has_unsupported_parts(table, keyno) ||
      !pq_secondary_covering_read_set_is_safe(table, keyno)) {
    mark_unsupported();
    return;
  }

  QUICK_RANGE *quick_range = param.ranges[0];
  if (quick_range == nullptr) {
    mark_unsupported();
    return;
  }

  key_range start_key{};
  key_range end_key{};
  quick_range->make_min_endpoint(&start_key);
  quick_range->make_max_endpoint(&end_key);

  PQ_copied_key_endpoint copied_start;
  PQ_copied_key_endpoint copied_end;
  if (!pq_copy_key_endpoint(start_key, &copied_start) ||
      !pq_copy_key_endpoint(end_key, &copied_end)) {
    mark_unsupported();
    return;
  }

  uint row_count = 0;
  const int error = table->file->pq_secondary_covering_range_smoke(
      thd, keyno, copied_start.present ? &copied_start.range : nullptr,
      copied_end.present ? &copied_end.range : nullptr, &row_count);
  if (error != 0) {
    mark_unsupported();
  } else {
    pq_global_stats.secondary_rows_materialized_smoke.fetch_add(
        row_count, std::memory_order_relaxed);
  }
}

static void pq_maybe_run_secondary_noncovering_icp_one_row_smoke(
    THD *thd, TABLE *table, AccessPath *range_scan, uint keyno) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_secondary_noncovering_icp_one_record_smoke",
                  enabled = true;);
  if (!enabled) return;

  pq_global_stats.secondary_visibility_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.secondary_visibility_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (thd == nullptr || table == nullptr || table->s == nullptr ||
      table->file == nullptr || range_scan == nullptr ||
      range_scan->type != AccessPath::INDEX_RANGE_SCAN ||
      keyno >= table->s->keys || keyno == table->s->primary_key ||
      table->part_info != nullptr || table->file->pushed_idx_cond == nullptr ||
      table->file->pushed_idx_cond_keyno != keyno) {
    mark_unsupported();
    return;
  }

  const auto &param = range_scan->index_range_scan();
  if (param.reverse || param.geometry || param.num_ranges != 1 ||
      param.ranges == nullptr ||
      pq_secondary_range_key_has_unsupported_parts(table, keyno) ||
      pq_secondary_covering_read_set_is_safe(table, keyno)) {
    mark_unsupported();
    return;
  }

  QUICK_RANGE *quick_range = param.ranges[0];
  if (quick_range == nullptr) {
    mark_unsupported();
    return;
  }

  key_range start_key{};
  key_range end_key{};
  quick_range->make_min_endpoint(&start_key);
  quick_range->make_max_endpoint(&end_key);

  PQ_copied_key_endpoint copied_start;
  PQ_copied_key_endpoint copied_end;
  if (!pq_copy_key_endpoint(start_key, &copied_start) ||
      !pq_copy_key_endpoint(end_key, &copied_end)) {
    mark_unsupported();
    return;
  }

  bool materialized = false;
  const int error = table->file->pq_secondary_noncovering_icp_one_row_smoke(
      thd, keyno, copied_start.present ? &copied_start.range : nullptr,
      copied_end.present ? &copied_end.range : nullptr, &materialized);
  if (error != 0) {
    mark_unsupported();
  } else if (materialized) {
    pq_global_stats.secondary_rows_materialized_smoke.fetch_add(
        1, std::memory_order_relaxed);
  }
}

static void pq_maybe_run_secondary_covering_ref_smoke(THD *thd, TABLE *table,
                                                      Index_lookup *ref) {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_secondary_covering_ref_smoke", enabled = true;);
  DBUG_EXECUTE_IF("pq_secondary_covering_ref_multi_key_smoke",
                  enabled = true;);
  if (!enabled) return;

  pq_global_stats.secondary_visibility_attempts.fetch_add(
      1, std::memory_order_relaxed);

  auto mark_unsupported = []() {
    pq_global_stats.secondary_visibility_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (thd == nullptr || table == nullptr || table->s == nullptr ||
      table->file == nullptr || ref == nullptr || ref->key < 0 ||
      static_cast<uint>(ref->key) >= table->s->keys ||
      static_cast<uint>(ref->key) == table->s->primary_key ||
      table->part_info != nullptr || table->file->pushed_idx_cond != nullptr ||
      ref->key_parts == 0 || ref->key_length == 0 ||
      ref->key_buff == nullptr || ref->key_copy == nullptr ||
      ref->cond_guards == nullptr || ref->depend_map != 0 ||
      ref->disable_cache || ref->keypart_hash != nullptr ||
      !pq_secondary_covering_read_set_is_safe(table,
                                              static_cast<uint>(ref->key)) ||
      !pq_secondary_ref_key_parts_are_safe(table, static_cast<uint>(ref->key),
                                           ref->key_parts)) {
    mark_unsupported();
    return;
  }

  for (uint part = 0; part < ref->key_parts; ++part) {
    if (ref->key_copy[part] != nullptr || ref->cond_guards[part] != nullptr) {
      mark_unsupported();
      return;
    }
  }

  if (ref->impossible_null_ref()) {
    mark_unsupported();
    return;
  }

  const key_part_map keypart_map = make_prev_keypart_map(ref->key_parts);
  if (calculate_key_len(table, ref->key, keypart_map) != ref->key_length) {
    mark_unsupported();
    return;
  }

  key_range ref_key{};
  ref_key.key = ref->key_buff;
  ref_key.length = ref->key_length;
  ref_key.keypart_map = keypart_map;
  ref_key.flag = HA_READ_KEY_EXACT;

  PQ_copied_key_endpoint copied_ref_key;
  if (!pq_copy_key_endpoint(ref_key, &copied_ref_key) ||
      !copied_ref_key.present) {
    mark_unsupported();
    return;
  }

  uint row_count = 0;
  const int error = table->file->pq_secondary_covering_ref_smoke(
      thd, static_cast<uint>(ref->key), &copied_ref_key.range, &row_count);
  if (error != 0) {
    mark_unsupported();
  } else {
    pq_global_stats.secondary_rows_materialized_smoke.fetch_add(
        row_count, std::memory_order_relaxed);
  }
}

/**
  Helper: set info and return false (not eligible).
  @param info  Output disqualification info
  @param r     The reason
  @param d     Detail string (static constant)
  @retval false  Always returns false (not eligible)
*/
static bool pq_reject(PQUnsuiteInfo *info, PQUnsuiteReason r,
                      const char *d = nullptr) {
  info->reason = r;
  info->detail = d;
  return false;
}

/**
  Check whether a single leaf table meets PQ eligibility requirements.

  MVP eligibility requires:
  - InnoDB engine
  - Non-temporary
  - Non-partitioned
  - Non-fulltext
  - Non-view/derived/CTE

  @param thd         Thread context
  @param table_ref   The Table_ref (leaf table) to check
  @param info        Output: set reason if disqualified

  @retval true   Table passes PQ eligibility
  @retval false  Table is disqualified
*/
static bool pq_check_single_table(Table_ref *table_ref,
                                  PQUnsuiteInfo *info) {
  // Must not be a view or derived table
  if (table_ref->is_view_or_derived()) {
    return pq_reject(info, PQUnsuiteReason::HAS_DERIVED_OR_VIEW,
                     "base table is view or derived");
  }

  TABLE *table = table_ref->table;
  if (table == nullptr) {
    // Table not opened yet - cannot reliably check. Fallback.
    return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_BY_PHASE1,
                     "table not opened");
  }

  TABLE_SHARE *share = table->s;

  // Must be InnoDB engine
  if (ha_legacy_type(share->db_type()) != DB_TYPE_INNODB) {
    return pq_reject(info, PQUnsuiteReason::NON_INNODB,
                     "table engine is not InnoDB");
  }

  // Must not be temporary
  if (share->tmp_table != NO_TMP_TABLE) {
    return pq_reject(info, PQUnsuiteReason::TEMPORARY_TABLE,
                     "table is temporary");
  }

  // Must not be partitioned
  if (share->m_part_info != nullptr) {
    pq_global_stats.partition_reject_probes.fetch_add(
        1, std::memory_order_relaxed);
    return pq_reject(info, PQUnsuiteReason::PARTITIONED_TABLE,
                     "table is partitioned");
  }

  // Must not have fulltext index involvement
  // Check: if the table has any fulltext key defined, conservatively reject.
  // A more precise check would verify if the query actually uses FTS,
  // but Phase 1 errs on the side of safety.
  for (uint i = 0; i < share->keys; i++) {
    if ((share->key_info[i].flags & HA_FULLTEXT) != 0) {
      return pq_reject(info, PQUnsuiteReason::FULLTEXT,
                       "table has fulltext index");
    }
  }

  return true;  // Table passes all checks
}

/**
  Check whether the query block's access path is a full table scan.

  MVP only supports full table scan on the single base table.
  Any secondary index access, range scan, ref access, ICP, or MRR
  must fallback to serial.

  Phase 7: improved robustness for EXPLAIN paths. We try both
  best_ref[] (available during optimization) and qep_tab[]
  (available after final plan construction). Either source
  that provides a valid JT_ALL type is sufficient to confirm full table scan
  eligibility. Any const, range, ref, index or covering-index path must remain
  serial in V1.

  @param join  The JOIN object (may be nullptr if not yet optimized)
  @param info  Output: set reason if disqualified
  @param allow_tmp_tables  Whether a narrowly gated GROUP BY candidate may
                           continue with optimizer-created tmp tables

  @retval true   Access path is full table scan
  @retval false  Access path is something else or cannot be determined
*/
static bool pq_check_full_table_scan(JOIN *join, PQUnsuiteInfo *info,
                                     bool allow_tmp_tables) {
  if (join == nullptr) {
    // JOIN not yet constructed - cannot check access path. Fallback.
    return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_BY_PHASE1,
                     "JOIN not available for access path check");
  }

  // If there are tmp_tables, the query involves materialization steps.
  if (join->tmp_tables > 0 && !allow_tmp_tables) {
    return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_BY_PHASE1,
                     "tmp tables present");
  }

  // Check the access type using best_ref[] (from make_join_plan).
  // best_ref[] is populated after make_join_plan() and contains
  // the optimizer's selected access path for each table. The preceding
  // eligibility checks already require a single base table, so best_ref[0]
  // is enough to catch const, range, index and full-scan decisions.
  join_type access_type = JT_UNKNOWN;
  TABLE *candidate_table = nullptr;
  uint candidate_index = MAX_KEY;
  AccessPath *candidate_range_scan = nullptr;
  Index_lookup *candidate_ref = nullptr;

  // Try best_ref[] first (available during optimization).
  if (join->best_ref != nullptr && join->primary_tables > 0 &&
      join->best_ref[0] != nullptr) {
    JOIN_TAB *first_tab = join->best_ref[0];
    access_type = first_tab->type();
    candidate_table = first_tab->table();
    candidate_index = first_tab->index();
    candidate_range_scan = first_tab->range_scan();
    candidate_ref = &first_tab->ref();
  }

  // If best_ref[] didn't give a valid type, try qep_tab[]
  // (available after the final plan is constructed).
  if (access_type == JT_UNKNOWN && join->qep_tab != nullptr &&
      join->primary_tables > 0) {
    access_type = join->qep_tab[0].type();
    candidate_table = join->qep_tab[0].table();
    candidate_index = join->qep_tab[0].index();
    candidate_range_scan = join->qep_tab[0].range_scan();
    candidate_ref = &join->qep_tab[0].ref();
  }

  // If we couldn't determine the access type from either source,
  // conservatively fallback.
  if (access_type == JT_UNKNOWN) {
    return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_BY_PHASE1,
                     "cannot determine access path type");
  }

  // Only JT_ALL (full table scan) is eligible for PQ in MVP.
  // Any other access type (ref, range, index scan, etc.) must fallback.
  if (access_type != JT_ALL) {
    if (access_type == JT_RANGE && candidate_range_scan != nullptr &&
        candidate_range_scan->type == AccessPath::INDEX_RANGE_SCAN) {
      candidate_index = candidate_range_scan->index_range_scan().index;
      if (candidate_table != nullptr && candidate_table->s != nullptr &&
          candidate_index != MAX_KEY &&
          candidate_index < candidate_table->s->keys &&
          candidate_index != candidate_table->s->primary_key) {
        pq_global_stats.secondary_range_probe_attempts.fetch_add(
            1, std::memory_order_relaxed);
        pq_global_stats.secondary_range_probe_unsupported.fetch_add(
            1, std::memory_order_relaxed);
        pq_maybe_run_secondary_range_partition_smoke(
            join->thd, candidate_table, candidate_range_scan, candidate_index);
        pq_maybe_run_secondary_visibility_smoke(
            join->thd, candidate_table, candidate_range_scan, candidate_index);
        pq_maybe_run_secondary_visibility_one_record_smoke(
            join->thd, candidate_table, candidate_range_scan, candidate_index);
        pq_maybe_run_secondary_covering_one_row_smoke(
            join->thd, candidate_table, candidate_range_scan, candidate_index);
        pq_maybe_run_secondary_covering_range_smoke(
            join->thd, candidate_table, candidate_range_scan, candidate_index);
        pq_maybe_run_secondary_noncovering_icp_one_row_smoke(
            join->thd, candidate_table, candidate_range_scan, candidate_index);
      }
    } else if (access_type == JT_REF) {
      pq_maybe_run_secondary_covering_ref_smoke(join->thd, candidate_table,
                                                candidate_ref);
    }
    return pq_reject(info, PQUnsuiteReason::NON_FULL_TABLE_SCAN,
                     pq_unsuite_reason_to_string(
                         PQUnsuiteReason::NON_FULL_TABLE_SCAN));
  }

  return true;  // Passed full table scan check
}

bool pq_check_query_block_eligible(THD *thd, Query_block *query_block,
                                   JOIN *join, PQUnsuiteInfo *info) {
  // Initialize info to eligible state
  info->reset();

  // ================================================================
  // 1. Global/session switch: parallel_query must be ON
  // ================================================================
  bool explicit_groupby_dop1_candidate = false;

  if (!thd->variables.parallel_query) {
    return pq_reject(info, PQUnsuiteReason::DISABLED,
                     "parallel_query is OFF");
  }

  // ================================================================
  // 2. This THD must not be a PQ worker (no recursive parallelism)
  // ================================================================
  if (thd->pq_is_worker) {
    return pq_reject(info, PQUnsuiteReason::WORKER_THD,
                     "THD is already a PQ worker");
  }

  // ================================================================
  // 3. Must use traditional optimizer, not hypergraph
  // ================================================================
  if (thd->lex->using_hypergraph_optimizer()) {
    return pq_reject(info, PQUnsuiteReason::HYPERGRAPH_OPTIMIZER,
                     "hypergraph optimizer in use");
  }

  // ================================================================
  // 4. Must be a simple SELECT statement
  // ================================================================
  if (thd->lex->sql_command != SQLCOM_SELECT) {
    return pq_reject(info, PQUnsuiteReason::NOT_SELECT,
                     "statement is not SELECT");
  }

  // ================================================================
  // 5. Must be a simple query block (no UNION, no subquery wrapping)
  // ================================================================
  if (!query_block->is_simple_query_block()) {
    return pq_reject(info, PQUnsuiteReason::MULTI_QUERY_BLOCK,
                     "query has multiple query blocks (UNION/subquery)");
  }

  // ================================================================
  // 6. Must have at least one table
  // ================================================================
  if (!query_block->has_tables()) {
    return pq_reject(info, PQUnsuiteReason::NO_TABLE,
                     "no tables in query block");
  }

  // ================================================================
  // 7. Must have exactly one base table (no joins)
  // ================================================================
  if (query_block->table_count() > 1) {
    return pq_reject(info, PQUnsuiteReason::MULTI_TABLE,
                     "more than one table");
  }

  // ================================================================
  // 8. Must not have DISTINCT
  // ================================================================
  if (query_block->is_distinct()) {
    return pq_reject(info, PQUnsuiteReason::HAS_DISTINCT,
                     "query has DISTINCT");
  }

  // ================================================================
  // 9. Must not have ORDER BY
  // ================================================================
  if (query_block->is_ordered()) {
    pq_maybe_run_saved_order_group_contract_smoke(query_block, join);
    pq_maybe_run_saved_order_group_restore_smoke(query_block, join);
    pq_maybe_run_saved_order_group_clone_copy_smoke(query_block, join);
    pq_maybe_run_orderby_filesort_contract_smoke(query_block, join);
    pq_maybe_run_orderby_eligibility_contract_smoke(thd, query_block, join);
    pq_maybe_run_orderby_execution_preflight_smoke(thd, query_block, join);
    pq_maybe_run_orderby_filesort_restored_order_contract_smoke(query_block,
                                                                join);
    pq_maybe_run_orderby_filesort_construct_smoke(query_block, join);
    pq_maybe_run_orderby_sort_param_init_smoke(query_block, join);
    pq_maybe_run_orderby_sort_state_handoff_smoke(query_block, join);
    pq_maybe_run_saved_order_chain_copy_smoke(query_block, join);
    pq_maybe_run_saved_order_chain_flags_smoke(query_block, join);
    pq_maybe_run_saved_order_chain_restore_smoke(query_block, join);
    pq_maybe_run_saved_order_chain_clone_copy_smoke(query_block, join);
    return pq_reject(info, PQUnsuiteReason::HAS_ORDER_BY,
                     "query has ORDER BY");
  }

  // ================================================================
  // 10. Explicit GROUP BY is still serial by default. V2-12A-3 allows a
  //     narrowly gated DOP=1 candidate to reach the GROUP BY factory
  //     diagnostics; the factory still returns nullptr unless a later
  //     substep creates a real PQ group iterator.
  // ================================================================
  if (query_block->is_explicitly_grouped()) {
    if (query_block->olap != UNSPECIFIED_OLAP_TYPE) {
      return pq_reject(info, PQUnsuiteReason::GROUP_BY_ROLLUP,
                       "GROUP BY WITH ROLLUP is not supported");
    }

    if (query_block->having_cond() != nullptr) {
      return pq_reject(info, PQUnsuiteReason::GROUP_BY_HAVING,
                       "GROUP BY with HAVING is not supported");
    }

    if (!is_simple_order(query_block->group_list.first)) {
      return pq_reject(info, PQUnsuiteReason::GROUP_BY_UNSUPPORTED_EXPR,
                       "GROUP BY key is not a direct field");
    }

    if (!pq_check_agg_supported(query_block)) {
      return pq_reject(info, PQUnsuiteReason::GROUP_BY_UNSUPPORTED_AGGREGATE,
                       "GROUP BY has unsupported aggregate function");
    }

    const bool groupby_dop1_candidate =
        thd->variables.parallel_query_experimental_groupby_dop1 &&
        thd->variables.parallel_default_dop == 1;
    const bool groupby_dop_partial_candidate =
        thd->variables.parallel_query_experimental_groupby_dop1 &&
        thd->variables.parallel_query_experimental_threaded_dop &&
        (thd->variables.parallel_default_dop == 2 ||
         thd->variables.parallel_default_dop == 4);
    if (groupby_dop1_candidate || groupby_dop_partial_candidate) {
      // Continue the normal single-table/full-scan/cost checks below. This
      // marks only a candidate; GROUP BY factories still own the final shape
      // decision and fall back for unsupported partial aggregation shapes.
      explicit_groupby_dop1_candidate = true;
    } else {
      return pq_reject(info, PQUnsuiteReason::GROUP_BY_PARTIAL_AGG_UNSUPPORTED,
                       "GROUP BY partial aggregation is not implemented");
    }
  }

  // ================================================================
  // 11. Implicit grouping (aggregate without GROUP BY):
  //     Phase 8 expansion: allow implicit grouping if all aggregates
  //     are PQ-compatible (COUNT, SUM, AVG, MIN, MAX).
  //     Unsupported aggregates (GROUP_CONCAT, STD, VAR, BIT_AND, etc.)
  //     still fall back to serial.
  // ================================================================
  if (query_block->is_implicitly_grouped()) {
    // Check whether all aggregate Items are PQ-compatible.
    if (!pq_check_agg_supported(query_block)) {
      return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_AGGREGATE,
                       "query has unsupported aggregate function");
    }
    // All aggregates are PQ-compatible. Continue eligibility check.
    // Note: implicit grouping queries are eligible but still fall
    // back to serial execution because TryCreatePQTableScanIterator
    // returns nullptr in Phase 8 (no real workers). Phase 6+ will
    // activate parallel aggregation when InnoDB PQ scan is ready.
  }

  // ================================================================
  // 11b. Must not have HAVING clause.
  //      Phase 8: conservatively reject HAVING because:
  //      - HAVING can reference aggregates that are NOT in the SELECT
  //        list (hidden aggregates), which pq_check_agg_supported()
  //        would miss if it only checks visible_fields().
  //      - HAVING is evaluated after aggregation; PQ V1 workers
  //        send partial aggregates, and the leader must apply HAVING
  //        after combining them, which is Phase 9 territory.
  // ================================================================
  if (query_block->having_cond() != nullptr) {
    return pq_reject(info, PQUnsuiteReason::HAS_HAVING,
                     "query has HAVING clause");
  }

  // ================================================================
  // 12. Must not have WITH ROLLUP
  // ================================================================
  // olap_type is defined in parser_yystype.h:
  //   enum olap_type { UNSPECIFIED_OLAP_TYPE, ROLLUP_TYPE };
  if (query_block->olap != UNSPECIFIED_OLAP_TYPE) {
    return pq_reject(info, PQUnsuiteReason::HAS_ROLLUP,
                     "query has WITH ROLLUP");
  }

  // ================================================================
  // 13. Must not have window functions
  // ================================================================
  if (query_block->has_windows()) {
    return pq_reject(info, PQUnsuiteReason::HAS_WINDOW,
                     "query has window functions");
  }

  // ================================================================
  // 14. Must not have full-text functions
  // ================================================================
  if (query_block->has_ft_funcs()) {
    return pq_reject(info, PQUnsuiteReason::FULLTEXT,
                     "query has full-text search");
  }

  // ================================================================
  // 15. Must not have semi-join or anti-join nests
  //     has_sj_candidates() checks for subqueries that are
  //     semi-join candidates during resolution. If any exist,
  //     conservatively reject. If none, but we cannot fully
  //     verify (sj_nests is updated during optimization), we
  //     still proceed because single-table queries should not
  //     have semi-joins.
  // ================================================================
  if (query_block->has_sj_candidates()) {
    return pq_reject(info, PQUnsuiteReason::HAS_SEMIJOIN,
                     "query has semi-join candidates");
  }

  // ================================================================
  // 16. Must not reference derived tables, views, or CTEs in the
  //     leaf table list (this overlaps with single-table check below,
  //     but we check the leaf list explicitly too)
  // ================================================================
  for (Table_ref *tr = query_block->leaf_tables; tr != nullptr;
       tr = tr->next_leaf) {
    if (tr->is_view_or_derived()) {
      return pq_reject(info, PQUnsuiteReason::HAS_DERIVED_OR_VIEW,
                       "leaf table is view or derived");
    }
  }

  // ================================================================
  // 17. Transaction isolation must not be SERIALIZABLE
  // ================================================================
  if (thd->tx_isolation == ISO_SERIALIZABLE) {
    return pq_reject(info, PQUnsuiteReason::SERIALIZABLE,
                     "transaction isolation is SERIALIZABLE");
  }

  // ================================================================
  // 18. Must not be a locking read (FOR UPDATE / LOCK IN SHARE MODE)
  //     Check lock_descriptor() on all leaf tables AND check
  //     the top-level table list for lock descriptors.
  //     A locking read uses TL_READ_WITH_SHARED_LOCKS or stronger.
  //     For single-table queries, the leaf table and top-level table
  //     should be the same Table_ref, but we check both to be safe.
  // ================================================================
  for (Table_ref *tr = query_block->leaf_tables; tr != nullptr;
       tr = tr->next_leaf) {
    thr_lock_type lt = tr->lock_descriptor().type;
    // Check both lock_descriptor() and updating flag.
    // For FOR UPDATE, lock_descriptor().type should be TL_WRITE
    // and updating should be true. For LOCK IN SHARE MODE,
    // lock_descriptor().type should be TL_READ_WITH_SHARED_LOCKS.
    if (lt >= TL_READ_WITH_SHARED_LOCKS || tr->updating) {
      return pq_reject(info, PQUnsuiteReason::LOCKING_READ,
                       "locking read detected");
    }
  }

  // Also check the top-level table list (for cases where
  // leaf_tables may not have lock descriptors propagated).
  for (Table_ref *tr = query_block->get_table_list(); tr != nullptr;
       tr = tr->next_local) {
    thr_lock_type lt = tr->lock_descriptor().type;
    if (lt >= TL_READ_WITH_SHARED_LOCKS || tr->updating) {
      return pq_reject(info, PQUnsuiteReason::LOCKING_READ,
                       "locking read detected");
    }
  }

  // ================================================================
  // 19. Single base table must pass InnoDB/temporary/partition/FTS
  //     checks
  // ================================================================
  Table_ref *single_table = query_block->get_table_list();
  if (!pq_check_single_table(single_table, info)) {
    return false;
  }

  // ================================================================
  // 20. Access path must be full table scan (MVP only supports scan)
  // ================================================================
  if (!pq_check_full_table_scan(join, info, explicit_groupby_dop1_candidate)) {
    return false;
  }

  // ================================================================
  // 21. Estimated cost must reach parallel_cost_threshold
  // ================================================================
  if (join != nullptr) {
    if (join->best_read < static_cast<double>(
            thd->variables.parallel_cost_threshold)) {
      return pq_reject(info, PQUnsuiteReason::COST_BELOW_THRESHOLD,
                       "estimated cost below threshold");
    }
  } else {
    // JOIN not yet optimized - cannot check cost. Fallback.
    return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_BY_PHASE1,
                     "JOIN not available for cost check");
  }

  // ================================================================
  // All checks passed - eligible for PQ (within Phase 1 MVP scope)
  // ================================================================
  info->reason = PQUnsuiteReason::NONE;
  info->detail = nullptr;
  return true;
}

/**
  Write eligibility result into Query_block and JOIN boolean fields.

  Does NOT set Query_block::pq_unsuite_info because the PQUnsuiteInfo
  object is typically stack-allocated and would become a dangling pointer.
  Reason storage is deferred until PQUnsuiteInfo can be allocated on a
  long-lived MEM_ROOT.
*/
void pq_mark_query_block_result(Query_block *query_block, JOIN *join,
                                bool eligible,
                                PQUnsuiteReason reason) {
  if (query_block == nullptr) return;

  query_block->pq_candidate = eligible;

  // Explicitly clear pq_unsuite_info to avoid stale pointers from
  // re-optimization or subsequent phases. Reason storage is deferred
  // until PQUnsuiteInfo can be allocated on a long-lived MEM_ROOT.
  query_block->pq_unsuite_info = nullptr;

  if (join != nullptr) {
    join->pq_eligible = eligible;
    join->pq_unsuitable_reason = reason;
    const PQ_execution_state state =
        eligible ? PQ_execution_state::ELIGIBLE
                 : (reason == PQUnsuiteReason::DISABLED
                        ? PQ_execution_state::DISABLED
                        : PQ_execution_state::NOT_ELIGIBLE);
    pq_set_execution_state(join->thd, state);
  }
}
