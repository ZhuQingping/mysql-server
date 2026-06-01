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

#include "sql/query_result.h"

struct TABLE;
class Temp_table_param;
class JOIN;
struct Field_raw_data;
class handler;

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
        mq_fields_data(nullptr),
        mq_fields_null_array(nullptr),
        mq_fields_null_flag(nullptr),
        m_stable_output(false) {}

  Query_result_mq(JOIN *join, MQueue_handle *msg_handler,
                  bool stab_output = false);
  bool send_result_set_metadata(THD *thd, const mem_root_deque<Item *> &,
                                uint flags) override;
  bool send_data(THD *thd, const mem_root_deque<Item *> &) override;
  bool send_eof(THD *thd MY_ATTRIBUTE((unused))) override;
  void cleanup() override;
  MQueue_handle *get_mq_handler() override { return m_handler; }

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
};

#endif  // QUERY_RESULT_MQ_INCLUDED
