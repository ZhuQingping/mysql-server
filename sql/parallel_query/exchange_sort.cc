/* Copyright (c) 2020, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/parallel_query/exchange_sort.h"
#include "sql/bounded_queue.h"
#include "sql/cmp_varlen_keys.h"
#include "sql/sql_executor.h"

/**
 * allocate memory for members
 *
 * @retval:
 *  false for success, and otherwise true.
 */
bool Exchange_sort::alloc() {
  int i, j;
  int readers = launch_workers();
  THD *thd = get_thd();

  m_min_records = new (thd->pq_context().mem_root) mq_record_st *[readers] { NULL };
  if (!m_min_records) goto err;

  for (i = 0; i < readers; i++) {
    m_min_records[i] = new (thd->pq_context().mem_root) mq_record_st();
    if (!m_min_records[i]) goto err;
  }

  m_record_groups = new (thd->pq_context().mem_root) mq_records_batch_st[readers];
  if (!m_record_groups) goto err;

  for (i = 0; i < readers; i++) {
    m_record_groups[i].records =
        new (thd->pq_context().mem_root) mq_record_st *[MAX_RECORD_STORE] { NULL };
    if (!m_record_groups[i].records) goto err;

    for (j = 0; j < MAX_RECORD_STORE; j++) {
      m_record_groups[i].records[j] = new (thd->pq_context().mem_root) mq_record_st();
      if (!m_record_groups[i].records[j]) goto err;

      //@TODO: if we use two different tmp tables on worker and leader thread,
      // we should not directly use record[0]'s length as the buffer length
      if (m_sort_param) {
        m_record_groups[i].records[j]->m_length = get_table()->s->reclength;
        m_record_groups[i].records[j]->m_data =
            new (thd->pq_context().mem_root) uchar[get_table()->s->reclength];
        m_record_groups[i].records[j]->m_sort_key =
            new (thd->pq_context().mem_root) uchar[m_sort_param->max_record_length() + 1];

        if (!m_record_groups[i].records[j]->m_data ||
            !m_record_groups[i].records[j]->m_sort_key) {
          goto err;
        }
      } else {
        m_record_groups[i].records[j]->m_buffer_len = RECORD_BUFFER_SIZE;
        m_record_groups[i].records[j]->m_data =
            new (thd->pq_context().mem_root) uchar[RECORD_BUFFER_SIZE];
        if (!m_record_groups[i].records[j]->m_data) goto err;
      }

      if (is_stable()) {
        m_record_groups[i].records[j]->m_row_id =
            new (thd->pq_context().mem_root) uchar[ref_length()];
        if (!m_record_groups[i].records[j]->m_row_id) goto err;
      }
    }
  }
  m_heap = new (thd->pq_context().mem_root)
      binary_heap(readers + 1, this, heap_compare_node, thd);
  if (!m_heap || m_heap->init_binary_heap()) goto err;

  return false;

err:
  sql_print_error("allocate space in Exchange_sort::alloc() error");
  return true;
}

/*
 * init sort-related structure
 *
 * @retval:
 *    false for success, and otherwise true.
 */
bool Exchange_sort::init() {
  if (Exchange::init()) return true;

  THD *thd = get_thd();
  uint row_id_length = ref_length();

  /** init Sort_param */
  if (m_sort && m_sort->m_order) {
    /** generate sort_order */
    int s_length = m_sort->make_sortorder(m_sort->m_order, false);

    // Mark merge sort order @see mark_desc_groups
    uint pos = 0;
    for (auto ord = m_sort->m_order; ord; ord = ord->next, pos++) {
      if (m_desc_groups.find(ord) != m_desc_groups.end()) {
        m_sort->sortorder[pos].reverse = true;
      }
    }
    if (!s_length) return true;
    m_sort_param = new (thd->pq_context().mem_root) Sort_param();
    if (!m_sort_param) return true;

    /** generate sort_param */
    TABLE *const table = get_table();
    m_sort->tables.push_back(table);

    m_sort_param->init_for_filesort(
        m_sort, make_array(m_sort->sortorder, s_length),
        sortlength(thd, m_sort->sortorder, s_length), m_sort->tables,
        launch_workers(), false);
    m_sort_param->local_sortorder =
        Bounds_checked_array<st_sort_field>(m_sort->sortorder, s_length);

    /** cache sort key for compare */
    m_tmp_key = new (thd->pq_context().mem_root) uchar[row_id_length];
    memset(m_tmp_key, 0, row_id_length);
    if (!m_tmp_key) return true;
  }

  assert(m_sort_param || is_stable());
  if (m_sort_param) {
    int key_len = m_sort_param->max_record_length() + 1;
    keys[0] = new (thd->pq_context().mem_root) uchar[key_len];
    keys[1] = new (thd->pq_context().mem_root) uchar[key_len];
    memset(keys[0], 0, key_len);
    memset(keys[1], 0, key_len);

    if (!keys[0] || !keys[1]) return true;
  }

  if (is_stable()) {
    assert(row_id_length == m_file->ref_length);
  }

  /** alloc space for binary heap etc. */
  return alloc();
}

