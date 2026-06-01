#include "row0pread_pq.h"
#include "btr0pcur.h"
#include "dict0dict.h"
#include "record_buffer.h"
#include "row0mysql.h"
#include "row0row.h"
#include "row0sel.h"
#include "row0vers.h"
#include "template_utils.h"
#include "ut0new.h"

#include "my_dbug.h"
// As we do atomic operations on a dberr_t pretending it's int, size must be
// the same.
static_assert(sizeof(dberr_t) == sizeof(int) &&
                  DB_SUCCESS == DB_SUCCESS_AS_IN_INNODB &&
                  DB_ERROR == DB_ERROR_AS_IN_INNODB &&
                  DB_END_OF_INDEX == DB_END_OF_INDEX_AS_IN_INNODB &&
                  DB_END_OF_RANGE == DB_END_OF_RANGE_AS_IN_INNODB &&
                  DB_NOT_FOUND == DB_NOT_FOUND_AS_IN_INNODB,
              "mismatch in pq_handler constants");

/** Persistent cursor wrapper around btr_pcur_t */
class PQ_PCursor {
 public:
  /** Constructor.
   *   @param[in,out]  pcur  Persistent cursor in use.
   *     @param[in]      mtr   Mini transaction used by the persistent cursor.
   */
  PQ_PCursor(btr_pcur_t *pcur, mtr_t *mtr) : m_mtr(mtr), m_pcur(pcur) {}

  /** Check if are threads waiting on the index latch. Yield the latch
  so that other threads can progress.
  @param[in] direction  the move direction when traversing records in page
  */
  void yield(row_sel_direction direction);

  /** Move to the next block.
   *   @param[in]  index  Index being traversed.
   *     @return DB_SUCCESS or error code. */
  dberr_t move_to_next_block(dict_index_t *index)
      MY_ATTRIBUTE((warn_unused_result));

  dberr_t move_to_prev_block(dict_index_t *index)
      MY_ATTRIBUTE((warn_unused_result));

  /** Restore the cursor position. */
  void restore_position() {
    auto relative = m_pcur->m_rel_pos;

    auto equal =
        m_pcur->restore_position(BTR_SEARCH_LEAF, m_mtr, UT_LOCATION_HERE);

    if (relative == BTR_PCUR_ON) {
      if (!equal) {
        page_cur_move_to_next(m_pcur->get_page_cur());
      }
    } else {
      ut_ad(relative == BTR_PCUR_AFTER ||
            relative == BTR_PCUR_AFTER_LAST_IN_TREE ||
            relative == BTR_PCUR_BEFORE);
    }
  }

 private:
  /** Mini transaction. */
  mtr_t *m_mtr{};

  /** Persistent cursor. */
  btr_pcur_t *m_pcur{};
};

buf_block_t *PQ_Scan_ctx::block_get_s_latched(const page_id_t &page_id,
                                              mtr_t *mtr, int line) const {
  auto block = buf_page_get_gen(page_id, m_config_copy.m_page_size, RW_S_LATCH,
                                nullptr, Page_fetch::SCAN,
                                ut::Location{__FILE__, (size_t)line}, mtr);

  buf_block_dbg_add_level(block, SYNC_TREE_NODE);

  return (block);
}

Iter::~Iter() {
  if (m_heap == nullptr) {
    return;
  }

  if (m_pcur != nullptr) {
    m_pcur->free_rec_buf();
    /* Created with placement new on the heap. */
    call_destructor(m_pcur);
  }

  if (m_pcur_template != nullptr) {
    m_pcur_template->free_rec_buf();
    /* Created with placement new on the heap. */
    call_destructor(m_pcur_template);
  }

  mem_heap_free(m_heap);
  m_heap = nullptr;
}

const void *Iter::get_border() {
  ut_ad(m_tuple == nullptr || dtuple_validate(m_tuple));
  return m_tuple;
}

void PQ_Scan_ctx::copy_row(const rec_t *rec, Iter *iter) const {
  auto &config = m_config_copy;
  iter->m_offsets =
      rec_get_offsets(rec, config.m_index, nullptr, ULINT_UNDEFINED,
                      UT_LOCATION_HERE, &iter->m_heap);

  /* Copy the row from the page to the scan iterator. The copy should use
  memory from the iterator heap because the scan iterator owns the copy. */
  auto rec_len = rec_offs_size(iter->m_offsets);

  auto copy_rec = static_cast<rec_t *>(mem_heap_alloc(iter->m_heap, rec_len));

  memcpy(copy_rec, rec, rec_len);

  iter->m_rec = copy_rec;

  auto tuple = row_rec_to_index_entry_low(iter->m_rec, config.m_index,
                                          iter->m_offsets, iter->m_heap);

  ut_ad(dtuple_validate(tuple));

  /* We have copied the entire record but we only need to compare the
  key columns when we check for boundary conditions. */
  const auto n_compare = dict_index_get_n_unique_in_tree(config.m_index);

  dtuple_set_n_fields_cmp(tuple, n_compare);

  iter->m_tuple = tuple;
}

