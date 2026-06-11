/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in license.xml
   elsewhere in this distribution.  You may use this software under the
   terms of the GNU General Public License, version 2.0,
   or the terms of any other license of the available in this distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/parallel_query/pq_group_aggregate_iterator.h"

#include <utility>

#include "sql/iterators/row_iterator.h"         // TableRowIterator
#include "sql/join_optimizer/access_path.h"  // AccessPath
#include "sql/item_sum.h"                    // Item_sum
#include "sql/pfs_batch_mode.h"              // PFSBatchMode
#include "sql/parallel_query/sql_parallel.h"  // pq_global_stats
#include "scope_guard.h"                     // create_scope_guard
#include "sql/sql_executor.h"                // copy_funcs, sum funcs
#include "sql/sql_optimizer.h"               // JOIN, Switch_ref_item_slice
#include "sql/sql_tmp_table.h"               // instantiate_tmp_table
#include "sql/sql_class.h"                   // THD
#include "sql/table.h"                       // TABLE
#include "sql/temp_table_param.h"            // Temp_table_param

namespace {

bool pq_is_integer_field_type(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONGLONG:
      return true;
    default:
      return false;
  }
}

bool pq_groupby_dop1_temp_shape_supported(JOIN *join,
                                          Temp_table_param *temp_table_param,
                                          TABLE *table) {
  if (join == nullptr || temp_table_param == nullptr || table == nullptr ||
      table->group == nullptr || join->sum_funcs == nullptr) {
    return false;
  }

  if (temp_table_param->precomputed_group_by ||
      temp_table_param->group_parts != 1 ||
      temp_table_param->sum_func_count != 1) {
    return false;
  }

  if (table->group->next != nullptr ||
      table->group->field_in_tmp_table == nullptr ||
      !pq_is_integer_field_type(table->group->field_in_tmp_table->type())) {
    return false;
  }

  Item_sum *sum = join->sum_funcs[0];
  if (sum == nullptr || join->sum_funcs[1] != nullptr) {
    return false;
  }

  return sum->sum_func() == Item_sum::COUNT_FUNC && !sum->has_with_distinct();
}

struct PQ_integer_group_state {
  int64 key{0};
  uint64 count_star{0};
  uint64 count_value{0};
  int64 sum{0};
  int64 min{0};
  int64 max{0};
  bool has_value{false};
};

bool pq_accumulate_integer_group(PQ_integer_group_state *groups,
                                 uint32 *group_count, int64 key, int64 value,
                                 bool value_is_null) {
  PQ_integer_group_state *state = nullptr;
  for (uint32 i = 0; i < *group_count; ++i) {
    if (groups[i].key == key) {
      state = &groups[i];
      break;
    }
  }

  if (state == nullptr) {
    if (*group_count >= 4) return true;
    state = &groups[*group_count];
    state->key = key;
    ++(*group_count);
  }

  ++state->count_star;
  if (value_is_null) return false;

  ++state->count_value;
  state->sum += value;
  if (!state->has_value) {
    state->min = value;
    state->max = value;
    state->has_value = true;
  } else {
    if (value < state->min) state->min = value;
    if (value > state->max) state->max = value;
  }
  return false;
}

const PQ_integer_group_state *pq_find_group(const PQ_integer_group_state *groups,
                                            uint32 group_count, int64 key) {
  for (uint32 i = 0; i < group_count; ++i) {
    if (groups[i].key == key) return &groups[i];
  }
  return nullptr;
}

class PQTemptableGroupAggregateIterator final : public TableRowIterator {
 public:
  PQTemptableGroupAggregateIterator(
      THD *thd, unique_ptr_destroy_only<RowIterator> subquery_iterator,
      Temp_table_param *temp_table_param, TABLE *table,
      unique_ptr_destroy_only<RowIterator> table_iterator, JOIN *join,
      int ref_slice)
      : TableRowIterator(thd, table),
        m_subquery_iterator(std::move(subquery_iterator)),
        m_table_iterator(std::move(table_iterator)),
        m_temp_table_param(temp_table_param),
        m_join(join),
        m_ref_slice(ref_slice) {}

