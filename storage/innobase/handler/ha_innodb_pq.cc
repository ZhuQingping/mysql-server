/* Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

/** @file handler/ha_innodb_pq.cc
 Parallel query related interface implementation of InnoDB
 *******************************************************/

/* Include necessary SQL headers */
#include <assert.h>
#include <current_thd.h>
#include <debug_sync.h>
#include <key_spec.h>
#include <log.h>
#include <my_bit.h>
#include <mysql/plugin.h>
#include <sql_class.h>
#include <sql_lex.h>
#include <sql_table.h>
#include <sql_thd_internal_api.h>
#include <sys/types.h>
#include "ha_prototypes.h"

#include "dd/cache/dictionary_client.h"
#include "dd/dd.h"
#include "dd/dictionary.h"
#include "dd/impl/properties_impl.h"
#include "dd/properties.h"
#include "dd/types/column.h"
#include "dd/types/index.h"
#include "dd/types/index_element.h"
#include "dd/types/partition.h"
#include "dd/types/partition_index.h"
#include "dd/types/table.h"
#include "dd/types/tablespace_file.h"
#include "dd_table_share.h"

#include "btr0sea.h"
#include "dict0crea.h"
#include "dict0dd.h"
#include "dict0dict.h"
#include "dict0priv.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "fsp0sysspace.h"
#include "fts0plugin.h"
#include "fts0priv.h"
#include "ha_innodb.h"
#include "ha_innopart.h"
#include "ha_prototypes.h"
#include "handler0alter.h"
#include "lex_string.h"
#include "log0log.h"

#include "my_dbug.h"
#include "my_io.h"

#include "clone0api.h"
#include "dict0dd.h"
#include "fts0plugin.h"
#include "fts0priv.h"
#include "lock0lock.h"
#include "pars0pars.h"
#include "partition_info.h"
#include "rem0types.h"
#include "row0ins.h"
#include "row0log.h"
#include "row0pread_pq.h"
#include "row0sel.h"
#include "sql/create_field.h"
#include "srv0mon.h"
#include "srv0start.h"
#include "trx0roll.h"
#include "trx0trx.h"
#include "ut0new.h"
#include "ut0stage.h"

void ha_innobase::init_shared_info() {
  m_shared_info.m_type = PQ_temp_table_type::TEMP_INNODB;
  m_shared_info.m_table = m_prebuilt->table;
  m_shared_info.m_share = m_share;
}

void ha_innobase::set_shared_info(PQ_shared_info *info) {
  assert(info->m_type == PQ_temp_table_type::TEMP_INNODB && info->m_share &&
         info->m_table);
  assert(m_old_share == nullptr ||
         m_prebuilt->table == static_cast<dict_table_t *>(info->m_table));
  if (m_old_share == nullptr) {
    m_old_share = m_share;
    m_share = static_cast<INNOBASE_SHARE *>(info->m_share);
  }
  if (m_old_table == nullptr) {
    m_old_table = m_prebuilt->table;
    m_prebuilt->table = static_cast<dict_table_t *>(info->m_table);
  }
}

bool ha_innobase::is_sharing_data() { return m_old_share && m_old_table; }

void ha_innobase::reset_shared_info() {
  if (m_old_share) {
    m_share = m_old_share;
    m_old_share = nullptr;
  }

  if (m_old_table) {
    m_prebuilt->table = m_old_table;
    m_old_table = nullptr;
  }

  if (m_prebuilt && m_prebuilt->old_index) {
    m_prebuilt->index = m_prebuilt->old_index;
    m_prebuilt->old_index = nullptr;
  }
}

bool pq_clone_innodb_snapshot(THD *worker_thd, THD *leader_thd) {
  trx_t *orig = thd_to_trx(leader_thd);
  ut_ad(orig);

  if (pq_create_trx(worker_thd)) {
    return true;
  }

  auto trx = thd_to_trx(worker_thd);
  /* Clone the read view. Note, even if current table is intrinsic, it's
  unknown that if this session will also access a normal table or not, which
  requires a consistent read view, so here it has to always clone the read
  view to guarantee the correctness. */
  auto snapshot = orig->read_view;
  if (srv_read_only_mode) {
    ut_ad(snapshot == nullptr);
    trx->read_view = nullptr;
    return false;
  } else if (!MVCC::is_view_active(trx->read_view)) {
    ut_ad(MVCC::is_view_active(snapshot));
    trx_clone_read_view(trx, snapshot);
    if (!MVCC::is_view_active(trx->read_view)) {
      return true;
    }
  }
  return false;
}