/** build the binary heap */
void Exchange_sort::build_heap() {
  int ngroups = launch_workers();
  for (int i = 0; i < ngroups; i++) {
    m_record_groups[i].completed = false;
    m_record_groups[i].n_read = 0;
    m_record_groups[i].n_total = 0;
    m_min_records[i]->m_data = nullptr;
  }

  /** reset for binary heap */
  m_heap->reset();
  /** when nowait = false, ensure that each slot has one record through
      read message from MQ in a blocking mode */
  bool nowait = false;

reread:
  for (int i = 0; i < ngroups; i++) {
    if (!m_record_groups[i].completed) {
      if (m_min_records[i]->m_data == nullptr) {
        if (read_group(i, nowait)) m_heap->add_unorderd(i);
      } else {
        load_group_records(i);
      }
    }
  }

  /** recheck each slot in m_records */
  for (int i = 0; i < ngroups; i++) {
    if (!m_record_groups[i].completed && !m_min_records[i]->m_data) {
      nowait = false;
      goto reread;
    }
  }

  /** build the binary heap */
  m_heap->build();
  m_init_heap = true;
}

/**return the next minimum record */
mq_record_st *Exchange_sort::get_min_record() {
  int i;

  if (!m_init_heap) {
    build_heap();
  } else {
    /** (1) obtain the top element of binary heap */
    i = m_heap->first();
    /**
     *  (2) try to read a record from MQ[i] in a blocking mode.
     *      If read_next() == true, then we push it into binary heap and
     *      adjust the binary heap; otherwise, there is no more message in
     * MQ[i], and we should remove the i-th worker from binary heap.
     *
     */
    if (read_group(i, false)) {
      bool direct_read = (m_record_groups[i].m_batch_status == ALWAYS_TRUE);
      m_heap->replace_first(i, direct_read);
      if (m_index_sort &&                    // table scan or index scan
          i == m_heap->first() &&            // current minimum batch
          m_heap->size() > 1 &&              // heap size is large than 1
          m_record_groups[i].m_new_group &&  // new coming group
          m_record_groups[i].n_total > 1 &&  // have unread data
          (m_record_groups[i].n_total > m_record_groups[i].n_read)) {
        update_batch_status(i);
      }
    } else {
      m_heap->remove_first();
    }
  }
  /** (3) fetch the top element of binary heap */
  if (m_heap->empty()) {
    return nullptr;
  } else {
    i = m_heap->first();
    return m_min_records[i];
  }
}

void Exchange_sort::update_batch_status(int i) {
  int lchild = m_heap->left_child(0);
  int rchild = m_heap->right_child(0);

  mq_record_st *cmp_lchild = lchild >= 0 ? get_record(lchild) : nullptr;
  mq_record_st *cmp_rchild = rchild >= 0 ? get_record(rchild) : nullptr;

  int max_element_idx = m_record_groups[i].n_total - 1;
  mq_record_st *current_max = m_record_groups[i].records[max_element_idx];
  bool cmp_lres{true}, cmp_rres{true};

  if (current_max && cmp_lchild &&
      heap_compare_records(cmp_lchild, current_max, this)) {
    cmp_lres = false;
  }

  if (cmp_lres && current_max && cmp_rchild &&
      heap_compare_records(cmp_rchild, current_max, this)) {
    cmp_rres = false;
  }
  m_record_groups[i].m_new_group = false;
  m_record_groups[i].m_batch_status =
      (cmp_lres && cmp_rres) ? ALWAYS_TRUE : NOT_ALWAYS_TRUE;
}

/**
 * try to load a batch of messages into a record group
 * @id: the worker to be loaded
 */
void Exchange_sort::load_group_records(int id) {
  assert(0 <= id && id < launch_workers());
  mq_records_batch_st *rec_group = &m_record_groups[id];
  /** try to read message from MQ with a non-blocking mode */
  for (int i = rec_group->n_total; i < MAX_RECORD_STORE; i++) {
    if (!load_group_record(id, i, &rec_group->completed, true)) break;
    /** now, read a new message */
    rec_group->n_total++;
  }

  rec_group->m_new_group = true;
  rec_group->m_batch_status = NOT_EVALUATED;
}

