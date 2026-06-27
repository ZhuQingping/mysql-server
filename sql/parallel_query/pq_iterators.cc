/* Copyright (c) 2026, Oracle and/or its affiliates.

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

#include "sql/parallel_query/pq_iterators.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "include/my_sqlcommand.h"
#include "my_base.h"
#include "my_bitmap.h"
#include "my_dbug.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/iterators/ref_row_iterators.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/mysqld.h"
#include "sql/parallel_query/exchange_sort.h"
#include "sql/parallel_query/pq_handler.h"
#include "sql/parallel_query/query_result_mq.h"
#include "sql/parallel_query/sql_parallel.h"
#include "sql/range_optimizer/index_range_scan.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/sql_class.h"
#include "sql/sql_executor.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"

namespace {

constexpr double kPQSecondaryCoveringMaxEstimatedRows = 64.0;
constexpr double kPQPrimaryClusteredMaxEstimatedRows = 64.0;
constexpr uint kPQPrimaryClusteredMaxBufferedRows = 64;
constexpr size_t kPQSecondaryDependentRefMaxBufferedRows = 1024;
constexpr uint kPQSecondaryNoncoveringIcpMaxRows = 64;
constexpr uint kPQSecondaryNoncoveringRangeMaxRows = 64;
constexpr uint64 kPQParallelScanIteratorReadWaitTimeoutUs = 1000;

struct PQ_copied_key_endpoint {
  key_range range{};
  std::vector<uchar> key;
  bool present{false};
};

struct PQ_worker_constant_ref_context_shape {
  std::vector<uchar> owned_key;
  uint keyno{MAX_KEY};
  key_part_map keypart_map{0};
  uint key_length{0};
  bool exact_read{false};
  bool reverse{false};
  bool constant_ref{false};
  bool owned_key_bytes{false};
};

class PQ_worker_constant_ref_cleanup_smoke {
 public:
  explicit PQ_worker_constant_ref_cleanup_smoke(
      const PQ_worker_constant_ref_context_shape &ctx, bool *cleanup_reached)
      : m_owned_key(ctx.owned_key),
        m_keyno(ctx.keyno),
        m_keypart_map(ctx.keypart_map),
        m_key_length(ctx.key_length),
        m_valid(ctx.owned_key_bytes && ctx.exact_read && ctx.constant_ref &&
                !ctx.reverse && ctx.key_length == ctx.owned_key.size()),
        m_cleanup_reached(cleanup_reached) {}

  PQ_worker_constant_ref_cleanup_smoke(
      const PQ_worker_constant_ref_cleanup_smoke &) = delete;
  PQ_worker_constant_ref_cleanup_smoke &operator=(
      const PQ_worker_constant_ref_cleanup_smoke &) = delete;

  ~PQ_worker_constant_ref_cleanup_smoke() {
    m_owned_key.clear();
    m_keyno = MAX_KEY;
    m_keypart_map = 0;
    m_key_length = 0;
    m_valid = false;
    if (m_cleanup_reached != nullptr) {
      *m_cleanup_reached = true;
    }
  }

  bool valid() const { return m_valid; }
  size_t owned_key_size() const { return m_owned_key.size(); }

 private:
  std::vector<uchar> m_owned_key;
  uint m_keyno{MAX_KEY};
  key_part_map m_keypart_map{0};
  uint m_key_length{0};
  bool m_valid{false};
  bool *m_cleanup_reached{nullptr};
};

bool pq_run_worker_ref_contract_smoke(
    THD *thd, TABLE *leader_table,
    const PQ_worker_constant_ref_context_shape &ctx, bool inject_failure) {
  pq_global_stats.worker_ref_contract_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (thd == nullptr || leader_table == nullptr || leader_table->file == nullptr ||
      !ctx.owned_key_bytes || !ctx.exact_read || !ctx.constant_ref ||
      ctx.reverse || ctx.owned_key.empty() ||
      ctx.owned_key.size() != ctx.key_length) {
    pq_global_stats.worker_ref_contract_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }

  Gather_operator gather(1);
  PQ_worker_info *worker = nullptr;
  bool cleanup_reached = false;
  bool cleanup_error = false;
  auto cleanup = [&]() {
    if (cleanup_reached) return;
    cleanup_reached = true;
    if (worker != nullptr) {
      if (worker->m_worker_ctx != nullptr &&
          worker->m_open_ctx.worker_handler != nullptr) {
        (void)worker->m_open_ctx.worker_handler->pq_worker_scan_end(
            worker->m_worker_ctx);
        worker->m_worker_ctx = nullptr;
      }
      if (worker->m_open_ctx.worker_table != nullptr) {
        pq_close_worker_table(&worker->m_open_ctx, cleanup_error);
      }
      if (worker->m_worker_thd != nullptr) {
        pq_destroy_worker_thd(worker);
        thd->store_globals();
      }
    }
    gather.destroy();
    pq_global_stats.worker_ref_contract_cleanup.fetch_add(
        1, std::memory_order_relaxed);
  };

  bool failed = gather.init();
  worker = gather.get_worker(0);
  if (!failed && worker != nullptr) {
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.actual_dop = 1;
  }
  failed = failed || worker == nullptr ||
           pq_create_worker_thd(worker, &gather) == nullptr;
  if (failed) {
    cleanup_error = true;
    cleanup();
    pq_global_stats.worker_ref_contract_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }

  failed = pq_open_worker_table(&worker->m_open_ctx);

  const bool ownership_ok =
      !failed && worker->m_open_ctx.worker_thd == worker->m_worker_thd &&
      worker->m_open_ctx.worker_table != nullptr &&
      worker->m_open_ctx.worker_handler != nullptr &&
      worker->m_open_ctx.worker_table != leader_table &&
      worker->m_open_ctx.worker_handler != leader_table->file &&
      worker->m_open_ctx.worker_table->file ==
          worker->m_open_ctx.worker_handler &&
      worker->m_open_ctx.worker_table->record[0] != leader_table->record[0] &&
      worker->m_open_ctx.worker_table->in_use == worker->m_worker_thd &&
      worker->m_worker_ctx == nullptr;

  if (failed || !ownership_ok) {
    cleanup_error = true;
    cleanup();
    pq_global_stats.worker_ref_contract_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }

  std::vector<uchar> worker_lookup(ctx.owned_key.begin(), ctx.owned_key.end());
  if (worker_lookup.size() != ctx.key_length) {
    cleanup_error = true;
    cleanup();
    pq_global_stats.worker_ref_contract_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }

  if (inject_failure) {
    pq_global_stats.worker_ref_contract_failures.fetch_add(
        1, std::memory_order_relaxed);
    cleanup_error = true;
    cleanup();
    return false;
  }

  pq_global_stats.worker_ref_contract_lookup_bytes.fetch_add(
      worker_lookup.size(), std::memory_order_relaxed);
  pq_global_stats.worker_ref_contract_ownership_success.fetch_add(
      1, std::memory_order_relaxed);
  pq_global_stats.worker_ref_contract_no_row_success.fetch_add(
      1, std::memory_order_relaxed);
  pq_global_stats.worker_ref_contract_success.fetch_add(
      1, std::memory_order_relaxed);

  cleanup();
  return false;
}

bool pq_copy_key_endpoint(const key_range &src, PQ_copied_key_endpoint *dst) {
  if (dst == nullptr) return false;
  dst->range = src;
  dst->key.clear();
  dst->present = src.keypart_map != 0;
  if (!dst->present) {
    dst->range.key = nullptr;
    dst->range.length = 0;
    return true;
  }
  if (src.length > 0 && src.key == nullptr) return false;
  dst->key.assign(src.key, src.key + src.length);
  dst->range.key = dst->key.empty() ? nullptr : dst->key.data();
  return true;
}

bool pq_build_worker_constant_ref_context_shape(
    TABLE *table, const Index_lookup *ref, const key_range &ref_key,
    PQ_worker_constant_ref_context_shape *ctx) {
  if (ctx == nullptr) return true;

  *ctx = PQ_worker_constant_ref_context_shape{};
  if (table == nullptr || table->s == nullptr || ref == nullptr ||
      ref->key < 0 || static_cast<uint>(ref->key) >= table->s->keys ||
      ref_key.key == nullptr || ref_key.length == 0 ||
      ref_key.keypart_map == 0 || ref_key.flag != HA_READ_KEY_EXACT ||
      ref->depend_map != 0) {
    return true;
  }

  ctx->keyno = static_cast<uint>(ref->key);
  ctx->keypart_map = ref_key.keypart_map;
  ctx->key_length = ref_key.length;
  ctx->exact_read = true;
  ctx->reverse = false;
  ctx->constant_ref = true;
  ctx->owned_key.assign(ref_key.key, ref_key.key + ref_key.length);
  ctx->owned_key_bytes =
      !ctx->owned_key.empty() && ctx->owned_key.data() != ref_key.key;

  return !ctx->owned_key_bytes;
}

bool pq_secondary_covering_field_type_is_safe(const Field *field) {
  if (field == nullptr || field->is_nullable() || field->is_gcol() ||
      field->is_hidden() || field->is_field_for_functional_index()) {
    return false;
  }

  switch (field->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_YEAR:
      return true;
    default:
      return false;
  }
}

bool pq_parallel_scan_iterator_row_value_shape_is_safe(const TABLE *table) {
  if (table == nullptr || table->s == nullptr || table->file == nullptr ||
      table->field == nullptr || table->s->fields != 2 ||
      table->s->blob_fields != 0 || table->s->reclength == 0 ||
      table->s->primary_key != MAX_KEY) {
    return false;
  }

  for (uint i = 0; i < table->s->fields; ++i) {
    const Field *field = table->field[i];
    if (field == nullptr || field->is_nullable() ||
        field->real_type() != MYSQL_TYPE_LONG) {
      return false;
    }
  }
  return true;
}

bool pq_secondary_covering_read_set_is_safe(const TABLE *table, uint keyno) {
  if (table == nullptr || table->s == nullptr || table->key_info == nullptr ||
      table->field == nullptr || table->read_set == nullptr ||
      keyno >= table->s->keys || keyno == table->s->primary_key) {
    return false;
  }

  const KEY &key = table->key_info[keyno];
  if (key.flags & (HA_SPATIAL | HA_MULTI_VALUED_KEY)) {
    return false;
  }

  bool saw_read_field = false;
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    if (field == nullptr || !bitmap_is_set(table->read_set, i)) {
      continue;
    }

    saw_read_field = true;
    if (!pq_secondary_covering_field_type_is_safe(field)) {
      return false;
    }

    bool found_full_keypart = false;
    for (uint part = 0; part < key.user_defined_key_parts; ++part) {
      const KEY_PART_INFO &key_part = key.key_part[part];
      if (key_part.field != field) {
        continue;
      }

      if ((key_part.key_part_flag &
           (HA_REVERSE_SORT | HA_PART_KEY_SEG | HA_VAR_LENGTH_PART |
            HA_BLOB_PART | HA_BIT_PART)) != 0 ||
          key_part.length != field->key_length()) {
        return false;
      }

      found_full_keypart = true;
      break;
    }

    if (!found_full_keypart) {
      return false;
    }
  }

  return saw_read_field;
}

bool pq_secondary_noncovering_clustered_read_set_is_safe(const TABLE *table,
                                                         uint keyno) {
  if (table == nullptr || table->s == nullptr || table->key_info == nullptr ||
      table->field == nullptr || table->read_set == nullptr ||
      keyno >= table->s->keys || keyno == table->s->primary_key) {
    return false;
  }

  const KEY &key = table->key_info[keyno];
  if (key.flags & (HA_SPATIAL | HA_MULTI_VALUED_KEY)) {
    return false;
  }

  bool saw_read_field = false;
  bool saw_noncovered_field = false;
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    if (field == nullptr || !bitmap_is_set(table->read_set, i)) {
      continue;
    }

    saw_read_field = true;
    if (!pq_secondary_covering_field_type_is_safe(field)) {
      return false;
    }

    bool found_full_keypart = false;
    for (uint part = 0; part < key.user_defined_key_parts; ++part) {
      const KEY_PART_INFO &key_part = key.key_part[part];
      if (key_part.field != field) {
        continue;
      }

      if ((key_part.key_part_flag &
           (HA_REVERSE_SORT | HA_PART_KEY_SEG | HA_VAR_LENGTH_PART |
            HA_BLOB_PART | HA_BIT_PART)) != 0 ||
          key_part.length != field->key_length()) {
        return false;
      }

      found_full_keypart = true;
      break;
    }

    if (!found_full_keypart) {
      saw_noncovered_field = true;
    }
  }

  return saw_read_field && saw_noncovered_field;
}

bool pq_secondary_key_parts_are_safe(const TABLE *table, uint keyno) {
  if (table == nullptr || table->s == nullptr || table->key_info == nullptr ||
      keyno >= table->s->keys || keyno == table->s->primary_key) {
    return false;
  }

  const KEY &key = table->key_info[keyno];
  if (key.flags & (HA_SPATIAL | HA_MULTI_VALUED_KEY)) {
    return false;
  }

  for (uint part = 0; part < key.user_defined_key_parts; ++part) {
    const KEY_PART_INFO &key_part = key.key_part[part];
    Field *field = key_part.field;
    if (!pq_secondary_covering_field_type_is_safe(field)) {
      return false;
    }
    if ((key_part.key_part_flag &
         (HA_REVERSE_SORT | HA_PART_KEY_SEG | HA_VAR_LENGTH_PART |
          HA_BLOB_PART | HA_BIT_PART)) != 0 ||
        field == nullptr || key_part.length != field->key_length()) {
      return false;
    }
  }

  return true;
}

bool pq_primary_clustered_key_parts_are_safe(const TABLE *table, uint keyno) {
  if (table == nullptr || table->s == nullptr || table->key_info == nullptr ||
      keyno >= table->s->keys || keyno != table->s->primary_key) {
    return false;
  }

  const KEY &key = table->key_info[keyno];
  if (key.user_defined_key_parts != 1 ||
      (key.flags & (HA_SPATIAL | HA_MULTI_VALUED_KEY)) != 0) {
    return false;
  }

  for (uint part = 0; part < key.user_defined_key_parts; ++part) {
    const KEY_PART_INFO &key_part = key.key_part[part];
    Field *field = key_part.field;
    if (!pq_secondary_covering_field_type_is_safe(field)) {
      return false;
    }
    if ((key_part.key_part_flag &
         (HA_REVERSE_SORT | HA_PART_KEY_SEG | HA_VAR_LENGTH_PART |
          HA_BLOB_PART | HA_BIT_PART)) != 0 ||
        field == nullptr || key_part.length != field->key_length()) {
      return false;
    }
  }

  return true;
}

bool pq_primary_clustered_read_set_is_safe(const TABLE *table) {
  if (table == nullptr || table->s == nullptr || table->field == nullptr ||
      table->read_set == nullptr) {
    return false;
  }

  bool saw_read_field = false;
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    if (field == nullptr || !bitmap_is_set(table->read_set, i)) {
      continue;
    }

    saw_read_field = true;
    if (field->is_gcol() || field->is_hidden() ||
        field->is_field_for_functional_index() ||
        field->is_flag_set(BLOB_FLAG) || field->type() == MYSQL_TYPE_GEOMETRY) {
      return false;
    }
  }

  return saw_read_field;
}

bool pq_primary_clustered_range_endpoints_are_safe(QUICK_RANGE *range) {
  if (range == nullptr) {
    return false;
  }

  key_range start_key;
  key_range end_key;
  range->make_min_endpoint(&start_key);
  range->make_max_endpoint(&end_key);

  if (start_key.keypart_map != 0 && start_key.flag != HA_READ_KEY_OR_NEXT &&
      start_key.flag != HA_READ_AFTER_KEY &&
      start_key.flag != HA_READ_KEY_EXACT) {
    return false;
  }
  if (end_key.keypart_map != 0 && end_key.flag != HA_READ_BEFORE_KEY &&
      end_key.flag != HA_READ_AFTER_KEY &&
      end_key.flag != HA_READ_KEY_EXACT) {
    return false;
  }
  return true;
}

bool pq_secondary_ref_is_constant_and_safe(TABLE *table,
                                           const Index_lookup *ref) {
  if (table == nullptr || table->s == nullptr || ref == nullptr ||
      ref->key < 0 || static_cast<uint>(ref->key) >= table->s->keys ||
      static_cast<uint>(ref->key) == table->s->primary_key ||
      ref->key_parts == 0 || ref->key_length == 0 ||
      ref->key_buff == nullptr || ref->key_copy == nullptr ||
      ref->cond_guards == nullptr || ref->depend_map != 0 ||
      ref->disable_cache || ref->keypart_hash != nullptr) {
    return false;
  }

  for (uint part = 0; part < ref->key_parts; ++part) {
    if (ref->key_copy[part] != nullptr || ref->cond_guards[part] != nullptr) {
      return false;
    }
  }

  const key_part_map keypart_map = make_prev_keypart_map(ref->key_parts);
  return calculate_key_len(table, static_cast<uint>(ref->key), keypart_map) ==
         ref->key_length;
}

bool pq_secondary_ref_is_dependent_scaffold_candidate(TABLE *table,
                                                      const Index_lookup *ref) {
  if (table == nullptr || table->s == nullptr || table->file == nullptr ||
      table->key_info == nullptr || table->s->db_type() != innodb_hton ||
      table->part_info != nullptr || table->file->pushed_idx_cond != nullptr ||
      ref == nullptr || ref->key < 0 ||
      static_cast<uint>(ref->key) >= table->s->keys ||
      static_cast<uint>(ref->key) == table->s->primary_key ||
      ref->key_parts == 0 || ref->key_length == 0 ||
      ref->key_buff == nullptr || ref->key_copy == nullptr ||
      ref->cond_guards == nullptr || ref->depend_map == 0 ||
      ref->disable_cache || ref->keypart_hash != nullptr) {
    return false;
  }

  for (uint part = 0; part < ref->key_parts; ++part) {
    if (ref->cond_guards[part] != nullptr) {
      return false;
    }
  }

  const uint keyno = static_cast<uint>(ref->key);
  const key_part_map keypart_map = make_prev_keypart_map(ref->key_parts);
  return calculate_key_len(table, keyno, keypart_map) == ref->key_length &&
         pq_secondary_key_parts_are_safe(table, keyno) &&
         pq_secondary_covering_read_set_is_safe(table, keyno);
}

bool pq_secondary_dependent_ref_gate_is_safe(const JOIN *join,
                                             const AccessPath *path,
                                             bool reverse) {
  return join != nullptr && path != nullptr && !reverse &&
         join->primary_tables == 2 && join->const_tables == 0 &&
         join->query_block != nullptr &&
         join->query_block->is_simple_query_block() &&
         !join->query_block->is_explicitly_grouped() &&
         join->query_block->having_cond() == nullptr &&
         path->num_output_rows() <= kPQSecondaryCoveringMaxEstimatedRows;
}

void pq_record_buffer_probe(TABLE *table) {
  if (table != nullptr && table->file != nullptr &&
      table->file->ha_get_record_buffer() != nullptr) {
    pq_global_stats.secondary_record_buffer_nonnull_probes.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    pq_global_stats.secondary_record_buffer_null_probes.fetch_add(
        1, std::memory_order_relaxed);
  }
}

void pq_secondary_reverse_ref_reject_probe() {
  pq_global_stats.secondary_reverse_reject_probes.fetch_add(
      1, std::memory_order_relaxed);
  pq_global_stats.secondary_reverse_ref_reject_probes.fetch_add(
      1, std::memory_order_relaxed);
}

class PQ_record_buffer_sink final : public PQ_row_sink {
 public:
  PQ_record_buffer_sink(TABLE *leader_table,
                        std::vector<std::vector<uchar>> *rows)
      : m_leader_table(leader_table), m_rows(rows) {}

  bool send_row(TABLE *source_table) override {
    if (m_failed || source_table == nullptr || source_table != m_leader_table ||
        source_table->s == nullptr || source_table->record[0] == nullptr ||
        source_table->s->reclength == 0 || m_rows == nullptr) {
      m_failed = true;
      return true;
    }

    const uchar *record = source_table->record[0];
    m_rows->emplace_back(record, record + source_table->s->reclength);
    return false;
  }

  bool should_abort() const override { return m_failed; }

 private:
  TABLE *const m_leader_table;
  std::vector<std::vector<uchar>> *const m_rows;
  bool m_failed{false};
};

class PQSecondaryCoveringRangeIterator final : public TableRowIterator {
 public:
  PQSecondaryCoveringRangeIterator(THD *thd, MEM_ROOT *mem_root, TABLE *table,
                                   ha_rows *examined_rows,
                                   double expected_rows, uint index_arg,
                                   bool need_rows_in_rowid_order,
                                   bool reuse_handler, uint mrr_flags,
                                   uint mrr_buf_size,
                                   Bounds_checked_array<QUICK_RANGE *> ranges)
      : TableRowIterator(thd, table),
        m_mem_root(mem_root),
        m_examined_rows(examined_rows),
        m_expected_rows(expected_rows),
        m_index(index_arg),
        m_need_rows_in_rowid_order(need_rows_in_rowid_order),
        m_reuse_handler(reuse_handler),
        m_mrr_flags(mrr_flags),
        m_mrr_buf_size(mrr_buf_size),
        m_ranges(ranges) {}

  bool Init() override {
    m_rows.clear();
    m_pos = 0;
    m_executed_counted = false;
    m_serial_iterator = nullptr;

    if (m_ranges.size() != 1 || table() == nullptr || table()->file == nullptr ||
        table()->s == nullptr || table()->s->reclength == 0) {
      return fallback_to_serial();
    }

    key_range start_key;
    key_range end_key;
    QUICK_RANGE *range = m_ranges[0];
    if (range == nullptr) {
      return fallback_to_serial();
    }
    range->make_min_endpoint(&start_key);
    range->make_max_endpoint(&end_key);

    PQ_copied_key_endpoint copied_start;
    PQ_copied_key_endpoint copied_end;
    if (!pq_copy_key_endpoint(start_key, &copied_start) ||
        !pq_copy_key_endpoint(end_key, &copied_end)) {
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }

    pq_record_buffer_probe(table());
    PQ_record_buffer_sink sink(table(), &m_rows);
    uint row_count = 0;
    const int error = table()->file->pq_secondary_covering_range_produce(
        thd(), m_index, copied_start.present ? &copied_start.range : nullptr,
        copied_end.present ? &copied_end.range : nullptr, &sink, &row_count);

    if (error == HA_ERR_UNSUPPORTED) {
      return fallback_to_serial();
    }

    if (error != 0) {
      PrintError(error);
      return true;
    }

    if (row_count != m_rows.size()) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
    pq_global_stats.queries_executed.fetch_add(1, std::memory_order_relaxed);
    pq_global_stats.secondary_rows_produced.fetch_add(row_count,
                                                      std::memory_order_relaxed);
    pq_global_stats.rows_scanned.fetch_add(row_count, std::memory_order_relaxed);
    m_executed_counted = true;
    return false;
  }

  int Read() override {
    if (m_serial_iterator != nullptr) {
      return m_serial_iterator->Read();
    }

    if (m_pos >= m_rows.size()) {
      table()->set_no_row();
      return -1;
    }

    std::memcpy(table()->record[0], m_rows[m_pos].data(),
                table()->s->reclength);
    ++m_pos;
    if (m_examined_rows != nullptr) {
      ++*m_examined_rows;
    }
    return 0;
  }

  void UnlockRow() override {
    if (m_serial_iterator != nullptr) {
      m_serial_iterator->UnlockRow();
      return;
    }
    TableRowIterator::UnlockRow();
  }

 private:
  bool fallback_to_serial() {
    pq_set_execution_state(thd(), PQ_execution_state::FALLBACK_SERIAL);
    pq_global_stats.queries_fallback.fetch_add(1, std::memory_order_relaxed);
    m_serial_iterator = NewIterator<IndexRangeScanIterator>(
        thd(), m_mem_root, table(), m_examined_rows, m_expected_rows, m_index,
        m_need_rows_in_rowid_order, m_reuse_handler, m_mem_root, m_mrr_flags,
        m_mrr_buf_size, m_ranges);
    return m_serial_iterator == nullptr || m_serial_iterator->Init();
  }

  MEM_ROOT *const m_mem_root;
  ha_rows *const m_examined_rows;
  const double m_expected_rows;
  const uint m_index;
  const bool m_need_rows_in_rowid_order;
  const bool m_reuse_handler;
  const uint m_mrr_flags;
  const uint m_mrr_buf_size;
  Bounds_checked_array<QUICK_RANGE *> m_ranges;
  std::vector<std::vector<uchar>> m_rows;
  size_t m_pos{0};
  bool m_executed_counted{false};
  unique_ptr_destroy_only<RowIterator> m_serial_iterator;
};

class PQPrimaryClusteredRangeIterator final : public TableRowIterator {
 public:
  PQPrimaryClusteredRangeIterator(THD *thd, MEM_ROOT *mem_root, TABLE *table,
                                  ha_rows *examined_rows, double expected_rows,
                                  uint index_arg,
                                  bool need_rows_in_rowid_order,
                                  bool reuse_handler, uint mrr_flags,
                                  uint mrr_buf_size,
                                  Bounds_checked_array<QUICK_RANGE *> ranges)
      : TableRowIterator(thd, table),
        m_mem_root(mem_root),
        m_examined_rows(examined_rows),
        m_expected_rows(expected_rows),
        m_index(index_arg),
        m_need_rows_in_rowid_order(need_rows_in_rowid_order),
        m_reuse_handler(reuse_handler),
        m_mrr_flags(mrr_flags),
        m_mrr_buf_size(mrr_buf_size),
        m_ranges(ranges) {}

  bool Init() override {
    m_rows.clear();
    m_pos = 0;
    m_serial_iterator = nullptr;

    if (m_ranges.size() != 1 || table() == nullptr || table()->file == nullptr ||
        table()->s == nullptr || table()->s->reclength == 0) {
      return fallback_to_serial();
    }

    QUICK_RANGE *range = m_ranges[0];
    if (range == nullptr) {
      return fallback_to_serial();
    }

    key_range start_key;
    key_range end_key;
    range->make_min_endpoint(&start_key);
    range->make_max_endpoint(&end_key);

    PQ_copied_key_endpoint copied_start;
    PQ_copied_key_endpoint copied_end;
    if (!pq_copy_key_endpoint(start_key, &copied_start) ||
        !pq_copy_key_endpoint(end_key, &copied_end)) {
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }

    std::vector<std::vector<uchar>> candidate_rows;
    pq_record_buffer_probe(table());
    PQ_record_buffer_sink sink(table(), &candidate_rows);
    uint row_count = 0;
    const int error = table()->file->pq_primary_range_produce(
        thd(), m_index, copied_start.present ? &copied_start.range : nullptr,
        copied_end.present ? &copied_end.range : nullptr,
        kPQPrimaryClusteredMaxBufferedRows, &sink, &row_count);

    if (error == HA_ERR_UNSUPPORTED) {
      return fallback_to_serial();
    }

    if (error != 0) {
      PrintError(error);
      return true;
    }

    if (row_count != candidate_rows.size() ||
        row_count > kPQPrimaryClusteredMaxBufferedRows) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    m_rows = std::move(candidate_rows);
    pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
    pq_global_stats.queries_executed.fetch_add(1, std::memory_order_relaxed);
    pq_global_stats.rows_scanned.fetch_add(row_count, std::memory_order_relaxed);
    return false;
  }

  int Read() override {
    if (m_serial_iterator != nullptr) {
      return m_serial_iterator->Read();
    }

    if (m_pos >= m_rows.size()) {
      table()->set_no_row();
      return -1;
    }

    std::memcpy(table()->record[0], m_rows[m_pos].data(),
                table()->s->reclength);
    ++m_pos;
    if (m_examined_rows != nullptr) {
      ++*m_examined_rows;
    }
    return 0;
  }

  void UnlockRow() override {
    if (m_serial_iterator != nullptr) {
      m_serial_iterator->UnlockRow();
      return;
    }
    TableRowIterator::UnlockRow();
  }

 private:
  bool fallback_to_serial() {
    m_rows.clear();
    pq_set_execution_state(thd(), PQ_execution_state::FALLBACK_SERIAL);
    pq_global_stats.queries_fallback.fetch_add(1, std::memory_order_relaxed);
    m_serial_iterator = NewIterator<IndexRangeScanIterator>(
        thd(), m_mem_root, table(), m_examined_rows, m_expected_rows, m_index,
        m_need_rows_in_rowid_order, m_reuse_handler, m_mem_root, m_mrr_flags,
        m_mrr_buf_size, m_ranges);
    return m_serial_iterator == nullptr || m_serial_iterator->Init();
  }

  MEM_ROOT *const m_mem_root;
  ha_rows *const m_examined_rows;
  const double m_expected_rows;
  const uint m_index;
  const bool m_need_rows_in_rowid_order;
  const bool m_reuse_handler;
  const uint m_mrr_flags;
  const uint m_mrr_buf_size;
  Bounds_checked_array<QUICK_RANGE *> m_ranges;
  std::vector<std::vector<uchar>> m_rows;
  size_t m_pos{0};
  unique_ptr_destroy_only<RowIterator> m_serial_iterator;
};

class PQSecondaryNoncoveringRangeIterator final : public TableRowIterator {
 public:
  PQSecondaryNoncoveringRangeIterator(
      THD *thd, MEM_ROOT *mem_root, TABLE *table, ha_rows *examined_rows,
      double expected_rows, uint index_arg, bool need_rows_in_rowid_order,
      bool reuse_handler, uint mrr_flags, uint mrr_buf_size,
      Bounds_checked_array<QUICK_RANGE *> ranges)
      : TableRowIterator(thd, table),
        m_mem_root(mem_root),
        m_examined_rows(examined_rows),
        m_expected_rows(expected_rows),
        m_index(index_arg),
        m_need_rows_in_rowid_order(need_rows_in_rowid_order),
        m_reuse_handler(reuse_handler),
        m_mrr_flags(mrr_flags),
        m_mrr_buf_size(mrr_buf_size),
        m_ranges(ranges) {}

  bool Init() override {
    m_rows.clear();
    m_pos = 0;
    m_serial_iterator = nullptr;

    if (m_ranges.size() != 1 || table() == nullptr || table()->file == nullptr ||
        table()->s == nullptr || table()->s->reclength == 0) {
      return fallback_to_serial();
    }

    QUICK_RANGE *range = m_ranges[0];
    if (range == nullptr) {
      return fallback_to_serial();
    }

    key_range start_key;
    key_range end_key;
    range->make_min_endpoint(&start_key);
    range->make_max_endpoint(&end_key);

    PQ_copied_key_endpoint copied_start;
    PQ_copied_key_endpoint copied_end;
    if (!pq_copy_key_endpoint(start_key, &copied_start) ||
        !pq_copy_key_endpoint(end_key, &copied_end)) {
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }

    pq_record_buffer_probe(table());
    PQ_record_buffer_sink sink(table(), &m_rows);
    uint row_count = 0;
    const int error = table()->file->pq_secondary_noncovering_range_produce(
        thd(), m_index, copied_start.present ? &copied_start.range : nullptr,
        copied_end.present ? &copied_end.range : nullptr, &sink, &row_count);

    if (error == HA_ERR_UNSUPPORTED) {
      m_rows.clear();
      return fallback_to_serial();
    }

    if (error != 0) {
      m_rows.clear();
      if (error != HA_ERR_OUT_OF_MEM && error != HA_ERR_QUERY_INTERRUPTED) {
        return fallback_to_serial();
      }
      PrintError(error == HA_ERR_UNSUPPORTED ? HA_ERR_INTERNAL_ERROR : error);
      return true;
    }

    if (row_count != m_rows.size()) {
      if (m_rows.size() >= kPQSecondaryNoncoveringRangeMaxRows ||
          row_count == 0) {
        m_rows.clear();
        return fallback_to_serial();
      }
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    if (row_count >= kPQSecondaryNoncoveringRangeMaxRows) {
      m_rows.clear();
      return fallback_to_serial();
    }

    pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
    pq_global_stats.queries_executed.fetch_add(1, std::memory_order_relaxed);
    pq_global_stats.secondary_rows_produced.fetch_add(row_count,
                                                      std::memory_order_relaxed);
    pq_global_stats.rows_scanned.fetch_add(row_count, std::memory_order_relaxed);
    return false;
  }

  int Read() override {
    if (m_serial_iterator != nullptr) {
      return m_serial_iterator->Read();
    }

    if (m_pos >= m_rows.size()) {
      table()->set_no_row();
      return -1;
    }

    std::memcpy(table()->record[0], m_rows[m_pos].data(),
                table()->s->reclength);
    ++m_pos;
    if (m_examined_rows != nullptr) {
      ++*m_examined_rows;
    }
    return 0;
  }

  void UnlockRow() override {
    if (m_serial_iterator != nullptr) {
      m_serial_iterator->UnlockRow();
      return;
    }
    TableRowIterator::UnlockRow();
  }

 private:
  bool fallback_to_serial() {
    pq_set_execution_state(thd(), PQ_execution_state::FALLBACK_SERIAL);
    pq_global_stats.queries_fallback.fetch_add(1, std::memory_order_relaxed);
    if (table() != nullptr && table()->file != nullptr) {
      if (table()->file->inited == handler::INDEX) {
        (void)table()->file->ha_index_end();
      } else if (table()->file->inited == handler::RND) {
        (void)table()->file->ha_rnd_end();
      }
    }
    m_serial_iterator = NewIterator<IndexRangeScanIterator>(
        thd(), m_mem_root, table(), m_examined_rows, m_expected_rows, m_index,
        m_need_rows_in_rowid_order, false, m_mem_root, m_mrr_flags,
        m_mrr_buf_size, m_ranges);
    return m_serial_iterator == nullptr || m_serial_iterator->Init();
  }

  MEM_ROOT *const m_mem_root;
  ha_rows *const m_examined_rows;
  const double m_expected_rows;
  const uint m_index;
  const bool m_need_rows_in_rowid_order;
  const bool m_reuse_handler;
  const uint m_mrr_flags;
  const uint m_mrr_buf_size;
  Bounds_checked_array<QUICK_RANGE *> m_ranges;
  std::vector<std::vector<uchar>> m_rows;
  size_t m_pos{0};
  unique_ptr_destroy_only<RowIterator> m_serial_iterator;
};

class PQSecondaryNoncoveringIcpRangeIterator final : public TableRowIterator {
 public:
  PQSecondaryNoncoveringIcpRangeIterator(
      THD *thd, MEM_ROOT *mem_root, TABLE *table, ha_rows *examined_rows,
      double expected_rows, uint index_arg, bool need_rows_in_rowid_order,
      bool reuse_handler, uint mrr_flags, uint mrr_buf_size,
      Bounds_checked_array<QUICK_RANGE *> ranges)
      : TableRowIterator(thd, table),
        m_mem_root(mem_root),
        m_examined_rows(examined_rows),
        m_expected_rows(expected_rows),
        m_index(index_arg),
        m_need_rows_in_rowid_order(need_rows_in_rowid_order),
        m_reuse_handler(reuse_handler),
        m_mrr_flags(mrr_flags),
        m_mrr_buf_size(mrr_buf_size),
        m_ranges(ranges) {}

  bool Init() override {
    m_rows.clear();
    m_pos = 0;
    m_serial_iterator = nullptr;

    if (m_ranges.size() != 1 || table() == nullptr || table()->file == nullptr ||
        table()->s == nullptr || table()->s->reclength == 0) {
      return fallback_to_serial();
    }

    QUICK_RANGE *range = m_ranges[0];
    if (range == nullptr) {
      return fallback_to_serial();
    }

    key_range start_key;
    key_range end_key;
    range->make_min_endpoint(&start_key);
    range->make_max_endpoint(&end_key);

    PQ_copied_key_endpoint copied_start;
    PQ_copied_key_endpoint copied_end;
    if (!pq_copy_key_endpoint(start_key, &copied_start) ||
        !pq_copy_key_endpoint(end_key, &copied_end)) {
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }

    pq_record_buffer_probe(table());
    PQ_record_buffer_sink sink(table(), &m_rows);
    uint row_count = 0;
    const int error = table()->file->pq_secondary_noncovering_icp_range_produce(
        thd(), m_index, copied_start.present ? &copied_start.range : nullptr,
        copied_end.present ? &copied_end.range : nullptr, &sink, &row_count);

    if (error == HA_ERR_UNSUPPORTED && m_rows.empty()) {
      return fallback_to_serial();
    }

    if (error != 0) {
      PrintError(error == HA_ERR_UNSUPPORTED ? HA_ERR_INTERNAL_ERROR : error);
      return true;
    }

    if (row_count != m_rows.size() ||
        row_count > kPQSecondaryNoncoveringIcpMaxRows) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
    pq_global_stats.queries_executed.fetch_add(1, std::memory_order_relaxed);
    pq_global_stats.secondary_rows_produced.fetch_add(row_count,
                                                      std::memory_order_relaxed);
    pq_global_stats.rows_scanned.fetch_add(row_count, std::memory_order_relaxed);
    return false;
  }

  int Read() override {
    if (m_serial_iterator != nullptr) {
      return m_serial_iterator->Read();
    }

    if (m_pos >= m_rows.size()) {
      table()->set_no_row();
      return -1;
    }

    std::memcpy(table()->record[0], m_rows[m_pos].data(),
                table()->s->reclength);
    ++m_pos;
    if (m_examined_rows != nullptr) {
      ++*m_examined_rows;
    }
    return 0;
  }

  void UnlockRow() override {
    if (m_serial_iterator != nullptr) {
      m_serial_iterator->UnlockRow();
      return;
    }
    TableRowIterator::UnlockRow();
  }

 private:
  bool fallback_to_serial() {
    pq_set_execution_state(thd(), PQ_execution_state::FALLBACK_SERIAL);
    pq_global_stats.queries_fallback.fetch_add(1, std::memory_order_relaxed);
    m_serial_iterator = NewIterator<IndexRangeScanIterator>(
        thd(), m_mem_root, table(), m_examined_rows, m_expected_rows, m_index,
        m_need_rows_in_rowid_order, m_reuse_handler, m_mem_root, m_mrr_flags,
        m_mrr_buf_size, m_ranges);
    return m_serial_iterator == nullptr || m_serial_iterator->Init();
  }

  MEM_ROOT *const m_mem_root;
  ha_rows *const m_examined_rows;
  const double m_expected_rows;
  const uint m_index;
  const bool m_need_rows_in_rowid_order;
  const bool m_reuse_handler;
  const uint m_mrr_flags;
  const uint m_mrr_buf_size;
  Bounds_checked_array<QUICK_RANGE *> m_ranges;
  std::vector<std::vector<uchar>> m_rows;
  size_t m_pos{0};
  unique_ptr_destroy_only<RowIterator> m_serial_iterator;
};

class PQSecondaryCoveringRefIterator final : public TableRowIterator {
 public:
  PQSecondaryCoveringRefIterator(THD *thd, MEM_ROOT *mem_root, TABLE *table,
                                 Index_lookup *ref, bool use_order,
                                 double expected_rows,
                                 ha_rows *examined_rows)
      : TableRowIterator(thd, table),
        m_mem_root(mem_root),
        m_ref(ref),
        m_use_order(use_order),
        m_expected_rows(expected_rows),
        m_examined_rows(examined_rows) {}

  bool Init() override {
    m_rows.clear();
    m_pos = 0;
    m_serial_iterator = nullptr;

    if (table() == nullptr || table()->file == nullptr ||
        table()->s == nullptr || table()->s->reclength == 0 ||
        m_ref == nullptr || !pq_secondary_ref_is_constant_and_safe(table(),
                                                                   m_ref)) {
      return fallback_to_serial();
    }

    if (m_ref->impossible_null_ref()) {
      return fallback_to_serial();
    }

    if (construct_lookup(thd(), table(), m_ref)) {
      table()->set_no_row();
      return false;
    }

    std::vector<uchar> copied_key(m_ref->key_buff,
                                  m_ref->key_buff + m_ref->key_length);
    key_range ref_key{};
    ref_key.key = copied_key.data();
    ref_key.length = m_ref->key_length;
    ref_key.keypart_map = make_prev_keypart_map(m_ref->key_parts);
    ref_key.flag = HA_READ_KEY_EXACT;

    DBUG_EXECUTE_IF("pq_worker_ref_ctx_shape_smoke", {
      pq_global_stats.worker_ref_ctx_attempts.fetch_add(
          1, std::memory_order_relaxed);
      PQ_worker_constant_ref_context_shape ctx;
      if (pq_build_worker_constant_ref_context_shape(table(), m_ref, ref_key,
                                                     &ctx) ||
          !ctx.exact_read || ctx.reverse || !ctx.constant_ref ||
          ctx.keyno != static_cast<uint>(m_ref->key) ||
          ctx.keypart_map != ref_key.keypart_map ||
          ctx.key_length != ref_key.length ||
          ctx.owned_key.size() != ref_key.length) {
        pq_global_stats.worker_ref_ctx_unsupported.fetch_add(
            1, std::memory_order_relaxed);
      } else {
        pq_global_stats.worker_ref_ctx_success.fetch_add(
            1, std::memory_order_relaxed);
        pq_global_stats.worker_ref_ctx_key_bytes.fetch_add(
            ctx.owned_key.size(), std::memory_order_relaxed);
      }
    });

    auto run_ref_ctx_cleanup_smoke = [&](bool inject_failure) {
      pq_global_stats.worker_ref_ctx_cleanup_attempts.fetch_add(
          1, std::memory_order_relaxed);
      PQ_worker_constant_ref_context_shape ctx;
      if (pq_build_worker_constant_ref_context_shape(table(), m_ref, ref_key,
                                                     &ctx)) {
        pq_global_stats.worker_ref_ctx_cleanup_unsupported.fetch_add(
            1, std::memory_order_relaxed);
        return;
      }

      bool cleanup_reached = false;
      bool cleanup_valid = false;
      {
        PQ_worker_constant_ref_cleanup_smoke scoped_ctx(ctx, &cleanup_reached);
        cleanup_valid = scoped_ctx.valid() &&
                        scoped_ctx.owned_key_size() == ref_key.length;
      }

      if (cleanup_reached && cleanup_valid) {
        pq_global_stats.worker_ref_ctx_cleanup_success.fetch_add(
            1, std::memory_order_relaxed);
      } else {
        pq_global_stats.worker_ref_ctx_cleanup_unsupported.fetch_add(
            1, std::memory_order_relaxed);
      }
      if (inject_failure) {
        pq_global_stats.worker_ref_ctx_cleanup_failures.fetch_add(
            1, std::memory_order_relaxed);
      }
    };

    DBUG_EXECUTE_IF("pq_worker_ref_ctx_cleanup_smoke", {
      run_ref_ctx_cleanup_smoke(false);
    });
    DBUG_EXECUTE_IF("pq_worker_ref_ctx_cleanup_fail_smoke", {
      run_ref_ctx_cleanup_smoke(true);
    });

    DBUG_EXECUTE_IF("pq_worker_ref_ctx_token_transport_smoke", {
      pq_global_stats.worker_ref_ctx_token_attempts.fetch_add(
          1, std::memory_order_relaxed);
      PQ_worker_constant_ref_context_shape ctx;
      if (pq_build_worker_constant_ref_context_shape(table(), m_ref, ref_key,
                                                     &ctx) ||
          ctx.owned_key.empty()) {
        pq_global_stats.worker_ref_ctx_token_unsupported.fetch_add(
            1, std::memory_order_relaxed);
      } else {
        uint32 token_bytes = 0;
        uint32 token_deep_copy = 0;
        uint32 token_normal_rejects = 0;
        uint32 token_invalid_rejects = 0;
        uint32 token_length_mismatch = 0;
        if (pq_run_query_result_mq_constant_ref_token_transport_smoke(
                ctx.owned_key.data(), static_cast<uint32>(ctx.owned_key.size()),
                static_cast<uint32>(ctx.owned_key.size()), &token_bytes,
                &token_deep_copy, &token_normal_rejects,
                &token_invalid_rejects, &token_length_mismatch)) {
          pq_global_stats.worker_ref_ctx_token_unsupported.fetch_add(
              1, std::memory_order_relaxed);
        } else {
          pq_global_stats.worker_ref_ctx_token_success.fetch_add(
              1, std::memory_order_relaxed);
          pq_global_stats.worker_ref_ctx_token_bytes.fetch_add(
              token_bytes, std::memory_order_relaxed);
          pq_global_stats.worker_ref_ctx_token_deep_copy_success.fetch_add(
              token_deep_copy, std::memory_order_relaxed);
          pq_global_stats.worker_ref_ctx_token_normal_rejects.fetch_add(
              token_normal_rejects, std::memory_order_relaxed);
          pq_global_stats.worker_ref_ctx_token_invalid_rejects.fetch_add(
              token_invalid_rejects, std::memory_order_relaxed);
          pq_global_stats.worker_ref_ctx_token_len_mismatch.fetch_add(
              token_length_mismatch, std::memory_order_relaxed);
        }
      }
    });
    DBUG_EXECUTE_IF("pq_worker_ref_contract_smoke", {
      PQ_worker_constant_ref_context_shape ctx;
      if (pq_build_worker_constant_ref_context_shape(table(), m_ref, ref_key,
                                                     &ctx)) {
        pq_global_stats.worker_ref_contract_unsupported.fetch_add(
            1, std::memory_order_relaxed);
      } else {
        (void)pq_run_worker_ref_contract_smoke(thd(), table(), ctx, false);
      }
    });
    DBUG_EXECUTE_IF("pq_worker_ref_contract_fail_smoke", {
      PQ_worker_constant_ref_context_shape ctx;
      if (pq_build_worker_constant_ref_context_shape(table(), m_ref, ref_key,
                                                     &ctx)) {
        pq_global_stats.worker_ref_contract_unsupported.fetch_add(
            1, std::memory_order_relaxed);
      } else {
        (void)pq_run_worker_ref_contract_smoke(thd(), table(), ctx, true);
      }
    });

    pq_record_buffer_probe(table());
    PQ_record_buffer_sink sink(table(), &m_rows);
    uint row_count = 0;
    const int error = table()->file->pq_secondary_covering_ref_produce(
        thd(), static_cast<uint>(m_ref->key), &ref_key, &sink, &row_count);

    if (error == HA_ERR_UNSUPPORTED) {
      return fallback_to_serial();
    }

    if (error != 0) {
      PrintError(error);
      return true;
    }

    if (row_count != m_rows.size()) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
    pq_global_stats.queries_executed.fetch_add(1, std::memory_order_relaxed);
    pq_global_stats.secondary_rows_produced.fetch_add(row_count,
                                                      std::memory_order_relaxed);
    pq_global_stats.rows_scanned.fetch_add(row_count, std::memory_order_relaxed);
    return false;
  }

  int Read() override {
    if (m_serial_iterator != nullptr) {
      return m_serial_iterator->Read();
    }

    if (m_pos >= m_rows.size()) {
      table()->set_no_row();
      return -1;
    }

    std::memcpy(table()->record[0], m_rows[m_pos].data(),
                table()->s->reclength);
    ++m_pos;
    if (m_examined_rows != nullptr) {
      ++*m_examined_rows;
    }
    return 0;
  }

  void UnlockRow() override {
    if (m_serial_iterator != nullptr) {
      m_serial_iterator->UnlockRow();
      return;
    }
    TableRowIterator::UnlockRow();
  }

 private:
  bool fallback_to_serial() {
    pq_set_execution_state(thd(), PQ_execution_state::FALLBACK_SERIAL);
    pq_global_stats.queries_fallback.fetch_add(1, std::memory_order_relaxed);
    m_serial_iterator = NewIterator<RefIterator<false>>(
        thd(), m_mem_root, table(), m_ref, m_use_order, m_expected_rows,
        m_examined_rows);
    return m_serial_iterator == nullptr || m_serial_iterator->Init();
  }

  MEM_ROOT *const m_mem_root;
  Index_lookup *const m_ref;
  const bool m_use_order;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  std::vector<std::vector<uchar>> m_rows;
  size_t m_pos{0};
  unique_ptr_destroy_only<RowIterator> m_serial_iterator;
};

class PQSecondaryDependentRefSmokeIterator final : public TableRowIterator {
 public:
  PQSecondaryDependentRefSmokeIterator(THD *thd, MEM_ROOT *mem_root,
                                       TABLE *table, Index_lookup *ref,
                                       bool use_order, double expected_rows,
                                       ha_rows *examined_rows)
      : TableRowIterator(thd, table),
        m_mem_root(mem_root),
        m_ref(ref),
        m_use_order(use_order),
        m_expected_rows(expected_rows),
        m_examined_rows(examined_rows) {}

  bool Init() override {
    m_smoke_ran = false;
    m_serial_iterator = NewIterator<RefIterator<false>>(
        thd(), m_mem_root, table(), m_ref, m_use_order, m_expected_rows,
        m_examined_rows);
    return m_serial_iterator == nullptr || m_serial_iterator->Init();
  }

  int Read() override {
    if (!m_smoke_ran) {
      m_smoke_ran = true;
      if (run_smoke_probe()) return 1;
      if (m_serial_iterator == nullptr || m_serial_iterator->Init()) return 1;
    }
    return m_serial_iterator->Read();
  }

  void UnlockRow() override {
    if (m_serial_iterator != nullptr) {
      m_serial_iterator->UnlockRow();
      return;
    }
    TableRowIterator::UnlockRow();
  }

 private:
  bool run_smoke_probe() {
    pq_global_stats.secondary_ref_probe_attempts.fetch_add(
        1, std::memory_order_relaxed);

    if (table() == nullptr || table()->file == nullptr ||
        table()->s == nullptr || table()->s->reclength == 0 ||
        m_ref == nullptr ||
        !pq_secondary_ref_is_dependent_scaffold_candidate(table(), m_ref)) {
      pq_global_stats.secondary_ref_fallback_probes.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    if (m_ref->impossible_null_ref()) {
      pq_global_stats.secondary_ref_empty_probes.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    if (construct_lookup(thd(), table(), m_ref)) {
      table()->set_no_row();
      pq_global_stats.secondary_ref_fallback_probes.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    std::vector<uchar> copied_key(m_ref->key_buff,
                                  m_ref->key_buff + m_ref->key_length);
    key_range ref_key{};
    ref_key.key = copied_key.data();
    ref_key.length = m_ref->key_length;
    ref_key.keypart_map = make_prev_keypart_map(m_ref->key_parts);
    ref_key.flag = HA_READ_KEY_EXACT;

    std::vector<std::vector<uchar>> smoke_rows;
    PQ_record_buffer_sink sink(table(), &smoke_rows);
    uint row_count = 0;
    const int error = table()->file->pq_secondary_covering_ref_produce(
        thd(), static_cast<uint>(m_ref->key), &ref_key, &sink, &row_count);

    if (error == HA_ERR_UNSUPPORTED) {
      pq_global_stats.secondary_ref_fallback_probes.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }

    if (error != 0) {
      PrintError(error);
      return true;
    }

    if (row_count != smoke_rows.size()) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    if (row_count == 0) {
      pq_global_stats.secondary_ref_empty_probes.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      pq_global_stats.secondary_ref_rows_produced.fetch_add(
          row_count, std::memory_order_relaxed);
    }
    return false;
  }

  MEM_ROOT *const m_mem_root;
  Index_lookup *const m_ref;
  const bool m_use_order;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  bool m_smoke_ran{false};
  unique_ptr_destroy_only<RowIterator> m_serial_iterator;
};

class PQSecondaryDependentRefIterator final : public TableRowIterator {
 public:
  PQSecondaryDependentRefIterator(THD *thd, MEM_ROOT *mem_root, TABLE *table,
                                  Index_lookup *ref, bool use_order,
                                  double expected_rows,
                                  ha_rows *examined_rows)
      : TableRowIterator(thd, table),
        m_mem_root(mem_root),
        m_ref(ref),
        m_use_order(use_order),
        m_expected_rows(expected_rows),
        m_examined_rows(examined_rows) {}

  bool Init() override {
    m_rows.clear();
    m_pos = 0;
    m_probe_done = false;
    m_emitted_any = false;
    m_serial_iterator = nullptr;
    return false;
  }

  int Read() override {
    if (m_serial_iterator != nullptr) {
      return m_serial_iterator->Read();
    }

    if (!m_probe_done) {
      m_probe_done = true;
      const int error = produce_current_probe();
      if (error == HA_ERR_UNSUPPORTED) {
        if (m_emitted_any) {
          PrintError(HA_ERR_INTERNAL_ERROR);
          return 1;
        }
        m_rows.clear();
        m_pos = 0;
        pq_global_stats.secondary_ref_probe_unsupported.fetch_add(
            1, std::memory_order_relaxed);
        pq_global_stats.secondary_ref_fallback_probes.fetch_add(
            1, std::memory_order_relaxed);
        if (init_serial_probe()) return 1;
        return m_serial_iterator->Read();
      }
      if (error != 0) {
        PrintError(error);
        return 1;
      }
    }

    if (m_pos >= m_rows.size()) {
      table()->set_no_row();
      return -1;
    }

    std::memcpy(table()->record[0], m_rows[m_pos].data(),
                table()->s->reclength);
    ++m_pos;
    m_emitted_any = true;
    if (m_examined_rows != nullptr) {
      ++*m_examined_rows;
    }
    return 0;
  }

  void UnlockRow() override {
    if (m_serial_iterator != nullptr) {
      m_serial_iterator->UnlockRow();
      return;
    }
    TableRowIterator::UnlockRow();
  }

 private:
  int produce_current_probe() {
    pq_global_stats.secondary_ref_probe_attempts.fetch_add(
        1, std::memory_order_relaxed);

    if (table() == nullptr || table()->file == nullptr ||
        table()->s == nullptr || table()->s->reclength == 0 ||
        m_ref == nullptr ||
        !pq_secondary_ref_is_dependent_scaffold_candidate(table(), m_ref)) {
      return HA_ERR_UNSUPPORTED;
    }

    if (m_ref->impossible_null_ref()) {
      table()->set_no_row();
      pq_global_stats.secondary_ref_empty_probes.fetch_add(
          1, std::memory_order_relaxed);
      return 0;
    }

    if (construct_lookup(thd(), table(), m_ref)) {
      table()->set_no_row();
      pq_global_stats.secondary_ref_empty_probes.fetch_add(
          1, std::memory_order_relaxed);
      return 0;
    }

    std::vector<uchar> copied_key(m_ref->key_buff,
                                  m_ref->key_buff + m_ref->key_length);
    key_range ref_key{};
    ref_key.key = copied_key.data();
    ref_key.length = m_ref->key_length;
    ref_key.keypart_map = make_prev_keypart_map(m_ref->key_parts);
    ref_key.flag = HA_READ_KEY_EXACT;

    pq_record_buffer_probe(table());
    PQ_record_buffer_sink sink(table(), &m_rows);
    uint row_count = 0;
    const int error = table()->file->pq_secondary_covering_ref_produce(
        thd(), static_cast<uint>(m_ref->key), &ref_key, &sink, &row_count);
    if (error != 0) {
      return error;
    }

    DBUG_EXECUTE_IF("pq_secondary_dependent_ref_unsupported_after_buffer", {
      if (!m_rows.empty()) return HA_ERR_UNSUPPORTED;
    });

    if (row_count != m_rows.size()) {
      return HA_ERR_INTERNAL_ERROR;
    }

    if (m_total_rows_buffered + m_rows.size() >
        kPQSecondaryDependentRefMaxBufferedRows) {
      return HA_ERR_UNSUPPORTED;
    }
    m_total_rows_buffered += m_rows.size();

    if (row_count == 0) {
      pq_global_stats.secondary_ref_empty_probes.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      pq_global_stats.secondary_ref_rows_produced.fetch_add(
          row_count, std::memory_order_relaxed);
    }

    if (!thd()->pq_executed) {
      pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
      pq_global_stats.queries_executed.fetch_add(1,
                                                 std::memory_order_relaxed);
    }
    pq_global_stats.secondary_rows_produced.fetch_add(
        row_count, std::memory_order_relaxed);
    pq_global_stats.rows_scanned.fetch_add(row_count,
                                           std::memory_order_relaxed);
    return 0;
  }

  bool init_serial_probe() {
    m_serial_iterator = NewIterator<RefIterator<false>>(
        thd(), m_mem_root, table(), m_ref, m_use_order, m_expected_rows,
        m_examined_rows);
    return m_serial_iterator == nullptr || m_serial_iterator->Init();
  }

  MEM_ROOT *const m_mem_root;
  Index_lookup *const m_ref;
  const bool m_use_order;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  std::vector<std::vector<uchar>> m_rows;
  size_t m_pos{0};
  size_t m_total_rows_buffered{0};
  bool m_probe_done{false};
  bool m_emitted_any{false};
  unique_ptr_destroy_only<RowIterator> m_serial_iterator;
};

}  // namespace

ParallelScanIterator::ParallelScanIterator(
    THD *thd, QEP_TAB *tab, TABLE *table, double expected_rows,
    ha_rows *examined_rows, JOIN *join, Gather_operator *gather,
    bool stab_output, AccessPath *access_path)
    : TableRowIterator(thd, table),
      m_tab(tab),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows),
      m_join(join),
      m_gather(gather),
      m_stable_output(stab_output),
      m_root_access_path(access_path) {}

ParallelScanIterator::~ParallelScanIterator() { cleanup_lifecycle(false); }

void ParallelScanIterator::cleanup_lifecycle(bool init_failed) {
  if (m_cleanup_done) return;

  /*
    M11-D1 keeps the commercial iterator fail-closed. This helper is still
    exercised from Init() failure and destructor so the future positive path
    has a single idempotent cleanup point before workers are enabled here.
  */
  if (m_owns_gather && m_gather != nullptr) {
    m_gather->destroy();
    delete m_gather;
    m_gather = nullptr;
  }

  if (m_leader_ctx != nullptr) {
    table()->file->pq_leader_scan_end(m_leader_ctx);
    m_leader_ctx = nullptr;
  }

  m_worker_started = false;
  m_no_fallback_commit = false;
  m_executed_counted = false;
  m_cleanup_done = true;
  pq_global_stats.parallel_scan_lifecycle_cleanup_calls.fetch_add(
      1, std::memory_order_relaxed);
  m_lifecycle_state =
      init_failed ? Lifecycle_state::FAIL_CLOSED
                  : Lifecycle_state::CLEANED_UP;
}

