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

/** @file row/row0pread_pq.cc
InnoDB PQ (Parallel Query) pull-row adapter implementation.

TEMPORARY MVP adapter (Phase 6B-2): Implements clustered full scan
pull-row traversal for SQL-layer PQ workers.

Key design decisions for this temporary implementation:
1. B+tree partitioning uses the upstream Parallel_reader's add_scan()
   and partition() methods via a "capture callback" that records range
   boundaries without processing rows. The callback-based approach
   allows us to reuse the existing partition infrastructure.
2. Pull-row cursor traversal uses the standard InnoDB row search path
   (row_search_mvcc / general_fetch) through the worker's own
   row_prebuilt_t, which naturally handles visibility, BLOBs, and
   MySQL format conversion.
3. This avoids reimplementing low-level InnoDB APIs (btr_pcur_t page
   cursor traversal, rec_get_offsets, mtr savepoints, etc.) and
   instead leverages the well-tested existing scan path.
4. Real worker row reads remain disabled until the worker snapshot
   contract is proven. Worker-local row_search_mvcc() must not create
   an independent read view, and workers must not mutate the leader trx.

This adapter is temporary and should be refactored in Phase 8 to
share code with Parallel_reader via controlled public/protected
wrappers, reducing duplication.

Created 2026-06-02 by Qingping Zhu (PQ Phase 6B-2). */

#include "row0pread_pq.h"

#include "btr0pcur.h"
#include "ha_prototypes.h"
#include "page0page.h"
#include "que0que.h"
#include "read0read.h"
#include "row0pread.h"
#include "row0mysql.h"
#include "row0sel.h"
#include "srv0srv.h"
#include "trx0trx.h"
#include "ut0new.h"

/* ============================================================ */
/* InnoDB_pq_iter                                               */
/* ============================================================ */

InnoDB_pq_iter::~InnoDB_pq_iter() {
  if (m_heap != nullptr) {
    mem_heap_free(m_heap);
    m_heap = nullptr;
    m_tuple = nullptr;
  }
}

dberr_t InnoDB_pq_iter::assign(const dtuple_t *tuple,
                               const dict_index_t *index) {
  if (m_heap != nullptr) {
    mem_heap_free(m_heap);
    m_heap = nullptr;
    m_tuple = nullptr;
  }

  if (tuple == nullptr) {
    return DB_SUCCESS;
  }

  m_heap = mem_heap_create(sizeof(dtuple_t) + 256, UT_LOCATION_HERE);
  if (m_heap == nullptr) {
    return DB_OUT_OF_MEMORY;
  }

  auto copy = dtuple_copy(tuple, m_heap);
  for (size_t i = 0; i < dtuple_get_n_fields(copy); ++i) {
    dfield_dup(&copy->fields[i], m_heap);
  }
  if (index != nullptr) {
    const auto searchable = dict_index_get_n_unique_in_tree(index);
    const auto n_cmp = dtuple_get_n_fields_cmp(copy);
    if (n_cmp > searchable) {
      dtuple_set_n_fields_cmp(copy, searchable);
    }
  }

  m_tuple = copy;
  return DB_SUCCESS;
}

/** Map InnoDB dberr_t to handler error code for the pull-row adapter.
Used only for row_search_mvcc return values.

@param[in]  err  InnoDB dberr_t from row_search_mvcc
@return handler error code */
static int pq_map_innodb_error(dberr_t err) {
  switch (err) {
    case DB_OUT_OF_MEMORY:
      return HA_ERR_OUT_OF_MEM;
    case DB_INTERRUPTED:
      return HA_ERR_QUERY_INTERRUPTED;
    case DB_DEADLOCK:
      return HA_ERR_LOCK_DEADLOCK;
    case DB_LOCK_WAIT_TIMEOUT:
      return HA_ERR_LOCK_WAIT_TIMEOUT;
    default:
      /* For other errors, use a generic mapping.
      convert_error_code_to_mysql is only in ha_innodb.cc, so we
      provide a simplified mapping here. */
      return HA_ERR_GENERIC;
  }
}

/* ============================================================ */
/* InnoDB_pq_scan_ctx                                           */
/* ============================================================ */

InnoDB_pq_scan_ctx::InnoDB_pq_scan_ctx(dict_index_t *index, const trx_t *trx,
                                        bool is_compact,
                                        const page_size_t &page_size)
    : m_index(index),
      m_trx(trx),
      m_is_compact(is_compact),
      m_page_size(page_size) {}

InnoDB_pq_scan_ctx::~InnoDB_pq_scan_ctx() = default;

bool InnoDB_pq_scan_ctx::has_active_read_view() const {
  if (m_trx == nullptr) {
    return false;
  }

  return srv_read_only_mode ||
         (m_trx->read_view != nullptr &&
          MVCC::is_view_active(m_trx->read_view));
}

dberr_t InnoDB_pq_scan_ctx::validate_pull_adapter_gate() const {
  if (m_index == nullptr || !m_index->is_clustered() || m_trx == nullptr) {
    return DB_UNSUPPORTED;
  }

  if (!has_active_read_view()) {
    return DB_UNSUPPORTED;
  }

  if (m_ranges.size() != 1) {
    return DB_UNSUPPORTED;
  }

  const auto &range = m_ranges[0];
  if (range.m_start != nullptr || range.m_end != nullptr || range.m_split) {
    return DB_UNSUPPORTED;
  }

  return DB_SUCCESS;
}

