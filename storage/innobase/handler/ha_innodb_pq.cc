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

/** @file handler/ha_innodb_pq.cc
InnoDB Parallel Query handler adapter.
*/

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "ha_innodb.h"
#include "mysql/plugin.h"
#include "row0pread_pq.h"
#include "row0sel.h"
#include "sql/parallel_query/pq_handler.h"
#include "sql/parallel_query/pq_resource_stat.h"
#include "sql/parallel_query/sql_parallel.h"
#include "sql/query_options.h"

#include "dict0dict.h"
#include "row0mysql.h"
#include "srv0srv.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "ut0new.h"

static int pq_map_dberr_to_handler_error(dberr_t err, bool *eof) {
  if (eof != nullptr) {
    *eof = false;
  }

  if (static_cast<int>(err) == PQ_DB_END_OF_RANGE_INT) {
    if (eof != nullptr) {
      *eof = true;
    }
    return 0;
  }

  switch (err) {
    case DB_SUCCESS:
      return 0;
    case DB_END_OF_INDEX:
    case DB_NOT_FOUND:
      if (eof != nullptr) {
        *eof = true;
      }
      return 0;
    case DB_OUT_OF_MEMORY:
      return HA_ERR_OUT_OF_MEM;
    case DB_INTERRUPTED:
      return HA_ERR_QUERY_INTERRUPTED;
    case DB_UNSUPPORTED:
      return HA_ERR_UNSUPPORTED;
    case DB_DEADLOCK:
      return HA_ERR_LOCK_DEADLOCK;
    case DB_LOCK_WAIT_TIMEOUT:
      return HA_ERR_LOCK_WAIT_TIMEOUT;
    default:
      return convert_error_code_to_mysql(err, 0, nullptr);
  }
}

class PQ_counting_row_sink final : public PQ_row_sink {
 public:
  PQ_counting_row_sink(PQ_row_sink *inner, uint max_rows)
      : m_inner(inner), m_max_rows(max_rows) {}

  bool send_row(TABLE *source_table) override {
    if (m_max_rows == 0 || m_rows >= m_max_rows) {
      m_limit_hit = true;
      return true;
    }
    if (m_inner == nullptr || m_inner->send_row(source_table)) {
      return true;
    }
    ++m_rows;
    return false;
  }

  bool should_abort() const override { return m_limit_hit || inner_abort(); }

  bool stop_is_success() const override {
    return m_limit_hit || (m_inner != nullptr && m_inner->stop_is_success());
  }

  uint rows() const { return m_rows; }
  bool limit_hit() const { return m_limit_hit; }
  bool inner_abort() const {
    return m_inner != nullptr && m_inner->should_abort();
  }

 private:
  PQ_row_sink *const m_inner;
  const uint m_max_rows;
  uint m_rows{0};
  bool m_limit_hit{false};
};

static bool pq_primary_range_start_flag_supported(const key_range *key) {
  return key == nullptr || key->keypart_map == 0 ||
         key->flag == HA_READ_KEY_OR_NEXT || key->flag == HA_READ_AFTER_KEY ||
         key->flag == HA_READ_KEY_EXACT;
}

static bool pq_primary_range_end_flag_supported(const key_range *key) {
  return key == nullptr || key->keypart_map == 0 ||
         key->flag == HA_READ_BEFORE_KEY || key->flag == HA_READ_AFTER_KEY ||
         key->flag == HA_READ_KEY_EXACT;
}

static bool pq_primary_range_flags_supported(const key_range *start_key,
                                             const key_range *end_key) {
  return pq_primary_range_start_flag_supported(start_key) &&
         pq_primary_range_end_flag_supported(end_key);
}

static dberr_t pq_seek_primary_range_boundary(ha_innobase *handler, TABLE *table,
                                              row_prebuilt_t *prebuilt,
                                              uint keyno,
                                              const key_range *key,
                                              bool start_boundary,
                                              dtuple_t **tuple,
                                              bool *empty_range) {
  if (tuple == nullptr || empty_range == nullptr) {
    return DB_UNSUPPORTED;
  }

  *tuple = nullptr;
  if (key == nullptr || key->keypart_map == 0) {
    return DB_SUCCESS;
  }

  if (handler == nullptr || table == nullptr || prebuilt == nullptr ||
      keyno >= table->s->keys || key->key == nullptr || key->length == 0 ||
      prebuilt->pq_heap == nullptr) {
    return DB_UNSUPPORTED;
  }

  const enum ha_rkey_function seek_flag =
      start_boundary
          ? (key->flag == HA_READ_AFTER_KEY ? HA_READ_AFTER_KEY
                                            : HA_READ_KEY_OR_NEXT)
          : (key->flag == HA_READ_BEFORE_KEY ? HA_READ_KEY_OR_NEXT
                                             : HA_READ_AFTER_KEY);
  const uint key_len = calculate_key_len(table, keyno, key->keypart_map);

  prebuilt->pq_tuple = nullptr;
  prebuilt->pq_index_read = true;
  const int error =
      handler->index_read(table->record[0], key->key, key_len, seek_flag);
  prebuilt->pq_index_read = false;

  if (error == 0) {
    if (prebuilt->pq_tuple == nullptr) {
      return DB_UNSUPPORTED;
    }
    *tuple = prebuilt->pq_tuple;
    return DB_SUCCESS;
  }

  if (error == HA_ERR_KEY_NOT_FOUND) {
    if (start_boundary) {
      *empty_range = true;
    }
    return DB_SUCCESS;
  }

  return DB_UNSUPPORTED;
}

class InnoDB_pq_sql_leader_context final : public PQ_Leader_context {
 public:
  explicit InnoDB_pq_sql_leader_context(InnoDB_pq_leader_ctx *innodb_ctx)
      : PQ_Leader_context(innodb_ctx != nullptr ? innodb_ctx->max_threads() : 0,
                          innodb_ctx != nullptr && innodb_ctx->is_reverse()),
        m_innodb_ctx(innodb_ctx) {}

  PQ_Leader_context_kind kind() const override {
    return PQ_Leader_context_kind::INNODB;
  }

  std::shared_ptr<PQ_Scan_ctx> make_scan_ctx(
      void *handler_specific [[maybe_unused]],
      const PQ_Config &config [[maybe_unused]]) override {
    return nullptr;
  }

  InnoDB_pq_leader_ctx *innodb_ctx() const { return m_innodb_ctx; }

 private:
  InnoDB_pq_leader_ctx *m_innodb_ctx{nullptr};
};

class InnoDB_pq_sql_worker_context final : public PQ_Worker_context {
 public:
  InnoDB_pq_sql_worker_context(InnoDB_pq_sql_leader_context &leader,
                               InnoDB_pq_worker_ctx *innodb_ctx,
                               PQ_Worker_open_context *open_ctx)
      : PQ_Worker_context(leader.is_reverse(), leader),
        m_innodb_ctx(innodb_ctx),
        m_open_ctx(open_ctx) {}

  ~InnoDB_pq_sql_worker_context() override {
    if (m_innodb_ctx != nullptr) {
      ut::delete_(m_innodb_ctx);
      m_innodb_ctx = nullptr;
    }
  }

  PQ_Worker_context_kind kind() const override {
    return PQ_Worker_context_kind::INNODB;
  }

  InnoDB_pq_worker_ctx *innodb_ctx() const { return m_innodb_ctx; }
  PQ_Worker_open_context *open_ctx() const { return m_open_ctx; }

 private:
  InnoDB_pq_worker_ctx *m_innodb_ctx{nullptr};
  PQ_Worker_open_context *m_open_ctx{nullptr};
};

