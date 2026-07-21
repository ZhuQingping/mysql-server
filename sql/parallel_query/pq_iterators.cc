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

/**
  @file
  Implementations of PQ iterators.
*/

#include "sql/parallel_query/pq_iterators.h"

#include <utility>

#include "scope_guard.h"
#include "sql/current_thd.h"
#include "sql/debug_sync.h"
#include "sql/item_strfunc.h"
#include "sql/iterators/composite_iterators.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/explain_access_path.h"
#include "sql/mysqld.h"
#include "sql/parallel_query/exchange_sort.h"
#include "sql/parallel_query/pq_clone.h"  // list_query_blocks_to_clone
#include "sql/parallel_query/pq_hash_join_shared_context.h"
#include "sql/parallel_query/sql_parallel.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_sort.h"

using std::make_pair;
using std::pair;

static inline pair<uchar *, key_part_map> FindKeyBufferAndMap(
    const Index_lookup *ref);

ParallelScanIterator::ParallelScanIterator(THD *thd, QEP_TAB *tab, TABLE *table,
                                           double expected_rows,
                                           ha_rows *examined_rows, JOIN *join,
                                           Gather_operator *gather,
                                           bool stab_output,
                                           AccessPath *access_path)
    : TableRowIterator(thd, table),
      m_record(table->record[0]),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows),
      m_dop(gather->m_dop),
      m_join(join),
      m_gather(gather),
      m_record_gather(nullptr),
      m_order(nullptr),
      m_tab(tab),
      m_stable_sort(stab_output),
      m_root_access_path(access_path) {}

/**
 * construct filesort on leader when needing stab_output or merge_sort
 *
 * @retavl: false if success, and otherwise true
 */
bool ParallelScanIterator::pq_make_filesort(Filesort **sort) {
  *sort = NULL;
  SQL_I_List<ORDER> orig_list;

  /** construct sort order based on group */
  if (m_join->pq_context().pq_rebuilt_group) {
    assert(m_join->pq_context().saved_join_group_list);
    restore_list(m_join->pq_context().saved_join_group_list, orig_list);
    m_order = restore_optimized_group_order(
        orig_list, m_join->pq_context().saved_optimized_vars.optimized_group_flags);
  } else {
    /**
     * if sorting is built after the first rewritten table, then
     * we have no need to rebuilt the sort order on leader.
     */
    if (m_join->pq_context().pq_last_sort_idx > (int)m_join->primary_tables) {
      return false;
    } else if (m_stable_sort) {
      if ((m_order = m_join->order.order) == nullptr) {
        if (m_join->m_ordered_index_usage == JOIN::ORDERED_INDEX_ORDER_BY &&
            m_join->query_block->pq_context().saved_order_list_ptrs) {
          restore_list(m_join->pq_context().saved_join_order, orig_list);
          m_order = restore_optimized_group_order(
              orig_list, m_join->pq_context().saved_optimized_vars.optimized_order_flags);
        } else {
          QEP_TAB *tab = &m_join->pq_context().qep_tab0[m_join->pq_context().idx_div_tab];
          std::vector<std::string> used_key_fields;
          if (get_table_key_fields(tab, used_key_fields) ||
              DBUG_EVALUATE_IF("pq_msort_error1", true, false))
            return true;

          if (set_key_order(m_tab, used_key_fields, &m_order,
                            &m_join->ref_items[REF_SLICE_PQ_TMP]) ||
              DBUG_EVALUATE_IF("pq_msort_error2", true, false))
            return true;

          // When performing a reverse rane scan, the ORDER BY clause is
          // optimized out. The pq message queue needs to explicitly confirm
          // whether the sorting based on index keys is in reverse order. For
          // example, in the query 'SELECT * FROM t4 WHERE f3 = 1 AND f2 = 1 AND
          // f4 = 3 AND f5 IN(2,3) ORDER BY f4 DESC LIMIT 1', if the composite
          // index KEY(f2,f3,f4,f5) is used, the gather must sort the row data
          // from each worker in reverse order based on the f2, f3, f4, and f5
          // columns to ensure the result is consistent with serial execution.
          AccessPath *range_scan = tab->range_scan();
          if (range_scan && tab->type() == JT_RANGE) {
            assert(range_scan->type == AccessPath::INDEX_RANGE_SCAN);
            bool reverse = range_scan->index_range_scan().reverse;
            for (ORDER *ord = m_order; ord && reverse; ord = ord->next) {
              ord->direction = ORDER_DESC;
            }
          }
        }
      }
    }
  }

  /** support stable sort on TABLE/INDEX SCAN */
  if (m_order || m_stable_sort) {
    *sort = m_tab->filesort;
    if (!(*sort)) {
      (*sort) = new (m_join->thd->pq_context().mem_root)
          Filesort(m_join->thd, {m_tab->table()}, /*keep_buffers=*/false,
                   m_order, HA_POS_ERROR, /*remove_duplicates=*/false,
                   /*force_sort_rowids=*/false, /*unwrap_rollup=*/false);
      if (!(*sort) || DBUG_EVALUATE_IF("pq_msort_error3", true, false))
        return true;
    }
  }
  return false;
}

