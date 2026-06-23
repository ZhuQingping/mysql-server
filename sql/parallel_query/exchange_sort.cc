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

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "sql/field.h"
#include "sql/handler.h"
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

bool pq_orderby_worker_frame_producer_owner_emit_row(
    PQ_orderby_worker_frame_producer_owner *producer,
    const void *record_image, uint32 record_image_len, const void *row_id,
    uint32 row_id_len, int64 sort_key) {
  if (producer == nullptr || producer->handle == nullptr ||
      producer->finished || producer->detached || record_image == nullptr ||
      record_image_len == 0 || row_id == nullptr || row_id_len == 0) {
    return true;
  }
  if (producer->has_last_sort_key && sort_key < producer->last_sort_key) {
    return true;
  }

  if (pq_send_orderby_frame(producer->handle, PQ_orderby_frame_type::ROW,
                            record_image, record_image_len, row_id, row_id_len,
                            &sort_key, sizeof(sort_key),
                            producer->worker_id)) {
    return true;
  }
  producer->last_sort_key = sort_key;
  producer->has_last_sort_key = true;
  return false;
}

bool pq_orderby_worker_frame_producer_owner_finish(
    PQ_orderby_worker_frame_producer_owner *producer) {
  if (producer == nullptr || producer->handle == nullptr ||
      producer->finished || producer->detached) {
    return true;
  }
  if (pq_send_orderby_frame(producer->handle, PQ_orderby_frame_type::FINISH,
                            nullptr, 0, nullptr, 0, nullptr, 0,
                            producer->worker_id)) {
    return true;
  }
  producer->finished = true;
  return false;
}

bool pq_orderby_worker_frame_producer_owner_error(
    PQ_orderby_worker_frame_producer_owner *producer) {
  if (producer == nullptr || producer->handle == nullptr ||
      producer->finished || producer->detached) {
    return true;
  }
  if (pq_send_orderby_frame(producer->handle, PQ_orderby_frame_type::ERROR,
                            nullptr, 0, nullptr, 0, nullptr, 0,
                            producer->worker_id)) {
    return true;
  }
  producer->finished = true;
  return false;
}

bool pq_orderby_worker_frame_producer_owner_detach(
    PQ_orderby_worker_frame_producer_owner *producer) {
  if (producer == nullptr || producer->handle == nullptr ||
      producer->detached) {
    return true;
  }
  producer->handle->close_producer();
  producer->finished = true;
  producer->detached = true;
  return false;
}

void pq_orderby_worker_frame_producer_owner_cleanup(
    PQ_orderby_worker_frame_producer_owner *producer) {
  if (producer == nullptr) return;
  producer->handle = nullptr;
  producer->finished = true;
  producer->detached = true;
  producer->cleanup_seen = true;
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

bool pq_orderby_row_id_contract_controlled_smoke() {
  const uchar record_image[] = {0x01, 0x02, 0x03, 0x04};
  const uchar synthetic_row_id[] = {0x10, 0x11, 0x12, 0x13};
  const uchar handler_ref[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25};

  PQ_orderby_decoded_frame non_row;
  non_row.type = PQ_orderby_frame_type::FINISH;
  if (!pq_validate_orderby_row_id_contract(
          nullptr,
          {PQ_orderby_row_id_source::SYNTHETIC_SMOKE, 0, true}) ||
      !pq_validate_orderby_row_id_contract(
          &non_row,
          {PQ_orderby_row_id_source::SYNTHETIC_SMOKE, 0, true})) {
    return true;
  }

  PQ_orderby_decoded_frame no_row_id;
  no_row_id.type = PQ_orderby_frame_type::ROW;
  no_row_id.record_image = record_image;
  no_row_id.record_image_len = sizeof(record_image);
  if (!pq_validate_orderby_row_id_contract(
          &no_row_id, {PQ_orderby_row_id_source::NONE, 0, true}) ||
      !pq_validate_orderby_row_id_contract(
          &no_row_id, {PQ_orderby_row_id_source::SYNTHETIC_SMOKE, 0, true}) ||
      !pq_validate_orderby_row_id_contract(
          &no_row_id, {PQ_orderby_row_id_source::HANDLER_REF,
                       sizeof(handler_ref), true})) {
    return true;
  }

  PQ_orderby_decoded_frame synthetic;
  synthetic.type = PQ_orderby_frame_type::ROW;
  synthetic.record_image = record_image;
  synthetic.record_image_len = sizeof(record_image);
  synthetic.row_id = synthetic_row_id;
  synthetic.row_id_len = sizeof(synthetic_row_id);
  if (pq_validate_orderby_row_id_contract(
          &synthetic,
          {PQ_orderby_row_id_source::SYNTHETIC_SMOKE, 0, true}) ||
      !pq_validate_orderby_row_id_contract(
          &synthetic, {PQ_orderby_row_id_source::SYNTHETIC_SMOKE,
                       sizeof(synthetic_row_id), true}) ||
      !pq_validate_orderby_row_id_contract(
          &synthetic, {PQ_orderby_row_id_source::NONE, 0, true})) {
    return true;
  }

  PQ_orderby_decoded_frame handler;
  handler.type = PQ_orderby_frame_type::ROW;
  handler.record_image = record_image;
  handler.record_image_len = sizeof(record_image);
  handler.row_id = handler_ref;
  handler.row_id_len = sizeof(handler_ref);
  if (pq_validate_orderby_row_id_contract(
          &handler, {PQ_orderby_row_id_source::HANDLER_REF,
                     sizeof(handler_ref), true}) ||
      !pq_validate_orderby_row_id_contract(
          &handler, {PQ_orderby_row_id_source::HANDLER_REF,
                     sizeof(handler_ref) + 1, true}) ||
      !pq_validate_orderby_row_id_contract(
          &handler, {PQ_orderby_row_id_source::HANDLER_REF, 0, true}) ||
      !pq_validate_orderby_row_id_contract(
          &handler, {PQ_orderby_row_id_source::HANDLER_REF,
                     sizeof(handler_ref), false})) {
    return true;
  }

  const PQ_orderby_handler_ref_lifetime_contract valid_lifetime{
      {PQ_orderby_row_id_source::HANDLER_REF, sizeof(handler_ref), true},
      true, true, true, false, false};
  const PQ_orderby_handler_ref_lifetime_contract synthetic_lifetime{
      {PQ_orderby_row_id_source::SYNTHETIC_SMOKE, 0, true},
      true, true, true, false, false};
  const PQ_orderby_handler_ref_lifetime_contract not_deep_copied{
      {PQ_orderby_row_id_source::HANDLER_REF, sizeof(handler_ref), true},
      true, false, true, false, false};
  const PQ_orderby_handler_ref_lifetime_contract worker_not_current{
      {PQ_orderby_row_id_source::HANDLER_REF, sizeof(handler_ref), true},
      true, true, false, false, false};
  const PQ_orderby_handler_ref_lifetime_contract handler_can_advance{
      {PQ_orderby_row_id_source::HANDLER_REF, sizeof(handler_ref), true},
      true, true, true, true, false};
  const PQ_orderby_handler_ref_lifetime_contract worker_detached{
      {PQ_orderby_row_id_source::HANDLER_REF, sizeof(handler_ref), true},
      true, true, true, false, true};
  if (pq_validate_orderby_handler_ref_lifetime_contract(&handler,
                                                        valid_lifetime) ||
      !pq_validate_orderby_handler_ref_lifetime_contract(
          &handler, synthetic_lifetime) ||
      !pq_validate_orderby_handler_ref_lifetime_contract(
          &handler, not_deep_copied) ||
      !pq_validate_orderby_handler_ref_lifetime_contract(
          &handler, worker_not_current) ||
      !pq_validate_orderby_handler_ref_lifetime_contract(
          &handler, handler_can_advance) ||
      !pq_validate_orderby_handler_ref_lifetime_contract(
          &handler, worker_detached)) {
    return true;
  }

  return false;
}

}  // namespace

bool pq_orderby_handler_ref_adapter_smoke(handler *tie_break_file,
                                          const uchar *left_ref,
                                          uint32 left_ref_len,
                                          const uchar *right_ref,
                                          uint32 right_ref_len,
                                          int *cmp_forward,
                                          int *cmp_reverse) {
  if (cmp_forward == nullptr || cmp_reverse == nullptr) return true;
  *cmp_forward = 0;
  *cmp_reverse = 0;

  if (tie_break_file == nullptr || left_ref == nullptr || right_ref == nullptr ||
      left_ref_len == 0 || right_ref_len == 0 ||
      left_ref_len != right_ref_len ||
      tie_break_file->ref_length != left_ref_len) {
    return true;
  }

  PQ_orderby_cached_record left;
  PQ_orderby_cached_record right;
  left.sort_key.push_back(1);
  right.sort_key.push_back(1);
  left.has_sort_key = true;
  right.has_sort_key = true;
  left.row_id.assign(left_ref, left_ref + left_ref_len);
  right.row_id.assign(right_ref, right_ref + right_ref_len);
  left.worker_id = 0;
  right.worker_id = 1;

  if (left.sort_key != right.sort_key) return true;

  *cmp_forward = tie_break_file->cmp_ref(left.row_id.data(), right.row_id.data());
  *cmp_reverse = tie_break_file->cmp_ref(right.row_id.data(), left.row_id.data());
  return *cmp_forward == 0 || *cmp_reverse == 0 ||
         !((*cmp_forward < 0 && *cmp_reverse > 0) ||
           (*cmp_forward > 0 && *cmp_reverse < 0));
}

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

bool pq_validate_orderby_row_id_contract(
    const PQ_orderby_decoded_frame *decoded,
    const PQ_orderby_row_id_contract &contract) {
  if (decoded == nullptr || decoded->type != PQ_orderby_frame_type::ROW) {
    return true;
  }

  switch (contract.source) {
    case PQ_orderby_row_id_source::NONE:
      return contract.stable_output_required ||
             contract.expected_ref_length != 0 || decoded->row_id != nullptr ||
             decoded->row_id_len != 0;
    case PQ_orderby_row_id_source::SYNTHETIC_SMOKE:
      return !contract.stable_output_required ||
             contract.expected_ref_length != 0 || decoded->row_id == nullptr ||
             decoded->row_id_len == 0;
    case PQ_orderby_row_id_source::HANDLER_REF:
      return !contract.stable_output_required ||
             contract.expected_ref_length == 0 || decoded->row_id == nullptr ||
             decoded->row_id_len != contract.expected_ref_length;
  }

  return true;
}

bool pq_validate_orderby_handler_ref_lifetime_contract(
    const PQ_orderby_decoded_frame *decoded,
    const PQ_orderby_handler_ref_lifetime_contract &contract) {
  if (contract.row_id_contract.source != PQ_orderby_row_id_source::HANDLER_REF ||
      !contract.row_id_contract.stable_output_required ||
      contract.row_id_contract.expected_ref_length == 0 ||
      !contract.ref_length_verified || !contract.ref_deep_copied ||
      !contract.worker_record_current || contract.handler_can_advance ||
      contract.worker_detached) {
    return true;
  }

  return pq_validate_orderby_row_id_contract(decoded,
                                             contract.row_id_contract);
}