/**
 *  fetch the current minimum record of the id-th records group
 *
 * @i: the ID of group
 * @nowait: the fetch mode, nowait = false for blocking-mode and
 *            otherwise, nowait = true
 */
bool Exchange_sort::read_group(int id, bool nowait) {
  assert(0 <= id && id < launch_workers());
  mq_records_batch_st *rec_group = &m_record_groups[id];

  /** the record has been fetched into records_batch */
  if (rec_group->n_read < rec_group->n_total) {
    m_min_records[id] = rec_group->records[rec_group->n_read++];
    return true;
  } else if (rec_group->completed) {
    return false;
  } else {
    if (rec_group->n_read == rec_group->n_total)
      rec_group->n_read = rec_group->n_total = 0;

    /** fetch the record from the id-th MQ */
    int i = rec_group->n_read;
    if (!load_group_record(id, i, &rec_group->completed, nowait)) return false;

    rec_group->n_total++;
    m_min_records[id] = rec_group->records[rec_group->n_read++];

    /** load a batch of records into the id-th record group */
    load_group_records(id);
    return true;
  }
}

/**
 * store the message from table->record as mq_record_struct
 *
 * @data: the message data
 * @msg_len: the message length
 */
bool Exchange_sort::store_mq_record(mq_record_st *rec, uchar *data,
                                    uint32 msg_len) {
  assert(rec && data);

  if (m_sort_param) {
    assert(rec->m_data && rec->m_length == get_table()->s->reclength);
    // making a deep copy from data to table's->record[0]
    DBUG_EXECUTE_IF("pq_msort_error5", { goto err; });
    if (!convert_mq_data_to_record(data, msg_len, rec->m_row_id)) {
      return false;
    }
    memcpy(rec->m_data, get_table()->record[0], get_table()->s->reclength);
    rec->has_sort_key = false;
  } else {
    THD *thd = get_thd();
    // store row_id for comparison
    if (is_stable()) {
      assert(rec->m_row_id && thd == current_thd);
      memcpy(rec->m_row_id, data, ref_length());
    }
    // making a deep copy from data to rec. First, we determine whether rec has
    // enough space to copy data. If there is not enough space, then alloc space
    // for rec.
    if (msg_len > rec->m_buffer_len) {
      if (rec->m_data) destroy(rec->m_data);
      uint32 new_buffer_len = rec->m_buffer_len;
      while (msg_len > new_buffer_len) {
        new_buffer_len *= 2;
      }
      rec->m_data = new (thd->pq_context().mem_root) uchar[new_buffer_len];
      if (!rec->m_data) goto err;
      rec->m_buffer_len = new_buffer_len;
    }
    memcpy(rec->m_data, data, msg_len);
    rec->m_length = msg_len;
    rec->has_sort_key = false;
  }

  return true;

err:
  my_error(ER_STD_BAD_ALLOC_ERROR, MYF(0), "", "(MSort::store_mq_record)");
  return false;
}

/**
 * read one message from MQ[id] and then copy it to
 * m_record_groups[id].records[i]
 *
 * @id: the ID of worker
 * @i: the i-th cached record in m_record_groups[id]
 * @completed: indicates whether MQ[i] has been read completed
 * @nowait: the mode of read message from MQ
 */
bool Exchange_sort::load_group_record(int id, int i, bool *completed,
                                      bool nowait) {
  assert(0 <= id && id < launch_workers());
  assert(0 <= i && i < MAX_RECORD_STORE);

  MQueue_handle *handle = get_mq_handle(id);
  uchar *data = nullptr;
  uint32 msg_len = 0;

  if (completed) *completed = false;
  /** receive one message from MQ */
  MQ_RESULT result = handle->receive((void **)&data, &msg_len, nowait);
  mq_records_batch_st *rec_group = &m_record_groups[id];
  if (result == MQ_DETACHED) {
    rec_group->completed = true;
    return false;
  }
  if (result == MQ_WOULD_BLOCK) {
    return false;
  }

  assert(result == MQ_SUCCESS);
  /** copy data into m_record_groups[id].records[i] */
  if (store_mq_record(rec_group->records[i], data, msg_len)) return true;

  return false;
}

/**
 * read the minimum record among all workers and fill it into table->record[0]
 *
 * @retval: true for success, and otherwise false
 */