  bool Init() override {
    if (m_subquery_iterator == nullptr || m_table_iterator == nullptr ||
        m_temp_table_param == nullptr || m_join == nullptr) {
      return true;
    }

    m_join->set_ref_item_slice(REF_SLICE_SAVED_BASE);

    if (m_subquery_iterator->Init()) {
      return true;
    }

    if (!table()->is_created()) {
      if (instantiate_tmp_table(thd(), table())) {
        return true;
      }
      empty_record(table());
    } else {
      if (table()->file->inited) {
        table()->file->ha_index_or_rnd_end();
      }
      table()->file->ha_delete_all_rows();
    }

    if (table()->file->ha_index_init(0, false)) {
      return true;
    }
    auto end_unique_index =
        create_scope_guard([&] { table()->file->ha_index_end(); });

    PFSBatchMode pfs_batch_mode(m_subquery_iterator.get());
    for (;;) {
      int read_error = m_subquery_iterator->Read();
      if (read_error > 0 || thd()->is_error()) {
        return true;
      }
      if (read_error < 0) {
        break;
      }
      if (thd()->killed) {
        thd()->send_kill_message();
        return true;
      }

      if (copy_funcs(m_temp_table_param, thd(), CFT_FIELDS)) {
        return true;
      }

      bool group_found = false;
      if (using_hash_key()) {
        if (copy_funcs(m_temp_table_param, thd())) {
          return true;
        }
        group_found = !check_unique_constraint(table());
      } else {
        for (ORDER *group = table()->group; group; group = group->next) {
          Item *item = *group->item;
          item->save_org_in_field(group->field_in_tmp_table);
          if (item->is_nullable()) {
            group->buff[-1] =
                static_cast<char>(group->field_in_tmp_table->is_null());
          }
        }
        const uchar *key = m_temp_table_param->group_buff;
        group_found = !table()->file->ha_index_read_map(
            table()->record[1], key, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
      }

      if (group_found) {
        restore_record(table(), record[1]);
        update_tmptable_sum_func(m_join->sum_funcs, table());
        if (thd()->is_error()) {
          return true;
        }

        int error = table()->file->ha_update_row(table()->record[1],
                                                 table()->record[0]);
        if (error != 0 && error != HA_ERR_RECORD_IS_THE_SAME) {
          if (move_table_to_disk(error, false)) {
            end_unique_index.release();
            return true;
          }

          const uchar *key = using_hash_key()
                                 ? table()->hash_field->field_ptr()
                                 : m_temp_table_param->group_buff;
          if (table()->file->ha_index_read_map(
                  table()->record[1], key, HA_WHOLE_KEY,
                  HA_READ_KEY_EXACT)) {
            return true;
          }

          restore_record(table(), record[1]);
          update_tmptable_sum_func(m_join->sum_funcs, table());
          if (thd()->is_error()) {
            return true;
          }

          error = table()->file->ha_update_row(table()->record[1],
                                               table()->record[0]);
          if (error != 0 && error != HA_ERR_RECORD_IS_THE_SAME) {
            PrintError(error);
            return true;
          }
        }
        continue;
      }

      Switch_ref_item_slice slice_switch(m_join, m_ref_slice);

      if (!using_hash_key()) {
        ORDER *group;
        KEY_PART_INFO *key_part;
        for (group = table()->group, key_part = table()->key_info[0].key_part;
             group; group = group->next, key_part++) {
          if (key_part->null_bit) {
            memcpy(table()->record[0] + key_part->offset - 1,
                   group->buff - 1, 1);
          }
        }
        if (copy_funcs(m_temp_table_param, thd())) {
          return true;
        }
      }

      init_tmptable_sum_functions(m_join->sum_funcs);
      if (thd()->is_error()) {
        return true;
      }

      int error = table()->file->ha_write_row(table()->record[0]);
      if (error != 0) {
        if (error == HA_ERR_FOUND_DUPP_KEY) {
          for (ORDER *group = table()->group; group; group = group->next) {
            if (group->field_in_tmp_table->type() == MYSQL_TYPE_TIMESTAMP) {
              my_error(ER_GROUPING_ON_TIMESTAMP_IN_DST, MYF(0));
              return true;
            }
          }
        }

        if (move_table_to_disk(error, true)) {
          end_unique_index.release();
          return true;
        }
      }
    }

    table()->file->ha_index_end();
    end_unique_index.release();

    table()->materialized = true;

    if (m_table_iterator->Init()) {
      return true;
    }

    if (!m_executed_counted) {
      pq_global_stats.groupby_dop1_temp_table_executed.fetch_add(
          1, std::memory_order_relaxed);
      m_executed_counted = true;
    }
    return false;
  }

  int Read() override {
    if (m_table_iterator == nullptr) return 1;
    if (m_join != nullptr && m_ref_slice != -1 &&
        !m_join->ref_items[m_ref_slice].is_null()) {
      m_join->set_ref_item_slice(m_ref_slice);
    }
    return m_table_iterator->Read();
  }

  void SetNullRowFlag(bool is_null_row) override {
    if (m_table_iterator != nullptr) {
      m_table_iterator->SetNullRowFlag(is_null_row);
    }
  }

  void UnlockRow() override {}

  void StartPSIBatchMode() override {
    // Batch mode is managed explicitly while materializing input rows.
  }

  void EndPSIBatchModeIfStarted() override {
    if (m_table_iterator != nullptr) {
      m_table_iterator->EndPSIBatchModeIfStarted();
    }
    if (m_subquery_iterator != nullptr) {
      m_subquery_iterator->EndPSIBatchModeIfStarted();
    }
  }

 private:
  bool using_hash_key() const { return table()->hash_field; }

  bool move_table_to_disk(int error, bool was_insert) {
    if (create_ondisk_from_heap(thd(), table(), error, was_insert,
                                false, nullptr)) {
      return true;
    }
    error = table()->file->ha_index_init(0, false);
    if (error != 0) {
      PrintError(error);
      return true;
    }
    return false;
  }

  unique_ptr_destroy_only<RowIterator> m_subquery_iterator;
  unique_ptr_destroy_only<RowIterator> m_table_iterator;
  Temp_table_param *m_temp_table_param{nullptr};
  JOIN *const m_join{nullptr};
  const int m_ref_slice{-1};
  bool m_executed_counted{false};
};

}  // namespace