bool InnoDB_pq_scan_ctx::store_mysql_record(byte *mysql_rec,
                                             row_prebuilt_t *prebuilt,
                                             const rec_t *rec, ulint *offsets,
                                             mem_heap_t *blob_heap) const {
  if (mysql_rec == nullptr || prebuilt == nullptr || rec == nullptr ||
      offsets == nullptr || m_index == nullptr || !m_index->is_clustered()) {
    return false;
  }

  return row_sel_store_mysql_rec(mysql_rec, prebuilt, rec, nullptr, true,
                                 m_index, m_index, offsets, false, nullptr,
                                 blob_heap);
}

bool InnoDB_pq_scan_ctx::store_callback_record(
    byte *mysql_rec, row_prebuilt_t *prebuilt,
    const Parallel_reader::Ctx *reader_ctx, mem_heap_t *blob_heap) const {
  if (reader_ctx == nullptr || reader_ctx->index() != m_index) {
    return false;
  }

  return store_mysql_record(mysql_rec, prebuilt, reader_ctx->record(),
                            reader_ctx->offsets(), blob_heap);
}

dberr_t InnoDB_pq_scan_ctx::smoke_callback_conversion(
    byte *mysql_rec, row_prebuilt_t *prebuilt, bool *converted) const {
  if (converted != nullptr) {
    *converted = false;
  }
  if (mysql_rec == nullptr || prebuilt == nullptr || converted == nullptr) {
    return DB_UNSUPPORTED;
  }

  if (m_index == nullptr || !m_index->is_clustered() || m_trx == nullptr ||
      !has_active_read_view()) {
    return DB_UNSUPPORTED;
  }

  Parallel_reader reader(0);
  Parallel_reader::Config config(Parallel_reader::Scan_range{}, m_index);

  bool saw_row = false;
  auto err = reader.add_scan(const_cast<trx_t *>(m_trx), config,
                             [&](const Parallel_reader::Ctx *reader_ctx) {
                               if (!store_callback_record(mysql_rec, prebuilt,
                                                          reader_ctx, nullptr)) {
                                 return DB_ERROR;
                               }
                               saw_row = true;
                               return DB_INTERRUPTED;
                             });
  if (err != DB_SUCCESS) {
    return err;
  }

  err = reader.run(0);
  if (saw_row && err == DB_INTERRUPTED) {
    *converted = true;
    return DB_SUCCESS;
  }

  if (err == DB_SUCCESS) {
    *converted = saw_row;
  }
  return err;
}

dberr_t InnoDB_pq_scan_ctx::produce_callback_rows(
    byte *mysql_rec, row_prebuilt_t *prebuilt, PQ_row_sink *row_sink) const {
  InnoDB_pq_range full_range;
  return produce_callback_rows_for_range(mysql_rec, prebuilt, row_sink,
                                         &full_range);
}

dberr_t InnoDB_pq_scan_ctx::produce_callback_rows_for_range(
    byte *mysql_rec, row_prebuilt_t *prebuilt, PQ_row_sink *row_sink,
    const InnoDB_pq_range *range) const {
  if (mysql_rec == nullptr || prebuilt == nullptr || row_sink == nullptr) {
    return DB_UNSUPPORTED;
  }

  if (m_index == nullptr || !m_index->is_clustered() || m_trx == nullptr ||
      !has_active_read_view()) {
    return DB_UNSUPPORTED;
  }

  if (range == nullptr) {
    return DB_SUCCESS;
  }

  Parallel_reader reader(0);
  Parallel_reader::Scan_range scan_range(
      range->m_start != nullptr ? range->m_start->tuple() : nullptr,
      range->m_end != nullptr ? range->m_end->tuple() : nullptr);
  Parallel_reader::Config config(scan_range, m_index);

  bool stop_requested = false;
  auto err = reader.add_scan(const_cast<trx_t *>(m_trx), config,
                             [&](const Parallel_reader::Ctx *reader_ctx) {
                               if (row_sink->should_abort()) {
                                 stop_requested = true;
                                 return DB_INTERRUPTED;
                               }
                               if (!store_callback_record(mysql_rec, prebuilt,
                                                          reader_ctx, nullptr)) {
                                 return DB_ERROR;
                               }
                               if (row_sink->send_row(prebuilt->m_mysql_table)) {
                                 return DB_INTERRUPTED;
                               }
                               return DB_SUCCESS;
                             });
  if (err != DB_SUCCESS) {
    return err;
  }

  err = reader.run(0);
  if (stop_requested && row_sink->stop_is_success() &&
      err == DB_INTERRUPTED) {
    return DB_SUCCESS;
  }
  return err;
}

dberr_t InnoDB_pq_scan_ctx::partition(size_t split_level) {
  return partition(split_level, nullptr, nullptr);
}

