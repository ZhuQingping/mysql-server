#include "sql/parallel_query/explain_pq_access_path.h"

#include <sstream>

#include "sql/iterators/timing_iterator.h"

void CollectWorkerIterTimingInfo(AccessPath *, JOIN *, AccessPath *, JOIN *) {}

void CopyLeaderIterTimingInfo(AccessPath *, AccessPath *) {}

std::string ParallelIteratorTimingString(
    const ParallelIterTimingInfo *worker_info) {
  if (worker_info == nullptr) return {};

  std::ostringstream out;
  out << "parallel workers: " << worker_info->pq_dop;
  return out.str();
}

void PQGatherTimingInfo(const RowIterator *, ParallelIterTimingInfo *) {}

void PQCopyTimingInfo(const RowIterator *, const ParallelIterTimingInfo *,
                      IteratorProfiler *) {}
