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

#ifndef SQL_PARALLEL_QUERY_EXCHANGE_SORT_INCLUDED
#define SQL_PARALLEL_QUERY_EXCHANGE_SORT_INCLUDED

#include <cstdint>
#include <vector>

#include "sql/parallel_query/binary_heap.h"
#include "sql/parallel_query/exchange.h"

struct TABLE;
class handler;

struct PQ_orderby_smoke_record {
  int64 key{0};
  uint32 worker_id{0};
  uint32 row_id{0};
};

enum class PQ_orderby_batch_compare_state : uint8 {
  NOT_EVALUATED = 0,
  ALWAYS_TRUE,
  NOT_ALWAYS_TRUE,
};

struct PQ_orderby_cached_record {
  std::vector<uchar> row_image;
  std::vector<uchar> row_id;
  std::vector<uchar> sort_key;
  uint32 worker_id{0};
  bool has_sort_key{false};
};

struct PQ_orderby_record_batch {
  std::vector<PQ_orderby_cached_record> records;
  size_t next_pos{0};
  bool completed{false};
  bool new_group{false};
  PQ_orderby_batch_compare_state compare_state{
      PQ_orderby_batch_compare_state::NOT_EVALUATED};
};

struct PQ_orderby_cached_merge_ctx {
  PQ_orderby_record_batch *batches{nullptr};
  bool descending{false};
};

enum class PQ_orderby_frame_type : uint16 {
  ROW = 1,
  FINISH = 2,
  ERROR = 3,
};

enum class PQ_orderby_row_id_source : uint8 {
  NONE = 0,
  SYNTHETIC_SMOKE = 1,
  HANDLER_REF = 2,
};

enum class PQ_orderby_loader_status : uint8 {
  ROW = 1,
  FINISH,
  WOULD_BLOCK,
  DETACHED,
  ERROR,
};

enum class PQ_orderby_shadow_read_status : uint8 {
  ROW = 1,
  EOF_REACHED,
  ERROR,
};

enum class PQ_orderby_stream_read_status : uint8 {
  ROW = 1,
  EOF_REACHED,
  WOULD_BLOCK,
  DETACHED,
  ERROR,
};

enum class PQ_orderby_materialize_status : uint8 {
  ROW = 1,
  EOF_REACHED,
  WOULD_BLOCK,
  DETACHED,
  UNSUPPORTED,
  DISABLED,
  ERROR,
};

enum class PQ_orderby_ordered_read_status : uint8 {
  ROW = 1,
  EOF_REACHED,
  WOULD_BLOCK,
  DETACHED,
  UNSUPPORTED,
  DISABLED,
  ERROR,
};

struct PQ_orderby_frame_header {
  uint32 magic;
  uint16 version;
  uint16 type;
  uint32 flags;
  uint32 record_image_len;
  uint32 row_id_len;
  uint32 sort_key_len;
  uint32 payload_len;
};

struct PQ_orderby_decoded_frame {
  PQ_orderby_frame_type type{PQ_orderby_frame_type::ERROR};
  const uchar *record_image{nullptr};
  uint32 record_image_len{0};
  const uchar *row_id{nullptr};
  uint32 row_id_len{0};
  const uchar *sort_key{nullptr};
  uint32 sort_key_len{0};
};

struct PQ_orderby_row_id_contract {
  PQ_orderby_row_id_source source{PQ_orderby_row_id_source::NONE};
  uint32 expected_ref_length{0};
  bool stable_output_required{false};
};

struct PQ_orderby_handler_ref_lifetime_contract {
  PQ_orderby_row_id_contract row_id_contract;
  bool ref_length_verified{false};
  bool ref_deep_copied{false};
  bool worker_record_current{false};
  bool handler_can_advance{false};
  bool worker_detached{false};
};

struct PQ_orderby_worker_frame_producer_owner {
  MQueue_handle *handle{nullptr};
  uint32 worker_id{0};
  int64 last_sort_key{0};
  bool has_last_sort_key{false};
  bool finished{false};
  bool detached{false};
  bool cleanup_seen{false};
};

struct PQ_orderby_sort_state_shape {
  uint32 workers{0};
  uint32 sort_order_length{0};
  uint32 max_record_length{0};
  uint32 ref_length{0};
  bool stable_output{false};
  bool index_sort{false};
  bool rowid_required{false};
  bool initialized{false};
};

