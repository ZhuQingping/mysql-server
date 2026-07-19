/* Copyright (c) 2020, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/join_optimizer/access_path.h"

#include "my_base.h"
#include "my_bit.h"
#include "sql/filesort.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/bka_iterator.h"
#include "sql/iterators/composite_iterators.h"
#include "sql/iterators/delete_rows_iterator.h"
#include "sql/iterators/hash_join_buffer.h"
#include "sql/iterators/hash_join_iterator.h"
#include "sql/iterators/ref_row_iterators.h"
#include "sql/iterators/sorting_iterator.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/iterators/window_iterators.h"
#include "sql/join_optimizer/bit_utils.h"
#include "sql/join_optimizer/cost_model.h"
#include "sql/join_optimizer/estimate_selectivity.h"
#include "sql/join_optimizer/overflow_bitset.h"
#include "sql/join_optimizer/relational_expression.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/mem_root_array.h"
#include "sql/parallel_query/pq_iterators.h"
#include "sql/parallel_query/pq_optimizer.h"
#include "sql/partial_result_cache.h"
#include "sql/range_optimizer/geometry_index_range_scan.h"
#include "sql/range_optimizer/group_index_skip_scan.h"
#include "sql/range_optimizer/group_index_skip_scan_plan.h"
#include "sql/range_optimizer/index_merge.h"
#include "sql/range_optimizer/index_range_scan.h"
#include "sql/range_optimizer/index_skip_scan.h"
#include "sql/range_optimizer/index_skip_scan_plan.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/range_optimizer/reverse_index_range_scan.h"
#include "sql/range_optimizer/rowid_ordered_retrieval.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_update.h"
#include "sql/table.h"

#include <algorithm>
#include <vector>

using pack_rows::TableCollection;
using std::vector;

AccessPath *NewSortAccessPath(THD *thd, AccessPath *child, Filesort *filesort,
                              ORDER *order, bool count_examined_rows) {
  assert(child != nullptr);
  assert(filesort != nullptr);
  assert(order != nullptr);

  AccessPath *path = new (thd->mem_root) AccessPath;
  path->type = AccessPath::SORT;
  path->count_examined_rows = count_examined_rows;
  path->sort().child = child;
  path->sort().filesort = filesort;
  path->sort().order = order;
  path->sort().remove_duplicates = filesort->m_remove_duplicates;
  path->sort().unwrap_rollup = false;
  path->sort().limit = filesort->limit;
  path->sort().force_sort_rowids = !filesort->using_addon_fields();

  if (filesort->using_addon_fields()) {
    path->sort().tables_to_get_rowid_for = 0;
  } else {
    if (filesort->tables.size() == 1 &&
        filesort->tables[0]->pos_in_table_list == nullptr) {
      // This can happen if we sort a single temporary table
      // which is not in the table list (e.g., one that was
      // specifically created for us). Filesort has special-casing
      // to always get the row ID in this case.
      path->sort().tables_to_get_rowid_for = 0;
    } else {
      FindTablesToGetRowidFor(path);
    }
  }

  return path;
}

AccessPath *NewDeleteRowsAccessPath(THD *thd, AccessPath *child,
                                    table_map delete_tables,
                                    table_map immediate_tables) {
  assert(IsSubset(immediate_tables, delete_tables));
  AccessPath *path = new (thd->mem_root) AccessPath;
  path->type = AccessPath::DELETE_ROWS;
  path->delete_rows().child = child;
  path->delete_rows().tables_to_delete_from = delete_tables;
  path->delete_rows().immediate_tables = immediate_tables;
  return path;
}

AccessPath *NewUpdateRowsAccessPath(THD *thd, AccessPath *child,
                                    table_map update_tables,
                                    table_map immediate_tables) {
  assert(IsSubset(immediate_tables, update_tables));
  AccessPath *path = new (thd->mem_root) AccessPath;
  path->type = AccessPath::UPDATE_ROWS;
  path->update_rows().child = child;
  path->update_rows().tables_to_update = update_tables;
  path->update_rows().immediate_tables = immediate_tables;
  return path;
}

static AccessPath *FindSingleAccessPathOfType(AccessPath *path,
                                              AccessPath::Type type) {
  AccessPath *found_path = nullptr;

  auto func = [type, &found_path](AccessPath *subpath, const JOIN *) {
#ifdef NDEBUG
    constexpr bool fast_exit = true;
#else
    constexpr bool fast_exit = false;
#endif
    if (subpath->type == type) {
      assert(found_path == nullptr);
      found_path = subpath;
      // If not in debug mode, stop as soon as we find the first one.
      if (fast_exit) {
        return true;
      }
    }
    return false;
  };
  // Our users generally want to stop at STREAM or MATERIALIZE nodes,
  // since they are table-oriented and those nodes have their own tables.
  WalkAccessPaths(path, /*join=*/nullptr,
                  WalkAccessPathPolicy::STOP_AT_MATERIALIZATION, func);
  return found_path;
}

static RowIterator *FindSingleIteratorOfType(AccessPath *path,
                                             AccessPath::Type type) {
  AccessPath *found_path = FindSingleAccessPathOfType(path, type);
  if (found_path == nullptr) {
    return nullptr;
  } else {
    return found_path->iterator->real_iterator();
  }
}

TABLE *GetBasicTable(const AccessPath *path) {
  switch (path->type) {
    // Basic access paths (those with no children, at least nominally).
    case AccessPath::TABLE_SCAN:
      return path->table_scan().table;
    case AccessPath::INDEX_SCAN:
      return path->index_scan().table;
    case AccessPath::REF:
      return path->ref().table;
    case AccessPath::REF_OR_NULL:
      return path->ref_or_null().table;
    case AccessPath::EQ_REF:
      return path->eq_ref().table;
    case AccessPath::PUSHED_JOIN_REF:
      return path->pushed_join_ref().table;
    case AccessPath::FULL_TEXT_SEARCH:
      return path->full_text_search().table;
    case AccessPath::CONST_TABLE:
      return path->const_table().table;
    case AccessPath::MRR:
      return path->mrr().table;
    case AccessPath::FOLLOW_TAIL:
      return path->follow_tail().table;
    case AccessPath::INDEX_RANGE_SCAN:
      return path->index_range_scan().used_key_part[0].field->table;
    case AccessPath::INDEX_MERGE:
      return path->index_merge().table;
    case AccessPath::ROWID_INTERSECTION:
      return path->rowid_intersection().table;
    case AccessPath::ROWID_UNION:
      return path->rowid_union().table;
    case AccessPath::INDEX_SKIP_SCAN:
      return path->index_skip_scan().table;
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
      return path->group_index_skip_scan().table;
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return path->dynamic_index_range_scan().table;
    case AccessPath::PQ_BLOCK_SCAN:
      return path->pq_block_scan().table;
    case AccessPath::PQ_REF_SCAN:
      return path->pq_ref_scan().table;

    // Basic access paths that don't correspond to a specific table.
    case AccessPath::PARALLEL_SCAN:
    case AccessPath::TABLE_VALUE_CONSTRUCTOR:
    case AccessPath::FAKE_SINGLE_ROW:
    case AccessPath::ZERO_ROWS:
    case AccessPath::ZERO_ROWS_AGGREGATED:
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
    case AccessPath::UNQUALIFIED_COUNT:

    // Note, some other AccessPaths may use its own temporary (derived) table.
    // We intentionally do not return such TABLEs.
    default:
      return nullptr;
  }
}

table_map GetUsedTableMap(const AccessPath *path, bool include_pruned_tables) {
  table_map tmap = 0;
  WalkTablesUnderAccessPath(
      const_cast<AccessPath *>(path),
      [&tmap](TABLE *table) {
        if (table->pos_in_table_list == nullptr) {
          // Materialization within a JOIN (e.g., for sorting). The table won't
          // have a map, so the caller will need to find the table manually.
          tmap |= RAND_TABLE_BIT;
        } else {
          tmap |= table->pos_in_table_list->map();
        }
        return false;
      },
      include_pruned_tables);
  return tmap;
}

Prealloced_array<TABLE *, 4> GetUsedTables(AccessPath *child,
                                           bool include_pruned_tables) {
  Prealloced_array<TABLE *, 4> tables{PSI_NOT_INSTRUMENTED};
  WalkTablesUnderAccessPath(
      child,
      [&tables](TABLE *table) {
        tables.push_back(table);
        return false;
      },
      include_pruned_tables);
  return tables;
}

Mem_root_array<TABLE *> CollectTables(THD *thd, AccessPath *root_path) {
  Mem_root_array<TABLE *> tables(thd->mem_root);
  WalkTablesUnderAccessPath(
      root_path, [&tables](TABLE *table) { return tables.push_back(table); },
      /*include_pruned_tables=*/true);
  return tables;
}