std::shared_ptr<Iter> PQ_Scan_ctx::create_persistent_cursor(
    const page_cur_t &page_cursor, mtr_t *mtr) const {
  ut_ad(index_s_own());
  auto &config = m_config_copy;

  std::shared_ptr<Iter> iter = std::make_shared<Iter>();

  iter->m_heap = mem_heap_create(
      2 * (sizeof(btr_pcur_t) + (srv_page_size / 16)), UT_LOCATION_HERE);

  ut_a(page_is_leaf(buf_block_get_frame(page_cursor.block)));

  auto rec = page_cursor.rec;

  const bool is_infimum = page_rec_is_infimum(rec);

  if (is_infimum) {
    rec = page_rec_get_next(rec);
  }

  if (page_rec_is_supremum(rec)) {
    /* Empty page, only root page can be empty. */
    ut_a(!is_infimum ||
         page_cursor.block->page.id.page_no() == config.m_index->page);
    return (iter);
  }

  void *ptr = mem_heap_alloc(iter->m_heap, sizeof(btr_pcur_t));
  void *ptr_tempalte = mem_heap_alloc(iter->m_heap, sizeof(btr_pcur_t));

  ::new (ptr) btr_pcur_t();
  ::new (ptr_tempalte) btr_pcur_t();

  iter->m_pcur = reinterpret_cast<btr_pcur_t *>(ptr);
  iter->m_pcur_template = reinterpret_cast<btr_pcur_t *>(ptr_tempalte);

  iter->m_pcur->init();

  /* Make a copy of the rec. */
  copy_row(rec, iter.get());

  iter->m_pcur->open_on_user_rec(page_cursor, PAGE_CUR_GE,
                                 BTR_ALREADY_S_LATCHED | BTR_SEARCH_LEAF);

  iter->m_pcur->store_position(mtr);
  btr_pcur_t::copy_stored_position(iter->m_pcur_template, iter->m_pcur);

  iter->m_pcur->set_fetch_type(Page_fetch::SCAN);

  return (iter);
}

void PQ_Scan_ctx::partition(const PQ_Borders &scan_borders, size_t level,
                            Ranges &ranges) {
  ut_ad(index_s_own());

  mtr_t mtr;
  mtr.start();
  mtr.set_log_mode(MTR_LOG_NO_REDO);
  auto &config = m_config_copy;
  create_ranges(scan_borders, config.m_index->page, 0, level, ranges, &mtr);

  assert(m_reader->is_reverse() == config.m_pq_reverse_scan);

  if (config.m_pq_reverse_scan && !ranges.empty()) {
    PQ_PCursor pcursor(config.m_pcur, &mtr);
    pcursor.restore_position();
    auto &iter = ranges.back()->m_iters.second;
    auto page_cursor = config.m_pcur->get_page_cur();
    page_cursor->index = config.m_index;
    iter = create_persistent_cursor(*page_cursor, &mtr);

    /* deep copy of start of first ctx */
    if (scan_borders.m_start == nullptr) {
      auto &first_iter = ranges.front()->m_iters.first;
      first_iter = std::make_shared<Iter>();
    }
    config.m_pcur->store_position(&mtr);
  } else if (scan_borders.m_end != nullptr && !ranges.empty()) {
    auto iter = down_cast<Iter *>(ranges.back()->m_iters.second.get());
    ut_a(iter->m_heap == nullptr);
    iter->m_heap = mem_heap_create(sizeof(btr_pcur_t) + (srv_page_size / 16),
                                   UT_LOCATION_HERE);
    /* Do a deep copy. */
    iter->m_tuple = pq_dtuple_dup(
        static_cast<const dtuple_t *>(scan_borders.m_end), iter->m_heap);
  }

  mtr.commit();
}

page_no_t PQ_Scan_ctx::search(const buf_block_t *block,
                              const dtuple_t *key) const {
  ut_ad(index_s_own());

  page_cur_t page_cursor;
  const auto index = m_config_copy.m_index;

  if (key != nullptr) {
    page_cur_search(block, index, key, PAGE_CUR_LE, &page_cursor);
  } else {
    page_cur_set_before_first(block, &page_cursor);
  }

  if (page_rec_is_infimum(page_cur_get_rec(&page_cursor))) {
    page_cur_move_to_next(&page_cursor);
  }

  const auto rec = page_cur_get_rec(&page_cursor);

  mem_heap_t *heap = nullptr;

  ulint offsets_[REC_OFFS_NORMAL_SIZE];
  auto offsets = offsets_;

  rec_offs_init(offsets_);

  offsets = rec_get_offsets(rec, index, offsets, ULINT_UNDEFINED,
                            UT_LOCATION_HERE, &heap);

  auto page_no = btr_node_ptr_get_child_page_no(rec, offsets);

  if (heap != nullptr) {
    mem_heap_free(heap);
  }

  return (page_no);
}

page_cur_t PQ_Scan_ctx::start_range(page_no_t page_no, mtr_t *mtr,
                                    const dtuple_t *key,
                                    Savepoints &savepoints) const {
  ut_ad(index_s_own());
  auto &config = m_config_copy;

  auto index = config.m_index;
  page_id_t page_id(index->space, page_no);

  /* Follow the left most pointer down on each page. */
  for (;;) {
    auto savepoint = mtr->get_savepoint();

    auto block = block_get_s_latched(page_id, mtr, __LINE__);

    savepoints.push_back({savepoint, block});

    if (!page_is_leaf(buf_block_get_frame(block))) {
      page_id.set_page_no(search(block, key));
      continue;
    }

    page_cur_t page_cursor;

    if (key != nullptr) {
      page_cur_search(block, index, key, PAGE_CUR_GE, &page_cursor);
    } else {
      page_cur_set_before_first(block, &page_cursor);
    }

    if (page_rec_is_infimum(page_cur_get_rec(&page_cursor))) {
      page_cur_move_to_next(&page_cursor);
    }

    return (page_cursor);
  }

  ut_error;
  return (page_cur_t{});
}

