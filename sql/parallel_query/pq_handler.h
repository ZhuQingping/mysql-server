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

#ifndef PQ_HANDLER_INCLUDED
#define PQ_HANDLER_INCLUDED

/**
  @file sql/parallel_query/pq_handler.h
  Parallel Query V1-MVP: Handler/InnoDB contract types.

  Phase 2 scope:
  - Define SQL-layer PQ handler contract types (PQ_Range, PQ_Slice,
    PQ_Scan_ctx, PQ_Leader_context, PQ_Worker_context).
  - These types are engine-agnostic base classes; InnoDB will subclass them
    with engine-specific members in row0pread_pq.h / ha_innodb_pq.cc.
  - No execution path hookup, no handler/InnoDB calls.
  - Not connected to sql/handler.h or storage/ code in this phase.

  Design rationale:
  - The upstream Parallel_reader (row0pread.h) provides thread budgeting,
    B+tree partitioning, visibility checks, and clustered full-scan primitives.
    It operates in push-row model: worker threads call a callback for each row.
  - SQL-layer PQ needs pull-row model: worker THD pulls rows via
    PQ_Ctx::read_record() to fill table->record[0], then executes the plan.
  - The contract types below bridge this gap: PQ_Leader_context orchestrates
    partitioning and thread budget; PQ_Worker_context provides pull-row API.
  - Phase 2 skeleton does NOT allocate threads, start scans, or call
    Parallel_reader. It only defines the type interfaces so that Phase 5/6
    can implement the concrete InnoDB subclass that wraps Parallel_reader.
*/

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <memory>
#include <utility>
#include <vector>

// Minimal MySQL header for key_part_map used by PQ_Ref_info
#include "my_base.h"

/**
  InnoDB error code constants as int values.

  These have the same numeric value as InnoDB's DB_SUCCESS etc.
  Used in the base-class interface so that engine-independent code can
  represent InnoDB-style error codes without pulling in db0err.h.

  Phase 2: defined as constexpr int, with static_assert verification
  deferred to the InnoDB subclass phase (Phase 6).
*/
constexpr int PQ_DB_SUCCESS = 10;
constexpr int PQ_DB_ERROR = 11;
constexpr int PQ_DB_END_OF_INDEX = 1502;
constexpr int PQ_DB_END_OF_RANGE = 1504;
constexpr int PQ_DB_NOT_FOUND = 1505;

// Forward declarations for base classes
class MQueue_handle;
class PQ_Slice;
class PQ_Range;
class PQ_Scan_ctx;
class PQ_Ctx;
class PQ_Leader_context;
class PQ_Worker_context;
class THD;
class handler;
struct TABLE;

/**
  Worker open carrier for V2 real execution.

  The SQL worker layer owns this carrier. Engine-specific worker contexts may
  reference these objects but must not own the worker TABLE or handler.
*/
struct PQ_Worker_open_context {
  THD *worker_thd{nullptr};
  TABLE *worker_table{nullptr};
  handler *worker_handler{nullptr};
  TABLE *leader_table{nullptr};
  PQ_Leader_context *leader_ctx{nullptr};
  uint worker_id{0};
  uint actual_dop{0};
  MQueue_handle *mq_handle{nullptr};
};

enum class PQ_leader_scan_mode : uint { PROBE, EXECUTE };

enum class PQ_Leader_context_kind { GENERIC, INNODB };

enum class PQ_Worker_context_kind { GENERIC, INNODB };

/**
  Specifies the scan range boundaries.

  Phase 2: uses generic void* pointers for start/end keys.
  The concrete InnoDB subclass (row0pread_pq.h) will replace these
  with dtuple_t* or rec_t* pointers.
*/
struct PQ_Borders {
  const void *m_start{};  ///< Start of scan (nullptr = -infinity)
  const void *m_end{};    ///< End of scan (nullptr = +infinity)

  PQ_Borders() = default;
  PQ_Borders(const void *start, const void *end) : m_start(start), m_end(end) {}
};

