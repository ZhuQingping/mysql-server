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
#include <cstring>
#include <utility>

#include "sql/field.h"
#include "sql/table.h"

namespace {

static_assert(sizeof(PQ_orderby_frame_header) == 28,
              "PQ ORDER BY frame header must stay wire-stable");

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

bool pq_copy_decoded_orderby_frame(const PQ_orderby_decoded_frame &decoded,
                                   uint32 worker_id,
                                   PQ_orderby_cached_record *record) {
  if (record == nullptr || decoded.type != PQ_orderby_frame_type::ROW ||
      decoded.record_image == nullptr || decoded.record_image_len == 0 ||
      decoded.row_id == nullptr || decoded.row_id_len == 0 ||
      decoded.sort_key == nullptr || decoded.sort_key_len == 0) {
    return true;
  }

  record->worker_id = worker_id;
  record->has_sort_key = true;
  record->row_image.assign(decoded.record_image,
                           decoded.record_image + decoded.record_image_len);
  record->row_id.assign(decoded.row_id, decoded.row_id + decoded.row_id_len);
  record->sort_key.assign(decoded.sort_key,
                          decoded.sort_key + decoded.sort_key_len);
  return false;
}

uint16 pq_orderby_frame_type_to_uint(PQ_orderby_frame_type type) {
  return static_cast<uint16>(type);
}

bool pq_is_valid_orderby_frame_type(uint16 type) {
  return type >= static_cast<uint16>(PQ_orderby_frame_type::ROW) &&
         type <= static_cast<uint16>(PQ_orderby_frame_type::ERROR);
}

bool pq_send_orderby_frame(MQueue_handle *handle, PQ_orderby_frame_type type,
                           const void *record_image, uint32 record_image_len,
                           const void *row_id, uint32 row_id_len,
                           const void *sort_key, uint32 sort_key_len,
                           uint32 flags = 0) {
  if (handle == nullptr) return true;
  if ((record_image_len > 0 && record_image == nullptr) ||
      (row_id_len > 0 && row_id == nullptr) ||
      (sort_key_len > 0 && sort_key == nullptr)) {
    return true;
  }
  if (type != PQ_orderby_frame_type::ROW &&
      (record_image_len != 0 || row_id_len != 0 || sort_key_len != 0)) {
    return true;
  }

  const uint64 payload_len64 = static_cast<uint64>(record_image_len) +
                               row_id_len + sort_key_len;
  if (payload_len64 > UINT32_MAX) return true;
  const uint32 payload_len = static_cast<uint32>(payload_len64);
  const uint64 total_len64 =
      static_cast<uint64>(sizeof(PQ_orderby_frame_header)) + payload_len;
  if (total_len64 > UINT32_MAX) return true;
  const uint32 total_len = static_cast<uint32>(total_len64);

  std::vector<uchar> message(total_len);
  auto *header = reinterpret_cast<PQ_orderby_frame_header *>(message.data());
  header->magic = PQ_ORDERBY_FRAME_MAGIC;
  header->version = PQ_ORDERBY_FRAME_VERSION;
  header->type = pq_orderby_frame_type_to_uint(type);
  header->flags = flags;
  header->record_image_len = record_image_len;
  header->row_id_len = row_id_len;
  header->sort_key_len = sort_key_len;
  header->payload_len = payload_len;

  uchar *payload = message.data() + sizeof(PQ_orderby_frame_header);
  if (record_image_len > 0) {
    memcpy(payload, record_image, record_image_len);
    payload += record_image_len;
  }
  if (row_id_len > 0) {
    memcpy(payload, row_id, row_id_len);
    payload += row_id_len;
  }
  if (sort_key_len > 0) {
    memcpy(payload, sort_key, sort_key_len);
  }

  return handle->send(message.data(), total_len) != MQ_SUCCESS;
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

bool pq_load_orderby_frame_batch(MQueue_handle *handle, uint32 worker_id,
                                 PQ_orderby_record_batch *batch,
                                 uint32 *finishes_read,
                                 uint32 *errors_read = nullptr) {
  if (handle == nullptr || batch == nullptr || finishes_read == nullptr) {
    return true;
  }

  batch->records.clear();
  batch->next_pos = 0;
  batch->completed = false;
  batch->new_group = false;
  batch->compare_state = PQ_orderby_batch_compare_state::NOT_EVALUATED;

  for (;;) {
    void *raw_data = nullptr;
    uint32 raw_len = 0;
    const MQ_RESULT result = handle->receive(&raw_data, &raw_len);
    if (result != MQ_SUCCESS) return true;

    PQ_orderby_decoded_frame decoded;
    if (pq_decode_orderby_frame(raw_data, raw_len, &decoded)) return true;

    if (decoded.type == PQ_orderby_frame_type::ROW) {
      PQ_orderby_cached_record record;
      if (pq_copy_decoded_orderby_frame(decoded, worker_id, &record)) {
        return true;
      }
      batch->records.push_back(std::move(record));
      continue;
    }

    if (decoded.type == PQ_orderby_frame_type::FINISH) {
      batch->completed = true;
      ++(*finishes_read);
      return false;
    }

    if (decoded.type == PQ_orderby_frame_type::ERROR && errors_read != nullptr) {
      ++(*errors_read);
    }
    return true;
  }
}

bool pq_orderby_materialization_table_supported(TABLE *table) {
  return table != nullptr && table->s != nullptr && table->record[0] != nullptr &&
         table->s->reclength > 0 && table->s->fields > 0 &&
         table->field != nullptr && table->field[0] != nullptr &&
         (table->field[0]->type() == MYSQL_TYPE_LONG ||
          table->field[0]->type() == MYSQL_TYPE_LONGLONG);
}

}  // namespace

bool pq_validate_orderby_frame(const void *raw_data, uint32 raw_len,
                               const PQ_orderby_frame_header **header,
                               const uchar **payload) {
  if (raw_data == nullptr || header == nullptr || payload == nullptr) {
    return true;
  }
  *header = nullptr;
  *payload = nullptr;
  if (raw_len < sizeof(PQ_orderby_frame_header)) return true;

  auto *frame = static_cast<const PQ_orderby_frame_header *>(raw_data);
  if (frame->magic != PQ_ORDERBY_FRAME_MAGIC ||
      frame->version != PQ_ORDERBY_FRAME_VERSION ||
      !pq_is_valid_orderby_frame_type(frame->type)) {
    return true;
  }

  const uint64 expected_payload_len =
      static_cast<uint64>(frame->record_image_len) + frame->row_id_len +
      frame->sort_key_len;
  if (expected_payload_len > UINT32_MAX ||
      expected_payload_len != frame->payload_len) {
    return true;
  }
  const uint64 expected_len =
      static_cast<uint64>(sizeof(PQ_orderby_frame_header)) +
      frame->payload_len;
  if (expected_len > UINT32_MAX || expected_len != raw_len) return true;

  if (frame->type != static_cast<uint16>(PQ_orderby_frame_type::ROW) &&
      frame->payload_len != 0) {
    return true;
  }

  *header = frame;
  *payload = frame->payload_len == 0
                 ? nullptr
                 : static_cast<const uchar *>(raw_data) +
                       sizeof(PQ_orderby_frame_header);
  return false;
}

bool pq_decode_orderby_frame(const void *raw_data, uint32 raw_len,
                             PQ_orderby_decoded_frame *decoded) {
  if (decoded == nullptr) return true;

  const PQ_orderby_frame_header *header = nullptr;
  const uchar *payload = nullptr;
  if (pq_validate_orderby_frame(raw_data, raw_len, &header, &payload)) {
    return true;
  }

  decoded->type = static_cast<PQ_orderby_frame_type>(header->type);
  decoded->record_image = nullptr;
  decoded->record_image_len = header->record_image_len;
  decoded->row_id = nullptr;
  decoded->row_id_len = header->row_id_len;
  decoded->sort_key = nullptr;
  decoded->sort_key_len = header->sort_key_len;

  if (decoded->type == PQ_orderby_frame_type::ROW) {
    if (payload == nullptr || header->record_image_len == 0) return true;
    decoded->record_image = payload;
    decoded->row_id = payload + header->record_image_len;
    decoded->sort_key = decoded->row_id + header->row_id_len;
  }
  return false;
}

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

bool Exchange_sort::init_sort_state_shape(uint32 workers, bool stable_output,
                                          bool index_sort,
                                          uint32 sort_order_length,
                                          uint32 max_record_length,
                                          uint32 ref_length) {
  cleanup_sort_state_shape();
  if (workers == 0 || sort_order_length == 0 || max_record_length == 0) {
    return true;
  }
  if (stable_output && ref_length == 0) return true;

  m_sort_state_shape.workers = workers;
  m_sort_state_shape.sort_order_length = sort_order_length;
  m_sort_state_shape.max_record_length = max_record_length;
  m_sort_state_shape.ref_length = ref_length;
  m_sort_state_shape.stable_output = stable_output;
  m_sort_state_shape.index_sort = index_sort;
  m_sort_state_shape.rowid_required = stable_output;
  m_sort_state_shape.initialized = true;
  return false;
}

void Exchange_sort::cleanup_sort_state_shape() {
  m_sort_state_shape = PQ_orderby_sort_state_shape{};
}

bool Exchange_sort::init_real_init_state_owner_shape(
    uint32 workers, bool stable_output, bool index_sort,
    uint32 sort_order_length, uint32 max_record_length, uint32 ref_length) {
  cleanup_real_init_state_owner_shape();
  if (workers == 0 || sort_order_length == 0 || max_record_length == 0) {
    return true;
  }
  if (stable_output && ref_length == 0) return true;
  if (max_record_length == UINT32_MAX) return true;

  m_real_init_state_shape.workers = workers;
  m_real_init_state_shape.sort_order_length = sort_order_length;
  m_real_init_state_shape.max_record_length = max_record_length;
  m_real_init_state_shape.ref_length = ref_length;
  m_real_init_state_shape.compare_key_buffer_length = max_record_length + 1;
  m_real_init_state_shape.tmp_key_buffer_length =
      stable_output ? ref_length : 0;
  m_real_init_state_shape.min_record_slots = workers;
  m_real_init_state_shape.record_group_slots = workers;
  m_real_init_state_shape.stable_output = stable_output;
  m_real_init_state_shape.index_sort = index_sort;
  m_real_init_state_shape.rowid_required = stable_output;
  m_real_init_state_shape.initialized = true;
  return false;
}

bool Exchange_sort::allocate_real_init_buffers_shape() {
  m_compare_key_buffers[0].clear();
  m_compare_key_buffers[1].clear();
  m_tmp_key_buffer.clear();
  m_min_records.clear();
  m_record_groups.clear();

  if (!m_real_init_state_shape.initialized ||
      m_real_init_state_shape.workers == 0 ||
      m_real_init_state_shape.compare_key_buffer_length == 0 ||
      m_real_init_state_shape.max_record_length == UINT32_MAX ||
      m_real_init_state_shape.compare_key_buffer_length !=
          m_real_init_state_shape.max_record_length + 1 ||
      m_real_init_state_shape.tmp_key_buffer_length !=
          (m_real_init_state_shape.rowid_required
               ? m_real_init_state_shape.ref_length
               : 0) ||
      m_real_init_state_shape.min_record_slots !=
          m_real_init_state_shape.workers ||
      m_real_init_state_shape.record_group_slots !=
          m_real_init_state_shape.workers) {
    return true;
  }
  if (m_real_init_state_shape.rowid_required &&
      m_real_init_state_shape.tmp_key_buffer_length == 0) {
    return true;
  }

  m_compare_key_buffers[0].assign(
      m_real_init_state_shape.compare_key_buffer_length, 0);
  m_compare_key_buffers[1].assign(
      m_real_init_state_shape.compare_key_buffer_length, 0);
  m_tmp_key_buffer.assign(m_real_init_state_shape.tmp_key_buffer_length, 0);
  m_min_records.resize(m_real_init_state_shape.min_record_slots);
  m_record_groups.resize(m_real_init_state_shape.record_group_slots);

  return m_compare_key_buffers[0].size() !=
             m_real_init_state_shape.compare_key_buffer_length ||
         m_compare_key_buffers[1].size() !=
             m_real_init_state_shape.compare_key_buffer_length ||
         m_tmp_key_buffer.size() !=
             m_real_init_state_shape.tmp_key_buffer_length ||
         m_min_records.size() != m_real_init_state_shape.min_record_slots ||
         m_record_groups.size() != m_real_init_state_shape.record_group_slots;
}

bool Exchange_sort::load_orderby_frame_to_record_group(
    MQueue_handle *handle, uint32 worker_id,
    PQ_orderby_loader_status *status) {
  if (status == nullptr) return true;
  *status = PQ_orderby_loader_status::ERROR;
  if (handle == nullptr || worker_id >= m_record_groups.size()) return true;

  void *raw_data = nullptr;
  uint32 raw_len = 0;
  const MQ_RESULT result = handle->receive(&raw_data, &raw_len);
  if (result == MQ_WOULD_BLOCK) {
    *status = PQ_orderby_loader_status::WOULD_BLOCK;
    return false;
  }
  if (result == MQ_DETACHED) {
    *status = PQ_orderby_loader_status::ERROR;
    return false;
  }
  if (result != MQ_SUCCESS) return true;

  PQ_orderby_decoded_frame decoded;
  if (pq_decode_orderby_frame(raw_data, raw_len, &decoded)) {
    *status = PQ_orderby_loader_status::ERROR;
    return false;
  }

  PQ_orderby_record_batch &batch = m_record_groups[worker_id];
  batch.compare_state = PQ_orderby_batch_compare_state::NOT_EVALUATED;

  if (decoded.type == PQ_orderby_frame_type::ROW) {
    PQ_orderby_cached_record record;
    if (pq_copy_decoded_orderby_frame(decoded, worker_id, &record)) {
      *status = PQ_orderby_loader_status::ERROR;
      return false;
    }
    batch.records.push_back(std::move(record));
    batch.completed = false;
    batch.new_group = true;
    *status = PQ_orderby_loader_status::ROW;
    return false;
  }

  if (decoded.type == PQ_orderby_frame_type::FINISH) {
    batch.completed = true;
    *status = PQ_orderby_loader_status::FINISH;
    return false;
  }

  *status = PQ_orderby_loader_status::ERROR;
  return false;
}

void Exchange_sort::cleanup_real_init_state_owner_shape() {
  m_compare_key_buffers[0].clear();
  m_compare_key_buffers[1].clear();
  m_tmp_key_buffer.clear();
  m_real_init_state_shape = PQ_orderby_real_init_state_shape{};
}

void Exchange_sort::cleanup_order_gather_shape() {
  m_min_records.clear();
  m_record_groups.clear();
  m_order_heap = nullptr;
  m_order_shape_workers = 0;
  m_order_shape_initialized = false;
  m_order_shape_stable_output = false;
  m_order_shape_index_sort = false;
  cleanup_sort_state_shape();
  cleanup_real_init_state_owner_shape();
}

bool Exchange_sort::run_orderby_sort_state_shape_smoke() {
  constexpr uint32 kWorkers = 3;
  constexpr uint32 kSortOrderLength = 2;
  constexpr uint32 kMaxRecordLength = 64;
  constexpr uint32 kRefLength = 8;

  if (run_orderby_sort_state_shape_handoff_smoke(
      kWorkers, /*stable_output=*/true, /*index_sort=*/false, kSortOrderLength,
      kMaxRecordLength, kRefLength)) {
    return true;
  }

  if (run_orderby_real_init_state_owner_smoke()) {
    return true;
  }

  if (run_orderby_real_init_allocation_smoke()) {
    return true;
  }

  return run_orderby_frame_loader_smoke();
}

bool Exchange_sort::run_orderby_sort_state_shape_handoff_smoke(
    uint32 workers, bool stable_output, bool index_sort,
    uint32 sort_order_length, uint32 max_record_length, uint32 ref_length) {
  if (init_sort_state_shape(workers, stable_output, index_sort,
                            sort_order_length, max_record_length, ref_length)) {
    cleanup_sort_state_shape();
    return true;
  }
  const bool valid =
      m_sort_state_shape.initialized &&
      m_sort_state_shape.workers == workers &&
      m_sort_state_shape.sort_order_length == sort_order_length &&
      m_sort_state_shape.max_record_length == max_record_length &&
      m_sort_state_shape.ref_length == ref_length &&
      m_sort_state_shape.stable_output == stable_output &&
      m_sort_state_shape.index_sort == index_sort &&
      m_sort_state_shape.rowid_required == stable_output;
  cleanup_sort_state_shape();
  return !valid;
}

bool Exchange_sort::run_orderby_real_init_state_owner_smoke() {
  constexpr uint32 kWorkers = 3;
  constexpr uint32 kSortOrderLength = 2;
  constexpr uint32 kMaxRecordLength = 64;
  constexpr uint32 kRefLength = 8;

  if (init_real_init_state_owner_shape(
          kWorkers, /*stable_output=*/true, /*index_sort=*/false,
          kSortOrderLength, kMaxRecordLength, kRefLength)) {
    cleanup_real_init_state_owner_shape();
    return true;
  }

  const bool valid =
      m_real_init_state_shape.initialized &&
      m_real_init_state_shape.workers == kWorkers &&
      m_real_init_state_shape.sort_order_length == kSortOrderLength &&
      m_real_init_state_shape.max_record_length == kMaxRecordLength &&
      m_real_init_state_shape.ref_length == kRefLength &&
      m_real_init_state_shape.compare_key_buffer_length ==
          kMaxRecordLength + 1 &&
      m_real_init_state_shape.tmp_key_buffer_length == kRefLength &&
      m_real_init_state_shape.min_record_slots == kWorkers &&
      m_real_init_state_shape.record_group_slots == kWorkers &&
      m_real_init_state_shape.stable_output &&
      !m_real_init_state_shape.index_sort &&
      m_real_init_state_shape.rowid_required;

  cleanup_real_init_state_owner_shape();
  if (!valid || m_real_init_state_shape.initialized ||
      m_real_init_state_shape.workers != 0 ||
      m_real_init_state_shape.compare_key_buffer_length != 0 ||
      m_real_init_state_shape.tmp_key_buffer_length != 0) {
    return true;
  }
  return false;
}

bool Exchange_sort::run_orderby_real_init_allocation_smoke() {
  constexpr uint32 kWorkers = 3;
  constexpr uint32 kSortOrderLength = 2;
  constexpr uint32 kMaxRecordLength = 64;
  constexpr uint32 kRefLength = 8;

  if (init_real_init_state_owner_shape(
          kWorkers, /*stable_output=*/true, /*index_sort=*/false,
          kSortOrderLength, kMaxRecordLength, kRefLength) ||
      allocate_real_init_buffers_shape()) {
    cleanup_order_gather_shape();
    return true;
  }

  const bool valid =
      m_compare_key_buffers[0].size() == kMaxRecordLength + 1 &&
      m_compare_key_buffers[1].size() == kMaxRecordLength + 1 &&
      m_tmp_key_buffer.size() == kRefLength &&
      m_min_records.size() == kWorkers && m_record_groups.size() == kWorkers &&
      m_real_init_state_shape.initialized &&
      m_real_init_state_shape.compare_key_buffer_length == kMaxRecordLength + 1;

  cleanup_order_gather_shape();
  if (!valid || !m_compare_key_buffers[0].empty() ||
      !m_compare_key_buffers[1].empty() || !m_tmp_key_buffer.empty() ||
      !m_min_records.empty() || !m_record_groups.empty() ||
      m_real_init_state_shape.initialized) {
    return true;
  }

  if (init_real_init_state_owner_shape(
          kWorkers, /*stable_output=*/true, /*index_sort=*/false,
          kSortOrderLength, kMaxRecordLength, kRefLength)) {
    cleanup_order_gather_shape();
    return true;
  }
  m_real_init_state_shape.compare_key_buffer_length = kMaxRecordLength;
  if (!allocate_real_init_buffers_shape() ||
      !m_compare_key_buffers[0].empty() || !m_compare_key_buffers[1].empty() ||
      !m_tmp_key_buffer.empty() || !m_min_records.empty() ||
      !m_record_groups.empty()) {
    cleanup_order_gather_shape();
    return true;
  }
  cleanup_order_gather_shape();

  return false;
}

bool Exchange_sort::run_orderby_frame_loader_smoke() {
  constexpr uint32 kWorkers = 3;
  constexpr uint32 kSortOrderLength = 2;
  constexpr uint32 kMaxRecordLength = 64;
  constexpr uint32 kRefLength = 8;

  bool initialized_here = false;
  if (m_mq_handles == nullptr) {
    if (init()) return true;
    initialized_here = true;
  }
  if (m_nqueues != kWorkers) {
    if (initialized_here) cleanup();
    return true;
  }

  bool failed =
      init_real_init_state_owner_shape(kWorkers, /*stable_output=*/true,
                                       /*index_sort=*/false, kSortOrderLength,
                                       kMaxRecordLength, kRefLength) ||
      allocate_real_init_buffers_shape();

  const int64 record10 = 100;
  const int64 record20 = 200;
  const uint32 rowid10 = 10;
  const uint32 rowid20 = 20;
  const int64 key1 = 1;
  const int64 key2 = 2;
  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                                   &record10, sizeof(record10), &rowid10,
                                   sizeof(rowid10), &key1, sizeof(key1)) ||
             pq_send_orderby_frame(get_mq_handle(0),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             pq_send_orderby_frame(get_mq_handle(1),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             pq_send_orderby_frame(get_mq_handle(2), PQ_orderby_frame_type::ROW,
                                   &record20, sizeof(record20), &rowid20,
                                   sizeof(rowid20), &key2, sizeof(key2)) ||
             pq_send_orderby_frame(get_mq_handle(2),
                                   PQ_orderby_frame_type::ERROR, nullptr, 0,
                                   nullptr, 0, nullptr, 0);
  }

  PQ_orderby_loader_status status = PQ_orderby_loader_status::ERROR;
  if (!failed) {
    failed = load_orderby_frame_to_record_group(get_mq_handle(0), 0, &status) ||
             status != PQ_orderby_loader_status::ROW ||
             m_record_groups[0].records.size() != 1;
  }
  if (!failed) {
    const PQ_orderby_cached_record &record = m_record_groups[0].records[0];
    int64 payload = 0;
    uint32 rowid = 0;
    int64 key = 0;
    failed = record.row_image.size() != sizeof(payload) ||
             record.row_id.size() != sizeof(rowid) ||
             record.sort_key.size() != sizeof(key) || !record.has_sort_key;
    if (!failed) {
      memcpy(&payload, record.row_image.data(), sizeof(payload));
      memcpy(&rowid, record.row_id.data(), sizeof(rowid));
      memcpy(&key, record.sort_key.data(), sizeof(key));
      failed = payload != record10 || rowid != rowid10 || key != key1;
    }
  }
  if (!failed) {
    failed = load_orderby_frame_to_record_group(get_mq_handle(0), 0, &status) ||
             status != PQ_orderby_loader_status::FINISH ||
             !m_record_groups[0].completed;
  }
  if (!failed) {
    failed = load_orderby_frame_to_record_group(get_mq_handle(1), 1, &status) ||
             status != PQ_orderby_loader_status::FINISH ||
             !m_record_groups[1].completed ||
             !m_record_groups[1].records.empty();
  }
  if (!failed) {
    failed = load_orderby_frame_to_record_group(get_mq_handle(2), 2, &status) ||
             status != PQ_orderby_loader_status::ROW ||
             m_record_groups[2].records.size() != 1;
  }
  if (!failed) {
    failed = load_orderby_frame_to_record_group(get_mq_handle(2), 2, &status) ||
             status != PQ_orderby_loader_status::ERROR;
  }
  if (!failed) {
    failed = load_orderby_frame_to_record_group(get_mq_handle(1), 1, &status) ||
             status != PQ_orderby_loader_status::WOULD_BLOCK ||
             !m_record_groups[1].records.empty();
  }
  if (!failed) {
    m_record_groups[1].completed = false;
    get_mq_handle(1)->close_producer();
    failed = load_orderby_frame_to_record_group(get_mq_handle(1), 1, &status) ||
             status != PQ_orderby_loader_status::ERROR ||
             m_record_groups[1].completed ||
             !m_record_groups[1].records.empty();
  }

  cleanup_order_gather_shape();
  failed = failed || !m_min_records.empty() || !m_record_groups.empty() ||
           !m_compare_key_buffers[0].empty() ||
           !m_compare_key_buffers[1].empty() || !m_tmp_key_buffer.empty();

  if (initialized_here) cleanup();
  return failed;
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

bool Exchange_sort::run_orderby_frame_contract_smoke(uint32 *rows_read,
                                                     uint32 *finishes_read,
                                                     uint32 *errors_read) {
  if (rows_read == nullptr || finishes_read == nullptr ||
      errors_read == nullptr) {
    return true;
  }
  *rows_read = 0;
  *finishes_read = 0;
  *errors_read = 0;

  PQ_mq_event sender_event;
  PQ_mq_event receiver_event;
  char ring[PQ_MQ_DEFAULT_RING_SIZE];
  MQueue queue(&sender_event, &receiver_event, ring, sizeof(ring));
  MQueue_handle handle(&queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
  if (handle.init()) return true;

  const int64 record0 = 100;
  const int64 record1 = 200;
  const uint32 rowid0 = 10;
  const uint32 rowid1 = 20;
  const int64 sortkey0 = 1;
  const int64 sortkey1 = 2;

  bool failed =
      pq_send_orderby_frame(&handle, PQ_orderby_frame_type::ROW, &record0,
                            sizeof(record0), &rowid0, sizeof(rowid0),
                            &sortkey0, sizeof(sortkey0)) ||
      pq_send_orderby_frame(&handle, PQ_orderby_frame_type::ROW, &record1,
                            sizeof(record1), &rowid1, sizeof(rowid1),
                            &sortkey1, sizeof(sortkey1)) ||
      pq_send_orderby_frame(&handle, PQ_orderby_frame_type::FINISH, nullptr, 0,
                            nullptr, 0, nullptr, 0) ||
      pq_send_orderby_frame(&handle, PQ_orderby_frame_type::ERROR, nullptr, 0,
                            nullptr, 0, nullptr, 0);

  for (uint32 i = 0; !failed && i < 4; ++i) {
    void *raw_data = nullptr;
    uint32 raw_len = 0;
    if (handle.receive(&raw_data, &raw_len) != MQ_SUCCESS) {
      failed = true;
      break;
    }

    PQ_orderby_decoded_frame decoded;
    if (pq_decode_orderby_frame(raw_data, raw_len, &decoded)) {
      failed = true;
      break;
    }

    if (decoded.type == PQ_orderby_frame_type::ROW) {
      const int64 expected_record = *rows_read == 0 ? record0 : record1;
      const uint32 expected_rowid = *rows_read == 0 ? rowid0 : rowid1;
      const int64 expected_sortkey = *rows_read == 0 ? sortkey0 : sortkey1;
      failed =
          *rows_read >= 2 || decoded.record_image_len != sizeof(record0) ||
          decoded.row_id_len != sizeof(rowid0) ||
          decoded.sort_key_len != sizeof(sortkey0) ||
          memcmp(decoded.record_image, &expected_record, sizeof(record0)) != 0 ||
          memcmp(decoded.row_id, &expected_rowid, sizeof(rowid0)) != 0 ||
          memcmp(decoded.sort_key, &expected_sortkey, sizeof(sortkey0)) != 0;
      if (!failed) ++(*rows_read);
    } else if (decoded.type == PQ_orderby_frame_type::FINISH) {
      failed = decoded.record_image_len != 0 || decoded.row_id_len != 0 ||
               decoded.sort_key_len != 0;
      if (!failed) ++(*finishes_read);
    } else if (decoded.type == PQ_orderby_frame_type::ERROR) {
      failed = decoded.record_image_len != 0 || decoded.row_id_len != 0 ||
               decoded.sort_key_len != 0;
      if (!failed) ++(*errors_read);
    }
  }

  PQ_orderby_frame_header invalid{};
  invalid.magic = PQ_MQ_MESSAGE_MAGIC;
  invalid.version = PQ_ORDERBY_FRAME_VERSION;
  invalid.type = pq_orderby_frame_type_to_uint(PQ_orderby_frame_type::ROW);
  invalid.record_image_len = sizeof(record0);
  invalid.payload_len = sizeof(record0);
  const PQ_orderby_frame_header *unused_header = nullptr;
  const uchar *unused_payload = nullptr;
  if (!pq_validate_orderby_frame(&invalid, sizeof(invalid), &unused_header,
                                 &unused_payload)) {
    failed = true;
  }

  handle.cleanup();
  return failed || *rows_read != 2 || *finishes_read != 1 ||
         *errors_read != 1;
}

bool Exchange_sort::run_orderby_frame_merge_smoke(uint32 *rows_read,
                                                  uint32 *finishes_read) {
  if (rows_read == nullptr || finishes_read == nullptr) return true;
  *rows_read = 0;
  *finishes_read = 0;

  bool initialized_here = false;
  if (m_mq_handles == nullptr) {
    if (init()) return true;
    initialized_here = true;
  }
  if (m_nqueues != 3) {
    if (initialized_here) cleanup();
    return true;
  }

  const int64 record10 = 100;
  const int64 record11 = 110;
  const int64 record20 = 200;
  const int64 record21 = 210;
  const int64 record30 = 300;
  const int64 record40 = 400;
  const uint32 rowid10 = 10;
  const uint32 rowid11 = 11;
  const uint32 rowid20 = 20;
  const uint32 rowid21 = 21;
  const uint32 rowid30 = 30;
  const uint32 rowid40 = 40;
  const int64 key1 = 1;
  const int64 key2 = 2;
  const int64 key3 = 3;
  const int64 key4 = 4;

  bool failed =
      pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                            &record10, sizeof(record10), &rowid10,
                            sizeof(rowid10), &key1, sizeof(key1)) ||
      pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                            &record30, sizeof(record30), &rowid30,
                            sizeof(rowid30), &key3, sizeof(key3)) ||
      pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::FINISH,
                            nullptr, 0, nullptr, 0, nullptr, 0) ||
      pq_send_orderby_frame(get_mq_handle(1), PQ_orderby_frame_type::ROW,
                            &record11, sizeof(record11), &rowid11,
                            sizeof(rowid11), &key1, sizeof(key1)) ||
      pq_send_orderby_frame(get_mq_handle(1), PQ_orderby_frame_type::ROW,
                            &record20, sizeof(record20), &rowid20,
                            sizeof(rowid20), &key2, sizeof(key2)) ||
      pq_send_orderby_frame(get_mq_handle(1), PQ_orderby_frame_type::FINISH,
                            nullptr, 0, nullptr, 0, nullptr, 0) ||
      pq_send_orderby_frame(get_mq_handle(2), PQ_orderby_frame_type::ROW,
                            &record21, sizeof(record21), &rowid21,
                            sizeof(rowid21), &key2, sizeof(key2)) ||
      pq_send_orderby_frame(get_mq_handle(2), PQ_orderby_frame_type::ROW,
                            &record40, sizeof(record40), &rowid40,
                            sizeof(rowid40), &key4, sizeof(key4)) ||
      pq_send_orderby_frame(get_mq_handle(2), PQ_orderby_frame_type::FINISH,
                            nullptr, 0, nullptr, 0, nullptr, 0);

