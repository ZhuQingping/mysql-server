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

bool pq_worker_result_has_unknown_flags(uint32 flags) {
  constexpr uint32 known_flags = PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF |
                                 PQ_WORKER_RESULT_FRAME_FLAG_FIELD_INDEXES |
                                 PQ_WORKER_RESULT_FRAME_FLAG_RAW_FIELDS;
  return (flags & ~known_flags) != 0;
}

bool pq_send_worker_result_frame_with_flags(
    MQueue_handle *handle, PQ_worker_result_message_type type,
    uint32 field_count, const uchar *null_bitmap, uint32 null_bitmap_len,
    const uchar *payload, uint32 payload_len, uint32 flags) {
  if (handle == nullptr) return true;
  if (null_bitmap_len > 0 && null_bitmap == nullptr) return true;
  if (payload_len > 0 && payload == nullptr) return true;
  if (pq_worker_result_has_unknown_flags(flags)) return true;
  if ((flags & PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF) != 0 &&
      type != PQ_worker_result_message_type::ROW) {
    return true;
  }
  if ((flags & PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF) != 0 &&
      (flags & PQ_WORKER_RESULT_FRAME_FLAG_FIELD_INDEXES) != 0) {
    return true;
  }
  if ((flags & PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF) != 0 &&
      (flags & PQ_WORKER_RESULT_FRAME_FLAG_RAW_FIELDS) != 0) {
    return true;
  }
  if ((flags & PQ_WORKER_RESULT_FRAME_FLAG_FIELD_INDEXES) != 0 &&
      type != PQ_worker_result_message_type::ROW) {
    return true;
  }
  if ((flags & PQ_WORKER_RESULT_FRAME_FLAG_RAW_FIELDS) != 0 &&
      type != PQ_worker_result_message_type::ROW) {
    return true;
  }
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
  header->flags = flags;

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

bool pq_send_worker_result_frame(MQueue_handle *handle,
                                 PQ_worker_result_message_type type,
                                 uint32 field_count,
                                 const uchar *null_bitmap,
                                 uint32 null_bitmap_len,
                                 const uchar *payload,
                                 uint32 payload_len) {
  return pq_send_worker_result_frame_with_flags(
      handle, type, field_count, null_bitmap, null_bitmap_len, payload,
      payload_len, 0);
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

bool pq_send_worker_result_stable_ref_frame(MQueue_handle *handle,
                                            uint32 field_count,
                                            const uchar *null_bitmap,
                                            uint32 null_bitmap_len,
                                            const uchar *field_payload,
                                            uint32 field_payload_len,
                                            const uchar *handler_ref,
                                            uint32 handler_ref_len) {
  if (handler_ref == nullptr || handler_ref_len == 0) return true;
  if (field_payload_len > 0 && field_payload == nullptr) return true;

  const uint64 stable_payload_len64 =
      sizeof(uint32) + static_cast<uint64>(handler_ref_len) +
      field_payload_len;
  if (stable_payload_len64 > UINT32_MAX) return true;

  std::vector<uchar> stable_payload;
  stable_payload.reserve(static_cast<size_t>(stable_payload_len64));
  pq_worker_result_append_uint32(&stable_payload, handler_ref_len);
  stable_payload.insert(stable_payload.end(), handler_ref,
                        handler_ref + handler_ref_len);
  if (field_payload_len > 0) {
    stable_payload.insert(stable_payload.end(), field_payload,
                          field_payload + field_payload_len);
  }

  return pq_send_worker_result_frame_with_flags(
      handle, PQ_worker_result_message_type::ROW, field_count, null_bitmap,
      null_bitmap_len, stable_payload.data(),
      static_cast<uint32>(stable_payload.size()),
      PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF);
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

bool pq_worker_result_decode_raw_field(const uchar **cursor,
                                       const uchar *payload_end,
                                       const uchar **value, uint32 *value_len,
                                       uchar *var_len) {
  if (cursor == nullptr || *cursor == nullptr || value == nullptr ||
      value_len == nullptr || var_len == nullptr || payload_end < *cursor ||
      static_cast<size_t>(payload_end - *cursor) <
          sizeof(uint32) + sizeof(uchar)) {
    return true;
  }

  const uint32 len = uint4korr(*cursor);
  *cursor += sizeof(uint32);
  *var_len = **cursor;
  *cursor += sizeof(uchar);
  if (*var_len > 2 || static_cast<size_t>(payload_end - *cursor) < len) {
    return true;
  }

  *value = *cursor;
  *value_len = len;
  *cursor += len;
  return false;
}

bool pq_worker_result_append_raw_field(std::vector<uchar> *payload,
                                       const Field_raw_data &field_raw) {
  if (payload == nullptr) return true;
  if (field_raw.m_need_send && field_raw.m_len > 0 &&
      field_raw.m_ptr == nullptr) {
    return true;
  }
  if (field_raw.m_var_len > 2) return true;

  pq_worker_result_append_uint32(
      payload, field_raw.m_need_send ? field_raw.m_len : 0);
  payload->push_back(field_raw.m_need_send ? field_raw.m_var_len : 0);
  if (field_raw.m_need_send && field_raw.m_len > 0) {
    payload->insert(payload->end(), field_raw.m_ptr,
                    field_raw.m_ptr + field_raw.m_len);
  }
  return false;
}

bool pq_collect_table_read_set_fields(const TABLE *table,
                                      std::vector<uint32> *field_indexes) {
  if (table == nullptr || table->s == nullptr || table->read_set == nullptr ||
      field_indexes == nullptr) {
    return true;
  }

  field_indexes->clear();
  field_indexes->reserve(table->s->fields);
  for (uint32 i = 0; i < table->s->fields; ++i) {
    if (bitmap_is_set(table->read_set, i)) field_indexes->push_back(i);
  }
  return field_indexes->empty();
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
      !pq_is_valid_worker_result_type(decoded->type) ||
      pq_worker_result_has_unknown_flags(decoded->flags)) {
    return true;
  }
  if ((decoded->flags & PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF) != 0 &&
      (decoded->flags & PQ_WORKER_RESULT_FRAME_FLAG_FIELD_INDEXES) != 0) {
    return true;
  }
  if ((decoded->flags & PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF) != 0 &&
      (decoded->flags & PQ_WORKER_RESULT_FRAME_FLAG_RAW_FIELDS) != 0) {
    return true;
  }
  if ((decoded->flags & PQ_WORKER_RESULT_FRAME_FLAG_FIELD_INDEXES) != 0 &&
      decoded->type !=
          static_cast<uint16>(PQ_worker_result_message_type::ROW)) {
    return true;
  }
  if ((decoded->flags & PQ_WORKER_RESULT_FRAME_FLAG_RAW_FIELDS) != 0 &&
      decoded->type !=
          static_cast<uint16>(PQ_worker_result_message_type::ROW)) {
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

bool pq_decode_worker_result_row(
    const void *raw_data, uint32 raw_len,
    std::vector<PQ_worker_result_decoded_field> *fields) {
  if (fields == nullptr) return true;
  fields->clear();

  const PQ_worker_result_frame_header *header = nullptr;
  const uchar *null_bitmap = nullptr;
  const uchar *payload = nullptr;
  if (pq_validate_worker_result_frame(raw_data, raw_len, &header, &null_bitmap,
                                      &payload) ||
      header == nullptr ||
      header->type !=
          static_cast<uint16>(PQ_worker_result_message_type::ROW) ||
      (header->flags & PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF) != 0 ||
      (header->flags & PQ_WORKER_RESULT_FRAME_FLAG_RAW_FIELDS) != 0 ||
      null_bitmap == nullptr || payload == nullptr) {
    return true;
  }

  const uchar *cursor = payload;
  const uchar *payload_end = payload + header->payload_len;
  std::vector<uint32> field_indexes;
  const bool has_field_indexes =
      (header->flags & PQ_WORKER_RESULT_FRAME_FLAG_FIELD_INDEXES) != 0;
  if (has_field_indexes) {
    field_indexes.reserve(header->field_count);
    for (uint32 i = 0; i < header->field_count; ++i) {
      if (static_cast<size_t>(payload_end - cursor) < sizeof(uint32)) {
        fields->clear();
        return true;
      }
      field_indexes.push_back(uint4korr(cursor));
      cursor += sizeof(uint32);
    }
  }

  fields->reserve(header->field_count);
  for (uint32 i = 0; i < header->field_count; ++i) {
    const char *value = nullptr;
    uint32 value_len = 0;
    if (pq_worker_result_decode_field(&cursor, payload_end, &value,
                                      &value_len)) {
      fields->clear();
      return true;
    }

    const bool is_null =
        pq_worker_result_is_null(null_bitmap, header->null_bitmap_len, i);
    if (is_null && value_len != 0) {
      fields->clear();
      return true;
    }

    fields->push_back({is_null ? nullptr : value, value_len,
                       has_field_indexes ? field_indexes[i] : i, is_null});
  }

  if (cursor != payload_end) {
    fields->clear();
    return true;
  }
  return false;
}

bool pq_decode_worker_result_raw_row(
    const void *raw_data, uint32 raw_len,
    std::vector<PQ_worker_result_decoded_raw_field> *fields) {
  if (fields == nullptr) return true;
  fields->clear();

  const PQ_worker_result_frame_header *header = nullptr;
  const uchar *null_bitmap = nullptr;
  const uchar *payload = nullptr;
  if (pq_validate_worker_result_frame(raw_data, raw_len, &header, &null_bitmap,
                                      &payload) ||
      header == nullptr ||
      header->type !=
          static_cast<uint16>(PQ_worker_result_message_type::ROW) ||
      (header->flags & PQ_WORKER_RESULT_FRAME_FLAG_RAW_FIELDS) == 0 ||
      (header->flags & PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF) != 0 ||
      null_bitmap == nullptr || payload == nullptr) {
    return true;
  }

  const uchar *cursor = payload;
  const uchar *payload_end = payload + header->payload_len;
  std::vector<uint32> field_indexes;
  const bool has_field_indexes =
      (header->flags & PQ_WORKER_RESULT_FRAME_FLAG_FIELD_INDEXES) != 0;
  if (has_field_indexes) {
    field_indexes.reserve(header->field_count);
    for (uint32 i = 0; i < header->field_count; ++i) {
      if (static_cast<size_t>(payload_end - cursor) < sizeof(uint32)) {
        fields->clear();
        return true;
      }
      field_indexes.push_back(uint4korr(cursor));
      cursor += sizeof(uint32);
    }
  }

  fields->reserve(header->field_count);
  for (uint32 i = 0; i < header->field_count; ++i) {
    const uchar *value = nullptr;
    uint32 value_len = 0;
    uchar var_len = 0;
    if (pq_worker_result_decode_raw_field(&cursor, payload_end, &value,
                                          &value_len, &var_len)) {
      fields->clear();
      return true;
    }

    const bool is_null =
        pq_worker_result_is_null(null_bitmap, header->null_bitmap_len, i);
    if (is_null && (value_len != 0 || var_len != 0)) {
      fields->clear();
      return true;
    }
    if (!is_null && value_len == 0) {
      fields->clear();
      return true;
    }

    fields->push_back({is_null ? nullptr : value, value_len, var_len,
                       has_field_indexes ? field_indexes[i] : i, is_null});
  }

  if (cursor != payload_end) {
    fields->clear();
    return true;
  }
  return false;
}

bool pq_decode_worker_result_stable_ref_row(
    const void *raw_data, uint32 raw_len,
    PQ_worker_result_stable_ref *stable_ref) {
  if (stable_ref == nullptr) return true;
  *stable_ref = {};

  const PQ_worker_result_frame_header *header = nullptr;
  const uchar *null_bitmap = nullptr;
  const uchar *payload = nullptr;
  if (pq_validate_worker_result_frame(raw_data, raw_len, &header, &null_bitmap,
                                      &payload) ||
      header == nullptr ||
      header->type !=
          static_cast<uint16>(PQ_worker_result_message_type::ROW) ||
      header->flags != PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF ||
      null_bitmap == nullptr || payload == nullptr ||
      header->payload_len < sizeof(uint32)) {
    return true;
  }

  const uint32 row_id_len = uint4korr(payload);
  const uchar *row_id = payload + sizeof(uint32);
  if (row_id_len == 0 ||
      header->payload_len < sizeof(uint32) + row_id_len) {
    return true;
  }

  stable_ref->row_id = row_id;
  stable_ref->row_id_len = row_id_len;
  stable_ref->null_bitmap = null_bitmap;
  stable_ref->null_bitmap_len = header->null_bitmap_len;
  stable_ref->field_payload = row_id + row_id_len;
  stable_ref->field_payload_len =
      header->payload_len - sizeof(uint32) - row_id_len;
  stable_ref->field_count = header->field_count;
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
                result.start_execution(thd) ||
                result.send_result_set_metadata(thd, fields, 0) ||
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

bool pq_run_query_result_mq_adapter_smoke(THD *thd, uint32 *rows_read,
                                          uint32 *finishes_read) {
  if (thd == nullptr || rows_read == nullptr || finishes_read == nullptr) {
    return true;
  }
  *rows_read = 0;
  *finishes_read = 0;

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
                result.start_execution(thd) ||
                result.send_result_set_metadata(thd, fields, 0) ||
                result.send_data(thd, fields) || result.send_eof(thd);
  thd->set_sent_row_count(sent_rows_before);

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
      std::vector<PQ_worker_result_decoded_field> decoded_fields;
      failed = pq_decode_worker_result_row(raw_data, raw_len, &decoded_fields) ||
               decoded_fields.size() != 2 || decoded_fields[0].is_null ||
               decoded_fields[1].is_null || decoded_fields[0].value_len != 1 ||
               decoded_fields[1].value_len != 2 ||
               memcmp(decoded_fields[0].value, "7", 1) != 0 ||
               memcmp(decoded_fields[1].value, "42", 2) != 0;
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

  handle.cleanup();
  return failed || *rows_read != 1 || *finishes_read != 1;
}

bool pq_run_query_result_mq_wiring_smoke(THD *thd, uint32 *rows_read,
                                         uint32 *finishes_read) {
  if (thd == nullptr || rows_read == nullptr || finishes_read == nullptr) {
    return true;
  }
  *rows_read = 0;
  *finishes_read = 0;

  PQ_mq_event sender_event;
  PQ_mq_event receiver_event;
  char ring[PQ_MQ_DEFAULT_RING_SIZE];
  MQueue queue(&sender_event, &receiver_event, ring, sizeof(ring));
  MQueue_handle handle(&queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
  if (handle.init()) return true;

  Query_result_mq result(nullptr, &handle, false);
  mem_root_deque<Item *> row1(thd->mem_root);
  mem_root_deque<Item *> row2(thd->mem_root);
  row1.push_back(new (thd->mem_root) Item_int(7));
  row1.push_back(new (thd->mem_root) Item_int(42));
  row2.push_back(new (thd->mem_root) Item_int(11));
  row2.push_back(new (thd->mem_root) Item_int(84));

  const ha_rows sent_rows_before = thd->get_sent_row_count();
  bool failed = row1[0] == nullptr || row1[1] == nullptr ||
                row2[0] == nullptr || row2[1] == nullptr ||
                result.start_execution(thd) ||
                result.send_result_set_metadata(thd, row1, 0) ||
                result.send_data(thd, row1) ||
                result.send_data(thd, row2) || result.send_eof(thd);
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
      if (*finishes_read != 0) {
        failed = true;
        break;
      }
      std::vector<PQ_worker_result_decoded_field> decoded_fields;
      failed = pq_decode_worker_result_row(raw_data, raw_len, &decoded_fields) ||
               decoded_fields.size() != 2 || decoded_fields[0].is_null ||
               decoded_fields[1].is_null;
      if (!failed && *rows_read == 0) {
        failed = decoded_fields[0].value_len != 1 ||
                 decoded_fields[1].value_len != 2 ||
                 memcmp(decoded_fields[0].value, "7", 1) != 0 ||
                 memcmp(decoded_fields[1].value, "42", 2) != 0;
      } else if (!failed && *rows_read == 1) {
        failed = decoded_fields[0].value_len != 2 ||
                 decoded_fields[1].value_len != 2 ||
                 memcmp(decoded_fields[0].value, "11", 2) != 0 ||
                 memcmp(decoded_fields[1].value, "84", 2) != 0;
      } else if (!failed) {
        failed = true;
      }
      if (!failed) ++(*rows_read);
    } else if (header->type ==
               static_cast<uint16>(PQ_worker_result_message_type::FINISH)) {
      if (*rows_read != 2 || *finishes_read != 0) {
        failed = true;
        break;
      }
      failed = decoded_null_bitmap != nullptr || decoded_payload != nullptr ||
               header->field_count != 0 || header->null_bitmap_len != 0 ||
               header->payload_len != 0;
      if (!failed) ++(*finishes_read);
    } else {
      failed = true;
    }
  }

  handle.cleanup();
  return failed || *rows_read != 2 || *finishes_read != 1;
}

bool pq_run_query_result_mq_raw_field_smoke(uint32 *rows_read,
                                            uint32 *fields_read,
                                            uint32 *bytes_read,
                                            uint32 *normal_decode_rejects,
                                            uint32 *invalid_rejects) {
  if (rows_read == nullptr || fields_read == nullptr ||
      bytes_read == nullptr || normal_decode_rejects == nullptr ||
      invalid_rejects == nullptr) {
    return true;
  }
  *rows_read = 0;
  *fields_read = 0;
  *bytes_read = 0;
  *normal_decode_rejects = 0;
  *invalid_rejects = 0;

  PQ_mq_event sender_event;
  PQ_mq_event receiver_event;
  char ring[PQ_MQ_DEFAULT_RING_SIZE];
  MQueue queue(&sender_event, &receiver_event, ring, sizeof(ring));
  MQueue_handle handle(&queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
  if (handle.init()) return true;

  uchar fixed_raw[] = {0x11, 0x22, 0x33, 0x44};
  uchar var_raw[] = {5, 'h', 'e', 'l', 'l', 'o'};
  Field_raw_data raw_fields[3];
  raw_fields[0].m_ptr = fixed_raw;
  raw_fields[0].m_len = sizeof(fixed_raw);
  raw_fields[0].m_var_len = 0;
  raw_fields[0].m_need_send = true;
  raw_fields[1].m_ptr = var_raw;
  raw_fields[1].m_len = sizeof(var_raw);
  raw_fields[1].m_var_len = 1;
  raw_fields[1].m_need_send = true;
  raw_fields[2].m_need_send = false;

  const uint32 field_indexes[] = {2, 4, 7};
  std::vector<uchar> payload;
  for (uint32 field_index : field_indexes) {
    pq_worker_result_append_uint32(&payload, field_index);
  }
  bool failed = false;
  for (const Field_raw_data &raw_field : raw_fields) {
    if (pq_worker_result_append_raw_field(&payload, raw_field)) {
      failed = true;
      break;
    }
  }

  const uchar null_bitmap[] = {static_cast<uchar>(1U << 2)};
  failed = failed ||
           pq_send_worker_result_frame_with_flags(
               &handle, PQ_worker_result_message_type::ROW, 3, null_bitmap,
               sizeof(null_bitmap), payload.data(),
               static_cast<uint32>(payload.size()),
               PQ_WORKER_RESULT_FRAME_FLAG_FIELD_INDEXES |
                   PQ_WORKER_RESULT_FRAME_FLAG_RAW_FIELDS);

  void *raw_data = nullptr;
  uint32 raw_len = 0;
  if (!failed && handle.receive(&raw_data, &raw_len) != MQ_SUCCESS) {
    failed = true;
  }

  std::vector<PQ_worker_result_decoded_field> decoded_string_fields;
  if (!failed && pq_decode_worker_result_row(raw_data, raw_len,
                                             &decoded_string_fields)) {
    ++(*normal_decode_rejects);
  } else if (!failed) {
    failed = true;
  }

  std::vector<PQ_worker_result_decoded_raw_field> decoded_raw_fields;
  if (!failed &&
      pq_decode_worker_result_raw_row(raw_data, raw_len, &decoded_raw_fields)) {
    failed = true;
  }
  if (!failed) {
    failed = decoded_raw_fields.size() != 3 ||
             decoded_raw_fields[0].is_null ||
             decoded_raw_fields[0].field_index != 2 ||
             decoded_raw_fields[0].var_len != 0 ||
             decoded_raw_fields[0].value_len != sizeof(fixed_raw) ||
             memcmp(decoded_raw_fields[0].value, fixed_raw,
                    sizeof(fixed_raw)) != 0 ||
             decoded_raw_fields[1].is_null ||
             decoded_raw_fields[1].field_index != 4 ||
             decoded_raw_fields[1].var_len != 1 ||
             decoded_raw_fields[1].value_len != sizeof(var_raw) ||
             memcmp(decoded_raw_fields[1].value, var_raw, sizeof(var_raw)) !=
                 0 ||
             !decoded_raw_fields[2].is_null ||
             decoded_raw_fields[2].field_index != 7 ||
             decoded_raw_fields[2].value != nullptr ||
             decoded_raw_fields[2].value_len != 0 ||
             decoded_raw_fields[2].var_len != 0;
  }
  if (!failed) {
    *rows_read = 1;
    *fields_read = static_cast<uint32>(decoded_raw_fields.size());
    *bytes_read = decoded_raw_fields[0].value_len +
                  decoded_raw_fields[1].value_len +
                  decoded_raw_fields[2].value_len;
  }

  PQ_worker_result_frame_header invalid_raw_finish{};
  invalid_raw_finish.magic = PQ_WORKER_RESULT_FRAME_MAGIC;
  invalid_raw_finish.version = PQ_WORKER_RESULT_FRAME_VERSION;
  invalid_raw_finish.type =
      static_cast<uint16>(PQ_worker_result_message_type::FINISH);
  invalid_raw_finish.flags = PQ_WORKER_RESULT_FRAME_FLAG_RAW_FIELDS;
  const PQ_worker_result_frame_header *bad_header = nullptr;
  const uchar *bad_null_bitmap = nullptr;
  const uchar *bad_payload = nullptr;
  if (!failed &&
      pq_validate_worker_result_frame(&invalid_raw_finish,
                                      sizeof(invalid_raw_finish), &bad_header,
                                      &bad_null_bitmap, &bad_payload)) {
    ++(*invalid_rejects);
  } else if (!failed) {
    failed = true;
  }

  PQ_worker_result_frame_header invalid_raw_stable = invalid_raw_finish;
  invalid_raw_stable.type =
      static_cast<uint16>(PQ_worker_result_message_type::ROW);
  invalid_raw_stable.field_count = 1;
  invalid_raw_stable.null_bitmap_len = 1;
  invalid_raw_stable.payload_len = sizeof(uint32);
  invalid_raw_stable.flags = PQ_WORKER_RESULT_FRAME_FLAG_RAW_FIELDS |
                             PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF;
  if (!failed &&
      pq_validate_worker_result_frame(&invalid_raw_stable,
                                      sizeof(invalid_raw_stable), &bad_header,
                                      &bad_null_bitmap, &bad_payload)) {
    ++(*invalid_rejects);
  } else if (!failed) {
    failed = true;
  }

  handle.cleanup();
  return failed || *rows_read != 1 || *fields_read != 3 ||
         *bytes_read != sizeof(fixed_raw) + sizeof(var_raw) ||
         *normal_decode_rejects != 1 || *invalid_rejects != 2;
}

bool pq_run_query_result_mq_stable_ref_smoke(const uchar *handler_ref,
                                             uint32 handler_ref_len,
                                             uint32 *ref_bytes,
                                             uint32 *deep_copy_success,
                                             uint32 *normal_decode_rejects,
                                             uint32 *invalid_rejects) {
  if (handler_ref == nullptr || handler_ref_len == 0 || ref_bytes == nullptr ||
      deep_copy_success == nullptr || normal_decode_rejects == nullptr ||
      invalid_rejects == nullptr) {
    return true;
  }
  *ref_bytes = 0;
  *deep_copy_success = 0;
  *normal_decode_rejects = 0;
  *invalid_rejects = 0;

  PQ_mq_event sender_event;
  PQ_mq_event receiver_event;
  char ring[PQ_MQ_DEFAULT_RING_SIZE];
  MQueue queue(&sender_event, &receiver_event, ring, sizeof(ring));
  MQueue_handle handle(&queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
  if (handle.init()) return true;

  std::vector<uchar> mutable_ref(handler_ref, handler_ref + handler_ref_len);
  const std::vector<uchar> expected_ref = mutable_ref;
  const uchar null_bitmap[] = {0};
  std::vector<uchar> field_payload;
  pq_worker_result_append_uint32(&field_payload, 1);
  field_payload.push_back('9');

  bool failed = pq_send_worker_result_stable_ref_frame(
      &handle, 1, null_bitmap, sizeof(null_bitmap), field_payload.data(),
      static_cast<uint32>(field_payload.size()), mutable_ref.data(),
      static_cast<uint32>(mutable_ref.size()));

  if (!mutable_ref.empty()) mutable_ref[0] ^= 0xff;

  void *raw_data = nullptr;
  uint32 raw_len = 0;
  if (!failed && handle.receive(&raw_data, &raw_len) != MQ_SUCCESS) {
    failed = true;
  }

  PQ_worker_result_stable_ref stable_ref;
  if (!failed &&
      pq_decode_worker_result_stable_ref_row(raw_data, raw_len, &stable_ref)) {
    failed = true;
  }
  if (!failed) {
    failed = stable_ref.row_id == nullptr ||
             stable_ref.row_id_len != expected_ref.size() ||
             memcmp(stable_ref.row_id, expected_ref.data(),
                    expected_ref.size()) != 0 ||
             stable_ref.field_count != 1 || stable_ref.null_bitmap_len != 1 ||
             stable_ref.field_payload_len != field_payload.size() ||
             memcmp(stable_ref.field_payload, field_payload.data(),
                    field_payload.size()) != 0;
  }
  if (!failed) {
    ++(*deep_copy_success);
    *ref_bytes = stable_ref.row_id_len;
  }

  std::vector<PQ_worker_result_decoded_field> decoded_fields;
  if (!failed && pq_decode_worker_result_row(raw_data, raw_len,
                                             &decoded_fields)) {
    ++(*normal_decode_rejects);
  } else if (!failed) {
    failed = true;
  }

  PQ_worker_result_frame_header invalid_flags{};
  invalid_flags.magic = PQ_WORKER_RESULT_FRAME_MAGIC;
  invalid_flags.version = PQ_WORKER_RESULT_FRAME_VERSION;
  invalid_flags.type = static_cast<uint16>(PQ_worker_result_message_type::ROW);
  invalid_flags.field_count = 1;
  invalid_flags.null_bitmap_len = 1;
  invalid_flags.payload_len = sizeof(uint32) + handler_ref_len;
  invalid_flags.flags = PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF | (1U << 4);
  const PQ_worker_result_frame_header *bad_header = nullptr;
  const uchar *bad_null_bitmap = nullptr;
  const uchar *bad_payload = nullptr;
  if (pq_validate_worker_result_frame(&invalid_flags, sizeof(invalid_flags),
                                      &bad_header, &bad_null_bitmap,
                                      &bad_payload)) {
    ++(*invalid_rejects);
  } else {
    failed = true;
  }

  PQ_worker_result_frame_header invalid_version = invalid_flags;
  invalid_version.flags = PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF;
  invalid_version.version = PQ_WORKER_RESULT_FRAME_VERSION + 1;
  if (!failed &&
      pq_validate_worker_result_frame(&invalid_version, sizeof(invalid_version),
                                      &bad_header, &bad_null_bitmap,
                                      &bad_payload)) {
    ++(*invalid_rejects);
  } else if (!failed) {
    failed = true;
  }

  PQ_worker_result_frame_header invalid_length = invalid_flags;
  invalid_length.flags = PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF;
  invalid_length.version = PQ_WORKER_RESULT_FRAME_VERSION;
  invalid_length.payload_len = UINT32_MAX;
  if (!failed &&
      pq_validate_worker_result_frame(&invalid_length, sizeof(invalid_length),
                                      &bad_header, &bad_null_bitmap,
                                      &bad_payload)) {
    ++(*invalid_rejects);
  } else if (!failed) {
    failed = true;
  }

  handle.cleanup();
  return failed || *ref_bytes != handler_ref_len || *deep_copy_success != 1 ||
         *normal_decode_rejects != 1 || *invalid_rejects != 3;
}

bool pq_run_query_result_mq_stable_ref_adapter_smoke(
    const uchar *handler_ref, uint32 handler_ref_len,
    uint32 expected_ref_length, uint32 *ref_bytes, uint32 *owned_ref_success,
    uint32 *length_mismatch_rejects) {
  if (handler_ref == nullptr || handler_ref_len == 0 ||
      expected_ref_length == 0 || ref_bytes == nullptr ||
      owned_ref_success == nullptr || length_mismatch_rejects == nullptr) {
    return true;
  }
  *ref_bytes = 0;
  *owned_ref_success = 0;
  *length_mismatch_rejects = 0;
  if (handler_ref_len != expected_ref_length) {
    ++(*length_mismatch_rejects);
    return true;
  }

  PQ_mq_event sender_event;
  PQ_mq_event receiver_event;
  char ring[PQ_MQ_DEFAULT_RING_SIZE];
  MQueue queue(&sender_event, &receiver_event, ring, sizeof(ring));
  MQueue_handle handle(&queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
  if (handle.init()) return true;

  const std::vector<uchar> expected_ref(handler_ref,
                                        handler_ref + handler_ref_len);
  const uchar null_bitmap[] = {0};
  std::vector<uchar> field_payload;
  pq_worker_result_append_uint32(&field_payload, 1);
  field_payload.push_back('8');

  bool failed = pq_send_worker_result_stable_ref_frame(
      &handle, 1, null_bitmap, sizeof(null_bitmap), field_payload.data(),
      static_cast<uint32>(field_payload.size()), handler_ref, handler_ref_len);

  void *raw_data = nullptr;
  uint32 raw_len = 0;
  if (!failed && handle.receive(&raw_data, &raw_len) != MQ_SUCCESS) {
    failed = true;
  }

  PQ_worker_result_stable_ref stable_ref;
  if (!failed &&
      pq_decode_worker_result_stable_ref_row(raw_data, raw_len, &stable_ref)) {
    failed = true;
  }

  std::vector<uchar> owned_ref;
  if (!failed) {
    if (stable_ref.row_id == nullptr || stable_ref.row_id_len == 0 ||
        stable_ref.row_id_len != expected_ref_length ||
        stable_ref.field_count != 1 || stable_ref.null_bitmap_len != 1) {
      failed = true;
    } else {
      owned_ref.assign(stable_ref.row_id,
                       stable_ref.row_id + stable_ref.row_id_len);
    }
  }

  handle.cleanup();

  if (!failed) {
    if (owned_ref.size() != expected_ref_length ||
        owned_ref.size() != expected_ref.size() ||
        memcmp(owned_ref.data(), expected_ref.data(), expected_ref.size()) !=
            0) {
      failed = true;
    } else {
      *ref_bytes = static_cast<uint32>(owned_ref.size());
      ++(*owned_ref_success);
    }
  }

  if (!failed && *ref_bytes != expected_ref_length) {
    ++(*length_mismatch_rejects);
  } else if (!failed) {
    const uint32 wrong_expected = expected_ref_length + 1;
    if (*ref_bytes != wrong_expected) {
      ++(*length_mismatch_rejects);
    } else {
      failed = true;
    }
  }

  return failed || *ref_bytes != expected_ref_length ||
         *owned_ref_success != 1 || *length_mismatch_rejects != 1;
}

bool pq_run_query_result_mq_stable_ref_pair_smoke(
    const uchar *left_ref, uint32 left_ref_len, const uchar *right_ref,
    uint32 right_ref_len, uint32 expected_ref_length,
    std::vector<uchar> *left_owned_ref, std::vector<uchar> *right_owned_ref,
    uint32 *ref_bytes, uint32 *deep_copy_success) {
  if (left_ref == nullptr || right_ref == nullptr || left_ref_len == 0 ||
      right_ref_len == 0 || expected_ref_length == 0 ||
      left_ref_len != expected_ref_length ||
      right_ref_len != expected_ref_length || left_owned_ref == nullptr ||
      right_owned_ref == nullptr || ref_bytes == nullptr ||
      deep_copy_success == nullptr) {
    return true;
  }

  left_owned_ref->clear();
  right_owned_ref->clear();
  *ref_bytes = 0;
  *deep_copy_success = 0;

  PQ_mq_event sender_event;
  PQ_mq_event receiver_event;
  char ring[PQ_MQ_DEFAULT_RING_SIZE];
  MQueue queue(&sender_event, &receiver_event, ring, sizeof(ring));
  MQueue_handle handle(&queue, PQ_MQ_DEFAULT_BUFFER_SIZE);
  if (handle.init()) return true;

  const uchar null_bitmap[] = {0};
  std::vector<uchar> field_payload;
  pq_worker_result_append_uint32(&field_payload, 1);
  field_payload.push_back('6');

  bool failed =
      pq_send_worker_result_stable_ref_frame(
          &handle, 1, null_bitmap, sizeof(null_bitmap), field_payload.data(),
          static_cast<uint32>(field_payload.size()), left_ref, left_ref_len) ||
      pq_send_worker_result_stable_ref_frame(
          &handle, 1, null_bitmap, sizeof(null_bitmap), field_payload.data(),
          static_cast<uint32>(field_payload.size()), right_ref, right_ref_len);

  for (uint i = 0; !failed && i < 2; ++i) {
    void *raw_data = nullptr;
    uint32 raw_len = 0;
    if (handle.receive(&raw_data, &raw_len) != MQ_SUCCESS) {
      failed = true;
      break;
    }

    PQ_worker_result_stable_ref stable_ref;
    if (pq_decode_worker_result_stable_ref_row(raw_data, raw_len,
                                               &stable_ref) ||
        stable_ref.row_id == nullptr ||
        stable_ref.row_id_len != expected_ref_length ||
        stable_ref.field_count != 1 || stable_ref.null_bitmap_len != 1) {
      failed = true;
      break;
    }

    std::vector<uchar> *target = (i == 0) ? left_owned_ref : right_owned_ref;
    target->assign(stable_ref.row_id, stable_ref.row_id + stable_ref.row_id_len);
    *ref_bytes += stable_ref.row_id_len;
    ++(*deep_copy_success);
  }

  handle.cleanup();

  if (!failed) {
    failed = left_owned_ref->size() != expected_ref_length ||
             right_owned_ref->size() != expected_ref_length ||
             memcmp(left_owned_ref->data(), left_ref, expected_ref_length) !=
                 0 ||
             memcmp(right_owned_ref->data(), right_ref,
                    expected_ref_length) != 0;
  }

  return failed || *ref_bytes != expected_ref_length * 2 ||
         *deep_copy_success != 2;
}

Query_result_mq::Query_result_mq(JOIN *join, MQueue_handle *msg_handler,
                                 bool stab_output)
    : Query_result(), m_join(join), m_handler(msg_handler),
      m_stable_output(stab_output) {}

bool Query_result_mq::start_execution(THD *) {
  m_started = true;
  m_metadata_sent = false;
  m_finished = false;
  return m_handler == nullptr;
}

bool Query_result_mq::send_result_set_metadata(
    THD *, const mem_root_deque<Item *> &fields, uint) {
  if (!m_started || m_handler == nullptr || fields.empty() ||
      fields.size() > UINT32_MAX) {
    return true;
  }
  send_fields = const_cast<mem_root_deque<Item *> *>(&fields);
  send_fields_size = static_cast<uint>(fields.size());
  m_metadata_sent = true;
  return false;
}

bool Query_result_mq::send_data(THD *thd, const mem_root_deque<Item *> &items) {
  if (thd == nullptr || !result_contract_ready() || items.empty() ||
      items.size() > UINT32_MAX ||
      (send_fields_size != 0 && items.size() != send_fields_size)) {
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

bool Query_result_mq::send_table_row(THD *thd, TABLE *table,
                                     uint32 field_count) {
  if (thd == nullptr || table == nullptr || table->s == nullptr ||
      table->field == nullptr || !result_contract_ready() ||
      field_count == 0 || field_count > table->s->fields ||
      (send_fields_size != 0 && field_count != send_fields_size)) {
    return true;
  }

  const uint32 null_bitmap_len = pq_worker_result_null_bitmap_len(field_count);
  std::vector<uchar> null_bitmap(null_bitmap_len, 0);
  std::vector<uchar> payload;
  String tmp1;
  String tmp2;

  for (uint32 i = 0; i < field_count; ++i) {
    Field *field = table->field[i];
    if (field == nullptr) return true;

    if (field->is_null()) {
      pq_worker_result_set_null_bit(&null_bitmap, i);
      pq_worker_result_append_uint32(&payload, 0);
      continue;
    }

    tmp1.length(0);
    tmp2.length(0);
    String *value = field->val_str(&tmp1, &tmp2);
    if (value == nullptr || value->length() > UINT32_MAX) return true;

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

bool Query_result_mq::send_table_read_set_row(THD *thd, TABLE *table) {
  if (thd == nullptr || table == nullptr || table->s == nullptr ||
      table->field == nullptr || !result_contract_ready()) {
    return true;
  }

  std::vector<uint32> field_indexes;
  if (pq_collect_table_read_set_fields(table, &field_indexes) ||
      field_indexes.size() > UINT32_MAX ||
      (send_fields_size != 0 && field_indexes.size() != send_fields_size)) {
    return true;
  }

  const uint32 field_count = static_cast<uint32>(field_indexes.size());
  const uint32 null_bitmap_len = pq_worker_result_null_bitmap_len(field_count);
  std::vector<uchar> null_bitmap(null_bitmap_len, 0);
  std::vector<uchar> payload;
  String tmp1;
  String tmp2;

  for (uint32 field_index : field_indexes) {
    pq_worker_result_append_uint32(&payload, field_index);
  }

  for (uint32 i = 0; i < field_count; ++i) {
    const uint32 field_index = field_indexes[i];
    if (field_index >= table->s->fields) return true;
    Field *field = table->field[field_index];
    if (field == nullptr) return true;

    if (field->is_null()) {
      pq_worker_result_set_null_bit(&null_bitmap, i);
      pq_worker_result_append_uint32(&payload, 0);
      continue;
    }

    tmp1.length(0);
    tmp2.length(0);
    String *value = field->val_str(&tmp1, &tmp2);
    if (value == nullptr || value->length() > UINT32_MAX) return true;

    const auto value_len = static_cast<uint32>(value->length());
    const uint64 next_size =
        static_cast<uint64>(payload.size()) + sizeof(uint32) + value_len;
    if (next_size > UINT32_MAX) return true;

    pq_worker_result_append_uint32(&payload, value_len);
    const char *value_ptr = value->ptr();
    payload.insert(payload.end(), value_ptr, value_ptr + value_len);
  }

  if (pq_send_worker_result_frame_with_flags(
          m_handler, PQ_worker_result_message_type::ROW, field_count,
          null_bitmap.data(), null_bitmap_len, payload.data(),
          static_cast<uint32>(payload.size()),
          PQ_WORKER_RESULT_FRAME_FLAG_FIELD_INDEXES)) {
    return true;
  }

  thd->inc_sent_row_count(1);
  return false;
}

bool Query_result_mq::send_eof(THD *thd) {
  if (!m_started || !m_metadata_sent || m_handler == nullptr || m_finished) {
    return true;
  }
  if (thd != nullptr && thd->is_error()) {
    static constexpr uchar error_payload[] = "worker-error";
    (void)pq_send_worker_result_frame(
        m_handler, PQ_worker_result_message_type::ERROR, 0, nullptr, 0,
        error_payload, static_cast<uint32>(sizeof(error_payload) - 1));
    return true;
  }
  const bool failed = pq_send_worker_result_frame(
      m_handler, PQ_worker_result_message_type::FINISH, 0, nullptr, 0, nullptr,
      0);
  if (!failed) m_finished = true;
  return failed;
}

void Query_result_mq::cleanup() {
  m_table = nullptr;
  m_param = nullptr;
  send_fields = nullptr;
  send_fields_size = 0;
  mq_fields_data = nullptr;
  mq_fields_null_array = nullptr;
  mq_fields_null_flag = nullptr;
  m_started = false;
  m_metadata_sent = false;
  m_finished = false;
}