bool pq_create_innodb_snapshot(THD *thd) {
  ut_ad(thd->is_pq_leader());
  if (pq_create_trx(thd)) {
    return true;
  }

  auto trx = thd_to_trx(thd);
  ut_ad(trx);
  if (srv_read_only_mode) {
    trx->read_view = nullptr;
    return false;
  } else if (!MVCC::is_view_active(trx->read_view)) {
    trx_assign_read_view(trx);
    if (!MVCC::is_view_active(trx->read_view)) return true;
  }
  return false;
}

int ha_innobase::pq_worker_scan_init(uint keyno, void *scan_ctx) {
  auto pq_leader = static_cast<Parallel_leader *>(scan_ctx);
  /* Prepare for scanning. */
  active_index = keyno;
  int result = change_active_index(active_index);
  if (result) return result;

  // register trx of prebuilt for XA transaction
  auto trx = m_prebuilt->trx;
  innobase_register_trx(ht, ha_thd(), trx);
  trx_start_if_not_started_xa(trx, false, UT_LOCATION_HERE);

  m_prebuilt->pq_worker = std::shared_ptr<Parallel_worker>(
      ut::new_withkey<Parallel_worker>(
          UT_NEW_THIS_FILE_PSI_KEY, pq_leader->is_reverse(),
          pq_leader->pq_slices_map, pq_leader->pq_key_map),
      [](Parallel_worker *worker) { ut::delete_(worker); });

  update_thd();
  ut_ad(keyno == pq_leader->key);
  m_prebuilt->is_attach_ctx = false;
  inited = PQ;

  mrr_have_range = false;
  return 0;
}

int ha_innobase::pq_index_scan_init(uint keyno, void *&scan_ctx,
                                    uint n_threads) {
  // table or index scan
  btr_pcur_t *pcur{nullptr};
  scan_ctx = nullptr;
  update_thd();
  dtuple_t *range_start{nullptr};
  dtuple_t *range_end{nullptr};
  auto pq_leader = ut::new_withkey<Parallel_leader>(
      UT_NEW_THIS_FILE_PSI_KEY, n_threads, ha_reverse_scan(), m_prebuilt->trx);
  if (pq_leader == nullptr) {
    return (HA_ERR_OUT_OF_MEM);
  }
  auto trx = pq_leader->m_trx;
  pq_leader->key = keyno;
  int ret;
  if (pq_leader->is_reverse()) {
    ret = ha_innobase::index_last(table->record[0]);
    if (!ret) {
      /*
        We save the last position. While in the "else" branch, we will not
        save the first position - it's not symmetric.
        The explanation follows. PQ_Scan_ctx::create_ranges() always moves
        into the index in forward order. It builds a range of the form (value1,
        <unspecified>), then moves forward, builds range (value2,<unspecified>)
        and updates the previous range's <unspecified> to value2.
        So, in both forward and backward scan, the final result of
        create_ranges() could be (2,5),(5,<unspecified>).
        Then when these ranges are processed, the code requires an explicit
        starting point:
        - in forward scan, we start at 2 and read until 5, or start at 5 and
        read until EOF.
        - in backward scan, we cannot start at <unspecified> (and read until
        5). That is why here, we save the value of the last entry in "pcur",
        and actually create_ranges() will pick it up and replace <unspecified>
        with it, producing ranges (2,5),(5,7). So backward scan will start at
        7.
      */
      pcur = m_prebuilt->pcur;
    }
  } else {
    ret = ha_innobase::index_first(table->record[0]);
  }

  DEBUG_SYNC(m_user_thd, "pq_init_01");

  PQ_Borders range_scan{range_start, range_end};
  PQ_Config config(range_scan, m_prebuilt->index);
  config.m_range_errno = ret;
  config.m_pcur = pcur;
  config.m_pq_reverse_scan = pq_leader->is_reverse();

  auto success = pq_leader->build_ranges(trx, config);
  auto slices = pq_leader->pq_slices_map[PQ_ref_key()];
  slices->mark_split();
  if (!success) {
    ut::delete_(pq_leader);
    return (HA_ERR_GENERIC);
  }

  scan_ctx = pq_leader;
  build_template(false);

  return (0);
}