bool ParallelScanIterator::Init() {
  m_lifecycle_state = Lifecycle_state::INITIALIZING;

  DBUG_EXECUTE_IF("pq_parallel_scan_iterator_order_gather_smoke", {
    pq_global_stats.parallel_scan_iterator_order_gather_attempts.fetch_add(
        1, std::memory_order_relaxed);

    Exchange_sort sort_exchange(3, PQ_MQ_DEFAULT_RING_SIZE);
    uint32 smoke_rows = 0;
    if (sort_exchange.run_cached_record_adapter_smoke(&smoke_rows) ||
        smoke_rows == 0) {
      cleanup_lifecycle(true);
      return true;
    }

    pq_global_stats.parallel_scan_iterator_order_gather_selected.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.parallel_scan_iterator_order_gather_smoke_rows.fetch_add(
        smoke_rows, std::memory_order_relaxed);
    m_no_fallback_commit = true;
    m_lifecycle_state = Lifecycle_state::ORDER_GATHER_VALIDATED;
    return false;
  });

  DBUG_EXECUTE_IF("pq_parallel_scan_iterator_row_value_smoke", {
    pq_global_stats.parallel_scan_iterator_row_value_attempts.fetch_add(
        1, std::memory_order_relaxed);

    if (!pq_parallel_scan_iterator_row_value_shape_is_safe(table())) {
      cleanup_lifecycle(true);
      return true;
    }

    uint execute_dop = 0;
    const int error = table()->file->pq_leader_scan_init(
        thd(), &m_leader_ctx, PQ_leader_scan_mode::EXECUTE, 1, &execute_dop,
        false);
    if (error != 0) {
      m_leader_ctx = nullptr;
      cleanup_lifecycle(true);
      return true;
    }

    m_gather = new Gather_operator(1);
    m_owns_gather = true;
    uint32 rows_enqueued = 0;
    if (m_gather == nullptr ||
        m_gather->prepare_leader_row_stream_smoke(thd(), table(), m_leader_ctx,
                                                  2, &rows_enqueued) ||
        rows_enqueued != 2) {
      cleanup_lifecycle(true);
      return true;
    }

    pq_global_stats.parallel_scan_iterator_row_value_selected.fetch_add(
        1, std::memory_order_relaxed);
    m_no_fallback_commit = true;
    m_lifecycle_state = Lifecycle_state::RUNNING;
    return false;
  });

  cleanup_lifecycle(true);
  return true;
}