unique_ptr_destroy_only<RowIterator> TryCreatePQGroupAggregateIterator(
    THD *thd, MEM_ROOT *mem_root, JOIN *join, AccessPath *aggregate_path,
    unique_ptr_destroy_only<RowIterator> *child_iterator) {
  (void)mem_root;

  if (thd == nullptr || join == nullptr || aggregate_path == nullptr ||
      child_iterator == nullptr) {
    return nullptr;
  }

  if (aggregate_path->type != AccessPath::AGGREGATE) {
    return nullptr;
  }

  if (!thd->variables.parallel_query ||
      !thd->variables.parallel_query_experimental_groupby_dop1 ||
      thd->variables.parallel_default_dop != 1) {
    return nullptr;
  }
  pq_global_stats.groupby_dop1_factory_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (!join->pq_eligible || aggregate_path->aggregate().rollup ||
      aggregate_path->aggregate().child == nullptr ||
      aggregate_path->aggregate().child->type != AccessPath::TABLE_SCAN) {
    pq_global_stats.groupby_dop1_factory_fallback.fetch_add(
        1, std::memory_order_relaxed);
    return nullptr;
  }

  /*
    V2-12A-3.2 stops here intentionally. Later substeps will replace this
    nullptr with a real PQGroupAggregateIterator after typed state, SQL
    result-row construction, and child iterator ownership are implemented.
  */
  pq_global_stats.groupby_dop1_factory_fallback.fetch_add(
      1, std::memory_order_relaxed);
  return nullptr;
}