dberr_t InnoDB_pq_scan_ctx::partition(size_t split_level,
                                      const dtuple_t *start,
                                      const dtuple_t *end) {
  m_ranges.clear();

  if (m_index == nullptr || m_trx == nullptr) {
    return DB_UNSUPPORTED;
  }

  if (m_trx->read_view != nullptr && !MVCC::is_view_active(m_trx->read_view)) {
    return DB_UNSUPPORTED;
  }

  Parallel_reader reader(0);
  Parallel_reader::Scan_range scan_range(start, end);
  Parallel_reader::Config config(scan_range, m_index);
  Parallel_reader::Exported_ranges exported_ranges{};
  auto err = reader.export_scan_ranges(const_cast<trx_t *>(m_trx), config,
                                       &exported_ranges, split_level);
  if (err != DB_SUCCESS) {
    return err;
  }

  m_ranges.reserve(exported_ranges.size());
  for (const auto &exported_range : exported_ranges) {
    InnoDB_pq_range range;
    range.m_id = exported_range.m_id;
    range.m_split = exported_range.m_split;

    if (exported_range.m_start != nullptr) {
      range.m_start = std::make_shared<InnoDB_pq_iter>();
      if (range.m_start == nullptr) {
        return DB_OUT_OF_MEMORY;
      }
      err = range.m_start->assign(exported_range.m_start, m_index);
      if (err != DB_SUCCESS) {
        return err;
      }
    }

    if (exported_range.m_end != nullptr) {
      range.m_end = std::make_shared<InnoDB_pq_iter>();
      if (range.m_end == nullptr) {
        return DB_OUT_OF_MEMORY;
      }
      err = range.m_end->assign(exported_range.m_end, m_index);
      if (err != DB_SUCCESS) {
        return err;
      }
    }

    m_ranges.push_back(std::move(range));
  }

  return DB_SUCCESS;
}

dberr_t InnoDB_pq_scan_ctx::validate_secondary_visibility_contract() const {
  if (m_index == nullptr || m_index->is_clustered() || m_trx == nullptr ||
      m_trx->read_view == nullptr ||
      !MVCC::is_view_active(m_trx->read_view)) {
    return DB_UNSUPPORTED;
  }

  /*
    Parallel_reader::check_visibility() still asserts for secondary indexes
    with an active read view. Keep M9-B3a row production fail-closed until a
    secondary MVCC/delete-mark helper is explicitly migrated.
  */
  return DB_UNSUPPORTED;
}

dberr_t InnoDB_pq_scan_ctx::validate_secondary_visibility_fast_path(
    const row_prebuilt_t *prebuilt, const rec_t *rec,
    const ulint *offsets) const {
  if (m_index == nullptr || m_index->is_clustered() || m_trx == nullptr ||
      m_trx->read_view == nullptr ||
      !MVCC::is_view_active(m_trx->read_view) || prebuilt == nullptr ||
      rec == nullptr || offsets == nullptr || prebuilt->idx_cond ||
      prebuilt->need_to_access_clustered || prebuilt->trx != m_trx) {
    return DB_UNSUPPORTED;
  }

  if (prebuilt->index != m_index || prebuilt->select_lock_type != LOCK_NONE) {
    return DB_UNSUPPORTED;
  }

  const trx_id_t max_trx_id = page_get_max_trx_id(page_align(rec));
  if (max_trx_id == 0 || !m_trx->read_view->sees(max_trx_id)) {
    return DB_UNSUPPORTED;
  }

  if (rec_get_deleted_flag(rec, m_is_compact)) {
    return DB_UNSUPPORTED;
  }

  return DB_SUCCESS;
}

dberr_t InnoDB_pq_scan_ctx::validate_secondary_visibility_with_cluster_lookup(
    row_prebuilt_t *prebuilt, const rec_t *rec, ulint *offsets,
    mem_heap_t **heap, mtr_t *mtr) const {
  if (m_index == nullptr || m_index->is_clustered() || m_trx == nullptr ||
      m_trx->read_view == nullptr ||
      !MVCC::is_view_active(m_trx->read_view) || prebuilt == nullptr ||
      rec == nullptr || offsets == nullptr || heap == nullptr ||
      mtr == nullptr || !mtr->is_active() || prebuilt->idx_cond ||
      prebuilt->select_lock_type != LOCK_NONE || prebuilt->index != m_index ||
      prebuilt->trx != m_trx || prebuilt->table == nullptr ||
      prebuilt->table->is_intrinsic() || prebuilt->clust_pcur == nullptr) {
    return DB_UNSUPPORTED;
  }

  const dberr_t fast_path_err =
      validate_secondary_visibility_fast_path(prebuilt, rec, offsets);
  if (fast_path_err == DB_SUCCESS) {
    return DB_SUCCESS;
  }

  if (prebuilt->sel_graph == nullptr) {
    row_prebuild_sel_graph(prebuilt);
  }

  if (prebuilt->sel_graph == nullptr) {
    return DB_UNSUPPORTED;
  }

  que_thr_t *thr = que_fork_get_first_thr(prebuilt->sel_graph);
  if (thr == nullptr) {
    return DB_UNSUPPORTED;
  }

  const rec_t *clust_rec = nullptr;
  ulint *clust_offsets = offsets;
  const dtuple_t *vrow = nullptr;
  const dberr_t err = pq_row_sel_get_clust_rec_for_mysql(
      prebuilt, m_index, rec, thr, &clust_rec, &clust_offsets, heap, &vrow,
      mtr);

  if (err != DB_SUCCESS) {
    return DB_UNSUPPORTED;
  }

  if (clust_rec == nullptr || rec_get_deleted_flag(clust_rec, m_is_compact)) {
    return DB_NOT_FOUND;
  }

  return DB_SUCCESS;
}

