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

#include "sql/parallel_query/query_result_mq.h"

#include "sql/field.h"
#include "sql/handler.h"
#include "sql/item_sum.h"
#include "sql/parallel_query/msg_queue.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_tmp_table.h"

/**
 * copy field->ptr to MQ
 *
 * @field_raw: the corresponding copy structure in MQ
 */
uint32 pq_build_field_raw(Field *field, Field_raw_data *field_raw) {
  // field must not be NULL value
  assert(field && !field->is_null());

  uint32 copy_bytes = 0;
  auto field_type = field->type();
  /*
   * For the variable-length field, we should first extract its
   * effective length and then only copy these effective content.
   * Corresponding, we need to use one byte to store field->length_bytes, as
   follows:
   *  |one byte | m_var_len | field_len |
                 \-------m_len-------/
   */
  if (field_type == MYSQL_TYPE_VARCHAR || field_type == MYSQL_TYPE_VAR_STRING) {
    Field_varstring *from = static_cast<Field_varstring *>(field);
    uint field_length =
        (from->get_length_bytes() == 1) ? *from->ptr : uint2korr(from->ptr);
    field_raw->m_ptr = from->ptr;
    field_raw->m_var_len = from->get_length_bytes();  //  m_var_len = 1 or 2
    field_raw->m_len = from->get_length_bytes() + field_length;

    // Note that: we use one more byte to store the
    // Field_varstring::length_bytes
    copy_bytes += 1 + field_raw->m_len;
  }
  /*
   * For the other fields, they are fixed-length fields whose field length
   * is field->pack_length();
   */
  else {
    field_raw->m_ptr = field->ptr;
    field_raw->m_len = field->pack_length();
    field_raw->m_var_len = 0;
    copy_bytes += field_raw->m_len;
  }
  return copy_bytes;
}

/**
 * build fields' raw data sending to MQ, and fill the NULL value info of field
 * to the null_array.
 *
 * @null_num: number of maybe-NULL field
 * @total_bytes: number of total copied bytes
 */
void pq_build_mq_fields(Field *field, Field_raw_data *mq_fields,
                        bool *null_array, int &null_num, uint32 &total_bytes) {
  /*
   * for a clearly defined NOT NULL field, its m_null_ptr is nullptr and we
   * should not mark it in null_array. For a maybe-NULL field, we first
   * determine this field is NULL or not.
   */
  /* (1) first, mark it as a NOT_CONST_ITEM */
  null_array[null_num++] = 0;

  /* (2) then, determine whether the field is NULL */
  null_array[null_num++] = field->is_null() ? 1 : 0;

  // If this field is a not NULL-value, then we copy it to MQ
  if (!null_array[null_num - 1]) {
    /* the case of NOT_CONST_ITEM & NOT_NULL_FIELD (i.e., 00)*/
    mq_fields->m_need_send = true;
    total_bytes += pq_build_field_raw(field, mq_fields);
  } else {
    /* the case of NOT_CONST_ITEM & NULL_FIELD (i.e., 01)*/
    mq_fields->m_need_send = false;
  }
}

/** store item's value into Field_raw_data */
void pq_build_mq_item(Item *item, Field_raw_data *mq_fields, bool *null_array,
                      int &null_num,
                      uint32 &total_bytes MY_ATTRIBUTE((unused))) {
  assert(item->skip_send_to_mq);
  null_array[null_num++] = 1;
  /* the case of CONST_ITEM & NOT_NULL_FIELD
   * (or CONST_ITEM & NULL_FIELD (e.g., Item_null)) */
  null_array[null_num++] = item->null_value;
  mq_fields->m_need_send = false;
}

bool pq_build_mq_count_distinct_item(Item *item, MQueue_handle *m_handler,
                                     Field_raw_data *mq_fields,
                                     bool *null_array, int &null_num,
                                     uint32 &total_bytes) {
  assert(item->type() == Item::SUM_FUNC_ITEM);
  assert(dynamic_cast<Item_sum *>(item)->sum_func() ==
         Item_sum::COUNT_DISTINCT_FUNC);
  Item_sum *item_sum = dynamic_cast<Item_sum *>(item);
  Batch_buffer_slot *bat_buf_slot = nullptr;
  bat_buf_slot = m_handler->m_ext_mgr->get_next_slot();
  Batch_buffer *bat_buf = bat_buf_slot->get_next_buffer();

  if (nullptr == bat_buf) {
    return true;
  }
  bat_buf->reset();
  ((Aggregator_distinct *)item_sum->get_aggr())->save_distinct_to_mq(bat_buf);

  null_array[null_num++] = 0;
  null_array[null_num++] = 0;
  mq_fields->m_need_send = true;
  mq_fields->m_len = PTR_SIGN;
  mq_fields->m_ptr = (uchar *)bat_buf;
  mq_fields->m_var_len = 0;
  total_bytes += sizeof(mq_fields->m_ptr);
  return false;
}