bool pq_run_orderby_handler_ref_wire_smoke(const uchar *record_image,
                                           uint32 record_image_len,
                                           const uchar *handler_ref,
                                           uint32 ref_length,
                                           uint32 *decoded_ref_bytes,
                                           uint32 *contract_success) {
  if (decoded_ref_bytes == nullptr || contract_success == nullptr) return true;
  *decoded_ref_bytes = 0;
  *contract_success = 0;

  if (record_image == nullptr || record_image_len == 0 ||
      handler_ref == nullptr || ref_length == 0) {
    return true;
  }

  PQ_mq_event sender_event;
  PQ_mq_event receiver_event;
  char ring[PQ_MQ_DEFAULT_RING_SIZE];
  MQueue queue(&sender_event, &receiver_event, ring, sizeof(ring));
  MQueue_handle handle(&queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
  if (handle.init()) return true;

  const int64 sort_key = 0;
  if (pq_send_orderby_frame(&handle, PQ_orderby_frame_type::ROW, record_image,
                            record_image_len, handler_ref, ref_length,
                            &sort_key, sizeof(sort_key), 0)) {
    return true;
  }

  void *raw_data = nullptr;
  uint32 raw_len = 0;
  if (handle.receive(&raw_data, &raw_len) != MQ_SUCCESS) return true;

  PQ_orderby_decoded_frame decoded;
  if (pq_decode_orderby_frame(raw_data, raw_len, &decoded) ||
      decoded.type != PQ_orderby_frame_type::ROW ||
      decoded.record_image_len != record_image_len ||
      decoded.row_id_len != ref_length ||
      decoded.sort_key_len != sizeof(sort_key) ||
      memcmp(decoded.record_image, record_image, record_image_len) != 0 ||
      memcmp(decoded.row_id, handler_ref, ref_length) != 0) {
    return true;
  }

  const PQ_orderby_row_id_contract row_id_contract{
      PQ_orderby_row_id_source::HANDLER_REF, ref_length, true};
  if (pq_validate_orderby_row_id_contract(&decoded, row_id_contract)) {
    return true;
  }

  std::vector<uchar> decoded_ref(decoded.row_id,
                                 decoded.row_id + decoded.row_id_len);
  if (decoded_ref.size() != ref_length ||
      memcmp(decoded_ref.data(), handler_ref, ref_length) != 0) {
    return true;
  }

  *decoded_ref_bytes = decoded.row_id_len;
  *contract_success = 1;
  return false;
}

bool Exchange_sort::read_mq_message(MQMessageType &type, void **datap,
                                    uint32 &data_len) {
  type = MQMessageType::FINISH;
  *datap = nullptr;
  data_len = 0;
  if (m_orderby_read_mq_shape_enabled && m_mq_handles != nullptr &&
      m_nqueues > 0) {
    PQ_orderby_loader_status status = PQ_orderby_loader_status::ERROR;
    PQ_orderby_decoded_frame decoded;
    if (read_orderby_frame_from_worker_shape(get_mq_handle(0), &status,
                                             &decoded)) {
      type = MQMessageType::ERROR;
      return true;
    }

    if (status == PQ_orderby_loader_status::ROW) {
      type = MQMessageType::ROW;
      *datap = const_cast<uchar *>(decoded.record_image);
      data_len = decoded.record_image_len;
      return true;
    }

    if (status == PQ_orderby_loader_status::ERROR) {
      type = MQMessageType::ERROR;
      return true;
    }
  }
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

bool Exchange_sort::init_runtime_sort_state_owner_shape(
    uint32 workers, bool stable_output, bool index_sort,
    uint32 sort_order_length, uint32 max_record_length, uint32 ref_length) {
  cleanup_runtime_sort_state_owner_shape();
  if (workers == 0 || sort_order_length == 0 || max_record_length == 0) {
    return true;
  }
  if (stable_output && ref_length == 0) return true;

  /*
    M11-E5h-1 only creates a fail-closed owner shell. It must not construct
    Filesort, initialize Sort_param, attach to JOIN cleanup, QEP, or AccessPath,
    or claim runtime readiness.
  */
  m_runtime_sort_state_owner_shape.workers = workers;
  m_runtime_sort_state_owner_shape.sort_order_length = sort_order_length;
  m_runtime_sort_state_owner_shape.max_record_length = max_record_length;
  m_runtime_sort_state_owner_shape.ref_length = ref_length;
  m_runtime_sort_state_owner_shape.stable_output = stable_output;
  m_runtime_sort_state_owner_shape.index_sort = index_sort;
  m_runtime_sort_state_owner_shape.initialized = true;
  return false;
}

bool Exchange_sort::init_runtime_sort_param_scalar_shape(
    uint32 sort_order_length, uint32 max_record_length, uint32 ref_length) {
  if (!m_runtime_sort_state_owner_shape.initialized ||
      sort_order_length == 0 || max_record_length == 0) {
    return true;
  }
  if (sort_order_length !=
          m_runtime_sort_state_owner_shape.sort_order_length ||
      max_record_length !=
          m_runtime_sort_state_owner_shape.max_record_length ||
      ref_length != m_runtime_sort_state_owner_shape.ref_length) {
    return true;
  }

  /*
    E5h-3 stores only scalar metadata. It does not construct or persist a real
    Sort_param, and it does not claim default ORDER BY runtime readiness.
  */
  m_runtime_sort_state_owner_shape.sort_param_order_length =
      sort_order_length;
  m_runtime_sort_state_owner_shape.sort_param_max_record_length =
      max_record_length;
  m_runtime_sort_state_owner_shape.sort_param_ref_length = ref_length;
  m_runtime_sort_state_owner_shape.sort_param_initialized = true;
  return false;
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

bool Exchange_sort::read_orderby_frame_from_worker_shape(
    MQueue_handle *handle, PQ_orderby_loader_status *status,
    PQ_orderby_decoded_frame *decoded) {
  if (status == nullptr || decoded == nullptr) return true;
  *status = PQ_orderby_loader_status::ERROR;
  *decoded = PQ_orderby_decoded_frame{};
  if (handle == nullptr) return true;

  void *raw_data = nullptr;
  uint32 raw_len = 0;
  const MQ_RESULT result = handle->receive(&raw_data, &raw_len);
  if (result == MQ_WOULD_BLOCK) {
    *status = PQ_orderby_loader_status::WOULD_BLOCK;
    return false;
  }
  if (result == MQ_DETACHED) {
    *status = PQ_orderby_loader_status::DETACHED;
    return false;
  }
  if (result != MQ_SUCCESS) return true;

  if (pq_decode_orderby_frame(raw_data, raw_len, decoded)) {
    *status = PQ_orderby_loader_status::ERROR;
    return false;
  }

  if (decoded->type == PQ_orderby_frame_type::ROW) {
    *status = PQ_orderby_loader_status::ROW;
  } else if (decoded->type == PQ_orderby_frame_type::FINISH) {
    *status = PQ_orderby_loader_status::FINISH;
  } else {
    *status = PQ_orderby_loader_status::ERROR;
  }
  return false;
}

void Exchange_sort::enable_orderby_read_mq_shape_for_smoke(bool enabled) {
  m_orderby_read_mq_shape_enabled = enabled;
}

bool Exchange_sort::load_orderby_frame_to_record_group(
    MQueue_handle *handle, uint32 worker_id,
    PQ_orderby_loader_status *status) {
  if (status == nullptr) return true;
  *status = PQ_orderby_loader_status::ERROR;
  if (worker_id >= m_record_groups.size()) return true;

  PQ_orderby_decoded_frame decoded;
  if (read_orderby_frame_from_worker_shape(handle, status, &decoded)) {
    return true;
  }
  if (*status == PQ_orderby_loader_status::ROW ||
      *status == PQ_orderby_loader_status::FINISH ||
      *status == PQ_orderby_loader_status::ERROR) {
    m_record_groups[worker_id].compare_state =
        PQ_orderby_batch_compare_state::NOT_EVALUATED;
  }
  if (*status == PQ_orderby_loader_status::WOULD_BLOCK ||
      *status == PQ_orderby_loader_status::DETACHED ||
      *status == PQ_orderby_loader_status::ERROR) {
    return false;
  }

  PQ_orderby_record_batch &batch = m_record_groups[worker_id];

  if (*status == PQ_orderby_loader_status::ROW) {
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

  if (*status == PQ_orderby_loader_status::FINISH) {
    batch.completed = true;
    return false;
  }

  *status = PQ_orderby_loader_status::ERROR;
  return false;
}

bool Exchange_sort::read_ordered_record_shadow_shape(
    std::vector<uchar> *row_image, PQ_orderby_shadow_read_status *status) {
  if (row_image == nullptr || status == nullptr) return true;
  row_image->clear();
  *status = PQ_orderby_shadow_read_status::ERROR;
  if (m_record_groups.empty()) return true;

  bool has_candidate = false;
  size_t candidate_group = 0;
  int64 candidate_key = 0;

  for (size_t group_index = 0; group_index < m_record_groups.size();
       ++group_index) {
    PQ_orderby_record_batch &batch = m_record_groups[group_index];
    if (!batch.completed) {
      *status = PQ_orderby_shadow_read_status::ERROR;
      return false;
    }
    if (batch.next_pos >= batch.records.size()) {
      continue;
    }

    const PQ_orderby_cached_record &record = batch.records[batch.next_pos];
    if (!record.has_sort_key || record.sort_key.size() != sizeof(int64)) {
      *status = PQ_orderby_shadow_read_status::ERROR;
      return false;
    }

    int64 key = 0;
    memcpy(&key, record.sort_key.data(), sizeof(key));
    if (!has_candidate || key < candidate_key ||
        (key == candidate_key && group_index < candidate_group)) {
      has_candidate = true;
      candidate_group = group_index;
      candidate_key = key;
    }
  }

  if (!has_candidate) {
    *status = PQ_orderby_shadow_read_status::EOF_REACHED;
    return false;
  }

  PQ_orderby_record_batch &batch = m_record_groups[candidate_group];
  const PQ_orderby_cached_record &record = batch.records[batch.next_pos];
  *row_image = record.row_image;
  ++batch.next_pos;
  batch.compare_state = PQ_orderby_batch_compare_state::NOT_EVALUATED;
  *status = PQ_orderby_shadow_read_status::ROW;
  return false;
}

bool Exchange_sort::read_ordered_record_stream_shape(
    binary_heap *heap, std::vector<bool> *in_heap,
    std::vector<bool> *terminal_workers, std::vector<uchar> *row_image,
    PQ_orderby_stream_read_status *status, uint32 *finishes_read,
    uint32 *would_blocks_read, uint32 *errors_read, uint32 *detaches_read,
    uint32 *refills_read, uint32 *heap_replaces_read,
    uint32 *heap_removes_read) {
  if (heap == nullptr || in_heap == nullptr || terminal_workers == nullptr ||
      row_image == nullptr || status == nullptr || finishes_read == nullptr ||
      would_blocks_read == nullptr || errors_read == nullptr ||
      detaches_read == nullptr || refills_read == nullptr ||
      heap_replaces_read == nullptr || heap_removes_read == nullptr) {
    return true;
  }
  row_image->clear();
  *status = PQ_orderby_stream_read_status::ERROR;
  if (m_mq_handles == nullptr || m_record_groups.empty() ||
      in_heap->size() != m_record_groups.size() ||
      terminal_workers->size() != m_record_groups.size()) {
    return true;
  }

  bool saw_would_block = false;
  for (uint32 worker = 0; worker < m_record_groups.size(); ++worker) {
    if ((*terminal_workers)[worker]) continue;

    PQ_orderby_record_batch &batch = m_record_groups[worker];
    if (batch.next_pos < batch.records.size()) {
      if (!(*in_heap)[worker] && heap->add(static_cast<int>(worker))) {
        return true;
      }
      (*in_heap)[worker] = true;
      continue;
    }

    bool loaded_row = false;
    for (;;) {
      PQ_orderby_loader_status loader_status = PQ_orderby_loader_status::ERROR;
      if (load_orderby_frame_to_record_group(get_mq_handle(worker), worker,
                                             &loader_status)) {
        return true;
      }
      if (loader_status == PQ_orderby_loader_status::ROW ||
          loader_status == PQ_orderby_loader_status::FINISH ||
          loader_status == PQ_orderby_loader_status::WOULD_BLOCK) {
        ++(*refills_read);
      }

      if (loader_status == PQ_orderby_loader_status::ROW) {
        loaded_row = true;
        continue;
      }

      if (loader_status == PQ_orderby_loader_status::FINISH) {
        (*terminal_workers)[worker] = true;
        ++(*finishes_read);
        break;
      }

      if (loader_status == PQ_orderby_loader_status::WOULD_BLOCK) {
        if (!loaded_row) saw_would_block = true;
        ++(*would_blocks_read);
        break;
      }

      if (loader_status == PQ_orderby_loader_status::DETACHED) {
        (*terminal_workers)[worker] = true;
        ++(*detaches_read);
        *status = PQ_orderby_stream_read_status::DETACHED;
        return false;
      }

      (*terminal_workers)[worker] = true;
      ++(*errors_read);
      *status = PQ_orderby_stream_read_status::ERROR;
      return false;
    }

    if (batch.next_pos < batch.records.size()) {
      if (!(*in_heap)[worker] && heap->add(static_cast<int>(worker))) {
        return true;
      }
      (*in_heap)[worker] = true;
      continue;
    }
  }

  if (saw_would_block) {
    *status = PQ_orderby_stream_read_status::WOULD_BLOCK;
    return false;
  }

  if (heap->empty()) {
    bool all_terminal = true;
    for (const bool terminal : *terminal_workers) {
      all_terminal = all_terminal && terminal;
    }
    *status = all_terminal ? PQ_orderby_stream_read_status::EOF_REACHED
                           : PQ_orderby_stream_read_status::WOULD_BLOCK;
    return false;
  }

  const int worker = heap->first();
  if (worker < 0 || static_cast<uint32>(worker) >= m_record_groups.size()) {
    return true;
  }
  PQ_orderby_record_batch &batch = m_record_groups[worker];
  if (batch.next_pos >= batch.records.size()) return true;

  const PQ_orderby_cached_record &record = batch.records[batch.next_pos];
  if (!record.has_sort_key || record.sort_key.size() != sizeof(int64) ||
      record.row_image.empty()) {
    *status = PQ_orderby_stream_read_status::ERROR;
    return false;
  }

  *row_image = record.row_image;
  ++batch.next_pos;
  if (batch.next_pos < batch.records.size()) {
    heap->replace_first(worker);
    ++(*heap_replaces_read);
  } else {
    heap->remove_first();
    (*in_heap)[worker] = false;
    ++(*heap_removes_read);
  }

  *status = PQ_orderby_stream_read_status::ROW;
  return false;
}

void Exchange_sort::cleanup_real_init_state_owner_shape() {
  m_compare_key_buffers[0].clear();
  m_compare_key_buffers[1].clear();
  m_tmp_key_buffer.clear();
  m_real_init_state_shape = PQ_orderby_real_init_state_shape{};
}

void Exchange_sort::cleanup_runtime_sort_state_owner_shape() {
  m_runtime_sort_state_owner_shape = PQ_orderby_runtime_sort_state_owner_shape{};
}

bool Exchange_sort::init_orderby_heap_reader_state_shape(uint32 workers,
                                                         bool descending) {
  cleanup_orderby_heap_reader_state_shape();
  if (workers == 0 || m_record_groups.size() != workers) return true;

  m_heap_reader_ctx.batches = m_record_groups.data();
  m_heap_reader_ctx.descending = descending;
  m_order_heap = new binary_heap(static_cast<int>(workers), &m_heap_reader_ctx,
                                 pq_orderby_cached_compare_batches);
  if (m_order_heap == nullptr || m_order_heap->init_binary_heap()) {
    cleanup_orderby_heap_reader_state_shape();
    return true;
  }

  m_heap_reader_in_heap.assign(workers, false);
  m_heap_reader_terminal_workers.assign(workers, false);
  m_heap_reader_counters = PQ_orderby_heap_reader_counters{};
  m_heap_reader_state_shape.workers = workers;
  m_heap_reader_state_shape.initialized = true;
  m_heap_reader_state_shape.heap_initialized = true;
  return false;
}

bool Exchange_sort::read_next_ordered_record_image_owned_shape(
    std::vector<uchar> *row_image, PQ_orderby_stream_read_status *status) {
  if (!m_heap_reader_state_shape.initialized || m_order_heap == nullptr ||
      row_image == nullptr || status == nullptr) {
    return true;
  }

  return read_ordered_record_stream_shape(
      m_order_heap, &m_heap_reader_in_heap, &m_heap_reader_terminal_workers,
      row_image, status, &m_heap_reader_counters.finishes_read,
      &m_heap_reader_counters.would_blocks_read,
      &m_heap_reader_counters.errors_read,
      &m_heap_reader_counters.detaches_read,
      &m_heap_reader_counters.refills_read,
      &m_heap_reader_counters.heap_replaces_read,
      &m_heap_reader_counters.heap_removes_read);
}

bool Exchange_sort::read_ordered_record_rich_status_shape(
    std::vector<uchar> *row_image, PQ_orderby_ordered_read_status *status) {
  if (row_image == nullptr || status == nullptr) return true;
  row_image->clear();
  *status = PQ_orderby_ordered_read_status::ERROR;

  if (!m_orderby_rich_status_shape_enabled) {
    *status = PQ_orderby_ordered_read_status::DISABLED;
    return false;
  }

  if (!m_heap_reader_state_shape.initialized || m_order_heap == nullptr) {
    *status = PQ_orderby_ordered_read_status::UNSUPPORTED;
    return false;
  }

  PQ_orderby_stream_read_status stream_status =
      PQ_orderby_stream_read_status::ERROR;
  if (read_next_ordered_record_image_owned_shape(row_image, &stream_status)) {
    return true;
  }

  switch (stream_status) {
    case PQ_orderby_stream_read_status::ROW:
      *status = PQ_orderby_ordered_read_status::ROW;
      return false;
    case PQ_orderby_stream_read_status::EOF_REACHED:
      *status = PQ_orderby_ordered_read_status::EOF_REACHED;
      return false;
    case PQ_orderby_stream_read_status::WOULD_BLOCK:
      *status = PQ_orderby_ordered_read_status::WOULD_BLOCK;
      return false;
    case PQ_orderby_stream_read_status::DETACHED:
      *status = PQ_orderby_ordered_read_status::DETACHED;
      return false;
    case PQ_orderby_stream_read_status::ERROR:
      *status = PQ_orderby_ordered_read_status::ERROR;
      return false;
  }

  *status = PQ_orderby_ordered_read_status::ERROR;
  return false;
}

bool Exchange_sort::read_default_ordered_record_heap_shape(
    std::vector<uchar> *row_image, PQ_orderby_ordered_read_status *status) {
  if (row_image == nullptr || status == nullptr) return true;
  row_image->clear();
  *status = PQ_orderby_ordered_read_status::ERROR;

  if (!m_orderby_default_heap_reader_shape_enabled) {
    *status = PQ_orderby_ordered_read_status::DISABLED;
    return false;
  }

  if (!m_orderby_rich_status_shape_enabled) {
    *status = PQ_orderby_ordered_read_status::UNSUPPORTED;
    return false;
  }

  if (read_ordered_record_rich_status_shape(row_image, status)) {
    return true;
  }

  if (*status == PQ_orderby_ordered_read_status::EOF_REACHED ||
      *status == PQ_orderby_ordered_read_status::DETACHED ||
      *status == PQ_orderby_ordered_read_status::ERROR) {
    const PQ_orderby_ordered_read_status terminal_status = *status;
    cleanup_order_gather_shape();
    *status = terminal_status;
  }
  return false;
}

bool Exchange_sort::init_orderby_materializer_owner_shape(
    TABLE *leader_table) {
  cleanup_orderby_materializer_owner_shape();
  if (!pq_orderby_materialization_table_supported(leader_table)) return true;

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

  m_materializer_original_record.assign(
      leader_table->record[0],
      leader_table->record[0] + leader_table->s->reclength);
  if (m_materializer_original_record.size() != leader_table->s->reclength) {
    cleanup_orderby_materializer_owner_shape();
    return true;
  }

  m_materializer_owner_shape.leader_table = leader_table;
  m_materializer_owner_shape.record_length = leader_table->s->reclength;
  m_materializer_owner_shape.field_index = field_index;
  m_materializer_owner_shape.initialized = true;
  m_materializer_owner_shape.original_record_saved = true;
  m_materializer_owner_shape.bitmap_state_saved = true;
  m_materializer_owner_shape.had_read_bit = had_read_bit;
  m_materializer_owner_shape.had_write_bit = had_write_bit;
  return false;
}

bool Exchange_sort::materialize_ordered_record_owner_shape(
    TABLE *leader_table, const std::vector<uchar> &row_image,
    PQ_orderby_materialize_status *status) {
  if (status == nullptr) return true;
  *status = PQ_orderby_materialize_status::ERROR;
  if (!m_materializer_owner_shape.initialized ||
      m_materializer_owner_shape.leader_table != leader_table ||
      !m_materializer_owner_shape.original_record_saved ||
      leader_table == nullptr || leader_table->s == nullptr ||
      leader_table->record[0] == nullptr ||
      m_materializer_owner_shape.record_length != leader_table->s->reclength) {
    *status = PQ_orderby_materialize_status::UNSUPPORTED;
    return false;
  }

  if (row_image.size() != m_materializer_owner_shape.record_length) {
    cleanup_orderby_materializer_owner_shape();
    *status = PQ_orderby_materialize_status::ERROR;
    return false;
  }

  memcpy(leader_table->record[0], row_image.data(), row_image.size());
  ++m_materializer_owner_shape.rows_materialized;
  *status = PQ_orderby_materialize_status::ROW;
  return false;
}

void Exchange_sort::cleanup_orderby_materializer_owner_shape() {
  TABLE *leader_table = m_materializer_owner_shape.leader_table;
  if (m_materializer_owner_shape.initialized && leader_table != nullptr) {
    if (m_materializer_owner_shape.original_record_saved &&
        leader_table->record[0] != nullptr &&
        m_materializer_original_record.size() ==
            m_materializer_owner_shape.record_length) {
      memcpy(leader_table->record[0], m_materializer_original_record.data(),
             m_materializer_original_record.size());
    }
    if (m_materializer_owner_shape.bitmap_state_saved) {
      const uint field_index = m_materializer_owner_shape.field_index;
      if (leader_table->read_set != nullptr &&
          !m_materializer_owner_shape.had_read_bit) {
        bitmap_clear_bit(leader_table->read_set, field_index);
      }
      if (leader_table->write_set != nullptr &&
          !m_materializer_owner_shape.had_write_bit) {
        bitmap_clear_bit(leader_table->write_set, field_index);
      }
    }
  }

  m_materializer_original_record.clear();
  m_materializer_owner_shape = PQ_orderby_materializer_owner_shape{};
  m_materializer_owner_shape.restored = true;
  m_orderby_materializer_shape_enabled = false;
}

void Exchange_sort::cleanup_orderby_heap_reader_state_shape() {
  if (m_order_heap != nullptr) {
    m_order_heap->reset();
    delete m_order_heap;
    m_order_heap = nullptr;
  }
  m_heap_reader_in_heap.clear();
  m_heap_reader_terminal_workers.clear();
  m_heap_reader_counters = PQ_orderby_heap_reader_counters{};
  m_heap_reader_ctx = PQ_orderby_cached_merge_ctx{};
  m_heap_reader_state_shape = PQ_orderby_heap_reader_state_shape{};
  m_heap_reader_state_shape.cleanup_seen = true;
}

void Exchange_sort::cleanup_order_gather_shape() {
  cleanup_orderby_materializer_owner_shape();
  cleanup_orderby_heap_reader_state_shape();
  m_min_records.clear();
  m_record_groups.clear();
  m_order_shape_workers = 0;
  m_order_shape_initialized = false;
  m_order_shape_stable_output = false;
  m_order_shape_index_sort = false;
  m_orderby_read_mq_shape_enabled = false;
  m_orderby_rich_status_shape_enabled = false;
  m_orderby_materializer_shape_enabled = false;
  m_orderby_default_heap_reader_shape_enabled = false;
  cleanup_sort_state_shape();
  cleanup_real_init_state_owner_shape();
  cleanup_runtime_sort_state_owner_shape();
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

  if (run_orderby_runtime_sort_state_owner_shape_smoke()) {
    return true;
  }

  if (run_orderby_runtime_sort_param_lifetime_smoke()) {
    return true;
  }

  if (run_orderby_real_init_allocation_smoke()) {
    return true;
  }

  if (run_orderby_frame_loader_smoke()) {
    return true;
  }

  return run_orderby_shadow_read_smoke();
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

bool Exchange_sort::run_orderby_runtime_sort_state_owner_shape_smoke() {
  constexpr uint32 kWorkers = 3;
  constexpr uint32 kSortOrderLength = 2;
  constexpr uint32 kMaxRecordLength = 64;
  constexpr uint32 kRefLength = 8;

  if (init_runtime_sort_state_owner_shape(
          kWorkers, /*stable_output=*/true, /*index_sort=*/false,
          kSortOrderLength, kMaxRecordLength, kRefLength)) {
    cleanup_runtime_sort_state_owner_shape();
    return true;
  }

  const bool valid =
      m_runtime_sort_state_owner_shape.initialized &&
      m_runtime_sort_state_owner_shape.workers == kWorkers &&
      m_runtime_sort_state_owner_shape.sort_order_length == kSortOrderLength &&
      m_runtime_sort_state_owner_shape.max_record_length == kMaxRecordLength &&
      m_runtime_sort_state_owner_shape.ref_length == kRefLength &&
      m_runtime_sort_state_owner_shape.stable_output &&
      !m_runtime_sort_state_owner_shape.index_sort &&
      !m_runtime_sort_state_owner_shape.runtime_ready &&
      !m_runtime_sort_state_owner_shape.filesort_constructed &&
      !m_runtime_sort_state_owner_shape.sort_param_initialized &&
      !m_runtime_sort_state_owner_shape.join_state_mutated &&
      !m_runtime_sort_state_owner_shape.filesorts_cleanup_attached &&
      !m_runtime_sort_state_owner_shape.qep_attached &&
      !m_runtime_sort_state_owner_shape.access_path_attached;

  cleanup_runtime_sort_state_owner_shape();
  return !valid || m_runtime_sort_state_owner_shape.initialized ||
         m_runtime_sort_state_owner_shape.runtime_ready ||
         m_runtime_sort_state_owner_shape.filesort_constructed ||
         m_runtime_sort_state_owner_shape.sort_param_initialized ||
         m_runtime_sort_state_owner_shape.join_state_mutated ||
         m_runtime_sort_state_owner_shape.filesorts_cleanup_attached ||
         m_runtime_sort_state_owner_shape.qep_attached ||
         m_runtime_sort_state_owner_shape.access_path_attached;
}

bool Exchange_sort::run_orderby_runtime_sort_param_lifetime_smoke() {
  constexpr uint32 kWorkers = 3;
  constexpr uint32 kSortOrderLength = 2;
  constexpr uint32 kMaxRecordLength = 64;
  constexpr uint32 kRefLength = 8;
  constexpr uint32 kSecondSortOrderLength = 3;
  constexpr uint32 kSecondMaxRecordLength = 96;
  constexpr uint32 kSecondRefLength = 12;

  if (init_runtime_sort_state_owner_shape(
          kWorkers, /*stable_output=*/true, /*index_sort=*/false,
          kSortOrderLength, kMaxRecordLength, kRefLength) ||
      init_runtime_sort_param_scalar_shape(kSortOrderLength, kMaxRecordLength,
                                           kRefLength)) {
    cleanup_runtime_sort_state_owner_shape();
    return true;
  }

  const bool first_valid =
      m_runtime_sort_state_owner_shape.sort_param_initialized &&
      m_runtime_sort_state_owner_shape.sort_param_order_length ==
          kSortOrderLength &&
      m_runtime_sort_state_owner_shape.sort_param_max_record_length ==
          kMaxRecordLength &&
      m_runtime_sort_state_owner_shape.sort_param_ref_length == kRefLength &&
      !m_runtime_sort_state_owner_shape.runtime_ready &&
      !m_runtime_sort_state_owner_shape.filesort_constructed;

  cleanup_runtime_sort_state_owner_shape();
  if (!first_valid || m_runtime_sort_state_owner_shape.sort_param_initialized ||
      m_runtime_sort_state_owner_shape.sort_param_order_length != 0 ||
      m_runtime_sort_state_owner_shape.sort_param_max_record_length != 0 ||
      m_runtime_sort_state_owner_shape.sort_param_ref_length != 0) {
    return true;
  }

  if (init_runtime_sort_state_owner_shape(
          kWorkers, /*stable_output=*/true, /*index_sort=*/false,
          kSecondSortOrderLength, kSecondMaxRecordLength, kSecondRefLength) ||
      init_runtime_sort_param_scalar_shape(kSecondSortOrderLength,
                                           kSecondMaxRecordLength,
                                           kSecondRefLength)) {
    cleanup_runtime_sort_state_owner_shape();
    return true;
  }

  const bool second_valid =
      m_runtime_sort_state_owner_shape.sort_param_initialized &&
      m_runtime_sort_state_owner_shape.sort_param_order_length ==
          kSecondSortOrderLength &&
      m_runtime_sort_state_owner_shape.sort_param_max_record_length ==
          kSecondMaxRecordLength &&
      m_runtime_sort_state_owner_shape.sort_param_ref_length ==
          kSecondRefLength &&
      !m_runtime_sort_state_owner_shape.runtime_ready &&
      !m_runtime_sort_state_owner_shape.filesort_constructed &&
      !m_runtime_sort_state_owner_shape.join_state_mutated &&
      !m_runtime_sort_state_owner_shape.filesorts_cleanup_attached &&
      !m_runtime_sort_state_owner_shape.qep_attached &&
      !m_runtime_sort_state_owner_shape.access_path_attached;

  cleanup_runtime_sort_state_owner_shape();
  return !second_valid || m_runtime_sort_state_owner_shape.initialized ||
         m_runtime_sort_state_owner_shape.sort_param_initialized ||
         m_runtime_sort_state_owner_shape.sort_param_order_length != 0 ||
         m_runtime_sort_state_owner_shape.sort_param_max_record_length != 0 ||
         m_runtime_sort_state_owner_shape.sort_param_ref_length != 0;
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
             status != PQ_orderby_loader_status::DETACHED ||
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

bool Exchange_sort::run_orderby_shadow_read_smoke() {
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

  const int64 record_low = 100;
  const int64 record_mid = 200;
  const int64 record_high = 300;
  const uint32 rowid = 1;
  const int64 key_low = 1;
  const int64 key_mid = 2;
  const int64 key_high = 3;
  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                                   &record_mid, sizeof(record_mid), &rowid,
                                   sizeof(rowid), &key_mid, sizeof(key_mid)) ||
             pq_send_orderby_frame(get_mq_handle(0),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             pq_send_orderby_frame(get_mq_handle(1), PQ_orderby_frame_type::ROW,
                                   &record_low, sizeof(record_low), &rowid,
                                   sizeof(rowid), &key_low, sizeof(key_low)) ||
             pq_send_orderby_frame(get_mq_handle(1),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             pq_send_orderby_frame(get_mq_handle(2), PQ_orderby_frame_type::ROW,
                                   &record_high, sizeof(record_high), &rowid,
                                   sizeof(rowid), &key_high,
                                   sizeof(key_high)) ||
             pq_send_orderby_frame(get_mq_handle(2),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0);
  }

  PQ_orderby_loader_status loader_status = PQ_orderby_loader_status::ERROR;
  for (uint32 worker = 0; !failed && worker < kWorkers; ++worker) {
    failed =
        load_orderby_frame_to_record_group(get_mq_handle(worker), worker,
                                           &loader_status) ||
        loader_status != PQ_orderby_loader_status::ROW ||
        load_orderby_frame_to_record_group(get_mq_handle(worker), worker,
                                           &loader_status) ||
        loader_status != PQ_orderby_loader_status::FINISH;
  }

  PQ_orderby_shadow_read_status read_status =
      PQ_orderby_shadow_read_status::ERROR;
  std::vector<uchar> row_image;
  const int64 expected_rows[] = {record_low, record_mid, record_high};
  for (const int64 expected_row : expected_rows) {
    if (failed) break;
    int64 actual_row = 0;
    failed = read_ordered_record_shadow_shape(&row_image, &read_status) ||
             read_status != PQ_orderby_shadow_read_status::ROW ||
             row_image.size() != sizeof(actual_row);
    if (!failed) {
      memcpy(&actual_row, row_image.data(), sizeof(actual_row));
      failed = actual_row != expected_row;
    }
  }
  if (!failed) {
    failed = read_ordered_record_shadow_shape(&row_image, &read_status) ||
             read_status != PQ_orderby_shadow_read_status::EOF_REACHED ||
             !row_image.empty();
  }

  cleanup_order_gather_shape();
  if (!failed) {
    failed = init_real_init_state_owner_shape(kWorkers, /*stable_output=*/true,
                                             /*index_sort=*/false,
                                             kSortOrderLength, kMaxRecordLength,
                                             kRefLength) ||
             allocate_real_init_buffers_shape();
  }
  if (!failed) {
    m_record_groups[0].completed = false;
    failed = read_ordered_record_shadow_shape(&row_image, &read_status) ||
             read_status != PQ_orderby_shadow_read_status::ERROR ||
             !row_image.empty();
  }

  cleanup_order_gather_shape();
  if (!failed) {
    failed = init_real_init_state_owner_shape(kWorkers, /*stable_output=*/true,
                                             /*index_sort=*/false,
                                             kSortOrderLength, kMaxRecordLength,
                                             kRefLength) ||
             allocate_real_init_buffers_shape();
  }
  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                                   &record_low, sizeof(record_low), &rowid,
                                   sizeof(rowid), &key_low, sizeof(key_low));
  }
  if (!failed) {
    failed = load_orderby_frame_to_record_group(get_mq_handle(0), 0,
                                                &loader_status) ||
             loader_status != PQ_orderby_loader_status::ROW ||
             read_ordered_record_shadow_shape(&row_image, &read_status) ||
             read_status != PQ_orderby_shadow_read_status::ERROR ||
             !row_image.empty();
  }

  cleanup_order_gather_shape();
  if (!failed) {
    failed = init_real_init_state_owner_shape(kWorkers, /*stable_output=*/true,
                                             /*index_sort=*/false,
                                             kSortOrderLength, kMaxRecordLength,
                                             kRefLength) ||
             allocate_real_init_buffers_shape();
  }
  if (!failed) {
    PQ_orderby_cached_record invalid_record;
    invalid_record.row_image.assign(
        reinterpret_cast<const uchar *>(&record_low),
        reinterpret_cast<const uchar *>(&record_low) + sizeof(record_low));
    invalid_record.sort_key.push_back(0);
    invalid_record.has_sort_key = true;
    m_record_groups[0].records.push_back(std::move(invalid_record));
    m_record_groups[0].completed = true;
    m_record_groups[1].completed = true;
    m_record_groups[2].completed = true;
    failed = read_ordered_record_shadow_shape(&row_image, &read_status) ||
             read_status != PQ_orderby_shadow_read_status::ERROR ||
             !row_image.empty();
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

bool Exchange_sort::run_orderby_worker_frame_producer_smoke(
    uint32 *rows_read, uint32 *finishes_read, uint32 *errors_read) {
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

  PQ_orderby_worker_frame_producer_owner worker0{&handle, 0};
  PQ_orderby_worker_frame_producer_owner worker1{&handle, 1};
  PQ_orderby_worker_frame_producer_owner worker2{&handle, 2};

  const int64 record10 = 100;
  const int64 record30 = 300;
  const int64 record20 = 200;
  const uint32 rowid10 = 10;
  const uint32 rowid30 = 30;
  const uint32 rowid20 = 20;

  bool failed =
      pq_orderby_worker_frame_producer_owner_emit_row(
          &worker0, &record10, sizeof(record10), &rowid10, sizeof(rowid10),
          1) ||
      pq_orderby_worker_frame_producer_owner_emit_row(
          &worker0, &record30, sizeof(record30), &rowid30, sizeof(rowid30),
          3) ||
      pq_orderby_worker_frame_producer_owner_finish(&worker0) ||
      !pq_orderby_worker_frame_producer_owner_emit_row(
          &worker0, &record10, sizeof(record10), &rowid10, sizeof(rowid10),
          4) ||
      pq_orderby_worker_frame_producer_owner_emit_row(
          &worker1, &record20, sizeof(record20), &rowid20, sizeof(rowid20),
          2) ||
      pq_orderby_worker_frame_producer_owner_finish(&worker1) ||
      pq_orderby_worker_frame_producer_owner_error(&worker2);

  PQ_mq_event negative_sender_event;
  PQ_mq_event negative_receiver_event;
  char negative_ring[PQ_MQ_DEFAULT_RING_SIZE];
  MQueue negative_queue(&negative_sender_event, &negative_receiver_event,
                        negative_ring, sizeof(negative_ring));
  MQueue_handle negative_handle(&negative_queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
  if (!failed && negative_handle.init()) failed = true;
  PQ_orderby_worker_frame_producer_owner negative_worker{&negative_handle, 9};
  if (!failed) {
    failed = pq_orderby_worker_frame_producer_owner_emit_row(
                 &negative_worker, &record30, sizeof(record30), &rowid30,
                 sizeof(rowid30), 30) ||
             !pq_orderby_worker_frame_producer_owner_emit_row(
                 &negative_worker, &record20, sizeof(record20), &rowid20,
                 sizeof(rowid20), 20);
  }

  int64 worker_last_key[3] = {0, 0, 0};
  bool worker_has_key[3] = {false, false, false};
  const PQ_orderby_frame_type expected_types[] = {
      PQ_orderby_frame_type::ROW, PQ_orderby_frame_type::ROW,
      PQ_orderby_frame_type::FINISH, PQ_orderby_frame_type::ROW,
      PQ_orderby_frame_type::FINISH, PQ_orderby_frame_type::ERROR};
  const uint32 expected_worker_ids[] = {0, 0, 0, 1, 1, 2};
  const int64 expected_records[] = {record10, record30, 0, record20, 0, 0};
  const uint32 expected_rowids[] = {rowid10, rowid30, 0, rowid20, 0, 0};
  const int64 expected_keys[] = {1, 3, 0, 2, 0, 0};

  for (uint32 i = 0; !failed && i < std::size(expected_types); ++i) {
    void *raw_data = nullptr;
    uint32 raw_len = 0;
    if (handle.receive(&raw_data, &raw_len) != MQ_SUCCESS) {
      failed = true;
      break;
    }

    const PQ_orderby_frame_header *header = nullptr;
    const uchar *payload = nullptr;
    PQ_orderby_decoded_frame decoded;
    failed = pq_validate_orderby_frame(raw_data, raw_len, &header, &payload) ||
             pq_decode_orderby_frame(raw_data, raw_len, &decoded) ||
             decoded.type != expected_types[i] ||
             header->flags != expected_worker_ids[i];
    if (failed) break;

    if (decoded.type == PQ_orderby_frame_type::ROW) {
      int64 record = 0;
      uint32 rowid = 0;
      int64 key = 0;
      const uint32 worker_id = header->flags;
      failed = worker_id >= 3 ||
               decoded.record_image_len != sizeof(record) ||
               decoded.row_id_len != sizeof(rowid) ||
               decoded.sort_key_len != sizeof(key);
      if (!failed) {
        memcpy(&record, decoded.record_image, sizeof(record));
        memcpy(&rowid, decoded.row_id, sizeof(rowid));
        memcpy(&key, decoded.sort_key, sizeof(key));
        failed = record != expected_records[i] || rowid != expected_rowids[i] ||
                 key != expected_keys[i] ||
                 (worker_has_key[worker_id] && key < worker_last_key[worker_id]);
      }
      if (!failed) {
        worker_last_key[worker_id] = key;
        worker_has_key[worker_id] = true;
        ++(*rows_read);
      }
    } else if (decoded.type == PQ_orderby_frame_type::FINISH) {
      ++(*finishes_read);
    } else {
      ++(*errors_read);
    }
  }

  PQ_mq_event detach_sender_event;
  PQ_mq_event detach_receiver_event;
  char detach_ring[PQ_MQ_DEFAULT_RING_SIZE];
  MQueue detach_queue(&detach_sender_event, &detach_receiver_event,
                      detach_ring, sizeof(detach_ring));
  MQueue_handle detach_handle(&detach_queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
  if (!failed && detach_handle.init()) failed = true;
  PQ_orderby_worker_frame_producer_owner detach_worker{&detach_handle, 7};
  if (!failed) {
    void *raw_data = nullptr;
    uint32 raw_len = 0;
    failed = pq_orderby_worker_frame_producer_owner_detach(&detach_worker) ||
             detach_handle.receive(&raw_data, &raw_len) != MQ_DETACHED ||
             !pq_orderby_worker_frame_producer_owner_emit_row(
                 &detach_worker, &record10, sizeof(record10), &rowid10,
                 sizeof(rowid10), 5);
  }
  if (!failed) {
    pq_orderby_worker_frame_producer_owner_cleanup(&detach_worker);
    failed = !detach_worker.cleanup_seen || detach_worker.handle != nullptr ||
             !pq_orderby_worker_frame_producer_owner_emit_row(
                 &detach_worker, &record10, sizeof(record10), &rowid10,
                 sizeof(rowid10), 6);
    pq_orderby_worker_frame_producer_owner_cleanup(&detach_worker);
    failed = failed || !detach_worker.cleanup_seen ||
             detach_worker.handle != nullptr;
  }
  detach_handle.cleanup();
  negative_handle.cleanup();
  handle.cleanup();

  return failed || *rows_read != 3 || *finishes_read != 2 ||
         *errors_read != 1;
}

bool Exchange_sort::run_orderby_worker_producer_adapter_skeleton_smoke(
    uint32 *rows_read, uint32 *finishes_read, uint32 *errors_read,
    uint32 *order_rejects, uint32 *after_finish_rejects) {
  if (rows_read == nullptr || finishes_read == nullptr ||
      errors_read == nullptr || order_rejects == nullptr ||
      after_finish_rejects == nullptr) {
    return true;
  }
  *rows_read = 0;
  *finishes_read = 0;
  *errors_read = 0;
  *order_rejects = 0;
  *after_finish_rejects = 0;

  PQ_mq_event sender_event;
  PQ_mq_event receiver_event;
  char ring[PQ_MQ_DEFAULT_RING_SIZE];
  MQueue queue(&sender_event, &receiver_event, ring, sizeof(ring));
  MQueue_handle handle(&queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
  if (handle.init()) return true;

  PQ_orderby_worker_frame_producer_owner worker0{&handle, 0};
  PQ_orderby_worker_frame_producer_owner worker1{&handle, 1};
  PQ_orderby_worker_frame_producer_owner worker2{&handle, 2};

  const int64 record10 = 100;
  const int64 record30 = 300;
  const int64 record20 = 200;
  const uint32 rowid10 = 10;
  const uint32 rowid30 = 30;
  const uint32 rowid20 = 20;

  bool failed =
      pq_orderby_worker_frame_producer_owner_emit_row(
          &worker0, &record10, sizeof(record10), &rowid10, sizeof(rowid10),
          1) ||
      pq_orderby_worker_frame_producer_owner_emit_row(
          &worker0, &record30, sizeof(record30), &rowid30, sizeof(rowid30),
          3) ||
      pq_orderby_worker_frame_producer_owner_finish(&worker0);

  if (!failed &&
      pq_orderby_worker_frame_producer_owner_emit_row(
          &worker0, &record10, sizeof(record10), &rowid10, sizeof(rowid10),
          4)) {
    ++(*after_finish_rejects);
  } else if (!failed) {
    failed = true;
  }

  if (!failed) {
    failed = pq_orderby_worker_frame_producer_owner_emit_row(
        &worker1, &record20, sizeof(record20), &rowid20, sizeof(rowid20), 2);
  }
  if (!failed &&
      pq_orderby_worker_frame_producer_owner_emit_row(
          &worker1, &record10, sizeof(record10), &rowid10, sizeof(rowid10),
          1)) {
    ++(*order_rejects);
  } else if (!failed) {
    failed = true;
  }
  if (!failed) {
    failed = pq_orderby_worker_frame_producer_owner_finish(&worker1) ||
             pq_orderby_worker_frame_producer_owner_error(&worker2);
  }

  const PQ_orderby_frame_type expected_types[] = {
      PQ_orderby_frame_type::ROW, PQ_orderby_frame_type::ROW,
      PQ_orderby_frame_type::FINISH, PQ_orderby_frame_type::ROW,
      PQ_orderby_frame_type::FINISH, PQ_orderby_frame_type::ERROR};
  const uint32 expected_worker_ids[] = {0, 0, 0, 1, 1, 2};
  const int64 expected_records[] = {record10, record30, 0, record20, 0, 0};
  const uint32 expected_rowids[] = {rowid10, rowid30, 0, rowid20, 0, 0};
  const int64 expected_keys[] = {1, 3, 0, 2, 0, 0};

  for (uint32 i = 0; !failed && i < std::size(expected_types); ++i) {
    void *raw_data = nullptr;
    uint32 raw_len = 0;
    if (handle.receive(&raw_data, &raw_len) != MQ_SUCCESS) {
      failed = true;
      break;
    }

    const PQ_orderby_frame_header *header = nullptr;
    const uchar *payload = nullptr;
    PQ_orderby_decoded_frame decoded;
    failed = pq_validate_orderby_frame(raw_data, raw_len, &header, &payload) ||
             pq_decode_orderby_frame(raw_data, raw_len, &decoded) ||
             decoded.type != expected_types[i] ||
             header->flags != expected_worker_ids[i];
    if (failed) break;

    if (decoded.type == PQ_orderby_frame_type::ROW) {
      int64 record = 0;
      uint32 rowid = 0;
      int64 key = 0;
      failed = decoded.record_image_len != sizeof(record) ||
               decoded.row_id_len != sizeof(rowid) ||
               decoded.sort_key_len != sizeof(key);
      if (!failed) {
        memcpy(&record, decoded.record_image, sizeof(record));
        memcpy(&rowid, decoded.row_id, sizeof(rowid));
        memcpy(&key, decoded.sort_key, sizeof(key));
        failed = record != expected_records[i] || rowid != expected_rowids[i] ||
                 key != expected_keys[i];
      }
      if (!failed) ++(*rows_read);
    } else if (decoded.type == PQ_orderby_frame_type::FINISH) {
      ++(*finishes_read);
    } else {
      ++(*errors_read);
    }
  }

  return failed || *rows_read != 3 || *finishes_read != 2 ||
         *errors_read != 1 || *order_rejects != 1 ||
         *after_finish_rejects != 1;
}

bool Exchange_sort::run_orderby_streaming_heap_read_smoke(
    uint32 *rows_read, uint32 *finishes_read, uint32 *would_blocks_read,
    uint32 *errors_read, uint32 *detaches_read, uint32 *refills_read,
    uint32 *heap_replaces_read, uint32 *heap_removes_read) {
  if (rows_read == nullptr || finishes_read == nullptr ||
      would_blocks_read == nullptr || errors_read == nullptr ||
      detaches_read == nullptr || refills_read == nullptr ||
      heap_replaces_read == nullptr || heap_removes_read == nullptr) {
    return true;
  }
  *rows_read = 0;
  *finishes_read = 0;
  *would_blocks_read = 0;
  *errors_read = 0;
  *detaches_read = 0;
  *refills_read = 0;
  *heap_replaces_read = 0;
  *heap_removes_read = 0;

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

  const int64 record1 = 100;
  const int64 record2 = 200;
  const int64 record3 = 300;
  const int64 record4 = 400;
  const uint32 rowid = 1;
  const int64 key1 = 1;
  const int64 key2 = 2;
  const int64 key3 = 3;
  const int64 key4 = 4;

  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                                   &record1, sizeof(record1), &rowid,
                                   sizeof(rowid), &key1, sizeof(key1)) ||
             pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                                   &record4, sizeof(record4), &rowid,
                                   sizeof(rowid), &key4, sizeof(key4)) ||
             pq_send_orderby_frame(get_mq_handle(0),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             pq_send_orderby_frame(get_mq_handle(2), PQ_orderby_frame_type::ROW,
                                   &record2, sizeof(record2), &rowid,
                                   sizeof(rowid), &key2, sizeof(key2)) ||
             pq_send_orderby_frame(get_mq_handle(2),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0);
  }

  PQ_orderby_cached_merge_ctx ctx{m_record_groups.data(),
                                  /*descending=*/false};
  binary_heap heap(static_cast<int>(kWorkers), &ctx,
                   pq_orderby_cached_compare_batches);
  std::vector<bool> in_heap(kWorkers, false);
  std::vector<bool> terminal_workers(kWorkers, false);
  if (!failed && heap.init_binary_heap()) failed = true;

  std::vector<uchar> row_image;
  PQ_orderby_stream_read_status status = PQ_orderby_stream_read_status::ERROR;
  if (!failed) {
    failed = read_ordered_record_stream_shape(
                 &heap, &in_heap, &terminal_workers, &row_image, &status,
                 finishes_read, would_blocks_read, errors_read, detaches_read,
                 refills_read, heap_replaces_read, heap_removes_read) ||
             status != PQ_orderby_stream_read_status::WOULD_BLOCK ||
             !row_image.empty();
  }
  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(1), PQ_orderby_frame_type::ROW,
                                   &record3, sizeof(record3), &rowid,
                                   sizeof(rowid), &key3, sizeof(key3)) ||
             pq_send_orderby_frame(get_mq_handle(1),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0);
  }

  const int64 expected_rows[] = {record1, record2, record3, record4};
  for (const int64 expected_row : expected_rows) {
    if (failed) break;
    failed = read_ordered_record_stream_shape(
                 &heap, &in_heap, &terminal_workers, &row_image, &status,
                 finishes_read, would_blocks_read, errors_read, detaches_read,
                 refills_read, heap_replaces_read, heap_removes_read) ||
             status != PQ_orderby_stream_read_status::ROW ||
             row_image.size() != sizeof(expected_row);
    if (!failed) {
      int64 actual_row = 0;
      memcpy(&actual_row, row_image.data(), sizeof(actual_row));
      failed = actual_row != expected_row;
    }
    if (!failed) ++(*rows_read);
  }

  if (!failed) {
    failed = read_ordered_record_stream_shape(
                 &heap, &in_heap, &terminal_workers, &row_image, &status,
                 finishes_read, would_blocks_read, errors_read, detaches_read,
                 refills_read, heap_replaces_read, heap_removes_read) ||
             status != PQ_orderby_stream_read_status::EOF_REACHED ||
             !row_image.empty() || *finishes_read != kWorkers;
  }

  cleanup_order_gather_shape();
  heap.reset();
  std::fill(in_heap.begin(), in_heap.end(), false);
  std::fill(terminal_workers.begin(), terminal_workers.end(), false);
  if (!failed) {
    failed = init_real_init_state_owner_shape(kWorkers, /*stable_output=*/true,
                                             /*index_sort=*/false,
                                             kSortOrderLength, kMaxRecordLength,
                                             kRefLength) ||
             allocate_real_init_buffers_shape();
  }
  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0),
                                   PQ_orderby_frame_type::ERROR, nullptr, 0,
                                   nullptr, 0, nullptr, 0);
  }
  if (!failed) {
    failed = read_ordered_record_stream_shape(
                 &heap, &in_heap, &terminal_workers, &row_image, &status,
                 finishes_read, would_blocks_read, errors_read, detaches_read,
                 refills_read, heap_replaces_read, heap_removes_read) ||
             status != PQ_orderby_stream_read_status::ERROR ||
             !row_image.empty() || *errors_read == 0;
  }

  cleanup_order_gather_shape();
  heap.reset();
  std::fill(in_heap.begin(), in_heap.end(), false);
  std::fill(terminal_workers.begin(), terminal_workers.end(), false);
  if (!failed) {
    failed = init_real_init_state_owner_shape(kWorkers, /*stable_output=*/true,
                                             /*index_sort=*/false,
                                             kSortOrderLength, kMaxRecordLength,
                                             kRefLength) ||
             allocate_real_init_buffers_shape();
  }
  if (!failed) {
    get_mq_handle(0)->close_producer();
    failed = read_ordered_record_stream_shape(
                 &heap, &in_heap, &terminal_workers, &row_image, &status,
                 finishes_read, would_blocks_read, errors_read, detaches_read,
                 refills_read, heap_replaces_read, heap_removes_read) ||
             status != PQ_orderby_stream_read_status::DETACHED ||
             !row_image.empty() || *detaches_read == 0;
  }

  cleanup_order_gather_shape();
  if (initialized_here) cleanup();
  return failed || *rows_read != 4 || *finishes_read != 3 ||
         *would_blocks_read == 0 || *errors_read != 1 ||
         *detaches_read != 1 || *refills_read == 0 ||
         *heap_replaces_read == 0 || *heap_removes_read == 0;
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
      const PQ_orderby_row_id_contract synthetic_contract{
          PQ_orderby_row_id_source::SYNTHETIC_SMOKE, 0, true};
      failed =
          *rows_read >= 2 || decoded.record_image_len != sizeof(record0) ||
          decoded.row_id_len != sizeof(rowid0) ||
          decoded.sort_key_len != sizeof(sortkey0) ||
          memcmp(decoded.record_image, &expected_record, sizeof(record0)) != 0 ||
          memcmp(decoded.row_id, &expected_rowid, sizeof(rowid0)) != 0 ||
          memcmp(decoded.sort_key, &expected_sortkey, sizeof(sortkey0)) != 0 ||
          pq_validate_orderby_row_id_contract(&decoded, synthetic_contract);
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

  if (!failed) {
    failed = pq_orderby_row_id_contract_controlled_smoke();
  }

  if (!failed) {
    const PQ_orderby_row_id_contract missing_metadata{
        PQ_orderby_row_id_source::NONE, 0, true};
    const PQ_orderby_row_id_contract synthetic_with_ref_length{
        PQ_orderby_row_id_source::SYNTHETIC_SMOKE, sizeof(rowid0), true};
    const PQ_orderby_row_id_contract handler_ref_with_zero_length{
        PQ_orderby_row_id_source::HANDLER_REF, 0, true};
    const PQ_orderby_row_id_contract handler_ref_wrong_length{
        PQ_orderby_row_id_source::HANDLER_REF, sizeof(rowid0) + 1, true};
    PQ_orderby_decoded_frame synthetic_decoded;
    failed =
        pq_send_orderby_frame(&handle, PQ_orderby_frame_type::ROW, &record0,
                              sizeof(record0), &rowid0, sizeof(rowid0),
                              &sortkey0, sizeof(sortkey0));
    if (!failed) {
      void *raw_data = nullptr;
      uint32 raw_len = 0;
      failed = handle.receive(&raw_data, &raw_len) != MQ_SUCCESS ||
               pq_decode_orderby_frame(raw_data, raw_len, &synthetic_decoded) ||
               !pq_validate_orderby_row_id_contract(&synthetic_decoded,
                                                    missing_metadata) ||
               !pq_validate_orderby_row_id_contract(
                   &synthetic_decoded, synthetic_with_ref_length) ||
               !pq_validate_orderby_row_id_contract(
                   &synthetic_decoded, handler_ref_with_zero_length) ||
               !pq_validate_orderby_row_id_contract(
                   &synthetic_decoded, handler_ref_wrong_length);
    }
  }

  if (!failed) {
    PQ_mq_event typed_sender_event;
    PQ_mq_event typed_receiver_event;
    char typed_ring[PQ_MQ_DEFAULT_RING_SIZE];
    MQueue typed_queue(&typed_sender_event, &typed_receiver_event, typed_ring,
                       sizeof(typed_ring));
    MQueue_handle typed_handle(&typed_queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
    if (typed_handle.init()) {
      failed = true;
    } else {
      PQ_orderby_loader_status status = PQ_orderby_loader_status::ERROR;
      PQ_orderby_decoded_frame decoded;
      failed = read_orderby_frame_from_worker_shape(&typed_handle, &status,
                                                    &decoded) ||
               status != PQ_orderby_loader_status::WOULD_BLOCK;

      if (!failed) {
        failed = pq_send_orderby_frame(&typed_handle,
                                       PQ_orderby_frame_type::ROW, &record0,
                                       sizeof(record0), &rowid0,
                                       sizeof(rowid0), &sortkey0,
                                       sizeof(sortkey0)) ||
                 read_orderby_frame_from_worker_shape(&typed_handle, &status,
                                                      &decoded) ||
                 status != PQ_orderby_loader_status::ROW ||
                 decoded.type != PQ_orderby_frame_type::ROW ||
                 decoded.record_image_len != sizeof(record0) ||
                 decoded.row_id_len != sizeof(rowid0) ||
                 decoded.sort_key_len != sizeof(sortkey0);
      }
      if (!failed) {
        failed = pq_send_orderby_frame(&typed_handle,
                                       PQ_orderby_frame_type::FINISH, nullptr,
                                       0, nullptr, 0, nullptr, 0) ||
                 read_orderby_frame_from_worker_shape(&typed_handle, &status,
                                                      &decoded) ||
                 status != PQ_orderby_loader_status::FINISH ||
                 decoded.type != PQ_orderby_frame_type::FINISH;
      }
      if (!failed) {
        failed = pq_send_orderby_frame(&typed_handle,
                                       PQ_orderby_frame_type::ERROR, nullptr, 0,
                                       nullptr, 0, nullptr, 0) ||
                 read_orderby_frame_from_worker_shape(&typed_handle, &status,
                                                      &decoded) ||
                 status != PQ_orderby_loader_status::ERROR ||
                 decoded.type != PQ_orderby_frame_type::ERROR;
      }
      if (!failed) {
        invalid.magic = PQ_MQ_MESSAGE_MAGIC;
        failed = typed_handle.send(&invalid, sizeof(invalid)) != MQ_SUCCESS ||
                 read_orderby_frame_from_worker_shape(&typed_handle, &status,
                                                      &decoded) ||
                 status != PQ_orderby_loader_status::ERROR;
      }
      typed_handle.cleanup();
    }
  }

  if (!failed) {
    PQ_mq_event detached_sender_event;
    PQ_mq_event detached_receiver_event;
    char detached_ring[PQ_MQ_DEFAULT_RING_SIZE];
    MQueue detached_queue(&detached_sender_event, &detached_receiver_event,
                          detached_ring, sizeof(detached_ring));
    MQueue_handle detached_handle(&detached_queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
    if (detached_handle.init()) {
      failed = true;
    } else {
      detached_handle.close_producer();
      PQ_orderby_loader_status status = PQ_orderby_loader_status::ERROR;
      PQ_orderby_decoded_frame decoded;
      failed = read_orderby_frame_from_worker_shape(&detached_handle, &status,
                                                    &decoded) ||
               status != PQ_orderby_loader_status::DETACHED;
      detached_handle.cleanup();
    }
  }

  if (!failed) {
    failed = run_orderby_read_mq_message_controlled_smoke();
  }

  handle.cleanup();
  return failed || *rows_read != 2 || *finishes_read != 1 ||
         *errors_read != 1;
}

bool Exchange_sort::run_orderby_read_mq_message_controlled_smoke() {
  MQMessageType type = MQMessageType::ERROR;
  void *data = reinterpret_cast<void *>(1);
  uint32 data_len = 99;
  if (read_mq_message(type, &data, data_len) || type != MQMessageType::FINISH ||
      data != nullptr || data_len != 0) {
    return true;
  }

  bool initialized_here = false;
  if (m_mq_handles == nullptr) {
    if (init()) return true;
    initialized_here = true;
  }

  enable_orderby_read_mq_shape_for_smoke(true);
  bool failed = false;
  if (read_mq_message(type, &data, data_len) ||
      type != MQMessageType::FINISH || data != nullptr || data_len != 0) {
    failed = true;
  }

  const int64 record0 = 100;
  const uint32 rowid0 = 10;
  const int64 sortkey0 = 1;
  failed = failed ||
           pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                                 &record0, sizeof(record0), &rowid0,
                                 sizeof(rowid0), &sortkey0,
                                 sizeof(sortkey0)) ||
           !read_mq_message(type, &data, data_len) ||
           type != MQMessageType::ROW || data == nullptr ||
           data_len != sizeof(record0) ||
           memcmp(data, &record0, sizeof(record0)) != 0;

  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             read_mq_message(type, &data, data_len) ||
             type != MQMessageType::FINISH || data != nullptr || data_len != 0;
  }

  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0),
                                   PQ_orderby_frame_type::ERROR, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             !read_mq_message(type, &data, data_len) ||
             type != MQMessageType::ERROR || data != nullptr || data_len != 0;
  }

  if (!failed) {
    PQ_orderby_frame_header invalid{};
    invalid.magic = PQ_MQ_MESSAGE_MAGIC;
    invalid.version = PQ_ORDERBY_FRAME_VERSION;
    invalid.type = pq_orderby_frame_type_to_uint(PQ_orderby_frame_type::ROW);
    invalid.record_image_len = sizeof(record0);
    invalid.payload_len = sizeof(record0);
    failed = get_mq_handle(0)->send(&invalid, sizeof(invalid)) != MQ_SUCCESS ||
             !read_mq_message(type, &data, data_len) ||
             type != MQMessageType::ERROR || data != nullptr || data_len != 0;
  }

  enable_orderby_read_mq_shape_for_smoke(false);
  if (initialized_here) cleanup();

  Exchange_sort detached_exchange(1, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed && detached_exchange.init()) failed = true;
  if (!failed) {
    detached_exchange.enable_orderby_read_mq_shape_for_smoke(true);
    detached_exchange.get_mq_handle(0)->close_producer();
    failed = detached_exchange.read_mq_message(type, &data, data_len) ||
             type != MQMessageType::FINISH || data != nullptr || data_len != 0;
    detached_exchange.enable_orderby_read_mq_shape_for_smoke(false);
  }
  detached_exchange.cleanup();

  return failed;
}

