#include "explain_pq_access_path.h"
#include "scope_guard.h"          // create_scope_guard
#include "sql-common/json_dom.h"  // Json_object
#include "sql/iterators/composite_iterators.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/explain_access_path.h"
#include "sql/sql_executor.h"  // QEP_TAB

#include <cinttypes>
#include <functional>
#include <string>
#include <vector>

template <class T>
using duration = std::chrono::duration<T>;
using std::vector;

/**
 * Serialize access path tree structure into vector, refer to function
 * "ExplainAccessPath".
 *
 * @param path[in]:   The AccessPath to traverse.
 * @param paths[out]: Serialized AccessPath elements.
 */
static void SerializeAccessPath(AccessPath *path,
                                std::vector<AccessPath *> *paths) {
  assert(path && paths);
  WalkAccessPathsProxy(
      path, /*cross_query_blocks=*/true,
      [&paths](AccessPath *p, const JOIN *) {
        if (nullptr == p) return true;
        paths->push_back(p);
        std::vector<ExplainChild> children;
        switch (p->type) {
          case AccessPath::REF: {
            AddChildrenFromIndexKey(p->ref().table, p->ref().ref, &children);
            break;
          }
          case AccessPath::REF_OR_NULL: {
            AddChildrenFromIndexKey(p->ref_or_null().table,
                                    p->ref_or_null().ref, &children);
            break;
          }
          case AccessPath::EQ_REF: {
            AddChildrenFromIndexKey(p->eq_ref().table, p->eq_ref().ref,
                                    &children);
            break;
          }
          case AccessPath::FILTER: {
            GetAccessPathsFromItem(p->filter().condition, "condition",
                                   &children);
            break;
          }
          case AccessPath::MATERIALIZE: {
            Index_lookup *ref = nullptr;
            if (p->materialize().table_path->type == AccessPath::REF) {
              ref = p->materialize().table_path->ref().ref;
            } else if (p->materialize().table_path->type ==
                       AccessPath::REF_OR_NULL) {
              ref = p->materialize().table_path->ref_or_null().ref;
            } else if (p->materialize().table_path->type ==
                       AccessPath::EQ_REF) {
              ref = p->materialize().table_path->eq_ref().ref;
            }
            AddChildrenFromIndexKey(/*only_base_table=*/nullptr, ref,
                                    &children);
            break;
          }
          case AccessPath::PQ_REF_SCAN: {
            AddChildrenFromIndexKey(p->pq_ref_scan().table,
                                    p->pq_ref_scan().ref, &children);
            break;
          }
          case AccessPath::PQ_BLOCK_SCAN: {
            AddChildrenFromIndexKey(p->pq_block_scan().table,
                                    &p->pq_block_scan().qep_tab->ref(),
                                    &children);
            break;
          }
          default:
            break;
        }
        for (size_t i = 0; i < children.size(); i++) {
          SerializeAccessPath(children[i].path, paths);
        }
        // Manually release the memory of Json_object.
        for (auto &child : children) {
          if (child.obj) {
            delete child.obj;
            child.obj = nullptr;
          }
        }
        return false;
      },
      /*post_order_traversal=*/false);
}

// Collect all paths from a path.
static void CollectAllPaths(JOIN *join, AccessPath *path,
                            vector<AccessPath *> *paths) {
  std::vector<ExplainChild> children;

  // Manually release the memory of Json_object.
  auto grd = create_scope_guard([&]() {
    for (auto &child : children) {
      if (child.obj) {
        delete child.obj;
        child.obj = nullptr;
      }
    }
  });

  SerializeAccessPath(path, paths);
  GetAccessPathsFromSelectList(join, &children);
  for (const auto &child : children) {
    SerializeAccessPath(child.path, paths);
  }
}

/**
 * Collect worker iterator timing info into leader's access path.
 *
 * @param worker_path[in]:  The worker's AccessPath to traverse.
 * @param worker_join[in]:  The worker's join object.
 * @param leader_path[out]: The leader's AccessPath to traverse.
 * @param leader_join[in]:  The leader's join object.
 */
void CollectWorkerIterTimingInfo(AccessPath *worker_path, JOIN *worker_join,
                                 AccessPath *leader_path, JOIN *leader_join) {
  if (!worker_path || !leader_path || (worker_path == leader_path)) {
    return;
  }
  std::vector<AccessPath *> worker_paths, leader_paths;

  // find access path from select list
  CollectAllPaths(worker_join, worker_path, &worker_paths);
  CollectAllPaths(leader_join, leader_path, &leader_paths);

  size_t iterator_num = std::min(worker_paths.size(), leader_paths.size());
  for (size_t i = 0; i < iterator_num; i++) {
    if (worker_paths[i] == leader_paths[i]) {
      continue;
    }
    // worker and leader should have same access path type
    if (worker_paths[i]->type != leader_paths[i]->type) {
      continue;
    }
    // Collect worker iterator execution info into leader iterator
    if (worker_paths[i]->iterator && leader_paths[i]->iterator) {
      leader_paths[i]->iterator->GatherTimingInfo(worker_paths[i]->iterator);
    }
  }
}