bool Exchange_sort::read_mq_record() {
  assert(get_exchange_type() == EXCHANGE_SORT);
  mq_record_st *record = get_min_record();
  if (!record) return false;

  if (m_sort_param) {
    assert(record->m_length == get_table()->s->reclength);
    memcpy(get_table()->record[0], record->m_data, record->m_length);
    return true;
  }

  return convert_mq_data_to_record(record->m_data, record->m_length);
}

/** cleanup allocated space */
void Exchange_sort::cleanup() {
  Exchange::cleanup();
  if (m_heap) m_heap->cleanup();
  if (m_sort_param) destroy(m_sort_param);
}

/**
 * compare table->record[0] of two workers in PQ_merge_sort
 * @a: the ID of first worker
 * @b: the ID of second worker
 * @arg: PQ_merge sort
 * @return
 *     true if a's record is less than b's record;
 *     false otherwise.
 */
bool heap_compare_records(mq_record_st *compare_a, mq_record_st *compare_b,
                          void *arg) {
  assert(arg && compare_a && compare_b);

  Exchange_sort *merge_sort = static_cast<Exchange_sort *>(arg);
  Filesort *filesort = merge_sort->get_filesort();
  assert(filesort && current_thd == merge_sort->get_thd());

  uchar *key_0 = merge_sort->get_key(0);
  uchar *key_1 = merge_sort->get_key(1);

  /** using previous old table when comparing row_id (or PK) */
  handler *file = merge_sort->get_file();
  uint ref_len MY_ATTRIBUTE((unused)) = merge_sort->ref_length();
  assert(ref_len == file->ref_length);

  bool force_stable_sort = merge_sort->is_stable();
  Sort_param *sort_param = merge_sort->get_sort_param();
  int key_len = 0, compare_len = 0;

  if (sort_param) {
    key_len = sort_param->max_record_length() + 1;
    compare_len = sort_param->max_compare_length();
  }

  /**
   * using row_id to achieve stable sort, i.e.,
   * record1 < record2 <=> key1 < key2 or (key1 = key2 && row_id1 < row_id2)
   */
  if (sort_param) {
    assert(merge_sort->get_table()->s->reclength == compare_a->m_length);
    if (!compare_a->has_sort_key) {
      memcpy(merge_sort->get_table()->record[0], compare_a->m_data,
             compare_a->m_length);
      sort_param->make_sortkey(key_0, key_len, filesort->tables);
      memcpy(compare_a->m_sort_key, key_0, key_len);
      compare_a->has_sort_key = true;
    } else {
      key_0 = compare_a->m_sort_key;
    }
  }

  if (sort_param) {
    assert(merge_sort->get_table()->s->reclength == compare_b->m_length);
    if (!compare_b->has_sort_key) {
      memcpy(merge_sort->get_table()->record[0], compare_b->m_data,
             compare_b->m_length);
      sort_param->make_sortkey(key_1, key_len, filesort->tables);
      memcpy(compare_b->m_sort_key, key_1, key_len);
      compare_b->has_sort_key = true;
    } else {
      key_1 = compare_b->m_sort_key;
    }
  }

  uchar *row_id_0 = nullptr;
  uchar *row_id_1 = nullptr;
  if (force_stable_sort) {
    row_id_0 = compare_a->m_row_id;
    row_id_1 = compare_b->m_row_id;
  }

  // c1: no ordering required
  if (!filesort->sortorder) {
    assert(sort_param == nullptr && force_stable_sort);
    assert(row_id_0 && row_id_1);
    return file->cmp_ref(row_id_0, row_id_1) < 0;
  } else {
    int cmp_key_result;
    // c2: with order
    if (sort_param->using_varlen_keys()) {
      cmp_varlen_keys(sort_param->local_sortorder, sort_param->use_hash, key_0,
                      key_1, &cmp_key_result);
      if (!force_stable_sort) {
        return cmp_key_result <= 0;
      } else {
        assert(row_id_0 && row_id_1);
        return (cmp_key_result < 0 ||
                (cmp_key_result == 0 && file->cmp_ref(row_id_0, row_id_1) < 0));
      }
    } else {
      int cmp = memcmp(key_0, key_1, compare_len);
      if (!force_stable_sort) {
        return cmp <= 0;
      } else {
        assert(row_id_0 && row_id_1);
        return (cmp < 0 || (cmp == 0 && file->cmp_ref(row_id_0, row_id_1) < 0));
      }
    }
  }
}

bool heap_compare_node(int a, int b, void *arg) {
  assert(arg);

  Exchange_sort *merge_sort = static_cast<Exchange_sort *>(arg);
  mq_record_st *compare_a = merge_sort->get_record(a);
  mq_record_st *compare_b = merge_sort->get_record(b);

  return heap_compare_records(compare_a, compare_b, arg);
}