bool Exchange_sort::run_orderby_rich_status_api_smoke() {
  std::vector<uchar> row_image;
  PQ_orderby_ordered_read_status rich_status =
      PQ_orderby_ordered_read_status::ERROR;

  bool failed = read_ordered_record_rich_status_shape(&row_image,
                                                      &rich_status) ||
                rich_status != PQ_orderby_ordered_read_status::DISABLED ||
                !row_image.empty();

  m_orderby_rich_status_shape_enabled = true;
  failed = failed ||
           read_ordered_record_rich_status_shape(&row_image, &rich_status) ||
           rich_status != PQ_orderby_ordered_read_status::UNSUPPORTED ||
           !row_image.empty();
  m_orderby_rich_status_shape_enabled = false;
  cleanup_order_gather_shape();

  constexpr uint32 kWorkers = 3;
  constexpr uint32 kSortOrderLength = 2;
  constexpr uint32 kMaxRecordLength = 64;
  constexpr uint32 kRefLength = 8;
  const uint32 rowid = 1;
  const int64 record1 = 100;
  const int64 record2 = 200;
  const int64 key1 = 1;
  const int64 key2 = 2;

  Exchange_sort row_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) {
    failed = row_exchange.init() ||
             row_exchange.init_real_init_state_owner_shape(
                 kWorkers, /*stable_output=*/true, /*index_sort=*/false,
                 kSortOrderLength, kMaxRecordLength, kRefLength) ||
             row_exchange.allocate_real_init_buffers_shape() ||
             row_exchange.init_orderby_heap_reader_state_shape(
                 kWorkers, /*descending=*/false);
  }
  if (!failed) {
    row_exchange.m_orderby_rich_status_shape_enabled = true;
    failed =
        pq_send_orderby_frame(row_exchange.get_mq_handle(0),
                              PQ_orderby_frame_type::ROW, &record1,
                              sizeof(record1), &rowid, sizeof(rowid), &key1,
                              sizeof(key1)) ||
        pq_send_orderby_frame(row_exchange.get_mq_handle(0),
                              PQ_orderby_frame_type::FINISH, nullptr, 0,
                              nullptr, 0, nullptr, 0) ||
        pq_send_orderby_frame(row_exchange.get_mq_handle(2),
                              PQ_orderby_frame_type::ROW, &record2,
                              sizeof(record2), &rowid, sizeof(rowid), &key2,
                              sizeof(key2)) ||
        pq_send_orderby_frame(row_exchange.get_mq_handle(2),
                              PQ_orderby_frame_type::FINISH, nullptr, 0,
                              nullptr, 0, nullptr, 0);
  }
  if (!failed) {
    failed = row_exchange.read_ordered_record_rich_status_shape(
                 &row_image, &rich_status) ||
             rich_status != PQ_orderby_ordered_read_status::WOULD_BLOCK ||
             !row_image.empty();
  }
  if (!failed) {
    failed =
        pq_send_orderby_frame(row_exchange.get_mq_handle(1),
                              PQ_orderby_frame_type::FINISH, nullptr, 0,
                              nullptr, 0, nullptr, 0);
  }
  const int64 expected_rows[] = {record1, record2};
  for (const int64 expected_row : expected_rows) {
    if (failed) break;
    failed = row_exchange.read_ordered_record_rich_status_shape(
                 &row_image, &rich_status) ||
             rich_status != PQ_orderby_ordered_read_status::ROW ||
             row_image.size() != sizeof(expected_row);
    if (!failed) {
      int64 actual_row = 0;
      memcpy(&actual_row, row_image.data(), sizeof(actual_row));
      failed = actual_row != expected_row;
    }
  }
  if (!failed) {
    failed = row_exchange.read_ordered_record_rich_status_shape(
                 &row_image, &rich_status) ||
             rich_status != PQ_orderby_ordered_read_status::EOF_REACHED ||
             !row_image.empty();
  }
  row_exchange.cleanup_order_gather_shape();
  row_exchange.cleanup();

  Exchange_sort error_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) {
    failed = error_exchange.init() ||
             error_exchange.init_real_init_state_owner_shape(
                 kWorkers, /*stable_output=*/true, /*index_sort=*/false,
                 kSortOrderLength, kMaxRecordLength, kRefLength) ||
             error_exchange.allocate_real_init_buffers_shape() ||
             error_exchange.init_orderby_heap_reader_state_shape(
                 kWorkers, /*descending=*/false);
  }
  if (!failed) {
    error_exchange.m_orderby_rich_status_shape_enabled = true;
    failed =
        pq_send_orderby_frame(error_exchange.get_mq_handle(0),
                              PQ_orderby_frame_type::ERROR, nullptr, 0,
                              nullptr, 0, nullptr, 0) ||
        error_exchange.read_ordered_record_rich_status_shape(&row_image,
                                                             &rich_status) ||
        rich_status != PQ_orderby_ordered_read_status::ERROR ||
        !row_image.empty();
  }
  error_exchange.cleanup_order_gather_shape();
  error_exchange.cleanup();

  Exchange_sort detached_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) {
    failed = detached_exchange.init() ||
             detached_exchange.init_real_init_state_owner_shape(
                 kWorkers, /*stable_output=*/true, /*index_sort=*/false,
                 kSortOrderLength, kMaxRecordLength, kRefLength) ||
             detached_exchange.allocate_real_init_buffers_shape() ||
             detached_exchange.init_orderby_heap_reader_state_shape(
                 kWorkers, /*descending=*/false);
  }
  if (!failed) {
    detached_exchange.m_orderby_rich_status_shape_enabled = true;
    detached_exchange.get_mq_handle(0)->close_producer();
    failed = detached_exchange.read_ordered_record_rich_status_shape(
                 &row_image, &rich_status) ||
             rich_status != PQ_orderby_ordered_read_status::DETACHED ||
             !row_image.empty();
  }
  detached_exchange.cleanup_order_gather_shape();
  detached_exchange.cleanup();

  return failed;
}