void PQ_Scan_ctx::create_range(Ranges &ranges, page_cur_t &leaf_page_cursor,
                               mtr_t *mtr) {
  auto &config = m_config_copy;
  leaf_page_cursor.index = config.m_index;

  PQ_ref_key ref_key1;
  if (config.m_ref_depend)
    new (&ref_key1) PQ_ref_key(config.m_ref_key, config.m_ref_key_len);

  auto slices = m_reader->pq_slices_map[ref_key1];
  assert(slices != nullptr);
  auto range_id = slices->m_slice_id.fetch_add(1, std::memory_order_relaxed);

  auto iter = create_persistent_cursor(leaf_page_cursor, mtr);
  /* Setup the previous range (next) to point to the current range. */
  if (!ranges.empty()) {
    ut_a((down_cast<Iter *>(ranges.back()->m_iters.second.get()))->m_heap ==
         nullptr);
    ranges.back()->m_iters.second = iter;
  }

  Iters iters = std::make_pair(iter, std::make_shared<Iter>());
  /*
    This is storing a naked pointer to 'this' in PQ_Range. To make sure that
    the lifetime of 'this' is long enough, we add a shared pointer to it in
    Parallel_leader_Base::m_scan_ctxs, see the comment of that class.
  */
  auto range = std::shared_ptr<PQ_Range>(
      ut::new_withkey<PQ_Range>(UT_NEW_THIS_FILE_PSI_KEY, range_id, this, iters,
                                false),
      [](PQ_Range *range) { ut::delete_(range); });

  ranges.push_back(range);
}

void PQ_Scan_ctx::create_ranges(const PQ_Borders &scan_borders,
                                page_no_t page_no, size_t depth,
                                const size_t level, Ranges &ranges,
                                mtr_t *mtr) {
  ut_ad(index_s_own());
  ut_a(max_threads() > 0);
  ut_a(page_no != FIL_NULL);

  auto &config = m_config_copy;
  if (config.m_range_errno) return;

  /* Do a breadth first traversal of the B+Tree using recursion. We want to
  set up the scan ranges in one pass. This guarantees that the tree structure
  cannot change while we are creating the scan sub-ranges.

  Once we create the persistent cursor (Range) for a sub-tree we can release
  the latches on all blocks traversed for that sub-tree. */

  /* For range building, a page is an atom. We never divide inside a page. So,
  if the table is so small that the whole index fits into 1 page, there will
  be only one range, only one PQ_Ctx, thus only one worker will read rows,
  while other workers will not (practically, the query will execute
  single-threaded). */

  const auto index = config.m_index;
  page_id_t page_id(index->space, page_no);
  Savepoint savepoint({mtr->get_savepoint(), nullptr});
  auto block = block_get_s_latched(page_id, mtr, __LINE__);
  savepoint.second = block;

  ulint offsets_[REC_OFFS_NORMAL_SIZE];
  auto offsets = offsets_;
  rec_offs_init(offsets_);
  page_cur_t page_cursor;
  page_cursor.index = index;
  auto start = static_cast<const dtuple_t *>(scan_borders.m_start);

  if (start != nullptr) {
    auto mode = PAGE_CUR_LE;
    page_cur_search(block, index, start, mode, &page_cursor);

    if (page_cur_is_after_last(&page_cursor)) {
      return;
    } else if (page_rec_is_infimum(page_cur_get_rec(&page_cursor))) {
      page_cur_move_to_next(&page_cursor);
    }
  } else {
    page_cur_set_before_first(block, &page_cursor);
    /* Skip the infimum record. */
    page_cur_move_to_next(&page_cursor);
  }

  mem_heap_t *heap{};
  const auto at_leaf = page_is_leaf(buf_block_get_frame(block));
  Savepoints savepoints{};

  while (!page_cur_is_after_last(&page_cursor)) {
    const auto rec = page_cur_get_rec(&page_cursor);

    ut_a(at_leaf || rec_get_node_ptr_flag(rec) ||
         !dict_table_is_comp(index->table));

    if (heap == nullptr) {
      heap = mem_heap_create(srv_page_size / 4, UT_LOCATION_HERE);
    }

    offsets = rec_get_offsets(rec, index, offsets, ULINT_UNDEFINED,
                              UT_LOCATION_HERE, &heap);
    const auto end = static_cast<const dtuple_t *>(scan_borders.m_end);

    if (end != nullptr && end->compare(rec, index, offsets) < 0) {
      break;
    }

    page_cur_t leaf_page_cursor;

    if (!at_leaf) {
      auto page_no = btr_node_ptr_get_child_page_no(rec, offsets);

      if (depth < level) {
        /* Need to create a range starting at a lower level in the tree. */
        create_ranges(scan_borders, page_no, depth + 1, level, ranges, mtr);
        page_cur_move_to_next(&page_cursor);
        continue;
      }

      /* Find the range start in the leaf node. */
      leaf_page_cursor = start_range(page_no, mtr, start, savepoints);
    } else {
      if (start != nullptr) {
        page_cur_search(block, index, start, PAGE_CUR_GE, &page_cursor);
        ut_a(!page_rec_is_infimum(page_cur_get_rec(&page_cursor)));
      } else {
        page_cur_set_before_first(block, &page_cursor);

        /* Skip the infimum record. */
        page_cur_move_to_next(&page_cursor);
        ut_a(!page_cur_is_after_last(&page_cursor));
      }

      /* Since we are alread at a leaf node use the current page cursor. */
      memcpy(&leaf_page_cursor, &page_cursor, sizeof(leaf_page_cursor));
    }

    ut_a(page_is_leaf(buf_block_get_frame(leaf_page_cursor.block)));

    if (!page_rec_is_supremum(page_cur_get_rec(&leaf_page_cursor))) {
      create_range(ranges, leaf_page_cursor, mtr);
    }

    /* We've created the persistent cursor, safe to release S latches on
    the blocks that are in this range (sub-tree). */
    for (auto &savepoint : savepoints) {
      mtr->release_block_at_savepoint(savepoint.first, savepoint.second);
    }

    if (m_depth == 0 && depth == 0) {
      m_depth = savepoints.size();
    }

    savepoints.clear();

    if (at_leaf) {
      break;
    }

    start = nullptr;

    page_cur_move_to_next(&page_cursor);
  }

  savepoints.push_back(savepoint);

  for (auto &savepoint : savepoints) {
    mtr->release_block_at_savepoint(savepoint.first, savepoint.second);
  }

  if (heap != nullptr) {
    mem_heap_free(heap);
  }
}