int ha_innobase::pq_range_scan_init(uint keyno, void *&scan_ctx,
                                    uint n_threads) {
  scan_ctx = nullptr;
  update_thd();
  auto pq_leader = ut::new_withkey<Parallel_leader>(
      UT_NEW_THIS_FILE_PSI_KEY, n_threads, ha_reverse_scan(), m_prebuilt->trx);
  if (pq_leader == nullptr) {
    return (HA_ERR_OUT_OF_MEM);
  }

  pq_leader->key = keyno;
  uint range_res{0};
  while (!(range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range))) {
    dtuple_t *range_start{nullptr};
    dtuple_t *range_end{nullptr};
    uint range_errno{0};
    btr_pcur_t *pcur{nullptr};

    auto start_key =
        mrr_cur_range.start_key.keypart_map ? &mrr_cur_range.start_key : 0;
    auto end_key =
        mrr_cur_range.end_key.keypart_map ? &mrr_cur_range.end_key : 0;

    m_prebuilt->pq_heap = mem_heap_create(
        2 * (sizeof(btr_pcur_t) + (srv_page_size / 16)), UT_LOCATION_HERE);

    range_errno = 0;
    // set range boundary
    /* 1) seq scan. range_start is pos on the first rec that is fulfill the
       range condition range_end is pos on the next rec of the last rec fulfill
       the range contition 2) reverse scan. range_start is pos on the prev rec
       of the first rec fulfill the range conition range_end is pos on the last
       rec fulfill the range conition
    */
    if (start_key) {
      const uchar *key = start_key->key;
      auto keypart_map = start_key->keypart_map;
      uint key_len = calculate_key_len(table, keyno, keypart_map);

      auto start_flag = start_key->flag;
      if (!pq_leader->is_reverse()) {
        /*
          For seq scan, range_start is pos on the first rec that fulfill the
          range condition. So use HA_READ_AFTER_KEY or HA_READ_KEY_OR_NEXT.
          For example, we have (1, 2, 3) three records:
          1) a > 2, use HA_READ_AFTER_KEY, start_range should be 3.
          2) a >= 2, use HA_READ_KEY_OR_NEXT, start_range should be 2.
        */
        start_flag = (start_key->flag == HA_READ_AFTER_KEY)
                         ? HA_READ_AFTER_KEY
                         : HA_READ_KEY_OR_NEXT;

        m_prebuilt->pq_index_read = true;
        int err =
            ha_innobase::index_read(table->record[0], key, key_len, start_flag);
        m_prebuilt->pq_index_read = false;

        if (!err) {
          range_start = m_prebuilt->pq_tuple;
        } else {
          /*
            For seq scan, it uses HA_READ_AFTER_KEY or HA_READ_KEY_OR_NEXT. If
            the err is HA_ERR_KEY_NOT_FOUND, the start_key must be after all
            records, this range should be skipped, so set range_errno to err.
            Or the err is other error number, we should also skip this range.
            For example, we have(1, 2, 3) three records, if start_key is 4, the
            err is HA_ERR_KEY_NOT_FOUND, we should skip this range.
          */
          range_errno = err;
        }
      } else {
        /*
          For reverse scan, range_start is pos on the prev rec of the first rec
          fulfill the range condition. So use HA_READ_KEY_OR_PREV or
          HA_READ_BEFORE_KEY.
          For example, we have (1, 2, 3) three records:
          1) a > 2, use HA_READ_KEY_OR_PREV, start_range should be 2.
          2) a >= 2, use HA_READ_BEFORE_KEY, start_range should be 1.
        */
        start_flag = (start_key->flag == HA_READ_AFTER_KEY)
                         ? HA_READ_KEY_OR_PREV
                         : HA_READ_BEFORE_KEY;

        m_prebuilt->pq_index_read = true;
        int err =
            ha_innobase::index_read(table->record[0], key, key_len, start_flag);
        m_prebuilt->pq_index_read = false;

        if (!err) {
          range_start = m_prebuilt->pq_tuple;
        } else if (err == HA_ERR_KEY_NOT_FOUND) {
          /*
            For reverse scan, it uses HA_READ_KEY_OR_PREV or
            HA_READ_BEFORE_KEY. If the err is HA_ERR_KEY_NOT_FOUND, the
            start_key must be before all records, range_start must be -∞.
         */
          range_errno = 0;
        } else {
          range_errno = err;
        }
      }
    }

    if (end_key && !range_errno) {
      const uchar *key = end_key->key;
      auto keypart_map = end_key->keypart_map;
      uint key_len = calculate_key_len(table, keyno, keypart_map);

      auto end_flag = end_key->flag;
      if (!pq_leader->is_reverse()) {
        /*
          For seq scan, range_end is pos on the next rec of the last rec
          fulfill the range condition. So use HA_READ_KEY_OR_NEXT or
          HA_READ_AFTER_KEY.
          For example, we have (1, 2, 3) three records:
          1) a < 2, use HA_READ_KEY_OR_NEXT, range_end should be 2.
          2) a <= 2, use HA_READ_AFTER_KEY, range_end should be 3.
        */
        end_flag = (end_key->flag == HA_READ_BEFORE_KEY) ? HA_READ_KEY_OR_NEXT
                                                         : HA_READ_AFTER_KEY;

        m_prebuilt->pq_index_read = true;
        int err =
            ha_innobase::index_read(table->record[0], key, key_len, end_flag);
        m_prebuilt->pq_index_read = false;

        if (!err) {
          range_end = m_prebuilt->pq_tuple;
        } else if (err == HA_ERR_KEY_NOT_FOUND) {
          /*
            For seq scan, it uses HA_READ_KEY_OR_NEXT or HA_READ_AFTER_KEY.
            If the err is HA_ERR_KEY_NOT_FOUND, the end_key must be after all
            records, range_end must be +∞.
          */
          range_errno = 0;
        } else {
          range_errno = err;
        }
      } else {
        /*
          For reverse scan, range_end is pos on the last rec fulfill the range
          condition. So use HA_READ_BEFORE_KEY or HA_READ_KEY_OR_PREV.
          For example, we have (1, 2, 3) three records:
          1) a < 2, use HA_READ_BEFORE_KEY, range_end should be 1.
          2) a <= 2, use HA_READ_KEY_OR_PREV, range_end should be 2.
        */
        end_flag = (end_key->flag == HA_READ_BEFORE_KEY) ? HA_READ_BEFORE_KEY
                                                         : HA_READ_KEY_OR_PREV;

        m_prebuilt->pq_index_read = true;
        int err =
            ha_innobase::index_read(table->record[0], key, key_len, end_flag);
        m_prebuilt->pq_index_read = false;

        if (!err) {
          range_end = m_prebuilt->pq_tuple;
          pcur = m_prebuilt->pcur;
        } else {
          /*
            For reverse scan, it uses HA_READ_BEFORE_KEY or
            HA_READ_KEY_OR_PREV. If the err is HA_ERR_KEY_NOT_FOUND,
            the end_key must be before all records, this range should be
            skipped, so set range_errno to err. Or the err is other error
            number, we should also skip this range.
            For example, we have(1, 2, 3) three records, if end_key is 0, the
            err is HA_ERR_KEY_NOT_FOUND, we should skip this range.
          */
          range_errno = err;
        }
      }
    } else if (end_key == nullptr && !range_errno && pq_leader->is_reverse()) {
      /*
        Only save the last position for reverse scan when end_key is NULL.
        Because it scans from range_end backward to range_start, we cannot
        start at <unspecified>. That is why here, we save the value of the
        last entry in "pcur", and actually create_ranges() will pick it up
        and replace <unspecified> with it.
      */
      ha_innobase::index_last(table->record[0]);
      pcur = m_prebuilt->pcur;
    }

    PQ_Borders range_scan{range_start, range_end};
    PQ_Config config(range_scan, m_prebuilt->index);
    config.m_range_errno = range_errno;
    config.m_pcur = pcur;
    config.m_pq_reverse_scan = pq_leader->is_reverse();

    auto success = pq_leader->build_ranges(m_prebuilt->trx, config);

    mem_heap_free(m_prebuilt->pq_heap);
    if (!success) {
      ut::delete_(pq_leader);
      return (HA_ERR_GENERIC);
    }
  }

  auto slices = pq_leader->pq_slices_map[PQ_ref_key()];
  slices->mark_split();
  scan_ctx = pq_leader;
  build_template(false);

  return (0);
}