/**
  Scan configuration base struct.

  Phase 2: holds borders and reverse-scan flag only.
  The InnoDB subclass (PQ_Config in row0pread_pq.h) adds
  dict_index_t*, page_size, is_compact, etc.
*/
struct PQ_Config {
  PQ_Borders m_scan_borders;  ///< Range boundaries
  bool m_pq_reverse_scan{false};  ///< True if reverse scan

  explicit PQ_Config(const PQ_Borders &borders) : m_scan_borders(borders) {}
  PQ_Config(const PQ_Config &) = default;
  virtual ~PQ_Config() = default;
};

/**
  A ref key that can be copied and compared.

  Used for "ref field" scan scenarios where each ref key value
  corresponds to a different slices queue. Phase 2 skeleton
  provides the type but does not integrate with InnoDB ref access.

  Uses std::vector<uchar> to hold key bytes, which provides
  correct copy/move semantics automatically (Rule of Zero).
*/
struct PQ_ref_key {
  std::vector<uchar> m_data;

  PQ_ref_key() = default;

  /**
    Construct from a raw byte source.

    @param src  Pointer to key bytes; nullptr is treated as empty key
    @param n    Number of bytes; if src is nullptr, n is ignored
  */
  PQ_ref_key(const uchar *src, std::size_t n) {
    if (src != nullptr && n > 0) {
      m_data.assign(src, src + n);
    }
  }

  /// @return pointer to key bytes, or nullptr if empty
  const uchar *ptr() const {
    return m_data.empty() ? nullptr : m_data.data();
  }

  /// @return length of key bytes
  std::size_t len() const { return m_data.size(); }

  bool operator==(const PQ_ref_key &other) const {
    return m_data == other.m_data;
  }
};

/**
  A bundle of ref-access description for "ref field" scans.

  Phase 2: defined but not used in any execution path.
  Phase 6/11 will integrate with InnoDB ref scan dispatch.
*/
struct PQ_Ref_info {
  bool pq_ref_depend{false};       ///< True if ref value is not constant
  const uchar *pq_ref_key_ptr{nullptr};  ///< Key value to look up
  uint pq_ref_key_len{0};          ///< Length of key value
};

/**
  Base class for a scan slice / execution context.

  A Slice represents a unit of work assigned to a worker thread.
  PQ_Range (B+tree range) and PQ_Ctx (execution context reading rows)
  both inherit from PQ_Slice.

  Phase 2: defines the interface without implementation.
*/
class PQ_Slice {
 public:
  PQ_Slice(size_t id, PQ_Scan_ctx *scan_ctx, bool split)
      : m_id(id), m_split(split), m_scan_ctx(scan_ctx) {}

  virtual ~PQ_Slice() = default;

  size_t id() const { return m_id; }

  void set_split(bool split) { m_split = split; }
  bool need_split() const { return m_split; }

  /// Split this context into sub-ranges. Engine-specific override.
  virtual void split() {}

  PQ_Scan_ctx *scan_ctx() const { return m_scan_ctx; }

 protected:
  size_t m_id{std::numeric_limits<size_t>::max()};
  bool m_split{false};
  PQ_Scan_ctx *m_scan_ctx{};
};

/**
  Iterator base for range boundaries.

  Phase 2: abstract interface; InnoDB subclass (Iter in row0pread_pq.h)
  adds m_heap, m_rec, m_tuple, m_pcur etc.
*/
struct PQ_Iter {
  virtual ~PQ_Iter() = default;
  virtual const void *get_border() = 0;
};

/** Range represented as a pair of iterators [first, second). */
using PQ_Iter_pair = std::pair<std::shared_ptr<PQ_Iter>,
                                std::shared_ptr<PQ_Iter>>;

/**
  A B+tree range that a worker will scan.

  Phase 2: inherits PQ_Slice, holds iterator pair for boundaries.
  The InnoDB PQ_Range subclass (row0pread_pq.h) overrides split()
  to call Scan_ctx::partition() on the B+tree.
*/
class PQ_Range : public PQ_Slice {
 public:
  PQ_Iter_pair m_iters;