int ParallelScanIterator::Read() {
  if (m_lifecycle_state == Lifecycle_state::ORDER_GATHER_VALIDATED) {
    cleanup_lifecycle(false);
    table()->set_no_row();
    return -1;
  }

  if (m_lifecycle_state != Lifecycle_state::RUNNING || m_gather == nullptr ||
      m_gather->get_exchange() == nullptr) {
    PrintError(HA_ERR_INTERNAL_ERROR);
    return 1;
  }

  Exchange_nosort *exchange = m_gather->get_exchange();

  for (;;) {
    if (m_gather->check_leader_kill(thd())) {
      m_gather->propagate_kill_to_workers(thd());
      cleanup_lifecycle(true);
      thd()->send_kill_message();
      return 1;
    }

    Exchange_nosort::Materialize_status status =
        Exchange_nosort::Materialize_status::ERROR;
    if (exchange->materialize_next_record_image_status(table(), &status)) {
      cleanup_lifecycle(true);
      PrintError(HA_ERR_INTERNAL_ERROR);
      return 1;
    }

    if (status == Exchange_nosort::Materialize_status::ROW) {
      if (!m_executed_counted) {
        pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
        pq_global_stats.queries_executed.fetch_add(1,
                                                   std::memory_order_relaxed);
        m_executed_counted = true;
      }
      pq_global_stats.rows_scanned.fetch_add(1, std::memory_order_relaxed);
      pq_global_stats.parallel_scan_iterator_row_value_rows.fetch_add(
          1, std::memory_order_relaxed);
      return 0;
    }

    if (status == Exchange_nosort::Materialize_status::EOF_REACHED) {
      if (!m_executed_counted) {
        pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
        pq_global_stats.queries_executed.fetch_add(1,
                                                   std::memory_order_relaxed);
        m_executed_counted = true;
      }
      cleanup_lifecycle(false);
      table()->set_no_row();
      return -1;
    }

    if (status == Exchange_nosort::Materialize_status::WOULD_BLOCK) {
      exchange->wait_for_message(kPQParallelScanIteratorReadWaitTimeoutUs);
      continue;
    }

    cleanup_lifecycle(true);
    PrintError(HA_ERR_INTERNAL_ERROR);
    return 1;
  }
}