std::shared_ptr<PQ_Ctx_Base> PQ_Scan_ctx::make_ctx(
    std::shared_ptr<PQ_Range> &range) {
  if (m_config_copy.is_intrinsic)
    return std::shared_ptr<PQ_Ctx_Base>(
        ut::new_withkey<PQ_Ctx_intrinsic>(UT_NEW_THIS_FILE_PSI_KEY, range->id(),
                                          this, range),
        [](PQ_Ctx_intrinsic *ctx) { ut::delete_(ctx); });
  else
    return std::shared_ptr<PQ_Ctx_Base>(
        ut::new_withkey<PQ_Ctx>(UT_NEW_THIS_FILE_PSI_KEY, range->id(), this,
                                range),
        [](PQ_Ctx *ctx) { ut::delete_(ctx); });
}

void PQ_Scan_ctx::index_s_lock() {
  if (m_s_locks.fetch_add(1, std::memory_order_acquire) == 0) {
    auto &config = m_config_copy;
    auto index = config.m_index;
    /* The latch can be unlocked by a thread that didn't originally lock it. */
    rw_lock_s_lock_gen(dict_index_get_lock(index), true, UT_LOCATION_HERE);
  }
}

void PQ_Scan_ctx::index_s_unlock() {
  if (m_s_locks.fetch_sub(1, std::memory_order_acquire) == 1) {
    auto &config = m_config_copy;
    auto index = config.m_index;
    /* The latch can be unlocked by a thread that didn't originally lock it. */
    rw_lock_s_unlock_gen(dict_index_get_lock(index), true);
  }
}

/**
 * Find a innodb record's visible version from multiple versions
 *
 * The @param[out] buf is buffer stored visible record, Since it do ICP check
 * in this function, and ICP item field is relative to handler::record[0],
 * So the 'buf' must be record[0]
 *
 */
dberr_t PQ_Scan_ctx::find_visible_record(byte *buf, const rec_t *&rec,
                                         const rec_t *&clust_rec,
                                         ulint *&offsets, ulint *&clust_offsets,
                                         mem_heap_t *&heap, mtr_t *mtr,
                                         bool &mtr_has_extra_clust_latch,
                                         row_prebuilt_t *prebuilt) {
  auto &config = m_config_copy;
  const auto table_name = config.m_index->table->name;
  ut_ad(m_trx->read_view == nullptr || MVCC::is_view_active(m_trx->read_view));
  ut_ad(!prebuilt->table->is_intrinsic());

  if (m_trx->read_view != nullptr) {
    auto view = m_trx->read_view;

    if (config.m_index->is_clustered()) {
      trx_id_t rec_trx_id;

      if (config.m_index->trx_id_offset > 0) {
        rec_trx_id = trx_read_trx_id(rec + config.m_index->trx_id_offset);
      } else {
        rec_trx_id = row_get_rec_trx_id(rec, config.m_index, offsets);
      }

      if (m_trx->isolation_level > TRX_ISO_READ_UNCOMMITTED &&
          !view->changes_visible(rec_trx_id, table_name)) {
        rec_t *old_vers;

        row_vers_build_for_consistent_read(rec, mtr, config.m_index, &offsets,
                                           view, &heap, heap, &old_vers,
                                           nullptr, nullptr);

        rec = old_vers;
        if (rec == nullptr) {
          return DB_NOT_FOUND;
        }
      }
    } else {
      /* Secondary index scan not supported yet. */
      auto max_trx_id = page_get_max_trx_id(page_align(rec));
      ut_ad(max_trx_id > 0);

      ut_ad(prebuilt && prebuilt->select_lock_type == LOCK_NONE);
      if (prebuilt->idx_cond) {
        switch (row_search_idx_cond_check(buf, prebuilt, rec, offsets)) {
          case ICP_NO_MATCH: {
            return DB_NOT_FOUND;
          }
          case ICP_OUT_OF_RANGE:
            return DB_END_OF_RANGE;
          case ICP_MATCH:
            break;
        }
      }

      if (!view->sees(max_trx_id) || prebuilt->need_to_access_clustered) {
        if (prebuilt->sel_graph == nullptr) row_prebuild_sel_graph(prebuilt);
        que_thr_t *thr = que_fork_get_first_thr(prebuilt->sel_graph);

        mtr_has_extra_clust_latch = true;
        int err = pq_row_sel_get_clust_rec_for_mysql(
            prebuilt, config.m_index, rec, thr, &clust_rec, &clust_offsets,
            &heap, NULL, mtr);

        if (err != DB_SUCCESS)
          return DB_NOT_FOUND;
        else {
          if (clust_rec == NULL) {
            /* The record did not exist in the read view */
            ut_ad(prebuilt->select_lock_type == LOCK_NONE);

            return DB_NOT_FOUND;
          } else if (rec_get_deleted_flag(clust_rec, config.m_is_compact)) {
            /* The record is delete marked: we can skip it */
            return DB_NOT_FOUND;
          } else {
            return DB_SUCCESS;
          }
        }
      }
    }
  } else if (srv_read_only_mode && /** innodb_read_only */
             (prebuilt &&
              prebuilt->need_to_access_clustered && /** secondary index and
                                                       non-covered index */
              !config.m_index->is_clustered())) {
    if (prebuilt->idx_cond) {
      switch (row_search_idx_cond_check(buf, prebuilt, rec, offsets)) {
        case ICP_NO_MATCH:
          return DB_NOT_FOUND;
        case ICP_OUT_OF_RANGE:
          return DB_END_OF_RANGE;
        case ICP_MATCH:
          break;
      }
    }

    if (prebuilt->sel_graph == nullptr) row_prebuild_sel_graph(prebuilt);

    mtr_has_extra_clust_latch = true;
    que_thr_t *thr = que_fork_get_first_thr(prebuilt->sel_graph);
    int err = pq_row_sel_get_clust_rec_for_mysql(
        prebuilt, config.m_index, rec, thr, &clust_rec, &clust_offsets, &heap,
        NULL, mtr);

    if (err != DB_SUCCESS)
      return DB_NOT_FOUND;
    else {
      if (clust_rec == NULL) {
        /* The record did not exist in the read view */
        ut_ad(prebuilt->select_lock_type == LOCK_NONE);

        return DB_NOT_FOUND;
      } else if (rec_get_deleted_flag(clust_rec, config.m_is_compact)) {
        /* The record is delete marked: we can skip it */
        return DB_NOT_FOUND;
      } else {
        return DB_SUCCESS;
      }
    }
  }

  if (rec_get_deleted_flag(rec, config.m_is_compact)) {
    /* This record was deleted in the latest committed version, or it was
    deleted and then reinserted-by-update before purge kicked in. Skip it. */
    return DB_NOT_FOUND;
  }

  return DB_SUCCESS;
}

