/* Copyright (c) 2025, Oracle and/or its affiliates.

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

#ifndef QUERY_RESULT_MQ_INCLUDED
#define QUERY_RESULT_MQ_INCLUDED

#include "sql/parallel_query/msg_queue.h"
#include "sql/query_result.h"

#include <vector>

struct Field_raw_data {
  uchar *m_ptr{nullptr};
  uint32 m_len{0};
  uchar m_var_len{0};
  bool m_need_send{true};
};

enum class PQ_worker_result_message_type : uint16 {
  ROW = 1,
  FINISH = 2,
  ERROR = 3,
};

struct PQ_worker_result_frame_header {
  uint32 magic;
  uint16 version;
  uint16 type;
  uint32 field_count;
  uint32 null_bitmap_len;
  uint32 payload_len;
  uint32 flags;
};

struct PQ_worker_result_decoded_field {
  /*
    Borrowed pointer into the validated worker-result frame payload. The caller
    must not keep it past the backing raw buffer lifetime or past the next
    MQueue_handle::receive() on the same queue.
  */
  const char *value{nullptr};
  uint32 value_len{0};
  bool is_null{false};
};

constexpr uint32 PQ_WORKER_RESULT_FRAME_MAGIC = 0x50515752;  // "PQWR"
constexpr uint16 PQ_WORKER_RESULT_FRAME_VERSION = 1;
constexpr uint32 PQ_WORKER_RESULT_FRAME_FLAG_STABLE_REF = 1U << 0;

struct PQ_worker_result_stable_ref {
  const uchar *row_id{nullptr};
  uint32 row_id_len{0};
  const uchar *null_bitmap{nullptr};
  uint32 null_bitmap_len{0};
  const uchar *field_payload{nullptr};
  uint32 field_payload_len{0};
  uint32 field_count{0};
};

struct TABLE;
class Temp_table_param;
class JOIN;
class handler;

bool pq_validate_worker_result_frame(
    const void *raw_data, uint32 raw_len,
    const PQ_worker_result_frame_header **header, const uchar **null_bitmap,
    const uchar **payload);

bool pq_decode_worker_result_row(
    const void *raw_data, uint32 raw_len,
    std::vector<PQ_worker_result_decoded_field> *fields);

bool pq_decode_worker_result_stable_ref_row(
    const void *raw_data, uint32 raw_len,
    PQ_worker_result_stable_ref *stable_ref);

bool pq_run_query_result_mq_contract_smoke(uint32 *rows_read,
                                           uint32 *finishes_read);

bool pq_run_query_result_mq_send_data_smoke(THD *thd, uint32 *rows_read,
                                            uint32 *finishes_read,
                                            uint32 *errors_read);

bool pq_run_query_result_mq_adapter_smoke(THD *thd, uint32 *rows_read,
                                          uint32 *finishes_read);

bool pq_run_query_result_mq_wiring_smoke(THD *thd, uint32 *rows_read,
                                         uint32 *finishes_read);

bool pq_run_query_result_mq_stable_ref_smoke(const uchar *handler_ref,
                                             uint32 handler_ref_len,
                                             uint32 *ref_bytes,
                                             uint32 *deep_copy_success,
                                             uint32 *normal_decode_rejects,
                                             uint32 *invalid_rejects);

bool pq_run_query_result_mq_stable_ref_adapter_smoke(
    const uchar *handler_ref, uint32 handler_ref_len,
    uint32 expected_ref_length, uint32 *ref_bytes, uint32 *owned_ref_success,
    uint32 *length_mismatch_rejects);

bool pq_run_query_result_mq_stable_ref_pair_smoke(
    const uchar *left_ref, uint32 left_ref_len, const uchar *right_ref,
    uint32 right_ref_len, uint32 expected_ref_length,
    std::vector<uchar> *left_owned_ref, std::vector<uchar> *right_owned_ref,
    uint32 *ref_bytes, uint32 *deep_copy_success);

/*
  This is used to get result from a query executed by PQ worker
*/

class Query_result_mq : public Query_result {
 public:
  Query_result_mq()
      : Query_result(),
        m_table(nullptr),
        m_param(nullptr),
        m_join(nullptr),
        m_handler(nullptr),
        send_fields(nullptr),
        send_fields_size(0),
        mq_fields_null_array(nullptr),
        mq_fields_null_flag(nullptr),
        m_stable_output(false) {}

  Query_result_mq(JOIN *join, MQueue_handle *msg_handler,
                  bool stab_output = false);
  bool start_execution(THD *thd) override;
  bool send_result_set_metadata(THD *thd, const mem_root_deque<Item *> &,
                                uint flags) override;
  bool send_data(THD *thd, const mem_root_deque<Item *> &) override;
  bool send_table_row(THD *thd, TABLE *table, uint32 field_count);
  bool send_eof(THD *thd MY_ATTRIBUTE((unused))) override;
  void cleanup() override;
  MQueue_handle *get_mq_handler() { return m_handler; }
  bool result_contract_ready() const {
    return m_started && m_metadata_sent && !m_finished;
  }

  TABLE *m_table{nullptr};
  Temp_table_param *m_param{nullptr};

 private:
  JOIN *m_join{nullptr};
  MQueue_handle *m_handler{nullptr};
  mem_root_deque<Item *> *send_fields{nullptr};
  uint send_fields_size{0};
  Field_raw_data *mq_fields_data{nullptr};
  bool *mq_fields_null_array{nullptr};
  char *mq_fields_null_flag{nullptr};
  bool m_stable_output;
  bool m_started{false};
  bool m_metadata_sent{false};
  bool m_finished{false};
};

#endif  // QUERY_RESULT_MQ_INCLUDED
