/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in license.xml
   elsewhere in this distribution.  You may use the software under the
   terms of the GNU General Public License, version 2.0,
   or the terms of any other license of the available in this distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/parallel_query/sql_parallel.cc
  Parallel Query: Gather operator and worker lifecycle implementation.

  Phase 4 scope:
  - Implement PQ_worker_info::transition_status() with forward-only transitions.
  - Implement PQ_worker_manager::start/wait/abort/cleanup.
  - Implement Gather_operator::init/destroy/start_workers/gather_rows/
    wait_for_workers/abort_workers/resolve_error_priority.
  - V2-8K-1 worker manager starts joinable no-op worker scaffold threads.
  - No optimizer integration, no execution path change, no handler/InnoDB call.

  Memory ownership (Phase 4):
  - PQ_worker_info array allocated with new[].
  - Exchange_nosort created with new; its init() allocates MQ infrastructure.
  - All allocations freed in Gather_operator::destroy().
  - Phase 5/6 must switch to pq_mem_root for production safety.

  Inert guarantee:
  - No code in this file is reachable from normal SELECT execution.
  - Gather_operator is only constructible via explicit new, and no code
    path in Phase 4 creates one.
  - THD::pq_leader and THD::pq_worker_info remain nullptr unless
    Phase 5 explicitly sets them.
*/

#include "sql/parallel_query/sql_parallel.h"

#include <cassert>
#include <cstring>
#include <limits>
#include <vector>

#include "my_systime.h"
#include "my_dbug.h"
#include "mysql/psi/mysql_thread.h"
#include "mysqld_error.h"         // ER_QUERY_INTERRUPTED
#include "scope_guard.h"          // create_scope_guard
#include "sql/debug_sync.h"       // DEBUG_SYNC
#include "sql/field.h"            // Field
#include "sql/item.h"             // Item_field, Item_int
#include "sql/mysqld.h"           // key_thread_parallel_query_worker
#include "sql/parallel_query/pq_clone.h"
#include "sql/parallel_query/pq_resource_stat.h"
#include "sql/handler.h"          // handler
#include "sql/parallel_query/exchange_sort.h"  // Exchange_sort
#include "sql/parallel_query/pq_iterators.h"   // PQblockScanIterator
#include "sql/parallel_query/query_result_mq.h"  // pq_run_query_result_mq_contract_smoke
#include "sql/sql_base.h"         // close_thread_tables, open_ltable
#include "sql/sql_class.h"        // THD
#include "sql/sql_optimizer.h"    // JOIN
#include "sql/sql_thd_internal_api.h"  // create_internal_thd
#include "sql/table.h"            // TABLE, Table_ref
#include "sql/transaction.h"      // trans_commit_stmt, trans_rollback_stmt

// ---------------------------------------------------------------------------
// PQ_global_stats instance
// ---------------------------------------------------------------------------

PQ_global_stats pq_global_stats;

bool check_pq_running_threads(uint dop, ulong timeout_ms) {
  bool success = false;

  mysql_mutex_lock(&LOCK_pq_threads_running);
  if (parallel_threads_running + dop > parallel_max_threads) {
    if (timeout_ms > 0) {
      struct timespec start_ts;
      struct timespec end_ts;
      struct timespec abstime;
      ulong wait_timeout = timeout_ms;
      int wait_result;

      do {
        set_timespec(&start_ts, 0);
        abstime.tv_sec = start_ts.tv_sec + wait_timeout / TIME_THOUSAND;
        abstime.tv_nsec =
            start_ts.tv_nsec + (wait_timeout % TIME_THOUSAND) * TIME_MILLION;
        if (abstime.tv_nsec >= TIME_BILLION) {
          abstime.tv_sec++;
          abstime.tv_nsec -= TIME_BILLION;
        }

        wait_result = mysql_cond_timedwait(&COND_pq_threads_running,
                                           &LOCK_pq_threads_running, &abstime);
        if (parallel_threads_running + dop <= parallel_max_threads) {
          success = true;
          break;
        }
        if (wait_result != 0) break;

        set_timespec(&end_ts, 0);
        const ulong diff_time =
            (end_ts.tv_sec - start_ts.tv_sec) * TIME_THOUSAND +
            (end_ts.tv_nsec - start_ts.tv_nsec) / TIME_MILLION;
        if (diff_time >= wait_timeout) break;
        wait_timeout -= diff_time;
      } while (wait_timeout > 0);
    }
  } else {
    success = true;
  }

  if (success) {
    parallel_threads_running += dop;
  } else {
    ++parallel_threads_refused;
  }
  mysql_mutex_unlock(&LOCK_pq_threads_running);
  return success;
}

Gather_operator *make_pq_gather_operator(JOIN *join [[maybe_unused]],
                                          uint dop [[maybe_unused]]) {
  return nullptr;
}

PQ_exec_status make_pq_leader_plan(JOIN *join [[maybe_unused]],
                                   THD *thd [[maybe_unused]]) {
  return PQ_exec_status::SEQ_EXEC;
}

PQ_exec_status make_pq_unit_plan(Query_expression *unit [[maybe_unused]],
                                 THD *thd [[maybe_unused]]) {
  return PQ_exec_status::SEQ_EXEC;
}

void *pq_worker_exec(void *arg [[maybe_unused]]) { return nullptr; }

bool pq_make_join_readinfo(JOIN *join, Gather_operator *gather,
                           QEP_TAB *div_tab) {
  (void)join;
  (void)gather;
  (void)div_tab;
  return true;
}

bool pq_check_stable_sort(JOIN *join [[maybe_unused]]) { return false; }

void EstimatePQGatherOperatorCost(AccessPath *path [[maybe_unused]],
                                  THD *thd [[maybe_unused]]) {}

namespace {

struct PQ_worker_thread_arg {
  PQ_worker_info *worker{nullptr};
  Gather_operator *gather{nullptr};
};

bool pq_run_worker_thread_task(PQ_worker_info *worker, Gather_operator *gather);

bool pq_make_join_readinfo_smoke_contract(JOIN *join, Gather_operator *gather,
                                          QEP_TAB *div_tab) {
  /*
    Smoke-only minimum contract: the worker ExecuteIterator probe can prove
    that readinfo reached a well-formed shell, while the exported production
    pq_make_join_readinfo() remains fail-closed until real QEP_TAB/access-path
    construction is migrated.
  */
  return join != nullptr && gather != nullptr && div_tab == nullptr;
}

QEP_TAB *pq_first_worker_smoke_qep_tab(JOIN *join) {
  if (join == nullptr || join->qep_tab == nullptr ||
      join->const_tables >= join->primary_tables) {
    return nullptr;
  }
  QEP_TAB *tab = &join->qep_tab[join->const_tables];
  return tab->table() != nullptr ? tab : nullptr;
}

bool pq_worker_join_ownership_preflight(JOIN *worker_join, JOIN *leader_join) {
  if (worker_join == nullptr || leader_join == nullptr ||
      worker_join->query_block == nullptr ||
      leader_join->query_block == nullptr ||
      worker_join->query_expression() == nullptr ||
      leader_join->query_expression() == nullptr) {
    return false;
  }

  return worker_join->query_block != leader_join->query_block &&
         worker_join->query_expression() != leader_join->query_expression();
}

bool pq_parse_worker_result_uint64(
    const PQ_worker_result_decoded_field &field, uint64 *value) {
  if (value == nullptr || field.is_null || field.value == nullptr ||
      field.value_len == 0) {
    return true;
  }

  uint64 parsed_value = 0;
  for (uint32 i = 0; i < field.value_len; ++i) {
    const char ch = field.value[i];
    if (ch < '0' || ch > '9') return true;
    const uint64 digit = static_cast<uint64>(ch - '0');
    if (parsed_value >
        (std::numeric_limits<uint64>::max() - digit) / 10) {
      return true;
    }
    parsed_value = parsed_value * 10 + digit;
  }

  *value = parsed_value;
  return false;
}

bool pq_checked_mul_uint64(uint64 left, uint64 right, uint64 *result) {
  if (result == nullptr) return true;
  if (left != 0 && right > std::numeric_limits<uint64>::max() / left) {
    return true;
  }

  *result = left * right;
  return false;
}

bool pq_drain_worker_result_frames(PQ_worker_info *worker,
                                   size_t expected_fields,
                                   uint32 max_messages, uint32 *rows_read,
                                   uint32 *finishes_read, uint32 *errors_read,
                                   bool collect_value_checksums,
                                   Exchange_nosort *exchange,
                                   TABLE *materialize_table) {
  if (worker == nullptr || worker->m_mq_handle == nullptr ||
      rows_read == nullptr || finishes_read == nullptr ||
      errors_read == nullptr || expected_fields == 0) {
    return true;
  }

  *rows_read = 0;
  *finishes_read = 0;
  *errors_read = 0;

  if (exchange != nullptr && materialize_table != nullptr) {
    for (uint32 i = 0; i < max_messages && *finishes_read == 0 &&
                       *errors_read == 0;
         ++i) {
      uint64 id_value = 0;
      uint64 v_value = 0;
      Exchange_nosort::Materialize_status status =
          Exchange_nosort::Materialize_status::ERROR;
      if (exchange->materialize_next_worker_result_status(
              materialize_table, &status, &id_value, &v_value)) {
        return true;
      }

      if (status == Exchange_nosort::Materialize_status::ROW) {
        if (expected_fields != 2) return true;
        uint64 id_v_value = 0;
        if (pq_checked_mul_uint64(id_value, v_value, &id_v_value)) {
          return true;
        }

        if (collect_value_checksums) {
          pq_global_stats.worker_execute_iterator_threaded_smoke_value_rows
              .fetch_add(1, std::memory_order_relaxed);
          pq_global_stats.worker_execute_iterator_threaded_smoke_value_id_sum
              .fetch_add(id_value, std::memory_order_relaxed);
          pq_global_stats.worker_execute_iterator_threaded_smoke_value_v_sum
              .fetch_add(v_value, std::memory_order_relaxed);
          pq_global_stats
              .worker_execute_iterator_threaded_smoke_value_id_v_sum
              .fetch_add(id_v_value, std::memory_order_relaxed);
        }

        pq_global_stats.worker_execute_iterator_threaded_smoke_mat_rows
            .fetch_add(1, std::memory_order_relaxed);
        pq_global_stats.worker_execute_iterator_threaded_smoke_mat_id_sum
            .fetch_add(id_value, std::memory_order_relaxed);
        pq_global_stats.worker_execute_iterator_threaded_smoke_mat_v_sum
            .fetch_add(v_value, std::memory_order_relaxed);
        pq_global_stats.worker_execute_iterator_threaded_smoke_mat_id_v_sum
            .fetch_add(id_v_value, std::memory_order_relaxed);
        ++*rows_read;
        continue;
      }

      if (status == Exchange_nosort::Materialize_status::EOF_REACHED) {
        ++*finishes_read;
        break;
      }

      if (status == Exchange_nosort::Materialize_status::ERROR) {
        ++*errors_read;
        break;
      }

      if (worker->is_terminal()) break;
      exchange->wait_for_message(10000);
    }
    return false;
  }

  for (uint32 i = 0; i < max_messages && *finishes_read == 0 &&
                     *errors_read == 0;
       ++i) {
    void *raw_data = nullptr;
    uint32 raw_len = 0;
    const MQ_RESULT receive_result =
        worker->m_mq_handle->receive(&raw_data, &raw_len);
    if (receive_result == MQ_WOULD_BLOCK) {
      if (worker->is_terminal()) break;
      PQ_mq_event *receiver = worker->m_mq_handle->get_receiver();
      if (receiver == nullptr) return true;
      receiver->wait_latch(10000);
      receiver->reset_latch();
      continue;
    }
    if (receive_result != MQ_SUCCESS) return true;

    const PQ_worker_result_frame_header *header = nullptr;
    const uchar *decoded_null_bitmap = nullptr;
    const uchar *decoded_payload = nullptr;
    if (pq_validate_worker_result_frame(raw_data, raw_len, &header,
                                        &decoded_null_bitmap,
                                        &decoded_payload)) {
      return true;
    }

    if (header->type ==
        static_cast<uint16>(PQ_worker_result_message_type::ROW)) {
      if (*finishes_read != 0) return true;
      std::vector<PQ_worker_result_decoded_field> decoded_fields;
      if (pq_decode_worker_result_row(raw_data, raw_len, &decoded_fields) ||
          decoded_fields.size() != expected_fields) {
        return true;
      }
      if (collect_value_checksums) {
        if (expected_fields != 2) {
          pq_global_stats.worker_execute_iterator_threaded_smoke_value_errors
              .fetch_add(1, std::memory_order_relaxed);
          return true;
        }
        uint64 id_value = 0;
        uint64 v_value = 0;
        if (pq_parse_worker_result_uint64(decoded_fields[0], &id_value) ||
            pq_parse_worker_result_uint64(decoded_fields[1], &v_value)) {
          pq_global_stats.worker_execute_iterator_threaded_smoke_value_errors
              .fetch_add(1, std::memory_order_relaxed);
          return true;
        }
        uint64 id_v_value = 0;
        if (pq_checked_mul_uint64(id_value, v_value, &id_v_value)) {
          pq_global_stats.worker_execute_iterator_threaded_smoke_value_errors
              .fetch_add(1, std::memory_order_relaxed);
          return true;
        }
        pq_global_stats.worker_execute_iterator_threaded_smoke_value_rows
            .fetch_add(1, std::memory_order_relaxed);
        pq_global_stats.worker_execute_iterator_threaded_smoke_value_id_sum
            .fetch_add(id_value, std::memory_order_relaxed);
        pq_global_stats.worker_execute_iterator_threaded_smoke_value_v_sum
            .fetch_add(v_value, std::memory_order_relaxed);
        pq_global_stats.worker_execute_iterator_threaded_smoke_value_id_v_sum
            .fetch_add(id_v_value, std::memory_order_relaxed);
      }
      ++*rows_read;
    } else if (header->type ==
               static_cast<uint16>(PQ_worker_result_message_type::FINISH)) {
      if (decoded_null_bitmap != nullptr || decoded_payload != nullptr ||
          header->field_count != 0 || header->null_bitmap_len != 0 ||
          header->payload_len != 0) {
        return true;
      }
      ++*finishes_read;
      return false;
    } else if (header->type ==
               static_cast<uint16>(PQ_worker_result_message_type::ERROR)) {
      ++*errors_read;
      return false;
    } else {
      return true;
    }
  }

  return *finishes_read != 1 || *errors_read != 0;
}

class PQ_worker_execute_smoke_plan {
 public:
  bool create(PQ_worker_info *worker, Gather_operator *gather, JOIN *leader_join) {
    m_worker = worker;
    m_gather = gather;

    m_worker_thd = pq_create_worker_thd(worker, gather);
    if (m_worker_thd == nullptr) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_worker_open
          .fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    m_owns_worker_thd = true;

    return create_join(leader_join);
  }

