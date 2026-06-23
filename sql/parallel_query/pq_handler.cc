/* Copyright (c) 2026, Oracle and/or its affiliates.

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

/**
  @file sql/parallel_query/pq_handler.cc
  Parallel Query handler/InnoDB contract implementation.

  This file keeps the 8.0.46-facing class names while restoring the commercial
  queue/ref-key dispatch semantics used by worker-side PQ execution.
*/

#include "sql/parallel_query/pq_handler.h"

#include <cassert>
#include <chrono>
#include <thread>

void PQ_Range::split() {
  if (m_scan_ctx == nullptr || m_iters.first == nullptr ||
      m_iters.second == nullptr || m_scan_ctx->reader() == nullptr) {
    return;
  }

  PQ_Borders scan_borders = {m_iters.first->get_border(),
                             m_iters.second->get_border()};

  m_scan_ctx->index_s_lock();

  PQ_Ranges ranges{};
  m_scan_ctx->partition(scan_borders, 1, ranges);

  m_scan_ctx->index_s_unlock();

  if (!ranges.empty()) {
    ranges.back()->m_iters.second = m_iters.second;
  }

  auto &config = m_scan_ctx->m_config;
  PQ_ref_key ref_key;
  if (config.m_ref_depend) {
    ref_key = PQ_ref_key(config.m_ref_key, config.m_ref_key_len);
  }

  auto *slices = m_scan_ctx->reader()->pq_slices_map[ref_key];
  assert(slices != nullptr);
  if (slices == nullptr) {
    return;
  }

  while (m_id != slices->m_spliting_id) {
    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }

  slices->prepare_split_slice(ranges, config.m_pq_reverse_scan);
}

void PQ_ranges_queue::mark_split(size_t n_slices, size_t n_threads,
                                 size_t &split_cur) {
  if (n_threads == 0 || m_elements.empty()) {
    return;
  }

  size_t n_split_slices = n_slices % n_threads;
  if (n_split_slices == 0 || m_elements.size() < n_split_slices) {
    return;
  }

  if (m_reverse) {
    auto it = m_elements.begin();
    while (n_split_slices > 0 && it != m_elements.end()) {
      (*it)->set_split(true);
      if (n_split_slices == 1) {
        split_cur = (*it)->id();
      }
      --n_split_slices;
      ++it;
    }
  } else {
    auto it = m_elements.rbegin();
    while (n_split_slices > 0 && it != m_elements.rend()) {
      (*it)->set_split(true);
      if (n_split_slices == 1) {
        split_cur = (*it)->id();
      }
      --n_split_slices;
      ++it;
    }
  }
}

PQ_slices_map::~PQ_slices_map() {
  std::lock_guard<std::mutex> guard(m_mutex);
  for (auto &entry : m_entries) {
    delete entry.second;
    entry.second = nullptr;
  }
  m_entries.clear();
}

std::shared_ptr<PQ_Ctx> PQ_Scan_ctx::create_context(
    std::shared_ptr<PQ_Range> &range) {
  auto ctx = make_ctx(range);
  if (ctx == nullptr) {
    return nullptr;
  }

#ifndef NDEBUG
  ctx->set_parent_id(range->id());
#endif
  ctx->reader = m_reader;
  return ctx;
}

size_t PQ_Scan_ctx::max_threads() const {
  return m_reader != nullptr ? m_reader->max_threads() : 0;
}

bool PQ_Ctx::check_ref_key(const PQ_Ref_info &ref_info) {
  auto &config = m_scan_ctx->m_config;
  if (ref_info.pq_ref_key_len != config.m_ref_key_len) {
    return false;
  }
  if (ref_info.pq_ref_key_len == 0) {
    return true;
  }
  if (ref_info.pq_ref_key_ptr == nullptr || config.m_ref_key == nullptr) {
    return false;
  }
  return std::memcmp(ref_info.pq_ref_key_ptr, config.m_ref_key,
                     ref_info.pq_ref_key_len) == 0;
}

PQ_Leader_context::PQ_Leader_context(size_t max_threads, bool reverse_scan)
    : m_max_threads(max_threads), m_reverse(reverse_scan) {
  PQ_ref_key null_key;
  auto *manager = new PQ_slices_manager(m_max_threads, m_reverse);
  pq_slices_map.insert(&null_key, manager);
  pq_key_map.insert(&null_key, true);
}

PQ_Leader_context::~PQ_Leader_context() {
  m_scan_ctxs.clear();
}