/**
 * init the mq_record_gather
 */
bool ParallelScanIterator::pq_init_record_gather() {
  THD *thd = m_join->thd;
  Filesort *sort = NULL;
  if (pq_make_filesort(&sort)) return true;
  /* If m_record_gather has been new, release it first. */
  if (m_record_gather) {
    m_record_gather->mq_scan_end();
  }
  m_record_gather = new (thd->pq_context().mem_root)
      MQ_record_gather(thd, m_tab, &m_join->tmp_fields[REF_SLICE_PQ_TMP]);

  // optimize merge sort procedure for table/index scan
  bool index_sort = m_stable_sort || (m_join->m_ordered_index_usage ==
                                      JOIN::ORDERED_INDEX_ORDER_BY);

  if (!m_record_gather ||
      m_record_gather->mq_scan_init(sort, m_dop, m_gather->m_desc_groups,
                                    m_stable_sort, index_sort) ||
      DBUG_EVALUATE_IF("pq_msort_error4", true, false))
    return true;

  /** set each worker's MQ_handle */
  for (uint i = 0; i < m_dop; i++) {
    m_gather->m_workers[i]->m_handle =
        m_record_gather->m_exchange->get_mq_handle(i);
  }
  return false;
}

/**
 * launch worker threads
 *
 * @retval: false if success, and otherwise true
 */
