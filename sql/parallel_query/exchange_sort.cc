/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in license.xml
   elsewhere in this distribution.  You may use this software under
   the terms of the GNU General Public License, version 2.0,
   or the terms of any other license that is available in this distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/parallel_query/exchange_sort.h"

#include <array>

namespace {

struct PQ_orderby_smoke_stream {
  const PQ_orderby_smoke_record *records{nullptr};
  uint32 count{0};
  uint32 pos{0};
};

struct PQ_orderby_smoke_merge_ctx {
  PQ_orderby_smoke_stream *streams{nullptr};
  bool descending{false};
};

struct PQ_orderby_cached_merge_ctx {
  PQ_orderby_record_batch *batches{nullptr};
  bool descending{false};
};

bool pq_orderby_smoke_compare_records(const PQ_orderby_smoke_record &left,
                                      const PQ_orderby_smoke_record &right,
                                      bool descending) {
  if (left.key != right.key) {
    return descending ? left.key > right.key : left.key < right.key;
  }

  if (left.row_id != right.row_id) return left.row_id < right.row_id;
  return left.worker_id < right.worker_id;
}

bool pq_orderby_smoke_compare_streams(int a, int b, void *arg) {
  auto *ctx = static_cast<PQ_orderby_smoke_merge_ctx *>(arg);
  const PQ_orderby_smoke_record &left =
      ctx->streams[a].records[ctx->streams[a].pos];
  const PQ_orderby_smoke_record &right =
      ctx->streams[b].records[ctx->streams[b].pos];
  return pq_orderby_smoke_compare_records(left, right, ctx->descending);
}

bool pq_orderby_cached_compare_records(const PQ_orderby_cached_record &left,
                                       const PQ_orderby_cached_record &right,
                                       bool descending) {
  const int cmp =
      left.sort_key == right.sort_key
          ? 0
          : (left.sort_key < right.sort_key ? -1 : 1);
  if (cmp != 0) return descending ? cmp > 0 : cmp < 0;

  if (left.row_id != right.row_id) return left.row_id < right.row_id;
  return left.worker_id < right.worker_id;
}

bool pq_orderby_cached_compare_batches(int a, int b, void *arg) {
  auto *ctx = static_cast<PQ_orderby_cached_merge_ctx *>(arg);
  const PQ_orderby_record_batch &left_batch = ctx->batches[a];
  const PQ_orderby_record_batch &right_batch = ctx->batches[b];
  const PQ_orderby_cached_record &left =
      left_batch.records[left_batch.next_pos];
  const PQ_orderby_cached_record &right =
      right_batch.records[right_batch.next_pos];
  return pq_orderby_cached_compare_records(left, right, ctx->descending);
}

bool pq_orderby_smoke_merge(PQ_orderby_smoke_stream *streams, uint32 nstreams,
                            bool descending,
                            const uint32 *expected_row_ids,
                            uint32 expected_count, uint32 *rows_read) {
  if (streams == nullptr || expected_row_ids == nullptr || rows_read == nullptr) {
    return true;
  }
  *rows_read = 0;

  PQ_orderby_smoke_merge_ctx ctx{streams, descending};
  binary_heap heap(static_cast<int>(nstreams), &ctx,
                   pq_orderby_smoke_compare_streams);
  if (heap.init_binary_heap()) return true;

  for (uint32 i = 0; i < nstreams; ++i) {
    streams[i].pos = 0;
    if (streams[i].records == nullptr || streams[i].count == 0) return true;
    if (heap.add_unordered(static_cast<int>(i))) return true;
  }
  heap.build();

  while (!heap.empty()) {
    const int stream_id = heap.first();
    PQ_orderby_smoke_stream &stream = streams[stream_id];
    const PQ_orderby_smoke_record &record = stream.records[stream.pos];

    if (*rows_read >= expected_count ||
        record.row_id != expected_row_ids[*rows_read]) {
      return true;
    }
    ++(*rows_read);
    ++stream.pos;

    if (stream.pos < stream.count) {
      heap.replace_first(stream_id);
    } else {
      heap.remove_first();
    }
  }

  return *rows_read != expected_count;
}

PQ_orderby_cached_record pq_make_cached_orderby_record(int64 key,
                                                       uint32 worker_id,
                                                       uint32 row_id,
                                                       int64 payload) {
  PQ_orderby_cached_record record;
  record.worker_id = worker_id;
  record.has_sort_key = true;
  record.sort_key.resize(sizeof(key));
  record.row_id.resize(sizeof(row_id));
  record.row_image.resize(sizeof(payload));
  memcpy(record.sort_key.data(), &key, sizeof(key));
  memcpy(record.row_id.data(), &row_id, sizeof(row_id));
  memcpy(record.row_image.data(), &payload, sizeof(payload));
  return record;
}

bool pq_orderby_cached_merge(PQ_orderby_record_batch *batches, uint32 nbatches,
                             bool descending,
                             const uint32 *expected_row_ids,
                             uint32 expected_count, uint32 *rows_read) {
  if (batches == nullptr || expected_row_ids == nullptr || rows_read == nullptr) {
    return true;
  }
  *rows_read = 0;

  PQ_orderby_cached_merge_ctx ctx{batches, descending};
  binary_heap heap(static_cast<int>(nbatches), &ctx,
                   pq_orderby_cached_compare_batches);
  if (heap.init_binary_heap()) return true;

  for (uint32 i = 0; i < nbatches; ++i) {
    batches[i].next_pos = 0;
    batches[i].completed = batches[i].records.empty();
    batches[i].compare_state = PQ_orderby_batch_compare_state::NOT_EVALUATED;
    if (!batches[i].completed && heap.add_unordered(static_cast<int>(i))) {
      return true;
    }
  }
  heap.build();

  while (!heap.empty()) {
    const int batch_id = heap.first();
    PQ_orderby_record_batch &batch = batches[batch_id];
    const PQ_orderby_cached_record &record = batch.records[batch.next_pos];

    uint32 row_id = 0;
    if (!record.has_sort_key || record.row_id.size() != sizeof(row_id) ||
        record.row_image.empty() || *rows_read >= expected_count) {
      return true;
    }
    memcpy(&row_id, record.row_id.data(), sizeof(row_id));
    if (row_id != expected_row_ids[*rows_read]) return true;

    ++(*rows_read);
    ++batch.next_pos;
    batch.new_group = batch.next_pos < batch.records.size();

    if (batch.next_pos < batch.records.size()) {
      heap.replace_first(batch_id);
    } else {
      batch.completed = true;
      heap.remove_first();
    }
  }

  return *rows_read != expected_count;
}

}  // namespace