dberr_t InnoDB_pq_scan_ctx::validate_one_secondary_record_for_smoke(
    row_prebuilt_t *prebuilt, const dtuple_t *start,
    const dtuple_t *end) const {
  if (m_index == nullptr || m_index->is_clustered() || m_trx == nullptr ||
      m_trx->read_view == nullptr ||
      !MVCC::is_view_active(m_trx->read_view) || prebuilt == nullptr ||
      prebuilt->idx_cond || prebuilt->select_lock_type != LOCK_NONE ||
      prebuilt->index != m_index || prebuilt->trx != m_trx ||
      prebuilt->table == nullptr || prebuilt->table->is_intrinsic() ||
      prebuilt->pcur == nullptr || prebuilt->clust_pcur == nullptr) {
    return DB_UNSUPPORTED;
  }

  mem_heap_t *heap = mem_heap_create(256, UT_LOCATION_HERE);
  if (heap == nullptr) {
    return DB_OUT_OF_MEMORY;
  }

  mtr_t mtr;
  mtr.start();

  dberr_t result = DB_SUCCESS;
  prebuilt->pcur->init();
  if (start != nullptr) {
    prebuilt->pcur->open_on_user_rec(m_index, start, PAGE_CUR_GE,
                                     BTR_SEARCH_LEAF, &mtr, UT_LOCATION_HERE);
  } else {
    prebuilt->pcur->begin_leaf(m_index, BTR_SEARCH_LEAF, &mtr);
    result = prebuilt->pcur->move_to_next_user_rec(&mtr);
  }

  if (result == DB_SUCCESS && !prebuilt->pcur->is_on_user_rec()) {
    result = DB_SUCCESS;
    goto cleanup;
  }

  if (result == DB_END_OF_INDEX) {
    result = DB_SUCCESS;
    goto cleanup;
  }

  if (result != DB_SUCCESS) {
    result = DB_UNSUPPORTED;
    goto cleanup;
  }

  {
    const rec_t *rec = prebuilt->pcur->get_rec();
    ulint *offsets = rec_get_offsets(rec, m_index, nullptr, ULINT_UNDEFINED,
                                     UT_LOCATION_HERE, &heap);

    if (end != nullptr && end->compare(rec, m_index, offsets) <= 0) {
      result = DB_SUCCESS;
      goto cleanup;
    }

    result = validate_secondary_visibility_fast_path(prebuilt, rec, offsets);
    if (result == DB_UNSUPPORTED) {
      result = validate_secondary_visibility_with_cluster_lookup(
          prebuilt, rec, offsets, &heap, &mtr);
    }

    if (result == DB_NOT_FOUND) {
      result = DB_SUCCESS;
    } else if (result != DB_SUCCESS) {
      result = DB_UNSUPPORTED;
    }
  }

cleanup:
  prebuilt->pcur->close();
  prebuilt->clust_pcur->close();
  mtr.commit();
  mem_heap_free(heap);
  return result;
}

static bool pq_secondary_materialization_template_is_safe(
    const row_prebuilt_t *prebuilt, const dict_index_t *index) {
  if (prebuilt == nullptr || index == nullptr || prebuilt->mysql_template == nullptr ||
      prebuilt->n_template == 0 || prebuilt->templ_contains_blob) {
    return false;
  }

  for (ulint i = 0; i < prebuilt->n_template; ++i) {
    const mysql_row_templ_t *templ = &prebuilt->mysql_template[i];
    if (templ->is_virtual || templ->is_multi_val ||
        templ->rec_field_no == ULINT_UNDEFINED || templ->mysql_null_bit_mask != 0 ||
        templ->mysql_type == DATA_MYSQL_TRUE_VARCHAR ||
        templ->mbminlen != templ->mbmaxlen ||
        DATA_LARGE_MTYPE(templ->type) || DATA_GEOMETRY_MTYPE(templ->type)) {
      return false;
    }

    if (templ->rec_field_no >= dict_index_get_n_fields(index)) {
      return false;
    }

    const dict_field_t *field = index->get_field(templ->rec_field_no);
    if (field == nullptr || field->prefix_len != 0 || field->col == nullptr ||
        field->col->is_nullable()) {
      return false;
    }
  }

  return true;
}