struct PQ_orderby_real_init_state_shape {
  uint32 workers{0};
  uint32 sort_order_length{0};
  uint32 max_record_length{0};
  uint32 ref_length{0};
  uint32 compare_key_buffer_length{0};
  uint32 tmp_key_buffer_length{0};
  uint32 min_record_slots{0};
  uint32 record_group_slots{0};
  bool stable_output{false};
  bool index_sort{false};
  bool rowid_required{false};
  bool initialized{false};
};

struct PQ_orderby_runtime_sort_state_owner_shape {
  uint32 workers{0};
  uint32 sort_order_length{0};
  uint32 max_record_length{0};
  uint32 ref_length{0};
  uint32 sort_param_order_length{0};
  uint32 sort_param_max_record_length{0};
  uint32 sort_param_ref_length{0};
  bool stable_output{false};
  bool index_sort{false};
  bool initialized{false};
  bool runtime_ready{false};
  bool filesort_constructed{false};
  bool sort_param_initialized{false};
  bool join_state_mutated{false};
  bool filesorts_cleanup_attached{false};
  bool qep_attached{false};
  bool access_path_attached{false};
};

struct PQ_orderby_heap_reader_state_shape {
  uint32 workers{0};
  bool initialized{false};
  bool heap_initialized{false};
  bool cleanup_seen{false};
};

struct PQ_orderby_heap_reader_counters {
  uint32 finishes_read{0};
  uint32 would_blocks_read{0};
  uint32 errors_read{0};
  uint32 detaches_read{0};
  uint32 refills_read{0};
  uint32 heap_replaces_read{0};
  uint32 heap_removes_read{0};
};

struct PQ_orderby_materializer_owner_shape {
  TABLE *leader_table{nullptr};
  uint32 record_length{0};
  uint field_index{0};
  uint32 rows_materialized{0};
  bool initialized{false};
  bool original_record_saved{false};
  bool bitmap_state_saved{false};
  bool had_read_bit{false};
  bool had_write_bit{false};
  bool restored{false};
};

struct PQ_orderby_ordered_diag_contract_shape {
  bool would_block_distinct_from_eof{false};
  bool error_cleanup_ready{false};
  bool detach_cleanup_ready{false};
  bool kill_polling_wired{false};
  bool default_read_ready{false};
  bool diagnostics_ready{false};
};

struct PQ_orderby_ref_owner_shape {
  handler *tie_break_file{nullptr};
  uint32 expected_ref_length{0};
  bool stable_output{false};
  bool initialized{false};
};

constexpr uint32 PQ_ORDERBY_FRAME_MAGIC = 0x50514f46;  // "PQOF"
constexpr uint16 PQ_ORDERBY_FRAME_VERSION = 1;

bool pq_validate_orderby_frame(const void *raw_data, uint32 raw_len,
                               const PQ_orderby_frame_header **header,
                               const uchar **payload);

bool pq_decode_orderby_frame(const void *raw_data, uint32 raw_len,
                             PQ_orderby_decoded_frame *decoded);

bool pq_validate_orderby_row_id_contract(
    const PQ_orderby_decoded_frame *decoded,
    const PQ_orderby_row_id_contract &contract);

bool pq_validate_orderby_handler_ref_lifetime_contract(
    const PQ_orderby_decoded_frame *decoded,
    const PQ_orderby_handler_ref_lifetime_contract &contract);

bool pq_orderby_handler_ref_adapter_smoke(handler *tie_break_file,
                                          const uchar *left_ref,
                                          uint32 left_ref_len,
                                          const uchar *right_ref,
                                          uint32 right_ref_len,
                                          int *cmp_forward,
                                          int *cmp_reverse);

bool pq_run_orderby_handler_ref_wire_smoke(const uchar *record_image,
                                           uint32 record_image_len,
                                           const uchar *handler_ref,
                                           uint32 ref_length,
                                           uint32 *decoded_ref_bytes,
                                           uint32 *contract_success);

class Exchange_sort final : public Exchange {
 public:
  Exchange_sort() = default;
  Exchange_sort(uint32 nqueues, uint32 ring_size) : Exchange(nqueues, ring_size) {}

  bool read_mq_message(MQMessageType &type, void **datap,
                       uint32 &data_len) override;

  ExchangeType get_exchange_type() const override { return EXCHANGE_SORT; }