bool ParallelScanIterator::pq_launch_worker() {
  THD *thd = m_join->thd;
  assert(thd == current_thd);

  Gather_operator *gather = m_tab->gather;
  PQ_worker_manager **workers = gather->m_workers;
  int launch_workers = 0;

  // set debug sync for launching worker threads
  DEBUG_SYNC(thd, "pq_wait_launch_worker");

  auto handle_non_launched_workers = create_scope_guard([this, gather]() {
    if (gather->m_pq_hash_join_shared_context != nullptr) {
      for (uint i = gather->m_pq_hash_join_shared_context
                        ->NumberOfConstructedWorkers();
           i < m_dop; ++i) {
        // These are non-launched workers where the execution tree never was
        // constructed. For hash join, this means that neither the hash join
        // destructor nor HashJoinIterator::End() will be called for these
        // workers. For parallel-aware hash join this introduces a small
        // problem: the synchronization barriers were initialized with "m_dop"
        // participants. But these workers that never launched will not
        // participate in these barriers, causing the launced workers to wait
        // forever for these non-existing workers (the number of pariticpants is
        // reduced in End() and/or the destructor). Avoid this problem by
        // manually reducing the number of participants in the synchronization
        // barriers.
        gather->m_pq_hash_join_shared_context->ExecutionBarrier()
            .ArriveAndDrop();
        gather->m_pq_hash_join_shared_context->DestructionBarrier()
            .ArriveAndDrop();
      }
    }
  });

  if (gather->m_pq_hash_join_shared_context != nullptr) {
    // In case of a parallel-aware hash join, reset the barriers to its original
    // state. At the end of one hash join execution, each worker will call
    // "ArriveAndDrop" on the barriers, which will decrement the number of
    // participants in the barrier for the current run as well as any subsequent
    // runs as well. The reason for this is that some workers may finish its
    // work early (most likely due to data skew), while other worker may
    // continue with multiple build and probe phases. Without decrementing the
    // number of workers in the barrier, longer running workers would be stuck
    // waiting for these "early quitters". Now, if the parallel-aware hash join
    // is inside a dependent subquery, we are going to re-use the same barriers
    // multiple times. We thus need to bring the number of workers back up to
    // its original value.
    gather->m_pq_hash_join_shared_context->DestructionBarrier().Reset(
        gather->m_dop);
    gather->m_pq_hash_join_shared_context->ExecutionBarrier().Reset(
        gather->m_dop);
  }

  // when workers encounter error during execution,
  // directly abort the parallel execution
  for (uint i = 0; i < m_dop; i++) {
    assert(!workers[i]->thd_worker);
    workers[i]->m_status = PQ_worker_state::INIT;
#ifndef NDEBUG
    if (i >= m_dop / 2) DEBUG_SYNC(thd, "pq_wait_kill");
#endif  // NDEBUG
    if (thd->is_error() || thd->pq_context().has_error() || thd->killed) goto err;
    my_thread_handle id;
    id.thread = 0;

#ifndef NDEBUG
    // this value will be copied by the worker at runtime
    thd->pq_context().skip_fetch_ctx = false;
    if (i == 0 && DBUG_EVALUATE_IF("pq_skip_fetch_ctx", true, false)) {
      thd->pq_context().skip_fetch_ctx = true;
    } else if (i == 1) {
      // all workers except worker_0 can fetch scan ctx
      DEBUG_SYNC(thd, "pq_launch_worker_1");
    }
#endif  // NDEBUG
    /**
     * pq_worker_error8: all workers are failed to launch
     * pq_worker_error9: worker's id in [0, 2, 4, ..] are failed to launch
     */
    if (DBUG_EVALUATE_IF("pq_worker_error8", false, true) &&
        DBUG_EVALUATE_IF("pq_worker_error9", (i % 2), true)) {
      mysql_thread_create(key_thread_parallel_query, &id, NULL, pq_worker_exec,
                          (void *)workers[i]);
    }
    workers[i]->thread_id = id;
    int expected_status = PQ_worker_state::READY | PQ_worker_state::COMPELET |
                          PQ_worker_state::ERROR | PQ_worker_state::OVER;
    if (id.thread != 0) {
      /** Record the thread id so that we can later determine whether the thread
       * started */
      workers[i]->m_active = workers[i]->wait_for_status(thd, expected_status);
      /** partial workers may fail before execution */
      if (!workers[i]->m_active ||
          DBUG_EVALUATE_IF("pq_worker_error7", (i >= m_dop / 2), false)) {
        goto err;
      }
      launch_workers++;
    } else {
      sql_print_warning("worker %d has failed to start up\n", i);
      MQueue_handle *mq_handler = m_record_gather->m_exchange->get_mq_handle(i);
      if (mq_handler) mq_handler->set_detached_status(MQ_HAVE_DETACHED);
    }
  }
  /** if all workers are not launched, then directly return false */
  if (!launch_workers) goto err;
  return false;

err:
  for (uint i = 0; i < m_dop; i++) {
    if (workers[i]->thread_id.thread && workers[i]->thd_worker) {
      workers[i]->thd_worker->pq_context().set_error();
    }
  }
  return true;
}

/**
 * wait all workers finish their execution
 */