  PQ_Range(size_t id, PQ_Scan_ctx *scan_ctx, PQ_Iter_pair &iters, bool split)
      : PQ_Slice(id, scan_ctx, split), m_iters(iters) {}

  void split() override;  // Phase 2: declared, .cc has stub
};

using PQ_Ranges = std::vector<std::shared_ptr<PQ_Range>>;

/**
  Scan context base: owns the partitioning algorithm and configuration.

  Phase 2: abstract base with virtual partition() and index lock methods.
  The InnoDB subclass (PQ_Scan_ctx in row0pread_pq.h) implements these
  using B+tree traversal and InnoDB mutex/latch primitives.
*/
class PQ_Scan_ctx {
 public:
  PQ_Scan_ctx(PQ_Leader_context *reader, size_t id, PQ_Config &config)
      : m_id(id), m_config(config), m_reader(reader) {}

  virtual ~PQ_Scan_ctx() = default;

  /// S-lock the index to prevent structure changes during partitioning.
  virtual void index_s_lock() = 0;

  /// S-unlock the index after partitioning.
  virtual void index_s_unlock() = 0;

  /// Partition the data source into ranges for parallel scan.
  virtual void partition(const PQ_Borders &scan_borders, size_t level,
                         PQ_Ranges &ranges) = 0;

  /// Create an execution context for a range.
  virtual std::shared_ptr<PQ_Ctx> make_ctx(
      std::shared_ptr<PQ_Range> &range) = 0;

  PQ_Leader_context *reader() const { return m_reader; }
  size_t id() const { return m_id; }

  /// Error state management.
  void set_error_state(int err) {
    m_err.store(err, std::memory_order_relaxed);
  }
  bool is_error_set() const {
    return m_err.load(std::memory_order_relaxed) != PQ_DB_SUCCESS;
  }

 protected:
  size_t m_id{std::numeric_limits<size_t>::max()};
  PQ_Config &m_config;  ///< Reference to config stored in subclass
  size_t m_depth{};      ///< B+tree depth (set during partitioning)
  PQ_Leader_context *m_reader{};
  std::atomic<int> m_err{PQ_DB_SUCCESS};
};

/**
  Execution context: what a worker thread uses to pull rows from a range.

  Phase 2: abstract base with read_record() pull-row API.
  The InnoDB subclass (PQ_Ctx in row0pread_pq.h) implements
  read_record() by traversing B+tree pages within the range
  and converting InnoDB records to MySQL format.

  This is the key interface difference from Parallel_reader's push-row
  model. Instead of a callback invoked per-row, the SQL worker calls
  read_record(buf, handler_specific) to pull one row at a time,
  which fills table->record[0] and lets the worker execute its plan
  step (WHERE filter, aggregation, MQ send).
*/
class PQ_Ctx : public PQ_Slice {
 public:
  PQ_Ctx(size_t id, PQ_Scan_ctx *scan_ctx,
         std::shared_ptr<PQ_Range> &range)
      : PQ_Slice(id, scan_ctx, false), m_range(range) {}

  virtual ~PQ_Ctx() = default;

  /**
    Pull one record from the scan range into the provided buffer.

    @param[out] buf             Record buffer (table->record[0])
    @param      handler_specific  Engine-specific pointer

    @retval PQ_DB_SUCCESS       Row read successfully
    @retval PQ_DB_END_OF_RANGE  No more rows in this range
    @retval PQ_DB_ERROR         Fatal error
  */
  virtual int read_record(uchar *buf, void *handler_specific) = 0;

  /// Reset cursor state for a new read round (ref-scan multi-round).
  virtual void reset_for_next_read_round(bool reverse) = 0;

 protected:
  std::shared_ptr<PQ_Range> m_range{};
};