unique_ptr_destroy_only<RowIterator> TryCreatePQTemptableGroupAggregateIterator(
    THD *thd, MEM_ROOT *mem_root, JOIN *join, AccessPath *aggregate_path,
    unique_ptr_destroy_only<RowIterator> *subquery_iterator,
    unique_ptr_destroy_only<RowIterator> *table_iterator,
    Temp_table_param *temp_table_param, TABLE *table, int ref_slice) {
  (void)mem_root;

  if (thd == nullptr || join == nullptr || aggregate_path == nullptr ||
      subquery_iterator == nullptr || table_iterator == nullptr ||
      temp_table_param == nullptr || table == nullptr || ref_slice < -1) {
    return nullptr;
  }

  if (aggregate_path->type != AccessPath::TEMPTABLE_AGGREGATE) {
    return nullptr;
  }

  if (!thd->variables.parallel_query ||
      !thd->variables.parallel_query_experimental_groupby_dop1 ||
      thd->variables.parallel_default_dop != 1) {
    return nullptr;
  }
  pq_global_stats.groupby_dop1_factory_attempts.fetch_add(
      1, std::memory_order_relaxed);

  const bool supported_shape =
      pq_groupby_dop1_temp_shape_supported(join, temp_table_param, table);
  if (supported_shape) {
    pq_global_stats.groupby_temp_shape_supported.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    pq_global_stats.groupby_temp_shape_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  }

  if (!join->pq_eligible || !supported_shape) {
    pq_global_stats.groupby_dop1_factory_fallback.fetch_add(
        1, std::memory_order_relaxed);
    return nullptr;
  }

  pq_global_stats.groupby_dop1_factory_selected.fetch_add(
      1, std::memory_order_relaxed);
  return unique_ptr_destroy_only<RowIterator>(
      new (mem_root) PQTemptableGroupAggregateIterator(
          thd, std::move(*subquery_iterator), temp_table_param, table,
          std::move(*table_iterator), join, ref_slice));
}

bool RunPQGroupAggregateTypedStateSmoke(uint32 *groups_built,
                                        uint64 *sum_total) {
  if (groups_built == nullptr || sum_total == nullptr) return true;

  PQ_integer_group_state groups[4];
  uint32 group_count = 0;

  if (pq_accumulate_integer_group(groups, &group_count, 1, 10, false) ||
      pq_accumulate_integer_group(groups, &group_count, 1, 20, false) ||
      pq_accumulate_integer_group(groups, &group_count, 2, 0, true) ||
      pq_accumulate_integer_group(groups, &group_count, 2, 5, false) ||
      pq_accumulate_integer_group(groups, &group_count, 3, 7, false)) {
    return true;
  }

  const PQ_integer_group_state *g1 = pq_find_group(groups, group_count, 1);
  const PQ_integer_group_state *g2 = pq_find_group(groups, group_count, 2);
  const PQ_integer_group_state *g3 = pq_find_group(groups, group_count, 3);
  if (g1 == nullptr || g2 == nullptr || g3 == nullptr || group_count != 3) {
    return true;
  }

  if (g1->count_star != 2 || g1->count_value != 2 || g1->sum != 30 ||
      g1->min != 10 || g1->max != 20) {
    return true;
  }
  if (g2->count_star != 2 || g2->count_value != 1 || g2->sum != 5 ||
      g2->min != 5 || g2->max != 5) {
    return true;
  }
  if (g3->count_star != 1 || g3->count_value != 1 || g3->sum != 7 ||
      g3->min != 7 || g3->max != 7) {
    return true;
  }

  *groups_built = group_count;
  *sum_total = static_cast<uint64>(g1->sum + g2->sum + g3->sum);
  return false;
}