bool pq_run_parallel_scan_lifecycle_smoke(THD *thd) {
  if (thd == nullptr) return true;

  pq_global_stats.parallel_scan_lifecycle_smoke_attempts.fetch_add(
      1, std::memory_order_relaxed);

  ParallelScanIterator iterator(thd, nullptr, nullptr, 0.0, nullptr, nullptr,
                                nullptr, false, nullptr);
  const bool init_failed = iterator.Init();
  if (!init_failed) return true;

  pq_global_stats.parallel_scan_lifecycle_fail_closed.fetch_add(
      1, std::memory_order_relaxed);
  return false;
}

PQblockScanIterator::PQblockScanIterator(
    THD *thd, TABLE *table, double expected_rows, ha_rows *examined_rows,
    PQTabType tab_type, Gather_operator *gather, QEP_TAB *tab, bool need_rowid,
    MQueue_handle *handler)
    : TableRowIterator(thd, table),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows),
      m_tab_type(tab_type),
      m_gather(gather),
      m_tab(tab),
      m_need_rowid(need_rowid),
      m_handler(handler) {}

PQblockScanIterator::~PQblockScanIterator() { End(); }

bool PQblockScanIterator::Init() {
  m_seen_eof = false;

  if (m_inited) {
    return false;
  }

  if (thd() == nullptr || table() == nullptr || table()->file == nullptr ||
      !thd()->pq_is_worker || thd()->pq_worker_info == nullptr) {
    return true;
  }

  auto *worker = static_cast<PQ_worker_info *>(thd()->pq_worker_info);
  if (worker->m_open_ctx.worker_thd == nullptr) {
    worker->m_open_ctx.worker_thd = thd();
  }
  if (worker->m_open_ctx.worker_table == nullptr) {
    worker->m_open_ctx.worker_table = table();
  }
  if (worker->m_open_ctx.worker_handler == nullptr) {
    worker->m_open_ctx.worker_handler = table()->file;
  }
  if (worker->m_open_ctx.worker_handler != table()->file ||
      worker->m_open_ctx.worker_table != table()) {
    return true;
  }

  if (m_handler != nullptr && worker->m_open_ctx.mq_handle == nullptr) {
    worker->m_open_ctx.mq_handle = m_handler;
  }

  if (worker->m_worker_ctx != nullptr) {
    return true;
  }

  if (table()->file->ha_rnd_init(true) != 0) {
    return true;
  }
  m_rnd_inited = true;

  PQ_Worker_context *new_ctx = nullptr;
  const int error =
      table()->file->pq_worker_scan_init(&worker->m_open_ctx, &new_ctx);
  if (error != 0 || new_ctx == nullptr) {
    if (m_rnd_inited && table()->file->inited == handler::RND) {
      table()->file->ha_rnd_end();
    }
    m_rnd_inited = false;
    if (error != HA_ERR_UNSUPPORTED) {
      PrintError(error);
    }
    return true;
  }

  worker->m_worker_ctx = new_ctx;
  m_worker_ctx = new_ctx;
  m_inited = true;
  return false;
}