void PQ_PCursor::yield(row_sel_direction direction) {
  /* We should always yield on a block boundary. */
  if (direction == ROW_SEL_NEXT) {
    ut_ad(m_pcur->is_after_last_on_page());

    /* Store the cursor position on the last user record on the page. */
    m_pcur->move_to_prev_on_page();

  } else {
    ut_ad(direction == ROW_SEL_PREV);
    ut_ad(m_pcur->is_before_first_on_page());

    /* Store the cursor position on the first user record on the page */
    m_pcur->move_to_next_on_page();
  }

  m_pcur->store_position(m_mtr);

  m_mtr->commit();

  /* Yield so that another thread can proceed. */
  std::this_thread::yield();

  m_mtr->start();

  m_mtr->set_log_mode(MTR_LOG_NO_REDO);

  /* Restore position on the record, or its predecessor if the record
  was purged meanwhile. */

  restore_position();

  if (direction == ROW_SEL_NEXT) {
    if (!m_pcur->is_after_last_on_page()) {
      /* Move to the successor of the saved record. */
      m_pcur->move_to_next_on_page();
    }
  } else if (direction == ROW_SEL_PREV) {
    if (!m_pcur->is_before_first_on_page()) {
      /* Move to the precursor of the saved record */
      m_pcur->move_to_prev_on_page();
    }
  }
}

dberr_t PQ_PCursor::move_to_next_block(dict_index_t *index) {
  ut_ad(m_pcur->is_after_last_on_page());

  if (rw_lock_get_waiters(dict_index_get_lock(index)) ||
      DBUG_EVALUATE_IF("move_block_yield", true, false)) {
    /* There are waiters on the index tree lock. Store and restore
    the cursor position, and yield so that scanning a large table
    will not starve other threads. */

    yield(/*move_direction=*/ROW_SEL_NEXT);

    /* It's possible that the restore places the cursor in the middle of
    the block. We need to account for that too. */

    if (m_pcur->is_on_user_rec()) {
      return (DB_SUCCESS);
    }
  }

  auto cur = m_pcur->get_page_cur();
  auto next_page_no = btr_page_get_next(page_cur_get_page(cur), m_mtr);

  if (next_page_no == FIL_NULL) {
    m_mtr->commit();

    return (DB_END_OF_INDEX);
  }

  auto block = page_cur_get_block(cur);
  const auto &page_id = block->page.id;

  block = buf_page_get_gen(
      page_id_t(page_id.space(), next_page_no), block->page.size, RW_S_LATCH,
      nullptr, Page_fetch::SCAN, ut::Location{__FILE__, __LINE__}, m_mtr);

  buf_block_dbg_add_level(block, SYNC_TREE_NODE);

  btr_leaf_page_release(page_cur_get_block(cur), RW_S_LATCH, m_mtr);

  page_cur_set_before_first(block, cur);

  /* Skip the infimum record. */
  page_cur_move_to_next(cur);

  /* Page can't be empty unless it is a root page. */
  ut_ad(!page_cur_is_after_last(cur));

  return (DB_SUCCESS);
}

