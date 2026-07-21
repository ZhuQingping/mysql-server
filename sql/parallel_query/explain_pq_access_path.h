#ifndef SQL_JOIN_OPTIMIZER_EXPLAIN_PQ_ACCESS_PATH_H
#define SQL_JOIN_OPTIMIZER_EXPLAIN_PQ_ACCESS_PATH_H

#include <atomic>
#include <chrono>
#include <string>

#include "sql/sql_class.h"

class IteratorProfiler;
class JOIN;
class RowIterator;
struct AccessPath;

class IteratorTimingInfo {
  // To avoid a lot of repetitive writing.
  using steady_clock = std::chrono::steady_clock;

 public:
  IteratorTimingInfo()
      : m_num_rows(0),
        m_num_init_calls(0),
        m_time_spent_in_first_row(0),
        m_time_spent_in_other_rows(0) {}

  IteratorTimingInfo(const IteratorTimingInfo &info) {
    m_num_rows = info.m_num_rows;
    m_num_init_calls = info.m_num_init_calls;
    m_time_spent_in_first_row = info.m_time_spent_in_first_row;
    m_time_spent_in_other_rows = info.m_time_spent_in_other_rows;
  }

  IteratorTimingInfo &operator=(const IteratorTimingInfo &info) {
    if (this == &info) return *this;
    m_num_rows = info.m_num_rows;
    m_num_init_calls = info.m_num_init_calls;
    m_time_spent_in_first_row = info.m_time_spent_in_first_row;
    m_time_spent_in_other_rows = info.m_time_spent_in_other_rows;
    return *this;
  }

 public:
  // Keep same with TimingIterator
  uint64_t m_num_rows;
  uint64_t m_num_init_calls;
  steady_clock::time_point::duration m_time_spent_in_first_row;
  steady_clock::time_point::duration m_time_spent_in_other_rows;
};

class ParallelIterTimingInfo {
 public:
  IteratorTimingInfo *time_info;
  std::atomic<size_t> copy_num;
  size_t pq_dop;

 public:
  ParallelIterTimingInfo() = delete;
  explicit ParallelIterTimingInfo(int dop, THD *thd)
      : copy_num(0), pq_dop(dop) {
    time_info = new (thd->pq_context().mem_root) IteratorTimingInfo[dop];
  }
};

void CollectWorkerIterTimingInfo(AccessPath *worker_path, JOIN *worker_join,
                                 AccessPath *leader_path, JOIN *leader_join);
void CopyLeaderIterTimingInfo(AccessPath *non_parallel_path,
                              AccessPath *parallel_path);
std::string ParallelIteratorTimingString(
    const ParallelIterTimingInfo *worker_info);
void PQGatherTimingInfo(const RowIterator *iter,
                        ParallelIterTimingInfo *worker_info);
void PQCopyTimingInfo(const RowIterator *iter,
                      const ParallelIterTimingInfo *worker_info,
                      IteratorProfiler *profiler);
#endif  // SQL_JOIN_OPTIMIZER_EXPLAIN_PQ_ACCESS_PATH_H