dberr_t InnoDB_pq_scan_ctx::materialize_one_secondary_record_for_smoke(
    byte *mysql_rec, row_prebuilt_t *prebuilt, const dtuple_t *start,
    const dtuple_t *end) const {
  if (mysql_rec == nullptr || m_index == nullptr || m_index->is_clustered() ||
      dict_table_has_fts_index(m_index->table) || m_trx == nullptr ||
      m_trx->read_view == nullptr ||
      !MVCC::is_view_active(m_trx->read_view) || prebuilt == nullptr ||
      prebuilt->idx_cond || prebuilt->need_to_access_clustered ||
      !prebuilt->read_just_key || prebuilt->select_lock_type != LOCK_NONE ||
      prebuilt->index != m_index || prebuilt->trx != m_trx ||
      prebuilt->table == nullptr || prebuilt->table->is_intrinsic() ||
      prebuilt->pcur == nullptr || prebuilt->clust_pcur == nullptr ||
      !pq_secondary_materialization_template_is_safe(prebuilt, m_index)) {
    return DB_UNSUPPORTED;
  }

  mem_heap_t *heap = mem_heap_create(256, UT_LOCATION_HERE);
  if (heap == nullptr) {
    return DB_OUT_OF_MEMORY;
  }

  mtr_t mtr;
  mtr.start();

  dberr_t result = DB_SUCCESS;
  prebuilt->pcur->init();
  if (start != nullptr) {
    prebuilt->pcur->open_on_user_rec(m_index, start, PAGE_CUR_GE,
                                     BTR_SEARCH_LEAF, &mtr, UT_LOCATION_HERE);
  } else {
    prebuilt->pcur->begin_leaf(m_index, BTR_SEARCH_LEAF, &mtr);
    result = prebuilt->pcur->move_to_next_user_rec(&mtr);
  }

  if (result == DB_SUCCESS && !prebuilt->pcur->is_on_user_rec()) {
    result = DB_SUCCESS;
    goto cleanup;
  }

  if (result == DB_END_OF_INDEX) {
    result = DB_SUCCESS;
    goto cleanup;
  }

  if (result != DB_SUCCESS) {
    result = DB_UNSUPPORTED;
    goto cleanup;
  }

  {
    const rec_t *rec = prebuilt->pcur->get_rec();
    ulint *offsets = rec_get_offsets(rec, m_index, nullptr, ULINT_UNDEFINED,
                                     UT_LOCATION_HERE, &heap);

    if (end != nullptr && end->compare(rec, m_index, offsets) <= 0) {
      result = DB_SUCCESS;
      goto cleanup;
    }

    result = validate_secondary_visibility_fast_path(prebuilt, rec, offsets);
    if (result == DB_UNSUPPORTED) {
      result = validate_secondary_visibility_with_cluster_lookup(
          prebuilt, rec, offsets, &heap, &mtr);
    }

    if (result == DB_NOT_FOUND) {
      result = DB_SUCCESS;
      goto cleanup;
    }

    if (result != DB_SUCCESS) {
      result = DB_UNSUPPORTED;
      goto cleanup;
    }

    if (!row_sel_store_mysql_rec(mysql_rec, prebuilt, rec, nullptr, false,
                                 m_index, m_index, offsets, false, nullptr,
                                 prebuilt->blob_heap)) {
      result = DB_UNSUPPORTED;
      goto cleanup;
    }
  }

cleanup:
  prebuilt->pcur->close();
  prebuilt->clust_pcur->close();
  mtr.commit();
  mem_heap_free(heap);
  return result;
}

dberr_t InnoDB_pq_scan_ctx::materialize_secondary_range_for_smoke(
    byte *mysql_rec, row_prebuilt_t *prebuilt, const dtuple_t *start,
    const dtuple_t *end, uint max_rows, uint *row_count) const {
  if (row_count != nullptr) {
    *row_count = 0;
  }

  if (mysql_rec == nullptr || row_count == nullptr || max_rows == 0 ||
      m_index == nullptr || m_index->is_clustered() ||
      dict_table_has_fts_index(m_index->table) || m_trx == nullptr ||
      m_trx->read_view == nullptr ||
      !MVCC::is_view_active(m_trx->read_view) || prebuilt == nullptr ||
      prebuilt->idx_cond || prebuilt->need_to_access_clustered ||
      !prebuilt->read_just_key || prebuilt->select_lock_type != LOCK_NONE ||
      prebuilt->index != m_index || prebuilt->trx != m_trx ||
      prebuilt->table == nullptr || prebuilt->table->is_intrinsic() ||
      prebuilt->pcur == nullptr || prebuilt->clust_pcur == nullptr ||
      !pq_secondary_materialization_template_is_safe(prebuilt, m_index)) {
    return DB_UNSUPPORTED;
  }

  mem_heap_t *heap = mem_heap_create(256, UT_LOCATION_HERE);
  if (heap == nullptr) {
    return DB_OUT_OF_MEMORY;
  }

  mtr_t mtr;
  mtr.start();

  dberr_t result = DB_SUCCESS;
  uint materialized = 0;

  prebuilt->pcur->init();
  if (start != nullptr) {
    prebuilt->pcur->open_on_user_rec(m_index, start, PAGE_CUR_GE,
                                     BTR_SEARCH_LEAF, &mtr, UT_LOCATION_HERE);
  } else {
    prebuilt->pcur->begin_leaf(m_index, BTR_SEARCH_LEAF, &mtr);
    result = prebuilt->pcur->move_to_next_user_rec(&mtr);
  }

  while (result == DB_SUCCESS && prebuilt->pcur->is_on_user_rec()) {
    const rec_t *rec = prebuilt->pcur->get_rec();
    ulint *offsets = rec_get_offsets(rec, m_index, nullptr, ULINT_UNDEFINED,
                                     UT_LOCATION_HERE, &heap);
    if (offsets == nullptr) {
      result = DB_OUT_OF_MEMORY;
      break;
    }

    if (end != nullptr && end->compare(rec, m_index, offsets) <= 0) {
      result = DB_SUCCESS;
      break;
    }

    if (materialized >= max_rows) {
      result = DB_UNSUPPORTED;
      break;
    }

    result = validate_secondary_visibility_fast_path(prebuilt, rec, offsets);
    if (result != DB_SUCCESS) {
      result = DB_UNSUPPORTED;
      break;
    }

    if (!row_sel_store_mysql_rec(mysql_rec, prebuilt, rec, nullptr, false,
                                 m_index, m_index, offsets, false, nullptr,
                                 prebuilt->blob_heap)) {
      result = DB_UNSUPPORTED;
      break;
    }

    ++materialized;
    mem_heap_empty(heap);
    result = prebuilt->pcur->move_to_next_user_rec(&mtr);
  }

  if (result == DB_END_OF_INDEX || result == DB_SUCCESS) {
    *row_count = materialized;
    result = DB_SUCCESS;
  } else {
    *row_count = 0;
  }

  prebuilt->pcur->close();
  prebuilt->clust_pcur->close();
  mtr.commit();
  mem_heap_free(heap);
  return result;
}