bool PQ_Leader_context::build_ranges(void *trx, const PQ_Config &config) {
  auto scan_ctx = make_scan_ctx(trx, config);
  if (scan_ctx == nullptr) {
    return false;
  }

  m_scan_ctxs.push_back(scan_ctx);
  scan_ctx->index_s_lock();
  ++m_scan_ctx_id;

  PQ_ref_key ref_key;
  if (config.m_ref_depend) {
    ref_key = PQ_ref_key(config.m_ref_key, config.m_ref_key_len);
    auto *manager = new PQ_slices_manager(m_max_threads, m_reverse);
    if (!pq_slices_map.insert(&ref_key, manager)) {
      delete manager;
    }
    pq_key_map.insert(&ref_key, true);
  }

  auto *slices = pq_slices_map[ref_key];
  assert(slices != nullptr);
  if (slices == nullptr) {
    scan_ctx->index_s_unlock();
    return false;
  }

  PQ_Ranges ranges{};
  scan_ctx->partition(config.m_scan_borders, 0, ranges);

  slices->push(ranges, true);
  slices->m_n_slices += ranges.size();
  slices->m_btree_depth = static_cast<uint>(scan_ctx->depth());

  scan_ctx->index_s_unlock();

  return true;
}

bool PQ_Worker_context::dispatch_ctx(
    const PQ_Ref_info &ref_info, std::shared_ptr<PQ_Ctx> *ctx) {
  for (;;) {
    auto next_ctx = get_ctx(ref_info);
    if (next_ctx == nullptr) {
      *ctx = nullptr;
      return true;
    }
    *ctx = next_ctx;
    return false;
  }
}

std::shared_ptr<PQ_Ctx> PQ_Worker_context::get_ctx(
    const PQ_Ref_info &ref_info) {
  std::shared_ptr<PQ_Ctx> ctx = nullptr;

  PQ_ref_key ref_key;
  if (ref_info.pq_ref_depend) {
    ref_key = PQ_ref_key(ref_info.pq_ref_key_ptr, ref_info.pq_ref_key_len);
  }

  if (!m_leader.pq_key_map[ref_key]) {
    return ctx;
  }

  for (;;) {
    auto *slices = m_leader.pq_slices_map[ref_key];
    assert(slices != nullptr);
    if (slices == nullptr) {
      return nullptr;
    }

    auto range = slices->fetch_one();
    if (range != nullptr && range->need_split()) {
      range->split();
      continue;
    }

    if (range != nullptr) {
      ctx = range->scan_ctx()->create_context(range);
      if (ctx != nullptr) {
        m_local_ctxs.enqueue(ctx);
      }
      return ctx;
    }

    for (;;) {
      ctx = m_local_ctxs.dequeue();
      if (ctx != nullptr && ref_info.pq_ref_depend) {
        if (!ctx->check_ref_key(ref_info)) {
          m_local_ctxs.enqueue(ctx);
          continue;
        }
        m_local_ctxs.enqueue(ctx);
        return ctx;
      }

      if (ctx == nullptr) {
        m_local_ctxs.rotate();
      } else {
        m_local_ctxs.enqueue(ctx);
      }
      return ctx;
    }
  }
}

bool Parallel_worker::dispatch_ctx(const PQ_Ref_info &ref_info,
                                   std::shared_ptr<PQ_Ctx_Base> *ctx) {
  for (;;) {
    auto next_ctx = get_ctx(ref_info);
    if (next_ctx == nullptr) {
      *ctx = nullptr;
      return true;
    }
    *ctx = next_ctx;
    return false;
  }
}

std::shared_ptr<PQ_Ctx_Base> Parallel_worker::get_ctx(
    const PQ_Ref_info &ref_info) {
  std::shared_ptr<PQ_Ctx_Base> ctx = nullptr;

  PQ_ref_key ref_key;
  if (ref_info.pq_ref_depend) {
    ref_key = PQ_ref_key(ref_info.pq_ref_key_ptr, ref_info.pq_ref_key_len);
  }

  if (!pq_key_map[ref_key]) {
    return ctx;
  }

  for (;;) {
    auto *slices = pq_slices_map[ref_key];
    assert(slices != nullptr);
    if (slices == nullptr) {
      return nullptr;
    }

    auto range = slices->fetch_one();
    if (range != nullptr && range->need_split()) {
      range->split();
      continue;
    }

    if (range != nullptr) {
      ctx = range->scan_ctx()->create_context(range);
      if (ctx != nullptr) {
        m_local_ctxs.enqueue(ctx);
      }
      return ctx;
    }

    for (;;) {
      ctx = m_local_ctxs.dequeue();
      if (ctx != nullptr && ref_info.pq_ref_depend) {
        if (!ctx->check_ref_key(ref_info)) {
          m_local_ctxs.enqueue(ctx);
          continue;
        }
        m_local_ctxs.enqueue(ctx);
        return ctx;
      }

      if (ctx == nullptr) {
        m_local_ctxs.rotate();
      } else {
        m_local_ctxs.enqueue(ctx);
      }
      return ctx;
    }
  }
}

const char *pq_worker_status_to_string(PQ_Worker_status status) {
  switch (status) {
    case PQ_Worker_status::NOT_STARTED:
      return "NOT_STARTED";
    case PQ_Worker_status::RUNNING:
      return "RUNNING";
    case PQ_Worker_status::FINISHED:
      return "FINISHED";
    case PQ_Worker_status::ERROR:
      return "ERROR";
    case PQ_Worker_status::KILLED:
      return "KILLED";
    case PQ_Worker_status::ABORTED:
      return "ABORTED";
    default:
      return "UNKNOWN";
  }
}