  PQ_orderby_record_batch batches[3];
  for (uint32 worker_id = 0; !failed && worker_id < 3; ++worker_id) {
    failed = pq_load_orderby_frame_batch(get_mq_handle(worker_id), worker_id,
                                         &batches[worker_id], finishes_read);
  }

  constexpr uint32 expected_row_ids[] = {10, 11, 20, 21, 30, 40};
  if (!failed) {
    failed = pq_orderby_cached_merge(
        batches, 3, false, expected_row_ids,
        static_cast<uint32>(std::size(expected_row_ids)), rows_read);
  }

  if (initialized_here) cleanup();
  return failed || *rows_read != 6 || *finishes_read != 3;
}

bool Exchange_sort::run_orderby_frame_merge_edge_smoke(uint32 *rows_read,
                                                       uint32 *finishes_read,
                                                       uint32 *errors_read) {
  if (rows_read == nullptr || finishes_read == nullptr ||
      errors_read == nullptr) {
    return true;
  }
  *rows_read = 0;
  *finishes_read = 0;
  *errors_read = 0;

  bool initialized_here = false;
  if (m_mq_handles == nullptr) {
    if (init()) return true;
    initialized_here = true;
  }
  if (m_nqueues != 3) {
    if (initialized_here) cleanup();
    return true;
  }

  const int64 record50 = 500;
  const uint32 rowid50 = 50;
  const int64 key5 = 5;

  bool failed =
      pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::FINISH,
                            nullptr, 0, nullptr, 0, nullptr, 0) ||
      pq_send_orderby_frame(get_mq_handle(1), PQ_orderby_frame_type::ERROR,
                            nullptr, 0, nullptr, 0, nullptr, 0) ||
      pq_send_orderby_frame(get_mq_handle(2), PQ_orderby_frame_type::ROW,
                            &record50, sizeof(record50), &rowid50,
                            sizeof(rowid50), &key5, sizeof(key5)) ||
      pq_send_orderby_frame(get_mq_handle(2), PQ_orderby_frame_type::FINISH,
                            nullptr, 0, nullptr, 0, nullptr, 0);

  PQ_orderby_record_batch batches[2];
  if (!failed) {
    failed = pq_load_orderby_frame_batch(get_mq_handle(0), 0, &batches[0],
                                         finishes_read);
  }

  if (!failed) {
    PQ_orderby_record_batch error_batch;
    const bool saw_expected_error =
        pq_load_orderby_frame_batch(get_mq_handle(1), 1, &error_batch,
                                    finishes_read, errors_read);
    failed = !saw_expected_error || *errors_read != 1;
  }

  if (!failed) {
    failed = pq_load_orderby_frame_batch(get_mq_handle(2), 2, &batches[1],
                                         finishes_read);
  }

  constexpr uint32 expected_row_ids[] = {50};
  if (!failed) {
    failed = pq_orderby_cached_merge(
        batches, 2, false, expected_row_ids,
        static_cast<uint32>(std::size(expected_row_ids)), rows_read);
  }

  if (initialized_here) cleanup();
  return failed || *rows_read != 1 || *finishes_read != 2 ||
         *errors_read != 1;
}

