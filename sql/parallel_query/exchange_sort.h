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

class Exchange_sort final : public Exchange {
 public:
  Exchange_sort() = default;
  Exchange_sort(uint32 nqueues, uint32 ring_size) : Exchange(nqueues, ring_size) {}

  bool read_mq_message(MQMessageType &type, void **datap,
                       uint32 &data_len) override;

  ExchangeType get_exchange_type() const override { return EXCHANGE_SORT; }

  bool run_synthetic_order_merge_smoke(uint32 *rows_read);

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
  binary_heap *m_order_heap{nullptr};
  uint32 m_order_shape_workers{0};
  bool m_order_shape_initialized{false};
  bool m_order_shape_stable_output{false};
  bool m_order_shape_index_sort{false};
};

#endif  // SQL_PARALLEL_QUERY_EXCHANGE_SORT_INCLUDED