/**
  Initialize InnoDB PQ leader scan for clustered full scan.

  Phase 6B-2: Real implementation.

  Steps:
  1. Validate that we're on a clustered index full scan.
  2. Create InnoDB_pq_leader_ctx and partition metadata.
  3. Return the leader context via the PQ_Leader_context** parameter.

  V2-3/V2-8E/M5: PROBE is still used as a bridge probe before fallback. It may
  bind a temporary read view for local smoke checks, but only as reversible
  handler state owned by the returned leader context. EXECUTE is the
  no-fallback commit point and binds the leader statement read view before
  workers may read through a Parallel_reader visibility adapter.

  Conservative behavior: any unsupported scenario returns
  HA_ERR_UNSUPPORTED, causing fallback to serial execution.
  This does NOT change existing serial query behavior.

  @param[in]  leader_thd      Leader thread THD
  @param[out] leader_ctx      Output leader context
  @param[in]  mode            PROBE is fallback-safe; EXECUTE is commit point
  @param[in]  requested_dop   Requested DOP
  @param[in]  reverse         Reverse scan (unsupported in V1-MVP)
  @return 0 on success, handler error code on failure
*/
int ha_innobase::pq_leader_scan_init(THD *leader_thd,
                                     PQ_Leader_context **leader_ctx,
                                     PQ_leader_scan_mode mode,
                                     uint requested_dop, uint *actual_dop,
                                     bool reverse) {
  if (leader_ctx != nullptr) {
    *leader_ctx = nullptr;
  }
  if (actual_dop != nullptr) {
    *actual_dop = 0;
  }
  (void)leader_thd;

  auto record_probe_gate_unsupported = [mode]() {
    if (mode == PQ_leader_scan_mode::PROBE) {
      pq_global_stats.probe_gate_unsupported.fetch_add(
          1, std::memory_order_relaxed);
    }
  };
  auto record_probe_thread_budget_unsupported = [mode]() {
    if (mode == PQ_leader_scan_mode::PROBE) {
      pq_global_stats.probe_thread_budget_unsupported.fetch_add(
          1, std::memory_order_relaxed);
    }
  };
  auto record_probe_init_unsupported = [mode]() {
    if (mode == PQ_leader_scan_mode::PROBE) {
      pq_global_stats.probe_init_unsupported.fetch_add(
          1, std::memory_order_relaxed);
    }
  };

  if (m_pq_leader_ctx != nullptr || m_pq_sql_leader_ctx != nullptr ||
      !m_pq_worker_ctxs.empty()) {
    pq_leader_scan_end(static_cast<PQ_Leader_context *>(nullptr));
  }

  /* V1-MVP: reverse scan is not supported. */
  if (reverse) {
    record_probe_gate_unsupported();
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (mode != PQ_leader_scan_mode::PROBE &&
      mode != PQ_leader_scan_mode::EXECUTE) {
    record_probe_gate_unsupported();
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  /* DOP must be at least 1. */
  if (requested_dop == 0) {
    record_probe_gate_unsupported();
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  /* Validate prebuilt and choose the clustered scan index. The SQL PQ
  iterator probes before TableScanIterator::Init() calls rnd_init(), so
  m_prebuilt->index may still be unset on the first eligible execution. */
  if (m_prebuilt == nullptr || m_prebuilt->table == nullptr ||
      m_prebuilt->trx == nullptr) {
    record_probe_gate_unsupported();
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  auto index = m_prebuilt->index != nullptr ? m_prebuilt->index
                                            : m_prebuilt->table->first_index();
  if (index == nullptr) {
    record_probe_gate_unsupported();
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  /* Only support clustered index full scan in V1-MVP. */
  if (!index->is_clustered()) {
    record_probe_gate_unsupported();
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  /* Validate that the index is usable. */
  if (!index->is_usable(m_prebuilt->trx)) {
    record_probe_gate_unsupported();
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  auto trx = m_prebuilt->trx;
  bool close_read_view_on_end = false;

  if (m_prebuilt->select_lock_type != LOCK_NONE) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  /* Check thread budget: are enough parallel read threads available? */
  auto available = Parallel_reader::available_threads(requested_dop, false);
  if (available < requested_dop) {
    if (available > 0) {
      Parallel_reader::release_threads(available);
    }
    record_probe_thread_budget_unsupported();
    /* Not enough threads available; fallback to serial. */
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  /* Create InnoDB PQ leader context. */
  auto innodb_leader_ctx = ut::new_withkey<InnoDB_pq_leader_ctx>(
      UT_NEW_THIS_FILE_PSI_KEY, requested_dop, reverse);

  if (innodb_leader_ctx == nullptr) {
    Parallel_reader::release_threads(available);
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  auto cleanup_execute_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  if (mode == PQ_leader_scan_mode::PROBE ||
      mode == PQ_leader_scan_mode::EXECUTE) {
    const bool had_active_read_view =
        srv_read_only_mode ||
        (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
    trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
    if (!srv_read_only_mode) {
      trx_assign_read_view(trx);
    }
    close_read_view_on_end =
        !had_active_read_view &&
        !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
  }

  /* Initialize: partition the B+tree. */
  bool is_compact = dict_table_is_comp(index->table);
  page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));

  innodb_leader_ctx->set_close_read_view_on_end(close_read_view_on_end);
  auto err = innodb_leader_ctx->init(index, trx, is_compact, page_size);

  if (err != DB_SUCCESS) {
    cleanup_execute_read_view();
    ut::delete_(innodb_leader_ctx);
    Parallel_reader::release_threads(available);
    if (err == DB_UNSUPPORTED) {
      record_probe_init_unsupported();
    }
    return pq_map_dberr_to_handler_error(err, nullptr);
  }

  /* If the table is empty (no ranges), still return success.
  Workers will get eof=true immediately on their first next call. */
  if (innodb_leader_ctx->n_ranges() == 0) {
    /* Empty table: no parallelism needed, but the API is valid. */
    /* Note: In V1-MVP we could also return unsupported here
    to fallback serial, but returning the empty leader ctx
    is more correct -- empty table scan is trivially parallel-safe. */
  }

  auto sql_leader_ctx = ut::new_withkey<InnoDB_pq_sql_leader_context>(
      UT_NEW_THIS_FILE_PSI_KEY, innodb_leader_ctx);
  if (sql_leader_ctx == nullptr) {
    cleanup_execute_read_view();
    ut::delete_(innodb_leader_ctx);
    Parallel_reader::release_threads(available);
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  if (mode == PQ_leader_scan_mode::EXECUTE) {
    m_prebuilt->sql_stat_start = false;
  }

  /* Store the InnoDB PQ leader context in the handler for
  internal use (worker init, scan next, cleanup). */
  m_pq_leader_ctx = innodb_leader_ctx;
  m_pq_sql_leader_ctx = sql_leader_ctx;
  pq_global_stats.ranges_built.fetch_add(innodb_leader_ctx->n_ranges(),
                                         std::memory_order_relaxed);
  if (leader_ctx != nullptr) {
    *leader_ctx = sql_leader_ctx;
  }
  if (actual_dop != nullptr) {
    *actual_dop = static_cast<uint>(available);
  }

  return 0;
}

int ha_innobase::pq_leader_scan_init(uint keyno, void *&scan_ctx,
                                     uint n_threads) {
  scan_ctx = nullptr;

  if (n_threads == 0 || m_prebuilt == nullptr || m_prebuilt->trx == nullptr ||
      m_prebuilt->table == nullptr) {
    return HA_ERR_UNSUPPORTED;
  }

  if (ha_reverse_scan() || pq_ref || pq_range_type != PQ_QUICK_SELECT_NONE) {
    return HA_ERR_UNSUPPORTED;
  }

  active_index = keyno;
  const int index_result = change_active_index(active_index);
  if (index_result != 0) {
    return index_result;
  }

  PQ_Leader_context *typed_ctx = nullptr;
  uint actual_dop = 0;
  const int result = pq_leader_scan_init(
      ha_thd(), &typed_ctx, PQ_leader_scan_mode::EXECUTE, n_threads,
      &actual_dop, ha_reverse_scan());
  if (result != 0) {
    scan_ctx = nullptr;
    return result;
  }

  scan_ctx = typed_ctx;
  return 0;
}

/**
  Initialize InnoDB PQ worker scan for clustered full scan.

  This entry only creates a worker context after the SQL layer has opened an
  independent worker TABLE/handler. The public pull-row API remains disabled;
  guarded callback/smoke paths use this context.

  @param[in]  open_ctx     Worker open context; used only for V2-8C gates
  @param[out] worker_ctx   Output worker context
  @return 0 on success, handler error code on failure
*/
int ha_innobase::pq_worker_scan_init(PQ_Worker_open_context *open_ctx,
                                     PQ_Worker_context **worker_ctx) {
  if (worker_ctx != nullptr) {
    *worker_ctx = nullptr;
  }

  if (open_ctx == nullptr || open_ctx->leader_ctx == nullptr ||
      open_ctx->worker_thd == nullptr || open_ctx->worker_table == nullptr ||
      open_ctx->worker_handler == nullptr ||
      open_ctx->worker_handler != this ||
      open_ctx->worker_table == open_ctx->leader_table ||
      open_ctx->actual_dop == 0) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (open_ctx->leader_ctx->kind() != PQ_Leader_context_kind::INNODB) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (m_prebuilt == nullptr || table != open_ctx->worker_table ||
      open_ctx->worker_table->in_use != open_ctx->worker_thd ||
      m_prebuilt->m_mysql_table != open_ctx->worker_table ||
      m_prebuilt->m_mysql_handler != this ||
      (open_ctx->leader_table != nullptr &&
       (m_prebuilt->m_mysql_table == open_ctx->leader_table ||
        m_prebuilt->m_mysql_handler == open_ctx->leader_table->file))) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (open_ctx->leader_table != nullptr &&
      open_ctx->leader_table->file != nullptr &&
      open_ctx->leader_table->file->pushed_idx_cond != nullptr) {
    auto *leader_handler = open_ctx->leader_table->file;
    if (pushed_idx_cond == nullptr ||
        pushed_idx_cond == leader_handler->pushed_idx_cond ||
        pushed_idx_cond_keyno != leader_handler->pushed_idx_cond_keyno) {
      return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
    }
  }

  auto sql_leader =
      static_cast<InnoDB_pq_sql_leader_context *>(open_ctx->leader_ctx);
  auto innodb_leader = sql_leader->innodb_ctx();
  if (innodb_leader == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (open_ctx->worker_table->s != nullptr &&
      open_ctx->worker_table->s->blob_fields > 0) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (open_ctx->worker_table->record[0] == nullptr ||
      (open_ctx->leader_table != nullptr &&
       open_ctx->worker_table->record[0] == open_ctx->leader_table->record[0])) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  auto innodb_worker = ut::new_withkey<InnoDB_pq_worker_ctx>(
      UT_NEW_THIS_FILE_PSI_KEY, open_ctx->worker_id, innodb_leader);
  if (innodb_worker == nullptr) {
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }
  auto assigned_range = innodb_leader->dispatch_next_range();
  if (assigned_range != nullptr) {
    innodb_worker->init(assigned_range);
    pq_global_stats.ranges_dispatched.fetch_add(1, std::memory_order_relaxed);
  } else {
    pq_global_stats.empty_worker_ranges.fetch_add(1,
                                                  std::memory_order_relaxed);
  }

  auto sql_worker = ut::new_withkey<InnoDB_pq_sql_worker_context>(
      UT_NEW_THIS_FILE_PSI_KEY, *sql_leader, innodb_worker, open_ctx);
  if (sql_worker == nullptr) {
    ut::delete_(innodb_worker);
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  m_pq_worker_ctxs.push_back(innodb_worker);
  if (worker_ctx != nullptr) {
    *worker_ctx = sql_worker;
  }
  return 0;
}

int ha_innobase::pq_worker_scan_init(uint keyno, void *scan_ctx) {
  if (scan_ctx == nullptr || m_prebuilt == nullptr || m_prebuilt->trx == nullptr) {
    return HA_ERR_UNSUPPORTED;
  }

  auto *pq_leader = static_cast<PQ_Leader_context *>(scan_ctx);
  active_index = keyno;
  const int result = change_active_index(active_index);
  if (result != 0) {
    return result;
  }

  auto *trx = m_prebuilt->trx;
  innobase_register_trx(ht, ha_thd(), trx);
  trx_start_if_not_started_xa(trx, false, UT_LOCATION_HERE);

  m_prebuilt->pq_worker = std::shared_ptr<Parallel_worker>(
      ut::new_withkey<Parallel_worker>(
          UT_NEW_THIS_FILE_PSI_KEY, pq_leader->is_reverse(),
          pq_leader->pq_slices_map, pq_leader->pq_key_map),
      [](Parallel_worker *worker) { ut::delete_(worker); });
  m_prebuilt->pq_ctx = nullptr;
  m_prebuilt->pq_ref_info = {};
  m_prebuilt->is_attach_ctx = false;

  update_thd();
  mrr_have_range = false;
  return 0;
}

/**
  Pull one row for a PQ worker via the callback-backed typed bridge.

  This keeps the latent row_search_mvcc() cursor path disabled. Rows are
  produced through the existing Parallel_reader callback producer and buffered
  in the worker typed context with a small memory cap.

  @param[in]   worker_ctx  Worker context (PQ_Worker_context*; unused in V2-8C gate)
  @param[out]  record       MySQL row buffer (table->record[0])
  @param[out]  eof          True when range is exhausted
  @return 0 on success, handler error code on failure
*/
int ha_innobase::pq_worker_scan_next(PQ_Worker_context *worker_ctx,
                                     uchar *record, bool *eof) {
  if (eof != nullptr) {
    *eof = false;
  }

  if (worker_ctx == nullptr || record == nullptr || eof == nullptr ||
      m_prebuilt == nullptr ||
      worker_ctx->kind() != PQ_Worker_context_kind::INNODB) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, eof);
  }

  auto sql_worker =
      static_cast<InnoDB_pq_sql_worker_context *>(worker_ctx);
  auto *open_ctx = sql_worker->open_ctx();
  auto innodb_worker = sql_worker->innodb_ctx();
  if (innodb_worker == nullptr || innodb_worker->leader_ctx() == nullptr ||
      innodb_worker->leader_ctx()->scan_ctx() == nullptr ||
      m_prebuilt->m_mysql_table == nullptr ||
      m_prebuilt->m_mysql_table->record[0] == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, eof);
  }

  if (open_ctx == nullptr || open_ctx->worker_handler != this ||
      open_ctx->worker_table == nullptr ||
      open_ctx->worker_table != m_prebuilt->m_mysql_table ||
      open_ctx->worker_table->record[0] != record ||
      open_ctx->worker_thd == nullptr ||
      open_ctx->worker_table->in_use != open_ctx->worker_thd ||
      open_ctx->worker_table->file != this) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, eof);
  }

  static constexpr size_t kMaxTypedPullBridgeBufferBytes = 8 * 1024 * 1024;
  size_t max_buffer_bytes = kMaxTypedPullBridgeBufferBytes;
  if (ha_thd() != nullptr) {
    const ulonglong session_limit = ha_thd()->variables.parallel_memory_limit;
    if (session_limit < max_buffer_bytes) {
      max_buffer_bytes = static_cast<size_t>(session_limit);
    }
  }

  for (;;) {
    pq_global_stats.worker_typed_pull_next_calls.fetch_add(
        1, std::memory_order_relaxed);
    auto err = innodb_worker->read_callback_record(record, m_prebuilt, eof,
                                                   max_buffer_bytes);
    if (err != DB_SUCCESS) {
      return pq_map_dberr_to_handler_error(err, eof);
    }
    if (!*eof) {
      pq_global_stats.worker_typed_pull_next_rows.fetch_add(
          1, std::memory_order_relaxed);
      return 0;
    }

    auto *range = innodb_worker->leader_ctx()->dispatch_next_range();
    if (range == nullptr) {
      pq_global_stats.worker_typed_pull_next_eofs.fetch_add(
          1, std::memory_order_relaxed);
      return 0;
    }

    innodb_worker->init(range);
    pq_global_stats.ranges_dispatched.fetch_add(1,
                                                std::memory_order_relaxed);
    *eof = false;
  }
}

int ha_innobase::pq_worker_scan_next(void *scan_ctx [[maybe_unused]],
                                     uchar *buf [[maybe_unused]]) {
  return HA_ERR_UNSUPPORTED;
}

int ha_innobase::pq_worker_scan_callback_smoke(PQ_Worker_context *worker_ctx,
                                               uchar *record,
                                               bool *converted) {
  if (converted != nullptr) {
    *converted = false;
  }

  if (worker_ctx == nullptr || record == nullptr || converted == nullptr ||
      m_prebuilt == nullptr ||
      worker_ctx->kind() != PQ_Worker_context_kind::INNODB) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  auto sql_worker =
      static_cast<InnoDB_pq_sql_worker_context *>(worker_ctx);
  auto innodb_worker = sql_worker->innodb_ctx();
  if (innodb_worker == nullptr || innodb_worker->leader_ctx() == nullptr ||
      innodb_worker->leader_ctx()->scan_ctx() == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  auto err = innodb_worker->leader_ctx()->scan_ctx()->smoke_callback_conversion(
      record, m_prebuilt, converted);
  if (err == DB_SUCCESS) {
    return 0;
  }
  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_worker_scan_callback_produce(
    PQ_Worker_context *worker_ctx, PQ_row_sink *row_sink) {
  if (worker_ctx == nullptr || row_sink == nullptr || m_prebuilt == nullptr ||
      worker_ctx->kind() != PQ_Worker_context_kind::INNODB) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  auto sql_worker =
      static_cast<InnoDB_pq_sql_worker_context *>(worker_ctx);
  auto innodb_worker = sql_worker->innodb_ctx();
  if (innodb_worker == nullptr || innodb_worker->leader_ctx() == nullptr ||
      innodb_worker->leader_ctx()->scan_ctx() == nullptr ||
      m_prebuilt->m_mysql_table == nullptr ||
      m_prebuilt->m_mysql_table->record[0] == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  auto *leader_ctx = innodb_worker->leader_ctx();
  auto *scan_ctx = leader_ctx->scan_ctx();
  auto *range = innodb_worker->assigned_range();

  while (range != nullptr) {
    auto err = scan_ctx->produce_callback_rows_for_range(
        m_prebuilt->m_mysql_table->record[0], m_prebuilt, row_sink, range);
    if (err != DB_SUCCESS) {
      return pq_map_dberr_to_handler_error(err, nullptr);
    }
    if (row_sink->should_abort()) {
      break;
    }

    range = leader_ctx->dispatch_next_range();
    if (range != nullptr) {
      innodb_worker->init(range);
      pq_global_stats.ranges_dispatched.fetch_add(1,
                                                  std::memory_order_relaxed);
    }
  }

  return 0;
}

int ha_innobase::pq_secondary_range_partition_smoke(
    THD *leader_thd, uint keyno, const key_range *start_key,
    const key_range *end_key, uint requested_dop, uint *ranges_built) {
  if (ranges_built != nullptr) {
    *ranges_built = 0;
  }

  if (leader_thd == nullptr || ranges_built == nullptr || requested_dop == 0 ||
      table == nullptr || table->s == nullptr || keyno >= table->s->keys ||
      m_prebuilt == nullptr || m_prebuilt->table == nullptr ||
      m_prebuilt->trx == nullptr || m_prebuilt->idx_cond) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (keyno == table->s->primary_key) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (start_key != nullptr && start_key->flag != HA_READ_KEY_OR_NEXT) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }
  if (end_key != nullptr && end_key->flag != HA_READ_BEFORE_KEY) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted()) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  const KEY *key = &table->key_info[keyno];
  mem_heap_t *heap = mem_heap_create(
      2 * (key->actual_key_parts * sizeof(dfield_t) + sizeof(dtuple_t)),
      UT_LOCATION_HERE);
  if (heap == nullptr) {
    cleanup_read_view();
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  dtuple_t *range_start = nullptr;
  dtuple_t *range_end = nullptr;
  dberr_t err = DB_SUCCESS;

  if (start_key != nullptr && start_key->keypart_map != 0) {
    range_start = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_start, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_start, m_prebuilt->srch_key_val1, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(start_key->key),
        static_cast<ulint>(start_key->length));
    if (range_start->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  if (err == DB_SUCCESS && end_key != nullptr && end_key->keypart_map != 0) {
    range_end = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_end, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_end, m_prebuilt->srch_key_val2, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(end_key->key),
        static_cast<ulint>(end_key->length));
    if (range_end->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  if (err == DB_SUCCESS) {
    const bool is_compact = dict_table_is_comp(index->table);
    page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
    InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
    err = scan_ctx.partition(0, range_start, range_end);
    if (err == DB_SUCCESS) {
      *ranges_built = static_cast<uint>(scan_ctx.ranges().size());
    }
  }
  mem_heap_free(heap);
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_primary_range_partition_smoke(
    THD *leader_thd, uint keyno, const key_range *start_key,
    const key_range *end_key, uint requested_dop, uint *ranges_built) {
  if (ranges_built != nullptr) {
    *ranges_built = 0;
  }

  if (leader_thd == nullptr || ranges_built == nullptr || requested_dop == 0 ||
      table == nullptr || table->s == nullptr || keyno >= table->s->keys ||
      m_prebuilt == nullptr || m_prebuilt->table == nullptr ||
      m_prebuilt->trx == nullptr || m_prebuilt->idx_cond) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (keyno != table->s->primary_key) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (start_key != nullptr && start_key->flag != HA_READ_KEY_OR_NEXT) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }
  if (end_key != nullptr && end_key->flag != HA_READ_BEFORE_KEY) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || !index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted()) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  const KEY *key = &table->key_info[keyno];
  mem_heap_t *heap = mem_heap_create(
      2 * (key->actual_key_parts * sizeof(dfield_t) + sizeof(dtuple_t)),
      UT_LOCATION_HERE);
  if (heap == nullptr) {
    cleanup_read_view();
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  dtuple_t *range_start = nullptr;
  dtuple_t *range_end = nullptr;
  dberr_t err = DB_SUCCESS;

  if (start_key != nullptr && start_key->keypart_map != 0) {
    range_start = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_start, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_start, m_prebuilt->srch_key_val1, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(start_key->key),
        static_cast<ulint>(start_key->length));
    if (range_start->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  if (err == DB_SUCCESS && end_key != nullptr && end_key->keypart_map != 0) {
    range_end = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_end, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_end, m_prebuilt->srch_key_val2, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(end_key->key),
        static_cast<ulint>(end_key->length));
    if (range_end->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  if (err == DB_SUCCESS) {
    const bool is_compact = dict_table_is_comp(index->table);
    page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
    InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
    err = scan_ctx.partition(0, range_start, range_end);
    if (err == DB_SUCCESS) {
      *ranges_built = static_cast<uint>(scan_ctx.ranges().size());
    }
  }

  mem_heap_free(heap);
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_primary_range_produce(
    THD *leader_thd, uint keyno, const key_range *start_key,
    const key_range *end_key, uint max_rows, PQ_row_sink *row_sink,
    uint *row_count) {
  if (row_count != nullptr) {
    *row_count = 0;
  }

  if (leader_thd == nullptr || row_sink == nullptr || row_count == nullptr ||
      max_rows == 0 || table == nullptr || table->s == nullptr ||
      keyno >= table->s->keys || m_prebuilt == nullptr ||
      m_prebuilt->m_mysql_table == nullptr ||
      m_prebuilt->m_mysql_table->record[0] == nullptr ||
      m_prebuilt->table == nullptr || m_prebuilt->table->is_intrinsic() ||
      m_prebuilt->trx == nullptr || m_prebuilt->idx_cond ||
      m_prebuilt->select_lock_type != LOCK_NONE ||
      m_prebuilt->pcur == nullptr || m_prebuilt->clust_pcur == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (keyno != table->s->primary_key) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (!pq_primary_range_flags_supported(start_key, end_key)) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || !index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted() ||
      dict_table_has_fts_index(index->table)) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  mem_heap_t *heap = mem_heap_create(2 * (sizeof(btr_pcur_t) + srv_page_size / 16),
                                     UT_LOCATION_HERE);
  if (heap == nullptr) {
    cleanup_read_view();
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }
  m_prebuilt->pq_heap = heap;

  const uint saved_active_index = active_index;
  Item *saved_pushed_idx_cond = pushed_idx_cond;
  const uint saved_pushed_idx_cond_keyno = pushed_idx_cond_keyno;
  dict_index_t *saved_index = m_prebuilt->index;
  const unsigned saved_read_just_key = m_prebuilt->read_just_key;
  const unsigned saved_template_type = m_prebuilt->template_type;
  const unsigned saved_n_template = m_prebuilt->n_template;
  const unsigned saved_null_bitmap_len = m_prebuilt->null_bitmap_len;
  const unsigned saved_need_to_access_clustered =
      m_prebuilt->need_to_access_clustered;
  const unsigned saved_templ_contains_blob = m_prebuilt->templ_contains_blob;
  const unsigned saved_templ_contains_fixed_point =
      m_prebuilt->templ_contains_fixed_point;
  const ulint saved_mysql_prefix_len = m_prebuilt->mysql_prefix_len;
  const bool saved_idx_cond = m_prebuilt->idx_cond;
  const ulint saved_idx_cond_n_cols = m_prebuilt->idx_cond_n_cols;
  const bool saved_keep_other_fields_on_keyread =
      m_prebuilt->keep_other_fields_on_keyread;
  const bool saved_in_fts_query = m_prebuilt->in_fts_query;
  const bool saved_m_end_range = m_prebuilt->m_end_range;
  mysql_row_templ_t *saved_mysql_template_ptr = m_prebuilt->mysql_template;
  std::vector<mysql_row_templ_t> saved_mysql_template;
  if (saved_mysql_template_ptr != nullptr && table->s->fields > 0) {
    saved_mysql_template.resize(table->s->fields);
    std::memcpy(saved_mysql_template.data(), saved_mysql_template_ptr,
                table->s->fields * sizeof(mysql_row_templ_t));
  }

  auto restore_prebuilt_template_state = [&]() {
    reset_template();
    if (saved_mysql_template_ptr == nullptr) {
      if (m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = nullptr;
    } else {
      if (m_prebuilt->mysql_template != saved_mysql_template_ptr &&
          m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = saved_mysql_template_ptr;
      if (!saved_mysql_template.empty()) {
        std::memcpy(m_prebuilt->mysql_template, saved_mysql_template.data(),
                    saved_mysql_template.size() * sizeof(mysql_row_templ_t));
      }
    }
    active_index = saved_active_index;
    pushed_idx_cond = saved_pushed_idx_cond;
    pushed_idx_cond_keyno = saved_pushed_idx_cond_keyno;
    m_prebuilt->index = saved_index;
    m_prebuilt->read_just_key = saved_read_just_key;
    m_prebuilt->template_type = saved_template_type;
    m_prebuilt->n_template = saved_n_template;
    m_prebuilt->null_bitmap_len = saved_null_bitmap_len;
    m_prebuilt->need_to_access_clustered = saved_need_to_access_clustered;
    m_prebuilt->templ_contains_blob = saved_templ_contains_blob;
    m_prebuilt->templ_contains_fixed_point =
        saved_templ_contains_fixed_point;
    m_prebuilt->mysql_prefix_len = saved_mysql_prefix_len;
    m_prebuilt->idx_cond = saved_idx_cond;
    m_prebuilt->idx_cond_n_cols = saved_idx_cond_n_cols;
    m_prebuilt->keep_other_fields_on_keyread =
        saved_keep_other_fields_on_keyread;
    m_prebuilt->in_fts_query = saved_in_fts_query;
    m_prebuilt->m_end_range = saved_m_end_range;
  };

  dtuple_t *range_start = nullptr;
  dtuple_t *range_end = nullptr;
  dberr_t err = DB_SUCCESS;
  bool empty_range = false;

  const int active_result = change_active_index(keyno);
  if (active_result != 0) {
    err = DB_UNSUPPORTED;
  }

  if (err == DB_SUCCESS) {
    err = pq_seek_primary_range_boundary(this, table, m_prebuilt, keyno, start_key,
                                         true, &range_start, &empty_range);
  }
  if (err == DB_SUCCESS && !empty_range) {
    err = pq_seek_primary_range_boundary(this, table, m_prebuilt, keyno, end_key,
                                         false, &range_end, &empty_range);
  }

  bool restored_prebuilt_template_state = false;

  if (err == DB_SUCCESS && !empty_range) {
    m_prebuilt->index = index;
    m_prebuilt->read_just_key = 0;
    build_template(false);

    const bool is_compact = dict_table_is_comp(index->table);
    page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
    InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
    err = scan_ctx.partition(0, range_start, range_end);

    PQ_counting_row_sink counting_sink(row_sink, max_rows);
    if (err == DB_SUCCESS) {
      for (const auto &range : scan_ctx.ranges()) {
        err = scan_ctx.produce_callback_rows_for_range(
            m_prebuilt->m_mysql_table->record[0], m_prebuilt, &counting_sink,
            &range);
        if (err != DB_SUCCESS || counting_sink.should_abort()) {
          break;
        }
      }
    }
    if (err == DB_INTERRUPTED && counting_sink.limit_hit()) {
      err = DB_UNSUPPORTED;
    }
    if (err == DB_SUCCESS && counting_sink.inner_abort()) {
      err = DB_INTERRUPTED;
    }
    if (err == DB_SUCCESS) {
      *row_count = counting_sink.rows();
    }

    restore_prebuilt_template_state();
    restored_prebuilt_template_state = true;
  } else if (err == DB_SUCCESS && empty_range) {
    *row_count = 0;
  }

  if (!restored_prebuilt_template_state) {
    restore_prebuilt_template_state();
  }

  m_prebuilt->pq_heap = nullptr;
  m_prebuilt->pq_tuple = nullptr;
  m_prebuilt->pq_index_read = false;

  if (err != DB_SUCCESS) {
    *row_count = 0;
  }

  mem_heap_free(heap);
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_secondary_visibility_smoke(THD *leader_thd, uint keyno) {
  if (leader_thd == nullptr || table == nullptr || table->s == nullptr ||
      keyno >= table->s->keys || keyno == table->s->primary_key ||
      m_prebuilt == nullptr || m_prebuilt->table == nullptr ||
      m_prebuilt->table->is_intrinsic() || m_prebuilt->trx == nullptr ||
      m_prebuilt->idx_cond) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted()) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  const bool is_compact = dict_table_is_comp(index->table);
  page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
  InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
  const dberr_t err = scan_ctx.validate_secondary_visibility_contract();
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_secondary_visibility_one_record_smoke(
    THD *leader_thd, uint keyno, const key_range *start_key,
    const key_range *end_key) {
  if (leader_thd == nullptr || table == nullptr || table->s == nullptr ||
      keyno >= table->s->keys || keyno == table->s->primary_key ||
      m_prebuilt == nullptr || m_prebuilt->table == nullptr ||
      m_prebuilt->table->is_intrinsic() || m_prebuilt->trx == nullptr ||
      m_prebuilt->idx_cond || m_prebuilt->pcur == nullptr ||
      m_prebuilt->clust_pcur == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (start_key != nullptr && start_key->flag != HA_READ_KEY_OR_NEXT) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }
  if (end_key != nullptr && end_key->flag != HA_READ_BEFORE_KEY) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted()) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  const KEY *key = &table->key_info[keyno];
  mem_heap_t *heap = mem_heap_create(
      2 * (key->actual_key_parts * sizeof(dfield_t) + sizeof(dtuple_t)),
      UT_LOCATION_HERE);
  if (heap == nullptr) {
    cleanup_read_view();
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  dtuple_t *range_start = nullptr;
  dtuple_t *range_end = nullptr;
  dberr_t err = DB_SUCCESS;

  if (start_key != nullptr && start_key->keypart_map != 0) {
    range_start = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_start, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_start, m_prebuilt->srch_key_val1, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(start_key->key),
        static_cast<ulint>(start_key->length));
    if (range_start->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  if (err == DB_SUCCESS && end_key != nullptr && end_key->keypart_map != 0) {
    range_end = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_end, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_end, m_prebuilt->srch_key_val2, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(end_key->key),
        static_cast<ulint>(end_key->length));
    if (range_end->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  dict_index_t *saved_index = m_prebuilt->index;
  if (err == DB_SUCCESS) {
    m_prebuilt->index = index;
    const bool is_compact = dict_table_is_comp(index->table);
    page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
    InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
    err = scan_ctx.validate_one_secondary_record_for_smoke(
        m_prebuilt, range_start, range_end);
    m_prebuilt->index = saved_index;
  }

  mem_heap_free(heap);
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_secondary_covering_one_row_smoke(
    THD *leader_thd, uint keyno, const key_range *start_key,
    const key_range *end_key) {
  if (leader_thd == nullptr || table == nullptr || table->s == nullptr ||
      keyno >= table->s->keys || keyno == table->s->primary_key ||
      m_prebuilt == nullptr || m_prebuilt->m_mysql_table == nullptr ||
      m_prebuilt->m_mysql_table->record[0] == nullptr ||
      m_prebuilt->table == nullptr || m_prebuilt->table->is_intrinsic() ||
      m_prebuilt->trx == nullptr || m_prebuilt->idx_cond ||
      m_prebuilt->pcur == nullptr || m_prebuilt->clust_pcur == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (start_key != nullptr && start_key->flag != HA_READ_KEY_OR_NEXT) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }
  if (end_key != nullptr && end_key->flag != HA_READ_BEFORE_KEY) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted()) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  const KEY *key = &table->key_info[keyno];
  mem_heap_t *heap = mem_heap_create(
      2 * (key->actual_key_parts * sizeof(dfield_t) + sizeof(dtuple_t)),
      UT_LOCATION_HERE);
  if (heap == nullptr) {
    cleanup_read_view();
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  dtuple_t *range_start = nullptr;
  dtuple_t *range_end = nullptr;
  dberr_t err = DB_SUCCESS;

  if (start_key != nullptr && start_key->keypart_map != 0) {
    range_start = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_start, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_start, m_prebuilt->srch_key_val1, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(start_key->key),
        static_cast<ulint>(start_key->length));
    if (range_start->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  if (err == DB_SUCCESS && end_key != nullptr && end_key->keypart_map != 0) {
    range_end = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_end, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_end, m_prebuilt->srch_key_val2, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(end_key->key),
        static_cast<ulint>(end_key->length));
    if (range_end->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  dict_index_t *saved_index = m_prebuilt->index;
  const unsigned saved_read_just_key = m_prebuilt->read_just_key;
  const unsigned saved_template_type = m_prebuilt->template_type;
  const unsigned saved_n_template = m_prebuilt->n_template;
  const unsigned saved_null_bitmap_len = m_prebuilt->null_bitmap_len;
  const unsigned saved_need_to_access_clustered =
      m_prebuilt->need_to_access_clustered;
  const unsigned saved_templ_contains_blob = m_prebuilt->templ_contains_blob;
  const unsigned saved_templ_contains_fixed_point =
      m_prebuilt->templ_contains_fixed_point;
  const ulint saved_mysql_prefix_len = m_prebuilt->mysql_prefix_len;
  const ulint saved_idx_cond_n_cols = m_prebuilt->idx_cond_n_cols;
  const bool saved_keep_other_fields_on_keyread =
      m_prebuilt->keep_other_fields_on_keyread;
  const bool saved_in_fts_query = m_prebuilt->in_fts_query;
  const bool saved_m_end_range = m_prebuilt->m_end_range;
  mysql_row_templ_t *saved_mysql_template_ptr = m_prebuilt->mysql_template;
  std::vector<mysql_row_templ_t> saved_mysql_template;
  if (saved_mysql_template_ptr != nullptr && table->s->fields > 0) {
    saved_mysql_template.resize(table->s->fields);
    std::memcpy(saved_mysql_template.data(), saved_mysql_template_ptr,
                table->s->fields * sizeof(mysql_row_templ_t));
  }

  auto restore_prebuilt_template_state = [&]() {
    reset_template();
    if (saved_mysql_template_ptr == nullptr) {
      if (m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = nullptr;
    } else {
      if (m_prebuilt->mysql_template != saved_mysql_template_ptr &&
          m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = saved_mysql_template_ptr;
      if (!saved_mysql_template.empty()) {
        std::memcpy(m_prebuilt->mysql_template, saved_mysql_template.data(),
                    saved_mysql_template.size() * sizeof(mysql_row_templ_t));
      }
    }
    m_prebuilt->index = saved_index;
    m_prebuilt->read_just_key = saved_read_just_key;
    m_prebuilt->template_type = saved_template_type;
    m_prebuilt->n_template = saved_n_template;
    m_prebuilt->null_bitmap_len = saved_null_bitmap_len;
    m_prebuilt->need_to_access_clustered = saved_need_to_access_clustered;
    m_prebuilt->templ_contains_blob = saved_templ_contains_blob;
    m_prebuilt->templ_contains_fixed_point =
        saved_templ_contains_fixed_point;
    m_prebuilt->mysql_prefix_len = saved_mysql_prefix_len;
    m_prebuilt->idx_cond_n_cols = saved_idx_cond_n_cols;
    m_prebuilt->keep_other_fields_on_keyread =
        saved_keep_other_fields_on_keyread;
    m_prebuilt->in_fts_query = saved_in_fts_query;
    m_prebuilt->m_end_range = saved_m_end_range;
  };

  if (err == DB_SUCCESS) {
    m_prebuilt->index = index;
    m_prebuilt->read_just_key = 1;
    build_template(false);
    const bool is_compact = dict_table_is_comp(index->table);
    page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
    InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
    err = scan_ctx.materialize_one_secondary_record_for_smoke(
        m_prebuilt->m_mysql_table->record[0], m_prebuilt, range_start,
        range_end);
    restore_prebuilt_template_state();
  }

  mem_heap_free(heap);
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_secondary_noncovering_icp_one_row_smoke(
    THD *leader_thd, uint keyno, const key_range *start_key,
    const key_range *end_key, bool *materialized) {
  if (materialized != nullptr) {
    *materialized = false;
  }

  if (leader_thd == nullptr || materialized == nullptr || table == nullptr ||
      table->s == nullptr || keyno >= table->s->keys ||
      keyno == table->s->primary_key || pushed_idx_cond == nullptr ||
      pushed_idx_cond_keyno != keyno || m_prebuilt == nullptr ||
      m_prebuilt->m_mysql_table == nullptr ||
      m_prebuilt->m_mysql_table->record[0] == nullptr ||
      m_prebuilt->table == nullptr || m_prebuilt->table->is_intrinsic() ||
      m_prebuilt->trx == nullptr || m_prebuilt->pcur == nullptr ||
      m_prebuilt->clust_pcur == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (start_key != nullptr && start_key->flag != HA_READ_KEY_OR_NEXT) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }
  if (end_key != nullptr && end_key->flag != HA_READ_BEFORE_KEY) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted()) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  const KEY *key = &table->key_info[keyno];
  mem_heap_t *heap = mem_heap_create(
      2 * (key->actual_key_parts * sizeof(dfield_t) + sizeof(dtuple_t)),
      UT_LOCATION_HERE);
  if (heap == nullptr) {
    cleanup_read_view();
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  dtuple_t *range_start = nullptr;
  dtuple_t *range_end = nullptr;
  dberr_t err = DB_SUCCESS;

  if (start_key != nullptr && start_key->keypart_map != 0) {
    range_start = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_start, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_start, m_prebuilt->srch_key_val1, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(start_key->key),
        static_cast<ulint>(start_key->length));
    if (range_start->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  if (err == DB_SUCCESS && end_key != nullptr && end_key->keypart_map != 0) {
    range_end = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_end, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_end, m_prebuilt->srch_key_val2, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(end_key->key),
        static_cast<ulint>(end_key->length));
    if (range_end->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  const uint saved_active_index = active_index;
  Item *saved_pushed_idx_cond = pushed_idx_cond;
  const uint saved_pushed_idx_cond_keyno = pushed_idx_cond_keyno;
  dict_index_t *saved_index = m_prebuilt->index;
  const unsigned saved_read_just_key = m_prebuilt->read_just_key;
  const unsigned saved_template_type = m_prebuilt->template_type;
  const unsigned saved_n_template = m_prebuilt->n_template;
  const unsigned saved_null_bitmap_len = m_prebuilt->null_bitmap_len;
  const unsigned saved_need_to_access_clustered =
      m_prebuilt->need_to_access_clustered;
  const unsigned saved_templ_contains_blob = m_prebuilt->templ_contains_blob;
  const unsigned saved_templ_contains_fixed_point =
      m_prebuilt->templ_contains_fixed_point;
  const ulint saved_mysql_prefix_len = m_prebuilt->mysql_prefix_len;
  const bool saved_idx_cond = m_prebuilt->idx_cond;
  const ulint saved_idx_cond_n_cols = m_prebuilt->idx_cond_n_cols;
  const bool saved_keep_other_fields_on_keyread =
      m_prebuilt->keep_other_fields_on_keyread;
  const bool saved_in_fts_query = m_prebuilt->in_fts_query;
  const bool saved_m_end_range = m_prebuilt->m_end_range;
  mysql_row_templ_t *saved_mysql_template_ptr = m_prebuilt->mysql_template;
  std::vector<mysql_row_templ_t> saved_mysql_template;
  if (saved_mysql_template_ptr != nullptr && table->s->fields > 0) {
    saved_mysql_template.resize(table->s->fields);
    std::memcpy(saved_mysql_template.data(), saved_mysql_template_ptr,
                table->s->fields * sizeof(mysql_row_templ_t));
  }

  auto restore_prebuilt_template_state = [&]() {
    reset_template();
    if (saved_mysql_template_ptr == nullptr) {
      if (m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = nullptr;
    } else {
      if (m_prebuilt->mysql_template != saved_mysql_template_ptr &&
          m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = saved_mysql_template_ptr;
      if (!saved_mysql_template.empty()) {
        std::memcpy(m_prebuilt->mysql_template, saved_mysql_template.data(),
                    saved_mysql_template.size() * sizeof(mysql_row_templ_t));
      }
    }
    active_index = saved_active_index;
    pushed_idx_cond = saved_pushed_idx_cond;
    pushed_idx_cond_keyno = saved_pushed_idx_cond_keyno;
    m_prebuilt->index = saved_index;
    m_prebuilt->read_just_key = saved_read_just_key;
    m_prebuilt->template_type = saved_template_type;
    m_prebuilt->n_template = saved_n_template;
    m_prebuilt->null_bitmap_len = saved_null_bitmap_len;
    m_prebuilt->need_to_access_clustered = saved_need_to_access_clustered;
    m_prebuilt->templ_contains_blob = saved_templ_contains_blob;
    m_prebuilt->templ_contains_fixed_point =
        saved_templ_contains_fixed_point;
    m_prebuilt->mysql_prefix_len = saved_mysql_prefix_len;
    m_prebuilt->idx_cond = saved_idx_cond;
    m_prebuilt->idx_cond_n_cols = saved_idx_cond_n_cols;
    m_prebuilt->keep_other_fields_on_keyread =
        saved_keep_other_fields_on_keyread;
    m_prebuilt->in_fts_query = saved_in_fts_query;
    m_prebuilt->m_end_range = saved_m_end_range;
  };

  if (err == DB_SUCCESS) {
    active_index = keyno;
    pushed_idx_cond = saved_pushed_idx_cond;
    pushed_idx_cond_keyno = keyno;
    m_prebuilt->index = index;
    m_prebuilt->read_just_key = 0;
    build_template(false);
    const bool is_compact = dict_table_is_comp(index->table);
    page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
    InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
    err = scan_ctx.materialize_one_secondary_icp_record_for_smoke(
        m_prebuilt->m_mysql_table->record[0], m_prebuilt, range_start,
        range_end, materialized);
    restore_prebuilt_template_state();
  }

  mem_heap_free(heap);
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_secondary_covering_range_smoke(
    THD *leader_thd, uint keyno, const key_range *start_key,
    const key_range *end_key, uint *row_count) {
  if (row_count != nullptr) {
    *row_count = 0;
  }

  if (leader_thd == nullptr || row_count == nullptr || table == nullptr ||
      table->s == nullptr || keyno >= table->s->keys ||
      keyno == table->s->primary_key || m_prebuilt == nullptr ||
      m_prebuilt->m_mysql_table == nullptr ||
      m_prebuilt->m_mysql_table->record[0] == nullptr ||
      m_prebuilt->table == nullptr || m_prebuilt->table->is_intrinsic() ||
      m_prebuilt->trx == nullptr || m_prebuilt->idx_cond ||
      m_prebuilt->pcur == nullptr || m_prebuilt->clust_pcur == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (start_key != nullptr && start_key->flag != HA_READ_KEY_OR_NEXT) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }
  if (end_key != nullptr && end_key->flag != HA_READ_BEFORE_KEY) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted()) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  const KEY *key = &table->key_info[keyno];
  mem_heap_t *heap = mem_heap_create(
      2 * (key->actual_key_parts * sizeof(dfield_t) + sizeof(dtuple_t)),
      UT_LOCATION_HERE);
  if (heap == nullptr) {
    cleanup_read_view();
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  dtuple_t *range_start = nullptr;
  dtuple_t *range_end = nullptr;
  dberr_t err = DB_SUCCESS;

  if (start_key != nullptr && start_key->keypart_map != 0) {
    range_start = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_start, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_start, m_prebuilt->srch_key_val1, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(start_key->key),
        static_cast<ulint>(start_key->length));
    if (range_start->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  if (err == DB_SUCCESS && end_key != nullptr && end_key->keypart_map != 0) {
    range_end = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_end, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_end, m_prebuilt->srch_key_val2, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(end_key->key),
        static_cast<ulint>(end_key->length));
    if (range_end->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  dict_index_t *saved_index = m_prebuilt->index;
  const unsigned saved_read_just_key = m_prebuilt->read_just_key;
  const unsigned saved_template_type = m_prebuilt->template_type;
  const unsigned saved_n_template = m_prebuilt->n_template;
  const unsigned saved_null_bitmap_len = m_prebuilt->null_bitmap_len;
  const unsigned saved_need_to_access_clustered =
      m_prebuilt->need_to_access_clustered;
  const unsigned saved_templ_contains_blob = m_prebuilt->templ_contains_blob;
  const unsigned saved_templ_contains_fixed_point =
      m_prebuilt->templ_contains_fixed_point;
  const ulint saved_mysql_prefix_len = m_prebuilt->mysql_prefix_len;
  const ulint saved_idx_cond_n_cols = m_prebuilt->idx_cond_n_cols;
  const bool saved_keep_other_fields_on_keyread =
      m_prebuilt->keep_other_fields_on_keyread;
  const bool saved_in_fts_query = m_prebuilt->in_fts_query;
  const bool saved_m_end_range = m_prebuilt->m_end_range;
  mysql_row_templ_t *saved_mysql_template_ptr = m_prebuilt->mysql_template;
  std::vector<mysql_row_templ_t> saved_mysql_template;
  if (saved_mysql_template_ptr != nullptr && table->s->fields > 0) {
    saved_mysql_template.resize(table->s->fields);
    std::memcpy(saved_mysql_template.data(), saved_mysql_template_ptr,
                table->s->fields * sizeof(mysql_row_templ_t));
  }

  auto restore_prebuilt_template_state = [&]() {
    reset_template();
    if (saved_mysql_template_ptr == nullptr) {
      if (m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = nullptr;
    } else {
      if (m_prebuilt->mysql_template != saved_mysql_template_ptr &&
          m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = saved_mysql_template_ptr;
      if (!saved_mysql_template.empty()) {
        std::memcpy(m_prebuilt->mysql_template, saved_mysql_template.data(),
                    saved_mysql_template.size() * sizeof(mysql_row_templ_t));
      }
    }
    m_prebuilt->index = saved_index;
    m_prebuilt->read_just_key = saved_read_just_key;
    m_prebuilt->template_type = saved_template_type;
    m_prebuilt->n_template = saved_n_template;
    m_prebuilt->null_bitmap_len = saved_null_bitmap_len;
    m_prebuilt->need_to_access_clustered = saved_need_to_access_clustered;
    m_prebuilt->templ_contains_blob = saved_templ_contains_blob;
    m_prebuilt->templ_contains_fixed_point =
        saved_templ_contains_fixed_point;
    m_prebuilt->mysql_prefix_len = saved_mysql_prefix_len;
    m_prebuilt->idx_cond_n_cols = saved_idx_cond_n_cols;
    m_prebuilt->keep_other_fields_on_keyread =
        saved_keep_other_fields_on_keyread;
    m_prebuilt->in_fts_query = saved_in_fts_query;
    m_prebuilt->m_end_range = saved_m_end_range;
  };

  if (err == DB_SUCCESS) {
    constexpr uint kMaxSmokeRows = 64;
    m_prebuilt->index = index;
    m_prebuilt->read_just_key = 1;
    build_template(false);
    const bool is_compact = dict_table_is_comp(index->table);
    page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
    InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
    err = scan_ctx.materialize_secondary_range_for_smoke(
        m_prebuilt->m_mysql_table->record[0], m_prebuilt, range_start,
        range_end, kMaxSmokeRows, row_count);
    restore_prebuilt_template_state();
  }

  if (err != DB_SUCCESS) {
    *row_count = 0;
  }

  mem_heap_free(heap);
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_secondary_covering_ref_smoke(
    THD *leader_thd, uint keyno, const key_range *ref_key, uint *row_count) {
  if (row_count != nullptr) {
    *row_count = 0;
  }

  if (leader_thd == nullptr || row_count == nullptr || ref_key == nullptr ||
      ref_key->keypart_map == 0 || ref_key->key == nullptr ||
      table == nullptr || table->s == nullptr || keyno >= table->s->keys ||
      keyno == table->s->primary_key || m_prebuilt == nullptr ||
      m_prebuilt->m_mysql_table == nullptr ||
      m_prebuilt->m_mysql_table->record[0] == nullptr ||
      m_prebuilt->table == nullptr || m_prebuilt->table->is_intrinsic() ||
      m_prebuilt->trx == nullptr || m_prebuilt->idx_cond ||
      m_prebuilt->pcur == nullptr || m_prebuilt->clust_pcur == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (ref_key->flag != HA_READ_KEY_EXACT) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted()) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  const KEY *key = &table->key_info[keyno];
  mem_heap_t *heap = mem_heap_create(
      key->actual_key_parts * sizeof(dfield_t) + sizeof(dtuple_t),
      UT_LOCATION_HERE);
  if (heap == nullptr) {
    cleanup_read_view();
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  dtuple_t *ref_tuple = dtuple_create(heap, key->actual_key_parts);
  dict_index_copy_types(ref_tuple, index, key->actual_key_parts);
  row_sel_convert_mysql_key_to_innobase(
      ref_tuple, m_prebuilt->srch_key_val1, m_prebuilt->srch_key_val_len,
      index, reinterpret_cast<const byte *>(ref_key->key),
      static_cast<ulint>(ref_key->length));
  dberr_t err = ref_tuple->n_fields == 0 ? DB_UNSUPPORTED : DB_SUCCESS;

  dict_index_t *saved_index = m_prebuilt->index;
  const unsigned saved_read_just_key = m_prebuilt->read_just_key;
  const unsigned saved_template_type = m_prebuilt->template_type;
  const unsigned saved_n_template = m_prebuilt->n_template;
  const unsigned saved_null_bitmap_len = m_prebuilt->null_bitmap_len;
  const unsigned saved_need_to_access_clustered =
      m_prebuilt->need_to_access_clustered;
  const unsigned saved_templ_contains_blob = m_prebuilt->templ_contains_blob;
  const unsigned saved_templ_contains_fixed_point =
      m_prebuilt->templ_contains_fixed_point;
  const ulint saved_mysql_prefix_len = m_prebuilt->mysql_prefix_len;
  const ulint saved_idx_cond_n_cols = m_prebuilt->idx_cond_n_cols;
  const bool saved_keep_other_fields_on_keyread =
      m_prebuilt->keep_other_fields_on_keyread;
  const bool saved_in_fts_query = m_prebuilt->in_fts_query;
  const bool saved_m_end_range = m_prebuilt->m_end_range;
  mysql_row_templ_t *saved_mysql_template_ptr = m_prebuilt->mysql_template;
  std::vector<mysql_row_templ_t> saved_mysql_template;
  if (saved_mysql_template_ptr != nullptr && table->s->fields > 0) {
    saved_mysql_template.resize(table->s->fields);
    std::memcpy(saved_mysql_template.data(), saved_mysql_template_ptr,
                table->s->fields * sizeof(mysql_row_templ_t));
  }

  auto restore_prebuilt_template_state = [&]() {
    reset_template();
    if (saved_mysql_template_ptr == nullptr) {
      if (m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = nullptr;
    } else {
      if (m_prebuilt->mysql_template != saved_mysql_template_ptr &&
          m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = saved_mysql_template_ptr;
      if (!saved_mysql_template.empty()) {
        std::memcpy(m_prebuilt->mysql_template, saved_mysql_template.data(),
                    saved_mysql_template.size() * sizeof(mysql_row_templ_t));
      }
    }
    m_prebuilt->index = saved_index;
    m_prebuilt->read_just_key = saved_read_just_key;
    m_prebuilt->template_type = saved_template_type;
    m_prebuilt->n_template = saved_n_template;
    m_prebuilt->null_bitmap_len = saved_null_bitmap_len;
    m_prebuilt->need_to_access_clustered = saved_need_to_access_clustered;
    m_prebuilt->templ_contains_blob = saved_templ_contains_blob;
    m_prebuilt->templ_contains_fixed_point =
        saved_templ_contains_fixed_point;
    m_prebuilt->mysql_prefix_len = saved_mysql_prefix_len;
    m_prebuilt->idx_cond_n_cols = saved_idx_cond_n_cols;
    m_prebuilt->keep_other_fields_on_keyread =
        saved_keep_other_fields_on_keyread;
    m_prebuilt->in_fts_query = saved_in_fts_query;
    m_prebuilt->m_end_range = saved_m_end_range;
  };

  if (err == DB_SUCCESS) {
    constexpr uint kMaxSmokeRows = 64;
    m_prebuilt->index = index;
    m_prebuilt->read_just_key = 1;
    build_template(false);
    const bool is_compact = dict_table_is_comp(index->table);
    page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
    InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
    err = scan_ctx.materialize_secondary_ref_for_smoke(
        m_prebuilt->m_mysql_table->record[0], m_prebuilt, ref_tuple,
        kMaxSmokeRows, row_count);
    restore_prebuilt_template_state();
  }

  if (err != DB_SUCCESS) {
    *row_count = 0;
  }

  mem_heap_free(heap);
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_secondary_covering_ref_produce(
    THD *leader_thd, uint keyno, const key_range *ref_key,
    PQ_row_sink *row_sink, uint *row_count) {
  if (row_count != nullptr) {
    *row_count = 0;
  }

  if (leader_thd == nullptr || row_count == nullptr || row_sink == nullptr ||
      ref_key == nullptr || ref_key->keypart_map == 0 ||
      ref_key->key == nullptr || table == nullptr || table->s == nullptr ||
      keyno >= table->s->keys || keyno == table->s->primary_key ||
      m_prebuilt == nullptr || m_prebuilt->m_mysql_table == nullptr ||
      m_prebuilt->m_mysql_table->record[0] == nullptr ||
      m_prebuilt->table == nullptr || m_prebuilt->table->is_intrinsic() ||
      m_prebuilt->trx == nullptr || m_prebuilt->idx_cond ||
      m_prebuilt->pcur == nullptr || m_prebuilt->clust_pcur == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (ref_key->flag != HA_READ_KEY_EXACT) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted()) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  const KEY *key = &table->key_info[keyno];
  mem_heap_t *heap = mem_heap_create(
      key->actual_key_parts * sizeof(dfield_t) + sizeof(dtuple_t),
      UT_LOCATION_HERE);
  if (heap == nullptr) {
    cleanup_read_view();
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  dtuple_t *ref_tuple = dtuple_create(heap, key->actual_key_parts);
  dict_index_copy_types(ref_tuple, index, key->actual_key_parts);
  row_sel_convert_mysql_key_to_innobase(
      ref_tuple, m_prebuilt->srch_key_val1, m_prebuilt->srch_key_val_len,
      index, reinterpret_cast<const byte *>(ref_key->key),
      static_cast<ulint>(ref_key->length));
  dberr_t err = ref_tuple->n_fields == 0 ? DB_UNSUPPORTED : DB_SUCCESS;

  dict_index_t *saved_index = m_prebuilt->index;
  const unsigned saved_read_just_key = m_prebuilt->read_just_key;
  const unsigned saved_template_type = m_prebuilt->template_type;
  const unsigned saved_n_template = m_prebuilt->n_template;
  const unsigned saved_null_bitmap_len = m_prebuilt->null_bitmap_len;
  const unsigned saved_need_to_access_clustered =
      m_prebuilt->need_to_access_clustered;
  const unsigned saved_templ_contains_blob = m_prebuilt->templ_contains_blob;
  const unsigned saved_templ_contains_fixed_point =
      m_prebuilt->templ_contains_fixed_point;
  const ulint saved_mysql_prefix_len = m_prebuilt->mysql_prefix_len;
  const ulint saved_idx_cond_n_cols = m_prebuilt->idx_cond_n_cols;
  const bool saved_keep_other_fields_on_keyread =
      m_prebuilt->keep_other_fields_on_keyread;
  const bool saved_in_fts_query = m_prebuilt->in_fts_query;
  const bool saved_m_end_range = m_prebuilt->m_end_range;
  mysql_row_templ_t *saved_mysql_template_ptr = m_prebuilt->mysql_template;
  std::vector<mysql_row_templ_t> saved_mysql_template;
  if (saved_mysql_template_ptr != nullptr && table->s->fields > 0) {
    saved_mysql_template.resize(table->s->fields);
    std::memcpy(saved_mysql_template.data(), saved_mysql_template_ptr,
                table->s->fields * sizeof(mysql_row_templ_t));
  }

  auto restore_prebuilt_template_state = [&]() {
    reset_template();
    if (saved_mysql_template_ptr == nullptr) {
      if (m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = nullptr;
    } else {
      if (m_prebuilt->mysql_template != saved_mysql_template_ptr &&
          m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = saved_mysql_template_ptr;
      if (!saved_mysql_template.empty()) {
        std::memcpy(m_prebuilt->mysql_template, saved_mysql_template.data(),
                    saved_mysql_template.size() * sizeof(mysql_row_templ_t));
      }
    }
    m_prebuilt->index = saved_index;
    m_prebuilt->read_just_key = saved_read_just_key;
    m_prebuilt->template_type = saved_template_type;
    m_prebuilt->n_template = saved_n_template;
    m_prebuilt->null_bitmap_len = saved_null_bitmap_len;
    m_prebuilt->need_to_access_clustered = saved_need_to_access_clustered;
    m_prebuilt->templ_contains_blob = saved_templ_contains_blob;
    m_prebuilt->templ_contains_fixed_point =
        saved_templ_contains_fixed_point;
    m_prebuilt->mysql_prefix_len = saved_mysql_prefix_len;
    m_prebuilt->idx_cond_n_cols = saved_idx_cond_n_cols;
    m_prebuilt->keep_other_fields_on_keyread =
        saved_keep_other_fields_on_keyread;
    m_prebuilt->in_fts_query = saved_in_fts_query;
    m_prebuilt->m_end_range = saved_m_end_range;
  };

  if (err == DB_SUCCESS) {
    constexpr uint kMaxRows = 64;
    m_prebuilt->index = index;
    m_prebuilt->read_just_key = 1;
    build_template(false);
    const bool is_compact = dict_table_is_comp(index->table);
    page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
    InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
    err = scan_ctx.produce_secondary_ref_for_user_gate(
        m_prebuilt->m_mysql_table->record[0], m_prebuilt, ref_tuple, kMaxRows,
        row_sink, row_count);
    restore_prebuilt_template_state();
  }

  if (err != DB_SUCCESS) {
    *row_count = 0;
  }

  mem_heap_free(heap);
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_secondary_covering_range_produce(
    THD *leader_thd, uint keyno, const key_range *start_key,
    const key_range *end_key, PQ_row_sink *row_sink, uint *row_count) {
  if (row_count != nullptr) {
    *row_count = 0;
  }

  if (leader_thd == nullptr || row_count == nullptr || row_sink == nullptr ||
      table == nullptr || table->s == nullptr || keyno >= table->s->keys ||
      keyno == table->s->primary_key || m_prebuilt == nullptr ||
      m_prebuilt->m_mysql_table == nullptr ||
      m_prebuilt->m_mysql_table->record[0] == nullptr ||
      m_prebuilt->table == nullptr || m_prebuilt->table->is_intrinsic() ||
      m_prebuilt->trx == nullptr || m_prebuilt->idx_cond ||
      m_prebuilt->pcur == nullptr || m_prebuilt->clust_pcur == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (start_key != nullptr && start_key->flag != HA_READ_KEY_OR_NEXT) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }
  if (end_key != nullptr && end_key->flag != HA_READ_BEFORE_KEY) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted()) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  const KEY *key = &table->key_info[keyno];
  mem_heap_t *heap = mem_heap_create(
      2 * (key->actual_key_parts * sizeof(dfield_t) + sizeof(dtuple_t)),
      UT_LOCATION_HERE);
  if (heap == nullptr) {
    cleanup_read_view();
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  dtuple_t *range_start = nullptr;
  dtuple_t *range_end = nullptr;
  dberr_t err = DB_SUCCESS;

  if (start_key != nullptr && start_key->keypart_map != 0) {
    range_start = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_start, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_start, m_prebuilt->srch_key_val1, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(start_key->key),
        static_cast<ulint>(start_key->length));
    if (range_start->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  if (err == DB_SUCCESS && end_key != nullptr && end_key->keypart_map != 0) {
    range_end = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_end, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_end, m_prebuilt->srch_key_val2, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(end_key->key),
        static_cast<ulint>(end_key->length));
    if (range_end->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  dict_index_t *saved_index = m_prebuilt->index;
  const unsigned saved_read_just_key = m_prebuilt->read_just_key;
  const unsigned saved_template_type = m_prebuilt->template_type;
  const unsigned saved_n_template = m_prebuilt->n_template;
  const unsigned saved_null_bitmap_len = m_prebuilt->null_bitmap_len;
  const unsigned saved_need_to_access_clustered =
      m_prebuilt->need_to_access_clustered;
  const unsigned saved_templ_contains_blob = m_prebuilt->templ_contains_blob;
  const unsigned saved_templ_contains_fixed_point =
      m_prebuilt->templ_contains_fixed_point;
  const ulint saved_mysql_prefix_len = m_prebuilt->mysql_prefix_len;
  const ulint saved_idx_cond_n_cols = m_prebuilt->idx_cond_n_cols;
  const bool saved_keep_other_fields_on_keyread =
      m_prebuilt->keep_other_fields_on_keyread;
  const bool saved_in_fts_query = m_prebuilt->in_fts_query;
  const bool saved_m_end_range = m_prebuilt->m_end_range;
  mysql_row_templ_t *saved_mysql_template_ptr = m_prebuilt->mysql_template;
  std::vector<mysql_row_templ_t> saved_mysql_template;
  if (saved_mysql_template_ptr != nullptr && table->s->fields > 0) {
    saved_mysql_template.resize(table->s->fields);
    std::memcpy(saved_mysql_template.data(), saved_mysql_template_ptr,
                table->s->fields * sizeof(mysql_row_templ_t));
  }

  auto restore_prebuilt_template_state = [&]() {
    reset_template();
    if (saved_mysql_template_ptr == nullptr) {
      if (m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = nullptr;
    } else {
      if (m_prebuilt->mysql_template != saved_mysql_template_ptr &&
          m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = saved_mysql_template_ptr;
      if (!saved_mysql_template.empty()) {
        std::memcpy(m_prebuilt->mysql_template, saved_mysql_template.data(),
                    saved_mysql_template.size() * sizeof(mysql_row_templ_t));
      }
    }
    m_prebuilt->index = saved_index;
    m_prebuilt->read_just_key = saved_read_just_key;
    m_prebuilt->template_type = saved_template_type;
    m_prebuilt->n_template = saved_n_template;
    m_prebuilt->null_bitmap_len = saved_null_bitmap_len;
    m_prebuilt->need_to_access_clustered = saved_need_to_access_clustered;
    m_prebuilt->templ_contains_blob = saved_templ_contains_blob;
    m_prebuilt->templ_contains_fixed_point =
        saved_templ_contains_fixed_point;
    m_prebuilt->mysql_prefix_len = saved_mysql_prefix_len;
    m_prebuilt->idx_cond_n_cols = saved_idx_cond_n_cols;
    m_prebuilt->keep_other_fields_on_keyread =
        saved_keep_other_fields_on_keyread;
    m_prebuilt->in_fts_query = saved_in_fts_query;
    m_prebuilt->m_end_range = saved_m_end_range;
  };

  if (err == DB_SUCCESS) {
    constexpr uint kMaxRows = 64;
    m_prebuilt->index = index;
    m_prebuilt->read_just_key = 1;
    build_template(false);
    const bool is_compact = dict_table_is_comp(index->table);
    page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
    InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
    err = scan_ctx.produce_secondary_range_for_user_gate(
        m_prebuilt->m_mysql_table->record[0], m_prebuilt, range_start,
        range_end, kMaxRows, row_sink, row_count);
    restore_prebuilt_template_state();
  }

  if (err != DB_SUCCESS) {
    *row_count = 0;
  }

  mem_heap_free(heap);
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

int ha_innobase::pq_secondary_noncovering_icp_range_produce(
    THD *leader_thd, uint keyno, const key_range *start_key,
    const key_range *end_key, PQ_row_sink *row_sink, uint *row_count) {
  if (row_count != nullptr) {
    *row_count = 0;
  }

  if (leader_thd == nullptr || row_count == nullptr || row_sink == nullptr ||
      table == nullptr || table->s == nullptr || keyno >= table->s->keys ||
      keyno == table->s->primary_key || pushed_idx_cond == nullptr ||
      pushed_idx_cond_keyno != keyno || m_prebuilt == nullptr ||
      m_prebuilt->m_mysql_table == nullptr ||
      m_prebuilt->m_mysql_table->record[0] == nullptr ||
      m_prebuilt->table == nullptr || m_prebuilt->table->is_intrinsic() ||
      m_prebuilt->trx == nullptr || m_prebuilt->pcur == nullptr ||
      m_prebuilt->clust_pcur == nullptr) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  if (start_key != nullptr && start_key->flag != HA_READ_KEY_OR_NEXT) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }
  if (end_key != nullptr && end_key->flag != HA_READ_BEFORE_KEY) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  dict_index_t *index = innobase_get_index(keyno);
  if (index == nullptr || index->is_clustered() ||
      !index->is_usable(m_prebuilt->trx) || index->is_corrupted()) {
    return pq_map_dberr_to_handler_error(DB_UNSUPPORTED, nullptr);
  }

  trx_t *trx = m_prebuilt->trx;
  const bool had_active_read_view =
      srv_read_only_mode ||
      (trx->read_view != nullptr && MVCC::is_view_active(trx->read_view));
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }
  const bool close_read_view_on_end =
      !had_active_read_view &&
      !thd_test_options(leader_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  auto cleanup_read_view = [&]() {
    if (close_read_view_on_end && trx->read_view != nullptr &&
        MVCC::is_view_active(trx->read_view)) {
      mutex_enter(&trx_sys->mutex);
      trx_sys->mvcc->view_close(trx->read_view, true);
      mutex_exit(&trx_sys->mutex);
    }
    if (close_read_view_on_end && m_prebuilt != nullptr) {
      m_prebuilt->sql_stat_start = true;
    }
  };

  const KEY *key = &table->key_info[keyno];
  mem_heap_t *heap = mem_heap_create(
      2 * (key->actual_key_parts * sizeof(dfield_t) + sizeof(dtuple_t)),
      UT_LOCATION_HERE);
  if (heap == nullptr) {
    cleanup_read_view();
    return pq_map_dberr_to_handler_error(DB_OUT_OF_MEMORY, nullptr);
  }

  dtuple_t *range_start = nullptr;
  dtuple_t *range_end = nullptr;
  dberr_t err = DB_SUCCESS;

  if (start_key != nullptr && start_key->keypart_map != 0) {
    range_start = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_start, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_start, m_prebuilt->srch_key_val1, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(start_key->key),
        static_cast<ulint>(start_key->length));
    if (range_start->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  if (err == DB_SUCCESS && end_key != nullptr && end_key->keypart_map != 0) {
    range_end = dtuple_create(heap, key->actual_key_parts);
    dict_index_copy_types(range_end, index, key->actual_key_parts);
    row_sel_convert_mysql_key_to_innobase(
        range_end, m_prebuilt->srch_key_val2, m_prebuilt->srch_key_val_len,
        index, reinterpret_cast<const byte *>(end_key->key),
        static_cast<ulint>(end_key->length));
    if (range_end->n_fields == 0) {
      err = DB_UNSUPPORTED;
    }
  }

  const uint saved_active_index = active_index;
  Item *saved_pushed_idx_cond = pushed_idx_cond;
  const uint saved_pushed_idx_cond_keyno = pushed_idx_cond_keyno;
  dict_index_t *saved_index = m_prebuilt->index;
  const unsigned saved_read_just_key = m_prebuilt->read_just_key;
  const unsigned saved_template_type = m_prebuilt->template_type;
  const unsigned saved_n_template = m_prebuilt->n_template;
  const unsigned saved_null_bitmap_len = m_prebuilt->null_bitmap_len;
  const unsigned saved_need_to_access_clustered =
      m_prebuilt->need_to_access_clustered;
  const unsigned saved_templ_contains_blob = m_prebuilt->templ_contains_blob;
  const unsigned saved_templ_contains_fixed_point =
      m_prebuilt->templ_contains_fixed_point;
  const ulint saved_mysql_prefix_len = m_prebuilt->mysql_prefix_len;
  const bool saved_idx_cond = m_prebuilt->idx_cond;
  const ulint saved_idx_cond_n_cols = m_prebuilt->idx_cond_n_cols;
  const bool saved_keep_other_fields_on_keyread =
      m_prebuilt->keep_other_fields_on_keyread;
  const bool saved_in_fts_query = m_prebuilt->in_fts_query;
  const bool saved_m_end_range = m_prebuilt->m_end_range;
  mysql_row_templ_t *saved_mysql_template_ptr = m_prebuilt->mysql_template;
  std::vector<mysql_row_templ_t> saved_mysql_template;
  if (saved_mysql_template_ptr != nullptr && table->s->fields > 0) {
    saved_mysql_template.resize(table->s->fields);
    std::memcpy(saved_mysql_template.data(), saved_mysql_template_ptr,
                table->s->fields * sizeof(mysql_row_templ_t));
  }

  auto restore_prebuilt_template_state = [&]() {
    reset_template();
    if (saved_mysql_template_ptr == nullptr) {
      if (m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = nullptr;
    } else {
      if (m_prebuilt->mysql_template != saved_mysql_template_ptr &&
          m_prebuilt->mysql_template != nullptr) {
        ut::free(m_prebuilt->mysql_template);
      }
      m_prebuilt->mysql_template = saved_mysql_template_ptr;
      if (!saved_mysql_template.empty()) {
        std::memcpy(m_prebuilt->mysql_template, saved_mysql_template.data(),
                    saved_mysql_template.size() * sizeof(mysql_row_templ_t));
      }
    }
    active_index = saved_active_index;
    pushed_idx_cond = saved_pushed_idx_cond;
    pushed_idx_cond_keyno = saved_pushed_idx_cond_keyno;
    m_prebuilt->index = saved_index;
    m_prebuilt->read_just_key = saved_read_just_key;
    m_prebuilt->template_type = saved_template_type;
    m_prebuilt->n_template = saved_n_template;
    m_prebuilt->null_bitmap_len = saved_null_bitmap_len;
    m_prebuilt->need_to_access_clustered = saved_need_to_access_clustered;
    m_prebuilt->templ_contains_blob = saved_templ_contains_blob;
    m_prebuilt->templ_contains_fixed_point =
        saved_templ_contains_fixed_point;
    m_prebuilt->mysql_prefix_len = saved_mysql_prefix_len;
    m_prebuilt->idx_cond = saved_idx_cond;
    m_prebuilt->idx_cond_n_cols = saved_idx_cond_n_cols;
    m_prebuilt->keep_other_fields_on_keyread =
        saved_keep_other_fields_on_keyread;
    m_prebuilt->in_fts_query = saved_in_fts_query;
    m_prebuilt->m_end_range = saved_m_end_range;
  };

  if (err == DB_SUCCESS) {
    constexpr uint kMaxRows = 64;
    active_index = keyno;
    pushed_idx_cond = saved_pushed_idx_cond;
    pushed_idx_cond_keyno = keyno;
    m_prebuilt->index = index;
    m_prebuilt->read_just_key = 0;
    build_template(false);
    const bool is_compact = dict_table_is_comp(index->table);
    page_size_t page_size(dict_tf_to_fsp_flags(index->table->flags));
    InnoDB_pq_scan_ctx scan_ctx(index, trx, is_compact, page_size);
    err = scan_ctx.produce_secondary_icp_range_for_user_gate(
        m_prebuilt->m_mysql_table->record[0], m_prebuilt, range_start,
        range_end, kMaxRows, row_sink, row_count);
    restore_prebuilt_template_state();
  }

  if (err != DB_SUCCESS) {
    *row_count = 0;
  }

  mem_heap_free(heap);
  cleanup_read_view();

  return pq_map_dberr_to_handler_error(err, nullptr);
}

/**
  End a PQ worker scan. Cleans up worker cursor state and resources.

  Phase 6B-2: Real implementation with idempotent cleanup.

  @param[in]  worker_ctx  Typed worker context wrapper. Unknown context kinds
              are ignored for idempotent cleanup.
  @return 0 always (cleanup errors are logged, not returned).
*/
int ha_innobase::pq_worker_scan_end(PQ_Worker_context *worker_ctx) {
  if (worker_ctx == nullptr) {
    return 0;
  }

  if (worker_ctx->kind() != PQ_Worker_context_kind::INNODB) {
    return 0;
  }

  auto sql_worker =
      static_cast<InnoDB_pq_sql_worker_context *>(worker_ctx);
  auto innodb_worker = sql_worker->innodb_ctx();

  auto it = std::find(m_pq_worker_ctxs.begin(), m_pq_worker_ctxs.end(),
                      innodb_worker);
  if (it != m_pq_worker_ctxs.end()) {
    m_pq_worker_ctxs.erase(it);
  }

  ut::delete_(sql_worker);
  return 0;
}

int ha_innobase::pq_worker_scan_end() {
  pq_ref_depend = false;

  if (m_prebuilt == nullptr) {
    return 0;
  }

  m_prebuilt->pq_ctx = nullptr;
#ifdef UNIV_DEBUG
  m_prebuilt->pq_prev_ctx = nullptr;
#endif
  m_prebuilt->pq_worker = nullptr;
  m_prebuilt->pq_ref_info = {};
  m_prebuilt->is_attach_ctx = false;

  m_prebuilt->n_fetch_cached = 0;
  m_prebuilt->fetch_cache_first = 0;
  m_prebuilt->n_rows_fetched = 0;
  return 0;
}

/**
  End a PQ leader scan. Releases all resources: thread budget,
  scan context, ranges, and worker contexts.

  Phase 6B-2: Real implementation with idempotent cleanup.

  @param[in]  leader_ctx  Leader context (unused in 6B-2; cleanup is
              done from internal m_pq_leader_ctx).
  @return 0 always (cleanup errors are logged, not returned).
*/
int ha_innobase::pq_leader_scan_end(PQ_Leader_context *leader_ctx) {
  /* Clean up all worker contexts first (reverse order). */
  for (auto it = m_pq_worker_ctxs.rbegin(); it != m_pq_worker_ctxs.rend();
       ++it) {
    if (*it != nullptr) {
      ut::delete_(*it);
    }
  }
  m_pq_worker_ctxs.clear();

  /* Clean up the leader context. */
  if (m_pq_leader_ctx != nullptr) {
    /* Release thread budget back to the parallel reader pool. */
    auto dop = m_pq_leader_ctx->max_threads();
    if (dop > 0) {
      Parallel_reader::release_threads(dop);
    }

    if (m_pq_leader_ctx->close_read_view_on_end()) {
      trx_t *trx = m_pq_leader_ctx->trx();
      if (trx != nullptr && trx->read_view != nullptr &&
          MVCC::is_view_active(trx->read_view)) {
        mutex_enter(&trx_sys->mutex);
        trx_sys->mvcc->view_close(trx->read_view, true);
        mutex_exit(&trx_sys->mutex);
      }
      if (m_prebuilt != nullptr) {
        m_prebuilt->sql_stat_start = true;
      }
    }

    ut::delete_(m_pq_leader_ctx);
    m_pq_leader_ctx = nullptr;
  }

  if (m_pq_sql_leader_ctx != nullptr) {
    ut::delete_(m_pq_sql_leader_ctx);
    m_pq_sql_leader_ctx = nullptr;
  }

  return 0;
}

int ha_innobase::pq_leader_scan_end(void *leader_ctx) {
  return pq_leader_scan_end(static_cast<PQ_Leader_context *>(leader_ctx));
}