bool Exchange_sort::run_orderby_default_heap_reader_contract_smoke() {
  constexpr uint32 kWorkers = 3;
  constexpr uint32 kSortOrderLength = 2;
  constexpr uint32 kMaxRecordLength = 64;
  constexpr uint32 kRefLength = 8;
  const uint32 rowid = 1;
  const int64 record_low = 100;
  const int64 record_high = 200;
  const int64 key_low = 1;
  const int64 key_high = 2;

  std::vector<uchar> row_image;
  PQ_orderby_ordered_read_status status =
      PQ_orderby_ordered_read_status::ERROR;

  bool failed = read_default_ordered_record_heap_shape(&row_image, &status) ||
                status != PQ_orderby_ordered_read_status::DISABLED ||
                !row_image.empty();

  m_orderby_default_heap_reader_shape_enabled = true;
  if (!failed) {
    failed = read_default_ordered_record_heap_shape(&row_image, &status) ||
             status != PQ_orderby_ordered_read_status::UNSUPPORTED ||
             !row_image.empty();
  }
  m_orderby_default_heap_reader_shape_enabled = false;
  cleanup_order_gather_shape();

  auto setup_exchange = [&](Exchange_sort *exchange) {
    return exchange == nullptr || exchange->init() ||
           exchange->init_real_init_state_owner_shape(
               kWorkers, /*stable_output=*/true, /*index_sort=*/false,
               kSortOrderLength, kMaxRecordLength, kRefLength) ||
           exchange->allocate_real_init_buffers_shape() ||
           exchange->init_orderby_heap_reader_state_shape(
               kWorkers, /*descending=*/false);
  };

  auto enable_default_heap_reader = [](Exchange_sort *exchange) {
    exchange->m_orderby_rich_status_shape_enabled = true;
    exchange->m_orderby_default_heap_reader_shape_enabled = true;
  };

  Exchange_sort row_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) failed = setup_exchange(&row_exchange);
  if (!failed) {
    enable_default_heap_reader(&row_exchange);
    PQ_orderby_worker_frame_producer_owner worker0{
        row_exchange.get_mq_handle(0), 0};
    PQ_orderby_worker_frame_producer_owner worker2{
        row_exchange.get_mq_handle(2), 2};
    failed =
        pq_orderby_worker_frame_producer_owner_emit_row(
            &worker0, &record_high, sizeof(record_high), &rowid,
            sizeof(rowid), key_high) ||
        pq_orderby_worker_frame_producer_owner_finish(&worker0) ||
        pq_orderby_worker_frame_producer_owner_emit_row(
            &worker2, &record_low, sizeof(record_low), &rowid, sizeof(rowid),
            key_low) ||
        pq_orderby_worker_frame_producer_owner_finish(&worker2);
  }
  if (!failed) {
    failed = row_exchange.read_default_ordered_record_heap_shape(
                 &row_image, &status) ||
             status != PQ_orderby_ordered_read_status::WOULD_BLOCK ||
             !row_image.empty();
  }
  if (!failed) {
    PQ_orderby_worker_frame_producer_owner worker1{
        row_exchange.get_mq_handle(1), 1};
    failed = pq_orderby_worker_frame_producer_owner_finish(&worker1);
  }
  const int64 expected_rows[] = {record_low, record_high};
  for (const int64 expected_row : expected_rows) {
    if (failed) break;
    failed = row_exchange.read_default_ordered_record_heap_shape(
                 &row_image, &status) ||
             status != PQ_orderby_ordered_read_status::ROW ||
             row_image.size() != sizeof(expected_row);
    if (!failed) {
      int64 actual_row = 0;
      memcpy(&actual_row, row_image.data(), sizeof(actual_row));
      failed = actual_row != expected_row;
    }
  }
  if (!failed) {
    failed = row_exchange.read_default_ordered_record_heap_shape(
                 &row_image, &status) ||
             status != PQ_orderby_ordered_read_status::EOF_REACHED ||
             !row_image.empty();
  }
  if (!failed) {
    failed = row_exchange.read_default_ordered_record_heap_shape(
                 &row_image, &status) ||
             status != PQ_orderby_ordered_read_status::DISABLED ||
             !row_image.empty();
  }
  row_exchange.cleanup_order_gather_shape();
  row_exchange.cleanup();

  Exchange_sort error_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) failed = setup_exchange(&error_exchange);
  if (!failed) {
    enable_default_heap_reader(&error_exchange);
    PQ_orderby_worker_frame_producer_owner worker0{
        error_exchange.get_mq_handle(0), 0};
    failed = pq_orderby_worker_frame_producer_owner_error(&worker0) ||
             error_exchange.read_default_ordered_record_heap_shape(
                 &row_image, &status) ||
             status != PQ_orderby_ordered_read_status::ERROR ||
             !row_image.empty() ||
             error_exchange.read_default_ordered_record_heap_shape(
                 &row_image, &status) ||
             status != PQ_orderby_ordered_read_status::DISABLED;
  }
  error_exchange.cleanup_order_gather_shape();
  error_exchange.cleanup();

  Exchange_sort detached_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) failed = setup_exchange(&detached_exchange);
  if (!failed) {
    enable_default_heap_reader(&detached_exchange);
    PQ_orderby_worker_frame_producer_owner worker0{
        detached_exchange.get_mq_handle(0), 0};
    failed = pq_orderby_worker_frame_producer_owner_detach(&worker0) ||
             detached_exchange.read_default_ordered_record_heap_shape(
                 &row_image, &status) ||
             status != PQ_orderby_ordered_read_status::DETACHED ||
             !row_image.empty() ||
             detached_exchange.read_default_ordered_record_heap_shape(
                 &row_image, &status) ||
             status != PQ_orderby_ordered_read_status::DISABLED;
  }
  detached_exchange.cleanup_order_gather_shape();
  detached_exchange.cleanup();

  return failed;
}