  bool run_synthetic_order_merge_smoke(uint32 *rows_read);
  bool run_cached_record_adapter_smoke(uint32 *rows_read);
  bool run_orderby_frame_contract_smoke(uint32 *rows_read,
                                        uint32 *finishes_read,
                                        uint32 *errors_read);
  bool run_orderby_read_mq_message_controlled_smoke();
  bool run_orderby_worker_frame_producer_smoke(uint32 *rows_read,
                                               uint32 *finishes_read,
                                               uint32 *errors_read);
  bool run_orderby_worker_producer_adapter_skeleton_smoke(
      uint32 *rows_read, uint32 *finishes_read, uint32 *errors_read,
      uint32 *order_rejects, uint32 *after_finish_rejects);
  bool run_orderby_frame_merge_smoke(uint32 *rows_read,
                                     uint32 *finishes_read);
  bool run_orderby_frame_merge_edge_smoke(uint32 *rows_read,
                                          uint32 *finishes_read,
                                          uint32 *errors_read);
  bool run_orderby_frame_materialization_smoke(TABLE *leader_table,
                                               uint32 *rows_read,
                                               uint32 *unsupported);
  bool run_orderby_streaming_materialization_smoke(TABLE *leader_table,
                                                   uint32 *rows_read,
                                                   uint32 *unsupported,
                                                   uint32 *length_errors);
  bool materialize_next_ordered_record_image_status(
      TABLE *leader_table, PQ_orderby_materialize_status *status);
  bool run_orderby_materialize_api_skeleton_smoke(TABLE *leader_table,
                                                  uint32 *disabled,
                                                  uint32 *unsupported,
                                                  uint32 *rows_read);
  bool run_orderby_ordered_reader_skeleton_smoke(
      uint32 *rows_read, uint32 *finishes_read, uint32 *would_blocks_read,
      uint32 *errors_read, uint32 *detaches_read, uint32 *refills_read,
      uint32 *heap_replaces_read, uint32 *heap_removes_read);
  bool run_orderby_ordered_diag_skeleton_smoke(uint32 *kill_not_wired);
  bool run_orderby_ref_owner_smoke(handler *tie_break_file,
                                   uint32 expected_ref_length,
                                   uint32 *ref_bytes,
                                   uint32 *mismatch_rejects,
                                   uint32 *no_handler_rejects,
                                   uint32 *no_ref_rejects);
  bool run_orderby_sort_state_shape_smoke();
  bool run_orderby_sort_state_shape_handoff_smoke(uint32 workers,
                                                  bool stable_output,
                                                  bool index_sort,
                                                  uint32 sort_order_length,
                                                  uint32 max_record_length,
                                                  uint32 ref_length);
  bool run_orderby_runtime_sort_state_owner_shape_smoke();
  bool run_orderby_runtime_sort_param_lifetime_smoke();
  bool run_orderby_real_init_state_owner_smoke();
  bool run_orderby_real_init_allocation_smoke();
  bool run_orderby_frame_loader_smoke();
  bool run_orderby_shadow_read_smoke();
  bool run_orderby_streaming_heap_read_smoke(uint32 *rows_read,
                                             uint32 *finishes_read,
                                             uint32 *would_blocks_read,
                                             uint32 *errors_read,
                                             uint32 *detaches_read,
                                             uint32 *refills_read,
                                             uint32 *heap_replaces_read,
                                             uint32 *heap_removes_read);

  bool init_order_gather_shape(uint32 workers, bool stable_output,
                               bool index_sort);
  bool read_ordered_record_shape();
  void cleanup_order_gather_shape();

  bool order_gather_shape_initialized() const {
    return m_order_shape_initialized;
  }

  bool order_gather_shape_stable_output() const {
    return m_order_shape_stable_output;
  }

  bool order_gather_shape_index_sort() const {
    return m_order_shape_index_sort;
  }

  uint32 order_gather_shape_workers() const { return m_order_shape_workers; }