/**
  Get the tables that are accessed by EQ_REF and can be on the inner side of an
  outer join. These need some extra care in AggregateIterator when handling
  NULL-complemented rows, so that the cache in EQRefIterator is not disturbed by
  AggregateIterator's switching between groups.
 */
static table_map GetNullableEqRefTables(const AccessPath *root_path) {
  table_map tables = 0;
  WalkAccessPaths(
      root_path, /*join=*/nullptr,
      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
      [&tables](const AccessPath *path, const JOIN *) {
        if (path->type == AccessPath::EQ_REF) {
          const auto &param = path->eq_ref();
          if (param.table->is_nullable() && !param.ref->disable_cache) {
            tables |= param.table->pos_in_table_list->map();
          }
        }
        return false;
      });
  return tables;
}

// Mirrors QEP_TAB::pfs_batch_update(), with one addition:
// If there is more than one table, batch mode will be handled by the join
// iterators on the probe side, so joins will return false.
bool ShouldEnableBatchMode(AccessPath *path) {
  switch (path->type) {
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN:
    case AccessPath::REF:
    case AccessPath::REF_OR_NULL:
    case AccessPath::PUSHED_JOIN_REF:
    case AccessPath::FULL_TEXT_SEARCH:
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return true;
    case AccessPath::FILTER:
      if (path->filter().condition->has_subquery()) {
        return false;
      } else {
        return ShouldEnableBatchMode(path->filter().child);
      }
    case AccessPath::SORT:
      return ShouldEnableBatchMode(path->sort().child);
    case AccessPath::EQ_REF:
    case AccessPath::CONST_TABLE:
      // These can read only one row per scan, so batch mode will never be a
      // win (fall through).
    default:
      // All others, in particular joins.
      return false;
  }
}

bool FinalizeMaterializedSubqueries(THD *thd, JOIN *join, AccessPath *path) {
  if (path->type != AccessPath::FILTER ||
      !path->filter().materialize_subqueries) {
    return false;
  }
  return WalkItem(
      path->filter().condition, enum_walk::POSTFIX, [thd, join](Item *item) {
        if (!IsItemInSubSelect(item)) {
          return false;
        }
        Item_in_subselect *item_subs = down_cast<Item_in_subselect *>(item);
        Query_block *subquery_block = item_subs->unit->first_query_block();
        if (!item_subs->subquery_allows_materialization(thd, subquery_block,
                                                        join->query_block)) {
          return false;
        }
        if (item_subs->finalize_materialization_transform(
                thd, subquery_block->join)) {
          return true;
        }
        item_subs->create_iterators(thd);
        return false;
      });
}

namespace {

struct IteratorToBeCreated {
  AccessPath *path;
  JOIN *join;
  bool eligible_for_batch_mode;
  unique_ptr_destroy_only<RowIterator> *destination;
  Bounds_checked_array<unique_ptr_destroy_only<RowIterator>> children;

  void AllocChildren(MEM_ROOT *mem_root, int num_children) {
    children =
        Bounds_checked_array<unique_ptr_destroy_only<RowIterator>>::Alloc(
            mem_root, num_children);
  }
};

void SetupJobsForChildren(MEM_ROOT *mem_root, AccessPath *child, JOIN *join,
                          bool eligible_for_batch_mode,
                          IteratorToBeCreated *job,
                          Mem_root_array<IteratorToBeCreated> *todo) {
  // Make jobs for the child, and we'll return to this job later.
  job->AllocChildren(mem_root, 1);
  todo->push_back(*job);
  todo->push_back(
      {child, join, eligible_for_batch_mode, &job->children[0], {}});
}

void SetupJobsForChildren(MEM_ROOT *mem_root, AccessPath *outer,
                          AccessPath *inner, JOIN *join,
                          bool inner_eligible_for_batch_mode,
                          IteratorToBeCreated *job,
                          Mem_root_array<IteratorToBeCreated> *todo) {
  // Make jobs for the children, and we'll return to this job later.
  // Note that we push the inner before the outer job, so that we get
  // left created before right (invalidators in materialization access paths,
  // used in the old join optimizer, depend on this).
  job->AllocChildren(mem_root, 2);
  todo->push_back(*job);
  todo->push_back(
      {inner, join, inner_eligible_for_batch_mode, &job->children[1], {}});
  todo->push_back({outer, join, false, &job->children[0], {}});
}

}  // namespace