dberr_t InnoDB_pq_scan_ctx::materialize_secondary_ref_for_smoke(
    byte *mysql_rec, row_prebuilt_t *prebuilt, const dtuple_t *ref_key,
    uint max_rows, uint *row_count) const {
  if (row_count != nullptr) {
    *row_count = 0;
  }

  if (mysql_rec == nullptr || row_count == nullptr || ref_key == nullptr ||
      max_rows == 0 || m_index == nullptr || m_index->is_clustered() ||
      dict_table_has_fts_index(m_index->table) || m_trx == nullptr ||
      m_trx->read_view == nullptr ||
      !MVCC::is_view_active(m_trx->read_view) || prebuilt == nullptr ||
      prebuilt->idx_cond || prebuilt->need_to_access_clustered ||
      !prebuilt->read_just_key || prebuilt->select_lock_type != LOCK_NONE ||
      prebuilt->index != m_index || prebuilt->trx != m_trx ||
      prebuilt->table == nullptr || prebuilt->table->is_intrinsic() ||
      prebuilt->pcur == nullptr || prebuilt->clust_pcur == nullptr ||
      !pq_secondary_materialization_template_is_safe(prebuilt, m_index)) {
    return DB_UNSUPPORTED;
  }

  mem_heap_t *heap = mem_heap_create(256, UT_LOCATION_HERE);
  if (heap == nullptr) {
    return DB_OUT_OF_MEMORY;
  }

  mtr_t mtr;
  mtr.start();

  dberr_t result = DB_SUCCESS;
  uint materialized = 0;

  prebuilt->pcur->init();
  prebuilt->pcur->open_on_user_rec(m_index, ref_key, PAGE_CUR_GE,
                                   BTR_SEARCH_LEAF, &mtr, UT_LOCATION_HERE);

  while (result == DB_SUCCESS && prebuilt->pcur->is_on_user_rec()) {
    const rec_t *rec = prebuilt->pcur->get_rec();
    ulint *offsets = rec_get_offsets(rec, m_index, nullptr, ULINT_UNDEFINED,
                                     UT_LOCATION_HERE, &heap);
    if (offsets == nullptr) {
      result = DB_OUT_OF_MEMORY;
      break;
    }

    if (ref_key->compare(rec, m_index, offsets) != 0) {
      result = DB_SUCCESS;
      break;
    }

    if (materialized >= max_rows) {
      result = DB_UNSUPPORTED;
      break;
    }

    result = validate_secondary_visibility_fast_path(prebuilt, rec, offsets);
    if (result != DB_SUCCESS) {
      result = DB_UNSUPPORTED;
      break;
    }

    if (!row_sel_store_mysql_rec(mysql_rec, prebuilt, rec, nullptr, false,
                                 m_index, m_index, offsets, false, nullptr,
                                 prebuilt->blob_heap)) {
      result = DB_UNSUPPORTED;
      break;
    }

    ++materialized;
    mem_heap_empty(heap);
    result = prebuilt->pcur->move_to_next_user_rec(&mtr);
  }

  if (result == DB_END_OF_INDEX || result == DB_SUCCESS) {
    *row_count = materialized;
    result = DB_SUCCESS;
  } else {
    *row_count = 0;
  }

  prebuilt->pcur->close();
  prebuilt->clust_pcur->close();
  mtr.commit();
  mem_heap_free(heap);
  return result;
}

