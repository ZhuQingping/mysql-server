/****************************************************************************

Copyright (c) 2026, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/row0pread_pq.h
InnoDB PQ (Parallel Query) pull-row adapter for clustered full scan.

TEMPORARY MVP adapter (Phase 6B-2): This file provides InnoDB-internal
types and a pull-row API for SQL-layer PQ workers.

Key design decisions:
1. B+tree partitioning currently exports Parallel_reader-planned ranges.
   DOP=1 real scan will use a single whole clustered range first.
2. The older row_search_mvcc() pull path is a disabled latent adapter.
   V2-8D selected the Parallel_reader visibility adapter route for real
   worker rows, so worker-local row_search_mvcc() must not be wired into
   PQTableScanIterator::Read().
3. The leader trx_t owns the statement read view. Workers may only read a
   leader-equivalent snapshot through controlled visibility adapter code;
   they must not create independent read views or mutate the leader trx.
4. PQ_DB_END_OF_RANGE_INT is an internal adapter state (int, not
   dberr_t); never leaked as InnoDB public dberr_t.
5. Only clustered index full scan is supported in the first real path.
6. Unsupported scenarios fall back to serial execution.

Created 2026-06-02 by Qingping Zhu (PQ Phase 6B-2). */

#ifndef row0pread_pq_h
#define row0pread_pq_h

#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

#include "db0err.h"
#include "dict0dict.h"
#include "mem0mem.h"
#include "page0size.h"
#include "rem0types.h"
#include "row0pread.h"
#include "sql/parallel_query/pq_handler.h"
#include "trx0trx.h"
#include "ut0new.h"

// Forward declarations for types not yet defined by included headers.
// Note: rec_t is a typedef (byte) in rem0types.h, not a struct --
// do NOT forward-declare it as struct rec_t.
struct row_prebuilt_t;

/** Internal end-of-range status for the PQ pull-row adapter.

This is NOT a real InnoDB dberr_t value. It is used only within
the adapter to signal that a worker has exhausted its assigned
range. The public API converts this to eof=true, handler result 0.

We use an int constant rather than dberr_t constexpr to avoid
clang -Wenum-constexpr-conversion errors (the dberr_t enum range
is [0, 2047], and any value outside that range would be flagged).
The actual numeric value 2003 is chosen to be just above the
highest InnoDB dberr_t value (2002) while staying within the
enum's representable range for safe casting. */
constexpr int PQ_DB_END_OF_RANGE_INT = 2003;

/** Boundary iterator for a PQ scan range. */
struct InnoDB_pq_iter {
  ~InnoDB_pq_iter();

  /** Copy an exported Parallel_reader boundary tuple into this iterator. */
  dberr_t assign(const dtuple_t *tuple, const dict_index_t *index);

  const dtuple_t *tuple() const { return m_tuple; }

 private:
  mem_heap_t *m_heap{nullptr};
  const dtuple_t *m_tuple{nullptr};
};

/** A B+tree sub-range for a PQ worker to scan.

V2-4: boundaries are planned by Parallel_reader and stored as deep-copy
tuples. A nullptr boundary still means -infinity/+infinity. */
struct InnoDB_pq_range {
  /** Range ID. */
  size_t m_id{std::numeric_limits<size_t>::max()};

  /** Start boundary (nullptr = -infinity). */
  std::shared_ptr<InnoDB_pq_iter> m_start{nullptr};

  /** End boundary (nullptr = +infinity). */
  std::shared_ptr<InnoDB_pq_iter> m_end{nullptr};

  /** Whether this range should be further split. */
  bool m_split{false};
};

/** Scan context for InnoDB PQ parallel clustered full scan.

Holds the index being scanned, the covering transaction (with
its read view), and the partitioned ranges.

The read view is owned by m_trx (the leader transaction).
Workers access visibility through this scan context without
creating their own snapshots. */
class InnoDB_pq_scan_ctx {
 public:
  InnoDB_pq_scan_ctx(dict_index_t *index, const trx_t *trx,
                     bool is_compact, const page_size_t &page_size);

  ~InnoDB_pq_scan_ctx();

  dict_index_t *index() const { return m_index; }
  const trx_t *trx() const { return m_trx; }
  bool is_compact() const { return m_is_compact; }
  const page_size_t &page_size() const { return m_page_size; }

  const std::vector<InnoDB_pq_range> &ranges() const { return m_ranges; }

  /** @return true if the covering transaction has an active read view. */
  bool has_active_read_view() const;

  /** Validate the first gate for a future Parallel_reader pull adapter.

  This does not read rows. It only checks immutable prerequisites that must be
  true before a PQ worker can use the leader snapshot for visibility.

  @return DB_SUCCESS or DB_UNSUPPORTED. */
  dberr_t validate_pull_adapter_gate() const;

  /** Convert a visible clustered record to a MySQL record image.

  This is a narrow wrapper around row_sel_store_mysql_rec() for the future
  pull adapter. The caller owns mysql_rec, prebuilt, offsets, and blob_heap.

  @return true on success. */
  bool store_mysql_record(byte *mysql_rec, row_prebuilt_t *prebuilt,
                          const rec_t *rec, ulint *offsets,
                          mem_heap_t *blob_heap) const;