bool Exchange_sort::read_mq_message(MQMessageType &type, void **datap,
                                    uint32 &data_len) {
  type = MQMessageType::FINISH;
  *datap = nullptr;
  data_len = 0;
  return false;
}

bool Exchange_sort::init_order_gather_shape(uint32 workers,
                                            bool stable_output,
                                            bool index_sort) {
  cleanup_order_gather_shape();
  if (workers == 0) return true;

  m_min_records.resize(workers);
  m_record_groups.resize(workers);
  m_order_shape_workers = workers;
  m_order_shape_stable_output = stable_output;
  m_order_shape_index_sort = index_sort;
  m_order_shape_initialized = true;
  return false;
}

bool Exchange_sort::read_ordered_record_shape() {
  return !m_order_shape_initialized;
}

void Exchange_sort::cleanup_order_gather_shape() {
  m_min_records.clear();
  m_record_groups.clear();
  m_order_heap = nullptr;
  m_order_shape_workers = 0;
  m_order_shape_initialized = false;
  m_order_shape_stable_output = false;
  m_order_shape_index_sort = false;
}

bool Exchange_sort::run_synthetic_order_merge_smoke(uint32 *rows_read) {
  if (rows_read == nullptr) return true;
  *rows_read = 0;

  constexpr std::array<PQ_orderby_smoke_record, 2> asc_worker0{{
      {1, 0, 10},
      {3, 0, 30},
  }};
  constexpr std::array<PQ_orderby_smoke_record, 2> asc_worker1{{
      {1, 1, 11},
      {2, 1, 20},
  }};
  constexpr std::array<PQ_orderby_smoke_record, 2> asc_worker2{{
      {2, 2, 21},
      {4, 2, 40},
  }};
  constexpr uint32 asc_expected[] = {10, 11, 20, 21, 30, 40};

  PQ_orderby_smoke_stream asc_streams[] = {
      {asc_worker0.data(), static_cast<uint32>(asc_worker0.size()), 0},
      {asc_worker1.data(), static_cast<uint32>(asc_worker1.size()), 0},
      {asc_worker2.data(), static_cast<uint32>(asc_worker2.size()), 0},
  };

  uint32 asc_rows = 0;
  if (pq_orderby_smoke_merge(asc_streams, 3, false, asc_expected,
                             static_cast<uint32>(std::size(asc_expected)),
                             &asc_rows)) {
    return true;
  }

  constexpr std::array<PQ_orderby_smoke_record, 2> desc_worker0{{
      {4, 0, 40},
      {2, 0, 21},
  }};
  constexpr std::array<PQ_orderby_smoke_record, 2> desc_worker1{{
      {3, 1, 30},
      {1, 1, 11},
  }};
  constexpr std::array<PQ_orderby_smoke_record, 2> desc_worker2{{
      {2, 2, 20},
      {1, 2, 10},
  }};
  constexpr uint32 desc_expected[] = {40, 30, 20, 21, 10, 11};

  PQ_orderby_smoke_stream desc_streams[] = {
      {desc_worker0.data(), static_cast<uint32>(desc_worker0.size()), 0},
      {desc_worker1.data(), static_cast<uint32>(desc_worker1.size()), 0},
      {desc_worker2.data(), static_cast<uint32>(desc_worker2.size()), 0},
  };

  uint32 desc_rows = 0;
  if (pq_orderby_smoke_merge(desc_streams, 3, true, desc_expected,
                             static_cast<uint32>(std::size(desc_expected)),
                             &desc_rows)) {
    return true;
  }

  *rows_read = asc_rows + desc_rows;
  return false;
}