/**
 * Find pq sharing materialized table iterator, store into map, key of map is
 * QueryBlock select_number, value of map is QueryBlock subquery_path.
 *
 * @param path[in]: The AccessPath to traverse.
 *
 * @return unordered_map to store subquery_path of sharing materialized table.
 */
static std::unordered_map<int, AccessPath *> FindShareMaterializeNode(
    AccessPath *path,
    std::unordered_map<int, MaterializePathParameters *> *params) {
  std::vector<AccessPath *> share_mat;
  WalkAccessPathsProxy(
      path, /*cross_query_blocks=*/false,
      [&share_mat](AccessPath *p, const JOIN *) {
        if (p->type == AccessPath::MATERIALIZE && p->iterator) {
          auto mat_it =
              down_cast<TableRowIterator *>(p->iterator->real_iterator());
          if (mat_it->pq_sharing_table()) share_mat.push_back(p);
        }
        return false;
      },
      /*post_order_traversal=*/true);
  std::unordered_map<int, AccessPath *> materialize_info;
  for (size_t i = 0; i < share_mat.size(); i++) {
    MaterializePathParameters *param = share_mat[i]->materialize().param;
    for (const MaterializePathParameters::QueryBlock &query_block :
         param->query_blocks) {
      materialize_info[query_block.select_number] = query_block.subquery_path;
      params->emplace(query_block.select_number, param);
    }
  }
  return materialize_info;
}

/**
 * Since pq leader materialize shared tmp table for shared table scenario,
 * the execution iterator timing info stores in
 * ParallelScanIterator::m_root_access_path, refer to function
 * "ParallelScanIterator::para_exec_init", need to copy these execution timing
 * info to parallel access path.
 *
 * @param non_parallel_path[in]:  Non parallel AccessPath to traverse.
 * @param parallel_path[out]: Parallel AccessPath to traverse.
 */
void CopyLeaderIterTimingInfo(AccessPath *non_parallel_path,
                              AccessPath *parallel_path) {
  if (!parallel_path || !non_parallel_path ||
      (parallel_path == non_parallel_path)) {
    return;
  }
  std::unordered_map<int, MaterializePathParameters *> non_para_params;
  std::unordered_map<int, MaterializePathParameters *> para_params;
  std::unordered_map<int, AccessPath *> non_para_share_mat =
      FindShareMaterializeNode(non_parallel_path, &non_para_params);
  std::unordered_map<int, AccessPath *> para_share_mat =
      FindShareMaterializeNode(parallel_path, &para_params);

  for (const auto &iter : non_para_share_mat) {
    int select_number = iter.first;
    auto search = para_share_mat.find(select_number);
    if (search == para_share_mat.end()) {
      continue;
    }
    std::vector<AccessPath *> parallel_paths;
    std::vector<AccessPath *> non_parallel_paths;
    SerializeAccessPath(iter.second, &non_parallel_paths);
    SerializeAccessPath(search->second, &parallel_paths);
    size_t iterator_num =
        std::min(parallel_paths.size(), non_parallel_paths.size());
    for (size_t i = 0; i < iterator_num; i++) {
      if (parallel_paths[i] == non_parallel_paths[i]) {
        continue;
      }
      // worker and leader should have same access path type
      if (parallel_paths[i]->type != non_parallel_paths[i]->type) {
        continue;
      }
      if (!parallel_paths[i]->iterator || !non_parallel_paths[i]->iterator) {
        continue;
      }
      // copy worker iterator execution info into leader iterator
      parallel_paths[i]->iterator->CopyTimingInfo(
          non_parallel_paths[i]->iterator);
    }
  }

  for (const auto &non_pq_param : non_para_params) {
    auto pq_param = para_params.find(non_pq_param.first);
    // continue if not find param by query_block select_number
    if (pq_param == para_params.end()) {
      continue;
    }
    if (non_pq_param.second == pq_param->second) {
      continue;
    }
    // save serial materialize path parameter
    pq_param->second->saved_param = non_pq_param.second;
  }
}

/**
 * Aggregate iterator timing info of all workers, print the average execution
 * time, maximum execution time, and minimum execution time info of parallel
 * iterators.
 *
 * @param worker_info[in]: Iterator running timing info for all pq workers.
 *
 * @return Aggregated iterator timing info.
 */