  /** Convert the current row from a Parallel_reader callback context.

  This helper is for the future callback producer smoke. It does not advance
  cursors or own row memory.

  @return true on success. */
  bool store_callback_record(byte *mysql_rec, row_prebuilt_t *prebuilt,
                             const Parallel_reader::Ctx *reader_ctx,
                             mem_heap_t *blob_heap) const;

  /** Run a synchronous callback conversion smoke.

  This uses Parallel_reader in synchronous mode and converts at most one visible
  clustered record. It is an internal primitive for future safe-window smoke;
  it is not connected to SQL iterator Read().

  @param[out] mysql_rec  MySQL row buffer.
  @param[in]  prebuilt   Worker row_prebuilt_t for conversion.
  @param[out] converted  True if a row was converted.
  @return DB_SUCCESS or error code. */
  dberr_t smoke_callback_conversion(byte *mysql_rec, row_prebuilt_t *prebuilt,
                                    bool *converted) const;

  /** Produce all callback rows into a SQL-layer row sink.

  This uses Parallel_reader in synchronous mode. Each visible clustered record
  is converted into prebuilt->mysql_template format in mysql_rec and then sent
  through row_sink. The sink must deep-copy the record before returning.

  @param[out] mysql_rec  MySQL row buffer.
  @param[in]  prebuilt   Worker row_prebuilt_t for conversion.
  @param[in]  row_sink   SQL-layer sink for converted rows.
  @return DB_SUCCESS or error code. */
  dberr_t produce_callback_rows(byte *mysql_rec, row_prebuilt_t *prebuilt,
                                PQ_row_sink *row_sink) const;

  /** Produce callback rows from one assigned range.

  A nullptr range means this worker owns no range and should produce EOF
  without rows. This is needed before DOP>1 can safely use workers when ranges
  are fewer than requested workers.

  @param[out] mysql_rec  MySQL row buffer.
  @param[in]  prebuilt   Worker row_prebuilt_t for conversion.
  @param[in]  row_sink   SQL-layer sink for converted rows.
  @param[in]  range      Assigned static scan range, or nullptr.
  @return DB_SUCCESS or error code. */
  dberr_t produce_callback_rows_for_range(
      byte *mysql_rec, row_prebuilt_t *prebuilt, PQ_row_sink *row_sink,
      const InnoDB_pq_range *range) const;

  /** Mutable access to ranges for dispatch. */
  std::vector<InnoDB_pq_range> &mutable_ranges() { return m_ranges; }

  /** Partition the B+tree for parallel scan.

  V1-MVP: creates a single range covering the entire clustered
  index. Proper multi-range partitioning deferred to Phase 8.

  @param[in]  split_level  B+tree level to split at (unused in MVP).
  @return DB_SUCCESS or error code. */
  dberr_t partition(size_t split_level);

  /** Check visibility of a record.

  V2-8F placeholder: real visibility must follow upstream Parallel_reader
  semantics using the leader statement read view. The disabled
  row_search_mvcc() latent path is not the selected real execution route.

  @return true (placeholder; real check deferred). */
  bool check_visibility(const rec_t *&rec, ulint *&offsets,
                        mem_heap_t *&heap, mtr_t *mtr);

  void set_error_state(dberr_t err) {
    m_err.store(err, std::memory_order_relaxed);
  }
  bool is_error_set() const {
    return m_err.load(std::memory_order_relaxed) != DB_SUCCESS;
  }
  dberr_t get_error_state() const {
    return m_err.load(std::memory_order_relaxed);
  }

 private:
  dict_index_t *m_index{nullptr};
  const trx_t *m_trx{nullptr};
  bool m_is_compact{false};
  page_size_t m_page_size;
  size_t m_depth{0};
  std::vector<InnoDB_pq_range> m_ranges;
  std::atomic<dberr_t> m_err{DB_SUCCESS};
};

/** Pull-row cursor context for a PQ worker.

V1-MVP: Pull-row via row_search_mvcc() through the worker's
row_prebuilt_t. On first read_record() call, positions the cursor
with the same protocol as ha_innobase::index_first(); subsequent
calls advance forward (ROW_SEL_NEXT). This path must stay disabled
until worker snapshot ownership is proven. When end-of-index is
reached, returns eof=true. */
class InnoDB_pq_ctx {
 public:
  InnoDB_pq_ctx(size_t id, InnoDB_pq_scan_ctx *scan_ctx,
                InnoDB_pq_range *range);
  ~InnoDB_pq_ctx();

  /** Pull the next visible row and convert to MySQL format.

  Disabled latent adapter that uses row_search_mvcc() through the worker's
  row_prebuilt_t,
  which naturally handles:
  - MVCC visibility (only after the worker snapshot contract is proven)
  - Record to MySQL format conversion (row_sel_store_mysql_rec)
  - BLOB handling
  - Deleted record handling
  - mtr/latch management

  @param[out]  mysql_rec  MySQL row buffer (table->record[0]).
  @param[in]   prebuilt   row_prebuilt_t for scan state and conversion.
  @param[out]  eof        True when range exhausted.
  @return 0 on success, handler error code on fatal error. */
  int read_record(byte *mysql_rec, row_prebuilt_t *prebuilt, bool *eof);