int ha_innobase::pq_ref_build_ranges(void *scan_ctx, Key_ref &key_ref) {
  dtuple_t *range_start{nullptr};
  dtuple_t *range_end{nullptr};
  dict_index_t *index = m_prebuilt->index;
  uint range_errno{0};
  btr_pcur_t *pcur{nullptr};
  bool success = false;
  auto pq_leader = static_cast<Parallel_leader *>(scan_ctx);
  auto trx = pq_leader->m_trx;

  ut_ad((int)(pq_leader->key) == key_ref.keyno);

  range_errno = 0;
  uint key_len = calculate_key_len(table, key_ref.keyno, key_ref.keypart_map);

  PQ_ref_key ref_key1;
  if (pq_ref_depend) {
    new (&ref_key1) PQ_ref_key(key_ref.key, key_len);
    pq_leader->pq_key_map.enter();
    // this ref key slices has been built before, that is a duplicated ref key
    if (pq_leader->pq_key_map.find(ref_key1)) {
      pq_leader->pq_key_map.exit();
      return success;
    }
  }

  m_prebuilt->pq_heap = mem_heap_create(
      2 * (sizeof(btr_pcur_t) + (srv_page_size / 16)), UT_LOCATION_HERE);

  // Do not check idx_cond, or it may lose some records. Let workers do it.
  // If idx_cond is a condition which references a column from outer table,
  // but this row read can't match this condition, it will report
  // ERR_KEY_NOT_FOUND, and mark it false in pq_key_map, but for the following
  // rows of the outer table they may have a matched record, which will be lost.
  m_prebuilt->pq_index_read = true;
  // populate search range boudary from ref record value
  int ret = ha_innobase::index_read(table->record[0], key_ref.key, key_len,
                                    HA_READ_KEY_EXACT);
  m_prebuilt->pq_index_read = false;

  if (ret) {
    if (pq_ref_depend) pq_leader->pq_key_map.insert(&ref_key1, false);
    // record errorno when can't find ref key. which will lead process finish
    // early
    range_errno = ret;
  } else {
    // record range boudary for searching
    auto start_flag =
        pq_leader->is_reverse() ? HA_READ_BEFORE_KEY : HA_READ_KEY_OR_NEXT;

    m_prebuilt->pq_index_read = true;
    int err = ha_innobase::index_read(table->record[0], key_ref.key, key_len,
                                      start_flag);
    m_prebuilt->pq_index_read = false;

    if (!err) {
      range_start = m_prebuilt->pq_tuple;
    } else if (pq_leader->is_reverse()) {
      range_errno = (err == HA_ERR_KEY_NOT_FOUND) ? 0 : err;
    } else {
      range_errno = err;
    }

    if (!range_errno) {
      auto end_flag =
          pq_leader->is_reverse() ? HA_READ_KEY_OR_PREV : HA_READ_AFTER_KEY;
      m_prebuilt->pq_index_read = true;
      err = ha_innobase::index_read(table->record[0], key_ref.key, key_len,
                                    end_flag);
      m_prebuilt->pq_index_read = false;
      if (!err) {
        range_end = m_prebuilt->pq_tuple;
        pcur = m_prebuilt->pcur;
      } else {
        if (err == HA_ERR_KEY_NOT_FOUND) {
          ha_innobase::index_last(table->record[0]);
          pcur = m_prebuilt->pcur;
          range_errno = 0;
        } else {
          range_errno = err;
        }
      }
    }

    PQ_Borders range_scan{range_start, range_end};
    PQ_Config config(range_scan, index);
    config.m_range_errno = range_errno;
    config.m_pcur = pcur;
    config.m_pq_reverse_scan = pq_leader->is_reverse();
    config.m_ref_key = (uchar *)malloc(key_len);
    memcpy(config.m_ref_key, key_ref.key, key_len);
    config.m_ref_key_len = key_len;
    config.m_ref_depend = pq_ref_depend;
    success = pq_leader->build_ranges(trx, config);
    auto slices = pq_leader->pq_slices_map[ref_key1];
    assert(slices != nullptr);
    slices->mark_split();
    if (pq_ref_depend) pq_leader->pq_key_map.insert(&ref_key1, true);
  }
  if (pq_ref_depend) pq_leader->pq_key_map.exit();
  mem_heap_free(m_prebuilt->pq_heap);

  return success;
}