dberr_t InnoDB_pq_scan_ctx::produce_secondary_range_for_user_gate(
    byte *mysql_rec, row_prebuilt_t *prebuilt, const dtuple_t *start,
    const dtuple_t *end, uint max_rows, PQ_row_sink *row_sink,
    uint *row_count) const {
  if (row_count != nullptr) {
    *row_count = 0;
  }

  if (mysql_rec == nullptr || row_sink == nullptr || row_count == nullptr ||
      max_rows == 0 || m_index == nullptr || m_index->is_clustered() ||
      dict_table_has_fts_index(m_index->table) || m_trx == nullptr ||
      m_trx->read_view == nullptr ||
      !MVCC::is_view_active(m_trx->read_view) || prebuilt == nullptr ||
      prebuilt->idx_cond || prebuilt->need_to_access_clustered ||
      !prebuilt->read_just_key || prebuilt->select_lock_type != LOCK_NONE ||
      prebuilt->index != m_index || prebuilt->trx != m_trx ||
      prebuilt->table == nullptr || prebuilt->table->is_intrinsic() ||
      prebuilt->pcur == nullptr || prebuilt->clust_pcur == nullptr ||
      !pq_secondary_materialization_template_is_safe(prebuilt, m_index)) {
    return DB_UNSUPPORTED;
  }

  mem_heap_t *heap = mem_heap_create(256, UT_LOCATION_HERE);
  if (heap == nullptr) {
    return DB_OUT_OF_MEMORY;
  }

  mtr_t mtr;
  mtr.start();

  dberr_t result = DB_SUCCESS;
  uint produced = 0;

  prebuilt->pcur->init();
  if (start != nullptr) {
    prebuilt->pcur->open_on_user_rec(m_index, start, PAGE_CUR_GE,
                                     BTR_SEARCH_LEAF, &mtr, UT_LOCATION_HERE);
  } else {
    prebuilt->pcur->begin_leaf(m_index, BTR_SEARCH_LEAF, &mtr);
    result = prebuilt->pcur->move_to_next_user_rec(&mtr);
  }

  while (result == DB_SUCCESS && prebuilt->pcur->is_on_user_rec()) {
    if (row_sink->should_abort()) {
      result = row_sink->stop_is_success() ? DB_SUCCESS : DB_INTERRUPTED;
      break;
    }

    const rec_t *rec = prebuilt->pcur->get_rec();
    ulint *offsets = rec_get_offsets(rec, m_index, nullptr, ULINT_UNDEFINED,
                                     UT_LOCATION_HERE, &heap);
    if (offsets == nullptr) {
      result = DB_OUT_OF_MEMORY;
      break;
    }

    if (end != nullptr && end->compare(rec, m_index, offsets) <= 0) {
      result = DB_SUCCESS;
      break;
    }

    if (produced >= max_rows) {
      result = DB_UNSUPPORTED;
      break;
    }

    result = validate_secondary_visibility_fast_path(prebuilt, rec, offsets);
    if (result != DB_SUCCESS) {
      result = DB_UNSUPPORTED;
      break;
    }

    if (!row_sel_store_mysql_rec(mysql_rec, prebuilt, rec, nullptr, false,
                                 m_index, m_index, offsets, false, nullptr,
                                 prebuilt->blob_heap) ||
        row_sink->send_row(prebuilt->m_mysql_table)) {
      result = DB_UNSUPPORTED;
      break;
    }

    ++produced;
    mem_heap_empty(heap);
    result = prebuilt->pcur->move_to_next_user_rec(&mtr);
  }

  if (result == DB_END_OF_INDEX || result == DB_SUCCESS) {
    *row_count = produced;
    result = DB_SUCCESS;
  } else {
    *row_count = 0;
  }

  prebuilt->pcur->close();
  prebuilt->clust_pcur->close();
  mtr.commit();
  mem_heap_free(heap);
  return result;
}

bool InnoDB_pq_scan_ctx::check_visibility(const rec_t *&rec,
                                           ulint *&offsets,
                                           mem_heap_t *&heap,
                                           mtr_t *mtr) {
  return Parallel_reader::check_visibility(m_index, m_is_compact, m_trx, rec,
                                           offsets, heap, mtr);
}

/* ============================================================ */
/* InnoDB_pq_ctx                                                */
/* ============================================================ */

InnoDB_pq_ctx::InnoDB_pq_ctx(size_t id, InnoDB_pq_scan_ctx *scan_ctx,
                              InnoDB_pq_range *range)
    : m_id(id), m_scan_ctx(scan_ctx), m_range(range) {}

InnoDB_pq_ctx::~InnoDB_pq_ctx() = default;

void InnoDB_pq_ctx::reset_for_new_range(InnoDB_pq_range *range) {
  m_range = range;
  m_start_of_range = true;
  m_range_exhausted = false;
}

int InnoDB_pq_ctx::read_record(byte *mysql_rec, row_prebuilt_t *prebuilt,
                                bool *eof) {
  ut_ad(m_scan_ctx != nullptr);
  ut_ad(prebuilt != nullptr);

  if (eof != nullptr) {
    *eof = false;
  }

  if (m_range_exhausted) {
    if (eof != nullptr) {
      *eof = true;
    }
    return 0;
  }

  /* V1-MVP: Pull-row via the standard InnoDB row search path.

  Instead of reimplementing cursor traversal with low-level InnoDB
  APIs (btr_pcur_t page cursors, mtr_t savepoints, etc.), we use
  the existing row_search_mvcc() function through the worker's
  own row_prebuilt_t. This gives us:

  1. Correct MVCC visibility through the worker prebuilt. The caller
     must prove the worker snapshot contract before enabling this path.
  2. Correct MySQL format conversion (row_sel_store_mysql_rec).
  3. Correct BLOB handling (prebuilt->blob_heap).
  4. Correct deleted record handling.
  5. No need for custom mtr/latch management.

  Before this path is connected to real execution, the worker's prebuilt
  must be initialized with:
  - prebuilt->index = clustered index
  - a statement snapshot equivalent to the leader snapshot
  - prebuilt->select_lock_type = LOCK_NONE (consistent read)
  - prebuilt->row_read_type = ROW_READ_WITH_LOCKS

  On the first call (m_start_of_range = true), we position the
  cursor at the start of the index (or at the start boundary of
  the assigned range). On subsequent calls, we advance the cursor
  forward (ROW_SEL_NEXT direction).

  For V1-MVP (single range = whole table), we start at the
  leftmost record and scan until end-of-index. */

  if (m_start_of_range) {
    /* Position the cursor at the start of the scan range.

    For V1-MVP (whole table scan), position at the leftmost record.
    This is equivalent to ha_innobase::index_first(), which calls
    index_read(nullptr, 0, HA_READ_AFTER_KEY). That maps to
    row_search_mvcc(..., PAGE_CUR_G, ..., 0, 0) with an empty
    search_tuple. */

    prebuilt->index = m_scan_ctx->index();

    /* Ensure the index is usable. */
    if (!prebuilt->index->is_usable(prebuilt->trx)) {
      return HA_ERR_UNSUPPORTED;
    }

    /* Start the scan: position at first record of the index.
    This mirrors ha_innobase::index_first() -> index_read(nullptr,
    HA_READ_AFTER_KEY). PAGE_CUR_UNSUPP is only valid after a cursor
    has already been positioned. */

    dtuple_set_n_fields(prebuilt->search_tuple, 0);
    auto err = row_search_mvcc(mysql_rec, PAGE_CUR_G, prebuilt, 0, 0);

    if (err == DB_SUCCESS) {
      m_start_of_range = false;
      return 0;
    }

    if (err == DB_RECORD_NOT_FOUND || err == DB_END_OF_INDEX) {
      /* Empty table or end of index reached immediately. */
      m_range_exhausted = true;
      if (eof != nullptr) {
        *eof = true;
      }
      return 0;
    }

    /* Fatal error. */
    m_range_exhausted = true;
    return pq_map_innodb_error(err);
  }

  /* Advance the cursor to the next record.
  This mirrors ha_innobase::general_fetch(buf, ROW_SEL_NEXT, 0)
  -> row_search_mvcc(buf, PAGE_CUR_UNSUPP, prebuilt, 0, ROW_SEL_NEXT). */

  auto err = row_search_mvcc(mysql_rec, PAGE_CUR_UNSUPP, prebuilt, 0,
                              ROW_SEL_NEXT);

  if (err == DB_SUCCESS) {
    return 0;
  }

  if (err == DB_RECORD_NOT_FOUND || err == DB_END_OF_INDEX) {
    /* End of index: range exhausted. */
    m_range_exhausted = true;
    if (eof != nullptr) {
      *eof = true;
    }
    return 0;
  }

  /* Fatal error. */
  m_range_exhausted = true;
  return pq_map_innodb_error(err);
}

