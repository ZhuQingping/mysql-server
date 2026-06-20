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
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/mysqld.h"
#include "sql/parallel_query/pq_handler.h"
#include "sql/parallel_query/sql_parallel.h"
#include "sql/range_optimizer/index_range_scan.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"

namespace {

constexpr double kPQSecondaryCoveringMaxEstimatedRows = 64.0;

struct PQ_copied_key_endpoint {
  key_range range{};
  std::vector<uchar> key;
  bool present{false};
};

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

bool ParallelScanIterator::Init() { return true; }

int ParallelScanIterator::Read() { return 1; }

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

bool PQblockScanIterator::Init() { return true; }

int PQblockScanIterator::Read() { return 1; }

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

bool PQRefIterator::Init() { return true; }

int PQRefIterator::Read() { return 1; }

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
      table->part_info != nullptr || table->file->pushed_idx_cond != nullptr ||
      param.index >= table->s->keys || param.index == table->s->primary_key) {
    return nullptr;
  }

  if (!pq_secondary_key_parts_are_safe(table, param.index) ||
      !pq_secondary_covering_read_set_is_safe(table, param.index)) {
    return nullptr;
  }

  pq_set_execution_state(thd, PQ_execution_state::ITERATOR_SELECTED);
  return NewIterator<PQSecondaryCoveringRangeIterator>(
      thd, mem_root, mem_root, table, examined_rows,
      path->num_output_rows(), param.index, param.need_rows_in_rowid_order,
      param.reuse_handler, param.mrr_flags, param.mrr_buf_size,
      Bounds_checked_array{param.ranges, param.num_ranges});
}