int ha_innobase::pq_ref_scan_init(uint keyno, void *&scan_ctx, uint n_threads) {
  scan_ctx = nullptr;
  update_thd();
  auto pq_leader = ut::new_withkey<Parallel_leader>(
      UT_NEW_THIS_FILE_PSI_KEY, n_threads, ha_reverse_scan(), m_prebuilt->trx);
  if (pq_leader == nullptr) return (HA_ERR_OUT_OF_MEM);
  pq_leader->key = keyno;

  Key_ref key_ref{static_cast<int>(keyno), pq_ref_key.key,
                  pq_ref_key.keypart_map, pq_leader->is_reverse()};

  if (!pq_ref_depend) pq_ref_build_ranges(pq_leader, key_ref);

  scan_ctx = pq_leader;
  build_template(false);

  return (0);
}

int ha_innobase::pq_leader_scan_init(uint keyno, void *&scan_ctx,
                                     uint n_threads) {
  if (dict_table_is_discarded(m_prebuilt->table)) {
    ib_senderrf(ha_thd(), IB_LOG_LEVEL_ERROR, ER_TABLESPACE_DISCARDED,
                table->s->table_name.str);

    return (HA_ERR_NO_SUCH_TABLE);
  }
  active_index = keyno;
  int result = change_active_index(active_index);
  if (result) return result;
  m_prebuilt->index = innobase_get_index(keyno);
  auto trx = m_prebuilt->trx;
  innobase_register_trx(ht, ha_thd(), trx);
  trx_start_if_not_started_xa(trx, false, UT_LOCATION_HERE);
  trx_assign_read_view(trx);

  // equality reference
  if (pq_ref) return pq_ref_scan_init(keyno, scan_ctx, n_threads);
  // range scan
  else if (PQ_RANGE_SELECT == pq_range_type)
    return pq_range_scan_init(keyno, scan_ctx, n_threads);
  else
    return pq_index_scan_init(keyno, scan_ctx, n_threads);

  return (0);
}