/* ============================================================ */
/* InnoDB_pq_leader_ctx                                         */
/* ============================================================ */

InnoDB_pq_leader_ctx::InnoDB_pq_leader_ctx(size_t max_threads, bool reverse)
    : m_max_threads(max_threads), m_reverse(reverse) {}

InnoDB_pq_leader_ctx::~InnoDB_pq_leader_ctx() {
  if (m_scan_ctx != nullptr) {
    ut::delete_(m_scan_ctx);
    m_scan_ctx = nullptr;
  }
}

dberr_t InnoDB_pq_leader_ctx::init(dict_index_t *index, trx_t *trx,
                                    bool is_compact,
                                    const page_size_t &page_size) {
  m_trx = trx;

  m_scan_ctx = ut::new_withkey<InnoDB_pq_scan_ctx>(
      UT_NEW_THIS_FILE_PSI_KEY, index, trx, is_compact, page_size);

  if (m_scan_ctx == nullptr) {
    return DB_OUT_OF_MEMORY;
  }

  /* Partition the B+tree. V1-MVP: creates a single range
  covering the entire clustered index. */
  auto err = m_scan_ctx->partition(0);

  if (err != DB_SUCCESS) {
    ut::delete_(m_scan_ctx);
    m_scan_ctx = nullptr;
    return err;
  }

  return DB_SUCCESS;
}

InnoDB_pq_range *InnoDB_pq_leader_ctx::dispatch_next_range() {
  if (m_scan_ctx == nullptr) {
    return nullptr;
  }

  auto &ranges = m_scan_ctx->mutable_ranges();

  const auto range_id = m_next_range_id.fetch_add(1, std::memory_order_relaxed);
  if (range_id >= ranges.size()) {
    return nullptr;
  }

  auto &range = ranges[range_id];
  return &range;
}

/* ============================================================ */
/* InnoDB_pq_worker_ctx                                         */
/* ============================================================ */

InnoDB_pq_worker_ctx::InnoDB_pq_worker_ctx(size_t worker_id,
                                            InnoDB_pq_leader_ctx *leader_ctx)
    : m_worker_id(worker_id), m_leader_ctx(leader_ctx) {}

InnoDB_pq_worker_ctx::~InnoDB_pq_worker_ctx() {
  if (m_cursor_ctx != nullptr) {
    ut::delete_(m_cursor_ctx);
    m_cursor_ctx = nullptr;
  }
}

void InnoDB_pq_worker_ctx::init(InnoDB_pq_range *range) {
  if (m_cursor_ctx != nullptr) {
    ut::delete_(m_cursor_ctx);
    m_cursor_ctx = nullptr;
  }

  m_assigned_range = range;

  /* Create the pull-row cursor context. */
  m_cursor_ctx = ut::new_withkey<InnoDB_pq_ctx>(
      UT_NEW_THIS_FILE_PSI_KEY, m_worker_id,
      m_leader_ctx->scan_ctx(), range);
}

int InnoDB_pq_worker_ctx::read_record(byte *mysql_rec,
                                       row_prebuilt_t *prebuilt,
                                       bool *eof) {
  if (m_cursor_ctx == nullptr) {
    if (eof != nullptr) {
      *eof = true;
    }
    return 0;
  }

  auto err_code = m_cursor_ctx->read_record(mysql_rec, prebuilt, eof);

  /* Propagate error to worker context. */
  if (err_code != 0 && (eof == nullptr || !*eof)) {
    m_err.store(DB_ERROR, std::memory_order_relaxed);
  }

  return err_code;
}
