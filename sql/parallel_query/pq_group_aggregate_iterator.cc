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

#include <climits>
#include <utility>

#include "prealloced_array.h"                 // Prealloced_array
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

  if (sum->has_with_distinct()) {
    return false;
  }

  switch (sum->sum_func()) {
    case Item_sum::COUNT_FUNC:
      return true;
    case Item_sum::SUM_FUNC:
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC: {
      if (sum->argument_count() != 1 || sum->arguments() == nullptr ||
          sum->arguments()[0] == nullptr ||
          sum->arguments()[0]->type() != Item::FIELD_ITEM) {
        return false;
      }
      const Item_field *field_item =
          down_cast<const Item_field *>(sum->arguments()[0]);
      return field_item->field != nullptr &&
             pq_is_integer_field_type(field_item->field->type());
    }
    default:
      return false;
  }
}

bool pq_groupby_dop_partial_supported(JOIN *join,
                                      Temp_table_param *temp_table_param,
                                      TABLE *table) {
  if (!pq_groupby_dop1_temp_shape_supported(join, temp_table_param, table)) {
    return false;
  }
  Item_sum *sum = join->sum_funcs[0];
  if (sum == nullptr) return false;

  if (table->group->item == nullptr) return false;
  Item *group_item = *table->group->item;
  if (group_item == nullptr || group_item->type() != Item::FIELD_ITEM) {
    return false;
  }
  auto *group_field_item = down_cast<Item_field *>(group_item);
  if (group_field_item->field == nullptr ||
      group_field_item->field->table == nullptr ||
      group_field_item->field->is_nullable()) {
    return false;
  }

  if (sum->argument_count() != 1 || sum->arguments() == nullptr ||
      sum->arguments()[0] == nullptr ||
      sum->arguments()[0]->type() != Item::FIELD_ITEM) {
    return false;
  }
  auto *value_item = down_cast<Item_field *>(sum->arguments()[0]);
  if (value_item->field == nullptr ||
      value_item->field->table != group_field_item->field->table) {
    return false;
  }

  switch (sum->sum_func()) {
    case Item_sum::COUNT_FUNC:
      return !sum->arguments()[0]->is_nullable();
    case Item_sum::SUM_FUNC:
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC:
      return true;
    default:
      return false;
  }
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

struct PQ_count_group_state {
  longlong key{0};
  ulonglong count{0};
  longlong value{0};
  bool has_value{false};
};

bool pq_accumulate_count_group(Prealloced_array<PQ_count_group_state, 16> *groups,
                               longlong key) {
  for (PQ_count_group_state &group : *groups) {
    if (group.key == key) {
      ++group.count;
      return false;
    }
  }

  PQ_count_group_state group;
  group.key = key;
  group.count = 1;
  return groups->push_back(group);
}

bool pq_accumulate_min_max_group(
    Prealloced_array<PQ_count_group_state, 16> *groups, longlong key,
    longlong value, bool value_is_null, bool is_min) {
  PQ_count_group_state *state = nullptr;
  for (PQ_count_group_state &group : *groups) {
    if (group.key == key) {
      state = &group;
      break;
    }
  }

  if (state == nullptr) {
    PQ_count_group_state group;
    group.key = key;
    if (groups->push_back(group)) {
      return true;
    }
    state = &groups->back();
  }

  if (value_is_null) {
    return false;
  }

  if (!state->has_value) {
    state->value = value;
    state->has_value = true;
    return false;
  }

  if ((is_min && value < state->value) || (!is_min && value > state->value)) {
    state->value = value;
  }
  return false;
}

bool pq_add_longlong_checked(longlong lhs, longlong rhs, longlong *result) {
  if ((rhs > 0 && lhs > LLONG_MAX - rhs) ||
      (rhs < 0 && lhs < LLONG_MIN - rhs)) {
    return true;
  }
  *result = lhs + rhs;
  return false;
}

bool pq_accumulate_sum_group(Prealloced_array<PQ_count_group_state, 16> *groups,
                             longlong key, longlong value,
                             bool value_is_null, bool *overflow) {
  PQ_count_group_state *state = nullptr;
  for (PQ_count_group_state &group : *groups) {
    if (group.key == key) {
      state = &group;
      break;
    }
  }

  if (state == nullptr) {
    PQ_count_group_state group;
    group.key = key;
    if (groups->push_back(group)) {
      return true;
    }
    state = &groups->back();
  }

  if (value_is_null) {
    return false;
  }

  if (!state->has_value) {
    state->value = value;
    state->has_value = true;
    return false;
  }

  longlong new_sum = 0;
  if (pq_add_longlong_checked(state->value, value, &new_sum)) {
    *overflow = true;
    return false;
  }
  state->value = new_sum;
  return false;
}

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

    TABLE *dop_source_table = nullptr;
    uint32 dop_group_field_index = 0;
    uint32 dop_value_field_index = 0;
    PQ_partial_group_agg_kind dop_agg_kind = PQ_partial_group_agg_kind::SUM;
    if (can_use_dop_partial_path(&dop_source_table, &dop_group_field_index,
                                 &dop_value_field_index, &dop_agg_kind)) {
      return InitDopPartialPath(dop_source_table, dop_group_field_index,
                                dop_value_field_index, dop_agg_kind);
    }

    if (can_use_typed_count_path()) {
      return InitTypedCountPath();
    }
    if (can_use_typed_min_max_path()) {
      Item_sum *sum = m_join->sum_funcs[0];
      return InitTypedMinMaxPath(sum->sum_func() == Item_sum::MIN_FUNC);
    }
    if (can_use_typed_sum_path()) {
      return InitTypedSumPath();
    }

    return InitLegacyTempTablePath();
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

  bool can_use_dop_partial_path(TABLE **source_table, uint32 *group_field_index,
                                uint32 *value_field_index,
                                PQ_partial_group_agg_kind *agg_kind) const {
    if (source_table == nullptr || group_field_index == nullptr ||
        value_field_index == nullptr || agg_kind == nullptr) {
      return false;
    }
    *source_table = nullptr;

    if (thd()->variables.parallel_default_dop != 2 ||
        !thd()->variables.parallel_query_experimental_threaded_dop ||
        !thd()->variables.parallel_query_experimental_groupby_dop1) {
      return false;
    }

    Item_sum *sum = m_join->sum_funcs[0];
    if (sum == nullptr) return false;
    switch (sum->sum_func()) {
      case Item_sum::COUNT_FUNC:
        if (!can_use_typed_count_path()) return false;
        *agg_kind = PQ_partial_group_agg_kind::COUNT;
        break;
      case Item_sum::SUM_FUNC:
        if (!can_use_typed_sum_path()) return false;
        *agg_kind = PQ_partial_group_agg_kind::SUM;
        break;
      case Item_sum::MIN_FUNC:
        if (!can_use_typed_min_max_path()) return false;
        *agg_kind = PQ_partial_group_agg_kind::MIN;
        break;
      case Item_sum::MAX_FUNC:
        if (!can_use_typed_min_max_path()) return false;
        *agg_kind = PQ_partial_group_agg_kind::MAX;
        break;
      default:
        return false;
    }

    Item *group_item = *table()->group->item;
    if (group_item == nullptr || group_item->type() != Item::FIELD_ITEM) {
      return false;
    }
    auto *group_field_item = down_cast<Item_field *>(group_item);
    if (sum->argument_count() != 1 || sum->arguments() == nullptr ||
        sum->arguments()[0] == nullptr ||
        sum->arguments()[0]->type() != Item::FIELD_ITEM) {
      return false;
    }
    auto *value_item = down_cast<Item_field *>(sum->arguments()[0]);
    if (group_field_item->field == nullptr || value_item->field == nullptr ||
        group_field_item->field->table == nullptr ||
        group_field_item->field->table != value_item->field->table ||
        group_field_item->field->is_nullable()) {
      return false;
    }

    TABLE *base_table = group_field_item->field->table;
    if (base_table->file == nullptr || base_table->s == nullptr ||
        base_table->s->blob_fields > 0) {
      return false;
    }

    *source_table = base_table;
    *group_field_index = group_field_item->field->field_index();
    *value_field_index = value_item->field->field_index();
    return true;
  }

  bool InitLegacyTempTablePath() {
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

  bool InitDopPartialPath(TABLE *source_table, uint32 group_field_index,
                          uint32 value_field_index,
                          PQ_partial_group_agg_kind agg_kind) {
    m_join->set_ref_item_slice(REF_SLICE_SAVED_BASE);

    if (source_table == nullptr || source_table->file == nullptr) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    if (prepare_table_for_materialization()) {
      return true;
    }

    if (table()->file->ha_index_init(0, false)) {
      return true;
    }
    auto end_unique_index =
        create_scope_guard([&] { table()->file->ha_index_end(); });

    PQ_Leader_context *leader_ctx = nullptr;
    uint actual_dop = 0;
    int error = source_table->file->pq_leader_scan_init(
        thd(), &leader_ctx, PQ_leader_scan_mode::EXECUTE,
        thd()->variables.parallel_default_dop, &actual_dop, false);
    if (error != 0) {
      table()->file->ha_index_end();
      end_unique_index.release();
      if (error == HA_ERR_UNSUPPORTED) {
        switch (agg_kind) {
          case PQ_partial_group_agg_kind::COUNT:
            return InitTypedCountPath();
          case PQ_partial_group_agg_kind::SUM:
            return InitTypedSumPath();
          case PQ_partial_group_agg_kind::MIN:
            return InitTypedMinMaxPath(true);
          case PQ_partial_group_agg_kind::MAX:
            return InitTypedMinMaxPath(false);
          default:
            PrintError(HA_ERR_INTERNAL_ERROR);
            return true;
        }
      }
      PrintError(error);
      return true;
    }
    auto end_leader_scan = create_scope_guard(
        [&] { source_table->file->pq_leader_scan_end(leader_ctx); });

    const uint dop = actual_dop > 0 ? actual_dop
                                    : thd()->variables.parallel_default_dop;
    Gather_operator gather(dop);
    PQ_partial_group_merge_slot_v1 merge_slots[16];
    uint32 worker_groups = 0;
    uint32 merged_groups = 0;
    if (gather.init() ||
        gather.configure_worker_open_contexts(source_table, leader_ctx, dop) ||
        gather.run_worker_partial_group_merge(
            thd(), source_table, group_field_index, value_field_index,
            agg_kind, merge_slots, 16, &worker_groups, &merged_groups)) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    Field *key_field = (*table()->group->item)->get_tmp_table_field();
    Item_sum *sum = m_join->sum_funcs[0];
    Field *result_field = sum->get_result_field();
    if (key_field == nullptr || result_field == nullptr) {
      PrintError(HA_ERR_INTERNAL_ERROR);
      return true;
    }

    for (uint32 i = 0; i < 16; ++i) {
      if (!merge_slots[i].used) continue;
      empty_record(table());
      key_field->set_notnull();
      key_field->store(merge_slots[i].group_key, false);

      switch (agg_kind) {
        case PQ_partial_group_agg_kind::COUNT:
          result_field->set_notnull();
          result_field->store(
              static_cast<longlong>(merge_slots[i].count_value), true);
          break;
        case PQ_partial_group_agg_kind::SUM:
          if (merge_slots[i].has_value) {
            result_field->set_notnull();
            result_field->store(merge_slots[i].sum, false);
          } else {
            result_field->store(0LL, false);
            result_field->set_null();
          }
          break;
        case PQ_partial_group_agg_kind::MIN:
          if (merge_slots[i].has_value) {
            result_field->set_notnull();
            result_field->store(merge_slots[i].min, false);
          } else {
            result_field->store(0LL, false);
            result_field->set_null();
          }
          break;
        case PQ_partial_group_agg_kind::MAX:
          if (merge_slots[i].has_value) {
            result_field->set_notnull();
            result_field->store(merge_slots[i].max, false);
          } else {
            result_field->store(0LL, false);
            result_field->set_null();
          }
          break;
        default:
          PrintError(HA_ERR_INTERNAL_ERROR);
          return true;
      }

      error = table()->file->ha_write_row(table()->record[0]);
      if (error != 0) {
        if (move_table_to_disk(error, true)) {
          end_unique_index.release();
          return true;
        }
      }
    }

    table()->file->ha_index_end();
    end_unique_index.release();
    end_leader_scan.release();
    source_table->file->pq_leader_scan_end(leader_ctx);

    table()->materialized = true;

    if (m_table_iterator->Init()) {
      return true;
    }

    if (!m_executed_counted) {
      pq_global_stats.queries_executed.fetch_add(1,
                                                 std::memory_order_relaxed);
      pq_global_stats.groupby_dop_partial_selected.fetch_add(
          1, std::memory_order_relaxed);
      pq_global_stats.groupby_dop_partial_worker_groups.fetch_add(
          worker_groups, std::memory_order_relaxed);
      pq_global_stats.groupby_dop_partial_merged_groups.fetch_add(
          merged_groups, std::memory_order_relaxed);
      m_executed_counted = true;
    }
    return false;
  }

  bool can_use_typed_count_path() const {
    if (using_hash_key() || table()->group == nullptr ||
        table()->group->next != nullptr || table()->group->item == nullptr ||
        *table()->group->item == nullptr || m_join->sum_funcs == nullptr ||
        m_join->sum_funcs[0] == nullptr || m_join->sum_funcs[1] != nullptr) {
      return false;
    }

    Item_sum *sum = m_join->sum_funcs[0];
    Field *key_field = (*table()->group->item)->get_tmp_table_field();
    Field *result_field = sum->get_result_field();
    if (sum->sum_func() != Item_sum::COUNT_FUNC || key_field == nullptr ||
        result_field == nullptr || key_field->is_nullable() ||
        key_field->is_unsigned()) {
      return false;
    }

    /*
      The typed COUNT path counts every input row. That is correct for
      COUNT(*) and COUNT(non_nullable_expr), but not for COUNT(nullable_expr).
    */
    return sum->argument_count() == 1 && sum->arguments() != nullptr &&
           sum->arguments()[0] != nullptr &&
           !sum->arguments()[0]->is_nullable();
  }

  bool can_use_typed_min_max_path() const {
    if (using_hash_key() || table()->group == nullptr ||
        table()->group->next != nullptr || table()->group->item == nullptr ||
        *table()->group->item == nullptr || m_join->sum_funcs == nullptr ||
        m_join->sum_funcs[0] == nullptr || m_join->sum_funcs[1] != nullptr) {
      return false;
    }

    Item_sum *sum = m_join->sum_funcs[0];
    Field *key_field = (*table()->group->item)->get_tmp_table_field();
    Field *result_field = sum->get_result_field();
    if ((sum->sum_func() != Item_sum::MIN_FUNC &&
         sum->sum_func() != Item_sum::MAX_FUNC) ||
        key_field == nullptr || result_field == nullptr ||
        key_field->is_nullable() || key_field->is_unsigned() ||
        !pq_is_integer_field_type(result_field->type())) {
      return false;
    }

    if (sum->argument_count() != 1 || sum->arguments() == nullptr ||
        sum->arguments()[0] == nullptr ||
        sum->arguments()[0]->type() != Item::FIELD_ITEM) {
      return false;
    }

    const Item_field *field_item =
        down_cast<const Item_field *>(sum->arguments()[0]);
    return field_item->field != nullptr && !field_item->field->is_unsigned() &&
           pq_is_integer_field_type(field_item->field->type());
  }

  bool can_use_typed_sum_path() const {
    if (using_hash_key() || table()->group == nullptr ||
        table()->group->next != nullptr || table()->group->item == nullptr ||
        *table()->group->item == nullptr || m_join->sum_funcs == nullptr ||
        m_join->sum_funcs[0] == nullptr || m_join->sum_funcs[1] != nullptr) {
      return false;
    }

    Item_sum *sum = m_join->sum_funcs[0];
    Field *key_field = (*table()->group->item)->get_tmp_table_field();
    Field *result_field = sum->get_result_field();
    if (sum->sum_func() != Item_sum::SUM_FUNC || key_field == nullptr ||
        result_field == nullptr || key_field->is_nullable() ||
        key_field->is_unsigned()) {
      return false;
    }

    if (sum->argument_count() != 1 || sum->arguments() == nullptr ||
        sum->arguments()[0] == nullptr ||
        sum->arguments()[0]->type() != Item::FIELD_ITEM) {
      return false;
    }

    const Item_field *field_item =
        down_cast<const Item_field *>(sum->arguments()[0]);
    if (field_item->field == nullptr || field_item->field->is_unsigned()) {
      return false;
    }

    switch (field_item->field->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
        return true;
      default:
        return false;
    }
  }

  bool prepare_table_for_materialization() {
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
    return false;
  }

  bool InitTypedCountPath() {
    m_join->set_ref_item_slice(REF_SLICE_SAVED_BASE);

    if (m_subquery_iterator->Init()) {
      return true;
    }

    if (prepare_table_for_materialization()) {
      return true;
    }

    if (table()->file->ha_index_init(0, false)) {
      return true;
    }
    auto end_unique_index =
        create_scope_guard([&] { table()->file->ha_index_end(); });

    Field *key_field = (*table()->group->item)->get_tmp_table_field();
    Item_sum *sum = m_join->sum_funcs[0];
    Field *result_field = sum->get_result_field();
    if (key_field == nullptr || result_field == nullptr) {
      return true;
    }

    Prealloced_array<PQ_count_group_state, 16> groups(PSI_NOT_INSTRUMENTED);

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

      if (key_field->is_null()) {
        return true;
      }

      if (pq_accumulate_count_group(&groups, key_field->val_int())) {
        return true;
      }
    }

    for (const PQ_count_group_state &group : groups) {
      empty_record(table());
      key_field->set_notnull();
      key_field->store(group.key, false);

      result_field->set_notnull();
      result_field->store(static_cast<longlong>(group.count), true);

      int error = table()->file->ha_write_row(table()->record[0]);
      if (error != 0) {
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
      pq_global_stats.groupby_dop1_typed_count_executed.fetch_add(
          1, std::memory_order_relaxed);
      m_executed_counted = true;
    }
    return false;
  }

  bool InitTypedMinMaxPath(bool is_min) {
    m_join->set_ref_item_slice(REF_SLICE_SAVED_BASE);

    if (m_subquery_iterator->Init()) {
      return true;
    }

    if (prepare_table_for_materialization()) {
      return true;
    }

    if (table()->file->ha_index_init(0, false)) {
      return true;
    }
    auto end_unique_index =
        create_scope_guard([&] { table()->file->ha_index_end(); });

    Field *key_field = (*table()->group->item)->get_tmp_table_field();
    Item_sum *sum = m_join->sum_funcs[0];
    Field *result_field = sum->get_result_field();
    Item_field *value_item = down_cast<Item_field *>(sum->arguments()[0]);
    Field *value_field = value_item->field;
    if (key_field == nullptr || result_field == nullptr ||
        value_field == nullptr) {
      return true;
    }

    Prealloced_array<PQ_count_group_state, 16> groups(PSI_NOT_INSTRUMENTED);

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

      if (key_field->is_null()) {
        return true;
      }

      if (pq_accumulate_min_max_group(&groups, key_field->val_int(),
                                      value_field->val_int(),
                                      value_field->is_null(), is_min)) {
        return true;
      }
    }

    for (const PQ_count_group_state &group : groups) {
      empty_record(table());
      key_field->set_notnull();
      key_field->store(group.key, false);

      if (group.has_value) {
        result_field->set_notnull();
        result_field->store(group.value, false);
      } else {
        result_field->set_null();
        result_field->store(0LL, false);
      }

      int error = table()->file->ha_write_row(table()->record[0]);
      if (error != 0) {
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
      pq_global_stats.groupby_dop1_typed_minmax_executed.fetch_add(
          1, std::memory_order_relaxed);
      m_executed_counted = true;
    }
    return false;
  }

  bool InitTypedSumPath() {
    m_join->set_ref_item_slice(REF_SLICE_SAVED_BASE);

    if (m_subquery_iterator->Init()) {
      return true;
    }

    if (prepare_table_for_materialization()) {
      return true;
    }

    if (table()->file->ha_index_init(0, false)) {
      return true;
    }
    auto end_unique_index =
        create_scope_guard([&] { table()->file->ha_index_end(); });

    Field *key_field = (*table()->group->item)->get_tmp_table_field();
    Item_sum *sum = m_join->sum_funcs[0];
    Field *result_field = sum->get_result_field();
    Item_field *value_item = down_cast<Item_field *>(sum->arguments()[0]);
    Field *value_field = value_item->field;
    if (key_field == nullptr || result_field == nullptr ||
        value_field == nullptr) {
      return true;
    }

    bool overflow = false;
    Prealloced_array<PQ_count_group_state, 16> groups(PSI_NOT_INSTRUMENTED);

    {
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

        if (key_field->is_null()) {
          return true;
        }

        if (pq_accumulate_sum_group(&groups, key_field->val_int(),
                                    value_field->val_int(),
                                    value_field->is_null(), &overflow)) {
          return true;
        }
        if (overflow) {
          break;
        }
      }
    }

    if (overflow) {
      table()->file->ha_index_end();
      end_unique_index.release();
      return InitLegacyTempTablePath();
    }

    for (const PQ_count_group_state &group : groups) {
      empty_record(table());
      key_field->set_notnull();
      key_field->store(group.key, false);

      if (group.has_value) {
        result_field->set_notnull();
        result_field->store(group.value, false);
      } else {
        result_field->store(0LL, false);
        result_field->set_null();
      }

      int error = table()->file->ha_write_row(table()->record[0]);
      if (error != 0) {
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
      pq_global_stats.groupby_dop1_typed_sum_executed.fetch_add(
          1, std::memory_order_relaxed);
      m_executed_counted = true;
    }
    return false;
  }

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

  const bool groupby_dop1_enabled =
      thd->variables.parallel_query_experimental_groupby_dop1 &&
      thd->variables.parallel_default_dop == 1;
  const bool groupby_dop2_partial_enabled =
      thd->variables.parallel_query_experimental_groupby_dop1 &&
      thd->variables.parallel_query_experimental_threaded_dop &&
      thd->variables.parallel_default_dop == 2;

  if (!thd->variables.parallel_query ||
      (!groupby_dop1_enabled && !groupby_dop2_partial_enabled)) {
    return nullptr;
  }
  if (groupby_dop2_partial_enabled) {
    pq_global_stats.groupby_dop_partial_attempts.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    pq_global_stats.groupby_dop1_factory_attempts.fetch_add(
        1, std::memory_order_relaxed);
  }

  const bool supported_shape = groupby_dop2_partial_enabled
                                   ? pq_groupby_dop_partial_supported(
                                         join, temp_table_param, table)
                                   : pq_groupby_dop1_temp_shape_supported(
                                         join, temp_table_param, table);
  if (supported_shape) {
    pq_global_stats.groupby_temp_shape_supported.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    pq_global_stats.groupby_temp_shape_unsupported.fetch_add(
        1, std::memory_order_relaxed);
  }

  if (!join->pq_eligible || !supported_shape) {
    if (groupby_dop2_partial_enabled) {
      pq_global_stats.groupby_dop_partial_fallback.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      pq_global_stats.groupby_dop1_factory_fallback.fetch_add(
          1, std::memory_order_relaxed);
    }
    return nullptr;
  }

  if (!groupby_dop2_partial_enabled) {
    pq_global_stats.groupby_dop1_factory_selected.fetch_add(
        1, std::memory_order_relaxed);
  }
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