dberr_t PQ_PCursor::move_to_prev_block(dict_index_t *index) {
  ut_ad(m_pcur->is_before_first_on_page());

  if (rw_lock_get_waiters(dict_index_get_lock(index)) ||
      DBUG_EVALUATE_IF("move_block_yield", true, false)) {
    /* There are waiters on the index tree lock. Store and restore
    the cursor position, and yield so that scanning a large table
    will not starve other threads. */

    yield(/*move_direction=*/ROW_SEL_PREV);

    /* It's possible that the restore places the cursor in the middle of
    the block. We need to account for that too. */

    if (m_pcur->is_on_user_rec()) {
      return (DB_SUCCESS);
    }
  }

  auto cur = m_pcur->get_page_cur();

  auto prev_page_no = btr_page_get_prev(page_cur_get_page(cur), m_mtr);

  if (prev_page_no == FIL_NULL) {
    m_mtr->commit();

    return (DB_END_OF_INDEX);
  }

  auto block = page_cur_get_block(cur);
  const auto &page_id = block->page.id;

  block = buf_page_get_gen(
      page_id_t(page_id.space(), prev_page_no), block->page.size, RW_S_LATCH,
      nullptr, Page_fetch::SCAN, ut::Location{__FILE__, __LINE__}, m_mtr);

  buf_block_dbg_add_level(block, SYNC_TREE_NODE);

  btr_leaf_page_release(page_cur_get_block(cur), RW_S_LATCH, m_mtr);

  page_cur_set_after_last(block, cur);

  /* Skip the supremum record. */
  page_cur_move_to_prev(cur);

  /* Page can't be empty unless it is a root page. */
  ut_ad(!page_cur_is_before_first(cur));

  return (DB_SUCCESS);
}

PQ_Ctx::~PQ_Ctx() {
  if (m_blob_heap) {
    mem_heap_free(m_blob_heap);
  }
  if (m_heap) {
    mem_heap_free(m_heap);
  }
}

void PQ_Ctx::reset_for_next_read_round(bool reverse) {
  auto from = down_cast<Iter *>(
      (reverse ? m_range->m_iters.second : m_range->m_iters.first).get());
  btr_pcur_t::copy_stored_position(from->m_pcur, from->m_pcur_template);
  start_read = true;
}

void PQ_Ctx_intrinsic::reset_for_next_read_round(bool) {
  /* Intrinsic table will always do a search to locate the position for the
  first scan. In the following scan it will scan record by cached information
  unless the index tree has changed. Reset `start_read` to void reuse invalid
  cached information. */
  start_read = true;
}