void ParallelScanIterator::pq_wait_workers_finished() {
  THD *leader_thd = m_join->thd;
  assert(leader_thd == current_thd);
  /**
   * leader first detached the message queue, and then wait workers finish
   * the execution. The reason for detach MQ is that leader has fetched the
   * satisfied #records (e.g., limit operation).
   */
  Exchange *exchange = nullptr;
  if (m_record_gather && (exchange = m_record_gather->m_exchange)) {
    MQueue_handle *m_handle = nullptr;
    for (uint i = 0; i < m_gather->m_dop; i++) {
      if ((m_handle = exchange->get_mq_handle(i))) {
        m_handle->set_detached_status(MQ_HAVE_DETACHED);
      }
    }
  }
  /**
   * wait all such workers to finish execution, two conditions must meet:
   *  c1: the worker thread has been created
   *  c2: the worker has not yet finished
   */
  int expected_status = PQ_worker_state::COMPELET | PQ_worker_state::ERROR;
  for (uint i = 0; i < m_gather->m_dop; i++) {
    if (m_gather->m_workers[i]->thread_id.thread != 0)  // c1
    {
      if (m_gather->m_workers[i]->m_active &&
          !(m_gather->m_workers[i]->m_status & PQ_worker_state::COMPELET)) {
        m_gather->m_workers[i]->wait_for_status(leader_thd, expected_status);
      }
      my_thread_join(&m_gather->m_workers[i]->thread_id, NULL);
    }
  }
}

int ParallelScanIterator::pq_error_code() {
  THD *thd = m_join->thd;

  if (m_gather->m_ha_err == HA_ERR_TABLE_DEF_CHANGED) {
    m_gather->m_ha_err = 0;
    return HA_ERR_TABLE_DEF_CHANGED;
  }

  if (thd->is_killed()) {
    thd->send_kill_message();
  }

  /** collect worker threads status from DA info */
  JOIN *template_join = m_gather->m_template_join;
  THD *temp_thd = template_join->thd;
  thd->pq_merge_status(temp_thd);
  // No push down select_distinct to woker because BUG2023080901645.
  // So the total number of rows in the statement's output is not
  // thd->pq_context().current_found_rows, but rather the number of rows after
  // deduplication by the leader. So reset disable_distinct_in_pq_worker
  // to 0 and in Query_expression::ExecuteIteratorQuery function, it will
  // return *send_records_ptr.
  if (m_join->query_block->pq_context().disable_distinct_in_pq_worker)
    thd->pq_context().current_found_rows = 0;
  // update PQ leader's examined_rows through cloned template_join in gather;
  // The whole query's examined_rows leader_thd->m_examined_rows will be
  // updated in ExecuteIteratorQuery() when the whole query is finished.
  m_join->examined_rows = template_join->examined_rows;

  Diagnostics_area *da = temp_thd->get_stmt_da();

  DBUG_EXECUTE_IF("parallelscaniter_error",
                  da->set_error_status(temp_thd, ER_QUERY_INTERRUPTED););

  if (temp_thd->is_error()) {
    thd->raise_condition(da->mysql_errno(), da->returned_sqlstate(),
                         Sql_condition::SL_ERROR, da->message_text());
  }

  if (da->cond_count() > 0) {
    Diagnostics_area::Sql_condition_iterator it = da->sql_conditions();
    const Sql_condition *cond;
    while ((cond = it++)) {
      thd->raise_condition(cond->mysql_errno(), NULL, cond->severity(),
                           cond->message_text());
    }
  }
  /**  output parallel error code */
  if (!temp_thd->is_error() && !thd->is_error() &&
      !temp_thd->pq_context().explain_analyze && thd->pq_context().has_error()) {
    my_error(ER_PARALLEL_QUERY_ERROR, MYF(0), "Parallel execution error");
  }
  return 1;
}

bool ParallelScanIterator::para_exec_init(AccessPath *path) {
  if (!path) return false;
  bool rc = false;
  WalkAccessPathsProxy(
      path, /*cross_query_blocks=*/false,
      [&rc](AccessPath *lambda_path, const JOIN *) {
        // lambda_path->iterator is not nullptr, as the caller just called
        // force_create_iterators() on subqueries
        assert(lambda_path->iterator);
        if (lambda_path->type == AccessPath::MATERIALIZE) {
          if (materialize_iter_init_shared_table(
                  current_thd && current_thd->lex->is_explain_analyze,
                  lambda_path->iterator->real_iterator()))
            return rc = true;
        }
        return false;
      },
      /*post_order_traversal=*/true);
  return rc;
}

