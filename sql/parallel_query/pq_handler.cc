/* Copyright (c) 2026, Oracle and/or its affiliates.

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

/**
  @file sql/parallel_query/pq_handler.cc
  Parallel Query V1-MVP: Handler/InnoDB contract skeleton implementation.

  Phase 2 scope:
  - Stub implementations for PQ_Range::split(), PQ_Leader_context
    constructor/destructor/build_ranges(), PQ_Worker_context
    dispatch_ctx/get_ctx(), and pq_worker_status_to_string().
  - No handler/InnoDB calls, no thread creation, no scan start.
  - All stubs return failure/empty to ensure they are never accidentally
    used in an execution path before Phase 5/6 implements them properly.
*/

#include "sql/parallel_query/pq_handler.h"

// ========================================================================
// PQ_Range::split() -- stub
// ========================================================================
// Phase 6 InnoDB subclass (PQ_Range in row0pread_pq.h) will override
// this to call PQ_Scan_ctx::partition() on the B+tree with S-lock.
// This stub does nothing; it should never be called before Phase 6.
void PQ_Range::split() {
  // Phase 2 stub: no-op. Will be overridden by InnoDB subclass.
  // If accidentally called, the split flag remains set but no sub-ranges
  // are created, which will cause the worker to find no work and finish.
}

// ========================================================================
// PQ_Leader_context
// ========================================================================

PQ_Leader_context::PQ_Leader_context(size_t max_threads, bool reverse_scan)
    : m_max_threads(max_threads), m_reverse(reverse_scan) {
  // Phase 2: basic field initialization only.
  // Phase 6 will add slices_map, key_map initialization here,
  // mirroring Parallel_leader_Base::Parallel_leader_Base() from
  // the reference implementation.
}

PQ_Leader_context::~PQ_Leader_context() {
  // Phase 2: just clear the scan contexts list.
  // Phase 6 will also clean up slices_map entries and their
  // PQ_slices_mngr allocations.
  m_scan_ctxs.clear();
}

bool PQ_Leader_context::build_ranges(void *trx [[maybe_unused]],
                                      const PQ_Config &config
                                          [[maybe_unused]]) {
  // Phase 2 stub: always returns false (no ranges built).
  // Phase 6 InnoDB subclass will:
  // 1. Call make_scan_ctx(trx, config) to create PQ_Scan_ctx.
  // 2. S-lock the index.
  // 3. Call PQ_Scan_ctx::partition() for B+tree partitioning.
  // 4. Push ranges into the slices queue.
  // 5. S-unlock the index.
  // 6. Return true if ranges were successfully created.
  //
  // Design note: The recommended InnoDB route reuses
  // Parallel_reader::add_scan() for the partitioning step,
  // wrapping its Scan_ctx::partition() and Scan_ctx::create_contexts()
  // into PQ_Scan_ctx::partition(). This avoids duplicating the
  // B+tree traversal algorithm.
  return false;
}

// ========================================================================
// PQ_Worker_context
// ========================================================================

bool PQ_Worker_context::dispatch_ctx(
    const PQ_Ref_info &ref_info [[maybe_unused]],
    std::shared_ptr<PQ_Ctx> *ctx) {
  // Phase 2 stub: always returns true (all ranges exhausted).
  // Phase 5/6 will implement:
  // 1. Call get_ctx(ref_info) to fetch a PQ_Ctx from the slices queue.
  // 2. If ctx is nullptr, return true (worker should finish).
  // 3. If ctx is available, set *ctx and return false.
  *ctx = nullptr;
  return true;
}

std::shared_ptr<PQ_Ctx> PQ_Worker_context::get_ctx(
    const PQ_Ref_info &ref_info [[maybe_unused]]) {
  // Phase 2 stub: always returns nullptr (no contexts available).
  // Phase 5/6 will implement:
  // 1. Look up the slices queue for the given ref key.
  // 2. Fetch a PQ_Range from the queue.
  // 3. If the range needs splitting, call range->split() and retry.
  // 4. Otherwise, create a PQ_Ctx from the range and return it.
  return nullptr;
}

// ========================================================================
// pq_worker_status_to_string
// ========================================================================

const char *pq_worker_status_to_string(PQ_Worker_status status) {
  switch (status) {
    case PQ_Worker_status::NOT_STARTED:
      return "NOT_STARTED";
    case PQ_Worker_status::RUNNING:
      return "RUNNING";
    case PQ_Worker_status::FINISHED:
      return "FINISHED";
    case PQ_Worker_status::ERROR:
      return "ERROR";
    case PQ_Worker_status::KILLED:
      return "KILLED";
    case PQ_Worker_status::ABORTED:
      return "ABORTED";
    default:
      return "UNKNOWN";
  }
}