// clang-format on
int PQ_Ctx::read_record(uchar *buf, void *handler_specific) {
  auto prebuilt = reinterpret_cast<row_prebuilt_t *>(handler_specific);
  /**
   * The workflow of read a record is as follow steps:
   *   1. Fetch a record into handler::record[0] from record buffer if it isn't
   * empty.
   *
   * Else read records from innodb page cursor:
   *   2.1. Read a innodb record from the innodb page current cursor and
   *   find the visible version of record.
   *   2.2. Convert the innodb record to MySQL record and store it in record
   * buffer space.
   *   2.3. Move cursor to next record in the page, and repeat the above
   * process.
   *   Continue step 2.1~2.3 until record buffer is full, or reach the end of
   * index/ctx that need set record buffer's status as OUT_OF_RANGE.
   *   2.4. Dequeue a record from record buffer into handler::record[0]
   *
   *   The benefit of record buffer is that we can fetch more than one records
   *   in one mtr. Note that we only support the record buffer allocated from
   * server.
   */

  /* 1. Try to fetch one record from record buffer. */
  ha_rows max_rows_to_cache = 0;
  const auto record_buffer = row_sel_get_record_buffer(prebuilt);
  DBUG_EXECUTE("pq_n_fetch_cached_dirty", {
    if (!record_buffer) prebuilt->n_fetch_cached = 3;
  });

  if (record_buffer) {
    max_rows_to_cache = record_buffer->max_records();
    if (prebuilt->n_fetch_cached > 0) {
      /* the server has reset the record buffer, e.g., re-init in nested-loop
      join, then we should reset the cached-variables in m_prebuilt. */
      if (record_buffer->records() == 0) {
        prebuilt->n_fetch_cached = 0;
        prebuilt->fetch_cache_first = 0;
        prebuilt->n_rows_fetched = 0;
      } else {
        row_sel_dequeue_cached_row_for_mysql(buf, prebuilt);
        return DB_SUCCESS;
      }
    } else if (record_buffer->is_out_of_range()) {
      record_buffer->reset();
      /* need to fetch another scan ctx */
      return DB_END_OF_RANGE;
    }
  } else {
    prebuilt->n_fetch_cached = 0;
    prebuilt->fetch_cache_first = 0;
    prebuilt->n_rows_fetched = 0;
  }

  // Else there is no records exists in record buffer, then we should read
  // records from innodb and store them into record buffer.
  btr_pcur_t *pcur;
  dberr_t err = DB_SUCCESS;
  dberr_t found_result = DB_SUCCESS;

  int ret{0};
  const rec_t *clust_rec;
  const rec_t *rec;
  const rec_t *result_rec;
  ulint *offsets = offsets_;
  ulint *clust_offsets = clust_offsets_;

  if (start_read) {
    rec_offs_init(offsets_);
    rec_offs_init(clust_offsets_);
    start_read = false;
  }

  mtr_t mtr;
  mtr.start();
  mtr.set_log_mode(MTR_LOG_NO_REDO);
  bool mtr_has_extra_clust_latch = false;

  auto scan_ctx = down_cast<PQ_Scan_ctx *>(m_scan_ctx);
  auto &config = scan_ctx->m_config_copy;
  auto from = get_iter(config.m_pq_reverse_scan);
  pcur = from->m_pcur;
  PQ_PCursor pcursor(pcur, &mtr);
  pcursor.restore_position();

  const auto end_tuple = get_iter(!config.m_pq_reverse_scan)->m_tuple;
  auto index = config.m_index;
  auto cur = pcur->get_page_cur();
  dict_index_t *clust_index = index->table->first_index();
  if (m_blob_heap == nullptr)
    m_blob_heap = mem_heap_create(srv_page_size, UT_LOCATION_HERE);
  if (m_heap == nullptr)
    m_heap = mem_heap_create(srv_page_size / 4, UT_LOCATION_HERE);

next_record:
  using MoveBlockFunc = dberr_t (PQ_PCursor::*)(dict_index_t * index);
  using PcurMoveFunc = void (*)(page_cur_t * cur);
  using PcurCheckFunc = bool (*)(const page_cur_t *cur);
  PcurCheckFunc boundaryCheck = config.m_pq_reverse_scan
                                    ? page_cur_is_before_first
                                    : page_cur_is_after_last;
  PcurMoveFunc pcurMove =
      config.m_pq_reverse_scan ? page_cur_move_to_prev : page_cur_move_to_next;
  MoveBlockFunc moveBlock = config.m_pq_reverse_scan
                                ? &PQ_PCursor::move_to_prev_block
                                : &PQ_PCursor::move_to_next_block;
  if (boundaryCheck(cur)) {
    /* pcur points to last/first record, move to next/prev block. */
    mem_heap_empty(m_heap);

    /**
     * In case the table has more than 95 columns(4 slots in clust_offsets
     * array would be reserved for other purpose), the clust_offsets will
     * be re-allocated from the m_heap object in rec_get_ndp_offsets().
     * However, the m_heap has been just empied, so the memory became invalid.
     * Thus it is necessary to re-initialize the clust_offsets to
     * clust_offsets_. Please note that the offsets for secondary index
     * won't be affected because at most 16 columns can be defined on
     * one secondary index.
     */
    offsets = offsets_;
    clust_offsets = clust_offsets_;
    rec_offs_init(offsets_);
    rec_offs_init(clust_offsets_);
    err = (pcursor.*moveBlock)(index);

    if (err == DB_END_OF_INDEX) {
      /* As move_to_next{before}_block only returns {DB_SUCCESS,
      DB_END_OF_INDEX}, the following assertion must hold. */
      ut_ad(!mtr.is_active());
      /*
       * When encouter the end of index, if there is some records in record
       * buffer, it should read out all those records. Since pcur was set
       * invalid in prior move_to_prev/next_block, so it cann't further read any
       * record from this pcur after reading out all those records, seting
       * record buffer's out_of_range is preventing to read innodb record using
       * this pcur.
       *
       * When there is no record in record buffer, it should get next Ctx and
       * reset record buffer outside "read_record" function, if there is no more
       * any Ctx , it will finish reading record. Because the record buffer is
       * only accessed by one worker thread, so it is safe to set
       * record_buffer's out_of_range here.
       */
      if (prebuilt->n_fetch_cached > 0) {
        ut_ad(record_buffer);
        record_buffer->set_out_of_range(true);
        /* reach to the end, we already read a record, just return success */
        err = DB_SUCCESS;
        goto dequeue_cache;
      }
      /* there is no more record */
      return err;
    }
  }
  /* 2.1. read one record from innodb page */
  rec = page_cur_get_rec(cur);
  offsets = rec_get_offsets(rec, index, offsets, ULINT_UNDEFINED,
                            UT_LOCATION_HERE, &m_heap);
  clust_offsets = rec_get_offsets(rec, index, clust_offsets, ULINT_UNDEFINED,
                                  UT_LOCATION_HERE, &m_heap);

  /* Find visible version record
   *
   * Since this function will do ICP Item cond check, and ICP Item is relative
   * to mysql record buffer, so we should push down mysql record buffer which is
   * handler::record[0] as first parameter to this function.
   * */
  found_result = scan_ctx->find_visible_record(
      buf, rec, clust_rec, offsets, clust_offsets, m_heap, &mtr,
      mtr_has_extra_clust_latch, prebuilt);

  /* check range boundary */
  if (rec != nullptr && end_tuple != nullptr) {
    ret = end_tuple->compare(rec, index, offsets);
    /* Note: The range creation doesn't use MVCC. Therefore it's possible
    that the range boundary entry could have been deleted. */
    if ((!config.m_pq_reverse_scan && ret <= 0) ||
        (config.m_pq_reverse_scan && ret >= 0)) {
      if (prebuilt->n_fetch_cached > 0) {
        // There is cached records need to read, so return DB_SUCCESS here. and
        // set out of range for record buffer, which will lead fetch next Ctx
        // after read all cached records.
        record_buffer->set_out_of_range(true);
        err = DB_SUCCESS;
      } else {
        // There is no more records need to read, and don't cache any record, so
        // shoule fetch next Ctx.
        err = DB_END_OF_RANGE;
      }
      goto commit_mtr;
    }
  }

  /* 2.2 convert record to mysql format */
  if (found_result == DB_SUCCESS) {
    /* fetch the record to be cached in the record buffer (i.e., the record
    after the last already cached record.) */
    uchar *curr_buf =
        (record_buffer != nullptr) ? record_buffer->add_record() : buf;
    /* As we have not cloned end_range in handler, the evaluation of ICP
    only returns {DB_SUCCESS, DB_NOT_FOUND}. */
    if (mtr_has_extra_clust_latch) {
      result_rec = clust_rec;
      if (!row_sel_store_mysql_rec(curr_buf, prebuilt, clust_rec, nullptr, true,
                                   clust_index, prebuilt->index, clust_offsets,
                                   false, nullptr, m_blob_heap))
        err = DB_ERROR;
    } else {
      result_rec = rec;
      if (!row_sel_store_mysql_rec(
              curr_buf, prebuilt, rec, nullptr, config.m_index->is_clustered(),
              index, prebuilt->index, offsets, false, nullptr, m_blob_heap))
        err = DB_ERROR;
    }

    if (prebuilt->clust_index_was_generated) {
      row_sel_store_row_id_to_prebuilt(
          prebuilt, result_rec, result_rec == rec ? index : clust_index,
          result_rec == rec ? offsets : clust_offsets);
    }
  }

  /* there are two cases we should submit the mtr: (1) error occurs when
  converting record from Innodb format to MySQL format; (2) mtr has an extra
  latch on cluster record when retrieving the clustered index record via
  the PK fields got from secondary index. */

  /* case 1: convert record from Innodb format to MySQL format failed, we
  directly submit mtr and return for error handling */
  if (err != DB_SUCCESS) {
    goto commit_mtr;
  }

  /* case 2: retrieve cluster record from non-clustered index, we submit the
  mtr to free the latch on clustered index, and then re-start it. */
  if (mtr_has_extra_clust_latch) {
    ut_ad(mtr.is_active());
    pcur->store_position(&mtr);
    mtr.commit();

    /* re-start the mtr */
    mtr.start();
    mtr.set_log_mode(MTR_LOG_NO_REDO);
    pcursor.restore_position();
    mtr_has_extra_clust_latch = false;
  }

  // 2.3. Move page cursor to next/prev record in the page.
  // In the previous case 2 handling flow, the S-latch on the current page
  // is released and then reacquired. During the interval between these two
  // actions, the page might be modified. Therefore, before moving the page
  // cursor, it's necessary to first check whether the cursor is already at
  // the boundary position, to avoid triggering an assertion when performing
  // the page movement.
  if (!boundaryCheck(cur)) pcurMove(cur);

  if (found_result == DB_NOT_FOUND) {
    /* the record in the page is not visible or filtered by ICP, we
    should fetch next record in the page. Then we can't advance curr_buff. */
    goto next_record;
  } else if (record_buffer) {
    ++prebuilt->n_fetch_cached;

    /* fetch another record */
    if (prebuilt->n_fetch_cached < max_rows_to_cache) {
      goto next_record;
    }
  }

commit_mtr:
  pcur->store_position(&mtr);
  ut_ad(mtr.is_active());
  mtr.commit();

dequeue_cache:
  /* 2.4. Dequeue a record from record buffer */
  if (prebuilt->n_fetch_cached > 0)
    row_sel_dequeue_cached_row_for_mysql(buf, prebuilt);

  return err;
}

