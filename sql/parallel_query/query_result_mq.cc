/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/parallel_query/query_result_mq.h"

#include "my_byteorder.h"
#include "sql/item.h"
#include "sql/sql_class.h"

#include <cstring>
#include <vector>

namespace {

static_assert(sizeof(PQ_worker_result_frame_header) == 24,
              "PQ worker-result frame header must stay wire-stable");

bool pq_is_valid_worker_result_type(uint16 type) {
  switch (static_cast<PQ_worker_result_message_type>(type)) {
    case PQ_worker_result_message_type::ROW:
    case PQ_worker_result_message_type::FINISH:
    case PQ_worker_result_message_type::ERROR:
      return true;
  }
  return false;
}

bool pq_send_worker_result_frame(MQueue_handle *handle,
                                 PQ_worker_result_message_type type,
                                 uint32 field_count,
                                 const uchar *null_bitmap,
                                 uint32 null_bitmap_len,
                                 const uchar *payload,
                                 uint32 payload_len) {
  if (handle == nullptr) return true;
  if (null_bitmap_len > 0 && null_bitmap == nullptr) return true;
  if (payload_len > 0 && payload == nullptr) return true;
  if (type != PQ_worker_result_message_type::ROW &&
      (field_count != 0 || null_bitmap_len != 0)) {
    return true;
  }

  constexpr uint64 header_size = sizeof(PQ_worker_result_frame_header);
  const uint64 total_len64 =
      header_size + static_cast<uint64>(null_bitmap_len) + payload_len;
  if (total_len64 > UINT32_MAX) return true;
  const uint32 total_len = static_cast<uint32>(total_len64);
  uchar *message = new uchar[total_len];
  if (message == nullptr) return true;

  auto *header = reinterpret_cast<PQ_worker_result_frame_header *>(message);
  header->magic = PQ_WORKER_RESULT_FRAME_MAGIC;
  header->version = PQ_WORKER_RESULT_FRAME_VERSION;
  header->type = static_cast<uint16>(type);
  header->field_count = field_count;
  header->null_bitmap_len = null_bitmap_len;
  header->payload_len = payload_len;
  header->flags = 0;

  uchar *cursor = message + sizeof(PQ_worker_result_frame_header);
  if (null_bitmap_len > 0) {
    memcpy(cursor, null_bitmap, null_bitmap_len);
    cursor += null_bitmap_len;
  }
  if (payload_len > 0) {
    memcpy(cursor, payload, payload_len);
  }

  const MQ_RESULT result = handle->send(message, total_len);
  delete[] message;
  return result != MQ_SUCCESS;
}

uint32 pq_worker_result_null_bitmap_len(uint32 field_count) {
  return (field_count + 7) / 8;
}

void pq_worker_result_set_null_bit(std::vector<uchar> *null_bitmap,
                                   uint32 field_index) {
  (*null_bitmap)[field_index / 8] |=
      static_cast<uchar>(1U << (field_index % 8));
}

bool pq_worker_result_is_null(const uchar *null_bitmap,
                              uint32 null_bitmap_len, uint32 field_index) {
  const uint32 byte_index = field_index / 8;
  if (byte_index >= null_bitmap_len) return true;
  return (null_bitmap[byte_index] & (1U << (field_index % 8))) != 0;
}

void pq_worker_result_append_uint32(std::vector<uchar> *payload, uint32 value) {
  uchar encoded[4];
  int4store(encoded, value);
  payload->insert(payload->end(), encoded, encoded + sizeof(encoded));
}

bool pq_worker_result_decode_field(const uchar **cursor,
                                   const uchar *payload_end,
                                   const char **value,
                                   uint32 *value_len) {
  if (cursor == nullptr || *cursor == nullptr || value == nullptr ||
      value_len == nullptr || payload_end < *cursor ||
      static_cast<size_t>(payload_end - *cursor) < sizeof(uint32)) {
    return true;
  }

  const uint32 len = uint4korr(*cursor);
  *cursor += sizeof(uint32);
  if (static_cast<size_t>(payload_end - *cursor) < len) return true;

  *value = reinterpret_cast<const char *>(*cursor);
  *value_len = len;
  *cursor += len;
  return false;
}

bool pq_worker_result_validate_test_row(const PQ_worker_result_frame_header *h,
                                        const uchar *null_bitmap,
                                        const uchar *payload) {
  if (h == nullptr || null_bitmap == nullptr || payload == nullptr ||
      h->field_count != 2 || h->null_bitmap_len != 1) {
    return true;
  }
  if (pq_worker_result_is_null(null_bitmap, h->null_bitmap_len, 0) ||
      pq_worker_result_is_null(null_bitmap, h->null_bitmap_len, 1)) {
    return true;
  }

  const uchar *cursor = payload;
  const uchar *payload_end = payload + h->payload_len;
  const char *value = nullptr;
  uint32 value_len = 0;
  if (pq_worker_result_decode_field(&cursor, payload_end, &value,
                                    &value_len) ||
      value_len != 1 || memcmp(value, "7", 1) != 0) {
    return true;
  }
  if (pq_worker_result_decode_field(&cursor, payload_end, &value,
                                    &value_len) ||
      value_len != 2 || memcmp(value, "42", 2) != 0) {
    return true;
  }
  return cursor != payload_end;
}

}  // namespace