Query_result_mq::Query_result_mq(JOIN *join, MQueue_handle *msg_handler,
                                 bool stab_output)
    : Query_result(),
      m_table(nullptr),
      m_param(nullptr),
      send_fields(nullptr),
      send_fields_size(0),
      mq_fields_data(nullptr),
      mq_fields_null_array(nullptr),
      mq_fields_null_flag(nullptr) {
  m_join = join;
  m_handler = msg_handler;
  m_stable_output = stab_output;
}

#define MQ_FIELDS_DATA_HEADER_LENGTH 4

bool Query_result_mq::send_result_set_metadata(
    THD *thd, const mem_root_deque<Item *> &MY_ATTRIBUTE((unused)),
    uint flags MY_ATTRIBUTE((unused))) {
  m_param = new (thd->pq_mem_root) Temp_table_param();
  if (!m_param || m_join->make_worker_tmp_table()) return true;

  send_fields = &m_join->tmp_fields[REF_SLICE_PQ_TMP];
  uint field_size = send_fields->size();
  send_fields_size = field_size + MQ_FIELDS_DATA_HEADER_LENGTH;

  mq_fields_data = new (thd->pq_mem_root) Field_raw_data[send_fields_size]{};
  mq_fields_null_array = new (thd->pq_mem_root) bool[2 * field_size];
  mq_fields_null_flag = new (
      thd->pq_mem_root) char[field_size / MQ_FIELDS_DATA_HEADER_LENGTH + 2];

  if (!mq_fields_data || !mq_fields_null_array || !mq_fields_null_flag) {
    return true;
  }

  return false;
}