unique_ptr_destroy_only<RowIterator> CreateIteratorFromAccessPath(
    THD *thd, MEM_ROOT *mem_root, AccessPath *top_path, JOIN *top_join,
    bool top_eligible_for_batch_mode) {
  assert(IteratorsAreNeeded(thd, top_path));

  unique_ptr_destroy_only<RowIterator> ret;
  Mem_root_array<IteratorToBeCreated> todo(mem_root);
  todo.push_back({top_path, top_join, top_eligible_for_batch_mode, &ret, {}});

  // The access path trees can be pretty deep, and the stack frames can be big
  // on certain compilers/setups, so instead of explicit recursion, we push jobs
  // onto a MEM_ROOT-backed stack. This uses a little more RAM (the MEM_ROOT
  // typically lives to the end of the query), but reduces the stack usage
  // greatly.
  //
  // The general rule is that if an iterator requires any children, it will push
  // jobs for their access paths at the end of the stack and then re-push
  // itself. When the children are instantiated and we get back to the original
  // iterator, we'll actually instantiate it. (We distinguish between the two
  // cases on basis of whether job.children has been allocated or not; the child
  // iterator's destination will point into this array. The child list needs
  // to be allocated in a way that doesn't move around if the TODO job list
  // is reallocated, which we do by means of allocating it directly on the
  // MEM_ROOT.)
  while (!todo.empty()) {
    IteratorToBeCreated job = todo.back();
    todo.pop_back();

    AccessPath *path = job.path;
    JOIN *join = job.join;
    bool eligible_for_batch_mode = job.eligible_for_batch_mode;

    if (job.join != nullptr) {
      assert(!job.join->needs_finalize);
    }

    unique_ptr_destroy_only<RowIterator> iterator;

    ha_rows *examined_rows = nullptr;
    if (path->count_examined_rows && join != nullptr) {
      examined_rows = &join->examined_rows;
    }

    switch (path->type) {
      case AccessPath::TABLE_SCAN: {
        const auto &param = path->table_scan();
        iterator = NewIterator<TableScanIterator>(
            thd, mem_root, param.table, path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::INDEX_SCAN: {
        const auto &param = path->index_scan();
        if (param.reverse) {
          iterator = NewIterator<IndexScanIterator<true>>(
              thd, mem_root, param.table, param.idx, param.use_order,
              path->num_output_rows(), examined_rows);
        } else {
          iterator = NewIterator<IndexScanIterator<false>>(
              thd, mem_root, param.table, param.idx, param.use_order,
              path->num_output_rows(), examined_rows);
        }
        break;
      }
      case AccessPath::REF: {
        const auto &param = path->ref();
        if (param.reverse) {
          iterator = NewIterator<RefIterator<true>>(
              thd, mem_root, param.table, param.ref, param.use_order,
              path->num_output_rows(), examined_rows);
        } else {
          iterator = NewIterator<RefIterator<false>>(
              thd, mem_root, param.table, param.ref, param.use_order,
              path->num_output_rows(), examined_rows);
        }
        break;
      }
      case AccessPath::REF_OR_NULL: {
        const auto &param = path->ref_or_null();
        iterator = NewIterator<RefOrNullIterator>(
            thd, mem_root, param.table, param.ref, param.use_order,
            path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::EQ_REF: {
        const auto &param = path->eq_ref();
        iterator = NewIterator<EQRefIterator>(thd, mem_root, param.table,
                                              param.ref, examined_rows);
        break;
      }
      case AccessPath::PUSHED_JOIN_REF: {
        const auto &param = path->pushed_join_ref();
        iterator = NewIterator<PushedJoinRefIterator>(
            thd, mem_root, param.table, param.ref, param.use_order,
            param.is_unique, examined_rows);
        break;
      }
      case AccessPath::FULL_TEXT_SEARCH: {
        const auto &param = path->full_text_search();
        iterator = NewIterator<FullTextSearchIterator>(
            thd, mem_root, param.table, param.ref, param.ft_func,
            param.use_order, param.use_limit, examined_rows);
        break;
      }
      case AccessPath::CONST_TABLE: {
        const auto &param = path->const_table();
        iterator = NewIterator<ConstIterator>(thd, mem_root, param.table,
                                              param.ref, examined_rows);
        break;
      }
      case AccessPath::MRR: {
        const auto &param = path->mrr();
        const auto &bka_param = param.bka_path->bka_join();
        iterator = NewIterator<MultiRangeRowIterator>(
            thd, mem_root, param.table, param.ref, param.mrr_flags,
            bka_param.join_type,
            GetUsedTables(bka_param.outer, /*include_pruned_tables=*/true),
            bka_param.store_rowids, bka_param.tables_to_get_rowid_for);
        break;
      }
      case AccessPath::FOLLOW_TAIL: {
        const auto &param = path->follow_tail();
        iterator = NewIterator<FollowTailIterator>(
            thd, mem_root, param.table, path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::INDEX_RANGE_SCAN: {
        const auto &param = path->index_range_scan();
        TABLE *table = param.used_key_part[0].field->table;
        if (param.geometry) {
          iterator = NewIterator<GeometryIndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(),
              param.index, param.need_rows_in_rowid_order, param.reuse_handler,
              mem_root, param.mrr_flags, param.mrr_buf_size,
              Bounds_checked_array{param.ranges, param.num_ranges});
        } else if (param.reverse) {
          iterator = NewIterator<ReverseIndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(),
              param.index, mem_root, param.mrr_flags,
              Bounds_checked_array{param.ranges, param.num_ranges},
              param.using_extended_key_parts);
        } else {
          iterator = NewIterator<IndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(),
              param.index, param.need_rows_in_rowid_order, param.reuse_handler,
              mem_root, param.mrr_flags, param.mrr_buf_size,
              Bounds_checked_array{param.ranges, param.num_ranges});
        }
        break;
      }
      case AccessPath::INDEX_MERGE: {
        const auto &param = path->index_merge();
        unique_ptr_destroy_only<RowIterator> pk_quick_select;
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (size_t child_idx = 0; child_idx < param.children->size();
             ++child_idx) {
          AccessPath *range_scan = (*param.children)[child_idx];
          if (param.allow_clustered_primary_key_scan &&
              param.table->file->primary_key_is_clustered() &&
              range_scan->index_range_scan().index ==
                  param.table->s->primary_key) {
            assert(pk_quick_select == nullptr);
            pk_quick_select = std::move(job.children[child_idx]);
          } else {
            children.push_back(std::move(job.children[child_idx]));
          }
        }

        iterator = NewIterator<IndexMergeIterator>(
            thd, mem_root, mem_root, param.table, std::move(pk_quick_select),
            std::move(children));
        break;
      }
      case AccessPath::ROWID_INTERSECTION: {
        const auto &param = path->rowid_intersection();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size() +
                                          (param.cpk_child != nullptr ? 1 : 0));
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          if (param.cpk_child != nullptr) {
            todo.push_back({param.cpk_child,
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[param.children->size()],
                            {}});
          }
          continue;
        }

        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (size_t child_idx = 0; child_idx < param.children->size();
             ++child_idx) {
          children.push_back(std::move(job.children[child_idx]));
        }

        unique_ptr_destroy_only<RowIterator> cpk_child;
        if (param.cpk_child != nullptr) {
          cpk_child = std::move(job.children[param.children->size()]);
        }
        iterator = NewIterator<RowIDIntersectionIterator>(
            thd, mem_root, mem_root, param.table, param.retrieve_full_rows,
            param.need_rows_in_rowid_order, std::move(children),
            std::move(cpk_child));
        break;
      }
      case AccessPath::ROWID_UNION: {
        const auto &param = path->rowid_union();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (unique_ptr_destroy_only<RowIterator> &child : job.children) {
          children.push_back(std::move(child));
        }
        iterator = NewIterator<RowIDUnionIterator>(
            thd, mem_root, mem_root, param.table, std::move(children));
        break;
      }
      case AccessPath::INDEX_SKIP_SCAN: {
        const IndexSkipScanParameters *param = path->index_skip_scan().param;
        iterator = NewIterator<IndexSkipScanIterator>(
            thd, mem_root, path->index_skip_scan().table, param->index_info,
            path->index_skip_scan().index, param->eq_prefix_len,
            param->eq_prefix_key_parts, param->eq_prefixes,
            path->index_skip_scan().num_used_key_parts, mem_root,
            param->has_aggregate_function, param->min_range_key,
            param->max_range_key, param->min_search_key, param->max_search_key,
            param->range_cond_flag, param->range_key_len);
        break;
      }
      case AccessPath::GROUP_INDEX_SKIP_SCAN: {
        const GroupIndexSkipScanParameters *param =
            path->group_index_skip_scan().param;
        iterator = NewIterator<GroupIndexSkipScanIterator>(
            thd, mem_root, path->group_index_skip_scan().table,
            &param->min_functions, &param->max_functions,
            param->have_agg_distinct, param->min_max_arg_part,
            param->group_prefix_len, param->group_key_parts,
            param->real_key_parts, param->max_used_key_length,
            param->index_info, path->group_index_skip_scan().index,
            param->key_infix_len, mem_root, param->is_index_scan,
            &param->prefix_ranges, &param->key_infix_ranges,
            &param->min_max_ranges);
        break;
      }
      case AccessPath::DYNAMIC_INDEX_RANGE_SCAN: {
        const auto &param = path->dynamic_index_range_scan();
        iterator = NewIterator<DynamicRangeIterator>(
            thd, mem_root, param.table, param.qep_tab, examined_rows);
        break;
      }
      case AccessPath::TABLE_VALUE_CONSTRUCTOR: {
        assert(join != nullptr);
        Query_block *query_block = join->query_block;
        iterator = NewIterator<TableValueConstructorIterator>(
            thd, mem_root, examined_rows, *query_block->row_value_list,
            query_block->join->fields);
        break;
      }
      case AccessPath::FAKE_SINGLE_ROW:
        iterator =
            NewIterator<FakeSingleRowIterator>(thd, mem_root, examined_rows);
        break;
      case AccessPath::ZERO_ROWS: {
        iterator = NewIterator<ZeroRowsIterator>(thd, mem_root,
                                                 CollectTables(thd, path));
        break;
      }
      case AccessPath::ZERO_ROWS_AGGREGATED:
        iterator = NewIterator<ZeroRowsAggregatedIterator>(thd, mem_root, join,
                                                           examined_rows);
        break;
      case AccessPath::MATERIALIZED_TABLE_FUNCTION: {
        const auto &param = path->materialized_table_function();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.table_path, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<MaterializedTableFunctionIterator>(
            thd, mem_root, param.table_function, param.table,
            std::move(job.children[0]));
        break;
      }
      case AccessPath::UNQUALIFIED_COUNT:
        iterator = NewIterator<UnqualifiedCountIterator>(thd, mem_root, join);
        break;
      case AccessPath::NESTED_LOOP_JOIN: {
        const auto &param = path->nested_loop_join();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }

        iterator = NewIterator<NestedLoopIterator>(
            thd, mem_root, std::move(job.children[0]),
            std::move(job.children[1]), param.join_type, param.pfs_batch_mode,
            join, path->nested_loop_join().distinct_optimize);
        break;
      }
      case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL: {
        const auto &param = path->nested_loop_semijoin_with_duplicate_removal();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<NestedLoopSemiJoinWithDuplicateRemovalIterator>(
            thd, mem_root, std::move(job.children[0]),
            std::move(job.children[1]), param.table, param.key, param.key_len);
        break;
      }
      case AccessPath::BKA_JOIN: {
        const auto &param = path->bka_join();
        AccessPath *mrr_path =
            FindSingleAccessPathOfType(param.inner, AccessPath::MRR);
        if (job.children.is_null()) {
          mrr_path->mrr().bka_path = path;
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               /*inner_eligible_for_batch_mode=*/false, &job,
                               &todo);
          continue;
        }

        MultiRangeRowIterator *mrr_iterator =
            down_cast<MultiRangeRowIterator *>(
                mrr_path->iterator->real_iterator());
        iterator = NewIterator<BKAIterator>(
            thd, mem_root, std::move(job.children[0]),
            GetUsedTables(param.outer, /*include_pruned_tables=*/true),
            std::move(job.children[1]), thd->variables.join_buff_size,
            param.mrr_length_per_rec, param.rec_per_key, param.store_rowids,
            param.tables_to_get_rowid_for, mrr_iterator, param.join_type);
        break;
      }
      case AccessPath::HASH_JOIN: {
        const auto &param = path->hash_join();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               /*inner_eligible_for_batch_mode=*/true, &job,
                               &todo);
          continue;
        }
        const JoinPredicate *join_predicate = param.join_predicate;
        vector<HashJoinCondition> conditions;
        for (Item_eq_base *cond : join_predicate->expr->equijoin_conditions) {
          conditions.emplace_back(cond, thd->mem_root);
        }
        const bool probe_input_batch_mode =
            eligible_for_batch_mode && ShouldEnableBatchMode(param.outer);
        double estimated_build_rows = param.inner->num_output_rows();
        if (param.inner->num_output_rows() < 0.0) {
          // Not all access paths may propagate their costs properly.
          // Choose a fairly safe estimate (it's better to be too large
          // than too small).
          estimated_build_rows = 1048576.0;
        }
        JoinType join_type{JoinType::INNER};
        switch (join_predicate->expr->type) {
          case RelationalExpression::INNER_JOIN:
          case RelationalExpression::STRAIGHT_INNER_JOIN:
            join_type = JoinType::INNER;
            break;
          case RelationalExpression::LEFT_JOIN:
            join_type = JoinType::OUTER;
            break;
          case RelationalExpression::ANTIJOIN:
            join_type = JoinType::ANTI;
            break;
          case RelationalExpression::SEMIJOIN:
            join_type =
                param.rewrite_semi_to_inner ? JoinType::INNER : JoinType::SEMI;
            break;
          case RelationalExpression::TABLE:
          default:
            assert(false);
        }
        // See if we can allow the hash table to keep its contents across Init()
        // calls.
        //
        // The old optimizer will sometimes push join conditions referring
        // to outer tables (in the same query block) down in under the hash
        // operation, so without analysis of each filter and join condition, we
        // cannot say for sure, and thus have to turn it off. But the hypergraph
        // optimizer sets parameter_tables properly, so we're safe if we just
        // check that.
        //
        // Regardless of optimizer, we can push outer references down in under
        // the hash, but join->hash_table_generation will increase whenever we
        // need to recompute the query block (in JOIN::clear_hash_tables()).
        //
        // TODO(sgunders): The old optimizer had a concept of _when_ to clear
        // derived tables (invalidators), and this is somehow similar. If it
        // becomes a performance issue, consider reintroducing them.
        //
        // TODO(sgunders): Should this perhaps be set as a flag on the access
        // path instead of being computed here? We do make the same checks in
        // the cost model, so perhaps it should set the flag as well.
        uint64_t *hash_table_generation =
            (thd->lex->using_hypergraph_optimizer() &&
             path->parameter_tables == 0)
                ? &join->hash_table_generation
                : nullptr;

        bool use_bloom_filter = false;
        HashJoin::PQHashJoinSharedContext *pq_hash_join = nullptr;

        // PQ support explain analyze statement, leader will create fake
        // iterator of worker's plan, this iterator only used to gather workers'
        // execution timing info, for this scenario, do not create parallel hash
        // join shared context.
        if (thd->has_pq && !thd->pq_leader_create_fake_iter) {
          // If parallel hash_join_spill_to_disk is OFF, go back to the old
          // behavior where spill to disk was not supported for parallel hash
          // join (it could easily create an excessive amount of files, hitting
          // the limit of maximum number of open files).
          if (!thd->pq_support_features_switch_flag(
                  PQ_SUPPORT_FEATURES_SWITCH_HASH_JOIN_SPILL_TO_DISK)) {
            path->hash_join().allow_spill_to_disk = false;
          }

          if (thd->is_pq_worker()) {
            // We only set up parallel hash join if we are creating iterators
            // for a parallel worker; we need access to the Gather operator
            // below which only workers has access to.

            // Only use Bloom filter when we have partial data from the build
            // input. With increasing number of workers, each worker will read a
            // smaller subset of the build input. This will (most likely)
            // decrease the number of matching rows in the probe phase, and
            // having a bloom filter as a quick pre-filtering before the hash
            // table lookup gives us a small speedup. With a shared hash table
            // (partial results from both inputs), the number of matching rows
            // will not decrease with the number of workers, and the same
            // performance gain can not be expected.
            use_bloom_filter =
                path->hash_join().partial_results_from_build_input &&
                !path->hash_join().partial_results_from_probe_input;

            if (path->hash_join().partial_results_from_build_input &&
                path->hash_join().partial_results_from_probe_input) {
              pq_hash_join = thd->pq_worker_info->m_gather
                                 ->m_pq_hash_join_shared_context.get();
              assert(!use_bloom_filter && pq_hash_join);
            }
          }
        }

        if (join_predicate->expr->equijoin_conditions.empty()) {
          // There is no point in using a Bloom filter when we do not have any
          // equi-join conditions as every value will pass the Bloom filter test
          // anyways.
          use_bloom_filter = false;
        }

        iterator = NewIterator<HashJoinIterator>(
            thd, mem_root, std::move(job.children[1]),
            GetUsedTables(param.inner, /*include_pruned_tables=*/true),
            estimated_build_rows, std::move(job.children[0]),
            GetUsedTables(param.outer, /*include_pruned_tables=*/true),
            param.store_rowids, param.tables_to_get_rowid_for,
            thd->variables.join_buff_size, std::move(conditions),
            param.allow_spill_to_disk, join_type,
            join_predicate->expr->join_conditions, probe_input_batch_mode,
            hash_table_generation, use_bloom_filter, pq_hash_join, path);
        break;
      }
      case AccessPath::FILTER: {
        const auto &param = path->filter();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        if (FinalizeMaterializedSubqueries(thd, join, path)) {
          return nullptr;
        }
        iterator = NewIterator<FilterIterator>(
            thd, mem_root, std::move(job.children[0]), param.condition);
        break;
      }
      case AccessPath::SORT: {
        const auto &param = path->sort();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        ha_rows num_rows_estimate = param.child->num_output_rows() < 0.0
                                        ? HA_POS_ERROR
                                        : lrint(param.child->num_output_rows());
        Filesort *filesort = param.filesort;
        iterator = NewIterator<SortingIterator>(
            thd, mem_root, filesort, std::move(job.children[0]),
            num_rows_estimate, param.tables_to_get_rowid_for, examined_rows);
        if (filesort->m_remove_duplicates) {
          filesort->tables[0]->duplicate_removal_iterator =
              down_cast<SortingIterator *>(iterator->real_iterator());
        } else {
          filesort->tables[0]->sorting_iterator =
              down_cast<SortingIterator *>(iterator->real_iterator());
        }
        break;
      }
      case AccessPath::AGGREGATE: {
        const auto &param = path->aggregate();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        Prealloced_array<TABLE *, 4> tables =
            GetUsedTables(param.child, /*include_pruned_tables=*/true);
        iterator = NewIterator<AggregateIterator>(
            thd, mem_root, std::move(job.children[0]), join,
            TableCollection(tables, /*store_rowids=*/false,
                            /*tables_to_get_rowid_for=*/0,
                            GetNullableEqRefTables(param.child)),
            param.rollup);
        break;
      }
      case AccessPath::TEMPTABLE_AGGREGATE: {
        const auto &param = path->temptable_aggregate();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, 2);
          todo.push_back(job);
          todo.push_back({param.subquery_path,
                          join,
                          /*eligible_for_batch_mode=*/true,
                          &job.children[0],
                          {}});
          todo.push_back({param.table_path,
                          join,
                          eligible_for_batch_mode,
                          &job.children[1],
                          {}});
          continue;
        }

        iterator = unique_ptr_destroy_only<RowIterator>(
            temptable_aggregate_iterator::CreateIterator(
                thd, std::move(job.children[0]), param.temp_table_param,
                param.table, std::move(job.children[1]), join,
                param.ref_slice));

        break;
      }
      case AccessPath::LIMIT_OFFSET: {
        const auto &param = path->limit_offset();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        ha_rows *send_records = nullptr;
        if (param.send_records_override != nullptr) {
          send_records = param.send_records_override;
        } else if (join != nullptr) {
          send_records = &join->send_records;
        }
        iterator = NewIterator<LimitOffsetIterator>(
            thd, mem_root, std::move(job.children[0]), param.limit,
            param.offset, param.count_all_rows, param.reject_multiple_rows,
            send_records);
        break;
      }
      case AccessPath::STREAM: {
        const auto &param = path->stream();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, param.join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<StreamingIterator>(
            thd, mem_root, std::move(job.children[0]), param.temp_table_param,
            param.table, param.provide_rowid, param.join, param.ref_slice);
        break;
      }
      case AccessPath::MATERIALIZE: {
        // The table access path should be a single iterator, not a tree.
        // (ALTERNATIVE counts as a single iterator in this regard.)
        assert(
            path->materialize().table_path->type == AccessPath::TABLE_SCAN ||
            path->materialize().table_path->type == AccessPath::LIMIT_OFFSET ||
            path->materialize().table_path->type == AccessPath::REF ||
            path->materialize().table_path->type == AccessPath::REF_OR_NULL ||
            path->materialize().table_path->type == AccessPath::EQ_REF ||
            path->materialize().table_path->type == AccessPath::ALTERNATIVE ||
            path->materialize().table_path->type == AccessPath::CONST_TABLE ||
            path->materialize().table_path->type == AccessPath::INDEX_SCAN ||
            path->materialize().table_path->type == AccessPath::PQ_BLOCK_SCAN ||
            path->materialize().table_path->type == AccessPath::PQ_REF_SCAN ||
            path->materialize().table_path->type ==
                AccessPath::INDEX_RANGE_SCAN ||
            path->materialize().table_path->type ==
                AccessPath::DYNAMIC_INDEX_RANGE_SCAN);

        MaterializePathParameters *param = path->materialize().param;
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param->query_blocks.size() + 1);
          todo.push_back(job);
          todo.push_back({path->materialize().table_path,
                          join,
                          eligible_for_batch_mode,
                          &job.children[0],
                          {}});
          // If we can share the materialized table (including derived table and
          // semijoin materialized table), then PQ worker should not set its
          // materialized subquery_iterator, because it should not generate
          // records, it can directly read records from shared materialized
          // table. Thus we have no need to build each query_block's iterator.
          // In addition, if the subquery is optimized to zero, then we also no
          // need to build the iterator.
          bool need_child_iterator = true;
          bool pq_sharing_materialized_table =
              // derived table
              (param->unit && param->unit->m_pq_shared_info) ||
              // semijoin materialized table. When PQ worker reaches this point,
              // PQ leader must have filled the shared materialized table into
              // "m_pq_shared_info", otherwise
              // MaterializeIterator::init_shared_table() executes error and
              // leader will capture the error and then exit. So we can only use
              // "m_pq_shared_info" to determine whether the semijoin
              // materialized table has been shared.
              (param->sj_mat_exec && param->sj_mat_exec->m_pq_shared_info &&
               *param->sj_mat_exec->m_pq_shared_info);

          if (pq_sharing_materialized_table) {
            need_child_iterator = false;
          } else {
            // When this view (or derived table) is located into one optimized
            // zero-result iterator, we will not build this view's query_blocks
            // in worker thread as we have not called "setup_materialization".
            // Thus, we should skip the creation of iterator. This action is
            // safe as ZeroRowsIterator will never read rows.
            for (size_t i = 0; i < param->query_blocks.size(); i++) {
              auto access_path = param->query_blocks[i].subquery_path;
              if (nullptr == access_path) {
                need_child_iterator = false;
                break;
              }
            }
          }

          if (!need_child_iterator) {
            // see comments around "need_child_iterator".
#ifndef NDEBUG
            assert(thd->is_pq_worker());
            // Complicated multi-condition assertion, aligned with the logic of
            // MaterializeIterator::pq_sharing_table()
            bool pass = false;
            if (param->unit) {
              auto parent_query_block = param->unit->outer_query_block();
              if (parent_query_block->parallel_exec)
                pass = true;
              else {
                auto parent_unit =
                    parent_query_block->master_query_expression();
                if (parent_unit->subquery_suite_for_parallel_query() ==
                        PQSubqueryExecution::kMultipleByWorkers &&
                    parent_unit->outer_query_block()->parallel_exec)
                  pass = true;
              }
            }
            pass |= (param->sj_mat_exec != nullptr);
            assert(pass);
#endif  // NDEBUG
          } else {
            for (size_t i = 0; i < param->query_blocks.size(); ++i) {
              const MaterializePathParameters::QueryBlock &from =
                  param->query_blocks[i];
              // For pq explain analyze scenario, if materialize subquery
              // iterator already created, not create again during create fake
              // worker plan's running iterator, TPC-H query 13 scenario.
              job.children[i + 1] = nullptr;
              if (!thd->pq_leader_create_fake_iter ||
                  !from.subquery_path->iterator) {
                todo.push_back({from.subquery_path,
                                from.join,
                                /*eligible_for_batch_mode=*/true,
                                &job.children[i + 1],
                                {}});
              }
            }
          }
          continue;
        }
        unique_ptr_destroy_only<RowIterator> table_iterator =
            std::move(job.children[0]);
        Mem_root_array<materialize_iterator::QueryBlock> query_blocks(
            thd->mem_root, param->query_blocks.size());
        for (size_t i = 0; i < param->query_blocks.size(); ++i) {
          const MaterializePathParameters::QueryBlock &from =
              param->query_blocks[i];
          materialize_iterator::QueryBlock &to = query_blocks[i];
          to.subquery_iterator = std::move(job.children[i + 1]);
          to.select_number = from.select_number;
          to.join = from.join;
          to.disable_deduplication_by_hash_field =
              from.disable_deduplication_by_hash_field;
          to.copy_items = from.copy_items;
          to.temp_table_param = from.temp_table_param;
          to.is_recursive_reference = from.is_recursive_reference;
          to.m_first_distinct = from.m_first_distinct;
          to.m_total_operands = from.m_total_operands;
          to.m_operand_idx = from.m_operand_idx;

          if (to.is_recursive_reference) {
            // Find the recursive reference to ourselves; there should be
            // exactly one, as per the standard.
            RowIterator *recursive_reader = FindSingleIteratorOfType(
                from.subquery_path, AccessPath::FOLLOW_TAIL);
            if (recursive_reader == nullptr) {
              // The recursive reference was optimized away, e.g. due to an
              // impossible WHERE condition, so we're not a recursive
              // reference after all.
              to.is_recursive_reference = false;
            } else {
              to.recursive_reader =
                  down_cast<FollowTailIterator *>(recursive_reader);
            }
          }
        }
        JOIN *subjoin = param->ref_slice == -1 ? nullptr : query_blocks[0].join;

        iterator = unique_ptr_destroy_only<RowIterator>(
            materialize_iterator::CreateIterator(
                thd, std::move(query_blocks), param, std::move(table_iterator),
                subjoin));

        assert(iterator);
        break;
      }
      case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE: {
        const auto &param = path->materialize_information_schema_table();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.table_path, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<MaterializeInformationSchemaTableIterator>(
            thd, mem_root, std::move(job.children[0]), param.table_list,
            param.condition);
        break;
      }
      case AccessPath::APPEND: {
        const auto &param = path->append();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            const AppendPathParameters &child_param =
                (*param.children)[child_idx];
            todo.push_back({child_param.path,
                            child_param.join,
                            /*eligible_for_batch_mode=*/true,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        vector<unique_ptr_destroy_only<RowIterator>> children;
        children.reserve(param.children->size());
        for (unique_ptr_destroy_only<RowIterator> &child : job.children) {
          children.push_back(std::move(child));
        }
        iterator =
            NewIterator<AppendIterator>(thd, mem_root, std::move(children));
        break;
      }
      case AccessPath::WINDOW: {
        const auto &param = path->window();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        if (param.needs_buffering) {
          iterator = NewIterator<BufferingWindowIterator>(
              thd, mem_root, std::move(job.children[0]), param.temp_table_param,
              join, param.ref_slice);
        } else {
          iterator = NewIterator<WindowIterator>(
              thd, mem_root, std::move(job.children[0]), param.temp_table_param,
              join, param.ref_slice);
        }
        break;
      }
      case AccessPath::WEEDOUT: {
        const auto &param = path->weedout();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<WeedoutIterator>(
            thd, mem_root, std::move(job.children[0]), param.weedout_table,
            param.tables_to_get_rowid_for);
        break;
      }
      case AccessPath::REMOVE_DUPLICATES: {
        const auto &param = path->remove_duplicates();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<RemoveDuplicatesIterator>(
            thd, mem_root, std::move(job.children[0]), join, param.group_items,
            param.group_items_size);
        break;
      }
      case AccessPath::REMOVE_DUPLICATES_ON_INDEX: {
        const auto &param = path->remove_duplicates_on_index();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<RemoveDuplicatesOnIndexIterator>(
            thd, mem_root, std::move(job.children[0]), param.table, param.key,
            param.loosescan_key_len);
        break;
      }
      case AccessPath::ALTERNATIVE: {
        const auto &param = path->alternative();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, 2);
          todo.push_back(job);
          todo.push_back({param.child,
                          join,
                          eligible_for_batch_mode,
                          &job.children[0],
                          {}});
          todo.push_back({param.table_scan_path,
                          join,
                          eligible_for_batch_mode,
                          &job.children[1],
                          {}});
          continue;
        }
        iterator = NewIterator<AlternativeIterator>(
            thd, mem_root, param.table_scan_path->table_scan().table,
            std::move(job.children[0]), std::move(job.children[1]),
            param.used_ref);
        break;
      }
      case AccessPath::CACHE_INVALIDATOR: {
        const auto &param = path->cache_invalidator();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<CacheInvalidatorIterator>(
            thd, mem_root, std::move(job.children[0]), param.name);
        break;
      }
      case AccessPath::DELETE_ROWS: {
        const auto &param = path->delete_rows();
        if (job.children.is_null()) {
          // Setting up tables for delete must be done before the child
          // iterators are created, as some of the child iterators need to see
          // the final read set when they are constructed, so doing it in
          // DeleteRowsIterator's constructor or Init() is too late.
          SetUpTablesForDelete(thd, join);
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<DeleteRowsIterator>(
            thd, mem_root, std::move(job.children[0]), join,
            param.tables_to_delete_from, param.immediate_tables);
        break;
      }
      case AccessPath::UPDATE_ROWS: {
        const auto &param = path->update_rows();
        if (job.children.is_null()) {
          // Do the final setup for UPDATE before the child iterators are
          // created.
          if (FinalizeOptimizationForUpdate(join)) {
            return nullptr;
          }
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = CreateUpdateRowsIterator(thd, mem_root, join,
                                            std::move(job.children[0]));
        break;
      }
      case AccessPath::PARTIAL_RESULT_CACHE: {
        ptrc::PathParameters *param = path->ptrc().param;
        unique_ptr_destroy_only<RowIterator> child =
            CreateIteratorFromAccessPath(thd, path->ptrc().child, join,
                                         eligible_for_batch_mode);
        iterator = NewIterator<ptrc::PtrcIterator>(
            thd, mem_root, param->key_tables, param->res_tables,
            std::move(child), param->res_items, param->res_tmp_table,
            param->was_null,
            !param->is_exists_subquery);
        break;
      }
      case AccessPath::PARALLEL_SCAN: {
        //@TODO: release orig_iterator
        assert(join && join->query_block);
        double estimated_build_rows = path->num_output_rows();
        if (path->num_output_rows() < 0.0) {
          // Not all access paths may propagate their costs properly.
          // Choose a fairly safe estimate (it's better to be too large
          // than too small).
          estimated_build_rows = 1048576.0;
        }
        path->parallel_scan().m_root_access_path->iterator =
            join->m_root_iterator.get();
        iterator = NewIterator<ParallelScanIterator>(
            thd, mem_root, path->parallel_scan().qep_tab,
            path->parallel_scan().table, estimated_build_rows, examined_rows,
            path->parallel_scan().join, path->parallel_scan().gather,
            path->parallel_scan().stab_output,
            path->parallel_scan().m_root_access_path);
        break;
      }
      case AccessPath::PQ_BLOCK_SCAN: {
        Gather_operator *gather = path->pq_block_scan().gather;
        double estimated_build_rows = path->num_output_rows();
        if (path->num_output_rows() < 0.0) {
          // Not all access paths may propagate their costs properly.
          // Choose a fairly safe estimate (it's better to be too large
          // than too small).
          estimated_build_rows = 1048576.0;
        }
        iterator = NewIterator<PQblockScanIterator>(
            thd, mem_root, path->pq_block_scan().table, estimated_build_rows,
            examined_rows, path->pq_block_scan().tabType, gather,
            path->pq_block_scan().qep_tab, path->pq_block_scan().need_rowid,
            join->m_msg_handler);
        break;
      }
      case AccessPath::PQ_REF_SCAN: {
        double estimated_build_rows = path->num_output_rows();
        if (path->num_output_rows() < 0.0) {
          // Not all access paths may propagate their costs properly.
          // Choose a fairly safe estimate (it's better to be too large
          // than too small).
          estimated_build_rows = 1048576.0;
        }
        iterator = NewIterator<PQRefIterator>(
            thd, mem_root, path->pq_ref_scan().table, path->pq_ref_scan().ref,
            path->pq_ref_scan().use_order, path->pq_ref_scan().tabType,
            estimated_build_rows, examined_rows, path->pq_ref_scan().gather,
            path->pq_ref_scan().qep_tab);
      }
    }

    if (iterator == nullptr) {
      return nullptr;
    }

    path->iterator = iterator.get();
    *job.destination = std::move(iterator);
  }
  return ret;
}

