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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/** @file storage/temptable/src/handler_pq.cc
 Parallel query related interface implementation of TempTable
 *******************************************************/

#include <memory>

#include "sql/sql_class.h"
#include "temptable/handler.h"

namespace temptable {
/**
   When dividing a table scan for PQ, to reduce the risk that one single worker
   gets a range which produces a bigger join result than other workers (possible
   if data is skewed), we create (#workers)*this_constant ranges. That should
   make the said range be distributed among more workers.
*/
static const int PQ_BALANCE_FACTOR = 64;

struct PQ_Config : public PQ_Config_Base {
  using super = PQ_Config_Base;
  using super::super;
  PQ_Config(const PQ_Borders &scan_borders, const Storage &rows)
      : super(scan_borders), m_rows(rows) {}

  const Storage &m_rows;
};

struct Iter : public Iter_Base {
  const void *get_border() override { return m_rec_pos; }

  void *m_rec_pos{nullptr};  ///< pointer to pointer to record

  // nothing is needed here for index as it's not divided.
};

class PQ_Ctx : public PQ_Ctx_Base {
  using super = PQ_Ctx_Base;
  using super::super;
  int read_record(uchar *buf, void *handler_specific) override;
  void reset_for_next_read_round(bool reverse MY_ATTRIBUTE((unused))) override {
    /*
      It is reset at every re-read of the range (to choose between rnd_pos()
      and rnd_next())
    */
    start_read = true;
  }

 private:
  /**
     @returns a pointer to the start boundary, as iterator.
     @note that it is a naked pointer obtained from a shared pointer, use it to
     read members, don't store it.
     @param reverse    true if the scan is done in reverse order.
  */
  auto get_iter(bool reverse) {
    return down_cast<Iter *>(
        (reverse ? m_range->m_iters.second : m_range->m_iters.first).get());
  }
};

class Parallel_leader final : public Parallel_leader_Base {
 public:
  using Parallel_leader_Base::Parallel_leader_Base;
  std::shared_ptr<PQ_Scan_ctx_Base> make_scan_ctx(
      void * /*handler_specific*/, const PQ_Config_Base &config) override;
};

class PQ_Scan_ctx : public PQ_Scan_ctx_Base {
  using super = PQ_Scan_ctx_Base;

 public:
  PQ_Scan_ctx(Parallel_leader *reader, size_t id, const PQ_Config &config)
      : PQ_Scan_ctx_Base(reader, id, m_config_copy), m_config_copy(config) {}

 private:
  void index_s_lock() override {}
  void index_s_unlock() override {}
  bool index_s_own() const override { return true; }
  void partition(const PQ_Borders &, size_t, Ranges &) override;
  std::shared_ptr<PQ_Ctx_Base> make_ctx(
      std::shared_ptr<PQ_Range> &range) override {
    return std::allocate_shared<PQ_Ctx>(
        Mem_root_allocator<std::shared_ptr<PQ_Ctx>>(*THR_MALLOC), range->id(),
        this, range);
  }
  PQ_Config m_config_copy;