bool Exchange_sort::run_orderby_ordered_diag_contract_smoke(
    PQ_orderby_ordered_diag_contract_shape *diag) {
  if (diag == nullptr) return true;
  *diag = PQ_orderby_ordered_diag_contract_shape{};

  constexpr uint32 kWorkers = 3;
  constexpr uint32 kSortOrderLength = 2;
  constexpr uint32 kMaxRecordLength = 64;
  constexpr uint32 kRefLength = 8;
  const uint32 rowid = 1;
  const int64 record_low = 100;
  const int64 record_high = 200;
  const int64 key_low = 1;
  const int64 key_high = 2;

  std::vector<uchar> row_image;
  PQ_orderby_ordered_read_status status =
      PQ_orderby_ordered_read_status::ERROR;

  auto setup_exchange = [&](Exchange_sort *exchange) {
    return exchange == nullptr || exchange->init() ||
           exchange->init_real_init_state_owner_shape(
               kWorkers, /*stable_output=*/true, /*index_sort=*/false,
               kSortOrderLength, kMaxRecordLength, kRefLength) ||
           exchange->allocate_real_init_buffers_shape() ||
           exchange->init_orderby_heap_reader_state_shape(
               kWorkers, /*descending=*/false);
  };

  auto enable_default_heap_reader = [](Exchange_sort *exchange) {
    exchange->m_orderby_rich_status_shape_enabled = true;
    exchange->m_orderby_default_heap_reader_shape_enabled = true;
  };

  bool failed = false;
  Exchange_sort would_block_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  failed = setup_exchange(&would_block_exchange);
  if (!failed) {
    enable_default_heap_reader(&would_block_exchange);
    PQ_orderby_worker_frame_producer_owner worker0{
        would_block_exchange.get_mq_handle(0), 0};
    PQ_orderby_worker_frame_producer_owner worker2{
        would_block_exchange.get_mq_handle(2), 2};
    failed =
        pq_orderby_worker_frame_producer_owner_emit_row(
            &worker0, &record_high, sizeof(record_high), &rowid,
            sizeof(rowid), key_high) ||
        pq_orderby_worker_frame_producer_owner_finish(&worker0) ||
        pq_orderby_worker_frame_producer_owner_emit_row(
            &worker2, &record_low, sizeof(record_low), &rowid, sizeof(rowid),
            key_low) ||
        pq_orderby_worker_frame_producer_owner_finish(&worker2);
  }
  if (!failed) {
    failed = would_block_exchange.read_default_ordered_record_heap_shape(
        &row_image, &status);
    diag->would_block_distinct_from_eof =
        !failed && status == PQ_orderby_ordered_read_status::WOULD_BLOCK &&
        row_image.empty() &&
        would_block_exchange.m_orderby_default_heap_reader_shape_enabled;
  }
  would_block_exchange.cleanup_order_gather_shape();
  would_block_exchange.cleanup();

  Exchange_sort error_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) failed = setup_exchange(&error_exchange);
  if (!failed) {
    enable_default_heap_reader(&error_exchange);
    PQ_orderby_worker_frame_producer_owner worker0{
        error_exchange.get_mq_handle(0), 0};
    failed = pq_orderby_worker_frame_producer_owner_error(&worker0) ||
             error_exchange.read_default_ordered_record_heap_shape(
                 &row_image, &status);
    const bool saw_error =
        !failed && status == PQ_orderby_ordered_read_status::ERROR &&
        row_image.empty();
    if (!failed) {
      failed = error_exchange.read_default_ordered_record_heap_shape(
          &row_image, &status);
    }
    diag->error_cleanup_ready =
        saw_error && !failed &&
        status == PQ_orderby_ordered_read_status::DISABLED &&
        row_image.empty();
  }
  error_exchange.cleanup_order_gather_shape();
  error_exchange.cleanup();

  Exchange_sort detached_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) failed = setup_exchange(&detached_exchange);
  if (!failed) {
    enable_default_heap_reader(&detached_exchange);
    PQ_orderby_worker_frame_producer_owner worker0{
        detached_exchange.get_mq_handle(0), 0};
    failed = pq_orderby_worker_frame_producer_owner_detach(&worker0) ||
             detached_exchange.read_default_ordered_record_heap_shape(
                 &row_image, &status);
    const bool saw_detach =
        !failed && status == PQ_orderby_ordered_read_status::DETACHED &&
        row_image.empty();
    if (!failed) {
      failed = detached_exchange.read_default_ordered_record_heap_shape(
          &row_image, &status);
    }
    diag->detach_cleanup_ready =
        saw_detach && !failed &&
        status == PQ_orderby_ordered_read_status::DISABLED &&
        row_image.empty();
  }
  detached_exchange.cleanup_order_gather_shape();
  detached_exchange.cleanup();

  diag->kill_polling_wired = false;
  diag->default_read_ready = false;
  diag->diagnostics_ready = false;
  return failed;
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