/// Execute scalar uncorrelated subqueries. The evaluated result of subquery
/// is stored into leader and will be shared by worker thread in the outer
/// query's parallel execution.
///
/// @return false if executes successfully; otherwise return true.
bool ParallelScanIterator::exec_scalar_uncorrelated_subquery() {
  auto thd = current_thd;
  for (auto i : m_gather->m_uncorrelated_subqueries)
    /*
      Failure requires two condition: the subquery's execution failed, and it
      was recorded as an error. This second condition is necessary for
      "INSERT IGNORE ... SELECT (scalar subquery which returns more than one
      row)": when this statement is handled by PQ, the scalar subquery fails,
      but we don't want the insertion to fail. This is similar to the non-PQ
      path, where this happens:
      - Item_subselect::val_*() (A) calls Item_subselect::exec() (B)
      - (B) calls my_error() (but due to IGNORE this is not recorded as error
      but as warning), then it returns an error, but (A) returns no error
      - the callers of (A) then test THD::is_error() which is false, and
      continue happily.
    */
    if (i->exec(thd) && thd->is_error()) return true;
  return false;
}

bool ParallelScanIterator::Init() {
  assert(current_thd == m_join->thd);

  for (auto query_block : list_query_blocks_to_clone(m_join->query_block)) {
    AccessPath *path;
    if (query_block == m_join->query_block) {
      /*
        For the top query here which does PQ, the JOIN's root access path is a
        no-child path of type PARALLEL_SCAN, while the complete path
        (with all sub-paths for tables to read) has been saved in
        this->m_root_access_path (this is because PQ has rewritten the
        iterator tree). We need the latter, to search for Materialize
        iterators underneath.
      */
      path = m_root_access_path;
    } else {
      /*
        But for a correlated subquery, handled by a worker as "just a
        subquery", without any PQ specifics, the complete root access
        path is kept in the JOIN as usual.
      */
      path = query_block->join->m_root_access_path;
      /*
        A correlated subquery often creates its iterators during execution
        rather than during optimization (see force_create_iterators() in
        item_subselect.cc), we have to force their creation now so that
        para_exec_init) can find them.
      */
      if (query_block->master_query_expression()->force_create_iterators(
              m_join->thd))
        return true;
    }
    if (para_exec_init(path)) {  // materialize shared tmp table
      m_join->thd->pq_context().set_error();
      return true;
    }
  }

  if (exec_scalar_uncorrelated_subquery() || /** execute the inner uncorrelated
                    subquery */
      pq_init_record_gather() ||             /** init mq_record_gather */
      m_gather->init() ||                    /** cur innodb data */
      pq_create_innodb_snapshot(
          m_join->thd) ||   /** create read view for consistent read if
                   we have not generated read_view on divided (or cut) table*/
      pq_launch_worker() || /** launch worker threads */
      DBUG_EVALUATE_IF("pq_worker_error6", true, false)) {
    m_join->thd->pq_context().set_error();
    return true;
  }
  return false;
}

int ParallelScanIterator::Read() {
  /** kill query */
  if (m_join->thd->is_killed()) {
    m_join->thd->send_kill_message();
    return 1;
  }
  /** fetch message from MQ to table->record[0] */
  if (m_record_gather->mq_scan_next()) return 0;
  return -1;
}

int ParallelScanIterator::End() {
  /** wait all workers to finish their execution */
  pq_wait_workers_finished();
  /** output error code */
  return pq_error_code();
}

ParallelScanIterator::~ParallelScanIterator() {
  table()->file->ha_index_or_rnd_end();
  /** cleanup m_record_gather */
  if (m_record_gather) {
    m_record_gather->mq_scan_end();
  }
}