  /** Reset cursor for scanning a new range.
  @param[in]  range  New range to scan. */
  void reset_for_new_range(InnoDB_pq_range *range);

  size_t id() const { return m_id; }
  InnoDB_pq_scan_ctx *scan_ctx() const { return m_scan_ctx; }
  InnoDB_pq_range *range() const { return m_range; }

 private:
  size_t m_id{std::numeric_limits<size_t>::max()};
  InnoDB_pq_scan_ctx *m_scan_ctx{nullptr};
  InnoDB_pq_range *m_range{nullptr};
  bool m_start_of_range{true};
  bool m_range_exhausted{false};
};

/** Leader context for InnoDB PQ parallel clustered full scan.

Created by pq_leader_scan_init(). Owns the scan context and
partitioned ranges. */
class InnoDB_pq_leader_ctx {
 public:
  InnoDB_pq_leader_ctx(size_t max_threads, bool reverse);
  ~InnoDB_pq_leader_ctx();

  /** Initialize: create scan context and partition.
  @param[in]  index       Clustered index to scan.
  @param[in]  trx         Covering transaction with read view.
  @param[in]  is_compact  Row format flag.
  @param[in]  page_size   Tablespace page size.
  @return DB_SUCCESS or error code. */
  dberr_t init(dict_index_t *index, trx_t *trx, bool is_compact,
               const page_size_t &page_size);

  /** Dispatch the next range to a worker.
  @return pointer to next range, or nullptr if all dispatched. */
  InnoDB_pq_range *dispatch_next_range();

  size_t max_threads() const { return m_max_threads; }
  bool is_reverse() const { return m_reverse; }
  InnoDB_pq_scan_ctx *scan_ctx() const { return m_scan_ctx; }
  size_t n_ranges() const {
    return m_scan_ctx != nullptr ? m_scan_ctx->ranges().size() : 0;
  }
  size_t n_dispatched() const { return m_next_range_id; }
  bool all_ranges_dispatched() const { return m_next_range_id >= n_ranges(); }

  void set_error_state(dberr_t err) {
    m_err.store(err, std::memory_order_relaxed);
  }
  bool is_error_set() const {
    return m_err.load(std::memory_order_relaxed) != DB_SUCCESS;
  }
  dberr_t get_error_state() const {
    return m_err.load(std::memory_order_relaxed);
  }

  trx_t *trx() const { return m_trx; }

 private:
  size_t m_max_threads{0};
  bool m_reverse{false};
  trx_t *m_trx{nullptr};
  InnoDB_pq_scan_ctx *m_scan_ctx{nullptr};
  size_t m_next_range_id{0};
  std::atomic<dberr_t> m_err{DB_SUCCESS};
  std::vector<InnoDB_pq_range> m_empty_ranges;
};

/** Worker context for InnoDB PQ pull-row scan.

Created by pq_worker_scan_init(). Holds an InnoDB_pq_ctx
for the assigned range. */
class InnoDB_pq_worker_ctx {
 public:
  InnoDB_pq_worker_ctx(size_t worker_id, InnoDB_pq_leader_ctx *leader_ctx);
  ~InnoDB_pq_worker_ctx();

  /** Initialize cursor for the assigned range.
  @param[in]  range  Range this worker will scan. */
  void init(InnoDB_pq_range *range);

  /** Pull next row from cursor.
  @param[out]  mysql_rec  MySQL row buffer.
  @param[in]   prebuilt   row_prebuilt_t for scan state.
  @param[out]  eof        True when range exhausted.
  @return 0 on success, handler error code on fatal error. */
  int read_record(byte *mysql_rec, row_prebuilt_t *prebuilt, bool *eof);

  size_t worker_id() const { return m_worker_id; }
  InnoDB_pq_leader_ctx *leader_ctx() const { return m_leader_ctx; }
  InnoDB_pq_ctx *cursor_ctx() const { return m_cursor_ctx; }
  bool is_initialized() const { return m_cursor_ctx != nullptr; }
  InnoDB_pq_range *assigned_range() const { return m_assigned_range; }

  void set_error_state(dberr_t err) {
    m_err.store(err, std::memory_order_relaxed);
  }
  bool is_error_set() const {
    return m_err.load(std::memory_order_relaxed) != DB_SUCCESS;
  }

 private:
  size_t m_worker_id{std::numeric_limits<size_t>::max()};
  InnoDB_pq_leader_ctx *m_leader_ctx{nullptr};
  InnoDB_pq_ctx *m_cursor_ctx{nullptr};
  InnoDB_pq_range *m_assigned_range{nullptr};
  std::atomic<dberr_t> m_err{DB_SUCCESS};
};

#endif /* !row0pread_pq_h */