 private:
  std::vector<PQ_orderby_cached_record> m_min_records;
  std::vector<PQ_orderby_record_batch> m_record_groups;
  std::vector<uchar> m_compare_key_buffers[2];
  std::vector<uchar> m_tmp_key_buffer;
  binary_heap *m_order_heap{nullptr};
  uint32 m_order_shape_workers{0};
  bool m_order_shape_initialized{false};
  bool m_order_shape_stable_output{false};
  bool m_order_shape_index_sort{false};
  PQ_orderby_sort_state_shape m_sort_state_shape;
  PQ_orderby_real_init_state_shape m_real_init_state_shape;
  PQ_orderby_runtime_sort_state_owner_shape m_runtime_sort_state_owner_shape;
  PQ_orderby_heap_reader_state_shape m_heap_reader_state_shape;
  PQ_orderby_heap_reader_counters m_heap_reader_counters;
  PQ_orderby_cached_merge_ctx m_heap_reader_ctx;
  std::vector<bool> m_heap_reader_in_heap;
  std::vector<bool> m_heap_reader_terminal_workers;
  PQ_orderby_materializer_owner_shape m_materializer_owner_shape;
  std::vector<uchar> m_materializer_original_record;
  bool m_orderby_read_mq_shape_enabled{false};
  bool m_orderby_rich_status_shape_enabled{false};
  bool m_orderby_materializer_shape_enabled{false};
  bool m_orderby_default_heap_reader_shape_enabled{false};

  bool init_sort_state_shape(uint32 workers, bool stable_output,
                             bool index_sort, uint32 sort_order_length,
                             uint32 max_record_length, uint32 ref_length);
  void cleanup_sort_state_shape();
  bool init_real_init_state_owner_shape(uint32 workers, bool stable_output,
                                        bool index_sort,
                                        uint32 sort_order_length,
                                        uint32 max_record_length,
                                        uint32 ref_length);
  bool init_runtime_sort_state_owner_shape(uint32 workers, bool stable_output,
                                           bool index_sort,
                                           uint32 sort_order_length,
                                           uint32 max_record_length,
                                           uint32 ref_length);
  bool init_runtime_sort_param_scalar_shape(uint32 sort_order_length,
                                            uint32 max_record_length,
                                            uint32 ref_length);
  bool allocate_real_init_buffers_shape();
  bool read_orderby_frame_from_worker_shape(
      MQueue_handle *handle, PQ_orderby_loader_status *status,
      PQ_orderby_decoded_frame *decoded);
  void enable_orderby_read_mq_shape_for_smoke(bool enabled);
  bool load_orderby_frame_to_record_group(MQueue_handle *handle,
                                          uint32 worker_id,
                                          PQ_orderby_loader_status *status);
  bool read_ordered_record_shadow_shape(
      std::vector<uchar> *row_image,
      PQ_orderby_shadow_read_status *status);
  bool read_ordered_record_stream_shape(
      binary_heap *heap, std::vector<bool> *in_heap,
      std::vector<bool> *terminal_workers, std::vector<uchar> *row_image,
      PQ_orderby_stream_read_status *status, uint32 *finishes_read,
      uint32 *would_blocks_read, uint32 *errors_read, uint32 *detaches_read,
      uint32 *refills_read, uint32 *heap_replaces_read,
      uint32 *heap_removes_read);
  bool read_next_ordered_record_image_skeleton(
      binary_heap *heap, std::vector<bool> *in_heap,
      std::vector<bool> *terminal_workers, std::vector<uchar> *row_image,
      PQ_orderby_stream_read_status *status, uint32 *finishes_read,
      uint32 *would_blocks_read, uint32 *errors_read, uint32 *detaches_read,
      uint32 *refills_read, uint32 *heap_replaces_read,
      uint32 *heap_removes_read);
  bool init_orderby_heap_reader_state_shape(uint32 workers, bool descending);
  bool read_next_ordered_record_image_owned_shape(
      std::vector<uchar> *row_image, PQ_orderby_stream_read_status *status);
  bool read_ordered_record_rich_status_shape(
      std::vector<uchar> *row_image, PQ_orderby_ordered_read_status *status);
  bool run_orderby_rich_status_api_smoke();
  bool read_default_ordered_record_heap_shape(
      std::vector<uchar> *row_image, PQ_orderby_ordered_read_status *status);
  bool run_orderby_default_heap_reader_contract_smoke();
  bool run_orderby_ordered_diag_contract_smoke(
      PQ_orderby_ordered_diag_contract_shape *diag);
  bool init_orderby_materializer_owner_shape(TABLE *leader_table);
  bool materialize_ordered_record_owner_shape(
      TABLE *leader_table, const std::vector<uchar> &row_image,
      PQ_orderby_materialize_status *status);
  void cleanup_orderby_materializer_owner_shape();
  bool run_orderby_materializer_owner_shape_smoke(TABLE *leader_table);
  void cleanup_orderby_heap_reader_state_shape();
  void cleanup_real_init_state_owner_shape();
  void cleanup_runtime_sort_state_owner_shape();
};

#endif  // SQL_PARALLEL_QUERY_EXCHANGE_SORT_INCLUDED