bool Exchange_sort::run_orderby_streaming_materialization_smoke(
    TABLE *leader_table, uint32 *rows_read, uint32 *unsupported,
    uint32 *length_errors) {
  if (rows_read == nullptr || unsupported == nullptr ||
      length_errors == nullptr) {
    return true;
  }
  *rows_read = 0;
  *unsupported = 0;
  *length_errors = 0;

  if (!pq_orderby_materialization_table_supported(leader_table)) {
    *unsupported = 1;
    return false;
  }

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
  std::vector<uchar> record_low;
  std::vector<uchar> record_high;

  constexpr longlong low_value = 111;
  constexpr longlong high_value = 222;
  bool failed = first_field->store(high_value, false) != TYPE_OK;
  if (!failed) {
    record_high.assign(leader_table->record[0],
                       leader_table->record[0] + leader_table->s->reclength);
    failed = first_field->store(low_value, false) != TYPE_OK;
  }
  if (!failed) {
    record_low.assign(leader_table->record[0],
                      leader_table->record[0] + leader_table->s->reclength);
  }
  if (!original_record.empty()) {
    memcpy(leader_table->record[0], original_record.data(),
           original_record.size());
  }

  const uint32 rowid = 1;
  const int64 key_low = 1;
  const int64 key_high = 2;
  if (!failed) {
    failed = init_real_init_state_owner_shape(kWorkers, /*stable_output=*/true,
                                             /*index_sort=*/false,
                                             kSortOrderLength, kMaxRecordLength,
                                             kRefLength) ||
             allocate_real_init_buffers_shape();
  }
  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                                   record_high.data(),
                                   static_cast<uint32>(record_high.size()),
                                   &rowid, sizeof(rowid), &key_high,
                                   sizeof(key_high)) ||
             pq_send_orderby_frame(get_mq_handle(0),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             pq_send_orderby_frame(get_mq_handle(1), PQ_orderby_frame_type::ROW,
                                   record_low.data(),
                                   static_cast<uint32>(record_low.size()),
                                   &rowid, sizeof(rowid), &key_low,
                                   sizeof(key_low)) ||
             pq_send_orderby_frame(get_mq_handle(1),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             pq_send_orderby_frame(get_mq_handle(2),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0);
  }

  PQ_orderby_cached_merge_ctx ctx{m_record_groups.data(),
                                  /*descending=*/false};
  binary_heap heap(static_cast<int>(kWorkers), &ctx,
                   pq_orderby_cached_compare_batches);
  std::vector<bool> in_heap(kWorkers, false);
  std::vector<bool> terminal_workers(kWorkers, false);
  if (!failed && heap.init_binary_heap()) failed = true;

  uint32 finishes = 0;
  uint32 would_blocks = 0;
  uint32 errors = 0;
  uint32 detaches = 0;
  uint32 refills = 0;
  uint32 heap_replaces = 0;
  uint32 heap_removes = 0;
  std::vector<uchar> row_image;
  PQ_orderby_stream_read_status status = PQ_orderby_stream_read_status::ERROR;
  const longlong expected_values[] = {low_value, high_value};
  for (const longlong expected : expected_values) {
    if (failed) break;
    failed = read_ordered_record_stream_shape(
                 &heap, &in_heap, &terminal_workers, &row_image, &status,
                 &finishes, &would_blocks, &errors, &detaches, &refills,
                 &heap_replaces, &heap_removes) ||
             status != PQ_orderby_stream_read_status::ROW ||
             row_image.size() != leader_table->s->reclength;
    if (!failed) {
      memcpy(leader_table->record[0], row_image.data(), row_image.size());
      failed = first_field->val_int() != expected;
    }
    if (!failed) ++(*rows_read);
  }
  if (!failed) {
    failed = read_ordered_record_stream_shape(
                 &heap, &in_heap, &terminal_workers, &row_image, &status,
                 &finishes, &would_blocks, &errors, &detaches, &refills,
                 &heap_replaces, &heap_removes) ||
             status != PQ_orderby_stream_read_status::EOF_REACHED ||
             !row_image.empty();
  }

  cleanup_order_gather_shape();
  heap.reset();
  std::fill(in_heap.begin(), in_heap.end(), false);
  std::fill(terminal_workers.begin(), terminal_workers.end(), false);
  row_image.clear();
  status = PQ_orderby_stream_read_status::ERROR;
  if (!failed) {
    failed = init_real_init_state_owner_shape(kWorkers, /*stable_output=*/true,
                                             /*index_sort=*/false,
                                             kSortOrderLength, kMaxRecordLength,
                                             kRefLength) ||
             allocate_real_init_buffers_shape();
  }
  ctx.batches = m_record_groups.data();
  const uint32 short_record = 333;
  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                                   &short_record, sizeof(short_record), &rowid,
                                   sizeof(rowid), &key_low, sizeof(key_low)) ||
             pq_send_orderby_frame(get_mq_handle(0),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             pq_send_orderby_frame(get_mq_handle(1),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             pq_send_orderby_frame(get_mq_handle(2),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0);
  }
  if (!failed) {
    failed = read_ordered_record_stream_shape(
                 &heap, &in_heap, &terminal_workers, &row_image, &status,
                 &finishes, &would_blocks, &errors, &detaches, &refills,
                 &heap_replaces, &heap_removes) ||
             status != PQ_orderby_stream_read_status::ROW;
  }
  if (!failed && row_image.size() != leader_table->s->reclength) {
    ++(*length_errors);
  } else if (!failed) {
    failed = true;
  }

  if (!original_record.empty()) {
    memcpy(leader_table->record[0], original_record.data(),
           original_record.size());
  }
  cleanup_order_gather_shape();
  if (initialized_here) cleanup();
  if (leader_table->read_set != nullptr && !had_read_bit) {
    bitmap_clear_bit(leader_table->read_set, field_index);
  }
  if (leader_table->write_set != nullptr && !had_write_bit) {
    bitmap_clear_bit(leader_table->write_set, field_index);
  }

  return failed || *rows_read != 2 || *length_errors != 1;
}