int PQblockScanIterator::End() {
  if (m_worker_ctx != nullptr && table() != nullptr && table()->file != nullptr) {
    (void)table()->file->pq_worker_scan_end(m_worker_ctx);
    if (thd() != nullptr && thd()->pq_worker_info != nullptr) {
      auto *worker = static_cast<PQ_worker_info *>(thd()->pq_worker_info);
      if (worker->m_worker_ctx == m_worker_ctx) {
        worker->m_worker_ctx = nullptr;
      }
    }
    m_worker_ctx = nullptr;
  }
  if (m_rnd_inited && table() != nullptr && table()->file != nullptr &&
      table()->file->inited == handler::RND) {
    table()->file->ha_rnd_end();
  }
  m_rnd_inited = false;
  m_inited = false;
  return -1;
}

int PQblockScanIterator::Read() {
  if (m_seen_eof) {
    return -1;
  }

  if (m_worker_ctx == nullptr || table() == nullptr || table()->file == nullptr) {
    PrintError(HA_ERR_INTERNAL_ERROR);
    return 1;
  }

  bool eof = false;
  for (;;) {
    const int error =
        table()->file->pq_worker_scan_next(m_worker_ctx, table()->record[0],
                                           &eof);
    if (error == 0) {
      if (eof) {
        m_seen_eof = true;
        table()->set_no_row();
        return -1;
      }
      break;
    }
    if (error == HA_ERR_RECORD_DELETED && !thd()->killed) {
      continue;
    }
    return HandleError(error);
  }

  if (m_examined_rows != nullptr) {
    ++*m_examined_rows;
  }
  if (m_need_rowid) {
    table()->file->position(table()->record[0]);
  }
  return 0;
}

