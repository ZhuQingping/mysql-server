/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/parallel_query/pq_iterator.cc
  Parallel Query V1-MVP: PQ table scan iterator skeleton implementation.

  Phase 5B scope:
  - PQTableScanIterator constructor, destructor, Init(), Read() stubs.
  - TryCreatePQTableScanIterator guarded factory.
  - V2-1 returns a PQ iterator for ordinary eligible execution.
  - Init() falls back to an owned serial TableScanIterator before workers,
    Exchange/Gather, worker THDs, or row streams are touched.
  - V2-2 may probe the handler leader init/end contract with DOP=1 before
    serial fallback.
  - No real worker launch, no MQ interaction.
*/

#include "sql/parallel_query/pq_iterator.h"

#include <limits>

#include "my_base.h"
#include "my_dbug.h"
#include "sql/debug_sync.h"
#include "sql/iterators/basic_row_iterators.h"  // TableScanIterator
#include "sql/iterators/timing_iterator.h"     // NewIterator
#include "sql/mysqld.h"       // innodb_hton
#include "sql/parallel_query/pq_clone.h"  // pq_clone_activation_probe
#include "sql/parallel_query/pq_group_aggregate_iterator.h"
#include "sql/parallel_query/pq_iterators.h"
#include "sql/parallel_query/pq_resource_stat.h"
#include "sql/parallel_query/query_result_mq.h"
#include "sql/parallel_query/sql_parallel.h"  // pq_global_stats
#include "sql/sql_class.h"    // THD::variables, THD::pq_is_worker
#include "sql/sql_lex.h"      // LEX::is_explain
#include "sql/sql_optimizer.h"  // JOIN::pq_eligible
#include "sql/table.h"        // TABLE, TABLE_SHARE::db_type

// ---------------------------------------------------------------------------
// PQTableScanIterator implementation (Phase 5B stub)
// ---------------------------------------------------------------------------

namespace {

constexpr uint64 kPQReadWaitTimeoutUs = 1000;

bool pq_table_has_read_fields(const TABLE *table) {
  if (table == nullptr || table->s == nullptr || table->read_set == nullptr) {
    return false;
  }

  for (uint i = 0; i < table->s->fields; ++i) {
    if (bitmap_is_set(table->read_set, i)) return true;
  }
  return false;
}

bool pq_table_has_only_supported_pqwr_fields(const TABLE *table) {
  if (table == nullptr || table->s == nullptr || table->field == nullptr) {
    return false;
  }

  for (uint i = 0; i < table->s->fields; ++i) {
    const Field *field = table->field[i];
    if (field == nullptr) return false;
    switch (field->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_VARCHAR:
        break;
      default:
        return false;
    }
  }
  return true;
}

}  // namespace

PQTableScanIterator::PQTableScanIterator(THD *thd, MEM_ROOT *mem_root,
                                         TABLE *table, JOIN *join,
                                         double expected_rows,
                                         ha_rows *examined_rows)
    : TableRowIterator(thd, table),
      m_mem_root(mem_root),
      m_join(join),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows),
      m_record(table->record[0]) {}

PQTableScanIterator::~PQTableScanIterator() {
  cleanup_pq_resources(true);
}