std::string ParallelIteratorTimingString(
    const ParallelIterTimingInfo *worker_info) {
  assert(worker_info->pq_dop > 0);
  double sum_start_time = 0;
  double max_start_time = std::numeric_limits<double>::min();
  double min_start_time = std::numeric_limits<double>::max();
  double sum_end_time = 0;
  double max_end_time = std::numeric_limits<double>::min();
  double min_end_time = std::numeric_limits<double>::max();
  double sum_rows = 0;
  uint64_t max_rows = 0;
  uint64_t min_rows = 0;
  double sum_init_calls = 0;
  uint64_t max_init_calls = 0;
  uint64_t min_init_calls = 0;
  uint64_t work_threads = 0;
  for (size_t i = 0; i < worker_info->pq_dop; i++) {
    IteratorTimingInfo info = worker_info->time_info[i];
    if (info.m_num_init_calls > 0) {
      double first_row_ms =
          duration<double>(info.m_time_spent_in_first_row).count() * 1e3;
      double last_row_ms = duration<double>(info.m_time_spent_in_first_row +
                                            info.m_time_spent_in_other_rows)
                               .count() *
                           1e3;
      sum_start_time += (first_row_ms / info.m_num_init_calls);
      sum_end_time += (last_row_ms / info.m_num_init_calls);
      sum_rows += info.m_num_rows;
      sum_init_calls += info.m_num_init_calls;
      // update maximum and minimum end time worker index info
      if (last_row_ms > max_end_time) {
        max_end_time = last_row_ms / info.m_num_init_calls;
        max_start_time = first_row_ms / info.m_num_init_calls;
        max_rows = llrintf(static_cast<double>(info.m_num_rows) /
                           info.m_num_init_calls);
        max_init_calls = info.m_num_init_calls;
      }
      if (last_row_ms < min_end_time) {
        min_end_time = last_row_ms / info.m_num_init_calls;
        min_start_time = first_row_ms / info.m_num_init_calls;
        min_rows = llrintf(static_cast<double>(info.m_num_rows) /
                           info.m_num_init_calls);
        min_init_calls = info.m_num_init_calls;
      }
      work_threads++;
    }
  }
  if (sum_init_calls == 0) {
    return "(never executed)";
  } else {
    char buf[1024];
    std::string extra_info = ")";
    if (work_threads < worker_info->pq_dop) {
      extra_info = " " + std::to_string(worker_info->pq_dop - work_threads) +
                   " worker not executed)";
    }
    // print avg, max, min info
    snprintf(buf, sizeof(buf),
             "(actual time=%.3f,%.3f,%.3f..%.3f,%.3f,%.3f rows=%lld,%" PRIu64
             ",%" PRIu64 " loops=%lld,%" PRIu64 ",%" PRIu64,
             sum_start_time / work_threads, max_start_time, min_start_time,
             sum_end_time / work_threads, max_end_time, min_end_time,
             llrintf(sum_rows / sum_init_calls), max_rows, min_rows,
             llrintf(sum_init_calls / work_threads), max_init_calls,
             min_init_calls);
    return buf + extra_info;
  }
}

/**
 * Gather iterator running info from worker to leader.
 *
 * @param iter[in]: Iterator info.
 * @param worker_info[out]: Iterator running timing info for all pq workers.
 *
 * @returns void.
 */
void PQGatherTimingInfo(const RowIterator *iter,
                        ParallelIterTimingInfo *worker_info) {
  if (!current_thd || !worker_info) return;
  assert(iter && current_thd->pq_context().worker_info);
  IteratorTimingInfo running_info;
  iter->GetProfiler()->CopyToRunningInfo(running_info);
  if (running_info.m_num_init_calls > 0) {
    worker_info->time_info[current_thd->pq_context().worker_info->worker_index] =
        running_info;
    worker_info->copy_num++;
  }
}

/**
 * Copy running info from non-parallel iterator to parallel iterator for
 * parallel query leader.
 *
 * @param iter[in]: Iterator info.
 * @param worker_info[in]: Iterator running timing info for all pq workers.
 * @param profiler[out]: Iterator profiler info.
 *
 * @returns void.
 */
void PQCopyTimingInfo(const RowIterator *iter,
                      const ParallelIterTimingInfo *worker_info,
                      IteratorProfiler *profiler) {
  assert(iter);
  if (worker_info && worker_info->copy_num > 0) return;
  if (profiler->GetNumInitCalls() > 0) return;
  IteratorTimingInfo running_info;
  iter->GetProfiler()->CopyToRunningInfo(running_info);
  if (running_info.m_num_init_calls > 0) {
    profiler->SetFromRunningInfo(running_info);
  }
}
