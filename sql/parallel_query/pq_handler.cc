/* Copyright (c) 2021, Huawei and/or its affiliates. All rights reserved.

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

#include "pq_handler.h"
#include <thread>  // std::this_thread::sleep_for
#include "my_dbug.h"

void PQ_Range::split() {
  /* Setup the sub-range. */
  PQ_Borders scan_borders = {m_iters.first->get_border(),
                             m_iters.second->get_border()};

  /* S lock so that the tree structure doesn't change while we are
  figuring out the sub-trees to scan. */
  m_scan_ctx->index_s_lock();

  Ranges ranges{};
  m_scan_ctx->partition(scan_borders, 1, ranges);
  /*
   * Need release index s lock as soon as finish block spliting, because it have
   * no necessary to keep lock later, otherwise it would block innodb purge
   * thread that want to fetch this index x lock, which may lead dead-wait in
   * some case.
   */
  m_scan_ctx->index_s_unlock();
  if (!ranges.empty()) {
    ranges.back()->m_iters.second = m_iters.second;
  }

  auto &config = m_scan_ctx->m_config;
  PQ_ref_key ref_key1;
  if (config.m_ref_depend) {
    new (&ref_key1) PQ_ref_key(config.m_ref_key, config.m_ref_key_len);
  }

  auto slices = m_scan_ctx->reader()->pq_slices_map[ref_key1];
  assert(slices != nullptr);

  /*
   We would like to split this range, which means replacing it by sub-ranges
   in the queue. But, if we're doing an ordered index scan, the respective order
   of the substituted sub-ranges with other ranges must be preserved (or the
   records output by a worker would not be ordered anymore). This is why we must
   wait until all previous ranges in the queue have been split (by other
   workers).
  */
  while (m_id != slices->m_spliting_id) {
    std::this_thread::sleep_for(std::chrono::microseconds(20 /*usecs*/));
  }

  /* prepare the slice of the queueu*/
  slices->prepare_split_slice(ranges, config.m_pq_reverse_scan);
}

void PQ_ranges_queue::mark_split(size_t n_slices, size_t n_threads,
                                 size_t &split_cur) {
  size_t n_split_slices = n_slices % n_threads;
  assert(m_elements.size() >= n_split_slices);

  if (m_reverse) {
    auto it = m_elements.begin();
    while (n_split_slices > 0 && it != m_elements.end()) {
      (*it)->set_split(true);
      // last splited context
      if (n_split_slices == 1) {
        split_cur = (*it)->id();
      }
      n_split_slices--;
      it++;
    }
  } else {
    auto r_it = m_elements.rbegin();
    while (n_split_slices > 0 && r_it != m_elements.rend()) {
      (*r_it)->set_split(true);
      if (n_split_slices == 1) {
        split_cur = (*r_it)->id();
      }
      n_split_slices--;
      r_it++;
    }
  }
}

size_t PQ_Scan_ctx_Base::max_threads() const {
  return (m_reader->max_threads());
}

std::shared_ptr<PQ_Ctx_Base> PQ_Scan_ctx_Base::create_context(
    std::shared_ptr<PQ_Range> &range) {
  auto ctx = make_ctx(range);

  if (ctx.get() == nullptr) {
    return nullptr;
  } else {
#ifdef UNIV_DEBUG
    ctx->set_parent_id(range->id());
#endif
    ctx->reader = m_reader;
  }
  return (ctx);
}

bool PQ_Ctx_Base::check_ref_key(const PQ_Ref_info &ref_info) {
  auto &config = m_scan_ctx->m_config;
  if (ref_info.pq_ref_key_len != config.m_ref_key_len) {
    return false;
  } else {
    return memcmp(ref_info.pq_ref_key_ptr, config.m_ref_key,
                  ref_info.pq_ref_key_len) == 0;
  }
}

Parallel_leader_Base::Parallel_leader_Base(size_t max_threads,
                                           bool reverse_scan)
    : m_scan_ctxs(), m_max_threads(max_threads), m_reverse(reverse_scan) {
  PQ_ref_key null_key;
  auto *mngr =
      new PQ_slices_mngr<PQ_Range, PQ_ranges_queue>(m_max_threads, m_reverse);
  // null_key's slot used to store default slices, such as table/index scan,
  // range/ref const scan.
  pq_slices_map.insert(&null_key, mngr);
  pq_key_map.insert(&null_key, true);
}