bool pq_validate_worker_result_frame(
    const void *raw_data, uint32 raw_len,
    const PQ_worker_result_frame_header **header, const uchar **null_bitmap,
    const uchar **payload) {
  if (header == nullptr || null_bitmap == nullptr || payload == nullptr) {
    return true;
  }
  *header = nullptr;
  *null_bitmap = nullptr;
  *payload = nullptr;

  if (raw_data == nullptr ||
      raw_len < sizeof(PQ_worker_result_frame_header)) {
    return true;
  }

  const auto *decoded =
      static_cast<const PQ_worker_result_frame_header *>(raw_data);
  if (decoded->magic != PQ_WORKER_RESULT_FRAME_MAGIC ||
      decoded->version != PQ_WORKER_RESULT_FRAME_VERSION ||
      !pq_is_valid_worker_result_type(decoded->type)) {
    return true;
  }

  constexpr uint32 header_size =
      static_cast<uint32>(sizeof(PQ_worker_result_frame_header));
  uint32 remaining = raw_len - header_size;
  if (decoded->null_bitmap_len > remaining) return true;
  remaining -= decoded->null_bitmap_len;
  if (decoded->payload_len != remaining) return true;

  if (decoded->type == static_cast<uint16>(PQ_worker_result_message_type::ROW)) {
    if (decoded->field_count == 0 || decoded->null_bitmap_len == 0) {
      return true;
    }
    if (decoded->null_bitmap_len !=
        pq_worker_result_null_bitmap_len(decoded->field_count)) {
      return true;
    }
  } else if (decoded->field_count != 0 || decoded->null_bitmap_len != 0) {
    return true;
  }

  const uchar *cursor =
      static_cast<const uchar *>(raw_data) +
      sizeof(PQ_worker_result_frame_header);
  *header = decoded;
  *null_bitmap = decoded->null_bitmap_len == 0 ? nullptr : cursor;
  *payload = decoded->payload_len == 0 ? nullptr
                                       : cursor + decoded->null_bitmap_len;
  return false;
}