 public:
  ~PQ_Scan_ctx() override { m_config_copy.clear(); }
  friend class PQ_Ctx;
};

void PQ_Scan_ctx::partition(
    const PQ_Borders &scan_borders MY_ATTRIBUTE((unused)),
    size_t level MY_ATTRIBUTE((unused)), Ranges &ranges) {
  // Borders are left at nullptr, to mean "everything".
  assert(scan_borders.m_start == nullptr && scan_borders.m_end == nullptr);
  auto &config = m_config_copy;

  PQ_ref_key ref_key1;
  if (config.m_ref_depend)
    new (&ref_key1) PQ_ref_key(config.m_ref_key, config.m_ref_key_len);

  auto slices = m_reader->pq_slices_map[ref_key1];
  assert(slices != nullptr);
  auto alloc = Mem_root_allocator<std::shared_ptr<PQ_Range>>(*THR_MALLOC);

  // Note: data page size is 64 kB
  config.m_rows.do_over_ranges_of_pages(
      max_threads() * PQ_BALANCE_FACTOR,
      [&alloc, &ranges, this, &slices](void *range_start) {
        // Similar logic as in InnoDB's PQ_Scan_ctx::create_range()
        auto range_id =
            slices->m_slice_id.fetch_add(1, std::memory_order_relaxed);
        auto iter = std::make_shared<Iter>();
        iter->m_rec_pos = range_start;
        /* Setup the previous range (next) to point to the current range. */
        if (!ranges.empty()) {
          ranges.back()->m_iters.second = iter;
        }

        Iters iters = std::make_pair(iter, std::make_shared<Iter>());
        auto range =
            std::allocate_shared<PQ_Range>(alloc, range_id, this, iters, false);
        ranges.push_back(range);
      });

  // As this engine only supports PQ-division if the access method is JT_ALL
  // (see pq_not_support_scantype()), and as backward index scan is enabled
  // only in non-JT_ALL access methods (see test_if_skip_sort_order()), we can
  // assert the following, which simplifies the engine's PQ code.
  assert(!m_reader->is_reverse());
}

std::shared_ptr<PQ_Scan_ctx_Base> Parallel_leader::make_scan_ctx(
    void * /*handler_specific*/, const PQ_Config_Base &config) {
  return std::allocate_shared<PQ_Scan_ctx>(
      Mem_root_allocator<std::shared_ptr<PQ_Scan_ctx>>(*THR_MALLOC), this,
      m_scan_ctx_id, down_cast<const PQ_Config &>(config));
}

int PQ_Ctx::read_record(uchar *buf, void *handler_specific) {
  auto handler = static_cast<Handler *>(handler_specific);
  auto scan_ctx = down_cast<PQ_Scan_ctx *>(m_scan_ctx);
  auto &config = scan_ctx->m_config_copy;
  auto from = get_iter(config.m_pq_reverse_scan);
  auto to = get_iter(!config.m_pq_reverse_scan);
  int err;

  assert(scan_ctx->m_reader->key == MAX_KEY);  // table scan

  // 1. read record
  if (start_read) {
    start_read = false;
    err = handler->rnd_init(false);
    if (!err)
      // '&' is not a mistake, see code of rnd_pos() in Temptable engine.
      err = handler->rnd_pos(buf, reinterpret_cast<uchar *>(&from->m_rec_pos));
  } else {
    err = handler->rnd_next(buf);
  }

  if (err != (int)Result::OK) {
    if (err == (int)Result::END_OF_FILE) return DB_END_OF_INDEX_AS_IN_INNODB;
    return DB_ERROR_AS_IN_INNODB;
  }

  // 2. find visible version record
  // Nothing to do - no transactions in this engine.

  // 3. check range boundary

  handler->position(nullptr);
  auto end_tuple = to->m_rec_pos;
  if (end_tuple != nullptr) {
    if (!handler->cmp_ref(static_cast<uchar *>(handler->ref),
                          reinterpret_cast<uchar *>(&end_tuple)))
      return DB_END_OF_RANGE_AS_IN_INNODB;
  }

  return DB_SUCCESS_AS_IN_INNODB;
}

int Handler::pq_table_scan_init(void *&scan_ctx, uint n_threads) {
  auto bytes = (*THR_MALLOC)->Alloc(sizeof(Parallel_leader));
  if (!bytes) return (HA_ERR_OUT_OF_MEM);
  auto pq_leader = ::new (bytes) Parallel_leader(n_threads, ha_reverse_scan());

  pq_leader->key = MAX_KEY;

  void *range_start{nullptr};
  void *range_end{nullptr};
  PQ_Borders range_scan{range_start, range_end};
  PQ_Config config(range_scan, m_opened_table->rows());
  config.m_pq_reverse_scan = pq_leader->is_reverse();

  auto success = pq_leader->build_ranges(nullptr, config);
  if (!success) {
    return (HA_ERR_GENERIC);
  }

  // Here the pointer to the engine-specific data is stored into a generic
  // pointer, which will be passed back to the engine by future API calls.
  scan_ctx = pq_leader;
  return 0;
}

int Handler::pq_worker_scan_init(uint keyno MY_ATTRIBUTE((unused)),
                                 void *scan_ctx) {
  Parallel_leader *pq_leader = static_cast<Parallel_leader *>(scan_ctx);

  pq_worker = std::allocate_shared<Parallel_worker>(
      Mem_root_allocator<std::shared_ptr<Parallel_worker>>(*THR_MALLOC),
      pq_leader->is_reverse(), pq_leader->pq_slices_map, pq_leader->pq_key_map);

  pq_is_attach_ctx = false;
  inited = PQ;
  return false;
}

int Handler::pq_leader_scan_init(uint keyno, void *&scan_ctx, uint n_threads) {
  opened_table_validate();
  assert(n_threads > 0);

  scan_ctx = nullptr;

  /*
    Engine has no index scan (JT_INDEX) support (index_first() is not
    implemented). Engine has ref scan (JT_REF) support. But, division of index
    is not possible efficiently. Imagine we have to divide the range
    "derived.col_key=3". Because such index is either a std::multiset or
    std::unordered_multimap:

    - we don't have access to non-leaf levels to divide at their high level
    (like is done in InnoDB),
    - we could still divide between leaves, but then we would need
    constant-time iterator jumps (leader quickly jumping over N leaf index
    entries, to build a range of N such entries to give to one worker), alas
    the said jumps are linear-time, practically the leader would end up
    scanning all leaf entries matching value "3", just to build the ranges.
    Which is unacceptable.

    For unique index, this isn't an issue, as the range is one value only it
    does not have to be divided. However, in that case JT_EQ_REF is used,
    which isn't suitable for PQ either (see pq_not_support_scantype()). This
    especially means that the temporary table of "semi/anti join
    materialization lookup" will not do PQ; for "semi join materialization
    scan", it may do so.

    So, index cannot be used here. This is blocked by pq_not_support_scantype().

    Note that if the value to search for in the index is a constant (like if
    the equality in WHERE has "derived.col_key=3") then derived condition
    pushdown will sometimes move it to the derived table's definition, so the
    derived table will not be accessed by JT_REF actually, but probably by
    table scan, in which case it will possibly be a divided table.
  */
  if ((keyno != MAX_KEY) || pq_ref || (PQ_RANGE_SELECT == pq_range_type)) {
    assert(0);
    return HA_ERR_UNSUPPORTED;
  }

  handler::active_index = keyno;

  return pq_table_scan_init(scan_ctx, n_threads);
}

int Handler::pq_worker_scan_next(void *pq_leader_arg, uchar *buf) {
  // Similar to InnoDB's implementation
  int err{DB_SUCCESS_AS_IN_INNODB};
  assert(pq_leader_arg != nullptr);

  auto pq_leader = static_cast<Parallel_leader *>(pq_leader_arg);
  if (pq_leader->is_error_set()) return err;

retry:
  if (!pq_is_attach_ctx) {
    if (pq_ref_depend) {
      pq_ref_info.pq_ref_depend = pq_ref_depend;
      pq_ref_info.pq_ref_key_ptr = pq_ref_key.key;
      pq_ref_info.pq_ref_key_len = pq_ref_key.length;
    }
    if (
#ifndef NDEBUG
        !current_thd->pq_context().skip_fetch_ctx &&
#endif  // NDEBUG
        !pq_worker->dispatch_ctx(pq_ref_info, &cur_pq_ctx)) {
      err = DB_SUCCESS_AS_IN_INNODB;
      pq_is_attach_ctx = true;
    } else {
      err = DB_END_OF_INDEX_AS_IN_INNODB;
      goto end;
    }
  }

  {
    auto ctx = cur_pq_ctx;
    err = ctx->read_record(buf, this);
    if (err != DB_SUCCESS_AS_IN_INNODB) {
      if (err == DB_END_OF_INDEX_AS_IN_INNODB ||
          err == DB_END_OF_RANGE_AS_IN_INNODB) {
        pq_is_attach_ctx = false;
        goto retry;
      } else if (err == DB_NOT_FOUND_AS_IN_INNODB) {
        goto retry;
      } else if (!pq_leader->is_error_set()) {
        pq_leader->set_error_state(err);
      }
    }
  }

end:
  switch (err) {
    case DB_SUCCESS_AS_IN_INNODB:
      return 0;
    case DB_END_OF_INDEX_AS_IN_INNODB:
    case DB_END_OF_RANGE_AS_IN_INNODB:
      return HA_ERR_END_OF_FILE;
    default:
      return HA_ERR_INTERNAL_ERROR;
  }
}

int Handler::pq_leader_scan_end(void *scan_ctx) {
  handler::active_index = MAX_KEY;
  auto parallel_leader = static_cast<Parallel_leader *>(scan_ctx);
  parallel_leader->~Parallel_leader();
  return false;
}

int Handler::pq_worker_scan_end() {
  cur_pq_ctx = nullptr;
  pq_worker = nullptr;
  pq_ref_depend = false;
  pq_ref_info.pq_ref_depend = false;
  return 0;
}

template <typename CALLBACK>
void Storage::do_over_ranges_of_pages(size_t number_of_ranges,
                                      CALLBACK callback) const {
  if (m_number_of_elements == 0) return;
  assert(m_first_page != nullptr);

  size_t number_of_pages = m_number_of_elements / m_number_of_elements_per_page;
  if (number_of_pages * m_number_of_elements_per_page < m_number_of_elements)
    ++number_of_pages;  // ceiling
  assert(number_of_pages > 0);
  size_t pages_per_range = number_of_pages / number_of_ranges;
  if (pages_per_range * number_of_ranges < number_of_pages)
    ++pages_per_range;  // ceiling

  assert(m_first_page != nullptr);
  Page *cur_page = m_first_page;
  // compute rowid of first:
  Element *range_start = first_possible_element_on_page(cur_page);
  callback(range_start);  // register first range
  for (;;) {
    // find start of next range (== end of just-registered range)
    size_t jump;
    for (jump = pages_per_range; jump > 0; --jump) {
      if (cur_page == m_last_page) break;
      cur_page = *page_next_page_ptr(cur_page);  // get next page
    }
    if (jump > 0)  // we hit the table's end before the next range's start
    {
      break;
    }
    range_start = first_possible_element_on_page(cur_page);
    callback(range_start);
  }
}

} /* namespace temptable */