int ha_innopart::pq_leader_scan_init(uint keyno, void *&scan_ctx,
                                     uint n_threads) {
  const auto first_used_partition = m_part_info->get_first_used_partition();
  active_index = keyno;
  int result = change_active_index(first_used_partition, active_index);
  if (result) return result;
  auto trx = m_prebuilt->trx;
  innobase_register_trx(ht, ha_thd(), trx);
  trx_start_if_not_started_xa(trx, false, UT_LOCATION_HERE);
  trx_assign_read_view(trx);

  set_partition(first_used_partition);
  if (dict_table_is_discarded(m_prebuilt->table)) {
    ib_senderrf(ha_thd(), IB_LOG_LEVEL_ERROR, ER_TABLESPACE_DISCARDED,
                m_prebuilt->table->name.m_name);
    return (HA_ERR_NO_SUCH_TABLE);
  }
  // equality reference
  if (pq_ref) return pq_ref_scan_init(keyno, scan_ctx, n_threads);
  // range scan
  else if (PQ_RANGE_SELECT == pq_range_type)
    return pq_range_scan_init(keyno, scan_ctx, n_threads);
  else
    return pq_index_scan_init(keyno, scan_ctx, n_threads);

  return (0);
}

int ha_innopart::pq_worker_scan_init(uint keyno, void *scan_ctx) {
  auto pq_leader = static_cast<Parallel_leader *>(scan_ctx);
  active_index = keyno;
  auto part_id = m_part_info->get_first_used_partition();
  int result = change_active_index(part_id, active_index);
  if (result) return result;

  // register trx of prebuilt for XA transaction
  auto trx = m_prebuilt->trx;
  innobase_register_trx(ht, ha_thd(), trx);
  trx_start_if_not_started_xa(trx, false, UT_LOCATION_HERE);

  inited = PQ;
  m_prebuilt->pq_worker = std::shared_ptr<Parallel_worker>(
      ut::new_withkey<Parallel_worker>(
          UT_NEW_THIS_FILE_PSI_KEY, pq_leader->is_reverse(),
          pq_leader->pq_slices_map, pq_leader->pq_key_map),
      [](Parallel_worker *worker) { ut::delete_(worker); });

  update_thd();
  m_prebuilt->is_attach_ctx = false;
  m_last_part = part_id;
  return 0;
}