bool pq_run_query_result_mq_contract_smoke(uint32 *rows_read,
                                           uint32 *finishes_read) {
  if (rows_read == nullptr || finishes_read == nullptr) return true;
  *rows_read = 0;
  *finishes_read = 0;

  PQ_mq_event sender_event;
  PQ_mq_event receiver_event;
  char ring[PQ_MQ_DEFAULT_RING_SIZE];
  MQueue queue(&sender_event, &receiver_event, ring, sizeof(ring));
  MQueue_handle handle(&queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
  if (handle.init()) return true;

  const uchar null_bitmap[] = {0};
  const uchar payload[] = {1, 2, 3, 4};
  bool failed = pq_send_worker_result_frame(
                    &handle, PQ_worker_result_message_type::ROW, 1,
                    null_bitmap, sizeof(null_bitmap), payload,
                    sizeof(payload)) ||
                pq_send_worker_result_frame(
                    &handle, PQ_worker_result_message_type::FINISH, 0,
                    nullptr, 0, nullptr, 0);

  for (uint32 i = 0; !failed && i < 2; ++i) {
    void *raw_data = nullptr;
    uint32 raw_len = 0;
    if (handle.receive(&raw_data, &raw_len) != MQ_SUCCESS) {
      failed = true;
      break;
    }

    const PQ_worker_result_frame_header *header = nullptr;
    const uchar *decoded_null_bitmap = nullptr;
    const uchar *decoded_payload = nullptr;
    if (pq_validate_worker_result_frame(raw_data, raw_len, &header,
                                        &decoded_null_bitmap,
                                        &decoded_payload)) {
      failed = true;
      break;
    }

    if (header->type ==
        static_cast<uint16>(PQ_worker_result_message_type::ROW)) {
      failed = decoded_null_bitmap == nullptr || decoded_payload == nullptr ||
               header->field_count != 1 || header->null_bitmap_len != 1 ||
               header->payload_len != sizeof(payload);
      if (!failed) ++(*rows_read);
    } else if (header->type ==
               static_cast<uint16>(PQ_worker_result_message_type::FINISH)) {
      failed = decoded_null_bitmap != nullptr || decoded_payload != nullptr ||
               header->field_count != 0 || header->null_bitmap_len != 0 ||
               header->payload_len != 0;
      if (!failed) ++(*finishes_read);
    } else {
      failed = true;
    }
  }

  PQ_worker_result_frame_header malformed{};
  malformed.magic = PQ_WORKER_RESULT_FRAME_MAGIC;
  malformed.version = PQ_WORKER_RESULT_FRAME_VERSION;
  malformed.type = static_cast<uint16>(PQ_worker_result_message_type::ROW);
  malformed.field_count = 1;
  malformed.null_bitmap_len = UINT32_MAX;
  malformed.payload_len = UINT32_MAX;
  malformed.flags = 0;
  const PQ_worker_result_frame_header *bad_header = nullptr;
  const uchar *bad_null_bitmap = nullptr;
  const uchar *bad_payload = nullptr;
  if (!pq_validate_worker_result_frame(&malformed, sizeof(malformed),
                                       &bad_header, &bad_null_bitmap,
                                       &bad_payload)) {
    failed = true;
  }

  handle.cleanup();
  return failed || *rows_read != 1 || *finishes_read != 1;
}

bool pq_run_query_result_mq_send_data_smoke(THD *thd, uint32 *rows_read,
                                            uint32 *finishes_read,
                                            uint32 *errors_read) {
  if (thd == nullptr || rows_read == nullptr || finishes_read == nullptr ||
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

  Query_result_mq result(nullptr, &handle, false);
  mem_root_deque<Item *> fields(thd->mem_root);
  fields.push_back(new (thd->mem_root) Item_int(7));
  fields.push_back(new (thd->mem_root) Item_int(42));

  const ha_rows sent_rows_before = thd->get_sent_row_count();
  bool failed = fields[0] == nullptr || fields[1] == nullptr ||
                result.send_data(thd, fields) || result.send_eof(thd) ||
                pq_send_worker_result_frame(
                    &handle, PQ_worker_result_message_type::ERROR, 0,
                    nullptr, 0,
                    reinterpret_cast<const uchar *>("worker-error"), 12);
  thd->set_sent_row_count(sent_rows_before);

  for (uint32 i = 0; !failed && i < 3; ++i) {
    void *raw_data = nullptr;
    uint32 raw_len = 0;
    if (handle.receive(&raw_data, &raw_len) != MQ_SUCCESS) {
      failed = true;
      break;
    }

    const PQ_worker_result_frame_header *header = nullptr;
    const uchar *decoded_null_bitmap = nullptr;
    const uchar *decoded_payload = nullptr;
    if (pq_validate_worker_result_frame(raw_data, raw_len, &header,
                                        &decoded_null_bitmap,
                                        &decoded_payload)) {
      failed = true;
      break;
    }

    if (header->type ==
        static_cast<uint16>(PQ_worker_result_message_type::ROW)) {
      failed = pq_worker_result_validate_test_row(
          header, decoded_null_bitmap, decoded_payload);
      if (!failed) ++(*rows_read);
    } else if (header->type ==
               static_cast<uint16>(PQ_worker_result_message_type::FINISH)) {
      failed = decoded_null_bitmap != nullptr || decoded_payload != nullptr ||
               header->field_count != 0 || header->null_bitmap_len != 0 ||
               header->payload_len != 0;
      if (!failed) ++(*finishes_read);
    } else if (header->type ==
               static_cast<uint16>(PQ_worker_result_message_type::ERROR)) {
      failed = decoded_null_bitmap != nullptr || decoded_payload == nullptr ||
               header->field_count != 0 || header->null_bitmap_len != 0 ||
               header->payload_len != 12 ||
               memcmp(decoded_payload, "worker-error", 12) != 0;
      if (!failed) ++(*errors_read);
    } else {
      failed = true;
    }
  }

  handle.cleanup();
  return failed || *rows_read != 1 || *finishes_read != 1 || *errors_read != 1;
}

Query_result_mq::Query_result_mq(JOIN *join, MQueue_handle *msg_handler,
                                 bool stab_output)
    : Query_result(), m_join(join), m_handler(msg_handler),
      m_stable_output(stab_output) {}

bool Query_result_mq::send_result_set_metadata(
    THD *, const mem_root_deque<Item *> &, uint) {
  return false;
}

bool Query_result_mq::send_data(THD *thd, const mem_root_deque<Item *> &items) {
  if (thd == nullptr || m_handler == nullptr || items.empty() ||
      items.size() > UINT32_MAX) {
    return true;
  }

  const uint32 field_count = static_cast<uint32>(items.size());
  const uint32 null_bitmap_len = pq_worker_result_null_bitmap_len(field_count);
  std::vector<uchar> null_bitmap(null_bitmap_len, 0);
  std::vector<uchar> payload;
  String tmp;

  for (uint32 i = 0; i < field_count; ++i) {
    Item *item = items[i];
    if (item == nullptr) return true;

    tmp.length(0);
    String *value = item->val_str(&tmp);
    if (item->is_null() || value == nullptr) {
      pq_worker_result_set_null_bit(&null_bitmap, i);
      pq_worker_result_append_uint32(&payload, 0);
      continue;
    }

    if (value->length() > UINT32_MAX) return true;
    const auto value_len = static_cast<uint32>(value->length());
    const uint64 next_size =
        static_cast<uint64>(payload.size()) + sizeof(uint32) + value_len;
    if (next_size > UINT32_MAX) return true;

    pq_worker_result_append_uint32(&payload, value_len);
    const char *value_ptr = value->ptr();
    payload.insert(payload.end(), value_ptr, value_ptr + value_len);
  }

  if (pq_send_worker_result_frame(
          m_handler, PQ_worker_result_message_type::ROW, field_count,
          null_bitmap.data(), null_bitmap_len, payload.data(),
          static_cast<uint32>(payload.size()))) {
    return true;
  }

  thd->inc_sent_row_count(1);
  return false;
}

bool Query_result_mq::send_eof(THD *thd) {
  if (m_handler == nullptr) return true;
  if (thd != nullptr && thd->is_error()) {
    static constexpr uchar error_payload[] = "worker-error";
    (void)pq_send_worker_result_frame(
        m_handler, PQ_worker_result_message_type::ERROR, 0, nullptr, 0,
        error_payload, static_cast<uint32>(sizeof(error_payload) - 1));
    return true;
  }
  return pq_send_worker_result_frame(m_handler,
                                     PQ_worker_result_message_type::FINISH, 0,
                                     nullptr, 0, nullptr, 0);
}

void Query_result_mq::cleanup() {
  m_table = nullptr;
  m_param = nullptr;
  send_fields = nullptr;
  send_fields_size = 0;
  mq_fields_data = nullptr;
  mq_fields_null_array = nullptr;
  mq_fields_null_flag = nullptr;
}