bool Exchange_sort::run_cached_record_adapter_smoke(uint32 *rows_read) {
  if (rows_read == nullptr) return true;
  *rows_read = 0;

  PQ_orderby_record_batch asc_batches[3];
  asc_batches[0].records = {
      pq_make_cached_orderby_record(1, 0, 10, 100),
      pq_make_cached_orderby_record(3, 0, 30, 300),
  };
  asc_batches[1].records = {
      pq_make_cached_orderby_record(1, 1, 11, 110),
      pq_make_cached_orderby_record(2, 1, 20, 200),
  };
  asc_batches[2].records = {
      pq_make_cached_orderby_record(2, 2, 21, 210),
      pq_make_cached_orderby_record(4, 2, 40, 400),
  };
  constexpr uint32 asc_expected[] = {10, 11, 20, 21, 30, 40};

  uint32 asc_rows = 0;
  if (pq_orderby_cached_merge(asc_batches, 3, false, asc_expected,
                              static_cast<uint32>(std::size(asc_expected)),
                              &asc_rows)) {
    return true;
  }

  PQ_orderby_record_batch desc_batches[3];
  desc_batches[0].records = {
      pq_make_cached_orderby_record(4, 0, 40, 400),
      pq_make_cached_orderby_record(2, 0, 21, 210),
  };
  desc_batches[1].records = {
      pq_make_cached_orderby_record(3, 1, 30, 300),
      pq_make_cached_orderby_record(1, 1, 11, 110),
  };
  desc_batches[2].records = {
      pq_make_cached_orderby_record(2, 2, 20, 200),
      pq_make_cached_orderby_record(1, 2, 10, 100),
  };
  constexpr uint32 desc_expected[] = {40, 30, 20, 21, 10, 11};

  uint32 desc_rows = 0;
  if (pq_orderby_cached_merge(desc_batches, 3, true, desc_expected,
                              static_cast<uint32>(std::size(desc_expected)),
                              &desc_rows)) {
    return true;
  }

  *rows_read = asc_rows + desc_rows;
  return false;
}