PQRefIterator::PQRefIterator(THD *thd, TABLE *table, Index_lookup *ref,
                             bool use_order, PQTabType tab_type,
                             double expected_rows, ha_rows *examined_rows,
                             Gather_operator *gather, QEP_TAB *tab)
    : TableRowIterator(thd, table),
      m_ref(ref),
      m_use_order(use_order),
      m_tab_type(tab_type),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows),
      m_gather(gather),
      m_tab(tab) {}

PQRefIterator::~PQRefIterator() { End(); }

bool PQRefIterator::Init() {
  m_seen_eof = false;
  m_first_record_since_init = true;

  if (m_inited) {
    return false;
  }

  if (thd() == nullptr || table() == nullptr || table()->file == nullptr ||
      !thd()->pq_is_worker || thd()->pq_worker_info == nullptr ||
      m_ref == nullptr) {
    return true;
  }

  auto *worker = static_cast<PQ_worker_info *>(thd()->pq_worker_info);
  if (worker->m_open_ctx.worker_thd == nullptr) {
    worker->m_open_ctx.worker_thd = thd();
  }
  if (worker->m_open_ctx.worker_table == nullptr) {
    worker->m_open_ctx.worker_table = table();
  }
  if (worker->m_open_ctx.worker_handler == nullptr) {
    worker->m_open_ctx.worker_handler = table()->file;
  }
  if (worker->m_open_ctx.worker_handler != table()->file ||
      worker->m_open_ctx.worker_table != table()) {
    return true;
  }

  if (worker->m_worker_ctx != nullptr) {
    return true;
  }

  PQ_Worker_context *new_ctx = nullptr;
  const int error =
      table()->file->pq_worker_scan_init(&worker->m_open_ctx, &new_ctx);
  if (error != 0 || new_ctx == nullptr) {
    if (error != HA_ERR_UNSUPPORTED) {
      PrintError(error);
    }
    return true;
  }

  worker->m_worker_ctx = new_ctx;
  m_worker_ctx = new_ctx;
  m_inited = true;
  return false;
}