bool PQ_Ctx_intrinsic::out_of_range(const rec_t *rec, const ulint *offsets) {
  auto scan_ctx = down_cast<PQ_Scan_ctx *>(m_scan_ctx);
  auto &config = scan_ctx->m_config_copy;
  const auto end_tuple = get_iter(!config.m_pq_reverse_scan)->m_tuple;

  if (end_tuple == nullptr) {
    return false;
  }

  int ret = end_tuple->compare(rec, index(), offsets);

  return ((!config.m_pq_reverse_scan && ret <= 0) ||
          (config.m_pq_reverse_scan && ret >= 0));
}

int PQ_Ctx_intrinsic::read_record(uchar *buf, void *handler_specific) {
  auto prebuilt = reinterpret_cast<row_prebuilt_t *>(handler_specific);

  ut_ad(prebuilt->table->is_intrinsic());

  auto scan_ctx = down_cast<PQ_Scan_ctx *>(m_scan_ctx);
  auto &config = scan_ctx->m_config_copy;
  auto reverse_scan = config.m_pq_reverse_scan;
  uint32_t direction = 0;
  page_cur_mode_t mode = PAGE_CUR_UNSUPP;
  auto from = get_iter(reverse_scan);
  btr_pcur_t *prebuilt_pcur = prebuilt->pcur;
  const dtuple_t *search_tuple = prebuilt->search_tuple;

  /* Just to keep the same logic for normal tables, which is always
  scanning via the from->m_pcur */
  prebuilt->pcur = from->m_pcur;

  if (start_read) {
    start_read = false;
    mode = reverse_scan ? PAGE_CUR_LE : PAGE_CUR_GE;

    if (from->m_tuple != nullptr) {
      /* To reduce code changes, do a cast, nobody will update it later */
      prebuilt->search_tuple = const_cast<dtuple_t *>(from->m_tuple);
      ut_ad(prebuilt->search_tuple->n_fields > 0);
    } else {
      dtuple_set_n_fields(prebuilt->search_tuple, 0);
    }
  } else {
    direction = reverse_scan ? ROW_SEL_PREV : ROW_SEL_NEXT;
  }

  auto ret = row_search_no_mvcc(buf, mode, prebuilt, 0, direction);

  prebuilt->pcur = prebuilt_pcur;
  prebuilt->search_tuple = const_cast<dtuple_t *>(search_tuple);
  return ret;
}

std::shared_ptr<PQ_Scan_ctx_Base> Parallel_leader::make_scan_ctx(
    void *trx, const PQ_Config_Base &config) {
  auto scan_ctx = std::shared_ptr<PQ_Scan_ctx>(
      ut::new_withkey<PQ_Scan_ctx>(UT_NEW_THIS_FILE_PSI_KEY, this,
                                   m_scan_ctx_id, static_cast<trx_t *>(trx),
                                   down_cast<const PQ_Config &>(config)),
      [](PQ_Scan_ctx *scan_ctx) { ut::delete_(scan_ctx); });

  if (scan_ctx == nullptr) {
    ib::error() << "Out of memory";
  }
  return scan_ctx;
}