void FindTablesToGetRowidFor(AccessPath *path) {
  table_map handled_by_others = 0;

  auto add_tables_handled_by_others = [path, &handled_by_others](
                                          AccessPath *subpath, const JOIN *) {
    if (path == subpath) return false;  // Skip ourselves.
    switch (subpath->type) {
      case AccessPath::HASH_JOIN:
        handled_by_others |=
            GetUsedTableMap(subpath, /*include_pruned_tables=*/true);
        FindTablesToGetRowidFor(subpath);
        return true;  // Don't double-traverse.
      case AccessPath::BKA_JOIN:
        handled_by_others |= GetUsedTableMap(subpath->bka_join().outer,
                                             /*include_pruned_tables=*/true);
        FindTablesToGetRowidFor(subpath);
        return true;  // Don't double-traverse.
      case AccessPath::STREAM: {
        subpath->stream().provide_rowid = true;
        TABLE *table = subpath->stream().table;
        if (table->pos_in_table_list == nullptr) {
          // Don't need to set anything; see comment on the similar
          // test in NewSortAccessPath().
        } else {
          handled_by_others |= table->pos_in_table_list->map();
        }
        // Doesn't really matter, we don't cross query blocks anyway.
        return true;
      }
      case AccessPath::PARTIAL_RESULT_CACHE:
        handled_by_others |=
            GetUsedTableMap(subpath, /*include_pruned_tables=*/true);
        FindTablesToGetRowidFor(subpath);
        return true;  // Don't double-traverse.
      default:
        return false;
    }
  };

  // We stop at MATERIALIZE and STREAM (they supply row IDs for us without
  // having to ask the tables below).
  switch (path->type) {
    case AccessPath::HASH_JOIN:
      WalkAccessPaths(path, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->hash_join().store_rowids = true;
      path->hash_join().tables_to_get_rowid_for =
          GetUsedTableMap(path, /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    case AccessPath::BKA_JOIN:
      WalkAccessPaths(path->bka_join().outer, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->bka_join().store_rowids = true;
      path->bka_join().tables_to_get_rowid_for =
          GetUsedTableMap(path->bka_join().outer,
                          /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    case AccessPath::WEEDOUT:
      WalkAccessPaths(path, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->weedout().tables_to_get_rowid_for =
          GetUsedTableMap(path, /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    case AccessPath::SORT:
      WalkAccessPaths(path, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->sort().tables_to_get_rowid_for =
          GetUsedTableMap(path, /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    case AccessPath::PARTIAL_RESULT_CACHE:

      WalkAccessPaths(path, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->ptrc().param->res_tables->set_store_rowids(true);
      path->ptrc().param->res_tables->set_tables_to_get_rowid_for(
          GetUsedTableMap(path, /*include_pruned_tables=*/true) &
          ~handled_by_others);
      break;
    default:
      my_abort();
  }
}

// Move the join conditions that are left in path->filter_predicates into the
// hash join predicate of the given HASH_JOIN access path. Note that join
// conditions with subqueries are not moved. If the subqueries need to be
// materialized, then a filter access path is expected from the caller.
// So they will continue to stay as filters on top of the hash join.
//
// TODO(khatlen): It's a bit of a hack to widen the hash join condition like
// this after the plan has been found. It would be better if we found a way to
// encode the necessary information in the hypergraph itself. For example, when
// creating cycles in the hypergraph, we could add redundant complex hyperedges
// in addition to the simple cycle edges that we currently add.
static void MoveFilterPredicatesIntoHashJoinCondition(
    THD *thd, AccessPath *path, const Mem_root_array<Predicate> &predicates,
    int num_where_predicates) {
  Mem_root_array<Item_eq_base *> equijoin_conditions(thd->mem_root);
  Mem_root_array<Item *> join_conditions(thd->mem_root);
  MutableOverflowBitset moved_predicates(thd->mem_root, predicates.size());

  for (int filter_idx : BitsSetIn(path->filter_predicates)) {
    if (filter_idx >= num_where_predicates) break;
    const Predicate &predicate = predicates[filter_idx];
    if (!predicate.was_join_condition) continue;

    Item *condition = predicate.condition;
    // Conditions with subqueries are not moved.
    if (condition->has_subquery()) continue;
    moved_predicates.SetBit(filter_idx);
    if (condition->type() == Item::FUNC_ITEM &&
        down_cast<Item_func *>(condition)
            ->contains_only_equi_join_condition()) {
      equijoin_conditions.push_back(down_cast<Item_eq_base *>(condition));
    } else {
      join_conditions.push_back(condition);
    }
  }

  if (equijoin_conditions.empty() && join_conditions.empty()) {
    // No join conditions were found in the filter predicates.
    return;
  }

  // Create a new JoinPredicate with all the conditions. We don't fully
  // initialize it, since we're done planning and don't need most of the
  // information any more. Just add enough to make EXPLAIN and
  // CreateIteratorFromAccessPath() happy.
  // TODO(khatlen): Maybe it's better to put directly into the access path those
  // few parts of the join predicate that are needed, and leave the actual
  // predicate and relational expression out.
  auto &param = path->hash_join();
  for (Item_eq_base *item : param.join_predicate->expr->equijoin_conditions) {
    equijoin_conditions.push_back(item);
  }
  for (Item *item : param.join_predicate->expr->join_conditions) {
    join_conditions.push_back(item);
  }
  RelationalExpression *expr = new (thd->mem_root) RelationalExpression(thd);
  expr->type = param.join_predicate->expr->type;
  expr->equijoin_conditions = std::move(equijoin_conditions);
  expr->join_conditions = std::move(join_conditions);
  JoinPredicate *join_predicate = new (thd->mem_root) JoinPredicate;
  join_predicate->expr = expr;
  param.join_predicate = join_predicate;

  path->filter_predicates = OverflowBitset::Xor(
      thd->mem_root, path->filter_predicates, std::move(moved_predicates));
}

Item *ConditionFromFilterPredicates(const Mem_root_array<Predicate> &predicates,
                                    OverflowBitset mask,
                                    int num_where_predicates) {
  List<Item> items;
  for (int pred_idx : BitsSetIn(mask)) {
    if (pred_idx >= num_where_predicates) break;
    items.push_back(predicates[pred_idx].condition);
  }
  return CreateConjunction(&items);
}

void ExpandSingleFilterAccessPath(THD *thd, AccessPath *path, const JOIN *join,
                                  const Mem_root_array<Predicate> &predicates,
                                  unsigned num_where_predicates) {
  // Expand join filters for nested loop joins.
  if (path->type == AccessPath::NESTED_LOOP_JOIN &&
      !path->nested_loop_join().already_expanded_predicates &&
      !(path->nested_loop_join().equijoin_predicates.empty() &&
        path->nested_loop_join()
            .join_predicate->expr->join_conditions.empty()) &&
      path->nested_loop_join().inner->type != AccessPath::ZERO_ROWS) {
    AccessPath *right_path = path->nested_loop_join().inner;
    const RelationalExpression *expr =
        path->nested_loop_join().join_predicate->expr;

    // While we're collecting the join conditions, calculate cost and output
    // rows (purely for display purposes). Note that this mirrors the
    // calculation we are doing in CostingReceiver::ProposeNestedLoopJoin();
    // we don't have space in the AccessPath to store it there.
    double filter_cost = right_path->cost;
    double filter_rows = right_path->num_output_rows();

    List<Item> items;
    for (size_t filter_idx :
         BitsSetIn(path->nested_loop_join().equijoin_predicates)) {
      Item *condition = expr->equijoin_conditions[filter_idx];
      items.push_back(condition);
      filter_cost +=
          EstimateFilterCost(thd, filter_rows, condition, join->query_block)
              .cost_if_not_materialized;
      filter_rows *= EstimateSelectivity(thd, condition, /*trace=*/nullptr);
    }
    for (Item *condition : expr->join_conditions) {
      items.push_back(condition);
      filter_cost +=
          EstimateFilterCost(thd, filter_rows, condition, join->query_block)
              .cost_if_not_materialized;
      filter_rows *= EstimateSelectivity(thd, condition, /*trace=*/nullptr);
    }
    assert(!items.is_empty());

    AccessPath *filter_path = new (thd->mem_root) AccessPath;
    filter_path->type = AccessPath::FILTER;
    filter_path->filter().child = right_path;

    // We don't bother trying to materialize subqueries in join conditions,
    // since they should be very rare.
    filter_path->filter().materialize_subqueries = false;

    CopyBasicProperties(*right_path, filter_path);
    filter_path->filter().condition = CreateConjunction(&items);
    filter_path->cost = filter_cost;
    filter_path->set_num_output_rows(filter_rows);

    path->nested_loop_join().inner = filter_path;

    // Since multiple root paths may have their filters expanded,
    // and the same nested loop may be a subpath in several
    // of them, we need to make sure we don't add the join predicates
    // more than once, so mark them as done here.
    path->nested_loop_join().already_expanded_predicates = true;
  }

  // If a hash join follows an edge that is part of a cycle in the hypergraph,
  // there may be other applicable join predicates left in filter_predicates.
  // Say we have {t1,t2} HJ {t3} along the t1.a=t3.a edge. If there is also a
  // t2.b=t3.b edge, that predicate will be in filtered_predicates. In this
  // case, it is desirable to have t1.a=t3.a AND t2.b=t3.b as the hash join
  // predicate, and remove t2.b=t3.b from the filter predicates.
  if (path->type == AccessPath::HASH_JOIN &&
      path->hash_join().join_predicate->expr->join_predicate_first !=
          path->hash_join().join_predicate->expr->join_predicate_last) {
    MoveFilterPredicatesIntoHashJoinCondition(thd, path, predicates,
                                              num_where_predicates);
  }

  // Expand filters _after_ the access path (these are much more common).
  Item *condition = ConditionFromFilterPredicates(
      predicates, path->filter_predicates, num_where_predicates);
  if (condition == nullptr) {
    return;
  }
  AccessPath *new_path = new (thd->mem_root) AccessPath(*path);
  new_path->filter_predicates.Clear();
  new_path->set_num_output_rows(path->num_output_rows_before_filter);
  new_path->cost = path->cost_before_filter;

  // We don't really know how much of init_cost comes from the filter,
  // but we need to heed the invariant that cost >= init_cost
  // also for the new (non-filter) path we're creating, even if it's
  // just for display. Heuristically allocate as much as possible to
  // the filter.
  double filter_only_cost = path->cost - path->cost_before_filter;
  new_path->init_cost = std::max(new_path->init_cost - filter_only_cost, 0.0);
  new_path->init_once_cost =
      std::max(new_path->init_once_cost - filter_only_cost, 0.0);
  assert(new_path->cost >= new_path->init_cost);
  assert(new_path->init_cost >= new_path->init_once_cost);

  path->type = AccessPath::FILTER;
  path->filter().condition = condition;
  path->filter().child = new_path;
  path->filter().materialize_subqueries = false;

  // Clear filter_predicates, but keep applied_sargable_join_predicates.
  MutableOverflowBitset applied_sargable_join_predicates =
      path->applied_sargable_join_predicates().Clone(thd->mem_root);
  applied_sargable_join_predicates.ClearBits(0, num_where_predicates);
  path->filter_predicates = std::move(applied_sargable_join_predicates);
}

void ExpandFilterAccessPaths(THD *thd, AccessPath *path_arg, const JOIN *join,
                             const Mem_root_array<Predicate> &predicates,
                             unsigned num_where_predicates) {
  WalkAccessPaths(path_arg, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK,
                  [thd, &predicates, num_where_predicates](
                      AccessPath *path, const JOIN *sub_join) {
                    ExpandSingleFilterAccessPath(
                        thd, path, sub_join, predicates, num_where_predicates);
                    return false;
                  });
}

table_map GetHashJoinTables(AccessPath *path) {
  table_map tables = 0;
  WalkAccessPaths(
      path, /*join=*/nullptr, WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
      [&tables](AccessPath *subpath, const JOIN *) {
        if (subpath->type == AccessPath::HASH_JOIN) {
          tables |= GetUsedTableMap(subpath, /*include_pruned_tables=*/true);
          return true;
        }
        return false;
      });
  return tables;
}

static QEP_TAB *AccessPathGetQepTab(AccessPath *path [[maybe_unused]]) {
  QEP_TAB *qep_tab = nullptr;
  switch (path->type) {
    case AccessPath::TABLE_SCAN:
      qep_tab = path->table_scan().table->reginfo.qep_tab;
      break;
    case AccessPath::REF:
      qep_tab = path->ref().table->reginfo.qep_tab;
      break;
    case AccessPath::INDEX_SCAN:
      qep_tab = path->index_scan().table->reginfo.qep_tab;
      break;
    case AccessPath::INDEX_RANGE_SCAN:
      qep_tab = path->index_range_scan()
                    .used_key_part[0]
                    .field->table->reginfo.qep_tab;
      break;
    default:
      break;
  }
  return qep_tab;
}

/// Traverse the access path to mark the subpath that can be chosen as cut
/// table. For semi/anti/outer join, we can not read partial data from the inner
/// input. At same time, parallel query only support TABLE/INDEX/RANGE scan.
static void SetCutTable(AccessPath *path,
                        std::vector<QEP_TAB *> &qep_tab_can_cut) {
  table_map cannot_contain_cut_table = 0;
  auto set_cut_table = [&qep_tab_can_cut, &cannot_contain_cut_table](
                           AccessPath *access_path, const JOIN *) -> bool {
    if (cannot_contain_cut_table & GetUsedTableMap(access_path, true))
      return true;
    switch (access_path->type) {
      case AccessPath::TABLE_SCAN:
      case AccessPath::REF:
      case AccessPath::INDEX_SCAN:
      case AccessPath::INDEX_RANGE_SCAN:
        qep_tab_can_cut.emplace_back(AccessPathGetQepTab(access_path));
        break;
      case AccessPath::NESTED_LOOP_JOIN:
        // Inner table of nested loop semi/anti/outer join can not be cut table
        if (access_path->nested_loop_join().join_type != JoinType::INNER)
          cannot_contain_cut_table |=
              GetUsedTableMap(access_path->nested_loop_join().inner, true);
        break;
      case AccessPath::BKA_JOIN:
        // Inner table of bka semi/anti/outer join can not be cut table
        if (access_path->bka_join().join_type != JoinType::INNER)
          cannot_contain_cut_table |=
              GetUsedTableMap(access_path->bka_join().inner, true);
        break;
      case AccessPath::HASH_JOIN:
        // Inner table of hash semi/anti/outer join can not be cut table
        if (access_path->hash_join().join_predicate->expr->type !=
            RelationalExpression::INNER_JOIN)
          cannot_contain_cut_table |=
              GetUsedTableMap(access_path->hash_join().inner, true);
        break;
      case AccessPath::LIMIT_OFFSET:
        // A table below a LIMIT_OFFSET access path cannot be cut table for an
        // hash join. If such a table is cut into n-subtable, the LIMIT is
        // going to be applied to each subtable resulting in n-times too many
        // rows in the hash table. It is outright wrong for inner and outer
        // joins. For semi and anti joins, there are still too many rows in the
        // hash table, but because they only check the presence/absence resp. of
        // rows in the hash table, it does not produce wrong results at the time
        // of writing. Out of caution, we prevent all four.
      case AccessPath::REMOVE_DUPLICATES:
      case AccessPath::REMOVE_DUPLICATES_ON_INDEX:
      case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
      case AccessPath::WEEDOUT:
        // REMOVE_DUPLICATES, REMOVE_DUPLICATES_ON_INDEX,
        // NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL and
        // WEEDOUT can not contain cut table. If it contains the cut table, it
        // may result in duplicates in the hash table. Because it just remove
        // duplicates within the worker, and there will be duplication
        // between workers.
        return true;
      default:
        break;
    }
    return false;
  };

  WalkAccessPaths(
      path, nullptr,
      /*cross_query_blocks=*/WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
      set_cut_table,
      /*post_order_traversal=*/false);
}

/// Returns a table_map marking the "cut" table, if any. 0 if there is no
/// "cut" table.
/// Per our design, we have five main cases we need to handle:
/// 1) Two table join where the build table is the divided table:
///    No special handling.
/// 2) Two table join where the probe table is the divided table:
///    - If the build input is considered small, there is no special handling.
///    - If the build input is considered large, we convert the build input into
///      a "cut" table.
/// 3) N table join, where N > 2, and the divided table is among the
///    innermost tables:
///    - If the divided table is the build table of the innermost hash join,
///      there is no special handling (as we do in case 1).
///    - If the divided table is the probe table of the innermost hash join,
///      we convert the build table of the innermost hash join to a cut table
///      _if_ it is considered large (as we do in case 2).
/// 4) N table join, where N > 2, and the divided table is not among
///    the innermost tables:
///    We convert the largest of the innermost tables to a "cut" table (measured
///    in number of rows).
void MarkCutTable(AccessPath *root_access_path, const JOIN *join) {
  table_map divided_table_map = 0;
  for (uint i = join->const_tables; i < join->primary_tables; ++i) {
    if (join->qep_tab[i].pq_div_tab) {
      divided_table_map = join->qep_tab[i].table_ref->map();
      break;
    }
  }

  // Since we did an assert on join->thd->has_pq, we should find a divided
  // table.
  assert(divided_table_map > 0);

  std::vector<QEP_TAB *> qep_tab_can_cut;
  auto find_cut_table = [divided_table_map, &qep_tab_can_cut](
                            AccessPath *access_path, const JOIN *) -> bool {
    // The parallel-aware hash join have partial data from the build input
    // and the probe input. If probe input tables of hash join contain
    // divided table, we can choose a table from the build input tables as
    // cut table.
    if (access_path->type == AccessPath::HASH_JOIN &&
        (GetUsedTableMap(access_path->hash_join().outer, true) &
         divided_table_map) > 0) {
      // If probe tables includes divided table, and rowid is needed, and
      // allow to spill to disk, parallel-aware hash join is not applicable
      // because parallel-aware hash join might mess rowid during execution.
      if (!(access_path->hash_join().allow_spill_to_disk &&
            access_path->hash_join().store_rowids))
        // Mark the access paths that can be cut table in the build input.
        SetCutTable(access_path->hash_join().inner, qep_tab_can_cut);
    }
    return false;
  };
  WalkAccessPaths(
      root_access_path, nullptr,
      /*cross_query_blocks=*/WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
      find_cut_table, /*post_order_traversal=*/false);

  if (qep_tab_can_cut.empty()) return;

  // We are now in case 2, 3 with divided probe input and large build input, or
  // 4, where we convert one of the tables in the build input to a "cut" table.
  // If parallel_rows_threshold = 0, we select the largest table in number of
  // rows that comes before the divided table (in QEP_TAB index order) as the
  // "cut" table. If parallel_rows_threshold > 0, we select the table whose
  // prefix rows > parallel_rows_threshold / 2 as the "cut" table.
  // Note that we always assume that the build input is big for now, so case 2
  // is not considered here.
  if (join->thd->variables.parallel_rows_threshold > 0) {
    double prefix_rows_fetch = 1.0;
    for (uint i = join->const_tables; i < join->primary_tables; ++i) {
      prefix_rows_fetch *= join->qep_tab[i].position()->rows_fetched;
      auto it = std::find(qep_tab_can_cut.begin(), qep_tab_can_cut.end(),
                          &join->qep_tab[i]);
      if (it == qep_tab_can_cut.end()) continue;

      if (prefix_rows_fetch >
              ((double)join->thd->variables.parallel_rows_threshold / 2) &&
          !pq_check_not_support_tab(&join->qep_tab[i], true)) {
        join->qep_tab[i].pq_cut_tab = true;
        return;
      }
    }
  } else {
    std::sort(qep_tab_can_cut.begin(), qep_tab_can_cut.end(),
              [](const QEP_TAB *a, const QEP_TAB *b) {
                return a->records() > b->records();
              });

    for (auto qep_tab : qep_tab_can_cut) {
      if (!pq_check_not_support_tab(qep_tab, true)) {
        qep_tab->pq_cut_tab = true;
        return;
      }
    }
  }
}

bool IsPartialResultsInputHashJoin(const JOIN *join, AccessPath *path) {
  assert(path->type == AccessPath::HASH_JOIN);
  // Note: inner == build input, outer == probe input
  const table_map inner_tables = GetUsedTableMap(path->hash_join().inner, true);
  const table_map outer_tables = GetUsedTableMap(path->hash_join().outer, true);

  for (uint i = join->const_tables; i < join->primary_tables; ++i) {
    if (join->qep_tab[i].pq_cut_tab || join->qep_tab[i].pq_div_tab) {
      if (inner_tables & join->qep_tab[i].table_ref->map()) {
        path->hash_join().partial_results_from_build_input = true;
      } else if (outer_tables & join->qep_tab[i].table_ref->map()) {
        path->hash_join().partial_results_from_probe_input = true;
      }
    }
  }

  return (path->hash_join().partial_results_from_build_input &&
          path->hash_join().partial_results_from_probe_input);
}

// Mark in the hash join access paths which of the inputs that provides partial
// results (either from a divided table or from a cut table). This is needed for
// EXPLAIN to display the correct information.
bool MarkPartialInputsForHashJoin(AccessPath *root_access_path,
                                  const JOIN *join, Gather_operator *gather) {
  bool mark_partial_inputs = false;
  auto mark_partial_input_for_hash_join = [join, gather, &mark_partial_inputs](
                                              AccessPath *subpath,
                                              const JOIN *) {
    if (subpath->type == AccessPath::HASH_JOIN) {
      if (IsPartialResultsInputHashJoin(join, subpath)) {
        // Set up the shared area for parallel hash join. This will hold
        // the barriers and other structures that are shared between workers.
        // Gather surely already exists at this point (gather is created before
        // all workers) so we can access it. We allocate the shared area if it
        // has not already been so (which should mean that we're gather).
        if (gather->m_pq_hash_join_shared_context == nullptr) {
          HashJoin::PQHashJoinSharedContext *pq_hash_join =
              new (join->thd->pq_leader->pq_mem_root)
                  HashJoin::PQHashJoinSharedContext(
                      join->thd->pq_dop, join->thd->variables.join_buff_size);
          if (pq_hash_join == nullptr) {
            mark_partial_inputs = true;
            return true;
          }
          gather->m_pq_hash_join_shared_context.reset(pq_hash_join);
        }
      }
    }

    return false;
  };

  WalkAccessPaths(root_access_path, nullptr,
                  WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                  mark_partial_input_for_hash_join,
                  /*post_order_traversal=*/true);
  return mark_partial_inputs;
}

void WalkAccessPathsProxy(AccessPath *path,
                          bool cross_query_blocks [[maybe_unused]],
                          std::function<bool(AccessPath *, const JOIN *)> func,
                          bool post_order_traversal) {
  WalkAccessPaths(path, nullptr,
                  cross_query_blocks
                      ? WalkAccessPathPolicy::ENTIRE_TREE
                      : WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                  func, post_order_traversal);
}