static int convert_error_code(dberr_t err, int flags, THD *thd,
                              row_prebuilt_t *prebuilt, TABLE *table) {
  int error;
  switch (err) {
    case DB_SUCCESS:
      error = 0;
      srv_stats.n_rows_read.add(thd_get_thread_id(prebuilt->trx->mysql_thd), 1);
      break;
    case DB_END_OF_INDEX:
      error = HA_ERR_END_OF_FILE;
      break;
    default:
      error = convert_error_code_to_mysql(err, prebuilt->table->flags, thd);
      break;
  }

  return error;
}

/**
 * parallel scan worker read a record from partititon and store it in buf
 *
 */
int ha_innobase::pq_worker_scan_next(void *scan_ctx, uchar *buf) {
  dberr_t err{DB_SUCCESS};
  ut_a(scan_ctx != nullptr);

  auto pq_leader = static_cast<Parallel_leader *>(scan_ctx);
  if (pq_leader->is_error_set()) return err;

retry:
  if (!m_prebuilt->is_attach_ctx) {
    if (pq_ref_depend) {
      m_prebuilt->pq_ref_info = {pq_ref_depend, pq_ref_key.key,
                                 pq_ref_key.length};
      m_prebuilt->pq_worker->p_ref_key =
          std::make_shared<PQ_ref_key>(pq_ref_key.key, pq_ref_key.length);
    }

    RegisterActiveIndexes r_a_i(
        m_prebuilt->session,
        (m_prebuilt->table->is_intrinsic() ? m_prebuilt->old_index : nullptr));

    if (!m_prebuilt->pq_worker->dispatch_ctx(m_prebuilt->pq_ref_info,
                                             &m_prebuilt->pq_ctx)) {
      err = DB_SUCCESS;
      m_prebuilt->is_attach_ctx = true;
    } else {
      err = DB_END_OF_INDEX;
      goto end;
    }
  } else {
    // Check whether the current ref_key differs from the previously stored one.
    // If it has changed and m_prebuilt->is_attach_ctx is true, it needs to be
    // reset to false. In typical scenarios, a ref_key is scanned until
    // DB_END_OF_INDEX or DB_END_OF_RANGE, at which point is_attach_ctx is set
    // to false. However, consider the query 'SELECT DISTINCT t1.a FROM t1, t3
    // WHERE t1.a = t3.a'. Due to the presence of DISTINCT, the scan for a given
    // ref_key may stop after finding just one matching row in the inner table,
    // without resetting is_attach_ctx. As a result, the next ref_key inherits
    // the previous is_attach_ctx and pq_ctx, leading to incorrect join
    // conditions being satisfied and producing unintended output.
    if (pq_ref_depend && m_prebuilt->pq_worker->p_ref_key != nullptr &&
        (m_prebuilt->pq_worker->p_ref_key->len != pq_ref_key.length ||
         memcmp(m_prebuilt->pq_worker->p_ref_key->ptr, pq_ref_key.key,
                pq_ref_key.length))) {
      m_prebuilt->is_attach_ctx = false;
      goto retry;
    }
  }

  {
    auto ctx = m_prebuilt->pq_ctx;
    err = static_cast<dberr_t>(ctx->read_record(buf, m_prebuilt));
    if (err != DB_SUCCESS) {
      if (err == DB_END_OF_INDEX || err == DB_END_OF_RANGE) {
        m_prebuilt->is_attach_ctx = false;
        goto retry;
      } else if (err == DB_NOT_FOUND) {
        goto retry;
      } else if (!pq_leader->is_error_set()) {
        pq_leader->set_error_state(err);
      }
    }
  }

end:
  return (convert_error_code(err, 0, current_thd, m_prebuilt, table));
}

int ha_innobase::pq_leader_scan_end(void *scan_ctx) {
  active_index = MAX_KEY;
  Parallel_leader *parallel_leader = static_cast<Parallel_leader *>(scan_ctx);

  ut::delete_(parallel_leader);
  return 0;
}

int ha_innobase::pq_worker_scan_end() {
  pq_ref_depend = false;
  if (m_prebuilt == nullptr) return 0;

  m_prebuilt->pq_ctx = nullptr;
  m_prebuilt->pq_worker = nullptr;
  m_prebuilt->pq_ref_info.pq_ref_depend = false;

  /* reset the cached-variables of record buffer */
  m_prebuilt->n_fetch_cached = 0;
  m_prebuilt->fetch_cache_first = 0;
  m_prebuilt->n_rows_fetched = 0;
  return 0;
}