Parallel_leader_Base::~Parallel_leader_Base() {
  m_scan_ctxs.clear();
  for (int i = 0; i < PQ_slices_map::MAX_CELLS; i++) {
    Slice_map_cell::KV *kv = pq_slices_map.m_elements[i].head;
    while (kv) {
      delete kv->value;
      kv->value = nullptr;
      kv = kv->next;
    }
  }
}

bool Parallel_leader_Base::build_ranges(void *trx,
                                        const PQ_Config_Base &config) {
  auto scan_ctx = make_scan_ctx(trx, config);
  if (scan_ctx.get() == nullptr) {
    return (false);
  }

  m_scan_ctxs.push_back(scan_ctx);
  scan_ctx->index_s_lock();
  ++m_scan_ctx_id;

  PQ_ref_key ref_key1;
  // using for "ref field" scan, which would be many different ref key slices
  // and need to store in different queues.
  if (config.m_ref_depend) {
    new (&ref_key1) PQ_ref_key(config.m_ref_key, config.m_ref_key_len);
    auto *slices =
        new PQ_slices_mngr<PQ_Range, PQ_ranges_queue>(m_max_threads, m_reverse);
    if (!pq_slices_map.insert(&ref_key1, slices)) delete slices;
  }

  auto slices = pq_slices_map[ref_key1];
  assert(slices != nullptr);

  /* Split at the root node (level == 0). */
  Ranges ranges{};
  scan_ctx->partition(config.m_scan_borders, 0, ranges);

  slices->push(ranges, true);
  slices->m_n_slices += ranges.size();
  slices->m_btree_depth = scan_ctx->depth();

  scan_ctx->index_s_unlock();

  return true;
}

bool Parallel_worker::dispatch_ctx(const PQ_Ref_info &ref_info,
                                   std::shared_ptr<PQ_Ctx_Base> *out_ctx) {
  for (;;) {
    auto ctx = get_ctx(ref_info);

    if (ctx == nullptr) {
      *out_ctx = nullptr;
      return true;
    } else {
      *out_ctx = ctx;
      break;
    }
  }

  return false;
}

std::shared_ptr<PQ_Ctx_Base> Parallel_worker::get_ctx(
    const PQ_Ref_info &ref_info) {
  std::shared_ptr<PQ_Ctx_Base> ctx = nullptr;

  PQ_ref_key ref_key1;
  // if pq_ref_depend is true, it must be "ref field" scan.
  if (ref_info.pq_ref_depend)
    new (&ref_key1)
        PQ_ref_key(ref_info.pq_ref_key_ptr, ref_info.pq_ref_key_len);

  if (!pq_key_map[ref_key1]) return ctx;

  for (;;) {
    auto slices = pq_slices_map[ref_key1];
    assert(slices != nullptr);
    // Firstly fetch from leaders' global slices queue
    auto range = slices->fetch_one();
    if (range && range->need_split()) {
      range->split();
      continue;
    } else if (range) {
      ctx = range->scan_ctx()->create_context(range);
      // Store ctx into local queue, which will fetch again in later round
      m_local_ctxs.enqueue(ctx);
      range = nullptr;
      break;
    }

  local_fetch:
    ctx = m_local_ctxs.dequeue();
    if (ctx != nullptr && ref_info.pq_ref_depend) {
      // There would be differents ref key's slices in one queue, so need to
      // check "ref key" to find exact one.
      if (!ctx->check_ref_key(ref_info)) {
        m_local_ctxs.enqueue(ctx);
        // skip unmatched slices
        goto local_fetch;
      } else {
        m_local_ctxs.enqueue(ctx);
        break;
      }
    } else if (ctx == nullptr) {
      // because slices queue could fetch many rounds, so need rotate it in a
      // round end.
      m_local_ctxs.rotate();
      break;
    } else {
      m_local_ctxs.enqueue(ctx);
      break;
    }
  }

  return ctx;
}