PQblockScanIterator::PQblockScanIterator(
    THD *thd, TABLE *table, double expected_rows, ha_rows *examined_rows,
    PQTabType tabType, Gather_operator *gather, QEP_TAB *tab, bool need_rowid,
    MQueue_handle *handler)
    : TableRowIterator(thd, table),
      m_record(table->record[0]),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows),
      m_gather(gather),
      m_tabType(tabType),
      m_need_rowid(need_rowid),
      m_handler(handler),
      m_tab(tab) {
  m_pq_ctx = gather->pq_tabs[tabType].m_pq_ctx;
  keyno = gather->pq_tabs[tabType].keyno;
}

bool PQblockScanIterator::Init() {
  if (!m_inited) {
    assert(table()->file->ha_get_record_buffer() == nullptr);
    // We init the handler-related variables for parallel scan by
    // "pq_worker_scan_init" in PQ-related Iterator::Init(). Correspondingly, we
    // reset (or free) these inited variables by "pq_worker_scan_end" in
    // PQ-related Iterator::End().
    if (table()->file->pq_worker_scan_init(
            m_gather->pq_tabs[m_tabType].keyno,
            m_gather->pq_tabs[m_tabType].m_pq_ctx)) {
      return true;
    }

    m_is_mvi_unique_filter_enabled = false;
    // Enable & reset unique record filter for multi-valued index
    if (m_tab && m_tab->table() && m_tab->table()->key_info &&
        keyno < m_tab->table()->s->keys &&
        m_tab->table()->key_info[keyno].flags & HA_MULTI_VALUED_KEY) {
      m_tab->table()->file->ha_extra(HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER);
      m_tab->table()->prepare_for_position();
      m_is_mvi_unique_filter_enabled = true;
    }

    // As the Iterator may be inited multiple times in nested-loop join (NLJ)
    // we should allocate the space for record buffer first time, and after
    // that we only reset this record buffer.
    if (set_record_buffer(m_tab->table(), m_expected_rows)) {
      return true;
    }

    m_inited = true;
  } else {
    // Re-init this buffer in NLJ, then we should clear the
    // buffer before reading rows.
    table()->m_record_buffer.reset();
  }

  m_seen_eof = false;
  return false;
}

int PQblockScanIterator::End() {
  assert(thd() && thd()->pq_context().leader);
  table()->file->pq_worker_scan_end();
  return -1;
}

PQblockScanIterator::~PQblockScanIterator() {
  if (table() && table()->key_info && table()->file &&
      keyno < table()->s->keys &&
      table()->key_info[keyno].flags & HA_MULTI_VALUED_KEY) {
    table()->file->ha_extra(HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER);
  }
}

int PQblockScanIterator::Read() {
  if (m_seen_eof) {
    return -1;
  }

  int tmp;
  assert(!table()->file->pq_ref_depend);
  while (!m_handler->is_detached() &&
         (tmp = table()->file->ha_pq_next(m_record, m_pq_ctx))) {
    /*
      ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is
      reading and another deleting without locks.
    */
    if (tmp == HA_ERR_RECORD_DELETED && !thd()->killed &&
        !m_handler->is_detached()) {
      continue;
    }

    /*
      Filtering duplicate records from multi-value index read may return
      HA_ERR_KEY_NOT_FOUND when rowid is same.
     */
    if (tmp == HA_ERR_KEY_NOT_FOUND && m_is_mvi_unique_filter_enabled) continue;

    const int ret = HandleError(tmp);
    if (ret == -1) {
      m_seen_eof = true;
    }
    return ret;
  }

  if (m_handler->is_detached()) {
    return -1;
  }

  if (m_examined_rows != nullptr) {
    ++*m_examined_rows;
  }
  // write row_id into file
  if (m_need_rowid) {
    assert(table()->record[0] == m_record);
    table()->file->position(m_record);
  }

  return 0;
}