bool PQTableScanIterator::Init() {
  assert(m_join != nullptr);

  pq_set_execution_state(thd(), PQ_execution_state::ITERATOR_SELECTED);

  auto init_serial_fallback = [&]() -> bool {
    // V2-1 safe fallback window: no worker, Gather/Exchange, or row stream has
    // been initialized. Build the same serial table scan the caller would have
    // built when TryCreatePQTableScanIterator returned nullptr.
    assert(can_fallback_serial());
    if (m_serial_iterator == nullptr) {
      m_serial_iterator = NewIterator<TableScanIterator>(
          thd(), m_mem_root, table(), m_expected_rows, m_examined_rows);
      if (m_serial_iterator == nullptr) return true;
    }

    if (!m_fallback_counted) {
      pq_global_stats.queries_fallback.fetch_add(1, std::memory_order_relaxed);
      m_fallback_counted = true;
    }
    pq_set_execution_state(thd(), PQ_execution_state::FALLBACK_SERIAL);
    return m_serial_iterator->Init();
  };

  if (table() == nullptr || table()->s == nullptr || table()->s->blob_fields > 0) {
    return init_serial_fallback();
  }

  DBUG_EXECUTE_IF("pq_parallel_scan_lifecycle_smoke", {
    if (pq_run_parallel_scan_lifecycle_smoke(thd())) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }
    return init_serial_fallback();
  });

  DBUG_EXECUTE_IF("pq_worker_attach_contract_smoke", {
    uint execute_dop = 0;
    PQ_Leader_context *execute_ctx = nullptr;
    int execute_error = table()->file->pq_leader_scan_init(
        thd(), &execute_ctx, PQ_leader_scan_mode::EXECUTE, 1, &execute_dop,
        false);
    if (execute_error == 0) {
      Gather_operator attach_smoke(1);
      const bool attach_failed =
          attach_smoke.run_worker_attach_contract_smoke(thd(), table(),
                                                        execute_ctx);
      table()->file->pq_leader_scan_end(execute_ctx);
      if (attach_failed) {
        PrintError(HA_ERR_INTERNAL_ERROR);
        return true;
      }
      return init_serial_fallback();
    }
    if (execute_error != HA_ERR_UNSUPPORTED) {
      PrintError(execute_error);
      return true;
    }
    return init_serial_fallback();
  });

  DBUG_EXECUTE_IF("pq_worker_typed_pull_next_smoke", {
    uint execute_dop = 0;
    PQ_Leader_context *execute_ctx = nullptr;
    int execute_error = table()->file->pq_leader_scan_init(
        thd(), &execute_ctx, PQ_leader_scan_mode::EXECUTE, 1, &execute_dop,
        false);
    if (execute_error == 0) {
      Gather_operator pull_smoke(1);
      const bool pull_failed =
          pull_smoke.init() ||
          pull_smoke.configure_worker_open_contexts(table(), execute_ctx, 1) ||
          pull_smoke.run_worker_typed_pull_next_smoke(thd(), table(), 2);
      table()->file->pq_leader_scan_end(execute_ctx);
      if (pull_failed) {
        PrintError(HA_ERR_INTERNAL_ERROR);
        return true;
      }
      return init_serial_fallback();
    }
    if (execute_error != HA_ERR_UNSUPPORTED) {
      PrintError(execute_error);
      return true;
    }
    return init_serial_fallback();
  });

  DBUG_EXECUTE_IF("pq_worker_void_pull_fullscan_smoke", {
    Gather_operator void_pull_smoke(1);
    if (void_pull_smoke.run_worker_void_pull_fullscan_smoke(thd(), table(),
                                                            2)) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }
    return init_serial_fallback();
  });

  DBUG_EXECUTE_IF("pq_leader_row_stream_smoke", {
    pq_global_stats.leader_row_stream_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
    uint execute_dop = 0;
    int execute_error = table()->file->pq_leader_scan_init(
        thd(), &m_leader_ctx, PQ_leader_scan_mode::EXECUTE, 1, &execute_dop,
        false);
    if (execute_error == HA_ERR_UNSUPPORTED) {
      m_leader_ctx = nullptr;
      return init_serial_fallback();
    }
    if (execute_error != 0) {
      cleanup_pq_resources(true);
      PrintError(execute_error);
      return true;
    }

    m_gather = new Gather_operator(1);
    uint32 rows_enqueued = 0;
    if (m_gather == nullptr ||
        m_gather->prepare_leader_row_stream_smoke(thd(), table(), m_leader_ctx,
                                                  2, &rows_enqueued) ||
        rows_enqueued > 2) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    pq_global_stats.leader_row_stream_smoke_selected.fetch_add(
        1, std::memory_order_relaxed);
    mark_pq_started();
    return false;
  });

  DBUG_EXECUTE_IF("pq_leader_row_stream_error_smoke", {
    pq_global_stats.leader_row_stream_error_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
    uint execute_dop = 0;
    int execute_error = table()->file->pq_leader_scan_init(
        thd(), &m_leader_ctx, PQ_leader_scan_mode::EXECUTE, 1, &execute_dop,
        false);
    if (execute_error == HA_ERR_UNSUPPORTED) {
      m_leader_ctx = nullptr;
      return init_serial_fallback();
    }
    if (execute_error != 0) {
      cleanup_pq_resources(true);
      PrintError(execute_error);
      return true;
    }

    m_gather = new Gather_operator(1);
    if (m_gather == nullptr ||
        m_gather->prepare_leader_row_stream_error_smoke(thd(), table(),
                                                        m_leader_ctx)) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    pq_global_stats.leader_row_stream_error_smoke_selected.fetch_add(
        1, std::memory_order_relaxed);
    mark_pq_started();
    return false;
  });

  DBUG_EXECUTE_IF("pq_leader_pqwr_record_gather_smoke", {
    pq_global_stats.leader_row_stream_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
    uint execute_dop = 0;
    int execute_error = table()->file->pq_leader_scan_init(
        thd(), &m_leader_ctx, PQ_leader_scan_mode::EXECUTE, 1, &execute_dop,
        false);
    if (execute_error == HA_ERR_UNSUPPORTED) {
      m_leader_ctx = nullptr;
      return init_serial_fallback();
    }
    if (execute_error != 0) {
      cleanup_pq_resources(true);
      PrintError(execute_error);
      return true;
    }

    m_gather = new Gather_operator(1);
    m_record_gather = new MQ_record_gather(thd(), table());
    if (m_gather == nullptr || m_record_gather == nullptr ||
        m_gather->init() || m_record_gather->mq_scan_init(m_gather)) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    const ha_rows sent_rows_before = thd()->get_sent_row_count();
    Query_result_mq result(nullptr, m_record_gather->exchange()->get_mq_handle(0),
                           false);
    mem_root_deque<Item *> fields(thd()->mem_root);
    fields.push_back(new (thd()->mem_root) Item_int(7));
    fields.push_back(new (thd()->mem_root) Item_int(42));
    const bool send_failed =
        fields[0] == nullptr || fields[1] == nullptr ||
        result.start_execution(thd()) ||
        result.send_result_set_metadata(thd(), fields, 0) ||
        result.send_data(thd(), fields) || result.send_eof(thd());
    thd()->set_sent_row_count(sent_rows_before);
    if (send_failed) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    m_use_worker_result_record_gather = true;
    pq_global_stats.leader_row_stream_smoke_selected.fetch_add(
        1, std::memory_order_relaxed);
    mark_pq_started();
    return false;
  });

  DBUG_EXECUTE_IF("pq_parallel_scan_iterator_row_value_smoke", {
    m_parallel_scan_delegate = NewIterator<ParallelScanIterator>(
        thd(), m_mem_root, nullptr, table(), m_expected_rows, m_examined_rows,
        m_join, nullptr, false, nullptr);
    if (m_parallel_scan_delegate == nullptr ||
        m_parallel_scan_delegate->Init()) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }
    mark_pq_started();
    return false;
  });

  DBUG_EXECUTE_IF("pq_parallel_scan_iterator_order_gather_smoke", {
    m_parallel_scan_delegate = NewIterator<ParallelScanIterator>(
        thd(), m_mem_root, nullptr, table(), m_expected_rows, m_examined_rows,
        m_join, nullptr, false, nullptr);
    if (m_parallel_scan_delegate == nullptr ||
        m_parallel_scan_delegate->Init()) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }
    mark_pq_started();
    return false;
  });

  // V2-2 bridge smoke: prove the handler can create and release a SQL-visible
  // leader context without starting workers or reading rows. Unsupported
  // engines/states still use the V2-1 serial fallback path; real handler
  // errors are reported before serial iterator state is initialized.
  uint actual_dop = 0;
  uint requested_dop = thd()->variables.parallel_default_dop;
  if (requested_dop == 0) requested_dop = 1;
  bool force_threaded_pqwr_record_gather_path = false;
  DBUG_EXECUTE_IF("pq_read_threaded_pqwr_record_gather_path", {
    force_threaded_pqwr_record_gather_path = true;
  });
  bool force_visible_void_pull_fullscan_path = false;
  DBUG_EXECUTE_IF("pq_visible_void_pull_fullscan_path", {
    force_visible_void_pull_fullscan_path = true;
  });
  bool force_threaded_read_dop4_shadow_path = false;
  DBUG_EXECUTE_IF("pq_read_threaded_dop4_shadow_path", {
    force_threaded_read_dop4_shadow_path = true;
  });
  const bool explicit_threaded_read_dop4_shadow_path =
      requested_dop == 4 &&
      (force_threaded_read_dop4_shadow_path ||
       thd()->variables.parallel_query_experimental_threaded_dop4);
  const bool threaded_visible_void_pull_fullscan_path =
      !force_threaded_pqwr_record_gather_path &&
      !force_visible_void_pull_fullscan_path &&
      !explicit_threaded_read_dop4_shadow_path &&
      should_enter_threaded_visible_void_pull_fullscan_path(requested_dop);
  const bool visible_void_pull_fullscan_path =
      !force_threaded_pqwr_record_gather_path &&
      !threaded_visible_void_pull_fullscan_path &&
      should_enter_visible_void_pull_fullscan_path(requested_dop);
  const bool threaded_pqwr_record_gather_path =
      force_threaded_pqwr_record_gather_path ||
      (!visible_void_pull_fullscan_path &&
       !threaded_visible_void_pull_fullscan_path &&
       should_enter_threaded_pqwr_record_gather_path(requested_dop));
  const bool threaded_read_shadow_path =
      !threaded_pqwr_record_gather_path &&
      !visible_void_pull_fullscan_path &&
      !threaded_visible_void_pull_fullscan_path &&
      should_enter_threaded_read_shadow_path(requested_dop);
  const bool read_shadow_path =
      !threaded_read_shadow_path && !threaded_pqwr_record_gather_path &&
      !visible_void_pull_fullscan_path &&
      !threaded_visible_void_pull_fullscan_path &&
      should_enter_read_shadow_path(requested_dop);

  if (threaded_visible_void_pull_fullscan_path) {
    return init_threaded_visible_void_pull_fullscan_path(requested_dop);
  }

  if (visible_void_pull_fullscan_path) {
    return init_visible_void_pull_fullscan_path();
  }

  pq_global_stats.probe_attempts.fetch_add(1, std::memory_order_relaxed);
  int error = table()->file->pq_leader_scan_init(
      thd(), &m_leader_ctx, PQ_leader_scan_mode::PROBE, requested_dop,
      &actual_dop, false);
  bool probe_supported = false;
  if (error == 0) {
    pq_global_stats.probe_success.fetch_add(1, std::memory_order_relaxed);
    probe_supported = true;
    uint smoke_dop = actual_dop > 0 ? actual_dop : requested_dop;
    m_gather = new Gather_operator(smoke_dop);
    if (m_gather == nullptr || m_gather->init() ||
        m_gather->configure_worker_open_contexts(table(), m_leader_ctx,
                                                 smoke_dop) ||
        m_gather->run_worker_lifecycle_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator open_smoke(1);
    if (open_smoke.init() ||
        open_smoke.configure_worker_open_contexts(table(), m_leader_ctx, 1) ||
        open_smoke.run_worker_open_table_smoke(thd(), table())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    if (m_gather->run_exchange_row_image_smoke(thd(), table())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    if (m_gather->run_query_result_mq_contract_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    if (m_gather->run_query_result_mq_send_data_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    if (m_gather->run_query_result_mq_adapter_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    if (m_gather->run_query_result_mq_wiring_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    if (m_gather->run_query_result_mq_raw_field_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator threaded_result_smoke(1);
    if (threaded_result_smoke.run_query_result_mq_threaded_probe_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator partial_group_smoke(smoke_dop);
    if (partial_group_smoke.init() ||
        partial_group_smoke.run_exchange_partial_group_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator orderby_smoke(3);
    if (orderby_smoke.run_exchange_sort_smoke(thd(), table())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    uint32 typed_group_smoke_groups = 0;
    uint64 typed_group_smoke_sum = 0;
    if (RunPQGroupAggregateTypedStateSmoke(&typed_group_smoke_groups,
                                           &typed_group_smoke_sum)) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    pq_global_stats.groupby_typed_smoke_groups.fetch_add(
        typed_group_smoke_groups, std::memory_order_relaxed);
    pq_global_stats.groupby_typed_smoke_sum.fetch_add(
        typed_group_smoke_sum, std::memory_order_relaxed);
    Gather_operator producer_smoke(1);
    if (producer_smoke.init() ||
        producer_smoke.run_worker_producer_loop_smoke(thd(), table())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator producer_error_smoke(1);
    if (producer_error_smoke.init() ||
        producer_error_smoke.run_worker_producer_error_smoke(thd(), table())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator producer_abort_smoke(1);
    if (producer_abort_smoke.init() ||
        producer_abort_smoke.run_worker_producer_abort_smoke(thd(), table())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    cleanup_pq_resources(false);

    Gather_operator worker_execute_smoke(1);
    if (worker_execute_smoke.run_worker_execute_iterator_smoke(thd(), m_join,
                                                              nullptr)) {
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    DBUG_EXECUTE_IF("pq_worker_execute_iterator_threaded_precheck_smoke", {
      Gather_operator worker_execute_threaded_smoke(1);
      if (worker_execute_threaded_smoke
              .run_worker_execute_iterator_threaded_precheck_smoke(thd(),
                                                                   m_join)) {
        PrintError(HA_ERR_OUT_OF_MEM);
        return true;
      }
    });

    if (!read_shadow_path && !threaded_read_shadow_path &&
        !threaded_pqwr_record_gather_path) {
      PQ_Leader_context *partial_execute_ctx = nullptr;
      uint partial_execute_dop = 0;
      error = table()->file->pq_leader_scan_init(
          thd(), &partial_execute_ctx, PQ_leader_scan_mode::EXECUTE,
          smoke_dop, &partial_execute_dop, false);
      if (error == 0) {
        const uint worker_partial_dop =
            partial_execute_dop > 0 ? partial_execute_dop : smoke_dop;
        Gather_operator worker_partial_smoke(worker_partial_dop);
        const bool partial_failed =
            worker_partial_smoke.init() ||
            worker_partial_smoke.configure_worker_open_contexts(
                table(), partial_execute_ctx, worker_partial_dop) ||
            worker_partial_smoke.run_worker_partial_group_smoke(thd(),
                                                                table());
        table()->file->pq_leader_scan_end(partial_execute_ctx);
        if (partial_failed) {
          PrintError(HA_ERR_OUT_OF_MEM);
          return true;
        }
      } else if (error != HA_ERR_UNSUPPORTED) {
        PrintError(error);
        return true;
      }

      PQ_Leader_context *execute_ctx = nullptr;
      uint execute_dop = 0;
      error = table()->file->pq_leader_scan_init(
          thd(), &execute_ctx, PQ_leader_scan_mode::EXECUTE, 1, &execute_dop,
          false);
      if (error == 0) {
        Gather_operator callback_smoke(1);
        (void)(callback_smoke.init() ||
               callback_smoke.configure_worker_open_contexts(
                   table(), execute_ctx, 1) ||
               callback_smoke.run_worker_callback_conversion_smoke(thd(),
                                                                   table()));
        Gather_operator callback_multirow_smoke(1);
        (void)(callback_multirow_smoke.init() ||
               callback_multirow_smoke.configure_worker_open_contexts(
                   table(), execute_ctx, 1) ||
               callback_multirow_smoke.run_worker_callback_multirow_producer_smoke(
                   thd(), table()));
        table()->file->pq_leader_scan_end(execute_ctx);
      } else if (error != HA_ERR_UNSUPPORTED) {
        PrintError(error);
        return true;
      }
    }
  } else if (error == HA_ERR_UNSUPPORTED) {
    pq_global_stats.probe_unsupported.fetch_add(1, std::memory_order_relaxed);
  } else {
    cleanup_pq_resources(true);
    PrintError(error);
    return true;
  }

  if ((read_shadow_path || threaded_read_shadow_path ||
       threaded_pqwr_record_gather_path) &&
      probe_supported) {
    const uint threaded_execute_dop =
        (threaded_read_shadow_path || threaded_pqwr_record_gather_path)
            ? requested_dop
            : 1;
    uint execute_dop = 0;
    error = table()->file->pq_leader_scan_init(
        thd(), &m_leader_ctx, PQ_leader_scan_mode::EXECUTE,
        threaded_execute_dop, &execute_dop, false);
    if (error != 0) {
      cleanup_pq_resources(true);
      PrintError(error);
      return true;
    }

    const uint gather_dop = execute_dop > 0 ? execute_dop : threaded_execute_dop;
    m_gather = new Gather_operator(gather_dop);
    if (m_gather == nullptr || m_gather->init() ||
        m_gather->configure_worker_open_contexts(table(), m_leader_ctx,
                                                 gather_dop)) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }

    if (threaded_pqwr_record_gather_path) {
      m_record_gather = new MQ_record_gather(thd(), table());
      if (m_record_gather == nullptr ||
          m_record_gather->mq_scan_init(m_gather)) {
        cleanup_pq_resources(true);
        PrintError(HA_ERR_INTERNAL_ERROR);
        return true;
      }
      if (m_gather->run_worker_callback_pqwr_threaded_producer(
              thd(), table(), std::numeric_limits<uint32>::max())) {
        cleanup_pq_resources(true);
        PrintError(HA_ERR_INTERNAL_ERROR);
        return true;
      }
      m_use_worker_result_record_gather = true;
      pq_global_stats.visible_pqwr_record_gather_selected.fetch_add(
          1, std::memory_order_relaxed);
      DEBUG_SYNC(thd(), "pq_read_threaded_worker_started");
    } else if (threaded_read_shadow_path) {
      if (m_gather->run_worker_callback_threaded_producer(
              thd(), table(), std::numeric_limits<uint32>::max())) {
        cleanup_pq_resources(true);
        PrintError(HA_ERR_INTERNAL_ERROR);
        return true;
      }
      DEBUG_SYNC(thd(), "pq_read_threaded_worker_started");
      DBUG_EXECUTE_IF("pq_read_threaded_shadow_abort_after_start", {
        cleanup_pq_resources(true);
        PrintError(HA_ERR_INTERNAL_ERROR);
        return true;
      });
    } else {
      uint32 rows_produced = 0;
      if (m_gather->run_worker_callback_limited_producer(thd(), table(), 2,
                                                         &rows_produced)) {
        cleanup_pq_resources(true);
        PrintError(HA_ERR_INTERNAL_ERROR);
        return true;
      }
    }

    mark_pq_started();
    return false;
  }

  return init_serial_fallback();
}

void PQTableScanIterator::cleanup_pq_resources(bool abort_workers) {
  const bool visible_void_pull_cleanup = m_use_visible_void_pull;
  const bool threaded_visible_void_pull_cleanup =
      m_use_threaded_visible_void_pull;
  if (m_use_visible_void_pull && m_gather != nullptr) {
    THD *leader_thd = thd();
    auto *worker = m_gather->get_worker(0);
    if (worker != nullptr && worker->m_worker_ctx != nullptr &&
        worker->m_open_ctx.worker_handler != nullptr &&
        worker->m_open_ctx.worker_thd != nullptr) {
      worker->m_open_ctx.worker_thd->store_globals();
      worker->m_open_ctx.worker_handler->pq_worker_scan_end(
          worker->m_worker_ctx);
      leader_thd->store_globals();
      worker->m_worker_ctx = nullptr;
      pq_global_stats.visible_void_pull_cleanup.fetch_add(
          1, std::memory_order_relaxed);
    }
  }
  m_use_visible_void_pull = false;
  if (m_use_threaded_visible_void_pull) {
    pq_global_stats.visible_void_pull_cleanup.fetch_add(
        1, std::memory_order_relaxed);
  }
  m_use_threaded_visible_void_pull = false;

  if (m_record_gather != nullptr) {
    m_record_gather->mq_scan_end();
    delete m_record_gather;
    m_record_gather = nullptr;
  }
  m_use_worker_result_record_gather = false;

  if (m_gather != nullptr) {
    if (abort_workers && m_gather->is_initialized()) {
      m_gather->abort_workers(thd());
    }
    auto *worker =
        (visible_void_pull_cleanup || threaded_visible_void_pull_cleanup)
            ? m_gather->get_worker(0)
            : nullptr;
    if (worker != nullptr && worker->m_open_ctx.worker_thd != nullptr) {
      worker->m_open_ctx.worker_thd->store_globals();
    }
    m_gather->destroy();
    if (worker != nullptr) {
      thd()->store_globals();
    }
    delete m_gather;
    m_gather = nullptr;
  }

  if (m_leader_ctx != nullptr) {
    if (m_leader_ctx_from_ha_pq_init) {
      table()->file->ha_pq_end();
    } else {
      table()->file->pq_leader_scan_end(m_leader_ctx);
    }
    m_leader_ctx = nullptr;
  }
  m_leader_ctx_from_ha_pq_init = false;
}

bool PQTableScanIterator::init_visible_void_pull_fullscan_path() {
  m_gather = new Gather_operator(1);
  if (m_gather == nullptr || m_gather->init()) {
    cleanup_pq_resources(true);
    PrintError(HA_ERR_INTERNAL_ERROR);
    return true;
  }

  table()->file->pq_ref = false;
  table()->file->pq_ref_depend = false;
  table()->file->pq_range_type = PQ_QUICK_SELECT_NONE;
  table()->file->ha_set_reverse_scan(false);
  int init_error = table()->file->ha_pq_init(1, MAX_KEY);
  if (init_error != 0 || table()->file->pq_ctx == nullptr) {
    cleanup_pq_resources(true);
    PrintError(init_error != 0 ? init_error : HA_ERR_INTERNAL_ERROR);
    return true;
  }
  table()->file->pq_table_scan = true;
  m_leader_ctx = static_cast<PQ_Leader_context *>(table()->file->pq_ctx);
  m_leader_ctx_from_ha_pq_init = true;
  m_use_visible_void_pull = true;

  if (m_gather->configure_worker_open_contexts(table(), m_leader_ctx, 1)) {
    cleanup_pq_resources(true);
    PrintError(HA_ERR_INTERNAL_ERROR);
    return true;
  }

  auto *worker = m_gather->get_worker(0);
  if (worker == nullptr || pq_create_worker_thd(worker, m_gather) == nullptr) {
    thd()->store_globals();
    cleanup_pq_resources(true);
    PrintError(HA_ERR_INTERNAL_ERROR);
    return true;
  }

  worker->m_worker_thd->store_globals();
  const bool worker_init_failed =
      pq_open_worker_table(&worker->m_open_ctx) ||
      worker->m_open_ctx.worker_handler->pq_worker_scan_init(
          &worker->m_open_ctx, &worker->m_worker_ctx) != 0 ||
      worker->m_worker_ctx == nullptr;
  thd()->store_globals();
  if (worker_init_failed) {
    cleanup_pq_resources(true);
    PrintError(HA_ERR_INTERNAL_ERROR);
    return true;
  }

  pq_global_stats.visible_void_pull_selected.fetch_add(
      1, std::memory_order_relaxed);
  mark_pq_started();
  return false;
}

bool PQTableScanIterator::init_threaded_visible_void_pull_fullscan_path(
    uint requested_dop) {
  if (requested_dop == 0) return true;

  m_gather = new Gather_operator(requested_dop);
  if (m_gather == nullptr || m_gather->init()) {
    cleanup_pq_resources(true);
    PrintError(HA_ERR_INTERNAL_ERROR);
    return true;
  }

  table()->file->pq_ref = false;
  table()->file->pq_ref_depend = false;
  table()->file->pq_range_type = PQ_QUICK_SELECT_NONE;
  table()->file->ha_set_reverse_scan(false);
  const int init_error = table()->file->ha_pq_init(requested_dop, MAX_KEY);
  if (init_error != 0 || table()->file->pq_ctx == nullptr) {
    cleanup_pq_resources(true);
    PrintError(init_error != 0 ? init_error : HA_ERR_INTERNAL_ERROR);
    return true;
  }
  table()->file->pq_table_scan = true;
  m_leader_ctx = static_cast<PQ_Leader_context *>(table()->file->pq_ctx);
  m_leader_ctx_from_ha_pq_init = true;
  m_use_threaded_visible_void_pull = true;

  if (m_gather->configure_worker_open_contexts(table(), m_leader_ctx,
                                               requested_dop)) {
    cleanup_pq_resources(true);
    PrintError(HA_ERR_INTERNAL_ERROR);
    return true;
  }

  if (m_gather->run_worker_void_pull_threaded_producer(thd(), table())) {
    cleanup_pq_resources(true);
    PrintError(HA_ERR_INTERNAL_ERROR);
    return true;
  }

  pq_global_stats.visible_void_pull_selected.fetch_add(
      1, std::memory_order_relaxed);
  pq_global_stats.visible_void_pull_threaded_selected.fetch_add(
      1, std::memory_order_relaxed);
  mark_pq_started();
  return false;
}

bool PQTableScanIterator::should_enter_read_shadow_path(
    uint requested_dop) const {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_read_shadow_path", enabled = true;);
  return enabled && requested_dop > 0 && table() != nullptr &&
         table()->s != nullptr && table()->s->blob_fields == 0 &&
         table()->s->reclength > 0;
}

bool PQTableScanIterator::should_enter_threaded_read_shadow_path(
    uint requested_dop) const {
  const bool dop2_fullscan_enabled =
      thd() != nullptr && m_join != nullptr && m_join->pq_eligible &&
      thd()->variables.parallel_query && requested_dop == 2;
  bool dop1_enabled =
      thd() != nullptr &&
      thd()->variables.parallel_query_experimental_threaded_dop1;
  bool dop2_enabled =
      dop2_fullscan_enabled ||
      (thd() != nullptr &&
       thd()->variables.parallel_query_experimental_threaded_dop);
  bool dop4_enabled =
      thd() != nullptr &&
      thd()->variables.parallel_query_experimental_threaded_dop4;
  DBUG_EXECUTE_IF("pq_read_threaded_shadow_path", dop1_enabled = true;);
  DBUG_EXECUTE_IF("pq_read_threaded_dop2_shadow_path", {
    dop2_enabled = true;
  });
  DBUG_EXECUTE_IF("pq_read_threaded_dop4_shadow_path", {
    dop4_enabled = true;
  });
  return ((dop1_enabled && requested_dop == 1) ||
          (dop2_enabled && requested_dop == 2) ||
          (dop4_enabled && requested_dop == 4)) &&
         table() != nullptr &&
         table()->s != nullptr && table()->s->blob_fields == 0 &&
         table()->s->reclength > 0;
}

bool PQTableScanIterator::should_enter_visible_void_pull_fullscan_path(
    uint requested_dop) const {
  bool enabled = thd() != nullptr && m_join != nullptr && m_join->pq_eligible &&
                 thd()->variables.parallel_query && requested_dop == 2;
  DBUG_EXECUTE_IF("pq_visible_void_pull_fullscan_path", {
    enabled = true;
  });
  return enabled && table() != nullptr && table()->s != nullptr &&
         table()->s->blob_fields == 0 && table()->s->reclength > 0 &&
         pq_table_has_read_fields(table());
}

bool PQTableScanIterator::should_enter_threaded_visible_void_pull_fullscan_path(
    uint requested_dop) const {
  const bool enabled =
      thd() != nullptr && m_join != nullptr && m_join->pq_eligible &&
      thd()->variables.parallel_query &&
      (requested_dop == 2 || requested_dop == 4);
  return enabled && table() != nullptr && table()->s != nullptr &&
         table()->s->blob_fields == 0 && table()->s->reclength > 0 &&
         pq_table_has_read_fields(table());
}

bool PQTableScanIterator::should_enter_threaded_pqwr_record_gather_path(
    uint requested_dop) const {
  bool enabled = thd() != nullptr && m_join != nullptr && m_join->pq_eligible &&
                 thd()->variables.parallel_query && requested_dop == 2;
  DBUG_EXECUTE_IF("pq_read_threaded_pqwr_record_gather_path", {
    enabled = true;
  });
  return enabled && thd() != nullptr && m_join != nullptr &&
         m_join->pq_eligible && thd()->variables.parallel_query &&
         requested_dop == 2 && table() != nullptr && table()->s != nullptr &&
         table()->s->blob_fields == 0 && table()->s->fields >= 2 &&
         table()->s->reclength > 0 && pq_table_has_read_fields(table()) &&
         pq_table_has_only_supported_pqwr_fields(table());
}

int PQTableScanIterator::Read() {
  if (m_parallel_scan_delegate != nullptr) {
    return m_parallel_scan_delegate->Read();
  }

  if (m_serial_iterator != nullptr) {
    return m_serial_iterator->Read();
  }

  assert(m_runtime_state == Runtime_state::PQ_STARTED ||
         m_runtime_state == Runtime_state::PQ_ROW_RETURNED);
  assert(m_gather != nullptr);

  if (m_use_visible_void_pull) {
    auto *worker = m_gather->get_worker(0);
    if (worker == nullptr || worker->m_open_ctx.worker_handler == nullptr ||
        worker->m_open_ctx.worker_table == nullptr ||
        worker->m_open_ctx.worker_thd == nullptr ||
        worker->m_open_ctx.worker_table->record[0] == nullptr ||
        m_leader_ctx == nullptr || table() == nullptr || table()->s == nullptr ||
        table()->record[0] == nullptr) {
      pq_global_stats.visible_void_pull_failures.fetch_add(
          1, std::memory_order_relaxed);
      cleanup_pq_resources(true);
      PrintError(HA_ERR_INTERNAL_ERROR);
      return 1;
    }

    DBUG_EXECUTE_IF("pq_visible_void_pull_force_kill", {
      thd()->killed = THD::KILL_QUERY;
    });
    if (m_gather->check_leader_kill(thd())) {
      m_gather->propagate_kill_to_workers(thd());
      cleanup_pq_resources(true);
      thd()->send_kill_message();
      return 1;
    }

    THD *leader_thd = thd();
    worker->m_open_ctx.worker_thd->store_globals();
    const int error = worker->m_open_ctx.worker_handler->ha_pq_next(
        worker->m_open_ctx.worker_table->record[0], m_leader_ctx);
    leader_thd->store_globals();
    if (error == HA_ERR_END_OF_FILE) {
      pq_global_stats.visible_void_pull_eofs.fetch_add(
          1, std::memory_order_relaxed);
      if (!m_executed_counted) {
        pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
        pq_global_stats.queries_executed.fetch_add(
            1, std::memory_order_relaxed);
        m_executed_counted = true;
      }
      cleanup_pq_resources(false);
      return -1;
    }
    if (error != 0) {
      pq_global_stats.visible_void_pull_failures.fetch_add(
          1, std::memory_order_relaxed);
      cleanup_pq_resources(true);
      PrintError(error);
      return 1;
    }

    std::memcpy(table()->record[0], worker->m_open_ctx.worker_table->record[0],
                table()->s->reclength);
    if (!m_executed_counted) {
      mark_pq_row_returned();
      pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
      pq_global_stats.queries_executed.fetch_add(1,
                                                 std::memory_order_relaxed);
      m_executed_counted = true;
    }
    pq_global_stats.rows_scanned.fetch_add(1, std::memory_order_relaxed);
    pq_global_stats.visible_void_pull_rows.fetch_add(
        1, std::memory_order_relaxed);
    return 0;
  }

  assert(m_gather->get_exchange() != nullptr);

  auto *exchange = m_gather->get_exchange();

  for (;;) {
    DBUG_EXECUTE_IF("pq_leader_row_stream_force_kill", {
      thd()->killed = THD::KILL_QUERY;
    });
    if (m_gather->check_leader_kill(thd())) {
      m_gather->propagate_kill_to_workers(thd());
      cleanup_pq_resources(true);
      thd()->send_kill_message();
      return 1;
    }

    Exchange_nosort::Materialize_status status =
        Exchange_nosort::Materialize_status::ERROR;
    if (m_use_worker_result_record_gather) {
      uint64 id_value = 0;
      uint64 v_value = 0;
      bool eof = false;
      bool row = false;
      if (m_record_gather == nullptr ||
          m_record_gather->mq_scan_next_worker_result(&id_value, &v_value, &eof,
                                                      &row)) {
        cleanup_pq_resources(true);
        PrintError(HA_ERR_INTERNAL_ERROR);
        return 1;
      }
      if (row) {
        status = Exchange_nosort::Materialize_status::ROW;
      } else if (eof) {
        status = Exchange_nosort::Materialize_status::EOF_REACHED;
      } else {
        status = Exchange_nosort::Materialize_status::WOULD_BLOCK;
      }
    } else if (exchange->materialize_next_record_image_status(table(),
                                                              &status)) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_INTERNAL_ERROR);
      return 1;
    }

    if (status == Exchange_nosort::Materialize_status::ROW) {
      if (!m_executed_counted) {
        mark_pq_row_returned();
        pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
        pq_global_stats.queries_executed.fetch_add(1,
                                                   std::memory_order_relaxed);
        m_executed_counted = true;
      }
      pq_global_stats.rows_scanned.fetch_add(1, std::memory_order_relaxed);
      if (m_use_worker_result_record_gather) {
        pq_global_stats.visible_pqwr_record_gather_rows.fetch_add(
            1, std::memory_order_relaxed);
      }
      if (m_use_threaded_visible_void_pull) {
        pq_global_stats.visible_void_pull_rows.fetch_add(
            1, std::memory_order_relaxed);
      }
      DBUG_EXECUTE_IF("pq_leader_row_stream_smoke", {
        pq_global_stats.leader_row_stream_smoke_rows.fetch_add(
            1, std::memory_order_relaxed);
      });
      DBUG_EXECUTE_IF("pq_leader_pqwr_record_gather_smoke", {
        pq_global_stats.leader_row_stream_smoke_rows.fetch_add(
            1, std::memory_order_relaxed);
      });
      return 0;
    }

    if (status == Exchange_nosort::Materialize_status::EOF_REACHED) {
      if (m_use_threaded_visible_void_pull) {
        pq_global_stats.visible_void_pull_eofs.fetch_add(
            1, std::memory_order_relaxed);
      }
      const auto error_state = m_gather->resolve_error_priority(thd());
      if (error_state.has_error()) {
        int error_code = HA_ERR_INTERNAL_ERROR;
        if (error_state.error_code != 0) {
          error_code = error_state.error_code;
          thd()->pq_error = error_code;
        }
        cleanup_pq_resources(true);
        PrintError(error_code);
        return 1;
      }
      if (!m_executed_counted) {
        pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
        pq_global_stats.queries_executed.fetch_add(1,
                                                   std::memory_order_relaxed);
        m_executed_counted = true;
      }
      cleanup_pq_resources(false);
      return -1;
    }

    if (status == Exchange_nosort::Materialize_status::WOULD_BLOCK) {
      const auto error_state = m_gather->resolve_error_priority(thd());
      if (error_state.has_error()) {
        int error_code = HA_ERR_INTERNAL_ERROR;
        if (error_state.error_code != 0) {
          error_code = error_state.error_code;
          thd()->pq_error = error_code;
        }
        cleanup_pq_resources(true);
        PrintError(error_code);
        return 1;
      }
      exchange->wait_for_message(kPQReadWaitTimeoutUs);
      continue;
    }

    DBUG_EXECUTE_IF("pq_leader_row_stream_error_smoke", {
      pq_global_stats.leader_row_stream_error_smoke_errors.fetch_add(
          1, std::memory_order_relaxed);
    });
    const auto error_state = m_gather->resolve_error_priority(thd());
    if (m_use_threaded_visible_void_pull) {
      pq_global_stats.visible_void_pull_failures.fetch_add(
          1, std::memory_order_relaxed);
    }
    int error_code = HA_ERR_INTERNAL_ERROR;
    if (error_state.has_error() && error_state.error_code != 0) {
      error_code = error_state.error_code;
      thd()->pq_error = error_code;
    }
    cleanup_pq_resources(true);
    DBUG_EXECUTE_IF("pq_leader_row_stream_error_smoke", {
      pq_global_stats.leader_row_stream_error_smoke_cleanup.fetch_add(
          1, std::memory_order_relaxed);
    });
    PrintError(error_code);
    return 1;
  }
}

void PQTableScanIterator::UnlockRow() {
  if (m_parallel_scan_delegate != nullptr) {
    m_parallel_scan_delegate->UnlockRow();
  } else if (m_serial_iterator != nullptr) {
    m_serial_iterator->UnlockRow();
  } else {
    TableRowIterator::UnlockRow();
  }
}

void PQTableScanIterator::SetNullRowFlag(bool is_null_row) {
  if (m_parallel_scan_delegate != nullptr) {
    m_parallel_scan_delegate->SetNullRowFlag(is_null_row);
  } else if (m_serial_iterator != nullptr) {
    m_serial_iterator->SetNullRowFlag(is_null_row);
  } else {
    TableRowIterator::SetNullRowFlag(is_null_row);
  }
}

void PQTableScanIterator::StartPSIBatchMode() {
  if (m_parallel_scan_delegate != nullptr) {
    m_parallel_scan_delegate->StartPSIBatchMode();
  } else if (m_serial_iterator != nullptr) {
    m_serial_iterator->StartPSIBatchMode();
  } else {
    TableRowIterator::StartPSIBatchMode();
  }
}

void PQTableScanIterator::EndPSIBatchModeIfStarted() {
  if (m_parallel_scan_delegate != nullptr) {
    m_parallel_scan_delegate->EndPSIBatchModeIfStarted();
  } else if (m_serial_iterator != nullptr) {
    m_serial_iterator->EndPSIBatchModeIfStarted();
  } else {
    if (m_gather != nullptr || m_leader_ctx != nullptr) {
      cleanup_pq_resources(true);
    }
    TableRowIterator::EndPSIBatchModeIfStarted();
  }
}

// ---------------------------------------------------------------------------
// TryCreatePQTableScanIterator guarded factory
// ---------------------------------------------------------------------------

unique_ptr_destroy_only<RowIterator> TryCreatePQTableScanIterator(
    THD *thd, MEM_ROOT *mem_root, TABLE *table, JOIN *join,
    double expected_rows, ha_rows *examined_rows) {
  // -----------------------------------------------------------------
  // Guard 1: parallel_query system variable must be ON.
  // Default is OFF, so this guard alone ensures no behavior change
  // unless the user explicitly enables PQ.
  // -----------------------------------------------------------------
  if (!thd->variables.parallel_query) {
    return nullptr;
  }

  // -----------------------------------------------------------------
  // Guard 2: JOIN must exist. TABLE_SCAN paths that have no JOIN
  // (e.g., certain DDL/internal scans) must always go serial.
  // -----------------------------------------------------------------
  if (join == nullptr) {
    return nullptr;
  }

  // -----------------------------------------------------------------
  // Guard 3: optimizer must have marked this query as PQ-eligible.
  // pq_eligible == false means the optimizer found a reason to
  // reject PQ (e.g., subquery, UNION, non-SELECT, hypergraph).
  // -----------------------------------------------------------------
  if (!join->pq_eligible) {
    return nullptr;
  }

  // -----------------------------------------------------------------
  // Guard 4: table must use InnoDB engine. PQ V1 only supports
  // InnoDB clustered full scan. Other engines must go serial.
  // -----------------------------------------------------------------
  if (table->s->db_type() != innodb_hton) {
    return nullptr;
  }

  // -----------------------------------------------------------------
  // Guard 5: THD must not be a PQ worker. No recursive/nested
  // parallelism is allowed. A worker THD executing its own plan
  // must not spawn sub-workers.
  // -----------------------------------------------------------------
  if (thd->pq_is_worker) {
    return nullptr;
  }

  // EXPLAIN may build iterators, but it is not a real statement execution
  // fallback. Keep PQ execution status counters tied to non-EXPLAIN execution.
  if (thd->lex != nullptr && thd->lex->is_explain()) {
    pq_set_execution_state(thd, PQ_execution_state::ELIGIBLE);
    return nullptr;
  }

  DBUG_EXECUTE_IF("pq_parallel_scan_iterator_order_gather_smoke", {
    return NewIterator<PQTableScanIterator>(thd, mem_root, mem_root, table,
                                            join, expected_rows,
                                            examined_rows);
  });

  if (pq_clone_activation_probe(thd, join)) return nullptr;

  return NewIterator<PQTableScanIterator>(thd, mem_root, mem_root, table, join,
                                          expected_rows, examined_rows);
}