bool Exchange_sort::materialize_next_ordered_record_image_status(
    TABLE *leader_table, PQ_orderby_materialize_status *status) {
  if (status != nullptr) *status = PQ_orderby_materialize_status::ERROR;
  if (leader_table == nullptr || status == nullptr) return true;

  if (m_orderby_materializer_shape_enabled) {
    std::vector<uchar> row_image;
    PQ_orderby_ordered_read_status rich_status =
        PQ_orderby_ordered_read_status::ERROR;
    if (read_ordered_record_rich_status_shape(&row_image, &rich_status)) {
      return true;
    }

    switch (rich_status) {
      case PQ_orderby_ordered_read_status::ROW:
        if (materialize_ordered_record_owner_shape(leader_table, row_image,
                                                   status)) {
          return true;
        }
        if (*status == PQ_orderby_materialize_status::ERROR) {
          cleanup_order_gather_shape();
        }
        return false;
      case PQ_orderby_ordered_read_status::EOF_REACHED:
        cleanup_order_gather_shape();
        *status = PQ_orderby_materialize_status::EOF_REACHED;
        return false;
      case PQ_orderby_ordered_read_status::WOULD_BLOCK:
        *status = PQ_orderby_materialize_status::WOULD_BLOCK;
        return false;
      case PQ_orderby_ordered_read_status::DETACHED:
        cleanup_order_gather_shape();
        *status = PQ_orderby_materialize_status::DETACHED;
        return false;
      case PQ_orderby_ordered_read_status::UNSUPPORTED:
        *status = PQ_orderby_materialize_status::UNSUPPORTED;
        return false;
      case PQ_orderby_ordered_read_status::DISABLED:
        *status = PQ_orderby_materialize_status::DISABLED;
        return false;
      case PQ_orderby_ordered_read_status::ERROR:
        cleanup_order_gather_shape();
        *status = PQ_orderby_materialize_status::ERROR;
        return false;
    }

    *status = PQ_orderby_materialize_status::ERROR;
    return false;
  }

  /*
    M11-E5g-4b is a default-path boundary only. The ordered materializer must
    stay fail-closed until the default reader, worker producer, and iterator
    lifecycle are wired by later reviewed phases.
  */
  *status = PQ_orderby_materialize_status::DISABLED;
  return false;
}