bool PQRefIterator::Init() {
  m_seen_eof = false;
  m_first_record_since_init = true;

  if (!m_inited) {
    if (table()->file->pq_worker_scan_init(
            m_gather->pq_tabs[m_tabType].keyno,
            m_gather->pq_tabs[m_tabType].m_pq_ctx)) {
      return true;
    }

    m_is_mvi_unique_filter_enabled = false;
    // Enable & reset unique record filter for multi-valued index
    if (m_tab && m_tab->table() && m_tab->table()->key_info && m_ref->key > 0 &&
        m_ref->key < (int)m_tab->table()->s->keys &&
        m_tab->table()->key_info[m_ref->key].flags & HA_MULTI_VALUED_KEY) {
      m_tab->table()->file->ha_extra(HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER);
      m_tab->table()->prepare_for_position();
      m_is_mvi_unique_filter_enabled = true;
    }

    if (set_record_buffer(m_tab->table(), m_expected_rows)) {
      return true;
    }
    m_inited = true;
  } else {
    // Reset record buffer when iterator is init multi times
    table()->m_record_buffer.reset();
  }

  return false;
}

PQRefIterator::~PQRefIterator() {
  if (table() && table()->key_info && table()->file && m_ref->key > 0 &&
      m_ref->key < (int)table()->s->keys &&
      table()->key_info[m_ref->key].flags & HA_MULTI_VALUED_KEY) {
    table()->file->ha_extra(HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER);
  }
}

int PQRefIterator::End() {
  assert(thd() && thd()->pq_context().leader);
  table()->file->pq_worker_scan_end();
  return -1;
}

int PQRefIterator::Read() {  // Forward read.
  if (m_seen_eof) {
    return -1;
  }

  if (m_first_record_since_init) {
    m_first_record_since_init = false;
    /*
      a = b can never return true if a or b is NULL, so if we're asked
      to do such a lookup, we can say there won't be a match without even
      checking the index. This is “late NULLs filtering” (as opposed to
      “early NULLs filtering”, which propagates the IS NOT NULL constraint
      further back to the other table so we don't even get the request).
      See the internals manual for more details.
     */
    if (m_ref->impossible_null_ref()) {
      DBUG_PRINT("info", ("RefIterator null_rejected"));
      table()->set_no_row();
      return -1;
    }
    if (construct_lookup(thd(), table(), m_ref)) {
      table()->set_no_row();
      return -1;
    }

    pair<uchar *, key_part_map> key_buff_and_map = FindKeyBufferAndMap(m_ref);
    Key_ref key_ref{m_ref->key, key_buff_and_map.first, key_buff_and_map.second,
                    false};
    uint key_len =
        calculate_key_len(table(), key_ref.keyno, key_ref.keypart_map);
    table()->file->pq_ref_key.key = key_ref.key;
    table()->file->pq_ref_key.length = key_len;
    table()->file->pq_ref_depend = true;
    table()->file->pq_ref_build_ranges(m_gather->pq_tabs[m_tabType].m_pq_ctx,
                                       key_ref);
    // clear the record buffer
    table()->m_record_buffer.reset();
    int error = table()->file->ha_pq_next(
        table()->record[0], m_gather->pq_tabs[m_tabType].m_pq_ctx);

    if (error) {
      int res = HandleError(error);
      if (res == -1) {
        m_seen_eof = true;
      }
      return res;
    }
  } else {
    int error;
    // Fetch unique rows matching the Ref Key in case of multi-value index
    do {
      error = table()->file->ha_pq_next(table()->record[0],
                                        m_gather->pq_tabs[m_tabType].m_pq_ctx);
    } while (error == HA_ERR_KEY_NOT_FOUND && m_is_mvi_unique_filter_enabled);

    if (error) {
      int res = HandleError(error);
      if (res == -1) {
        m_seen_eof = true;
      }
      return res;
    }
  }
  if (m_examined_rows != nullptr) {
    ++*m_examined_rows;
  }
  return 0;
}

static inline pair<uchar *, key_part_map> FindKeyBufferAndMap(
    const Index_lookup *ref) {
  if (ref->keypart_hash != nullptr) {
    return make_pair(pointer_cast<uchar *>(ref->keypart_hash), key_part_map{1});
  } else {
    return make_pair(ref->key_buff, make_prev_keypart_map(ref->key_parts));
  }
}