bool Query_result_mq::send_data(
    THD *thd, const mem_root_deque<Item *> &items MY_ATTRIBUTE((unused))) {
  DBUG_ENTER("Query_result_mq::send_data");
  int null_num = 0;
  uint32 total_copy_bytes = 0;
  int i, j;

  assert(send_fields_size > MQ_FIELDS_DATA_HEADER_LENGTH);

  Field *result_field = nullptr;
  int idx = MQ_FIELDS_DATA_HEADER_LENGTH;

  /* currently supporting ITEM_FIELD and ITEM_FUNC */
  for (auto it = send_fields->begin(); it != send_fields->end(); it++, idx++) {
    Item *item = *it;
    // c1: check const item
    if (item->skip_create_tmp_table) {
      pq_build_mq_item(item, &mq_fields_data[idx], mq_fields_null_array,
                       null_num, total_copy_bytes);
      continue;
    }

    // deal for Item_sum::COUNT_DISTINCT_FUNC
    if ((item->type() == Item::SUM_FUNC_ITEM) &&
        (dynamic_cast<Item_sum *>(item)->sum_func() ==
         Item_sum::COUNT_DISTINCT_FUNC)) {
      if (pq_build_mq_count_distinct_item(item, m_handler, &mq_fields_data[idx],
                                          mq_fields_null_array, null_num,
                                          total_copy_bytes)) {
        m_handler->send_exception_msg(ERROR_MSG);
        DBUG_RETURN(true);
      }
      continue;
    }

    if (item->type() == Item::FIELD_ITEM &&
        dynamic_cast<Item_field *>(item)->field->item_sum_ref != nullptr) {
      Item_sum *item_sum = dynamic_cast<Item_sum *>(
          dynamic_cast<Item_field *>(item)->field->item_sum_ref);
      if (item_sum->sum_func() == Item_sum::COUNT_DISTINCT_FUNC) {
        if (pq_build_mq_count_distinct_item(
                item_sum, m_handler, &mq_fields_data[idx], mq_fields_null_array,
                null_num, total_copy_bytes)) {
          m_handler->send_exception_msg(ERROR_MSG);
          DBUG_RETURN(true);
        }
        continue;
      }
    }

    // c3: check item_result_field and item_field
    bool need_store_in_result_field = true;
    result_field = item->get_result_field();
    if (!result_field) {
      if (item->type() == Item::FIELD_ITEM &&
          DBUG_EVALUATE_IF("pq_mq_error4", false, true)) {
        auto item_field = down_cast<Item_field *>(item);
        need_store_in_result_field =
            (item_field->field->type() != item_field->result_field->type()) ||
            (item_field->field->pack_length() !=
             item_field->result_field->pack_length());
        result_field = need_store_in_result_field ? item_field->result_field
                                                  : item_field->field;
      } else if (item->skip_send_to_mq) {
        // const item creates field in tmp table; however, we have no need
        // to send it as its value can also be captured on leader.
        pq_build_mq_item(item, &mq_fields_data[idx], mq_fields_null_array,
                         null_num, total_copy_bytes);
        continue;
      }

      if (!result_field) {
        // c4: other cases will be shielded in JOIN::check_first_rewritten_tab
        sql_print_error("not supported field");
        m_handler->send_exception_msg(ERROR_MSG);
        DBUG_RETURN(true);
      }
    }

    assert(result_field);
    if (need_store_in_result_field) {
      // set no_conversions = true is more safe.
      item->save_in_field(result_field, /*no_conversions=*/true);
    }

    pq_build_mq_fields(result_field, &mq_fields_data[idx], mq_fields_null_array,
                       null_num, total_copy_bytes);
  }

  assert((uint)null_num ==
         2 * (send_fields_size - MQ_FIELDS_DATA_HEADER_LENGTH));
  uint16 null_len = ((null_num % 8 == 0) ? null_num / 8 : null_num / 8 + 1) + 1;

  memset(mq_fields_null_flag, 0, null_len);

  /*
   * Now, we use 2 bps/field as a header to send each Item, where the first bit
   * indicates that the corresponding item is a CONST_ITEM or not, and the
   * second bit indicates that the related result_field is NULL_FILED or not.
   * These two bits have at most four status:
   *   (0, 0)   =>  NOT_CONST_ITEM & NON_NULL_FIELD
   *   (0, 1)   =>  NOT_CONST_ITEM & NULL_FIELD
   *   (1, 0)   =>  CONST_ITEM & NON_NULL_FIELD
   *   (1, 1)   =>  CONST_ITEM & NULL_FIELD (such as Item_null)
   *
   * Only for the first case (0, 0), we need send the field data to MQ.
   *
   * null_flag[j] = 0 indicates the corresponding field is NOT NULL (or it is
   * not a const_item()). otherwise, null_flag[j] = 1.
   */

  for (i = 0; i < null_num; i++) {
    if (mq_fields_null_array[i]) {
      j = (i >> 3) + 1;
      mq_fields_null_flag[j] += 1 << (7 - (i & 7));
    }
  }

  mq_fields_data[3].m_ptr = (uchar *)mq_fields_null_flag;
  mq_fields_data[3].m_len = null_len;
  total_copy_bytes += null_len;

  /* there are at most 4096 fields and thus null_len is less than 2 * 2^12/8 =
   * 2^10. So, we can use 2 bytes to store it.
   */
  mq_fields_data[2].m_ptr = (uchar *)&null_len;
  mq_fields_data[2].m_len = 2;
  total_copy_bytes += 2;

  if (m_stable_output) {
    auto file = m_join->qep_tab[m_join->idx_div_tab].table()->file;
    assert(file);
    // PQblockScanIterator::Read() called file->position() to fill file->ref
    mq_fields_data[1].m_ptr = &file->ref[0];
    mq_fields_data[1].m_len = file->ref_length;
    mq_fields_data[1].m_var_len = 0;
    mq_fields_data[1].m_need_send = file->ref_length ? true : false;
    total_copy_bytes += file->ref_length;
  } else {
    mq_fields_data[1].m_need_send = false;
  }

  /* for total_copy_bytes, it is less than 2^16 * 2^16 = 2^32 and
   * we can use 4 bytes to store it
   */
  mq_fields_data[0].m_ptr = (uchar *)&total_copy_bytes;
  mq_fields_data[0].m_len = 4;

  // send messages to mq
  MQ_RESULT res;
  for (i = 0; i < (int)send_fields_size; i++) {
    /* for the case of NULL field, we need not send msg to MQ */
    if (!mq_fields_data[i].m_need_send) continue;

    res = m_handler->send(&mq_fields_data[i]);

    // In some case, we should detach the MQ and thus the MQ_DETACHED status can
    // also
    // be considered as an normal status.
    if (res == MQ_DETACHED) DBUG_RETURN(false);

    if (res != MQ_SUCCESS || DBUG_EVALUATE_IF("pq_mq_error5", true, false)) {
      sql_print_error("send message to MQ error");
      m_handler->send_exception_msg(ERROR_MSG);
      DBUG_RETURN(true);
    }
  }

  thd->inc_sent_row_count(1);
  DBUG_RETURN(false);
}

void Query_result_mq::cleanup() {
  if (m_param) {
    m_param->cleanup();
    destroy(m_param);
    m_param = nullptr;
  }

  if (m_table) {
    close_tmp_table(m_table);
    free_tmp_table(m_table);
    m_table = nullptr;
  }

  if (mq_fields_data) {
    destroy(mq_fields_data);
    mq_fields_data = nullptr;
  }

  if (mq_fields_null_array) {
    destroy(mq_fields_null_array);
    mq_fields_null_array = nullptr;
  }

  if (mq_fields_null_flag) {
    destroy(mq_fields_null_flag);
    mq_fields_null_flag = nullptr;
  }
}

bool Query_result_mq::send_eof(THD *thd) {
  if (thd->is_error()) return true;
  ::my_eof(thd);
  return false;
}