int PQRefIterator::End() {
  if (m_worker_ctx != nullptr && table() != nullptr && table()->file != nullptr) {
    (void)table()->file->pq_worker_scan_end(m_worker_ctx);
    if (thd() != nullptr && thd()->pq_worker_info != nullptr) {
      auto *worker = static_cast<PQ_worker_info *>(thd()->pq_worker_info);
      if (worker->m_worker_ctx == m_worker_ctx) {
        worker->m_worker_ctx = nullptr;
      }
    }
    m_worker_ctx = nullptr;
  }
  m_inited = false;
  return -1;
}

int PQRefIterator::Read() {
  if (m_seen_eof) {
    return -1;
  }

  if (m_worker_ctx == nullptr || table() == nullptr || table()->file == nullptr) {
    PrintError(HA_ERR_INTERNAL_ERROR);
    return 1;
  }

  if (m_first_record_since_init) {
    m_first_record_since_init = false;
    if (m_ref->impossible_null_ref()) {
      table()->set_no_row();
      m_seen_eof = true;
      return -1;
    }
    if (construct_lookup(thd(), table(), m_ref)) {
      table()->set_no_row();
      m_seen_eof = true;
      return -1;
    }
  }

  bool eof = false;
  for (;;) {
    const int error =
        table()->file->pq_worker_scan_next(m_worker_ctx, table()->record[0],
                                           &eof);
    if (error == 0) {
      if (eof) {
        m_seen_eof = true;
        table()->set_no_row();
        return -1;
      }
      break;
    }
    if (error == HA_ERR_RECORD_DELETED && !thd()->killed) {
      continue;
    }
    return HandleError(error);
  }

  if (m_examined_rows != nullptr) {
    ++*m_examined_rows;
  }
  return 0;
}