/**
  Leader context: orchestrates partitioning, thread budget, and scan lifecycle.

  Phase 2: skeleton with build_ranges() declaration and thread budget fields.
  The InnoDB subclass (Parallel_leader in row0pread_pq.h) implements
  build_ranges() using Parallel_reader::add_scan() for B+tree partitioning
  and the existing thread budget infrastructure.

  Design note on InnoDB route:
  - Phase 2 recommends a HYBRID approach:
    1. Reuse Parallel_reader for thread budgeting (available_threads/release_threads),
       B+tree partitioning (Scan_ctx::partition), and visibility checks
       (Scan_ctx::check_visibility).
    2. Add a thin PQ-specific adapter that converts Parallel_reader's push-row
       model to PQ_Ctx's pull-row model. The adapter does NOT duplicate
       row0pread algorithms; it wraps Parallel_reader::Scan_ctx and Ctx
       to expose them as PQ_Scan_ctx and PQ_Ctx subclass instances.
    3. The adapter is only needed where the SQL worker THD/JOIN/RowIterator
       pull model cannot be represented by the existing push-row adapter
       (Parallel_reader_adapter, used by handler::parallel_scan for DDL).

  This means:
  - No full independent row0pread_pq.cc implementation of partitioning.
  - row0pread_pq.h contains thin wrapper classes that delegate to
    Parallel_reader internals.
  - ha_innodb_pq.cc implements the handler PQ integration: leader scan init,
    worker scan init/next, and ref-scan range construction.
*/
class PQ_Leader_context {
 public:
  constexpr static size_t MAX_THREADS{256};

  /**
    Constructor.

    @param max_threads  Maximum worker threads for this scan
    @param reverse_scan True if scanning in reverse order
  */
  PQ_Leader_context(size_t max_threads, bool reverse_scan);

  virtual ~PQ_Leader_context();

  virtual PQ_Leader_context_kind kind() const {
    return PQ_Leader_context_kind::GENERIC;
  }

  size_t max_threads() const { return m_max_threads; }
  bool is_reverse() const { return m_reverse; }

  /**
    Build scan ranges by partitioning the data source.

    Phase 2: declared but stub-implemented (returns false).
    Phase 6 InnoDB subclass will delegate to Parallel_reader::add_scan()
    and Scan_ctx::partition().

    @param trx        Opaque transaction pointer (trx_t* in InnoDB)
    @param config     Scan configuration

    @retval true   Ranges built successfully
    @retval false  Error or empty table
  */
  virtual bool build_ranges(void *trx, const PQ_Config &config);

  /// Error state
  void set_error_state(int err) {
    m_err.store(err, std::memory_order_relaxed);
  }
  bool is_error_set() const {
    return m_err.load(std::memory_order_relaxed) != PQ_DB_SUCCESS;
  }

  /// Factory: create engine-specific PQ_Scan_ctx. Must be overridden.
  virtual std::shared_ptr<PQ_Scan_ctx> make_scan_ctx(
      void *handler_specific, const PQ_Config &config) = 0;

 protected:
  size_t m_max_threads{};
  size_t m_scan_ctx_id{};
  bool m_reverse{false};
  std::atomic<int> m_err{PQ_DB_SUCCESS};

  using Scan_ctxs = std::list<std::shared_ptr<PQ_Scan_ctx>>;
  Scan_ctxs m_scan_ctxs{};
};

/**
  Worker context: what a worker THD uses to dispatch and pull rows.

  Phase 2: skeleton with dispatch_ctx() and get_ctx() declarations.
  The concrete implementation (Parallel_worker in pq_handler.cc for
  engine-independent dispatch logic, plus InnoDB-specific parts in
  ha_innodb_pq.cc) will:
  - Maintain a local queue of PQ_Ctx instances (for ref-scan multi-round).
  - Fetch ranges from the leader's global slices queue.
  - Call PQ_Ctx::read_record() to pull rows into table->record[0].

  Phase 2: declared but not connected to any execution path.
*/
class PQ_Worker_context {
 public:
  PQ_Worker_context(bool reverse, PQ_Leader_context &leader)
      : m_reverse(reverse), m_leader(leader) {}

