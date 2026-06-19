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

Query_result_mq::Query_result_mq(JOIN *join, MQueue_handle *msg_handler,
                                 bool stab_output)
    : Query_result(), m_join(join), m_handler(msg_handler),
      m_stable_output(stab_output) {}

bool Query_result_mq::send_result_set_metadata(
    THD *, const mem_root_deque<Item *> &, uint) {
  return false;
}

bool Query_result_mq::send_data(THD *, const mem_root_deque<Item *> &) {
  return true;
}

bool Query_result_mq::send_eof(THD *) { return false; }

void Query_result_mq::cleanup() {
  m_table = nullptr;
  m_param = nullptr;
  send_fields = nullptr;
  send_fields_size = 0;
  mq_fields_null_array = nullptr;
  mq_fields_null_flag = nullptr;
}