unique_ptr_destroy_only<RowIterator> TryCreatePQSecondaryCoveringRangeIterator(
    THD *thd, MEM_ROOT *mem_root, JOIN *join, AccessPath *path,
    ha_rows *examined_rows, bool is_root_range_scan) {
  if (thd == nullptr || mem_root == nullptr || join == nullptr ||
      path == nullptr || path->type != AccessPath::INDEX_RANGE_SCAN) {
    return nullptr;
  }

  if (!is_root_range_scan) {
    return nullptr;
  }

  if (!thd->variables.parallel_query ||
      !thd->variables.parallel_query_experimental_threaded_dop ||
      thd->pq_is_worker) {
    return nullptr;
  }

  if (thd->lex == nullptr || thd->lex->is_explain() ||
      thd->lex->sql_command != SQLCOM_SELECT) {
    return nullptr;
  }

  if (!join->plan_is_single_table() || join->query_block == nullptr ||
      !join->query_block->is_simple_query_block() ||
      join->query_block->is_ordered() ||
      join->query_block->is_explicitly_grouped() ||
      join->query_block->having_cond() != nullptr) {
    return nullptr;
  }

  if (path->num_output_rows() > kPQSecondaryCoveringMaxEstimatedRows) {
    return nullptr;
  }

  const auto &param = path->index_range_scan();
  if (param.geometry || param.reverse || param.num_ranges != 1 ||
      param.ranges == nullptr || param.used_key_part == nullptr) {
    return nullptr;
  }

  TABLE *table = param.used_key_part[0].field != nullptr
                     ? param.used_key_part[0].field->table
                     : nullptr;
  if (table == nullptr || table->s == nullptr || table->file == nullptr ||
      table->key_info == nullptr || table->s->db_type() != innodb_hton ||
      table->part_info != nullptr || param.index >= table->s->keys ||
      param.index == table->s->primary_key) {
    return nullptr;
  }

  if (!pq_secondary_key_parts_are_safe(table, param.index)) {
    return nullptr;
  }

  if (table->file->pushed_idx_cond != nullptr) {
    if (table->file->pushed_idx_cond_keyno != param.index ||
        !pq_secondary_noncovering_clustered_read_set_is_safe(table,
                                                            param.index)) {
      return nullptr;
    }

    pq_set_execution_state(thd, PQ_execution_state::ITERATOR_SELECTED);
    return NewIterator<PQSecondaryNoncoveringIcpRangeIterator>(
        thd, mem_root, mem_root, table, examined_rows,
        path->num_output_rows(), param.index, param.need_rows_in_rowid_order,
        param.reuse_handler, param.mrr_flags, param.mrr_buf_size,
        Bounds_checked_array{param.ranges, param.num_ranges});
  }

  if (!pq_secondary_covering_read_set_is_safe(table, param.index)) {
    if (!pq_secondary_noncovering_clustered_read_set_is_safe(table,
                                                            param.index)) {
      return nullptr;
    }

    const ha_rows max_table_rows = table->file->estimate_rows_upper_bound();
    if (max_table_rows == HA_POS_ERROR ||
        max_table_rows > kPQSecondaryNoncoveringRangeMaxRows) {
      return nullptr;
    }

    pq_set_execution_state(thd, PQ_execution_state::ITERATOR_SELECTED);
    return NewIterator<PQSecondaryNoncoveringRangeIterator>(
        thd, mem_root, mem_root, table, examined_rows,
        path->num_output_rows(), param.index, param.need_rows_in_rowid_order,
        param.reuse_handler, param.mrr_flags, param.mrr_buf_size,
        Bounds_checked_array{param.ranges, param.num_ranges});
  }

  pq_set_execution_state(thd, PQ_execution_state::ITERATOR_SELECTED);
  return NewIterator<PQSecondaryCoveringRangeIterator>(
      thd, mem_root, mem_root, table, examined_rows,
      path->num_output_rows(), param.index, param.need_rows_in_rowid_order,
      param.reuse_handler, param.mrr_flags, param.mrr_buf_size,
      Bounds_checked_array{param.ranges, param.num_ranges});
}

unique_ptr_destroy_only<RowIterator> TryCreatePQPrimaryClusteredRangeIterator(
    THD *thd, MEM_ROOT *mem_root, JOIN *join, AccessPath *path,
    ha_rows *examined_rows, bool is_root_range_scan) {
  if (thd == nullptr || mem_root == nullptr || join == nullptr ||
      path == nullptr || path->type != AccessPath::INDEX_RANGE_SCAN) {
    return nullptr;
  }

  if (!is_root_range_scan) {
    return nullptr;
  }

  if (!thd->variables.parallel_query ||
      !thd->variables.parallel_query_experimental_threaded_dop ||
      thd->pq_is_worker) {
    return nullptr;
  }

  if (thd->lex == nullptr || thd->lex->is_explain() ||
      thd->lex->sql_command != SQLCOM_SELECT) {
    return nullptr;
  }

  if (!join->plan_is_single_table() || join->query_block == nullptr ||
      !join->query_block->is_simple_query_block() ||
      join->query_block->is_ordered() ||
      join->query_block->is_explicitly_grouped() ||
      join->query_block->having_cond() != nullptr) {
    return nullptr;
  }

  if (path->num_output_rows() > kPQPrimaryClusteredMaxEstimatedRows) {
    return nullptr;
  }

  const auto &param = path->index_range_scan();
  if (param.geometry || param.reverse || param.num_ranges != 1 ||
      param.ranges == nullptr || param.used_key_part == nullptr) {
    return nullptr;
  }
  if (!pq_primary_clustered_range_endpoints_are_safe(param.ranges[0])) {
    return nullptr;
  }

  TABLE *table = param.used_key_part[0].field != nullptr
                     ? param.used_key_part[0].field->table
                     : nullptr;
  if (table == nullptr || table->s == nullptr || table->file == nullptr ||
      table->key_info == nullptr || table->s->db_type() != innodb_hton ||
      table->part_info != nullptr || table->file->pushed_idx_cond != nullptr ||
      table->s->primary_key == MAX_KEY || param.index != table->s->primary_key ||
      !table->file->primary_key_is_clustered()) {
    return nullptr;
  }

  if (!pq_primary_clustered_key_parts_are_safe(table, param.index) ||
      !pq_primary_clustered_read_set_is_safe(table)) {
    return nullptr;
  }

  pq_set_execution_state(thd, PQ_execution_state::ITERATOR_SELECTED);
  return NewIterator<PQPrimaryClusteredRangeIterator>(
      thd, mem_root, mem_root, table, examined_rows, path->num_output_rows(),
      param.index, param.need_rows_in_rowid_order, param.reuse_handler,
      param.mrr_flags, param.mrr_buf_size,
      Bounds_checked_array{param.ranges, param.num_ranges});
}

unique_ptr_destroy_only<RowIterator> TryCreatePQSecondaryCoveringRefIterator(
    THD *thd, MEM_ROOT *mem_root, JOIN *join, AccessPath *path,
    ha_rows *examined_rows, bool is_root_ref) {
  if (thd == nullptr || mem_root == nullptr || join == nullptr ||
      path == nullptr || path->type != AccessPath::REF) {
    return nullptr;
  }

  if (!thd->variables.parallel_query ||
      !thd->variables.parallel_query_experimental_threaded_dop ||
      thd->pq_is_worker) {
    return nullptr;
  }

  if (thd->lex == nullptr || thd->lex->is_explain() ||
      thd->lex->sql_command != SQLCOM_SELECT) {
    return nullptr;
  }

  const auto &param = path->ref();
  TABLE *table = param.table;
  Index_lookup *ref = param.ref;

  if (!is_root_ref) {
    if (param.reverse) {
      pq_secondary_reverse_ref_reject_probe();
      return nullptr;
    }
    if (pq_secondary_dependent_ref_gate_is_safe(join, path, param.reverse) &&
        pq_secondary_ref_is_dependent_scaffold_candidate(table, ref)) {
      bool dependent_ref_smoke_enabled = false;
      DBUG_EXECUTE_IF("pq_secondary_dependent_ref_single_probe_smoke",
                      dependent_ref_smoke_enabled = true;);
      if (dependent_ref_smoke_enabled) {
        return NewIterator<PQSecondaryDependentRefSmokeIterator>(
            thd, mem_root, mem_root, table, ref, param.use_order,
            path->num_output_rows(), examined_rows);
      }
      pq_set_execution_state(thd, PQ_execution_state::ITERATOR_SELECTED);
      return NewIterator<PQSecondaryDependentRefIterator>(
          thd, mem_root, mem_root, table, ref, param.use_order,
          path->num_output_rows(), examined_rows);
    }
    return nullptr;
  }

  if (param.reverse) {
    pq_secondary_reverse_ref_reject_probe();
    return nullptr;
  }

  if (!join->plan_is_single_table() || join->query_block == nullptr ||
      !join->query_block->is_simple_query_block() ||
      join->query_block->is_ordered() ||
      join->query_block->is_explicitly_grouped() ||
      join->query_block->having_cond() != nullptr) {
    return nullptr;
  }

  if (path->num_output_rows() > kPQSecondaryCoveringMaxEstimatedRows) {
    return nullptr;
  }

  if (table == nullptr || table->s == nullptr ||
      table->file == nullptr || table->key_info == nullptr ||
      table->s->db_type() != innodb_hton || table->part_info != nullptr ||
      table->file->pushed_idx_cond != nullptr || ref == nullptr) {
    return nullptr;
  }

  const uint keyno = static_cast<uint>(ref->key);
  if (!pq_secondary_ref_is_constant_and_safe(table, ref) ||
      !pq_secondary_key_parts_are_safe(table, keyno) ||
      !pq_secondary_covering_read_set_is_safe(table, keyno)) {
    return nullptr;
  }

  pq_set_execution_state(thd, PQ_execution_state::ITERATOR_SELECTED);
  return NewIterator<PQSecondaryCoveringRefIterator>(
      thd, mem_root, mem_root, table, ref, param.use_order,
      path->num_output_rows(), examined_rows);
}