bool Exchange_sort::run_orderby_materializer_owner_shape_smoke(
    TABLE *leader_table) {
  if (!pq_orderby_materialization_table_supported(leader_table)) return false;

  constexpr uint32 kWorkers = 3;
  constexpr uint32 kSortOrderLength = 2;
  constexpr uint32 kMaxRecordLength = 64;
  constexpr uint32 kRefLength = 8;
  const uint32 rowid = 1;
  const int64 key_low = 1;
  const int64 key_high = 2;

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
  std::vector<uchar> record_low;
  std::vector<uchar> record_high;

  constexpr longlong low_value = 111;
  constexpr longlong high_value = 222;
  bool failed = first_field->store(high_value, false) != TYPE_OK;
  if (!failed) {
    record_high.assign(leader_table->record[0],
                       leader_table->record[0] + leader_table->s->reclength);
    failed = first_field->store(low_value, false) != TYPE_OK;
  }
  if (!failed) {
    record_low.assign(leader_table->record[0],
                      leader_table->record[0] + leader_table->s->reclength);
  }
  if (!original_record.empty()) {
    memcpy(leader_table->record[0], original_record.data(),
           original_record.size());
  }

  auto setup_exchange = [&](Exchange_sort *exchange) {
    return exchange == nullptr || exchange->init() ||
           exchange->init_real_init_state_owner_shape(
               kWorkers, /*stable_output=*/true, /*index_sort=*/false,
               kSortOrderLength, kMaxRecordLength, kRefLength) ||
           exchange->allocate_real_init_buffers_shape() ||
           exchange->init_orderby_heap_reader_state_shape(
               kWorkers, /*descending=*/false) ||
           exchange->init_orderby_materializer_owner_shape(leader_table);
  };

  auto enable_materializer = [](Exchange_sort *exchange) {
    exchange->m_orderby_rich_status_shape_enabled = true;
    exchange->m_orderby_materializer_shape_enabled = true;
  };

  auto record_restored = [&]() {
    return original_record.size() == leader_table->s->reclength &&
           memcmp(leader_table->record[0], original_record.data(),
                  original_record.size()) == 0;
  };

  Exchange_sort row_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) failed = setup_exchange(&row_exchange);
  if (!failed) {
    enable_materializer(&row_exchange);
    failed =
        pq_send_orderby_frame(row_exchange.get_mq_handle(0),
                              PQ_orderby_frame_type::ROW, record_high.data(),
                              static_cast<uint32>(record_high.size()), &rowid,
                              sizeof(rowid), &key_high, sizeof(key_high)) ||
        pq_send_orderby_frame(row_exchange.get_mq_handle(0),
                              PQ_orderby_frame_type::FINISH, nullptr, 0,
                              nullptr, 0, nullptr, 0) ||
        pq_send_orderby_frame(row_exchange.get_mq_handle(1),
                              PQ_orderby_frame_type::ROW, record_low.data(),
                              static_cast<uint32>(record_low.size()), &rowid,
                              sizeof(rowid), &key_low, sizeof(key_low)) ||
        pq_send_orderby_frame(row_exchange.get_mq_handle(1),
                              PQ_orderby_frame_type::FINISH, nullptr, 0,
                              nullptr, 0, nullptr, 0);
  }

  PQ_orderby_materialize_status status =
      PQ_orderby_materialize_status::ERROR;
  if (!failed) {
    failed = row_exchange.materialize_next_ordered_record_image_status(
                 leader_table, &status) ||
             status != PQ_orderby_materialize_status::WOULD_BLOCK ||
             !record_restored();
  }
  if (!failed) {
    failed =
        pq_send_orderby_frame(row_exchange.get_mq_handle(2),
                              PQ_orderby_frame_type::FINISH, nullptr, 0,
                              nullptr, 0, nullptr, 0);
  }
  const longlong expected_values[] = {low_value, high_value};
  for (const longlong expected : expected_values) {
    if (failed) break;
    failed = row_exchange.materialize_next_ordered_record_image_status(
                 leader_table, &status) ||
             status != PQ_orderby_materialize_status::ROW ||
             first_field->val_int() != expected;
  }
  if (!failed) {
    failed = row_exchange.materialize_next_ordered_record_image_status(
                 leader_table, &status) ||
             status != PQ_orderby_materialize_status::EOF_REACHED ||
             !record_restored();
  }
  row_exchange.cleanup_order_gather_shape();
  row_exchange.cleanup();

  Exchange_sort short_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) failed = setup_exchange(&short_exchange);
  if (!failed) {
    enable_materializer(&short_exchange);
    const uint32 short_record = 333;
    failed =
        pq_send_orderby_frame(short_exchange.get_mq_handle(0),
                              PQ_orderby_frame_type::ROW, &short_record,
                              sizeof(short_record), &rowid, sizeof(rowid),
                              &key_low, sizeof(key_low)) ||
        pq_send_orderby_frame(short_exchange.get_mq_handle(0),
                              PQ_orderby_frame_type::FINISH, nullptr, 0,
                              nullptr, 0, nullptr, 0) ||
        pq_send_orderby_frame(short_exchange.get_mq_handle(1),
                              PQ_orderby_frame_type::FINISH, nullptr, 0,
                              nullptr, 0, nullptr, 0) ||
        pq_send_orderby_frame(short_exchange.get_mq_handle(2),
                              PQ_orderby_frame_type::FINISH, nullptr, 0,
                              nullptr, 0, nullptr, 0);
  }
  if (!failed) {
    failed = short_exchange.materialize_next_ordered_record_image_status(
                 leader_table, &status) ||
             status != PQ_orderby_materialize_status::ERROR ||
             !record_restored();
  }
  short_exchange.cleanup_order_gather_shape();
  short_exchange.cleanup();

  Exchange_sort error_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) failed = setup_exchange(&error_exchange);
  if (!failed) {
    enable_materializer(&error_exchange);
    failed =
        pq_send_orderby_frame(error_exchange.get_mq_handle(0),
                              PQ_orderby_frame_type::ERROR, nullptr, 0,
                              nullptr, 0, nullptr, 0) ||
        error_exchange.materialize_next_ordered_record_image_status(
            leader_table, &status) ||
        status != PQ_orderby_materialize_status::ERROR || !record_restored();
  }
  error_exchange.cleanup_order_gather_shape();
  error_exchange.cleanup();

  Exchange_sort detached_exchange(kWorkers, PQ_MQ_DEFAULT_RING_SIZE);
  if (!failed) failed = setup_exchange(&detached_exchange);
  if (!failed) {
    enable_materializer(&detached_exchange);
    detached_exchange.get_mq_handle(0)->close_producer();
    failed = detached_exchange.materialize_next_ordered_record_image_status(
                 leader_table, &status) ||
             status != PQ_orderby_materialize_status::DETACHED ||
             !record_restored();
  }
  detached_exchange.cleanup_order_gather_shape();
  detached_exchange.cleanup();

  if (!original_record.empty()) {
    memcpy(leader_table->record[0], original_record.data(),
           original_record.size());
  }
  if (leader_table->read_set != nullptr && !had_read_bit) {
    bitmap_clear_bit(leader_table->read_set, field_index);
  }
  if (leader_table->write_set != nullptr && !had_write_bit) {
    bitmap_clear_bit(leader_table->write_set, field_index);
  }

  return failed;
}

bool Exchange_sort::run_orderby_materialize_api_skeleton_smoke(
    TABLE *leader_table, uint32 *disabled, uint32 *unsupported,
    uint32 *rows_read) {
  if (disabled == nullptr || unsupported == nullptr || rows_read == nullptr) {
    return true;
  }
  *disabled = 0;
  *unsupported = 0;
  *rows_read = 0;

  PQ_orderby_materialize_status status = PQ_orderby_materialize_status::ERROR;
  if (materialize_next_ordered_record_image_status(leader_table, &status)) {
    return true;
  }
  if (status == PQ_orderby_materialize_status::DISABLED) {
    *disabled = 1;
    return run_orderby_materializer_owner_shape_smoke(leader_table);
  }
  if (status == PQ_orderby_materialize_status::UNSUPPORTED) {
    *unsupported = 1;
    return false;
  }
  return true;
}

bool Exchange_sort::read_next_ordered_record_image_skeleton(
    binary_heap *heap, std::vector<bool> *in_heap,
    std::vector<bool> *terminal_workers, std::vector<uchar> *row_image,
    PQ_orderby_stream_read_status *status, uint32 *finishes_read,
    uint32 *would_blocks_read, uint32 *errors_read, uint32 *detaches_read,
    uint32 *refills_read, uint32 *heap_replaces_read,
    uint32 *heap_removes_read) {
  /*
    This is an ordered-reader boundary only. It delegates to the controlled
    stream-shape reader and deliberately has no TABLE, default Read(), or
    materialization side effect.
  */
  return read_ordered_record_stream_shape(
      heap, in_heap, terminal_workers, row_image, status, finishes_read,
      would_blocks_read, errors_read, detaches_read, refills_read,
      heap_replaces_read, heap_removes_read);
}

bool Exchange_sort::run_orderby_ordered_reader_skeleton_smoke(
    uint32 *rows_read, uint32 *finishes_read, uint32 *would_blocks_read,
    uint32 *errors_read, uint32 *detaches_read, uint32 *refills_read,
    uint32 *heap_replaces_read, uint32 *heap_removes_read) {
  if (rows_read == nullptr || finishes_read == nullptr ||
      would_blocks_read == nullptr || errors_read == nullptr ||
      detaches_read == nullptr || refills_read == nullptr ||
      heap_replaces_read == nullptr || heap_removes_read == nullptr) {
    return true;
  }
  *rows_read = 0;
  *finishes_read = 0;
  *would_blocks_read = 0;
  *errors_read = 0;
  *detaches_read = 0;
  *refills_read = 0;
  *heap_replaces_read = 0;
  *heap_removes_read = 0;

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

  const int64 record1 = 100;
  const int64 record2 = 200;
  const int64 record3 = 300;
  const int64 record4 = 400;
  const uint32 rowid = 1;
  const int64 key1 = 1;
  const int64 key2 = 2;
  const int64 key3 = 3;
  const int64 key4 = 4;

  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                                   &record1, sizeof(record1), &rowid,
                                   sizeof(rowid), &key1, sizeof(key1)) ||
             pq_send_orderby_frame(get_mq_handle(0), PQ_orderby_frame_type::ROW,
                                   &record4, sizeof(record4), &rowid,
                                   sizeof(rowid), &key4, sizeof(key4)) ||
             pq_send_orderby_frame(get_mq_handle(0),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0) ||
             pq_send_orderby_frame(get_mq_handle(2), PQ_orderby_frame_type::ROW,
                                   &record2, sizeof(record2), &rowid,
                                   sizeof(rowid), &key2, sizeof(key2)) ||
             pq_send_orderby_frame(get_mq_handle(2),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0);
  }

  if (!failed) {
    failed = init_orderby_heap_reader_state_shape(kWorkers,
                                                  /*descending=*/false);
  }

  std::vector<uchar> row_image;
  PQ_orderby_stream_read_status status = PQ_orderby_stream_read_status::ERROR;
  if (!failed) {
    failed = read_next_ordered_record_image_owned_shape(&row_image, &status) ||
             status != PQ_orderby_stream_read_status::WOULD_BLOCK ||
             !row_image.empty();
  }
  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(1), PQ_orderby_frame_type::ROW,
                                   &record3, sizeof(record3), &rowid,
                                   sizeof(rowid), &key3, sizeof(key3)) ||
             pq_send_orderby_frame(get_mq_handle(1),
                                   PQ_orderby_frame_type::FINISH, nullptr, 0,
                                   nullptr, 0, nullptr, 0);
  }

  const int64 expected_rows[] = {record1, record2, record3, record4};
  for (const int64 expected_row : expected_rows) {
    if (failed) break;
    failed = read_next_ordered_record_image_owned_shape(&row_image, &status) ||
             status != PQ_orderby_stream_read_status::ROW ||
             row_image.size() != sizeof(expected_row);
    if (!failed) {
      int64 actual_row = 0;
      memcpy(&actual_row, row_image.data(), sizeof(actual_row));
      failed = actual_row != expected_row;
    }
    if (!failed) ++(*rows_read);
  }

  if (!failed) {
    failed = read_next_ordered_record_image_owned_shape(&row_image, &status) ||
             status != PQ_orderby_stream_read_status::EOF_REACHED ||
             !row_image.empty() ||
             m_heap_reader_counters.finishes_read != kWorkers;
  }

  *finishes_read += m_heap_reader_counters.finishes_read;
  *would_blocks_read += m_heap_reader_counters.would_blocks_read;
  *errors_read += m_heap_reader_counters.errors_read;
  *detaches_read += m_heap_reader_counters.detaches_read;
  *refills_read += m_heap_reader_counters.refills_read;
  *heap_replaces_read += m_heap_reader_counters.heap_replaces_read;
  *heap_removes_read += m_heap_reader_counters.heap_removes_read;
  cleanup_order_gather_shape();
  if (!failed) {
    failed = init_real_init_state_owner_shape(kWorkers, /*stable_output=*/true,
                                             /*index_sort=*/false,
                                             kSortOrderLength, kMaxRecordLength,
                                             kRefLength) ||
             allocate_real_init_buffers_shape();
  }
  if (!failed) {
    failed = init_orderby_heap_reader_state_shape(kWorkers,
                                                  /*descending=*/false);
  }
  if (!failed) {
    failed = pq_send_orderby_frame(get_mq_handle(0),
                                   PQ_orderby_frame_type::ERROR, nullptr, 0,
                                   nullptr, 0, nullptr, 0);
  }
  if (!failed) {
    failed = read_next_ordered_record_image_owned_shape(&row_image, &status) ||
             status != PQ_orderby_stream_read_status::ERROR ||
             !row_image.empty() ||
             m_heap_reader_counters.errors_read == 0;
  }

  *finishes_read += m_heap_reader_counters.finishes_read;
  *would_blocks_read += m_heap_reader_counters.would_blocks_read;
  *errors_read += m_heap_reader_counters.errors_read;
  *detaches_read += m_heap_reader_counters.detaches_read;
  *refills_read += m_heap_reader_counters.refills_read;
  *heap_replaces_read += m_heap_reader_counters.heap_replaces_read;
  *heap_removes_read += m_heap_reader_counters.heap_removes_read;
  cleanup_order_gather_shape();
  if (!failed) {
    failed = init_real_init_state_owner_shape(kWorkers, /*stable_output=*/true,
                                             /*index_sort=*/false,
                                             kSortOrderLength, kMaxRecordLength,
                                             kRefLength) ||
             allocate_real_init_buffers_shape();
  }
  if (!failed) {
    failed = init_orderby_heap_reader_state_shape(kWorkers,
                                                  /*descending=*/false);
  }
  if (!failed) {
    get_mq_handle(0)->close_producer();
    failed = read_next_ordered_record_image_owned_shape(&row_image, &status) ||
             status != PQ_orderby_stream_read_status::DETACHED ||
             !row_image.empty() ||
             m_heap_reader_counters.detaches_read == 0;
  }

  *finishes_read += m_heap_reader_counters.finishes_read;
  *would_blocks_read += m_heap_reader_counters.would_blocks_read;
  *errors_read += m_heap_reader_counters.errors_read;
  *detaches_read += m_heap_reader_counters.detaches_read;
  *refills_read += m_heap_reader_counters.refills_read;
  *heap_replaces_read += m_heap_reader_counters.heap_replaces_read;
  *heap_removes_read += m_heap_reader_counters.heap_removes_read;
  cleanup_order_gather_shape();
  if (initialized_here) cleanup();
  if (!failed) {
    failed = run_orderby_rich_status_api_smoke();
  }
  if (!failed) {
    failed = run_orderby_default_heap_reader_contract_smoke();
  }
  return failed || *rows_read != 4 || *finishes_read != 3 ||
         *would_blocks_read == 0 || *errors_read != 1 ||
         *detaches_read != 1 || *refills_read == 0 ||
         *heap_replaces_read == 0 || *heap_removes_read == 0;
}

bool Exchange_sort::run_orderby_ordered_diag_skeleton_smoke(
    uint32 *kill_not_wired) {
  if (kill_not_wired == nullptr) return true;
  PQ_orderby_ordered_diag_contract_shape diag;
  if (run_orderby_ordered_diag_contract_smoke(&diag)) return true;
  if (!diag.would_block_distinct_from_eof || !diag.error_cleanup_ready ||
      !diag.detach_cleanup_ready || diag.kill_polling_wired ||
      diag.default_read_ready || diag.diagnostics_ready) {
    return true;
  }
  *kill_not_wired = 1;
  return false;
}