bool Exchange_sort::run_orderby_frame_materialization_smoke(
    TABLE *leader_table, uint32 *rows_read, uint32 *unsupported) {
  if (rows_read == nullptr || unsupported == nullptr) return true;
  *rows_read = 0;
  *unsupported = 0;

  if (!pq_orderby_materialization_table_supported(leader_table)) {
    *unsupported = 1;
    return false;
  }

  bool initialized_here = false;
  if (m_mq_handles == nullptr) {
    if (init()) return true;
    initialized_here = true;
  }
  if (m_nqueues == 0) {
    if (initialized_here) cleanup();
    return true;
  }

  Field *first_field = leader_table->field[0];
  const uint field_index = first_field->field_index();
  const bool had_read_bit =
      leader_table->read_set == nullptr ||
      bitmap_is_set(leader_table->read_set, field_index);
  const bool had_write_bit =
      leader_table->write_set == nullptr ||
      bitmap_is_set(leader_table->write_set, field_index);
  if (leader_table->read_set != nullptr && !had_read_bit) {
    bitmap_set_bit(leader_table->read_set, field_index);
  }
  if (leader_table->write_set != nullptr && !had_write_bit) {
    bitmap_set_bit(leader_table->write_set, field_index);
  }

  std::vector<uchar> original_record(
      leader_table->record[0],
      leader_table->record[0] + leader_table->s->reclength);

  constexpr longlong expected_value = 777;
  bool failed = first_field->store(expected_value, false) != TYPE_OK;
  std::vector<uchar> record_image;
  if (!failed) {
    record_image.assign(leader_table->record[0],
                        leader_table->record[0] + leader_table->s->reclength);
  }

  if (!original_record.empty()) {
    memcpy(leader_table->record[0], original_record.data(),
           original_record.size());
  }

  const uint32 rowid = 70;
  const int64 sortkey = 7;
  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                                   record_image.data(),
                                   static_cast<uint32>(record_image.size()),
                                   &rowid, sizeof(rowid), &sortkey,
                                   sizeof(sortkey));
  }

  void *raw_data = nullptr;
  uint32 raw_len = 0;
  PQ_orderby_decoded_frame decoded;
  if (!failed) {
    failed = get_mq_handle(0)->receive(&raw_data, &raw_len) != MQ_SUCCESS ||
             pq_decode_orderby_frame(raw_data, raw_len, &decoded) ||
             decoded.type != PQ_orderby_frame_type::ROW ||
             decoded.record_image_len != leader_table->s->reclength ||
             decoded.record_image == nullptr;
  }

  if (!failed) {
    memcpy(leader_table->record[0], decoded.record_image,
           decoded.record_image_len);
    failed = first_field->val_int() != expected_value;
  }

  if (!original_record.empty()) {
    memcpy(leader_table->record[0], original_record.data(),
           original_record.size());
  }
  if (initialized_here) cleanup();
  if (leader_table->read_set != nullptr && !had_read_bit) {
    bitmap_clear_bit(leader_table->read_set, field_index);
  }
  if (leader_table->write_set != nullptr && !had_write_bit) {
    bitmap_clear_bit(leader_table->write_set, field_index);
  }

  if (!failed) *rows_read = 1;
  return failed;
}