  bool create_with_borrowed_worker_thd(PQ_worker_info *worker,
                                       Gather_operator *gather,
                                       JOIN *leader_join) {
    m_worker = worker;
    m_gather = gather;
    m_worker_thd = worker != nullptr ? worker->m_worker_thd : nullptr;
    if (worker == nullptr || gather == nullptr || m_worker_thd == nullptr ||
        !m_worker_thd->pq_is_worker ||
        m_worker_thd->pq_worker_info != worker ||
        m_worker_thd->pq_leader != gather || m_worker_thd->lex == nullptr) {
      pq_global_stats.worker_execute_iterator_threaded_smoke_blocked_setup
          .fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    m_owns_worker_thd = false;

    return create_join(leader_join);
  }

 private:
  bool create_join(JOIN *leader_join) {
    if (m_worker_thd == nullptr) return false;

    m_worker_join = pq_make_join(m_worker_thd, leader_join);
    if (m_worker_join == nullptr) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_clone.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    if (!pq_make_join_readinfo_smoke_contract(m_worker_join, m_gather,
                                              nullptr)) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_readinfo.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    if (pq_dup_tabs_skeleton_preflight(m_worker_join, leader_join)) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_readinfo.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    pq_global_stats.worker_execute_iterator_smoke_plan_constructed.fetch_add(
        1, std::memory_order_relaxed);
    m_constructed = true;
    return true;
  }

 public:
  bool bind_result() {
    auto *mq_result = new (m_worker_thd->mem_root)
        Query_result_mq(m_worker_join, m_worker->m_mq_handle, false);
    if (mq_result == nullptr ||
        mq_result->get_mq_handler() != m_worker->m_mq_handle ||
        m_worker_join->query_expression() == nullptr ||
        m_worker_join->query_block == nullptr) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_result.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    m_original_expression_result =
        m_worker_join->query_expression()->query_result();
    m_original_block_result = m_worker_join->query_block->query_result();
    m_worker_join->query_expression()->set_query_result(mq_result);
    m_worker_join->query_block->set_query_result(mq_result);
    m_mq_result = mq_result;
    m_result_bound = true;

    if (m_worker_join->query_expression()->query_result() != mq_result ||
        m_worker_join->query_block->query_result() != mq_result) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_result.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    pq_global_stats.worker_execute_iterator_smoke_result_bound.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  bool send_current_row_result() {
    if (!m_result_bound || m_mq_result == nullptr || m_worker_thd == nullptr ||
        m_worker == nullptr || m_worker->m_mq_handle == nullptr ||
        m_worker_join == nullptr || m_worker_join->query_block == nullptr) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_result_send.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    mem_root_deque<Item *> *const fields = m_worker_join->fields;
    if (fields == nullptr || fields != &m_worker_join->query_block->fields ||
        fields->empty()) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_result_send.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    const ha_rows sent_rows_before = m_worker_thd->get_sent_row_count();
    bool failed = m_mq_result->start_execution(m_worker_thd) ||
                  m_mq_result->send_result_set_metadata(m_worker_thd, *fields,
                                                        0);
    if (!failed && m_mq_result->result_contract_ready()) {
      pq_global_stats.worker_execute_iterator_smoke_result_contract_ready
          .fetch_add(1, std::memory_order_relaxed);
    } else {
      pq_global_stats.worker_execute_iterator_smoke_blocked_result_contract
          .fetch_add(1, std::memory_order_relaxed);
    }
    failed = failed || !m_mq_result->result_contract_ready() ||
             m_mq_result->send_data(m_worker_thd, *fields) ||
             m_mq_result->send_eof(m_worker_thd);
    m_worker_thd->set_sent_row_count(sent_rows_before);

    uint32 rows_read = 0;
    uint32 finishes_read = 0;
    failed = drain_worker_result_frames(2, fields->size(), &rows_read,
                                        &finishes_read);

    if (failed || rows_read != 1 || finishes_read != 1) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_result_send.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    pq_global_stats.worker_execute_iterator_smoke_result_row_sent.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.worker_execute_iterator_smoke_result_eof_sent.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  bool record_execute_iterator_query_blocker() {
    if (!prepare_execute_iterator_unit_state() ||
        !preflight_execute_iterator_query()) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_execute.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    /*
      ExecuteIteratorQuery() produces all rows synchronously. Until the smoke
      runs in a real worker thread, the leader side cannot drain MQ frames
      concurrently, so the direct call stays intentionally blocked.
    */
    pq_global_stats.worker_execute_iterator_smoke_blocked_execute_no_consumer
        .fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  bool preflight_execute_iterator_query() {
    pq_global_stats.worker_execute_iterator_smoke_execute_preflight_attempts
        .fetch_add(1, std::memory_order_relaxed);

    Query_expression *const unit =
        m_worker_join != nullptr ? m_worker_join->query_expression() : nullptr;
    Query_block *const block =
        m_worker_join != nullptr ? m_worker_join->query_block : nullptr;

    const bool thd_ready =
        m_worker_thd != nullptr && m_worker_thd->pq_is_worker &&
        m_worker_thd->pq_worker_info == m_worker && m_worker_thd->lex != nullptr;
    if (thd_ready) {
      pq_global_stats.worker_execute_iterator_smoke_execute_preflight_thd_ready
          .fetch_add(1, std::memory_order_relaxed);
    }

    const bool root_ready =
        unit != nullptr && unit->root_access_path() != nullptr &&
        unit->root_iterator() != nullptr &&
        unit->root_access_path()->type == AccessPath::PQ_BLOCK_SCAN;
    if (root_ready) {
      pq_global_stats.worker_execute_iterator_smoke_execute_preflight_root_ready
          .fetch_add(1, std::memory_order_relaxed);
    }

    const bool result_ready =
        m_result_bound && m_mq_result != nullptr && m_worker != nullptr &&
        m_worker->m_mq_handle != nullptr && unit != nullptr && block != nullptr &&
        unit->query_result() == m_mq_result &&
        block->query_result() == m_mq_result &&
        m_mq_result->get_mq_handler() == m_worker->m_mq_handle;
    if (result_ready) {
      pq_global_stats
          .worker_execute_iterator_smoke_execute_preflight_result_ready
          .fetch_add(1, std::memory_order_relaxed);
    }

    const bool fields_ready =
        block != nullptr && m_worker_join != nullptr &&
        m_worker_join->fields == &block->fields && !block->fields.empty();
    if (fields_ready) {
      pq_global_stats
          .worker_execute_iterator_smoke_execute_preflight_fields_ready
          .fetch_add(1, std::memory_order_relaxed);
    }

    const bool unit_ready =
        unit != nullptr && unit->is_simple() && unit->is_prepared() &&
        unit->is_optimized() && !unit->is_executed() &&
        !unit->unfinished_materialization();
    if (unit_ready) {
      pq_global_stats.worker_execute_iterator_smoke_execute_preflight_unit_ready
          .fetch_add(1, std::memory_order_relaxed);
    } else {
      pq_global_stats
          .worker_execute_iterator_smoke_execute_preflight_blocked_unit
          .fetch_add(1, std::memory_order_relaxed);
    }

    return thd_ready && root_ready && result_ready && fields_ready && unit_ready;
  }

  bool prepare_execute_iterator_unit_state() {
    Query_expression *const unit =
        m_worker_join != nullptr ? m_worker_join->query_expression() : nullptr;
    Query_block *const block =
        m_worker_join != nullptr ? m_worker_join->query_block : nullptr;

    if (m_worker_thd == nullptr || m_worker_join == nullptr || unit == nullptr ||
        block == nullptr || m_worker_thd->lex == nullptr ||
        m_worker_thd->lex->unit != unit || unit->first_query_block() != block ||
        block->join != m_worker_join || !unit->is_simple() ||
        unit->is_executed() || unit->unfinished_materialization()) {
      return false;
    }

    if (!unit->is_prepared()) unit->set_prepared();
    if (!m_worker_join->is_optimized()) m_worker_join->set_optimized();
    if (!unit->is_optimized()) unit->set_optimized();

    m_unit_marked_execute_ready = true;
    return unit->is_prepared() && unit->is_optimized() &&
           m_worker_join->is_optimized() && !unit->is_executed();
  }

  void cleanup(bool close_worker_table, bool table_error) {
    restore_result();

    if (m_worker_join != nullptr) {
      Query_expression *const unit = m_worker_join->query_expression();
      m_worker_join->pq_restore();
      if (m_unit_marked_execute_ready && unit != nullptr) {
        unit->cleanup(true);
      } else {
        m_worker_join->destroy();
      }
      m_worker_join = nullptr;
    }
    if (close_worker_table && m_worker != nullptr) {
      pq_close_worker_table(&m_worker->m_open_ctx, table_error);
    }
    if (m_owns_worker_thd && m_worker != nullptr && m_worker_thd != nullptr) {
      pq_destroy_worker_thd(m_worker);
      m_worker_thd = nullptr;
    }

    if (m_constructed) {
      pq_global_stats.worker_execute_iterator_smoke_plan_cleaned.fetch_add(
          1, std::memory_order_relaxed);
      m_constructed = false;
    }
  }

  THD *worker_thd() const { return m_worker_thd; }
  JOIN *worker_join() const { return m_worker_join; }
  Query_result_mq *mq_result() const { return m_mq_result; }

 private:
  bool drain_worker_result_frames(uint32 max_messages, size_t expected_fields,
                                  uint32 *rows_read,
                                  uint32 *finishes_read) {
    if (m_worker == nullptr || m_worker->m_mq_handle == nullptr ||
        rows_read == nullptr || finishes_read == nullptr) {
      return true;
    }

    *rows_read = 0;
    *finishes_read = 0;
    for (uint32 i = 0; i < max_messages; ++i) {
      void *raw_data = nullptr;
      uint32 raw_len = 0;
      if (m_worker->m_mq_handle->receive(&raw_data, &raw_len) != MQ_SUCCESS) {
        return true;
      }

      const PQ_worker_result_frame_header *header = nullptr;
      const uchar *decoded_null_bitmap = nullptr;
      const uchar *decoded_payload = nullptr;
      if (pq_validate_worker_result_frame(raw_data, raw_len, &header,
                                          &decoded_null_bitmap,
                                          &decoded_payload)) {
        return true;
      }

      if (header->type ==
          static_cast<uint16>(PQ_worker_result_message_type::ROW)) {
        std::vector<PQ_worker_result_decoded_field> decoded_fields;
        if (pq_decode_worker_result_row(raw_data, raw_len, &decoded_fields) ||
            decoded_fields.empty() || decoded_fields.size() != expected_fields) {
          return true;
        }
        ++*rows_read;
      } else if (header->type ==
                 static_cast<uint16>(PQ_worker_result_message_type::FINISH)) {
        if (decoded_null_bitmap != nullptr || decoded_payload != nullptr ||
            header->field_count != 0 || header->null_bitmap_len != 0 ||
            header->payload_len != 0) {
          return true;
        }
        ++*finishes_read;
        return false;
      } else {
        return true;
      }
    }
    return true;
  }

  void restore_result() {
    if (!m_result_bound || m_worker_join == nullptr ||
        m_worker_join->query_expression() == nullptr ||
        m_worker_join->query_block == nullptr) {
      return;
    }
    if (m_mq_result != nullptr) m_mq_result->cleanup();
    m_worker_join->query_expression()->set_query_result(
        m_original_expression_result);
    m_worker_join->query_block->set_query_result(m_original_block_result);
    m_mq_result = nullptr;
    pq_global_stats.worker_execute_iterator_smoke_result_restored.fetch_add(
        1, std::memory_order_relaxed);
    m_result_bound = false;
  }

  PQ_worker_info *m_worker{nullptr};
  Gather_operator *m_gather{nullptr};
  THD *m_worker_thd{nullptr};
  JOIN *m_worker_join{nullptr};
  Query_result *m_original_expression_result{nullptr};
  Query_result *m_original_block_result{nullptr};
  Query_result_mq *m_mq_result{nullptr};
  bool m_result_bound{false};
  bool m_constructed{false};
  bool m_unit_marked_execute_ready{false};
  bool m_owns_worker_thd{false};
};

void *pq_worker_thread_entry(void *arg_ptr) {
  auto *arg = static_cast<PQ_worker_thread_arg *>(arg_ptr);
  PQ_worker_info *worker = arg != nullptr ? arg->worker : nullptr;
  Gather_operator *gather = arg != nullptr ? arg->gather : nullptr;
  delete arg;

  if (worker == nullptr || gather == nullptr) return nullptr;

  if (my_thread_init()) {
    worker->m_error_code = HA_ERR_OUT_OF_MEM;
    worker->transition_status(PQ_Worker_status::RUNNING);
    worker->transition_status(PQ_Worker_status::ERROR);
    return nullptr;
  }

  worker->transition_status(PQ_Worker_status::RUNNING);
  THD *worker_thd = pq_create_worker_thd(worker, gather);
  if (worker_thd == nullptr) {
    worker->m_error_code = HA_ERR_OUT_OF_MEM;
    worker->transition_status(PQ_Worker_status::ERROR);
    my_thread_end();
    return nullptr;
  }

  const bool failed = pq_run_worker_thread_task(worker, gather);
  pq_destroy_worker_thd(worker);
  if (failed) {
    if (worker->m_error_code == 0) worker->m_error_code = HA_ERR_INTERNAL_ERROR;
    worker->transition_status(PQ_Worker_status::ERROR);
  } else {
    worker->transition_status(PQ_Worker_status::FINISHED);
  }
  my_thread_end();
  return nullptr;
}

}  // namespace

const char *pq_execution_state_to_string(PQ_execution_state state) {
  switch (state) {
    case PQ_execution_state::DISABLED:
      return "disabled";
    case PQ_execution_state::NOT_ELIGIBLE:
      return "not_eligible";
    case PQ_execution_state::ELIGIBLE:
      return "eligible";
    case PQ_execution_state::ITERATOR_SELECTED:
      return "iterator_selected";
    case PQ_execution_state::EXECUTED:
      return "executed";
    case PQ_execution_state::FALLBACK_SERIAL:
      return "fallback_serial";
  }
  return "unknown";
}

uint pq_execution_state_to_uint(PQ_execution_state state) {
  return static_cast<uint>(state);
}

PQ_execution_state pq_execution_state_from_uint(uint state) {
  switch (static_cast<PQ_execution_state>(state)) {
    case PQ_execution_state::DISABLED:
    case PQ_execution_state::NOT_ELIGIBLE:
    case PQ_execution_state::ELIGIBLE:
    case PQ_execution_state::ITERATOR_SELECTED:
    case PQ_execution_state::EXECUTED:
    case PQ_execution_state::FALLBACK_SERIAL:
      return static_cast<PQ_execution_state>(state);
  }
  return PQ_execution_state::DISABLED;
}

void pq_set_execution_state(THD *thd, PQ_execution_state state) {
  if (thd == nullptr) return;
  thd->pq_execution_state = pq_execution_state_to_uint(state);
  thd->pq_executed = (state == PQ_execution_state::EXECUTED);
}

bool pq_open_worker_table(PQ_Worker_open_context *open_ctx) {
  if (open_ctx == nullptr || open_ctx->worker_thd == nullptr ||
      open_ctx->leader_table == nullptr || open_ctx->leader_table->s == nullptr ||
      open_ctx->worker_table != nullptr || open_ctx->worker_handler != nullptr) {
    return true;
  }

  THD *worker_thd = open_ctx->worker_thd;
  TABLE *leader_table = open_ctx->leader_table;
  TABLE_SHARE *share = leader_table->s;
  const char *alias = share->table_name.str;
  if (leader_table->pos_in_table_list != nullptr &&
      leader_table->pos_in_table_list->alias != nullptr) {
    alias = leader_table->pos_in_table_list->alias;
  }

  auto *worker_ref = new (worker_thd->mem_root)
      Table_ref(share->db.str, share->db.length, share->table_name.str,
                share->table_name.length, alias, TL_READ, MDL_SHARED_READ);
  if (worker_ref == nullptr) return true;

  TABLE *worker_table = open_ltable(worker_thd, worker_ref, TL_READ, 0);
  if (worker_table == nullptr || worker_table->file == nullptr ||
      worker_table == leader_table || worker_table->file == leader_table->file ||
      worker_table->record[0] == nullptr ||
      worker_table->record[0] == leader_table->record[0]) {
    trans_rollback_stmt(worker_thd);
    close_thread_tables(worker_thd);
    worker_thd->mdl_context.release_transactional_locks();
    return true;
  }

  open_ctx->worker_table = worker_table;
  bitmap_copy(worker_table->read_set, leader_table->read_set);
  bitmap_copy(worker_table->write_set, leader_table->write_set);
  worker_table->column_bitmaps_set_no_signal(worker_table->read_set,
                                             worker_table->write_set);
  open_ctx->worker_handler = worker_table->file;
  return false;
}

void pq_close_worker_table(PQ_Worker_open_context *open_ctx,
                           bool statement_error) {
  if (open_ctx == nullptr || open_ctx->worker_thd == nullptr) return;

  THD *worker_thd = open_ctx->worker_thd;
  if (statement_error) {
    trans_rollback_stmt(worker_thd);
  } else {
    trans_commit_stmt(worker_thd);
  }
  close_thread_tables(worker_thd);
  worker_thd->mdl_context.release_transactional_locks();
  open_ctx->worker_table = nullptr;
  open_ctx->worker_handler = nullptr;
}

THD *pq_create_worker_thd(PQ_worker_info *worker, Gather_operator *gather) {
  if (worker == nullptr || gather == nullptr || worker->m_worker_thd != nullptr) {
    return nullptr;
  }

  THD *worker_thd = create_internal_thd();
  if (worker_thd == nullptr) return nullptr;

  worker_thd->pq_is_worker = true;
  worker_thd->pq_leader = gather;
  worker_thd->pq_worker_info = worker;
  worker_thd->pq_dop = gather->dop();

  worker->m_worker_thd = worker_thd;
  worker->m_open_ctx.worker_thd = worker_thd;
  return worker_thd;
}

void pq_destroy_worker_thd(PQ_worker_info *worker) {
  if (worker == nullptr || worker->m_worker_thd == nullptr) return;

  THD *worker_thd = worker->m_worker_thd;
  if (worker->m_open_ctx.worker_table != nullptr ||
      worker->m_open_ctx.worker_handler != nullptr) {
    pq_close_worker_table(&worker->m_open_ctx, true);
  }

  worker_thd->pq_is_worker = false;
  worker_thd->pq_leader = nullptr;
  worker_thd->pq_worker_info = nullptr;
  worker_thd->pq_dop = 0;
  destroy_internal_thd(worker_thd);

  worker->m_worker_thd = nullptr;
  worker->reset_open_context();
}

// ---------------------------------------------------------------------------
// PQ_worker_info: transition_status
// ---------------------------------------------------------------------------

void PQ_worker_info::reset_open_context() {
  m_open_ctx = PQ_Worker_open_context{};
  m_open_ctx.worker_thd = m_worker_thd;
  m_open_ctx.worker_id = m_worker_id;
  m_open_ctx.mq_handle = m_mq_handle;
}

bool PQ_worker_info::transition_status(PQ_Worker_status new_status) {
  // Forward-only transition rules:
  // NOT_STARTED -> RUNNING -> FINISHED/ERROR/KILLED/ABORTED
  // Any -> ABORTED (abort can override any state except terminal)
  // Terminal states are final: no further transitions allowed.
  PQ_Worker_status current = m_status.load(std::memory_order_acquire);
  for (;;) {
    bool allowed = false;
    switch (current) {
      case PQ_Worker_status::NOT_STARTED:
        // NOT_STARTED can transition to RUNNING or ABORTED.
        allowed = new_status == PQ_Worker_status::RUNNING ||
                  new_status == PQ_Worker_status::ABORTED;
        break;

      case PQ_Worker_status::RUNNING:
        // RUNNING can transition to FINISHED, ERROR, KILLED, or ABORTED.
        allowed = new_status == PQ_Worker_status::FINISHED ||
                  new_status == PQ_Worker_status::ERROR ||
                  new_status == PQ_Worker_status::KILLED ||
                  new_status == PQ_Worker_status::ABORTED;
        break;

      default:
        allowed = false;
        break;
    }

    if (!allowed) return false;
    if (m_status.compare_exchange_weak(current, new_status,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      return true;
    }
  }
}

// ---------------------------------------------------------------------------
// PQ_worker_manager: stub implementations
// ---------------------------------------------------------------------------

bool PQ_worker_manager::start(PQ_worker_info **workers, uint32 n_workers,
                              THD *leader_thd [[maybe_unused]],
                              Gather_operator *gather) {
  assert(workers != nullptr);
  assert(n_workers > 0);

  for (uint32 i = 0; i < n_workers; i++) {
    if (workers[i] == nullptr) return true;  // Invalid worker info
    if (workers[i]->m_thread_started || workers[i]->m_thread_joined) {
      return true;
    }

    auto *arg = new (std::nothrow) PQ_worker_thread_arg{workers[i], gather};
    if (arg == nullptr) {
      abort(workers, i);
      wait(workers, i, leader_thd);
      return true;
    }

    if (mysql_thread_create(key_thread_parallel_query_worker,
                            &workers[i]->m_thread_handle, nullptr,
                            pq_worker_thread_entry, arg)) {
      delete arg;
      abort(workers, i);
      wait(workers, i, leader_thd);
      return true;
    }
    workers[i]->m_thread_started = true;
  }

  return false;  // Success
}

int PQ_worker_manager::wait(PQ_worker_info **workers, uint32 n_workers,
                            THD *leader_thd [[maybe_unused]]) {
  assert(workers != nullptr);
  assert(n_workers > 0);

  for (uint32 i = 0; i < n_workers; i++) {
    if (workers[i] == nullptr) continue;
    if (workers[i]->m_thread_started && !workers[i]->m_thread_joined) {
      if (my_thread_join(&workers[i]->m_thread_handle, nullptr) != 0) {
        workers[i]->m_error_code = HA_ERR_INTERNAL_ERROR;
        workers[i]->transition_status(PQ_Worker_status::ERROR);
        return -1;
      }
      workers[i]->m_thread_joined = true;
    } else if (!workers[i]->is_terminal()) {
      workers[i]->transition_status(PQ_Worker_status::FINISHED);
    }
    const PQ_Worker_status status =
        workers[i]->m_status.load(std::memory_order_acquire);
    if (status == PQ_Worker_status::ERROR ||
        status == PQ_Worker_status::KILLED ||
        status == PQ_Worker_status::ABORTED) {
      return -1;
    }
  }

  return 0;
}

void PQ_worker_manager::abort(PQ_worker_info **workers, uint32 n_workers) {
  if (workers == nullptr || n_workers == 0) return;

  // Phase 4 stub: transition all non-terminal workers to ABORTED.
  // In production (Phase 5), this will also:
  // - Send ABORT control token to each worker's MQ
  // - Set THD::killed on each worker THD
  // - Wait briefly for workers to acknowledge abort
  for (uint32 i = 0; i < n_workers; i++) {
    if (workers[i] == nullptr) continue;

    if (!workers[i]->is_terminal()) {
      workers[i]->transition_status(PQ_Worker_status::ABORTED);
    }
  }
}

void PQ_worker_manager::cleanup(PQ_worker_info **workers, uint32 n_workers,
                                bool free_array) {
  if (workers == nullptr) return;

  // Free individual PQ_worker_info structs
  for (uint32 i = 0; i < n_workers; i++) {
    if (workers[i] != nullptr) {
      if (workers[i]->m_thread_started && !workers[i]->m_thread_joined) {
        (void)my_thread_join(&workers[i]->m_thread_handle, nullptr);
        workers[i]->m_thread_joined = true;
      }
      if (workers[i]->m_worker_thd != nullptr) {
        pq_destroy_worker_thd(workers[i]);
      }
      delete workers[i];
      workers[i] = nullptr;
    }
  }

  // Free the array itself if requested
  if (free_array) {
    delete[] workers;
  }
}

// ---------------------------------------------------------------------------
// Gather_operator: init / destroy
// ---------------------------------------------------------------------------

Gather_operator::~Gather_operator() { destroy(); }

bool Gather_operator::init() {
  if (m_dop == 0) return true;  // Invalid DOP
  if (m_initialized) return false;  // Already initialized (no-op success)

  // Allocate worker info array.
  // RISK: uses new[] instead of pq_mem_root. Phase 5 must switch.
  m_workers = new PQ_worker_info *[m_dop];
  if (m_workers == nullptr) goto err;

  for (uint32 i = 0; i < m_dop; i++) {
    m_workers[i] = new PQ_worker_info(i);
    if (m_workers[i] == nullptr) goto err;

    // Wire MQ handle to Exchange (created below).
    // The handle pointer will be set after Exchange init.
  }

  // Create Exchange_nosort for round-robin row collection.
  // RISK: uses new instead of pq_mem_root. Phase 5 must switch.
  m_exchange = new Exchange_nosort(m_dop, m_ring_size);
  if (m_exchange == nullptr) goto err;

  // Initialize Exchange MQ infrastructure.
  if (m_exchange->init()) goto err;

  // Wire each worker's MQ handle to the Exchange handle for that worker.
  for (uint32 i = 0; i < m_dop; i++) {
    m_workers[i]->m_mq_handle = m_exchange->get_mq_handle(i);
    m_workers[i]->reset_open_context();
    m_workers[i]->m_open_ctx.actual_dop = m_dop;
  }

  m_initialized = true;
  m_all_finished = false;
  m_error_state.reset();

  return false;  // Success

err:
  // Cleanup partially allocated resources.
  destroy();
  return true;  // Failure
}

bool Gather_operator::configure_worker_open_contexts(
    TABLE *leader_table, PQ_Leader_context *leader_ctx, uint actual_dop) {
  if (!m_initialized || m_workers == nullptr || m_exchange == nullptr ||
      leader_table == nullptr || leader_ctx == nullptr || actual_dop == 0 ||
      actual_dop != m_dop) {
    return true;
  }

  for (uint32 i = 0; i < m_dop; i++) {
    auto *worker = m_workers[i];
    if (worker == nullptr) return true;

    worker->reset_open_context();
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.leader_ctx = leader_ctx;
    worker->m_open_ctx.actual_dop = actual_dop;
    worker->m_open_ctx.mq_handle = worker->m_mq_handle;
  }

  return false;
}

void Gather_operator::destroy() {
  // Clean up worker info array.
  // Note: PQ_worker_manager::cleanup handles the per-worker cleanup.
  if (m_workers != nullptr) {
    m_worker_mgr.cleanup(m_workers, m_dop, true);
    m_workers = nullptr;
  }

  if (m_thread_budget_acquired) {
    release_pq_running_threads(m_dop);
    m_thread_budget_acquired = false;
  }

  // Clean up Exchange_nosort.
  if (m_exchange != nullptr) {
    m_exchange->cleanup();
    delete m_exchange;
    m_exchange = nullptr;
  }

  m_initialized = false;
  m_all_finished = false;
  m_error_state.reset();
}

// ---------------------------------------------------------------------------
// Gather_operator: start_workers (stub)
// ---------------------------------------------------------------------------

bool Gather_operator::start_workers(THD *leader_thd) {
  if (!m_initialized) return true;  // Not initialized

  // Phase 4 stub: delegate to PQ_worker_manager::start().
  // In production (Phase 5), this will also:
  // - Create worker THDs
  // - Launch worker threads
  // - Wire worker THD::pq_worker_info to PQ_worker_info
  // - Wire worker THD::pq_leader to this Gather_operator
  return m_worker_mgr.start(m_workers, m_dop, leader_thd, this);
}

// ---------------------------------------------------------------------------
// Gather_operator: gather_rows (stub)
// ---------------------------------------------------------------------------

bool Gather_operator::gather_rows(MQMessageType &type, void **datap,
                                  uint32 &data_len) {
  if (!m_initialized || m_all_finished) {
    type = MQMessageType::FINISH;
    *datap = nullptr;
    data_len = 0;
    return false;
  }

  // Phase 4 stub: call Exchange_nosort::read_mq_message() but no real
  // workers are producing data, so it will return false (no data).
  // In production (Phase 5+), this will loop reading rows until all
  // workers finish, converting MQ data to record[0] format.
  bool result = m_exchange->read_mq_message(type, datap, data_len);

  // Check for all-done state
  if (!result && type == MQMessageType::FINISH) {
    m_all_finished = true;
  }

  return result;
}

// ---------------------------------------------------------------------------
// Gather_operator: wait_for_workers (stub)
// ---------------------------------------------------------------------------

int Gather_operator::wait_for_workers(THD *leader_thd) {
  if (!m_initialized) return -1;  // Not initialized

  // Phase 4 stub: delegate to PQ_worker_manager::wait().
  // Production will block on real worker completion events.
  int result = m_worker_mgr.wait(m_workers, m_dop, leader_thd);

  if (result == 0) {
    m_all_finished = true;
  }

  return result;
}

bool Gather_operator::run_worker_lifecycle_smoke(THD *leader_thd) {
  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  if (start_workers(leader_thd)) {
    if (initialized_here) destroy();
    return true;
  }

  int wait_result = wait_for_workers(leader_thd);
  auto error_state = resolve_error_priority(leader_thd);
  if (wait_result != 0 || error_state.has_error()) {
    if (initialized_here) destroy();
    return true;
  }

  pq_global_stats.worker_smoke_runs.fetch_add(1, std::memory_order_relaxed);

  if (initialized_here) destroy();
  return false;
}

bool Gather_operator::run_worker_producer_loop_smoke(
    THD *leader_thd [[maybe_unused]], TABLE *leader_table) {
  if (leader_table == nullptr || m_dop != 1) return true;

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  auto *exchange = get_exchange();
  if (worker == nullptr || exchange == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (!worker->transition_status(PQ_Worker_status::RUNNING)) {
    if (initialized_here) destroy();
    return true;
  }

  bool eof = false;
  bool row = false;
  bool failed = exchange->enqueue_finish_smoke(0) ||
                exchange->materialize_next_record_image(leader_table, &eof,
                                                        &row) ||
                !eof || row ||
                !worker->transition_status(PQ_Worker_status::FINISHED);

  if (!failed) {
    pq_global_stats.worker_producer_smoke_runs.fetch_add(
        1, std::memory_order_relaxed);
  }

  if (initialized_here) destroy();
  return failed;
}

bool Gather_operator::run_worker_producer_error_smoke(
    THD *leader_thd [[maybe_unused]], TABLE *leader_table) {
  if (leader_table == nullptr || m_dop != 1) return true;

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  auto *exchange = get_exchange();
  if (worker == nullptr || exchange == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (!worker->transition_status(PQ_Worker_status::RUNNING)) {
    if (initialized_here) destroy();
    return true;
  }

  bool eof = false;
  bool row = false;
  bool got_expected_error =
      !exchange->enqueue_error_smoke(0) &&
      exchange->materialize_next_record_image(leader_table, &eof, &row);
  worker->m_error_code = 1;
  bool failed = !got_expected_error ||
                !worker->transition_status(PQ_Worker_status::ERROR);

  if (!failed) {
    pq_global_stats.worker_producer_smoke_runs.fetch_add(
        1, std::memory_order_relaxed);
  }

  if (initialized_here) destroy();
  return failed;
}

bool Gather_operator::run_worker_producer_abort_smoke(
    THD *leader_thd, TABLE *leader_table) {
  if (leader_table == nullptr || m_dop != 1) return true;

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  auto *exchange = get_exchange();
  if (worker == nullptr || exchange == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (!worker->transition_status(PQ_Worker_status::RUNNING)) {
    if (initialized_here) destroy();
    return true;
  }

  abort_workers(leader_thd);

  bool eof = false;
  bool row = false;
  MQueue_handle *handle = exchange->get_mq_handle(0);
  const bool observed_eof =
      !exchange->materialize_next_record_image(leader_table, &eof, &row) &&
      eof && !row;
  const bool detached = handle != nullptr && handle->is_detached();
  const bool failed = worker->m_status.load(std::memory_order_acquire) !=
                          PQ_Worker_status::ABORTED ||
                      !detached || !observed_eof || !m_all_finished;

  if (!failed) {
    pq_global_stats.worker_producer_smoke_runs.fetch_add(
        1, std::memory_order_relaxed);
  }

  if (initialized_here) destroy();
  return failed;
}

bool Gather_operator::run_exchange_row_stream_smoke(THD *leader_thd
                                                    [[maybe_unused]]) {
  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *exchange = get_exchange();
  if (exchange == nullptr ||
      exchange->get_exchange_type() != Exchange::EXCHANGE_NOSORT) {
    if (initialized_here) destroy();
    return true;
  }

  uint32 rows_read = 0;
  uint32 finishes_read = 0;
  auto *nosort = static_cast<Exchange_nosort *>(exchange);
  bool failed =
      nosort->run_synthetic_row_stream_smoke(&rows_read, &finishes_read);
  if (failed) {
    if (initialized_here) destroy();
    return true;
  }

  pq_global_stats.exchange_smoke_rows.fetch_add(rows_read,
                                                std::memory_order_relaxed);
  pq_global_stats.exchange_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);

  if (initialized_here) destroy();
  return false;
}

bool Gather_operator::run_exchange_row_image_smoke(THD *leader_thd
                                                   [[maybe_unused]],
                                                   TABLE *table) {
  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *exchange = get_exchange();
  if (exchange == nullptr ||
      exchange->get_exchange_type() != Exchange::EXCHANGE_NOSORT) {
    if (initialized_here) destroy();
    return true;
  }

  uint32 rows_read = 0;
  uint32 finishes_read = 0;
  auto *nosort = static_cast<Exchange_nosort *>(exchange);
  bool failed =
      nosort->run_synthetic_row_image_smoke(table, &rows_read, &finishes_read);
  if (failed) {
    if (initialized_here) destroy();
    return true;
  }

  pq_global_stats.exchange_smoke_rows.fetch_add(rows_read,
                                                std::memory_order_relaxed);
  pq_global_stats.exchange_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);
  pq_global_stats.exchange_row_image_smoke_rows.fetch_add(
      rows_read, std::memory_order_relaxed);
  pq_global_stats.exchange_row_image_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);

  DBUG_EXECUTE_IF("pq_exchange_worker_result_smoke", {
    Exchange_nosort worker_result_exchange(2, PQ_MQ_DEFAULT_RING_SIZE);
    uint32 worker_result_rows = 0;
    uint32 worker_result_finishes = 0;
    if (worker_result_exchange.init() ||
        worker_result_exchange.run_synthetic_worker_result_smoke(
            leader_thd, table, &worker_result_rows, &worker_result_finishes)) {
      worker_result_exchange.cleanup();
      if (initialized_here) destroy();
      return true;
    }
    worker_result_exchange.cleanup();
    pq_global_stats.worker_result_smoke_rows.fetch_add(
        worker_result_rows, std::memory_order_relaxed);
    pq_global_stats.worker_result_smoke_finishes.fetch_add(
        worker_result_finishes, std::memory_order_relaxed);
  });

  if (initialized_here) destroy();
  return false;
}

bool Gather_operator::run_exchange_partial_group_smoke(
    THD *leader_thd [[maybe_unused]]) {
  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *exchange = get_exchange();
  if (exchange == nullptr ||
      exchange->get_exchange_type() != Exchange::EXCHANGE_NOSORT) {
    if (initialized_here) destroy();
    return true;
  }

  uint32 groups_read = 0;
  uint32 finishes_read = 0;
  auto *nosort = static_cast<Exchange_nosort *>(exchange);
  const bool failed = nosort->run_synthetic_partial_group_smoke(
      &groups_read, &finishes_read);
  if (failed) {
    if (initialized_here) destroy();
    return true;
  }

  uint32 payload_errors = 0;
  if (nosort->run_synthetic_partial_group_malformed_smoke(&payload_errors)) {
    if (initialized_here) destroy();
    return true;
  }

  pq_global_stats.exchange_partial_group_smoke_rows.fetch_add(
      groups_read, std::memory_order_relaxed);
  pq_global_stats.exchange_partial_group_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);
  pq_global_stats.groupby_partial_payload_errors.fetch_add(
      payload_errors, std::memory_order_relaxed);

  if (initialized_here) destroy();
  return false;
}

bool Gather_operator::run_exchange_sort_smoke(THD *leader_thd [[maybe_unused]],
                                              TABLE *leader_table) {
  Exchange_sort sort_exchange(3, PQ_MQ_DEFAULT_RING_SIZE);

  uint32 rows_read = 0;
  if (sort_exchange.run_synthetic_order_merge_smoke(&rows_read)) {
    return true;
  }
  uint32 cached_rows_read = 0;
  if (sort_exchange.run_cached_record_adapter_smoke(&cached_rows_read)) {
    return true;
  }
  uint32 frame_rows_read = 0;
  uint32 frame_finishes_read = 0;
  uint32 frame_errors_read = 0;
  if (sort_exchange.run_orderby_frame_contract_smoke(
          &frame_rows_read, &frame_finishes_read, &frame_errors_read)) {
    return true;
  }
  bool worker_frame_producer_enabled = false;
  uint32 worker_frame_rows_read = 0;
  uint32 worker_frame_finishes_read = 0;
  uint32 worker_frame_errors_read = 0;
  DBUG_EXECUTE_IF("pq_orderby_worker_frame_producer_smoke",
                  worker_frame_producer_enabled = true;);
  if (worker_frame_producer_enabled) {
    pq_global_stats.orderby_worker_frame_producer_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
    if (sort_exchange.run_orderby_worker_frame_producer_smoke(
            &worker_frame_rows_read, &worker_frame_finishes_read,
            &worker_frame_errors_read)) {
      pq_global_stats.orderby_worker_frame_producer_smoke_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }
    pq_global_stats.orderby_worker_frame_producer_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
  }
  bool worker_adapter_enabled = false;
  uint32 worker_adapter_rows = 0;
  uint32 worker_adapter_finishes = 0;
  uint32 worker_adapter_errors = 0;
  uint32 worker_adapter_order_rejects = 0;
  uint32 worker_adapter_after_finish_rejects = 0;
  DBUG_EXECUTE_IF("pq_orderby_worker_producer_adapter_skeleton_smoke",
                  worker_adapter_enabled = true;);
  if (worker_adapter_enabled) {
    pq_global_stats.orderby_worker_producer_adapter_skeleton_attempts.fetch_add(
        1, std::memory_order_relaxed);
    if (sort_exchange.run_orderby_worker_producer_adapter_skeleton_smoke(
            &worker_adapter_rows, &worker_adapter_finishes,
            &worker_adapter_errors, &worker_adapter_order_rejects,
            &worker_adapter_after_finish_rejects)) {
      pq_global_stats.orderby_worker_producer_adapter_skeleton_unsupported
          .fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    pq_global_stats.orderby_worker_producer_adapter_skeleton_success.fetch_add(
        1, std::memory_order_relaxed);
  }
  bool stream_heap_enabled = false;
  uint32 stream_heap_rows_read = 0;
  uint32 stream_heap_finishes_read = 0;
  uint32 stream_heap_would_blocks_read = 0;
  uint32 stream_heap_errors_read = 0;
  uint32 stream_heap_detaches_read = 0;
  uint32 stream_heap_refills_read = 0;
  uint32 stream_heap_replaces_read = 0;
  uint32 stream_heap_removes_read = 0;
  DBUG_EXECUTE_IF("pq_exchange_sort_stream_heap_smoke",
                  stream_heap_enabled = true;);
  if (stream_heap_enabled) {
    pq_global_stats.exchange_sort_stream_heap_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
    if (sort_exchange.run_orderby_streaming_heap_read_smoke(
            &stream_heap_rows_read, &stream_heap_finishes_read,
            &stream_heap_would_blocks_read, &stream_heap_errors_read,
            &stream_heap_detaches_read, &stream_heap_refills_read,
            &stream_heap_replaces_read, &stream_heap_removes_read)) {
      pq_global_stats.exchange_sort_stream_heap_smoke_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }
    pq_global_stats.exchange_sort_stream_heap_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
  }
  bool stream_materialized_enabled = false;
  uint32 stream_materialized_rows_read = 0;
  uint32 stream_materialized_unsupported = 0;
  uint32 stream_materialized_length_mismatch = 0;
  DBUG_EXECUTE_IF("pq_exchange_sort_stream_materialized_smoke",
                  stream_materialized_enabled = true;);
  if (stream_materialized_enabled) {
    pq_global_stats.exchange_sort_stream_materialized_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
    if (sort_exchange.run_orderby_streaming_materialization_smoke(
            leader_table, &stream_materialized_rows_read,
            &stream_materialized_unsupported,
            &stream_materialized_length_mismatch)) {
      pq_global_stats.exchange_sort_stream_materialized_smoke_unsupported
          .fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    if (stream_materialized_unsupported > 0) {
      pq_global_stats.exchange_sort_stream_materialized_smoke_unsupported
          .fetch_add(stream_materialized_unsupported,
                     std::memory_order_relaxed);
    } else {
      pq_global_stats.exchange_sort_stream_materialized_smoke_success
          .fetch_add(1, std::memory_order_relaxed);
    }
  }
  bool ordered_materialize_api_enabled = false;
  uint32 ordered_materialize_api_disabled = 0;
  uint32 ordered_materialize_api_unsupported = 0;
  uint32 ordered_materialize_api_rows = 0;
  DBUG_EXECUTE_IF("pq_exchange_sort_ordered_materialize_api_smoke",
                  ordered_materialize_api_enabled = true;);
  if (ordered_materialize_api_enabled) {
    pq_global_stats.exchange_sort_ordered_materialize_api_attempts
        .fetch_add(1, std::memory_order_relaxed);
    if (sort_exchange.run_orderby_materialize_api_skeleton_smoke(
            leader_table, &ordered_materialize_api_disabled,
            &ordered_materialize_api_unsupported,
            &ordered_materialize_api_rows)) {
      return true;
    }
  }
  bool ordered_reader_skeleton_enabled = false;
  uint32 ordered_reader_skeleton_rows = 0;
  uint32 ordered_reader_skeleton_finishes = 0;
  uint32 ordered_reader_skeleton_would_blocks = 0;
  uint32 ordered_reader_skeleton_errors = 0;
  uint32 ordered_reader_skeleton_detaches = 0;
  uint32 ordered_reader_skeleton_refills = 0;
  uint32 ordered_reader_skeleton_heap_replaces = 0;
  uint32 ordered_reader_skeleton_heap_removes = 0;
  DBUG_EXECUTE_IF("pq_exchange_sort_ordered_reader_skeleton_smoke",
                  ordered_reader_skeleton_enabled = true;);
  if (ordered_reader_skeleton_enabled) {
    pq_global_stats.exchange_sort_ordered_reader_skeleton_attempts.fetch_add(
        1, std::memory_order_relaxed);
    if (sort_exchange.run_orderby_ordered_reader_skeleton_smoke(
            &ordered_reader_skeleton_rows, &ordered_reader_skeleton_finishes,
            &ordered_reader_skeleton_would_blocks,
            &ordered_reader_skeleton_errors, &ordered_reader_skeleton_detaches,
            &ordered_reader_skeleton_refills,
            &ordered_reader_skeleton_heap_replaces,
            &ordered_reader_skeleton_heap_removes)) {
      pq_global_stats.exchange_sort_ordered_reader_skeleton_unsupported
          .fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    pq_global_stats.exchange_sort_ordered_reader_skeleton_success.fetch_add(
        1, std::memory_order_relaxed);
  }
  bool ordered_diag_enabled = false;
  uint32 ordered_diag_kill_not_wired = 0;
  DBUG_EXECUTE_IF("pq_exchange_sort_ordered_diag_smoke",
                  ordered_diag_enabled = true;);
  if (ordered_diag_enabled) {
    pq_global_stats.exchange_sort_ordered_diag_attempts.fetch_add(
        1, std::memory_order_relaxed);
    if (sort_exchange.run_orderby_ordered_diag_skeleton_smoke(
            &ordered_diag_kill_not_wired)) {
      return true;
    }
    pq_global_stats.exchange_sort_ordered_diag_success.fetch_add(
        1, std::memory_order_relaxed);
  }
  bool ref_owner_enabled = false;
  uint32 ref_owner_ref_bytes = 0;
  uint32 ref_owner_mismatch_rejects = 0;
  uint32 ref_owner_no_handler_rejects = 0;
  uint32 ref_owner_no_ref_rejects = 0;
  DBUG_EXECUTE_IF("pq_exchange_sort_ref_owner_smoke",
                  ref_owner_enabled = true;);
  if (ref_owner_enabled) {
    pq_global_stats.exchange_sort_ref_owner_attempts.fetch_add(
        1, std::memory_order_relaxed);
    if (leader_table == nullptr || leader_table->file == nullptr ||
        leader_table->file->ref_length == 0 ||
        sort_exchange.run_orderby_ref_owner_smoke(
            leader_table->file, leader_table->file->ref_length,
            &ref_owner_ref_bytes, &ref_owner_mismatch_rejects,
            &ref_owner_no_handler_rejects, &ref_owner_no_ref_rejects)) {
      pq_global_stats.exchange_sort_ref_owner_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }
    pq_global_stats.exchange_sort_ref_owner_success.fetch_add(
        1, std::memory_order_relaxed);
  }
  uint32 frame_merge_rows_read = 0;
  uint32 frame_merge_finishes_read = 0;
  if (sort_exchange.run_orderby_frame_merge_smoke(
          &frame_merge_rows_read, &frame_merge_finishes_read)) {
    return true;
  }
  uint32 frame_merge_edge_rows_read = 0;
  uint32 frame_merge_edge_finishes_read = 0;
  uint32 frame_merge_edge_errors_read = 0;
  if (sort_exchange.run_orderby_frame_merge_edge_smoke(
          &frame_merge_edge_rows_read, &frame_merge_edge_finishes_read,
          &frame_merge_edge_errors_read)) {
    return true;
  }
  uint32 frame_materialized_rows_read = 0;
  uint32 frame_materialized_unsupported = 0;
  if (sort_exchange.run_orderby_frame_materialization_smoke(
          leader_table, &frame_materialized_rows_read,
          &frame_materialized_unsupported)) {
    return true;
  }
  bool sort_state_shape_enabled = false;
  DBUG_EXECUTE_IF("pq_exchange_sort_state_shape_smoke",
                  sort_state_shape_enabled = true;);
  if (sort_state_shape_enabled) {
    pq_global_stats.exchange_sort_state_shape_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
    if (sort_exchange.run_orderby_sort_state_shape_smoke()) {
      pq_global_stats.exchange_sort_state_shape_smoke_unsupported.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      pq_global_stats.exchange_sort_state_shape_smoke_success.fetch_add(
          1, std::memory_order_relaxed);
    }
  }

  pq_global_stats.exchange_sort_smoke_runs.fetch_add(
      1, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_smoke_rows.fetch_add(
      rows_read + cached_rows_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_frame_smoke_rows.fetch_add(
      frame_rows_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_frame_smoke_finishes.fetch_add(
      frame_finishes_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_frame_smoke_errors.fetch_add(
      frame_errors_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_worker_frame_smoke_rows.fetch_add(
      worker_frame_rows_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_worker_frame_smoke_finishes.fetch_add(
      worker_frame_finishes_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_worker_frame_smoke_errors.fetch_add(
      worker_frame_errors_read, std::memory_order_relaxed);
  pq_global_stats.orderby_worker_producer_adapter_skeleton_rows.fetch_add(
      worker_adapter_rows, std::memory_order_relaxed);
  pq_global_stats.orderby_worker_producer_adapter_skeleton_finishes.fetch_add(
      worker_adapter_finishes, std::memory_order_relaxed);
  pq_global_stats.orderby_worker_producer_adapter_skeleton_errors.fetch_add(
      worker_adapter_errors, std::memory_order_relaxed);
  pq_global_stats.orderby_worker_producer_adapter_skeleton_order_rejects
      .fetch_add(worker_adapter_order_rejects, std::memory_order_relaxed);
  pq_global_stats
      .orderby_worker_producer_adapter_skeleton_after_finish_rejects.fetch_add(
          worker_adapter_after_finish_rejects, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_stream_heap_smoke_rows.fetch_add(
      stream_heap_rows_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_stream_heap_smoke_finishes.fetch_add(
      stream_heap_finishes_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_stream_heap_smoke_would_blocks.fetch_add(
      stream_heap_would_blocks_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_stream_heap_smoke_errors.fetch_add(
      stream_heap_errors_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_stream_heap_smoke_detaches.fetch_add(
      stream_heap_detaches_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_stream_heap_smoke_refills.fetch_add(
      stream_heap_refills_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_stream_heap_smoke_heap_replaces.fetch_add(
      stream_heap_replaces_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_stream_heap_smoke_heap_removes.fetch_add(
      stream_heap_removes_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_stream_materialized_smoke_rows.fetch_add(
      stream_materialized_rows_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_stream_materialized_smoke_length_mismatch
      .fetch_add(stream_materialized_length_mismatch,
                 std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_materialize_api_disabled.fetch_add(
      ordered_materialize_api_disabled, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_materialize_api_unsupported
      .fetch_add(ordered_materialize_api_unsupported,
                 std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_materialize_api_rows.fetch_add(
      ordered_materialize_api_rows, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_reader_skeleton_rows.fetch_add(
      ordered_reader_skeleton_rows, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_reader_skeleton_finishes.fetch_add(
      ordered_reader_skeleton_finishes, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_reader_skeleton_would_blocks.fetch_add(
      ordered_reader_skeleton_would_blocks, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_reader_skeleton_errors.fetch_add(
      ordered_reader_skeleton_errors, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_reader_skeleton_detaches.fetch_add(
      ordered_reader_skeleton_detaches, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_reader_skeleton_refills.fetch_add(
      ordered_reader_skeleton_refills, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_reader_skeleton_heap_replaces.fetch_add(
      ordered_reader_skeleton_heap_replaces, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_reader_skeleton_heap_removes.fetch_add(
      ordered_reader_skeleton_heap_removes, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ordered_diag_kill_not_wired.fetch_add(
      ordered_diag_kill_not_wired, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ref_owner_ref_bytes.fetch_add(
      ref_owner_ref_bytes, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ref_owner_mismatch_rejects.fetch_add(
      ref_owner_mismatch_rejects, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ref_owner_no_handler_rejects.fetch_add(
      ref_owner_no_handler_rejects, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_ref_owner_no_ref_rejects.fetch_add(
      ref_owner_no_ref_rejects, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_frame_merge_smoke_rows.fetch_add(
      frame_merge_rows_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_frame_merge_smoke_finishes.fetch_add(
      frame_merge_finishes_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_frame_merge_edge_smoke_rows.fetch_add(
      frame_merge_edge_rows_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_frame_merge_edge_smoke_finishes.fetch_add(
      frame_merge_edge_finishes_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_frame_merge_edge_smoke_errors.fetch_add(
      frame_merge_edge_errors_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_frame_materialized_smoke_rows.fetch_add(
      frame_materialized_rows_read, std::memory_order_relaxed);
  pq_global_stats.exchange_sort_frame_materialized_smoke_unsupported.fetch_add(
      frame_materialized_unsupported, std::memory_order_relaxed);
  return false;
}

bool Gather_operator::run_worker_open_table_smoke(THD *leader_thd,
                                                  TABLE *leader_table) {
  if (leader_thd == nullptr || leader_table == nullptr || m_dop != 1) {
    return true;
  }

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  if (worker == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (worker->m_open_ctx.leader_table == nullptr) {
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.actual_dop = m_dop;
  }

  if (pq_create_worker_thd(worker, this) == nullptr) {
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return true;
  }

  bool failed = pq_open_worker_table(&worker->m_open_ctx);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
        &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
  }
  if (!failed) {
    worker->m_open_ctx.worker_handler->pq_worker_scan_end(worker->m_worker_ctx);
    worker->m_worker_ctx = nullptr;
    pq_global_stats.worker_handler_smoke_runs.fetch_add(
        1, std::memory_order_relaxed);
    pq_close_worker_table(&worker->m_open_ctx, false);
  }
  pq_destroy_worker_thd(worker);
  leader_thd->store_globals();

  if (initialized_here) destroy();
  if (!failed) {
    pq_global_stats.worker_open_smoke_runs.fetch_add(
        1, std::memory_order_relaxed);
  }
  return failed;
}

bool Gather_operator::run_worker_attach_contract_smoke(
    THD *leader_thd, TABLE *leader_table, PQ_Leader_context *leader_ctx) {
  pq_global_stats.worker_attach_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (leader_thd == nullptr || leader_table == nullptr || leader_ctx == nullptr ||
      m_dop != 1) {
    return true;
  }

  bool initialized_here = false;
  bool cleanup_reached = false;
  bool failed = true;
  bool ownership_mismatch_smoke = false;
  bool icp_ownership_mismatch_smoke = false;
  bool icp_record_buffer_negative_smoke = false;
  bool handler_ref_cmp_smoke_pending = false;
  std::vector<uchar> handler_ref_cmp_smoke_ref;
  bool handler_ref_two_row_smoke_pending = false;
  std::vector<std::vector<uchar>> handler_ref_two_row_smoke_refs;
  bool stable_ref_pair_cmp_smoke_pending = false;
  std::vector<std::vector<uchar>> stable_ref_pair_cmp_smoke_refs;
  bool ref_adapter_shape_smoke_pending = false;
  std::vector<std::vector<uchar>> ref_adapter_shape_smoke_refs;
  bool ref_adapter_contract_smoke_pending = false;
  std::vector<std::vector<uchar>> ref_adapter_contract_smoke_refs;
  Item *saved_leader_pushed_idx_cond = nullptr;
  uint saved_leader_pushed_idx_cond_keyno = MAX_KEY;
  auto install_leader_icp_sentinel = [&]() {
    if (!icp_ownership_mismatch_smoke) {
      saved_leader_pushed_idx_cond = leader_table->file->pushed_idx_cond;
      saved_leader_pushed_idx_cond_keyno =
          leader_table->file->pushed_idx_cond_keyno;
      icp_ownership_mismatch_smoke = true;
    }
    leader_table->file->pushed_idx_cond =
        reinterpret_cast<Item *>(leader_table);
    leader_table->file->pushed_idx_cond_keyno = leader_table->s->primary_key;
  };
  auto cleanup = [&]() {
    if (cleanup_reached) return;
    cleanup_reached = true;

    if (icp_ownership_mismatch_smoke && leader_table != nullptr &&
        leader_table->file != nullptr) {
      leader_table->file->pushed_idx_cond = saved_leader_pushed_idx_cond;
      leader_table->file->pushed_idx_cond_keyno =
          saved_leader_pushed_idx_cond_keyno;
    }

    if (auto *worker = get_worker(0)) {
      if (worker->m_worker_ctx != nullptr &&
          worker->m_open_ctx.worker_handler != nullptr) {
        worker->m_open_ctx.worker_handler->pq_worker_scan_end(
            worker->m_worker_ctx);
        worker->m_worker_ctx = nullptr;
      }
      if (worker->m_open_ctx.worker_table != nullptr) {
        pq_close_worker_table(&worker->m_open_ctx, failed);
      }
      if (worker->m_worker_thd != nullptr) {
        pq_destroy_worker_thd(worker);
        leader_thd->store_globals();
      }
    }

    if (initialized_here) destroy();
    pq_global_stats.worker_attach_smoke_cleanup_calls.fetch_add(
        1, std::memory_order_relaxed);
  };

  if (!m_initialized) {
    if (init()) {
      cleanup();
      return true;
    }
    initialized_here = true;
  }

  if (configure_worker_open_contexts(leader_table, leader_ctx, m_dop)) {
    cleanup();
    return true;
  }

  auto *worker = get_worker(0);
  if (worker == nullptr) {
    cleanup();
    return true;
  }

  if (pq_create_worker_thd(worker, this) == nullptr) {
    leader_thd->store_globals();
    cleanup();
    return true;
  }

  if (pq_open_worker_table(&worker->m_open_ctx)) {
    cleanup();
    return true;
  }

  auto collect_two_worker_refs = [&worker](std::vector<std::vector<uchar>> *refs) {
    if (refs == nullptr) return true;
    refs->clear();
    bool two_row_failed = worker->m_open_ctx.worker_table == nullptr ||
                          worker->m_open_ctx.worker_handler == nullptr ||
                          worker->m_open_ctx.worker_table->record[0] == nullptr ||
                          worker->m_open_ctx.worker_handler->ha_rnd_init(true) != 0;
    for (uint i = 0; !two_row_failed && i < 2; ++i) {
      int error = worker->m_open_ctx.worker_handler->ha_rnd_next(
          worker->m_open_ctx.worker_table->record[0]);
      while (error == HA_ERR_RECORD_DELETED) {
        error = worker->m_open_ctx.worker_handler->ha_rnd_next(
            worker->m_open_ctx.worker_table->record[0]);
      }
      if (error != 0) {
        two_row_failed = true;
        break;
      }

      worker->m_open_ctx.worker_handler->position(
          worker->m_open_ctx.worker_table->record[0]);
      const uint ref_length = worker->m_open_ctx.worker_handler->ref_length;
      if (ref_length == 0 || worker->m_open_ctx.worker_handler->ref == nullptr) {
        two_row_failed = true;
        break;
      }
      refs->emplace_back(worker->m_open_ctx.worker_handler->ref,
                         worker->m_open_ctx.worker_handler->ref + ref_length);
      two_row_failed = refs->back().size() != ref_length;
    }
    if (worker->m_open_ctx.worker_handler != nullptr &&
        worker->m_open_ctx.worker_handler->inited) {
      worker->m_open_ctx.worker_handler->ha_rnd_end();
    }
    return two_row_failed || refs->size() < 2 || (*refs)[0] == (*refs)[1];
  };

  DBUG_EXECUTE_IF("pq_worker_record_buffer_probe_smoke", {
    if (worker->m_open_ctx.worker_handler != nullptr &&
        worker->m_open_ctx.worker_handler->ha_get_record_buffer() != nullptr) {
      pq_global_stats.worker_record_buffer_nonnull_probes.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      pq_global_stats.worker_record_buffer_null_probes.fetch_add(
          1, std::memory_order_relaxed);
    }
    failed = false;
    cleanup();
    pq_global_stats.worker_attach_smoke_success.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  });

  DBUG_EXECUTE_IF("pq_worker_icp_record_buffer_negative_smoke", {
    if (worker->m_open_ctx.worker_handler != nullptr &&
        worker->m_open_ctx.worker_handler->ha_get_record_buffer() != nullptr) {
      pq_global_stats.worker_record_buffer_nonnull_probes.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      pq_global_stats.worker_record_buffer_null_probes.fetch_add(
          1, std::memory_order_relaxed);
    }

    icp_record_buffer_negative_smoke = true;
    install_leader_icp_sentinel();
  });

  DBUG_EXECUTE_IF("pq_worker_ownership_mismatch_smoke", {
    /* Force the InnoDB ownership gate to reject this debug-only smoke. */
    ownership_mismatch_smoke = true;
    worker->m_open_ctx.worker_handler = leader_table->file;
  });

  DBUG_EXECUTE_IF("pq_worker_icp_ownership_mismatch_smoke", {
    /*
      Force an ICP-specific ownership mismatch without constructing a real
      Item tree. The InnoDB worker gate must reject the non-null leader ICP
      before any worker range dispatch or row production can dereference it.
    */
    install_leader_icp_sentinel();
  });

  DBUG_EXECUTE_IF("pq_orderby_handler_ref_two_row_cmp_smoke", {
    pq_global_stats.orderby_handler_ref_two_row_attempts.fetch_add(
        1, std::memory_order_relaxed);

    if (collect_two_worker_refs(&handler_ref_two_row_smoke_refs)) {
      pq_global_stats.orderby_handler_ref_two_row_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      cleanup();
      return true;
    }
    handler_ref_two_row_smoke_pending = true;
  });
  DBUG_EXECUTE_IF("pq_worker_result_stable_ref_pair_cmp_smoke", {
    pq_global_stats.stable_ref_pair_adapter_attempts.fetch_add(
        1, std::memory_order_relaxed);

    if (collect_two_worker_refs(&stable_ref_pair_cmp_smoke_refs)) {
      pq_global_stats.stable_ref_pair_adapter_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      cleanup();
      return true;
    }
    stable_ref_pair_cmp_smoke_pending = true;
  });
  DBUG_EXECUTE_IF("pq_orderby_fail_closed_ref_adapter_shape_smoke", {
    pq_global_stats.orderby_ref_adapter_shape_attempts.fetch_add(
        1, std::memory_order_relaxed);

    if (collect_two_worker_refs(&ref_adapter_shape_smoke_refs)) {
      pq_global_stats.orderby_ref_adapter_shape_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      cleanup();
      return true;
    }
    ref_adapter_shape_smoke_pending = true;
  });
  DBUG_EXECUTE_IF("pq_orderby_ref_adapter_contract_smoke", {
    pq_global_stats.orderby_ref_adapter_contract_attempts.fetch_add(
        1, std::memory_order_relaxed);

    if (collect_two_worker_refs(&ref_adapter_contract_smoke_refs)) {
      pq_global_stats.orderby_ref_adapter_contract_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      cleanup();
      return true;
    }
    ref_adapter_contract_smoke_pending = true;
  });

  if (worker->m_open_ctx.worker_handler == nullptr ||
      worker->m_open_ctx.worker_handler->pq_worker_scan_init(
          &worker->m_open_ctx, &worker->m_worker_ctx) != 0 ||
      worker->m_worker_ctx == nullptr) {
    if (icp_record_buffer_negative_smoke) {
      pq_global_stats.worker_icp_record_buffer_reject_probes.fetch_add(
          1, std::memory_order_relaxed);
    }
    cleanup();
    return !(ownership_mismatch_smoke || icp_ownership_mismatch_smoke);
  }

  DBUG_EXECUTE_IF("pq_orderby_worker_handler_ref_positive_smoke", {
    pq_global_stats.orderby_worker_handler_ref_attempts.fetch_add(
        1, std::memory_order_relaxed);

    bool converted = false;
    bool ref_failed = worker->m_open_ctx.worker_table == nullptr ||
                      worker->m_open_ctx.worker_table->record[0] == nullptr ||
                      worker->m_open_ctx.worker_handler == nullptr ||
                      worker->m_open_ctx.worker_handler
                              ->pq_worker_scan_callback_smoke(
                                  worker->m_worker_ctx,
                                  worker->m_open_ctx.worker_table->record[0],
                                  &converted) != 0 ||
                      !converted ||
                      worker->m_open_ctx.worker_handler->ref_length == 0 ||
                      worker->m_open_ctx.worker_handler->ref == nullptr;
    std::vector<uchar> copied_ref;

    if (!ref_failed) {
      worker->m_open_ctx.worker_handler->position(
          worker->m_open_ctx.worker_table->record[0]);
      const uint ref_length = worker->m_open_ctx.worker_handler->ref_length;
      copied_ref.assign(worker->m_open_ctx.worker_handler->ref,
                        worker->m_open_ctx.worker_handler->ref + ref_length);
      ref_failed = copied_ref.size() != ref_length;
    }

    if (!ref_failed &&
        worker->m_open_ctx.worker_handler->cmp_ref(copied_ref.data(),
                                                   copied_ref.data()) != 0) {
      ref_failed = true;
    }

    DBUG_EXECUTE_IF("pq_orderby_handler_ref_wire_smoke", {
      pq_global_stats.orderby_handler_ref_wire_attempts.fetch_add(
          1, std::memory_order_relaxed);

      uint32 decoded_ref_bytes = 0;
      uint32 contract_success = 0;
      const bool wire_failed =
          ref_failed || worker->m_open_ctx.worker_table == nullptr ||
          worker->m_open_ctx.worker_table->record[0] == nullptr ||
          pq_run_orderby_handler_ref_wire_smoke(
              worker->m_open_ctx.worker_table->record[0],
              worker->m_open_ctx.worker_table->s->reclength,
              copied_ref.data(), static_cast<uint32>(copied_ref.size()),
              &decoded_ref_bytes, &contract_success);

      if (wire_failed) {
        pq_global_stats.orderby_handler_ref_wire_unsupported.fetch_add(
            1, std::memory_order_relaxed);
        cleanup();
        return true;
      }

      pq_global_stats.orderby_handler_ref_wire_success.fetch_add(
          1, std::memory_order_relaxed);
      pq_global_stats.orderby_handler_ref_wire_bytes.fetch_add(
          decoded_ref_bytes, std::memory_order_relaxed);
      pq_global_stats.orderby_handler_ref_wire_contract_success.fetch_add(
          contract_success, std::memory_order_relaxed);
    });

    if (ref_failed) {
      pq_global_stats.orderby_worker_handler_ref_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      cleanup();
      return true;
    }

    pq_global_stats.orderby_worker_handler_ref_success.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.orderby_worker_handler_ref_bytes.fetch_add(
        copied_ref.size(), std::memory_order_relaxed);
    pq_global_stats.orderby_worker_handler_ref_cmp_equal.fetch_add(
        1, std::memory_order_relaxed);

    DBUG_EXECUTE_IF("pq_orderby_handler_ref_comparator_smoke", {
      pq_global_stats.orderby_handler_ref_cmp_attempts.fetch_add(
          1, std::memory_order_relaxed);
      handler_ref_cmp_smoke_ref = copied_ref;
      handler_ref_cmp_smoke_pending = true;
    });

  });

  failed = false;
  cleanup();
  if (handler_ref_cmp_smoke_pending) {
    if (leader_table == nullptr || leader_table->file == nullptr ||
        handler_ref_cmp_smoke_ref.empty() ||
        leader_table->file->cmp_ref(handler_ref_cmp_smoke_ref.data(),
                                    handler_ref_cmp_smoke_ref.data()) != 0) {
      pq_global_stats.orderby_handler_ref_cmp_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }

    pq_global_stats.orderby_handler_ref_cmp_success.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.orderby_handler_ref_cmp_bytes.fetch_add(
        handler_ref_cmp_smoke_ref.size(), std::memory_order_relaxed);
    pq_global_stats.orderby_handler_ref_cmp_equal.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.orderby_handler_ref_cmp_post_cleanup_success.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (handler_ref_two_row_smoke_pending) {
    if (leader_table == nullptr || leader_table->file == nullptr ||
        handler_ref_two_row_smoke_refs.size() < 2 ||
        handler_ref_two_row_smoke_refs[0].empty() ||
        handler_ref_two_row_smoke_refs[1].empty()) {
      pq_global_stats.orderby_handler_ref_two_row_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }

    const int cmp_forward =
        leader_table->file->cmp_ref(handler_ref_two_row_smoke_refs[0].data(),
                                    handler_ref_two_row_smoke_refs[1].data());
    const int cmp_reverse =
        leader_table->file->cmp_ref(handler_ref_two_row_smoke_refs[1].data(),
                                    handler_ref_two_row_smoke_refs[0].data());
    const bool antisymmetric =
        (cmp_forward < 0 && cmp_reverse > 0) ||
        (cmp_forward > 0 && cmp_reverse < 0);
    if (cmp_forward == 0 || cmp_reverse == 0 || !antisymmetric) {
      pq_global_stats.orderby_handler_ref_two_row_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }

    pq_global_stats.orderby_handler_ref_two_row_success.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.orderby_handler_ref_two_row_refs.fetch_add(
        handler_ref_two_row_smoke_refs.size(), std::memory_order_relaxed);
    pq_global_stats.orderby_handler_ref_two_row_cmp_nonzero.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.orderby_handler_ref_two_row_antisymmetric_success.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.orderby_handler_ref_two_row_post_cleanup_success.fetch_add(
        1, std::memory_order_relaxed);

    DBUG_EXECUTE_IF("pq_orderby_handler_ref_adapter_smoke", {
      pq_global_stats.orderby_handler_ref_adapter_smoke_attempts.fetch_add(
          1, std::memory_order_relaxed);

      int adapter_forward = 0;
      int adapter_reverse = 0;
      const bool adapter_failed =
          pq_orderby_handler_ref_adapter_smoke(
              leader_table->file, handler_ref_two_row_smoke_refs[0].data(),
              static_cast<uint32>(handler_ref_two_row_smoke_refs[0].size()),
              handler_ref_two_row_smoke_refs[1].data(),
              static_cast<uint32>(handler_ref_two_row_smoke_refs[1].size()),
              &adapter_forward, &adapter_reverse) ||
          ((adapter_forward < 0) != (cmp_forward < 0)) ||
          ((adapter_forward > 0) != (cmp_forward > 0)) ||
          ((adapter_reverse < 0) != (cmp_reverse < 0)) ||
          ((adapter_reverse > 0) != (cmp_reverse > 0));
      if (adapter_failed) {
        pq_global_stats.orderby_handler_ref_adapter_smoke_unsupported.fetch_add(
            1, std::memory_order_relaxed);
        return true;
      }

      pq_global_stats.orderby_handler_ref_adapter_smoke_success.fetch_add(
          1, std::memory_order_relaxed);
      pq_global_stats.orderby_handler_ref_adapter_smoke_refs.fetch_add(
          handler_ref_two_row_smoke_refs.size(), std::memory_order_relaxed);
      pq_global_stats.orderby_handler_ref_adapter_smoke_cmp_nonzero.fetch_add(
          1, std::memory_order_relaxed);
      pq_global_stats
          .orderby_handler_ref_adapter_smoke_antisymmetric_success.fetch_add(
              1, std::memory_order_relaxed);
      pq_global_stats
          .orderby_handler_ref_adapter_smoke_direction_match_success.fetch_add(
              1, std::memory_order_relaxed);
      pq_global_stats.orderby_handler_ref_adapter_smoke_tiebreak_success
          .fetch_add(1, std::memory_order_relaxed);
    });
    DBUG_EXECUTE_IF("pq_query_result_mq_stable_ref_smoke", {
      pq_global_stats.worker_result_stable_ref_attempts.fetch_add(
          1, std::memory_order_relaxed);

      uint32 stable_ref_bytes = 0;
      uint32 stable_ref_deep_copy = 0;
      uint32 stable_ref_normal_rejects = 0;
      uint32 stable_ref_invalid_rejects = 0;
      if (pq_run_query_result_mq_stable_ref_smoke(
              handler_ref_two_row_smoke_refs[0].data(),
              static_cast<uint32>(handler_ref_two_row_smoke_refs[0].size()),
              &stable_ref_bytes, &stable_ref_deep_copy,
              &stable_ref_normal_rejects, &stable_ref_invalid_rejects)) {
        pq_global_stats.worker_result_stable_ref_unsupported.fetch_add(
            1, std::memory_order_relaxed);
        return true;
      }

      pq_global_stats.worker_result_stable_ref_success.fetch_add(
          1, std::memory_order_relaxed);
      pq_global_stats.worker_result_stable_ref_bytes.fetch_add(
          stable_ref_bytes, std::memory_order_relaxed);
      pq_global_stats.worker_result_stable_ref_deep_copy_success.fetch_add(
          stable_ref_deep_copy, std::memory_order_relaxed);
      pq_global_stats.worker_result_stable_ref_normal_rejects.fetch_add(
          stable_ref_normal_rejects, std::memory_order_relaxed);
      pq_global_stats.worker_result_stable_ref_invalid_rejects.fetch_add(
          stable_ref_invalid_rejects, std::memory_order_relaxed);
    });
    DBUG_EXECUTE_IF("pq_worker_result_stable_ref_adapter_smoke", {
      pq_global_stats.stable_ref_adapter_attempts.fetch_add(
          1, std::memory_order_relaxed);

      uint32 stable_ref_adapter_bytes = 0;
      uint32 stable_ref_adapter_deep_copy = 0;
      uint32 stable_ref_adapter_length_mismatch = 0;
      const bool adapter_failed =
          leader_table == nullptr || leader_table->file == nullptr ||
          leader_table->file->ref_length == 0 ||
          pq_run_query_result_mq_stable_ref_adapter_smoke(
              handler_ref_two_row_smoke_refs[0].data(),
              static_cast<uint32>(handler_ref_two_row_smoke_refs[0].size()),
              leader_table->file->ref_length, &stable_ref_adapter_bytes,
              &stable_ref_adapter_deep_copy,
              &stable_ref_adapter_length_mismatch);
      if (adapter_failed) {
        pq_global_stats.stable_ref_adapter_unsupported.fetch_add(
            1, std::memory_order_relaxed);
        return true;
      }

      pq_global_stats.stable_ref_adapter_success.fetch_add(
          1, std::memory_order_relaxed);
      pq_global_stats.stable_ref_adapter_bytes.fetch_add(
          stable_ref_adapter_bytes, std::memory_order_relaxed);
      pq_global_stats.stable_ref_adapter_deep_copy_success.fetch_add(
          stable_ref_adapter_deep_copy, std::memory_order_relaxed);
      pq_global_stats.stable_ref_adapter_ref_length_mismatch.fetch_add(
          stable_ref_adapter_length_mismatch, std::memory_order_relaxed);
    });
  }
  if (stable_ref_pair_cmp_smoke_pending) {
    std::vector<uchar> left_owned_ref;
    std::vector<uchar> right_owned_ref;
    uint32 stable_ref_pair_bytes = 0;
    uint32 stable_ref_pair_deep_copy = 0;
    bool pair_failed =
        leader_table == nullptr || leader_table->file == nullptr ||
        leader_table->file->ref_length == 0 ||
        stable_ref_pair_cmp_smoke_refs.size() < 2 ||
        stable_ref_pair_cmp_smoke_refs[0].empty() ||
        stable_ref_pair_cmp_smoke_refs[1].empty() ||
        pq_run_query_result_mq_stable_ref_pair_smoke(
            stable_ref_pair_cmp_smoke_refs[0].data(),
            static_cast<uint32>(stable_ref_pair_cmp_smoke_refs[0].size()),
            stable_ref_pair_cmp_smoke_refs[1].data(),
            static_cast<uint32>(stable_ref_pair_cmp_smoke_refs[1].size()),
            leader_table->file->ref_length, &left_owned_ref, &right_owned_ref,
            &stable_ref_pair_bytes, &stable_ref_pair_deep_copy);
    if (!pair_failed &&
        (left_owned_ref.size() != leader_table->file->ref_length ||
         right_owned_ref.size() != leader_table->file->ref_length)) {
      pq_global_stats.stable_ref_pair_adapter_ref_length_mismatch.fetch_add(
          1, std::memory_order_relaxed);
      pair_failed = true;
    }
    if (!pair_failed && left_owned_ref == right_owned_ref) {
      pair_failed = true;
    }

    int pair_forward = 0;
    int pair_reverse = 0;
    pair_failed =
        pair_failed ||
        pq_orderby_handler_ref_adapter_smoke(
            leader_table->file, left_owned_ref.data(),
            static_cast<uint32>(left_owned_ref.size()), right_owned_ref.data(),
            static_cast<uint32>(right_owned_ref.size()), &pair_forward,
            &pair_reverse);
    const bool pair_antisymmetric =
        (pair_forward < 0 && pair_reverse > 0) ||
        (pair_forward > 0 && pair_reverse < 0);
    if (pair_failed || pair_forward == 0 || pair_reverse == 0 ||
        !pair_antisymmetric) {
      pq_global_stats.stable_ref_pair_adapter_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }

    pq_global_stats.stable_ref_pair_adapter_success.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.stable_ref_pair_adapter_refs.fetch_add(
        2, std::memory_order_relaxed);
    pq_global_stats.stable_ref_pair_adapter_bytes.fetch_add(
        stable_ref_pair_bytes, std::memory_order_relaxed);
    pq_global_stats.stable_ref_pair_adapter_deep_copy_success.fetch_add(
        stable_ref_pair_deep_copy, std::memory_order_relaxed);
    pq_global_stats.stable_ref_pair_adapter_cmp_nonzero.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.stable_ref_pair_adapter_antisymmetric_success.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.stable_ref_pair_adapter_tiebreak_success.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (ref_adapter_shape_smoke_pending) {
    std::vector<uchar> left_owned_ref;
    std::vector<uchar> right_owned_ref;
    uint32 ref_adapter_shape_bytes = 0;
    uint32 ref_adapter_shape_deep_copy = 0;
    bool adapter_shape_failed =
        leader_table == nullptr || leader_table->file == nullptr ||
        leader_table->file->ref_length == 0 ||
        ref_adapter_shape_smoke_refs.size() < 2 ||
        ref_adapter_shape_smoke_refs[0].empty() ||
        ref_adapter_shape_smoke_refs[1].empty() ||
        pq_run_query_result_mq_stable_ref_pair_smoke(
            ref_adapter_shape_smoke_refs[0].data(),
            static_cast<uint32>(ref_adapter_shape_smoke_refs[0].size()),
            ref_adapter_shape_smoke_refs[1].data(),
            static_cast<uint32>(ref_adapter_shape_smoke_refs[1].size()),
            leader_table->file->ref_length, &left_owned_ref, &right_owned_ref,
            &ref_adapter_shape_bytes, &ref_adapter_shape_deep_copy);
    if (!adapter_shape_failed &&
        (ref_adapter_shape_bytes != leader_table->file->ref_length * 2 ||
         ref_adapter_shape_deep_copy != 2)) {
      adapter_shape_failed = true;
    }

    int adapter_shape_forward = 0;
    int adapter_shape_reverse = 0;
    uint32 adapter_shape_rejects = 0;
    if (!adapter_shape_failed) {
      adapter_shape_failed = pq_orderby_fail_closed_ref_adapter_shape(
          leader_table->file, left_owned_ref.data(),
          static_cast<uint32>(left_owned_ref.size()), right_owned_ref.data(),
          static_cast<uint32>(right_owned_ref.size()), true,
          &adapter_shape_forward, &adapter_shape_reverse,
          &adapter_shape_rejects);
    }

    uint32 negative_rejects = 0;
    int negative_forward = 0;
    int negative_reverse = 0;
    const bool negative_sort_key_rejected =
        !adapter_shape_failed && !left_owned_ref.empty() &&
        !right_owned_ref.empty() &&
        pq_orderby_fail_closed_ref_adapter_shape(
            leader_table->file, left_owned_ref.data(),
            static_cast<uint32>(left_owned_ref.size()), right_owned_ref.data(),
            static_cast<uint32>(right_owned_ref.size()), false,
            &negative_forward, &negative_reverse, &negative_rejects) &&
        negative_rejects == 1;

    const bool adapter_shape_antisymmetric =
        (adapter_shape_forward < 0 && adapter_shape_reverse > 0) ||
        (adapter_shape_forward > 0 && adapter_shape_reverse < 0);
    if (adapter_shape_failed || adapter_shape_rejects != 0 ||
        adapter_shape_forward == 0 || adapter_shape_reverse == 0 ||
        !adapter_shape_antisymmetric || !negative_sort_key_rejected) {
      pq_global_stats.orderby_ref_adapter_shape_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }

    pq_global_stats.orderby_ref_adapter_shape_success.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.orderby_ref_adapter_shape_rejects.fetch_add(
        negative_rejects, std::memory_order_relaxed);
    pq_global_stats.orderby_ref_adapter_shape_cmp_nonzero.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.orderby_ref_adapter_shape_antisymmetric_success.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.orderby_ref_adapter_shape_tiebreak_success.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (ref_adapter_contract_smoke_pending) {
    std::vector<uchar> left_owned_ref;
    std::vector<uchar> right_owned_ref;
    uint32 ref_contract_bytes = 0;
    uint32 ref_contract_deep_copy = 0;
    bool contract_failed =
        leader_table == nullptr || leader_table->file == nullptr ||
        leader_table->file->ref_length == 0 ||
        ref_adapter_contract_smoke_refs.size() < 2 ||
        ref_adapter_contract_smoke_refs[0].empty() ||
        ref_adapter_contract_smoke_refs[1].empty() ||
        pq_run_query_result_mq_stable_ref_pair_smoke(
            ref_adapter_contract_smoke_refs[0].data(),
            static_cast<uint32>(ref_adapter_contract_smoke_refs[0].size()),
            ref_adapter_contract_smoke_refs[1].data(),
            static_cast<uint32>(ref_adapter_contract_smoke_refs[1].size()),
            leader_table->file->ref_length, &left_owned_ref, &right_owned_ref,
            &ref_contract_bytes, &ref_contract_deep_copy);
    if (!contract_failed &&
        (ref_contract_bytes != leader_table->file->ref_length * 2 ||
         ref_contract_deep_copy != 2)) {
      contract_failed = true;
    }

    int contract_forward = 0;
    int contract_reverse = 0;
    uint32 contract_rejects = 0;
    if (!contract_failed) {
      contract_failed = pq_orderby_fail_closed_ref_adapter_shape(
          leader_table->file, left_owned_ref.data(),
          static_cast<uint32>(left_owned_ref.size()), right_owned_ref.data(),
          static_cast<uint32>(right_owned_ref.size()), true,
          &contract_forward, &contract_reverse, &contract_rejects);
    }

    int base_forward = 0;
    int base_reverse = 0;
    if (!contract_failed) {
      contract_failed = pq_orderby_handler_ref_adapter_smoke(
          leader_table->file, left_owned_ref.data(),
          static_cast<uint32>(left_owned_ref.size()), right_owned_ref.data(),
          static_cast<uint32>(right_owned_ref.size()), &base_forward,
          &base_reverse);
    }

    uint32 null_handler_rejects = 0;
    uint32 len_mismatch_rejects = 0;
    uint32 equal_ref_rejects = 0;
    int negative_forward = 0;
    int negative_reverse = 0;
    const bool null_handler_rejected =
        !contract_failed &&
        pq_orderby_fail_closed_ref_adapter_shape(
            nullptr, left_owned_ref.data(),
            static_cast<uint32>(left_owned_ref.size()), right_owned_ref.data(),
            static_cast<uint32>(right_owned_ref.size()), true,
            &negative_forward, &negative_reverse, &null_handler_rejects) &&
        null_handler_rejects == 1;
    const bool len_mismatch_rejected =
        !contract_failed && left_owned_ref.size() > 1 &&
        pq_orderby_fail_closed_ref_adapter_shape(
            leader_table->file, left_owned_ref.data(),
            static_cast<uint32>(left_owned_ref.size() - 1),
            right_owned_ref.data(), static_cast<uint32>(right_owned_ref.size()),
            true, &negative_forward, &negative_reverse,
            &len_mismatch_rejects) &&
        len_mismatch_rejects == 1;
    const bool equal_ref_rejected =
        !contract_failed &&
        pq_orderby_fail_closed_ref_adapter_shape(
            leader_table->file, left_owned_ref.data(),
            static_cast<uint32>(left_owned_ref.size()), left_owned_ref.data(),
            static_cast<uint32>(left_owned_ref.size()), true,
            &negative_forward, &negative_reverse, &equal_ref_rejects) &&
        equal_ref_rejects == 1;

    const bool same_forward_direction =
        (contract_forward < 0) == (base_forward < 0) &&
        (contract_forward > 0) == (base_forward > 0);
    const bool same_reverse_direction =
        (contract_reverse < 0) == (base_reverse < 0) &&
        (contract_reverse > 0) == (base_reverse > 0);
    if (contract_failed || contract_rejects != 0 ||
        contract_forward == 0 || contract_reverse == 0 ||
        !same_forward_direction || !same_reverse_direction ||
        !null_handler_rejected || !len_mismatch_rejected ||
        !equal_ref_rejected) {
      pq_global_stats.orderby_ref_adapter_contract_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }

    pq_global_stats.orderby_ref_adapter_contract_success.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.orderby_ref_adapter_contract_null_handler_rejects.fetch_add(
        null_handler_rejects, std::memory_order_relaxed);
    pq_global_stats.orderby_ref_adapter_contract_len_mismatch_rejects.fetch_add(
        len_mismatch_rejects, std::memory_order_relaxed);
    pq_global_stats.orderby_ref_adapter_contract_equal_ref_rejects.fetch_add(
        equal_ref_rejects, std::memory_order_relaxed);
    pq_global_stats.orderby_ref_adapter_contract_direction_success.fetch_add(
        1, std::memory_order_relaxed);
  }
  pq_global_stats.worker_attach_smoke_success.fetch_add(
      1, std::memory_order_relaxed);
  return false;
}

bool Gather_operator::run_worker_callback_conversion_smoke(
    THD *leader_thd, TABLE *leader_table) {
  if (leader_thd == nullptr || leader_table == nullptr || m_dop != 1) {
    return true;
  }

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  if (worker == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (worker->m_open_ctx.leader_table == nullptr) {
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.actual_dop = m_dop;
  }

  if (pq_create_worker_thd(worker, this) == nullptr) {
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return true;
  }

  bool converted = false;
  bool failed = pq_open_worker_table(&worker->m_open_ctx);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
        &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
  }
  if (!failed) {
    pq_global_stats.callback_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_callback_smoke(
                 worker->m_worker_ctx, worker->m_open_ctx.worker_table->record[0],
                 &converted) != 0;
  }
  if (!failed && converted && m_exchange != nullptr) {
    failed = m_exchange->enqueue_record_image_smoke(
                 0, worker->m_open_ctx.worker_table) != 0;
  }
  if (!failed && converted && m_exchange != nullptr) {
    bool eof = false;
    bool row = false;
    failed = m_exchange->materialize_next_record_image(leader_table, &eof,
                                                       &row) ||
             !row || eof;
  }
  if (worker->m_worker_ctx != nullptr && worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->pq_worker_scan_end(worker->m_worker_ctx);
    worker->m_worker_ctx = nullptr;
  }
  if (worker->m_open_ctx.worker_table != nullptr) {
    pq_close_worker_table(&worker->m_open_ctx, failed);
  }
  pq_destroy_worker_thd(worker);
  leader_thd->store_globals();

  if (initialized_here) destroy();
  if (!failed && converted) {
    pq_global_stats.callback_smoke_rows.fetch_add(1,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_rows.fetch_add(1,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_finishes.fetch_add(
        1, std::memory_order_relaxed);
  }
  return failed;
}

bool Gather_operator::run_query_result_mq_contract_smoke(THD *leader_thd
                                                         [[maybe_unused]]) {
  uint32 rows_read = 0;
  uint32 finishes_read = 0;
  if (pq_run_query_result_mq_contract_smoke(&rows_read, &finishes_read)) {
    return true;
  }

  pq_global_stats.worker_result_smoke_rows.fetch_add(
      rows_read, std::memory_order_relaxed);
  pq_global_stats.worker_result_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);
  return false;
}

bool Gather_operator::run_query_result_mq_send_data_smoke(THD *leader_thd) {
  uint32 rows_read = 0;
  uint32 finishes_read = 0;
  uint32 errors_read = 0;
  if (pq_run_query_result_mq_send_data_smoke(leader_thd, &rows_read,
                                             &finishes_read, &errors_read)) {
    return true;
  }

  pq_global_stats.worker_result_smoke_rows.fetch_add(
      rows_read, std::memory_order_relaxed);
  pq_global_stats.worker_result_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);
  pq_global_stats.worker_result_smoke_errors.fetch_add(
      errors_read, std::memory_order_relaxed);
  return false;
}

bool Gather_operator::run_query_result_mq_adapter_smoke(THD *leader_thd) {
  uint32 rows_read = 0;
  uint32 finishes_read = 0;
  if (pq_run_query_result_mq_adapter_smoke(leader_thd, &rows_read,
                                           &finishes_read)) {
    return true;
  }

  pq_global_stats.worker_result_smoke_rows.fetch_add(
      rows_read, std::memory_order_relaxed);
  pq_global_stats.worker_result_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);
  return false;
}

bool Gather_operator::run_query_result_mq_wiring_smoke(THD *leader_thd) {
  uint32 rows_read = 0;
  uint32 finishes_read = 0;
  if (pq_run_query_result_mq_wiring_smoke(leader_thd, &rows_read,
                                          &finishes_read)) {
    return true;
  }

  pq_global_stats.worker_result_smoke_rows.fetch_add(
      rows_read, std::memory_order_relaxed);
  pq_global_stats.worker_result_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);
  return false;
}

bool Gather_operator::run_query_result_mq_threaded_probe_smoke(
    THD *leader_thd) {
  if (leader_thd == nullptr || m_dop != 1) return true;

  bool initialized_here = false;
  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  if (worker == nullptr || worker->m_mq_handle == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  worker->m_task = PQ_worker_task::QUERY_RESULT_MQ_PROBE;
  if (start_workers(leader_thd)) {
    worker->m_task = PQ_worker_task::NOOP;
    if (initialized_here) destroy();
    return true;
  }

  pq_global_stats.worker_result_smoke_workers.fetch_add(
      1, std::memory_order_relaxed);

  bool failed = false;
  uint32 rows_read = 0;
  uint32 finishes_read = 0;
  bool drained_before_wait = false;

  for (uint32 i = 0; !failed && finishes_read == 0 && i < 128; ++i) {
    void *raw_data = nullptr;
    uint32 raw_len = 0;
    const MQ_RESULT receive_result =
        worker->m_mq_handle->receive(&raw_data, &raw_len);
    if (receive_result == MQ_WOULD_BLOCK) {
      if (worker->is_terminal()) {
        failed = true;
        break;
      }
      PQ_mq_event *receiver = worker->m_mq_handle->get_receiver();
      if (receiver == nullptr) {
        failed = true;
        break;
      }
      receiver->wait_latch(10000);
      receiver->reset_latch();
      continue;
    }
    if (receive_result != MQ_SUCCESS) {
      failed = true;
      break;
    }

    const PQ_worker_result_frame_header *header = nullptr;
    const uchar *decoded_null_bitmap = nullptr;
    const uchar *decoded_payload = nullptr;
    if (pq_validate_worker_result_frame(raw_data, raw_len, &header,
                                        &decoded_null_bitmap,
                                        &decoded_payload)) {
      failed = true;
      break;
    }

    if (header->type ==
        static_cast<uint16>(PQ_worker_result_message_type::ROW)) {
      if (finishes_read != 0) {
        failed = true;
        break;
      }
      std::vector<PQ_worker_result_decoded_field> decoded_fields;
      failed = pq_decode_worker_result_row(raw_data, raw_len, &decoded_fields) ||
               decoded_fields.size() != 2 || decoded_fields[0].is_null ||
               decoded_fields[1].is_null;
      if (!failed && rows_read == 0) {
        failed = decoded_fields[0].value_len != 3 ||
                 decoded_fields[1].value_len != 3 ||
                 memcmp(decoded_fields[0].value, "101", 3) != 0 ||
                 memcmp(decoded_fields[1].value, "202", 3) != 0;
      } else if (!failed && rows_read == 1) {
        failed = decoded_fields[0].value_len != 3 ||
                 decoded_fields[1].value_len != 3 ||
                 memcmp(decoded_fields[0].value, "303", 3) != 0 ||
                 memcmp(decoded_fields[1].value, "404", 3) != 0;
      } else if (!failed) {
        failed = true;
      }
      if (!failed) ++rows_read;
    } else if (header->type ==
               static_cast<uint16>(PQ_worker_result_message_type::FINISH)) {
      if (rows_read != 2 || finishes_read != 0) {
        failed = true;
        break;
      }
      failed = decoded_null_bitmap != nullptr || decoded_payload != nullptr ||
               header->field_count != 0 || header->null_bitmap_len != 0 ||
               header->payload_len != 0;
      if (!failed) {
        ++finishes_read;
        drained_before_wait = true;
      }
    } else {
      failed = true;
    }
  }

  failed = failed || rows_read != 2 || finishes_read != 1;
  if (!failed && drained_before_wait) {
    pq_global_stats.worker_result_smoke_prewait_drains.fetch_add(
        1, std::memory_order_relaxed);
  }
  failed = failed || wait_for_workers(leader_thd) != 0;

  if (failed) abort_workers(leader_thd);
  worker->m_task = PQ_worker_task::NOOP;
  if (initialized_here) destroy();

  if (failed || rows_read != 2 || finishes_read != 1) return true;

  pq_global_stats.worker_result_smoke_rows.fetch_add(
      rows_read, std::memory_order_relaxed);
  pq_global_stats.worker_result_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);
  return false;
}

bool Gather_operator::run_worker_execute_iterator_smoke(THD *leader_thd,
                                                        JOIN *join,
                                                        PQ_Leader_context
                                                            *leader_ctx) {
  (void)leader_ctx;
  pq_global_stats.worker_execute_iterator_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (!pq_clone_contract_preflight(leader_thd, join)) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_clone.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }

  QEP_TAB *const source_tab = pq_first_worker_smoke_qep_tab(join);
  if (source_tab == nullptr) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_access_path.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }

  bool initialized_here = false;
  if (!is_initialized()) {
    if (init()) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_worker_open
          .fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  if (worker == nullptr) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_worker_open.fetch_add(
        1, std::memory_order_relaxed);
    if (initialized_here) destroy();
    return false;
  }

  worker->m_open_ctx.leader_table = source_tab->table();
  worker->m_open_ctx.actual_dop = m_dop;
  worker->m_open_ctx.mq_handle = worker->m_mq_handle;

  uint execute_dop = 0;
  PQ_Leader_context *execute_ctx = nullptr;
  const int execute_error = source_tab->table()->file->pq_leader_scan_init(
      leader_thd, &execute_ctx, PQ_leader_scan_mode::EXECUTE, 1, &execute_dop,
      false);
  if (execute_error != 0 || execute_ctx == nullptr) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_init.fetch_add(
        1, std::memory_order_relaxed);
    if (initialized_here) destroy();
    return false;
  }
  worker->m_open_ctx.leader_ctx = execute_ctx;
  worker->m_open_ctx.actual_dop = execute_dop > 0 ? execute_dop : 1;
  auto end_execute_ctx = [&]() {
    if (execute_ctx != nullptr) {
      source_tab->table()->file->pq_leader_scan_end(execute_ctx);
      execute_ctx = nullptr;
    }
  };

  PQ_worker_execute_smoke_plan worker_plan;
  if (!worker_plan.create(worker, this, join)) {
    worker_plan.cleanup(false, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }
  THD *worker_thd = worker_plan.worker_thd();
  JOIN *worker_join = worker_plan.worker_join();

  if (pq_clone_table_ref_preflight(worker_thd, source_tab->table_ref)) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_access_path.fetch_add(
        1, std::memory_order_relaxed);
    worker_plan.cleanup(false, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }

  if (pq_clone_position_scalar_preflight(source_tab)) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_access_path.fetch_add(
        1, std::memory_order_relaxed);
    worker_plan.cleanup(false, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }

  if (pq_clone_qep_tab_scalar_preflight(worker_join, source_tab)) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_access_path.fetch_add(
        1, std::memory_order_relaxed);
    worker_plan.cleanup(false, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }

  if (pq_open_worker_table(&worker->m_open_ctx)) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_worker_open.fetch_add(
        1, std::memory_order_relaxed);
    worker_plan.cleanup(false, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }
  pq_global_stats.worker_execute_iterator_smoke_worker_table_opened.fetch_add(
      1, std::memory_order_relaxed);

  if (pq_clone_table_scalar_preflight(worker_thd,
                                      worker->m_open_ctx.worker_table,
                                      source_tab->table())) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_access_path.fetch_add(
        1, std::memory_order_relaxed);
    worker_plan.cleanup(true, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }

  if (pq_bind_qep_tab_table_preflight(worker_join,
                                      worker->m_open_ctx.worker_table,
                                      source_tab->table())) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_access_path.fetch_add(
        1, std::memory_order_relaxed);
    worker_plan.cleanup(true, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }

  PQ_qep_tab_table_attach_state qep_tab_attach;
  if (pq_attach_qep_tab_table_smoke(worker_join,
                                    worker->m_open_ctx.worker_table,
                                    source_tab->table(), &qep_tab_attach)) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_access_path.fetch_add(
        1, std::memory_order_relaxed);
    worker_plan.cleanup(true, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }
  QEP_TAB *const worker_qep_tab = qep_tab_attach.tab;
  auto detach_qep_tab = create_scope_guard(
      [&]() { pq_detach_qep_tab_table_smoke(&qep_tab_attach); });

  if (pq_clone_worker_base_table_fields_smoke(
          worker_thd, worker_join->query_block, join->query_block,
          worker->m_open_ctx.worker_table, source_tab->table())) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_output_fields
        .fetch_add(1, std::memory_order_relaxed);
    detach_qep_tab.reset();
    worker_plan.cleanup(true, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }
  pq_global_stats.worker_execute_iterator_smoke_output_fields_cloned.fetch_add(
      1, std::memory_order_relaxed);

  if (source_tab->range_scan() != nullptr) {
    (void)pq_clone_range_scan_preflight(worker_thd,
                                        worker->m_open_ctx.worker_table,
                                        source_tab);
  }

  AccessPath *const worker_block_scan = NewPQblockScanAccessPath(
      worker_thd, worker->m_open_ctx.worker_table, this, DIV_TAB,
      worker_qep_tab,
      /*need_rowid=*/false);
  if (worker_block_scan == nullptr) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_access_path.fetch_add(
        1, std::memory_order_relaxed);
    detach_qep_tab.reset();
    worker_plan.cleanup(true, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }

  {
    auto iterator =
        CreateIteratorFromAccessPath(worker_thd, worker_block_scan, worker_join,
                                     /*eligible_for_batch_mode=*/false);
    if (iterator == nullptr) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_iterator.fetch_add(
          1, std::memory_order_relaxed);
      detach_qep_tab.reset();
      worker_plan.cleanup(true, true);
      end_execute_ctx();
      leader_thd->store_globals();
      if (initialized_here) destroy();
      return false;
    }

    pq_global_stats.worker_execute_iterator_smoke_iterator_constructed.fetch_add(
        1, std::memory_order_relaxed);
  }

  if (!worker_plan.bind_result()) {
    detach_qep_tab.reset();
    worker_plan.cleanup(true, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }
  pq_global_stats.worker_execute_iterator_smoke_result_bound_before_root_read
      .fetch_add(1, std::memory_order_relaxed);

  bool root_iterator_constructed = false;
  bool query_expression_root_owned = false;
  {
    AccessPath *const saved_root_access_path = worker_join->root_access_path();
    worker_join->set_root_access_path(worker_block_scan);
    pq_global_stats.worker_execute_iterator_smoke_root_attached.fetch_add(
        1, std::memory_order_relaxed);
    auto restore_root_access_path = create_scope_guard([&]() {
      if (query_expression_root_owned &&
          worker_join->query_expression() != nullptr) {
        worker_join->query_expression()->clear_root_access_path();
      }
      worker_join->set_root_access_path(saved_root_access_path);
    });

    if (worker_join->query_expression() == nullptr ||
        worker_join->query_expression()->create_pq_worker_root_iterator_smoke(
          worker_thd, worker_join, /*eligible_for_batch_mode=*/false)) {
      pq_global_stats.worker_execute_iterator_smoke_blocked_iterator.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      query_expression_root_owned = true;
      RowIterator *const iterator =
          worker_join->query_expression()->root_iterator();
      pq_global_stats.worker_execute_iterator_smoke_root_iterator_constructed
          .fetch_add(1, std::memory_order_relaxed);
      pq_global_stats.worker_execute_iterator_smoke_root_owned.fetch_add(
          1, std::memory_order_relaxed);
      (void)worker_plan.record_execute_iterator_query_blocker();
      if (iterator->Init()) {
        pq_global_stats.worker_execute_iterator_smoke_blocked_root_init
            .fetch_add(1, std::memory_order_relaxed);
      } else {
        pq_global_stats.worker_execute_iterator_smoke_root_init_success
            .fetch_add(1, std::memory_order_relaxed);
        if (iterator->Read() != 0) {
          pq_global_stats.worker_execute_iterator_smoke_blocked_root_read
              .fetch_add(1, std::memory_order_relaxed);
        } else {
          pq_global_stats.worker_execute_iterator_smoke_root_read_success
              .fetch_add(1, std::memory_order_relaxed);
          root_iterator_constructed = true;
        }
      }
    }
  }
  if (!root_iterator_constructed) {
    detach_qep_tab.reset();
    worker_plan.cleanup(true, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }
  if (!worker_plan.send_current_row_result()) {
    detach_qep_tab.reset();
    worker_plan.cleanup(true, true);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }
  detach_qep_tab.reset();

  if (!pq_worker_join_ownership_preflight(worker_join, join)) {
    pq_global_stats.worker_execute_iterator_smoke_blocked_ownership.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.worker_execute_iterator_smoke_blocked_execute.fetch_add(
        1, std::memory_order_relaxed);
    worker_plan.cleanup(true, false);
    end_execute_ctx();
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return false;
  }

  /*
    Keep the user-visible PQ gate closed. ExecuteIteratorQuery() is preflight
    ready, but it must run in a worker thread before the leader can drain MQ
    frames concurrently without risking a same-thread producer block.
  */
  pq_global_stats.worker_execute_iterator_smoke_blocked_execute.fetch_add(
      1, std::memory_order_relaxed);
  worker_plan.cleanup(true, false);
  end_execute_ctx();
  leader_thd->store_globals();
  if (initialized_here) destroy();
  return false;
}

bool Gather_operator::run_worker_execute_iterator_threaded_precheck_smoke(
    THD *leader_thd, JOIN *join) {
  pq_global_stats.worker_execute_iterator_threaded_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (leader_thd == nullptr || join == nullptr || m_dop != 1 ||
      !pq_clone_contract_preflight(leader_thd, join)) {
    pq_global_stats.worker_execute_iterator_threaded_smoke_blocked_setup
        .fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  QEP_TAB *const source_tab = pq_first_worker_smoke_qep_tab(join);
  if (source_tab == nullptr || source_tab->table() == nullptr) {
    pq_global_stats.worker_execute_iterator_threaded_smoke_blocked_setup
        .fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  bool initialized_here = false;
  if (!is_initialized()) {
    if (init()) {
      pq_global_stats.worker_execute_iterator_threaded_smoke_blocked_setup
          .fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  if (worker == nullptr || worker->m_task != PQ_worker_task::NOOP) {
    pq_global_stats.worker_execute_iterator_threaded_smoke_blocked_setup
        .fetch_add(1, std::memory_order_relaxed);
    if (initialized_here) destroy();
    return false;
  }

  uint execute_dop = 0;
  PQ_Leader_context *execute_ctx = nullptr;
  const int execute_error = source_tab->table()->file->pq_leader_scan_init(
      leader_thd, &execute_ctx, PQ_leader_scan_mode::EXECUTE, 1, &execute_dop,
      false);
  if (execute_error != 0 || execute_ctx == nullptr) {
    pq_global_stats.worker_execute_iterator_threaded_smoke_blocked_setup
        .fetch_add(1, std::memory_order_relaxed);
    if (initialized_here) destroy();
    return false;
  }

  worker->m_open_ctx.leader_table = source_tab->table();
  worker->m_open_ctx.leader_ctx = execute_ctx;
  worker->m_open_ctx.actual_dop = execute_dop > 0 ? execute_dop : 1;
  worker->m_open_ctx.mq_handle = worker->m_mq_handle;
  worker->m_task_leader_join = join;
  worker->m_task =
      DBUG_EVALUATE_IF("pq_worker_execute_iterator_threaded_call_smoke",
                       PQ_worker_task::EXECUTE_ITERATOR_CALL_SMOKE,
                       PQ_worker_task::EXECUTE_ITERATOR_SMOKE);

  bool failed = start_workers(leader_thd);
  uint32 rows_read = 0;
  uint32 finishes_read = 0;
  uint32 errors_read = 0;
  const bool call_smoke =
      worker->m_task == PQ_worker_task::EXECUTE_ITERATOR_CALL_SMOKE;
  if (!failed && call_smoke) {
    const size_t expected_fields = join->fields != nullptr ? join->fields->size()
                                                           : 0;
    failed = pq_drain_worker_result_frames(worker, expected_fields, 128,
                                           &rows_read, &finishes_read,
                                           &errors_read, true,
                                           get_exchange(),
                                           source_tab->table());
    if (!failed) {
      pq_global_stats.worker_execute_iterator_threaded_smoke_drained_rows
          .fetch_add(rows_read, std::memory_order_relaxed);
      pq_global_stats.worker_execute_iterator_threaded_smoke_drained_finishes
          .fetch_add(finishes_read, std::memory_order_relaxed);
    } else {
      pq_global_stats.worker_execute_iterator_threaded_smoke_drain_errors
          .fetch_add(1, std::memory_order_relaxed);
      pq_global_stats.worker_execute_iterator_smoke_blocked_execute_drain
          .fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (!failed) failed = wait_for_workers(leader_thd) != 0;
  if (failed) abort_workers(leader_thd);

  worker->m_task = PQ_worker_task::NOOP;
  worker->m_task_leader_join = nullptr;
  source_tab->table()->file->pq_leader_scan_end(execute_ctx);
  leader_thd->store_globals();
  if (initialized_here) destroy();

  return failed;
}

class PQ_limited_mq_row_sink final : public PQ_row_sink {
 public:
  PQ_limited_mq_row_sink(Exchange_nosort *exchange, uint32 worker_id,
                         uint32 max_rows)
      : m_exchange(exchange), m_worker_id(worker_id), m_max_rows(max_rows) {}

  bool send_row(TABLE *source_table) override {
    if (should_abort()) return true;
    m_failed = m_exchange == nullptr ||
               m_exchange->enqueue_record_image(m_worker_id, source_table);
    if (!m_failed) ++m_rows_sent;
    return m_failed;
  }

  bool should_abort() const override {
    return m_failed || m_rows_sent >= m_max_rows;
  }

  bool stop_is_success() const override {
    return !m_failed && m_rows_sent >= m_max_rows;
  }

  uint32 rows_sent() const { return m_rows_sent; }

 private:
  Exchange_nosort *m_exchange;
  uint32 m_worker_id;
  uint32 m_max_rows;
  uint32 m_rows_sent{0};
  bool m_failed{false};
};

class PQ_partial_group_mq_sink final : public PQ_row_sink {
 public:
  PQ_partial_group_mq_sink(Exchange_nosort *exchange, uint32 worker_id,
                           uint32 group_field_index, uint32 value_field_index,
                           PQ_partial_group_agg_kind agg_kind)
      : m_exchange(exchange),
        m_worker_id(worker_id),
        m_group_field_index(group_field_index),
        m_value_field_index(value_field_index),
        m_agg_kind(agg_kind) {}

  bool send_row(TABLE *source_table) override {
    if (m_failed || source_table == nullptr || source_table->s == nullptr ||
        source_table->s->fields <= m_group_field_index ||
        source_table->s->fields <= m_value_field_index) {
      m_failed = true;
      return true;
    }

    Field *group_field = source_table->field[m_group_field_index];
    Field *value_field = source_table->field[m_value_field_index];
    if (group_field == nullptr || value_field == nullptr) {
      m_failed = true;
      return true;
    }
    if (group_field->is_null()) {
      m_failed = true;
      return true;
    }
    if (value_field->result_type() != INT_RESULT) {
      m_skipped = true;
      return false;
    }
    if (group_field->result_type() != INT_RESULT) {
      m_skipped = true;
      return false;
    }
    if (source_table->read_set != nullptr &&
        !bitmap_is_set(source_table->read_set, group_field->field_index())) {
      m_skipped = true;
      return false;
    }
    if (source_table->read_set != nullptr &&
        !bitmap_is_set(source_table->read_set, value_field->field_index())) {
      m_skipped = true;
      return false;
    }

    PQ_partial_group_payload_v1 payload = {
        PQ_PARTIAL_GROUP_PAYLOAD_MAGIC,
        PQ_PARTIAL_GROUP_PAYLOAD_VERSION,
        static_cast<uint16>(m_agg_kind),
        m_worker_id,
        0,
        static_cast<int64>(group_field->val_int()),
        1,
        0,
        0,
        0,
        0};
    if (!value_field->is_null()) {
      const int64 value = static_cast<int64>(value_field->val_int());
      payload.count_value = 1;
      payload.sum = value;
      payload.min = value;
      payload.max = value;
    }
    if (pq_merge_partial_group_payload_v1(payload, m_slots, kMaxGroups,
                                          &m_group_count)) {
      m_failed = true;
      return true;
    }
    return false;
  }

  bool send_partial_group() {
    if (m_failed || m_exchange == nullptr) return true;
    if (m_skipped || m_group_count == 0) return false;

    for (uint32 i = 0; i < kMaxGroups; ++i) {
      if (!m_slots[i].used) continue;
      const PQ_partial_group_payload_v1 payload = {
          PQ_PARTIAL_GROUP_PAYLOAD_MAGIC,
          PQ_PARTIAL_GROUP_PAYLOAD_VERSION,
          static_cast<uint16>(m_agg_kind),
          m_worker_id,
          0,
          m_slots[i].group_key,
          m_slots[i].count_star,
          m_slots[i].count_value,
          m_slots[i].sum,
          m_slots[i].min,
          m_slots[i].max};
      if (m_exchange->enqueue_partial_group_smoke(m_worker_id, payload)) {
        return true;
      }
    }
    return false;
  }

  uint32 groups_sent() const { return m_group_count; }

 private:
  static constexpr uint32 kMaxGroups = 16;

  Exchange_nosort *m_exchange;
  uint32 m_worker_id;
  uint32 m_group_field_index;
  uint32 m_value_field_index;
  PQ_partial_group_agg_kind m_agg_kind;
  PQ_partial_group_merge_slot_v1 m_slots[kMaxGroups];
  uint32 m_group_count{0};
  bool m_skipped{false};
  bool m_failed{false};
};

namespace {

bool pq_run_callback_limited_producer_task(PQ_worker_info *worker,
                                           Gather_operator *gather) {
  if (worker == nullptr || gather == nullptr ||
      worker->m_open_ctx.leader_table == nullptr ||
      worker->m_task_max_rows == 0) {
    return true;
  }

  auto *exchange = gather->get_exchange();
  if (exchange == nullptr ||
      exchange->get_exchange_type() != Exchange::EXCHANGE_NOSORT) {
    return true;
  }

  auto *nosort = static_cast<Exchange_nosort *>(exchange);
  if (worker->m_task_force_error) {
    worker->m_error_code = HA_ERR_INTERNAL_ERROR;
    (void)nosort->enqueue_error_smoke(worker->m_worker_id);
    return true;
  }

  bool failed = pq_open_worker_table(&worker->m_open_ctx);
  bool rnd_inited = false;
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->ha_rnd_init(true) != 0;
    rnd_inited = !failed;
  }
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
        &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
  }
  if (!failed) {
    pq_global_stats.callback_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
  }

  PQ_limited_mq_row_sink row_sink(nosort, worker->m_worker_id,
                                  worker->m_task_max_rows);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler
                 ->pq_worker_scan_callback_produce(worker->m_worker_ctx,
                                                   &row_sink) != 0;
  }
  if (!failed) {
    failed = nosort->enqueue_finish_smoke(worker->m_worker_id);
  }

  if (worker->m_worker_ctx != nullptr &&
      worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->pq_worker_scan_end(
        worker->m_worker_ctx);
    worker->m_worker_ctx = nullptr;
  }
  if (rnd_inited && worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->ha_rnd_end();
  }
  if (worker->m_open_ctx.worker_table != nullptr) {
    pq_close_worker_table(&worker->m_open_ctx, failed);
  }

  worker->m_task_rows_sent.store(row_sink.rows_sent(),
                                 std::memory_order_release);
  if (failed) {
    (void)nosort->enqueue_error_smoke(worker->m_worker_id);
  }
  return failed;
}

bool pq_run_query_result_mq_probe_task(PQ_worker_info *worker) {
  if (worker == nullptr || worker->m_worker_thd == nullptr ||
      worker->m_mq_handle == nullptr) {
    return true;
  }

  THD *worker_thd = worker->m_worker_thd;
  Query_result_mq result(nullptr, worker->m_mq_handle, false);
  mem_root_deque<Item *> row1(worker_thd->mem_root);
  mem_root_deque<Item *> row2(worker_thd->mem_root);
  row1.push_back(new (worker_thd->mem_root) Item_int(101));
  row1.push_back(new (worker_thd->mem_root) Item_int(202));
  row2.push_back(new (worker_thd->mem_root) Item_int(303));
  row2.push_back(new (worker_thd->mem_root) Item_int(404));

  const ha_rows sent_rows_before = worker_thd->get_sent_row_count();
  const bool failed = row1[0] == nullptr || row1[1] == nullptr ||
                      row2[0] == nullptr || row2[1] == nullptr ||
                      result.start_execution(worker_thd) ||
                      result.send_result_set_metadata(worker_thd, row1, 0) ||
                      result.send_data(worker_thd, row1) ||
                      result.send_data(worker_thd, row2) ||
                      result.send_eof(worker_thd);
  worker_thd->set_sent_row_count(sent_rows_before);
  if (failed) worker->m_error_code = HA_ERR_INTERNAL_ERROR;
  return failed;
}

bool pq_run_worker_execute_iterator_threaded_precheck_task(
    PQ_worker_info *worker, Gather_operator *gather) {
  if (worker == nullptr || gather == nullptr ||
      worker->m_task_leader_join == nullptr ||
      worker->m_open_ctx.leader_ctx == nullptr ||
      worker->m_open_ctx.leader_table == nullptr ||
      worker->m_mq_handle == nullptr || worker->m_worker_thd == nullptr) {
    pq_global_stats.worker_execute_iterator_threaded_smoke_blocked_setup
        .fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  JOIN *const leader_join = worker->m_task_leader_join;
  QEP_TAB *const source_tab = pq_first_worker_smoke_qep_tab(leader_join);
  if (source_tab == nullptr || source_tab->table() == nullptr ||
      source_tab->table_ref == nullptr ||
      source_tab->table() != worker->m_open_ctx.leader_table) {
    pq_global_stats.worker_execute_iterator_threaded_smoke_blocked_setup
        .fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  bool success = false;
  bool close_worker_table = false;
  bool qep_tab_attached = false;
  const bool call_execute_iterator =
      worker->m_task == PQ_worker_task::EXECUTE_ITERATOR_CALL_SMOKE;
  PQ_qep_tab_table_attach_state qep_tab_attach;
  PQ_worker_execute_smoke_plan worker_plan;

  auto finish = [&]() {
    if (qep_tab_attached) {
      pq_detach_qep_tab_table_smoke(&qep_tab_attach);
      qep_tab_attached = false;
    }
    worker_plan.cleanup(close_worker_table, !success);
    worker->m_task_leader_join = nullptr;
    if (success) {
      pq_global_stats.worker_execute_iterator_threaded_smoke_success.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      pq_global_stats.worker_execute_iterator_threaded_smoke_blocked_preflight
          .fetch_add(1, std::memory_order_relaxed);
    }
    return false;
  };

  if (!worker_plan.create_with_borrowed_worker_thd(worker, gather,
                                                   leader_join)) {
    return finish();
  }

  THD *const worker_thd = worker_plan.worker_thd();
  JOIN *const worker_join = worker_plan.worker_join();
  if (worker_thd == nullptr || worker_join == nullptr ||
      pq_clone_table_ref_preflight(worker_thd, source_tab->table_ref) ||
      pq_clone_position_scalar_preflight(source_tab) ||
      pq_clone_qep_tab_scalar_preflight(worker_join, source_tab)) {
    return finish();
  }

  if (pq_open_worker_table(&worker->m_open_ctx)) return finish();
  close_worker_table = true;

  if (pq_clone_table_scalar_preflight(worker_thd,
                                      worker->m_open_ctx.worker_table,
                                      source_tab->table()) ||
      pq_bind_qep_tab_table_preflight(worker_join,
                                      worker->m_open_ctx.worker_table,
                                      source_tab->table()) ||
      pq_attach_qep_tab_table_smoke(worker_join,
                                    worker->m_open_ctx.worker_table,
                                    source_tab->table(), &qep_tab_attach)) {
    return finish();
  }
  qep_tab_attached = true;

  QEP_TAB *const worker_qep_tab = qep_tab_attach.tab;
  if (pq_clone_worker_base_table_fields_smoke(
          worker_thd, worker_join->query_block, leader_join->query_block,
          worker->m_open_ctx.worker_table, source_tab->table())) {
    return finish();
  }

  if (source_tab->range_scan() != nullptr) {
    (void)pq_clone_range_scan_preflight(worker_thd,
                                        worker->m_open_ctx.worker_table,
                                        source_tab);
  }

  AccessPath *const worker_block_scan = NewPQblockScanAccessPath(
      worker_thd, worker->m_open_ctx.worker_table, gather, DIV_TAB,
      worker_qep_tab,
      /*need_rowid=*/false);
  if (worker_block_scan == nullptr) return finish();

  {
    auto iterator =
        CreateIteratorFromAccessPath(worker_thd, worker_block_scan, worker_join,
                                     /*eligible_for_batch_mode=*/false);
    if (iterator == nullptr) return finish();
  }

  if (!worker_plan.bind_result()) return finish();

  bool query_expression_root_owned = false;
  bool root_precheck_failed = false;
  {
    AccessPath *const saved_root_access_path = worker_join->root_access_path();
    worker_join->set_root_access_path(worker_block_scan);
    auto restore_root_access_path = create_scope_guard([&]() {
      if (query_expression_root_owned &&
          worker_join->query_expression() != nullptr) {
        worker_join->query_expression()->clear_root_access_path();
      }
      worker_join->set_root_access_path(saved_root_access_path);
    });

    if (worker_join->query_expression() == nullptr ||
        worker_join->query_expression()->create_pq_worker_root_iterator_smoke(
            worker_thd, worker_join, /*eligible_for_batch_mode=*/false)) {
      root_precheck_failed = true;
    } else {
      query_expression_root_owned = true;
      success = worker_plan.prepare_execute_iterator_unit_state() &&
                worker_plan.preflight_execute_iterator_query();
      if (success && call_execute_iterator) {
        Query_expression *const unit = worker_join->query_expression();
        if (unit == nullptr) {
          success = false;
        } else {
          const ha_rows sent_rows_before = worker_thd->get_sent_row_count();
          pq_global_stats.worker_execute_iterator_smoke_execute_query_called
              .fetch_add(1, std::memory_order_relaxed);
          success = !unit->ExecuteIteratorQuery(worker_thd);
          const ha_rows sent_rows_after = worker_thd->get_sent_row_count();
          if (success) {
            pq_global_stats.worker_execute_iterator_smoke_execute_query_success
                .fetch_add(1, std::memory_order_relaxed);
            pq_global_stats.worker_execute_iterator_smoke_execute_query_rows
                .fetch_add(sent_rows_after >= sent_rows_before
                               ? sent_rows_after - sent_rows_before
                               : 0,
                           std::memory_order_relaxed);
            pq_global_stats.worker_execute_iterator_smoke_execute_query_finishes
                .fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    }
  }
  if (root_precheck_failed) return finish();

  return finish();
}

bool pq_run_worker_thread_task(PQ_worker_info *worker,
                               Gather_operator *gather) {
  if (worker == nullptr) return true;

  switch (worker->m_task) {
    case PQ_worker_task::NOOP:
      return false;
    case PQ_worker_task::CALLBACK_LIMITED_PRODUCER:
      return pq_run_callback_limited_producer_task(worker, gather);
    case PQ_worker_task::QUERY_RESULT_MQ_PROBE:
      return pq_run_query_result_mq_probe_task(worker);
    case PQ_worker_task::EXECUTE_ITERATOR_SMOKE:
    case PQ_worker_task::EXECUTE_ITERATOR_CALL_SMOKE:
      return pq_run_worker_execute_iterator_threaded_precheck_task(worker,
                                                                  gather);
  }
  return true;
}

}  // namespace

bool Gather_operator::run_worker_callback_multirow_producer_smoke(
    THD *leader_thd, TABLE *leader_table) {
  if (leader_thd == nullptr || leader_table == nullptr || m_dop != 1) {
    return true;
  }

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  auto *exchange = get_exchange();
  if (worker == nullptr || exchange == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (worker->m_open_ctx.leader_table == nullptr) {
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.actual_dop = m_dop;
  }

  if (pq_create_worker_thd(worker, this) == nullptr) {
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return true;
  }

  bool failed = pq_open_worker_table(&worker->m_open_ctx);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
        &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
  }
  if (!failed) {
    pq_global_stats.callback_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
  }

  PQ_limited_mq_row_sink row_sink(exchange, 0, 2);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler
                 ->pq_worker_scan_callback_produce(worker->m_worker_ctx,
                                                   &row_sink) != 0;
  }
  if (!failed) {
    failed = exchange->enqueue_finish_smoke(0);
  }

  uint32 rows_read = 0;
  bool saw_eof = false;
  while (!failed && !saw_eof) {
    Exchange_nosort::Materialize_status status =
        Exchange_nosort::Materialize_status::ERROR;
    failed = exchange->materialize_next_record_image_status(leader_table,
                                                            &status);
    if (failed) break;
    switch (status) {
      case Exchange_nosort::Materialize_status::ROW:
        ++rows_read;
        break;
      case Exchange_nosort::Materialize_status::EOF_REACHED:
        saw_eof = true;
        break;
      case Exchange_nosort::Materialize_status::WOULD_BLOCK:
      case Exchange_nosort::Materialize_status::ERROR:
        failed = true;
        break;
    }
  }

  failed = failed || !saw_eof || rows_read != row_sink.rows_sent() ||
           rows_read < 2;

  if (worker->m_worker_ctx != nullptr &&
      worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->pq_worker_scan_end(
        worker->m_worker_ctx);
    worker->m_worker_ctx = nullptr;
  }
  if (worker->m_open_ctx.worker_table != nullptr) {
    pq_close_worker_table(&worker->m_open_ctx, failed);
  }
  pq_destroy_worker_thd(worker);
  leader_thd->store_globals();

  if (initialized_here) destroy();
  if (!failed) {
    pq_global_stats.callback_smoke_rows.fetch_add(rows_read,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_rows.fetch_add(rows_read,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_finishes.fetch_add(
        1, std::memory_order_relaxed);
  }
  return failed;
}

bool Gather_operator::run_worker_typed_pull_next_smoke(THD *leader_thd,
                                                       TABLE *leader_table,
                                                       uint32 min_rows) {
  if (leader_thd == nullptr || leader_table == nullptr || m_dop != 1) {
    return true;
  }

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  if (worker == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (worker->m_open_ctx.leader_table == nullptr) {
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.actual_dop = m_dop;
  }

  if (pq_create_worker_thd(worker, this) == nullptr) {
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return true;
  }

  bool failed = pq_open_worker_table(&worker->m_open_ctx);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
                 &worker->m_open_ctx, &worker->m_worker_ctx) != 0 ||
             worker->m_worker_ctx == nullptr;
  }
  if (!failed) {
    if (worker->m_open_ctx.worker_handler->ha_get_record_buffer() != nullptr) {
      pq_global_stats.worker_record_buffer_nonnull_probes.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      pq_global_stats.worker_record_buffer_null_probes.fetch_add(
          1, std::memory_order_relaxed);
    }
  }

  uint32 rows_read = 0;
  bool eof = false;
  while (!failed && !eof) {
    if (rows_read > min_rows + 1024) {
      failed = true;
      break;
    }
    const int error = worker->m_open_ctx.worker_handler->pq_worker_scan_next(
        worker->m_worker_ctx, worker->m_open_ctx.worker_table->record[0],
        &eof);
    if (error != 0) {
      failed = true;
      break;
    }
    if (!eof) ++rows_read;
  }

  failed = failed || !eof || rows_read < min_rows;

  if (worker->m_worker_ctx != nullptr &&
      worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->pq_worker_scan_end(
        worker->m_worker_ctx);
    worker->m_worker_ctx = nullptr;
  }
  if (worker->m_open_ctx.worker_table != nullptr) {
    pq_close_worker_table(&worker->m_open_ctx, failed);
  }
  pq_destroy_worker_thd(worker);
  leader_thd->store_globals();

  if (initialized_here) destroy();
  return failed;
}

bool Gather_operator::run_worker_partial_group_smoke(THD *leader_thd,
                                                     TABLE *leader_table) {
  PQ_partial_group_merge_slot_v1 merge_slots[16];
  uint32 worker_groups = 0;
  uint32 merged_groups = 0;
  const bool failed = run_worker_partial_group_merge(
      leader_thd, leader_table, 0, 1, PQ_partial_group_agg_kind::SUM,
      merge_slots, 16, &worker_groups, &merged_groups);
  if (!failed) {
    pq_global_stats.groupby_dop_partial_worker_groups.fetch_add(
        worker_groups, std::memory_order_relaxed);
    pq_global_stats.groupby_dop_partial_merged_groups.fetch_add(
        merged_groups, std::memory_order_relaxed);
  }
  return failed;
}

bool Gather_operator::run_worker_partial_group_merge(
    THD *leader_thd, TABLE *leader_table, uint32 group_field_index,
    uint32 value_field_index, PQ_partial_group_agg_kind agg_kind,
    PQ_partial_group_merge_slot_v1 *merge_slots, uint32 slot_count,
    uint32 *worker_groups, uint32 *merged_groups) {
  if (worker_groups != nullptr) *worker_groups = 0;
  if (merged_groups != nullptr) *merged_groups = 0;
  if (leader_thd == nullptr || leader_table == nullptr || m_dop == 0) {
    return true;
  }
  if (merge_slots == nullptr || slot_count == 0 || worker_groups == nullptr ||
      merged_groups == nullptr) {
    return true;
  }

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *exchange = get_exchange();
  if (exchange == nullptr ||
      exchange->get_exchange_type() != Exchange::EXCHANGE_NOSORT) {
    if (initialized_here) destroy();
    return true;
  }

  bool *rnd_inited = new (std::nothrow) bool[m_dop];
  if (rnd_inited == nullptr) {
    if (initialized_here) destroy();
    return true;
  }
  for (uint32 i = 0; i < m_dop; ++i) rnd_inited[i] = false;

  bool failed = false;
  for (uint32 i = 0; !failed && i < m_dop; ++i) {
    auto *worker = get_worker(i);
    if (worker == nullptr || worker->m_open_ctx.leader_ctx == nullptr ||
        worker->m_open_ctx.leader_table == nullptr) {
      failed = true;
      break;
    }

    if (pq_create_worker_thd(worker, this) == nullptr) {
      leader_thd->store_globals();
      failed = true;
      break;
    }
    worker->m_worker_thd->store_globals();

    failed = pq_open_worker_table(&worker->m_open_ctx);
    if (!failed) {
      failed = worker->m_open_ctx.worker_handler->ha_rnd_init(true) != 0;
      rnd_inited[i] = !failed;
    }
    if (!failed) {
      failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
          &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
    }
  }
  leader_thd->store_globals();

  if (!failed) {
    DEBUG_SYNC(leader_thd, "pq_groupby_dop_partial_workers_opened");
    failed = leader_thd->killed != THD::NOT_KILLED;
  }

  for (uint32 i = 0; !failed && i < m_dop; ++i) {
    auto *worker = get_worker(i);
    worker->m_worker_thd->store_globals();
    DBUG_EXECUTE_IF("pq_groupby_dop_partial_force_worker_error", {
      worker->m_error_code = HA_ERR_INTERNAL_ERROR;
      failed = true;
    });
    if (failed) break;
    PQ_partial_group_mq_sink group_sink(exchange, worker->m_worker_id,
                                        group_field_index, value_field_index,
                                        agg_kind);
    failed = worker->m_open_ctx.worker_handler
                 ->pq_worker_scan_callback_produce(worker->m_worker_ctx,
                                                   &group_sink) != 0;
    if (!failed && group_sink.groups_sent() > 0) {
      failed = group_sink.send_partial_group();
      if (!failed) *worker_groups += group_sink.groups_sent();
    }
    if (!failed) {
      failed = exchange->enqueue_finish_smoke(worker->m_worker_id);
    }
  }

  for (uint32 i = 0; i < m_dop; ++i) {
    auto *worker = get_worker(i);
    if (worker == nullptr) continue;
    if (worker->m_worker_thd != nullptr) {
      worker->m_worker_thd->store_globals();
    }
    if (worker->m_worker_ctx != nullptr &&
        worker->m_open_ctx.worker_handler != nullptr) {
      worker->m_open_ctx.worker_handler->pq_worker_scan_end(
          worker->m_worker_ctx);
      worker->m_worker_ctx = nullptr;
    }
    if (rnd_inited[i] && worker->m_open_ctx.worker_handler != nullptr) {
      worker->m_open_ctx.worker_handler->ha_rnd_end();
    }
    if (worker->m_open_ctx.worker_table != nullptr) {
      pq_close_worker_table(&worker->m_open_ctx, failed);
    }
    pq_destroy_worker_thd(worker);
  }
  leader_thd->store_globals();

  uint32 payloads_read = 0;
  while (!failed) {
    MQMessageType type;
    void *datap = nullptr;
    uint32 data_len = 0;
    const bool got_message = exchange->read_mq_message(type, &datap, data_len);

    if (!got_message) {
      if (exchange->all_done()) break;
      failed = true;
      break;
    }

    if (type == MQMessageType::PARTIAL_GROUP) {
      const PQ_partial_group_payload_v1 *payload = nullptr;
      failed = pq_validate_partial_group_payload_v1(datap, data_len, m_dop,
                                                    &payload) ||
               pq_merge_partial_group_payload_v1(*payload, merge_slots,
                                                 slot_count, merged_groups);
      if (!failed) ++payloads_read;
      continue;
    }

    if (type != MQMessageType::FINISH) {
      failed = true;
      break;
    }
  }

  failed = failed || payloads_read != *worker_groups ||
           (*worker_groups > 0 &&
            (*merged_groups == 0 || *merged_groups > *worker_groups));

  delete[] rnd_inited;
  if (initialized_here) destroy();
  return failed;
}

bool Gather_operator::run_worker_callback_limited_producer(
    THD *leader_thd, TABLE *leader_table, uint32 max_rows,
    uint32 *rows_sent) {
  if (rows_sent != nullptr) *rows_sent = 0;
  if (leader_thd == nullptr || leader_table == nullptr || m_dop != 1 ||
      max_rows == 0 || rows_sent == nullptr) {
    return true;
  }

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  auto *exchange = get_exchange();
  if (worker == nullptr || exchange == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (worker->m_open_ctx.leader_table == nullptr) {
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.actual_dop = m_dop;
  }

  if (pq_create_worker_thd(worker, this) == nullptr) {
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return true;
  }

  bool failed = pq_open_worker_table(&worker->m_open_ctx);
  bool rnd_inited = false;
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->ha_rnd_init(true) != 0;
    rnd_inited = !failed;
  }
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
        &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
  }
  if (!failed) {
    pq_global_stats.callback_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
  }

  PQ_limited_mq_row_sink row_sink(exchange, 0, max_rows);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler
                 ->pq_worker_scan_callback_produce(worker->m_worker_ctx,
                                                   &row_sink) != 0;
  }
  if (!failed) {
    failed = exchange->enqueue_finish_smoke(0);
  }

  if (worker->m_worker_ctx != nullptr &&
      worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->pq_worker_scan_end(
        worker->m_worker_ctx);
    worker->m_worker_ctx = nullptr;
  }
  if (rnd_inited && worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->ha_rnd_end();
  }
  if (worker->m_open_ctx.worker_table != nullptr) {
    pq_close_worker_table(&worker->m_open_ctx, failed);
  }
  pq_destroy_worker_thd(worker);
  leader_thd->store_globals();

  if (initialized_here) destroy();
  if (!failed) {
    *rows_sent = row_sink.rows_sent();
    pq_global_stats.callback_smoke_rows.fetch_add(*rows_sent,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_rows.fetch_add(*rows_sent,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_finishes.fetch_add(
        1, std::memory_order_relaxed);
  }
  return failed;
}

bool Gather_operator::prepare_leader_row_stream_smoke(
    THD *leader_thd, TABLE *leader_table, PQ_Leader_context *leader_ctx,
    uint32 row_limit, uint32 *rows_enqueued) {
  if (rows_enqueued != nullptr) *rows_enqueued = 0;
  if (leader_thd == nullptr || leader_table == nullptr || leader_ctx == nullptr ||
      row_limit == 0 || rows_enqueued == nullptr || m_dop != 1) {
    return true;
  }

  if (!m_initialized && init()) return true;
  if (configure_worker_open_contexts(leader_table, leader_ctx, m_dop)) {
    return true;
  }

  return run_worker_callback_limited_producer(leader_thd, leader_table,
                                              row_limit, rows_enqueued);
}

bool Gather_operator::prepare_leader_row_stream_error_smoke(
    THD *leader_thd, TABLE *leader_table, PQ_Leader_context *leader_ctx) {
  if (leader_thd == nullptr || leader_table == nullptr ||
      leader_ctx == nullptr || m_dop != 1) {
    return true;
  }

  if (!m_initialized && init()) return true;
  if (configure_worker_open_contexts(leader_table, leader_ctx, m_dop)) {
    return true;
  }

  Exchange_nosort *exchange = get_exchange();
  auto *worker = get_worker(0);
  if (exchange == nullptr || worker == nullptr) return true;

  if (!worker->transition_status(PQ_Worker_status::RUNNING)) return true;
  int worker_error_code = HA_ERR_INTERNAL_ERROR;
  DBUG_EXECUTE_IF("pq_leader_row_stream_error_smoke_out_of_mem", {
    worker_error_code = HA_ERR_OUT_OF_MEM;
  });
  worker->m_error_code = worker_error_code;
  const bool failed = exchange->enqueue_error_smoke(0);
  if (!worker->transition_status(PQ_Worker_status::ERROR)) return true;
  return failed;
}

bool Gather_operator::run_worker_callback_threaded_producer(
    THD *leader_thd, TABLE *leader_table, uint32 max_rows) {
  if (leader_thd == nullptr || leader_table == nullptr || m_dop == 0 ||
      max_rows == 0 || !m_initialized || m_thread_budget_acquired) {
    return true;
  }

  auto *exchange = get_exchange();
  if (exchange == nullptr ||
      exchange->get_exchange_type() != Exchange::EXCHANGE_NOSORT) {
    return true;
  }

  for (uint32 i = 0; i < m_dop; ++i) {
    auto *worker = get_worker(i);
    if (worker == nullptr) return true;

    if (worker->m_open_ctx.leader_table == nullptr) {
      worker->m_open_ctx.leader_table = leader_table;
      worker->m_open_ctx.actual_dop = m_dop;
    }

    worker->m_task = PQ_worker_task::CALLBACK_LIMITED_PRODUCER;
    worker->m_task_max_rows = max_rows;
    worker->m_task_force_error = false;
    DBUG_EXECUTE_IF("pq_read_threaded_shadow_force_worker_error",
                    worker->m_task_force_error = (i == 0););
    worker->m_task_rows_sent.store(0, std::memory_order_release);
  }

  bool enforce_thread_budget = parallel_max_threads > 0;
  DBUG_EXECUTE_IF("pq_read_threaded_force_thread_budget_refuse", {
    enforce_thread_budget = true;
  });
  if (enforce_thread_budget) {
    if (!check_pq_running_threads(m_dop, 0)) {
      return true;
    }
    m_thread_budget_acquired = true;
  }

  if (start_workers(leader_thd)) {
    if (m_thread_budget_acquired) {
      release_pq_running_threads(m_dop);
      m_thread_budget_acquired = false;
    }
    for (uint32 i = 0; i < m_dop; ++i) {
      auto *worker = get_worker(i);
      if (worker != nullptr) worker->m_task = PQ_worker_task::NOOP;
    }
    return true;
  }

  pq_global_stats.workers_launched.fetch_add(m_dop, std::memory_order_relaxed);
  return false;
}

// ---------------------------------------------------------------------------
// Gather_operator: abort_workers (stub)
// ---------------------------------------------------------------------------

void Gather_operator::abort_workers(THD *leader_thd [[maybe_unused]]) {
  if (!m_initialized) return;

  // Phase 4 stub: abort worker manager + abort MQ consumer side.
  // In production (Phase 5), this will also:
  // - Send ABORT token to each worker's MQ
  // - Set THD::killed on leader and worker THDs
  m_worker_mgr.abort(m_workers, m_dop);

  // Abort MQ consumer side on all handles.
  // This tells workers that the leader is no longer reading.
  if (m_exchange != nullptr) {
    for (uint32 i = 0; i < m_dop; i++) {
      MQueue_handle *handle = m_exchange->get_mq_handle(i);
      if (handle != nullptr) {
        handle->abort_consumer();
      }
    }
  }

  m_all_finished = true;
}

// ---------------------------------------------------------------------------
// Gather_operator: resolve_error_priority
// ---------------------------------------------------------------------------

Gather_operator::GatherErrorState Gather_operator::resolve_error_priority(
    THD *leader_thd) {
  GatherErrorState result;
  result.reset();

  // ================================================================
  // Priority 1: KILL -- check leader THD killed flag.
  // Phase 8: now actually checks THD::killed instead of stub comment.
  // If the leader is killed (KILL QUERY, KILL CONNECTION, or
  // max_execution_time exceeded), this takes absolute priority.
  // ================================================================
  if (leader_thd != nullptr && leader_thd->killed != 0) {
    result.priority = PRIORITY_KILL;
    result.error_code = ER_QUERY_INTERRUPTED;
    result.source_worker_id = 0;  // Kill originated from leader, not a worker
    m_error_state = result;
    return result;
  }

  // ================================================================
  // Priority 2: LEADER_FATAL -- check for OOM or unrecoverable error.
  // Phase 8: checks THD::pq_error for leader-side errors.
  // ================================================================
  if (leader_thd != nullptr && leader_thd->pq_error != 0) {
    if (result.priority < PRIORITY_LEADER_FATAL) {
      result.priority = PRIORITY_LEADER_FATAL;
      result.error_code = leader_thd->pq_error;
      result.source_worker_id = 0;  // Leader-side error
    }
  }

  // ================================================================
  // Priority 3: WORKER_FATAL -- check each worker's error code.
  // ================================================================
  for (uint32 i = 0; i < m_dop; i++) {
    if (m_workers[i] == nullptr) continue;

    const PQ_Worker_status status =
        m_workers[i]->m_status.load(std::memory_order_acquire);
    if (status == PQ_Worker_status::ERROR ||
        status == PQ_Worker_status::KILLED) {
      // Worker fatal: update result if higher priority than current.
      if (result.priority < PRIORITY_WORKER_FATAL) {
        result.priority = PRIORITY_WORKER_FATAL;
        result.error_code = m_workers[i]->m_error_code;
        result.source_worker_id = m_workers[i]->m_worker_id;
      }
    }
  }

  // ================================================================
  // Priority 4: MQ_CLOSED -- check if Exchange reports all workers done.
  // ================================================================
  if (m_all_finished && !result.has_error()) {
    result.priority = PRIORITY_MQ_CLOSED;
    result.error_code = 0;
  }

  // ================================================================
  // Priority 5: NORMAL_FINISH -- default if no errors found.
  // ================================================================
  if (!result.has_error() && !m_all_finished) {
    result.priority = PRIORITY_NORMAL_FINISH;
  }

  m_error_state = result;
  return result;
}

// ---------------------------------------------------------------------------
// Gather_operator: check_leader_kill
// ---------------------------------------------------------------------------

bool Gather_operator::check_leader_kill(THD *leader_thd) {
  if (leader_thd == nullptr) return false;

  // Check THD::killed flag (set by KILL QUERY, KILL CONNECTION, or shutdown).
  if (leader_thd->killed != 0) {
    return true;
  }

  // Check max_execution_time exceeded.
  // In MySQL 8.0, max_execution_time is checked via
  // THD->get_stmt_da()->statement_warn_area().has_warn_expr().
  // For simplicity, Phase 8 checks if max_execution_time_set > 0 and
  // the timeout has been exceeded. The actual timeout logic is handled
  // by MySQL's execution timer; here we just check the killed flag
  // which is also set on timeout.
  // Note: MySQL sets THD::killed = THD::KILL_QUERY on max_execution_time
  // timeout, so the killed check above already covers this.

  return false;
}

// ---------------------------------------------------------------------------
// Gather_operator: propagate_kill_to_workers
// ---------------------------------------------------------------------------

void Gather_operator::propagate_kill_to_workers(THD *leader_thd
                                                 [[maybe_unused]]) {
  if (!m_initialized) return;

  // Set all worker statuses to KILLED.
  for (uint32 i = 0; i < m_dop; i++) {
    if (m_workers[i] == nullptr) continue;
    if (!m_workers[i]->is_terminal()) {
      m_workers[i]->transition_status(PQ_Worker_status::KILLED);
    }
  }

  // Close MQ producer side on all handles to signal workers that
  // the leader has stopped reading. This causes workers to detect
  // MQ_DETACHED on their next send() call and exit.
  if (m_exchange != nullptr) {
    for (uint32 i = 0; i < m_dop; i++) {
      MQueue_handle *handle = m_exchange->get_mq_handle(i);
      if (handle != nullptr) {
        handle->abort_consumer();
      }
    }
  }

  // Phase 5+ will also:
  // - Set THD::killed on each worker THD
  // - Send ABORT control token to each worker's MQ

  m_all_finished = true;
}
