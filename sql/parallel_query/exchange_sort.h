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

#include "sql/parallel_query/binary_heap.h"
#include "sql/parallel_query/exchange.h"

struct PQ_orderby_smoke_record {
  int64 key{0};
  uint32 worker_id{0};
  uint32 row_id{0};
};

class Exchange_sort final : public Exchange {
 public:
  Exchange_sort() = default;
  Exchange_sort(uint32 nqueues, uint32 ring_size) : Exchange(nqueues, ring_size) {}

  bool read_mq_message(MQMessageType &type, void **datap,
                       uint32 &data_len) override;

  ExchangeType get_exchange_type() const override { return EXCHANGE_SORT; }

  bool run_synthetic_order_merge_smoke(uint32 *rows_read);
};

#endif  // SQL_PARALLEL_QUERY_EXCHANGE_SORT_INCLUDED