  virtual ~PQ_Worker_context() = default;

  virtual PQ_Worker_context_kind kind() const {
    return PQ_Worker_context_kind::GENERIC;
  }

  /**
    Pick a PQ_Ctx for execution based on ref key.

    @param ref_info  Ref key description (for "ref field" scans)
    @param[out] ctx  Found context (nullptr if not found)

    @retval true   All ranges exhausted (worker should finish)
    @retval false  A context is available for execution
  */
  virtual bool dispatch_ctx(const PQ_Ref_info &ref_info,
                            std::shared_ptr<PQ_Ctx> *ctx);

  /// Get the next available PQ_Ctx.
  virtual std::shared_ptr<PQ_Ctx> get_ctx(const PQ_Ref_info &ref_info);

 protected:
  bool m_reverse{false};
  PQ_Leader_context &m_leader;
};

/**
  Push sink for worker-produced row images.

  InnoDB callback producers write each visible row into the worker TABLE record
  buffer, then call send_row(). The SQL layer owns the sink implementation and
  must deep-copy the row image into Exchange/MQ before send_row() returns.
*/
class PQ_row_sink {
 public:
  virtual ~PQ_row_sink() = default;

  /**
    Send the current worker TABLE record image.

    @retval false  Row accepted
    @retval true   Sink failed or aborted
  */
  virtual bool send_row(TABLE *source_table) = 0;

  /** @return true when the producer should stop early. */
  virtual bool should_abort() const { return false; }

  /** @return true when an early stop requested by should_abort() is normal. */
  virtual bool stop_is_success() const { return false; }
};

/**
  Scan status codes for PQ worker lifecycle.

  Used by Gather_operator to track worker state transitions.
  Phase 2: defined but not integrated into execution path.
*/
enum class PQ_Worker_status {
  NOT_STARTED,     ///< Worker thread not yet launched
  RUNNING,         ///< Worker is actively scanning rows
  FINISHED,        ///< Worker completed all ranges normally
  ERROR,           ///< Worker encountered a fatal error
  KILLED,          ///< Worker was killed (by leader or KILL command)
  ABORTED          ///< Worker aborted due to leader/other worker error
};

/**
  Convert PQ_Worker_status to human-readable string.
  Phase 2: implemented in pq_handler.cc.
*/
const char *pq_worker_status_to_string(PQ_Worker_status status);

/**
  Handler PQ API signature declarations.

  These are the method signatures that InnoDB's ha_innobase will
  implement in Phase 6. They are NOT added to sql/handler.h in
  Phase 2; they are documented here as contract declarations.

  Phase 2 records these signatures so that Phase 5 (plan rewrite)
  and Phase 6 (InnoDB full scan) know what handler API to call.

  Current handler PQ API:
  - int pq_leader_scan_init(THD *leader_thd, PQ_Leader_context **leader_ctx,
                            PQ_leader_scan_mode mode, uint requested_dop,
                            uint *actual_dop, bool reverse);
    Leader initializes partitioning. PROBE must remain fallback-safe; EXECUTE
    is the future no-fallback commit point for read-view binding.

  - int pq_worker_scan_init(PQ_Worker_open_context *open_ctx,
                            PQ_Worker_context **worker_ctx);
    Worker binds an independently opened worker TABLE/handler/record buffer to
    scan context.

  - int pq_worker_scan_next(uchar *buf);
    Worker pulls one row into buf, returns 0/HA_ERR_END_OF_FILE/error.

  - int pq_worker_scan_callback_produce(PQ_Worker_context *worker_ctx,
                                        PQ_row_sink *row_sink);
    Worker pushes each callback-produced row into a SQL-owned row sink.

  - void pq_worker_scan_end();
    Worker ends scan; idempotent.

  - void pq_leader_scan_end(void *scan_ctx);
    Leader releases partitioning resources and thread budget.
*/

#endif  // PQ_HANDLER_INCLUDED
