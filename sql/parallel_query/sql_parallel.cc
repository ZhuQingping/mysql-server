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

#include "sql/parallel_query/sql_parallel.h"
#include "include/my_alloc.h"
#include "include/my_dbug.h"
#include "include/my_sys.h"
#include "include/mysql/psi/mysql_thread.h"
#include "my_sqlcommand.h"
#include "mysqld_error.h"
#include "sql/auth/auth_acls.h"
#include "sql/debug_sync.h"
#include "sql/error_handler.h"
#include "sql/filesort.h"
#include "sql/handler.h"
#include "sql/item_sum.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/cost_model.h"
#include "sql/join_optimizer/explain_access_path.h"
#include "sql/log.h"
#include "sql/mysqld.h"
#include "sql/mysqld_thd_manager.h"  // Global_THD_manager
#include "sql/opt_trace.h"
#include "sql/parallel_query/exchange.h"
#include "sql/parallel_query/exchange_sort.h"
#include "sql/parallel_query/explain_pq_access_path.h"
#include "sql/parallel_query/msg_queue.h"
#include "sql/parallel_query/pq_clone.h"
#include "sql/parallel_query/pq_hash_join_shared_context.h"
#include "sql/parallel_query/pq_resource_stat.h"
#include "sql/parallel_query/query_result_mq.h"
#include "sql/partial_result_cache.h"
#include "sql/query_result.h"
#include "sql/range_optimizer/path_helpers.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/sql_base.h"
#include "sql/sql_executor.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_parse.h"
#include "sql/sql_plan_cache.h"
#include "sql/sql_tmp_table.h"
#include "sql/transaction.h"

Item *make_cond_for_index(Item *cond, TABLE *table, uint keyno,
                          bool other_tbls_ok);

Item *make_cond_remainder(Item *cond, bool exclude_index);

void thd_set_thread_stack(THD *thd, const char *stack_start);

bool replace_with_pq_table_access_path(THD *thd, AccessPath *path);

using PathCosts = std::deque<AccessPath>;
bool pq_rewrite_full_access_path_cost(THD *thd, JOIN *join, AccessPath *path,
                                      PathCosts *orig_costs = nullptr);

bool check_pq_running_threads(uint dop, ulong timeout_ms) {
  bool success = false;
  mysql_mutex_lock(&LOCK_pq_threads_running);
  if (parallel_threads_running + dop > parallel_max_threads) {
    if (timeout_ms > 0) {
      struct timespec start_ts;
      struct timespec end_ts;
      struct timespec abstime;
      ulong wait_timeout = timeout_ms;
      int wait_result;

    start:
      set_timespec(&start_ts, 0);
      /* Calcuate the waiting period. */
      abstime.tv_sec = start_ts.tv_sec + wait_timeout / TIME_THOUSAND;
      abstime.tv_nsec =
          start_ts.tv_nsec + (wait_timeout % TIME_THOUSAND) * TIME_MILLION;
      if (abstime.tv_nsec >= TIME_BILLION) {
        abstime.tv_sec++;
        abstime.tv_nsec -= TIME_BILLION;
      }
      wait_result = mysql_cond_timedwait(&COND_pq_threads_running,
                                         &LOCK_pq_threads_running, &abstime);
      if (parallel_threads_running + dop <= parallel_max_threads) {
        success = true;
      } else {
        success = false;
        if (!wait_result) {  // wait isn't timeout
          set_timespec(&end_ts, 0);
          ulong difftime = (end_ts.tv_sec - start_ts.tv_sec) * TIME_THOUSAND +
                           (end_ts.tv_nsec - start_ts.tv_nsec) / TIME_MILLION;
          wait_timeout -= difftime;
          goto start;
        }
      }
    }
  } else
    success = true;

  if (success) {
    parallel_threads_running += dop;
    current_thd->pq_threads_running += dop;
  }
  mysql_mutex_unlock(&LOCK_pq_threads_running);
  return success;
}

/**
 * Init record gather
 *
 * @retval: false if success, and otherwise true
 */
bool MQ_record_gather::mq_scan_init(Filesort *sort, int workers,
                                    const std::set<const ORDER *> &desc_groups,
                                    bool stab_output, bool index_sort) {
  if (sort) {
    m_exchange = new (m_thd->pq_mem_root)
        Exchange_sort(m_thd,
                      // this TABLE is used for exchanging data in the MQ,
                      // its handler is not yet open
                      m_tab->table(), m_recv_items, sort,
                      // this handler is open and is the divided table
                      m_tab->split_table()->file, workers,
                      // we may use row IDs of rows of the divided table
                      m_tab->split_table()->file->ref_length,
                      // groups using desc indexes
                      desc_groups, stab_output, index_sort);
  } else {
    m_exchange = new (m_thd->pq_mem_root)
        Exchange_nosort(m_thd, m_tab->table(), m_recv_items, workers,
                        m_tab->split_table()->file->ref_length, stab_output);
  }

  if (!m_exchange || m_exchange->init()) return true;

  return false;
}

/**
 * read table->record[0] from workers through message queue
 *
 * @retval: false if success, and otherwise true
 */

bool MQ_record_gather::mq_scan_next() {
  assert(m_exchange);
  return (m_exchange->read_mq_record());
}

void MQ_record_gather::mq_scan_end() {
  assert(m_exchange);
  m_exchange->cleanup();
}

PQ_worker_manager::PQ_worker_manager()
    : m_gather(nullptr),
      thd_leader(nullptr),
      thd_worker(nullptr),
      m_status(INIT),
      m_active(false) {
  mysql_mutex_init(0, &m_mutex, MY_MUTEX_INIT_FAST);
  mysql_cond_init(0, &m_cond);
}

PQ_worker_manager::~PQ_worker_manager() {
  mysql_mutex_destroy(&m_mutex);
  mysql_cond_destroy(&m_cond);
}

/*
 * PQ worker wait for an status
 *
 * @leader: PQ leader thread
 * @status:
 *
 * @retval:
 *    true if normally execute, and otherwise false (i.e., execution-error)
 */

bool PQ_worker_manager::wait_for_status(THD *leader MY_ATTRIBUTE((unused)),
                                        uint status) {
  assert(leader == current_thd);
  mysql_mutex_lock(&m_mutex);
  while (!(this->m_status & status)) {
    struct timespec abstime;
    Timeout_type wait_time = 5;
    set_timespec(&abstime, wait_time);
    mysql_cond_timedwait(&m_cond, &m_mutex, &abstime);
  }
  mysql_mutex_unlock(&m_mutex);
  return !(this->m_status & PQ_worker_state::ERROR);
}

void PQ_worker_manager::signal_status(THD *thd, PQ_worker_state status) {
  mysql_mutex_lock(&m_mutex);
  if (!(this->m_status & status)) {
    this->m_status = status;
    this->thd_worker = thd;
  }
  mysql_cond_signal(&m_cond);
  mysql_mutex_unlock(&m_mutex);
}

void PQ_worker_manager::set_kill_state(THD::killed_state state_to_set) {
  if (thread_id.thread != 0 && m_active &&
      (m_status & PQ_worker_state::READY) && thd_worker) {
    thd_worker->killed = state_to_set;
  }
}

Gather_operator::Gather_operator(uint dop, MEM_ROOT *root)
    : m_dop(dop),
      m_template_join(nullptr),
      m_workers(nullptr),
      m_ha_err(0),
      m_stmt_da(false),
      m_code_state(nullptr),
      m_uncorrelated_subqueries(root) {
  memset(&pq_tabs, 0, sizeof(pq_tabs));
}

/*
 * replace the parameter item of Aggr. with the generated item in rewritten tab.
 *
 */
void pq_replace_avg_func(THD *thd, Query_block *select MY_ATTRIBUTE((unused)),
                         mem_root_deque<Item *> *fields,
                         nesting_map select_nest_level MY_ATTRIBUTE((unused))) {
  Item *item = nullptr;
  for (size_t i = 0; i < fields->size(); i++) {
    item = (*fields)[i];
    if (item->real_item()->type() == Item::SUM_FUNC_ITEM) {
      Item_sum *item_old = (Item_sum *)(item->real_item());
      assert(item_old);
      if (item_old->sum_func() == Item_sum::AVG_FUNC) {
        Item_sum_avg *item_avg = dynamic_cast<Item_sum_avg *>(item_old);
        assert(item_avg);
        item_avg->pq_avg_type = PQ_LEADER;
        item_avg->init_store_info();
        item_avg->resolve_type(thd);
      }
    } else if (item->real_item()->type() == Item::FIELD_AVG_ITEM) {
      // TODO is there a chance that Field_avg_item is from a avg(distinct)?
      // we do not enlarge worker's avg pack_length
      Item_avg_field *item_avg_field =
          dynamic_cast<Item_avg_field *>(item->real_item());
      Item_sum_avg *item_avg = item_avg_field->avg_item;
      if (item_avg->sum_func() == Item_sum::AVG_FUNC) {
        item_avg->pq_avg_type = PQ_LEADER;
        item_avg_field->pq_avg_type = PQ_LEADER;
        item_avg->init_store_info();
        item_avg->resolve_type(thd);
        fields->operator[](i) = item_avg;
      }
    } else if (item->real_item()->type() == Item::FIELD_ITEM) {
      Item_sum *item_sum =
          down_cast<Item_field *>(item->real_item())->field->item_sum_ref;
      if (item_sum && item_sum->sum_func() == Item_sum::AVG_FUNC) {
        Item_sum_avg *item_avg = down_cast<Item_sum_avg *>(item_sum);
        item_avg->pq_avg_type = PQ_LEADER;
        item_avg->init_store_info();
        item_avg->resolve_type(thd);
        fields->operator[](i) = item_avg;
      }
    }
  }
}

std::string get_count_distinct_item_name(THD *thd, Item_sum *sum) {
  uint num_args = sum->argument_count();
  std::string item_name;
  for (uint i = 0; i < num_args; ++i) {
    if (i) item_name += ",";
    Item *item = sum->get_arg(i);
    if (item->type() == Item::INT_ITEM) {
      item_name += std::to_string(item->val_int());
    } else if (item->type() == Item::FUNC_ITEM) {
      String str;
      item->print(thd, &str,
                  enum_query_type(QT_NO_TABLE | QT_NO_DEFAULT_DB |
                                  QT_SUBSELECT_AS_ONLY_SELECT_NUMBER));
      item_name = str.c_ptr_safe();
      item_name.erase(std::remove(item_name.begin(), item_name.end(), '`'),
                      item_name.end());
    } else {
      item_name += item->item_name.ptr();
    }
  }
  return item_name;
}

bool restore_item_name(THD *thd, Item_sum *sum, Item_field *item_field) {
  Item *item = sum->get_arg(0);
  if (sum->sum_func() == Item_sum::COUNT_DISTINCT_FUNC) {
    std::string item_name = get_count_distinct_item_name(thd, sum);
    item_field->item_name.copy(item_name.c_str(), item_name.length());
  } else {
    if (item->type() == Item::INT_ITEM) {
      const std::string &item_name = std::to_string(item->val_int());
      item_field->item_name.copy(item_name.c_str(), item_name.length());
    } else {
      item_field->item_name.copy(item->item_name);
    }
  }

  item_field->field_name = item_field->item_name.ptr();
  item_field->table_name = "<temporary>";

  return false;
}

/**
 * build sum funcs based on PQ leader temp table field when orig JOIN old
 * fields_list contain sum funcs. because origin sum item has been replaced by
 * Item_field in temp table fields list
 *
 * @fields_orig：orig fields list which could contain sum funs item
 * @fields_new: PQ leader temp table's fields list
 *
 */
bool pq_build_sum_funcs(THD *thd, Query_block *select, Ref_item_array &ref_ptr,
                        mem_root_deque<Item *> *fields,
                        nesting_map select_nest_level) {
  size_t num_hidden_fields = CountHiddenFields(*fields);
  uint saved_allow_sum_funcs = thd->lex->allow_sum_func;
  thd->lex->allow_sum_func |= select_nest_level;

  Item *item = nullptr;
  for (size_t i = 0; i < fields->size(); i++) {
    item = fields->operator[](i);
    if (item->real_item()->type() == Item::FIELD_ITEM) {
      Item_field *item_field = dynamic_cast<Item_field *>(item);
      if (item_field->field->item_sum_ref == nullptr) continue;
      Item_sum *item_ref = item_field->field->item_sum_ref;

      if (item_ref->type() == Item::SUM_FUNC_ITEM) {
        restore_item_name(thd, item_ref, item_field);
        item_field->table_name = "<temporary>";
        thd->clone_phase = THD::PQ_EXECUTEION;
        // If count_distinct_func is in having_condition, like
        // 'select * from t1 group by id having count(distinct id) > 0;'
        // Item_sum_count(count(distinct id))->hidden is true and
        // in change_to_use_tmp_fields function, it will create Item_field from
        // count(distinct id) and Item_field::hidden = item_sum_count::hidden =
        // true. It will lead to mysqld crash in Aggregator_distinct::setup at
        // assert(!item->hidden);
        if (item_ref->sum_func() == Item_sum::COUNT_DISTINCT_FUNC) {
          item_field->hidden = false;
        }

        Item_sum *sum_func =
            item_ref->pq_rebuild_sum_func(thd, select, item_field);
        if (!sum_func) {
          thd->lex->allow_sum_func = saved_allow_sum_funcs;
          return true;
        }
        sum_func->orig_func = item_ref;
        if (sum_func->refix_fields(thd, nullptr)) return true;
        sum_func->hidden = item_ref->hidden;
        fields->operator[](i) = sum_func;
        ref_ptr[item_ref->hidden ? fields->size() - i - 1
                                 : i - num_hidden_fields] = sum_func;
      }
    }
  }
  thd->lex->allow_sum_func = saved_allow_sum_funcs;
  return false;
}

THD *pq_new_thd(THD *thd) {
  DBUG_TRACE;

  THD *new_thd = new (thd->pq_mem_root) THD();
  if (!new_thd ||
      DBUG_EVALUATE_IF("dup_thd_abort", (!(new_thd->net.error = 0)), false)) {
    goto err;
  }

  new_thd->set_new_thread_id();
  thd_set_thread_stack(new_thd, (char *)&new_thd);
  new_thd->init_cost_model();
  new_thd->store_globals();
  new_thd->want_privilege = 0;
  new_thd->net.error = 0;
  new_thd->set_db(thd->db());
  new_thd->pq_copy_from(thd);

  if (thd->pq_explain_analyze ||
      (thd->lex->is_explain() && thd->lex->explain_format->is_iterator_based()))
    new_thd->pq_explain_analyze = true;

  return new_thd;

err:
  if (new_thd) {
    end_connection(new_thd);
    close_connection(new_thd, 0, false, false);
    new_thd->release_resources();
    new_thd->get_stmt_da()->reset_diagnostics_area();
    destroy(new_thd);
  }
  return nullptr;
}

/**
  Reset the query_blocks_to_materialize field of a derived query expression
  during EXPLAIN for MATERIALIZE access paths.

  When creating a MATERIALIZE AccessPath, the derived query expression's
  m_query_blocks_to_materialize is std::move'd into
  materialize().param->query_blocks. This function restores or clears
  that field depending on the given status:

  - MatStatus::UNFINISHED:
    Restore m_query_blocks_to_materialize to its pre-move state by copying
    back query_blocks from materialize().param->query_blocks.

  - MatStatus::FINISHED:
    Reset m_query_blocks_to_materialize to an empty array, reflecting the
    post-move state.

  This is needed to ensure that EXPLAIN output for derived subqueries
  correctly reflects the MATERIALIZE operator.

  @param thd   Thread context
  @param orig  The original Query_block containing derived tables
  @param status Whether to restore (UNFINISHED) or clear (FINISHED) the
                query_blocks_to_materialize field
*/
enum class MatStatus { FINISHED, UNFINISHED };
static void reset_derived_materialize_query_blocks(THD *thd, Query_block *orig,
                                                   MatStatus status) {
  if (!thd->lex->is_explain() || !orig->join->m_root_access_path) return;

  for (Table_ref *tbl_list = orig->leaf_tables; tbl_list != nullptr;
       tbl_list = tbl_list->next_leaf) {
    if (!tbl_list->is_view_or_derived()) continue;
    Query_expression *derived = tbl_list->derived_query_expression();

    Mem_root_array<MaterializePathParameters::QueryBlock> query_blocks(
        thd->pq_mem_root);
    bool is_find = false;
    WalkAccessPathsProxy(
        orig->join->m_root_access_path, /*cross_query_blocks=*/false,
        [derived, &query_blocks, &is_find, status](AccessPath *p,
                                                   const JOIN *) {
          if (p->type == AccessPath::MATERIALIZE &&
              p->materialize().param->unit == derived &&
              p->materialize().param->query_blocks.size() > 1) {
            if (status == MatStatus::UNFINISHED) {
              for (size_t i = 0;
                   i < p->materialize().param->query_blocks.size(); ++i) {
                const MaterializePathParameters::QueryBlock &from =
                    p->materialize().param->query_blocks[i];
                query_blocks.push_back(from);
              }
            }
            is_find = true;
            return true;
          }
          return false;
        },
        /*post_order_traversal=*/true);
    if (!is_find) continue;
    if (status == MatStatus::UNFINISHED) {
      assert(!derived->unfinished_materialization());
      derived->set_query_blocks_to_materialize(query_blocks);
    } else {
      Mem_root_array<MaterializePathParameters::QueryBlock> temp(thd->mem_root);
      derived->set_query_blocks_to_materialize(temp);
    }
  }
}

/**
 * make a parallel query gather operator from a serial query plan
 *
 * @join : is a pysics serial query plan
 * @dop is : the degree of parallel
 *
 */
Gather_operator *make_pq_gather_operator(JOIN *join, uint dop) {
  THD *thd = current_thd;
  assert(thd == join->thd && thd->has_pq && (join->idx_div_tab >= 0));
  JOIN *template_join = nullptr;
  Gather_operator *gather_opr = nullptr;
  std::vector<Query_block *> query_blocks_to_clone;

  // duplicate a query plan template from join, which is used in PQ workers.
  // Whereas the leader will use the original query block (Query_block).
  THD *new_thd = pq_new_thd(join->thd);
  if (!new_thd) goto err;
  new_thd->pq_leader = thd;
  new_thd->has_pq = true;
  template_join = pq_make_join(new_thd, join);

  if (!template_join || pq_dup_tabs(template_join, join, true)) {
    goto err;
  }

  template_join->need_tmp_pq = true;

  query_blocks_to_clone =
      list_query_blocks_to_clone(template_join->query_block);
  for (auto query_block : query_blocks_to_clone) {
    auto join2 = query_block->join;
    if (!join2) goto err;
    auto orig_join2 = query_block->pq_is_clone_of()->join;
    Switch_for_resolution_of_query_block switch_resol(
        new_thd->lex, query_block, query_block != template_join->query_block);
    if (join2 && (join2->setup_tmp_table_info(orig_join2) ||
                  (join2->qep_tab && join2->make_tmp_tables_info()) ||
                  DBUG_EVALUATE_IF("pq_gather_error1", true, false))) {
      sql_print_warning("[Parallel query] Setup gather tmp tables failed");
      goto err;
    }
  }

  /** duplicate a new THD and set it as current_thd, so here should restore old
   * THD */
  thd->store_globals();
  gather_opr = new (thd->pq_mem_root) Gather_operator(dop, thd->pq_mem_root);

  if (!gather_opr || DBUG_EVALUATE_IF("pq_gather_error2", true, false)) {
    goto err;
  }

  // Later in execution, the leader will need to evaluate scalar uncorrelated
  // subqueries. Here we collect these in a list, by walking the query block.
  // The upcoming alterations of the query block in make_leader_tmp_table()
  // (like, switch of ref slice) make it difficult to find items, which is why
  // we have to do the collection now.
  WalkQueryBlock(join->query_block, [&gather_opr](Item *item) {
    if (item->type() == Item::SUBSELECT_ITEM) {
      auto subq_item = down_cast<Item_subselect *>(item);
      if ((subq_item->substype() == Item_subselect::SINGLEROW_SUBS ||
           subq_item->substype() == Item_subselect::EXISTS_SUBS) &&
          !subq_item->is_uncacheable())
        gather_opr->m_uncorrelated_subqueries.push_back(subq_item);
    }
    return false;
  });

  gather_opr->m_template_join = template_join;
  if (join->idx_cut_tab >= 0) gather_opr->tab_set.insert(CUT_TAB);

  for (auto tabType : gather_opr->tab_set) {
    auto idx = (tabType == DIV_TAB) ? join->idx_div_tab : join->idx_cut_tab;
    gather_opr->pq_tabs[tabType].m_tab = &join->qep_tab[idx];
    gather_opr->pq_tabs[tabType].m_table = join->qep_tab[idx].table();
  }

#ifndef NDEBUG
  gather_opr->m_code_state = my_thread_var_dbug();
  assert(gather_opr->m_code_state && *(gather_opr->m_code_state));
#endif  // NDEBUG
  template_join->thd->push_diagnostics_area(&gather_opr->m_stmt_da);

  gather_opr->m_workers =
      thd->pq_mem_root->ArrayAlloc<PQ_worker_manager *>(dop);

  if (!gather_opr->m_workers ||
      DBUG_EVALUATE_IF("pq_gather_error3", (!(gather_opr->m_workers = nullptr)),
                       false)) {
    goto err;
  }

  for (uint i = 0; i < dop; i++) {
    gather_opr->m_workers[i] = new (thd->pq_mem_root) PQ_worker_manager();
    if (!gather_opr->m_workers[i]) goto err;
    gather_opr->m_workers[i]->m_gather = gather_opr;
    gather_opr->m_workers[i]->thd_leader = thd;
    gather_opr->m_workers[i]->thd_worker = nullptr;
    gather_opr->m_workers[i]->thread_id.thread = 0;
    gather_opr->m_workers[i]->worker_index = i;
  }

  gather_opr->prepare();

#ifndef NDEBUG
  if (DBUG_EVALUATE_IF("pq_gather_error4", true, false)) goto err;
#endif

  // Before generating the parallel execution plan, restore
  // derived->m_query_blocks_to_materialize to avoid losing the ability to
  // print the subquery execution plan during EXPLAIN.
  reset_derived_materialize_query_blocks(thd, join->query_block,
                                         MatStatus::UNFINISHED);
  // This marks each query block as "optimized", and gives it a root
  // iterator, all things needed to execute properly later.
  for (auto query_block : query_blocks_to_clone) {
    auto join2 = query_block->join;
    if (!join2 ||
        pq_make_join_readinfo(
            join2, (join2 == template_join) ? gather_opr : nullptr, false)) {
      goto err;
    }
    query_block->pq_unlink_clone();
  }
  return gather_opr;

err:
  if (new_thd) new_thd->store_globals();
  pq_free_join(new_thd, template_join);
  pq_free_thd(new_thd);
  if (gather_opr && gather_opr->m_workers) {
    for (uint i = 0; i < dop; i++) {
      destroy(gather_opr->m_workers[i]);
    }
  }
  destroy(gather_opr);
  thd->store_globals();
  return nullptr;
}

void SetupPQTab(PQTab *pqTab, QEP_TAB *tab, JOIN *join) {
  TABLE *table = tab->table();
  table->file->ha_set_reverse_scan(tab->m_reversed_access);
  table->file->pq_ref = false;
  join_type type = tab->type();
  switch (type) {
    case JT_ALL:
      pqTab->keyno = (table->file->ht->db_type == DB_TYPE_INNODB)
                         ?
                         // In InnoDB, table scan is index scan on primary key
                         table->s->primary_key
                         : MAX_KEY;
      /*
       * Note that: order/group-by may be optimized in test_skip_sort(), and
       * correspondingly the order/group-by is finished with the generated
       * tab->quick().
       */
      if (tab->range_scan() &&
          join->m_ordered_index_usage != JOIN::ORDERED_INDEX_VOID) {
        pqTab->keyno = used_index(tab->range_scan());
      }
      pqTab->table_scan = true;
      break;
    case JT_RANGE:
      assert(tab->range_scan() &&
             tab->range_scan()->type == AccessPath::INDEX_RANGE_SCAN);
      pqTab->keyno = used_index(tab->range_scan());
      break;
    case JT_REF:
      table->file->pq_ref_key.key = tab->ref().key_buff;
      table->file->pq_ref_key.keypart_map =
          make_prev_keypart_map(tab->ref().key_parts);
      table->file->pq_ref_key.length = tab->ref().key_length;
      table->file->pq_ref_key.flag = HA_READ_KEY_OR_NEXT;
      table->file->pq_ref = true;
      pqTab->keyno = tab->ref().key;
      break;
    case JT_INDEX_SCAN:
      pqTab->keyno = tab->index();
      break;
    default:
      assert(0);
      pqTab->keyno = table->s->primary_key;
  }
}

void Gather_operator::prepare() {
  for (auto tabType : tab_set) {
#ifndef NDEBUG
    auto idx = (tabType == DIV_TAB) ? m_template_join->idx_div_tab
                                    : m_template_join->idx_cut_tab;
    assert(idx >= 0);
    assert(current_thd == pq_tabs[tabType].m_table->in_use);
#endif  // NDEBUG

    SetupPQTab(&pq_tabs[tabType], pq_tabs[tabType].m_tab, m_template_join);
  }
}

int InitPQTab(PQTab *pqTab, uint dop) {
  QEP_TAB *tab = pqTab->m_tab;
  join_type type = tab->type();
  AccessPath *range_scan = tab->range_scan();
  TABLE *table = tab->table();
  if (range_scan && type == JT_RANGE) {
    table->file->pq_range_type =
        range_scan->type == AccessPath::INDEX_RANGE_SCAN ? PQ_RANGE_SELECT
                                                         : PQ_QUICK_SELECT_NONE;
    assert(range_scan->type == AccessPath::INDEX_RANGE_SCAN);

    // shared_reset is to set mrr_* for handler, which is necessary for in
    // ha_innobase::pq_range_scan_init.
    bool reverse = range_scan->index_range_scan().reverse;
    if (reverse) {
      if (down_cast<ReverseIndexRangeScanIterator *>(
              range_scan->iterator->real_iterator())
              ->shared_reset())
        return -1;
      table->file->ha_set_reverse_scan(true);
    } else {
      if (DBUG_EVALUATE_IF("pq_range_scan_reset", 1, 0) ||
          down_cast<IndexRangeScanIterator *>(
              range_scan->iterator->real_iterator())
              ->shared_reset())
        return -1;
    }
  } else {
    table->file->pq_range_type = PQ_QUICK_SELECT_NONE;
  }

  table->file->pq_table_scan = false;
  /** partition table into blocks for parallel scan by multiple workers */
  int error = table->file->ha_pq_init(dop, pqTab->keyno);
  pqTab->m_pq_ctx = table->file->pq_ctx;
  if (error) {
    table->file->print_error(error, MYF(0));
    return error;
  }

  return error;
}

bool Gather_operator::init() {
  for (auto tabType : tab_set) {
    if ((m_ha_err = InitPQTab(&pq_tabs[tabType], m_dop))) return true;
  }
  return false;
}

void pq_free_gather(Gather_operator *gather) {
  THD *thd_temp = gather->m_template_join->thd;
  if (thd_temp == nullptr) return;

  THD *saved_thd = current_thd;

  thd_set_thread_stack(thd_temp, (char *)thd_temp);
  thd_temp->store_globals();

  uint tables = gather->m_template_join->tables;
  for (uint i = 0; i < tables; i++) {
    if (gather->m_template_join->qep_tab[i].table()) {
      gather->m_template_join->qep_tab[i].table()->set_keyread(false);
      gather->m_template_join->qep_tab[i].set_keyread_optim();
    }
  }

  pq_free_join(thd_temp, gather->m_template_join);
  for (uint i = 0; i < gather->m_dop; i++) {
    destroy(gather->m_workers[i]);
  }
  destroy(gather);

  pq_free_thd(thd_temp);
  thd_set_thread_stack(saved_thd, (char *)&saved_thd);
  saved_thd->store_globals();
}

static std::vector<uint32_t> backup_leader_plan(THD *thd MY_ATTRIBUTE((unused)),
                                                bool graceful_fallback,
                                                JOIN *join) {
  if (!graceful_fallback) return {};
  join->query_block->pq_backup();

#ifndef NDEBUG
  return thd->mem_root->CalcMemDigest();
#endif  // NDEBUG
  return {};
}

static void restore_leader_plan(
    THD *thd MY_ATTRIBUTE((unused)), bool graceful_fallback,
    const std::vector<uint32_t> &orig_digests MY_ATTRIBUTE((unused)),
    JOIN *join) {
  join->pq_stable_sort = false;
  join->qep_tab = join->qep_tab0;
  join->ref_items = join->ref_items0;
  join->tmp_fields = join->tmp_fields0;
  if (!graceful_fallback) return;

  join->query_block->pq_restore();
  join->pq_restore();

#ifndef NDEBUG
  bool has_derived_table = false;
  for (Table_ref *table_ref = join->query_block->leaf_tables;
       table_ref != nullptr; table_ref = table_ref->next_leaf) {
    if (table_ref->is_view_or_derived()) {
      has_derived_table = true;
      break;
    }
  }

  // When falling back to the serial execution plan, it is necessary
  // to ensure that the data structures related to the serial execution
  // plan have not been modified.
  // The data structures related to the serial execution plan are stored
  // in thd->mem_root. In backup_leader_plan, the initial digest values
  // orig_digests are calculated. At this point, the digest values of
  // thd->mem_root are recalculated. By comparing the two sets of digest
  // values, it is confirmed that the memory content of thd->mem_root has
  // not been modified, which means that the serial execution plan has not
  // changed and falling back to the serial execution plan is reliable.
  // From backup_leader_plan to restore_leader_plan, thd->pq_mem_root is
  // used, and thd->mem_root is not used. This is a prerequisite for the
  // aforementioned verification mechanism.
  // A PQ clone of a derived table shares the original Query_expression while
  // building its temporary table. Its materialization state must be reset for
  // serial fallback, so its MEM_ROOT cannot be byte-identical to the backup.
  // Keep the strict check for plans that do not have this shared state.
  if (!has_derived_table) {
    std::vector<uint32_t> digests = thd->mem_root->CalcMemDigest();
    assert(digests == orig_digests);
  }
#endif  // NDEBUG
}

/**
 * make pq-execution plan for query unit
 *
 *@retval
 *   true if there occurs some error, and
 *   false for SEQ_EXEC or PARL_EXEC
 * */
PQ_exec_status make_pq_unit_plan(Query_expression *unit, THD *thd) {
  PQ_exec_status status = PQ_exec_status::SEQ_EXEC;
  JOIN *join = nullptr;
  // check that we are working on the first query block and
  // that we have an INSERT/REPLACE SELECT statement.

  const bool select_insert =
      thd->lex->query_block &&
      thd->lex->query_block == unit->first_query_block() &&
      (thd->lex->sql_command == SQLCOM_INSERT_SELECT ||
       thd->lex->sql_command == SQLCOM_REPLACE_SELECT);

  Name_resolution_context_state ctx_state;
  if (select_insert) {
    Table_ref *const table_list = thd->lex->query_tables;
    Table_ref *first_select_table = table_list->next_local;
    //  Save the state of the current name resolution context.
    ctx_state.save_state(&thd->lex->query_block->context, table_list);
    // Remove the insert table from the first query block : calls to fix_fields
    // later on will need to match tables from the SELECT clause. This code is
    // inspired from similar code in sql_insert.cc .
    thd->lex->query_block->m_table_list.first =
        thd->lex->query_block->context.table_list =
            thd->lex->query_block->context.first_name_resolution_table =
                first_select_table;
  }

  if (unit->is_simple()) {
    join = unit->first_query_block()->join;
    if (join && join->suite_for_parallel_query()) {
      status = make_pq_leader_plan(join, thd);
      if (status == PQ_exec_status::ABORT_EXEC) {
        return status;
      }
    }
  } else if (unit->is_union_all_of_simple()) {
    // For now, allow only union all of simple query blocks
    for (Query_block *sl = unit->first_query_block(); sl;
         sl = sl->next_query_block()) {
      join = sl->join;
      if (join && join->suite_for_parallel_query()) {
        unit->set_limit(thd, sl);  // restore join's limit
        status = make_pq_leader_plan(join, thd);
        if (status == PQ_exec_status::ABORT_EXEC) {
          return status;
        }
      }
    }
  }
  if (select_insert) {
    Table_ref *const table_list = thd->lex->query_tables;
    Table_ref *first_select_table = table_list->next_local;
    // Restore the insert table and the name resolution context
    thd->lex->query_block->m_table_list.first =
        thd->lex->query_block->context.table_list = table_list;
    table_list->next_local = first_select_table;
    ctx_state.restore_state(&thd->lex->query_block->context, table_list);
  }
  assert(status == PQ_exec_status::SEQ_EXEC ||
         status == PQ_exec_status::PARL_EXEC);
  if (unit->create_access_paths(thd)) {
    assert(thd->is_error());
    if (thd->get_stmt_da()->error_condition()->mysql_errno() !=
        ER_PARALLEL_FAIL_INIT) {
      my_error(ER_PARALLEL_FAIL_INIT, MYF(0));
    }
    status = PQ_exec_status::ABORT_EXEC;
  }

  DEBUG_SYNC(thd, "after_pq_leader_plan");
  return status;
}

/**
 * Mark group direction for streaming aggregation.
 * When doing streaming aggregation, leader will use a min heap to
 * do the merge sort, it will find the minimal row each time.
 * If workers are using reverse sorted index, they will send rows
 * from biggest to smallest to leader, but leader still sort rows in asc
 * order, in this case, leader will not do merge sort correctly.
 * @param join: original join, before rewrite by leader.
 * @param gather: object to store all parallel query params.
 */
void mark_desc_groups(JOIN *join, Gather_operator *gather) {
  // If leader doesn't need to merge groups, That means leader
  // will add a tmp table to do the deduplicate, this function is
  // not needed.
  if (!join->pq_rebuilt_group) {
    return;
  }

  auto mark_desc_group = [join, gather](const ORDER *group) {
    // If a group's direction is set, leader will use this direction
    // for merge sort, which direction is the same with workers, and
    // no need to find the index direction again.
    if (group->direction != ORDER_NOT_RELEVANT) {
      return;
    }

    const Item *item = (*group->item)->real_item();
    if (item->type() != Item::FIELD_ITEM) {
      return;
    }

    // For case: 'group by a order by a, b', idx_a(a) is a DESC index. field 'a'
    // shouldn't be set as 'reverse' on PQ message queue, because workers'
    // output order is following 'order by a, b'.
    ORDER *order = join->order.order;
    bool skip = false;
    while (order) {
      if ((*order->item)->eq_with_binary_cmp_arg(item, false)) {
        skip = true;
        break;
      }
      order = order->next;
    }
    if (skip) return;

    const Field *field = down_cast<const Item_field *>(item)->field;
    const TABLE *table = field->table;
    const QEP_TAB *tab = table->reginfo.qep_tab;
    assert(tab);

    uint idx = tab->effective_index();
    // If no index is used for this column, GROUP BY may still be stream-based
    // for other columns, if this one is constant.
    if (idx == MAX_KEY) {
      return;
    }
    KEY *keyinfo = table->key_info + idx;
    if (table->key_info == nullptr || keyinfo == nullptr) {
      return;
    }

    KEY_PART_INFO *key_part = keyinfo->key_part;
    KEY_PART_INFO *key_part_end = key_part + keyinfo->user_defined_key_parts;
    key_part_map const_key_parts = table->const_key_parts[idx];
    /*
      Skip key parts that are constants in the WHERE clause.
      This part is copied from optimizer's code.
      @see test_if_order_by_key
    */
    for (; const_key_parts & 1 && key_part < key_part_end;
         const_key_parts >>= 1) {
      key_part++;
    }

    for (; key_part < key_part_end; key_part++) {
      if (!(key_part->key_part_flag & HA_REVERSE_SORT)) {
        continue;
      }

      if (field->eq(key_part->field)) {
        gather->m_desc_groups.insert(group);
      }
    }
  };

  // This function fills gather->m_desc_groups with groups with descending
  // index: In bool ParallelScanIterator::pq_init_record_gather(), Filesort
  // *sort is made using join->saved_join_group_list, then mq_scan_init() takes
  // in sort and  m_desc_groups, and compares the two, it therefore makes sense
  // to use join->saved_join_group_list here
  if (join->saved_join_group_list) {
    for (auto group : *join->saved_join_group_list) mark_desc_group(group);
  } else {
    for (auto &group : join->query_block->group_list) mark_desc_group(&group);
  }
}

static double get_full_access_path_cost(AccessPath *path) {
  double cost = -1.0;
  WalkAccessPathsProxy(
      path, /*cross_query_blocks=*/false,
      [&cost](AccessPath *p, const JOIN *) {
        if (p->cost > 0.0) {
          cost = p->cost;
          return true;
        } else if (p->type >= AccessPath::NESTED_LOOP_JOIN &&
                   p->type <= AccessPath::HASH_JOIN) {
          return true;
        }
        return false;
      },
      /*post_order_traversal=*/false);
  return cost;
}

/*
  Some operators in the execution plan perform duplicate removal, but
  the optimizer does not modify the row count in the results. To avoid
  affecting the cost calculation for whether to execute PQ, decisions
  are uniformly not made based on the cost.
*/
static bool is_untrust_full_access_path_row_nums(AccessPath *path) {
  bool ret = false;
  WalkAccessPathsProxy(
      path, /*cross_query_blocks=*/false,
      [&ret](AccessPath *p, const JOIN *) {
        if ((p->type == AccessPath::MATERIALIZE &&
             p->num_output_rows() >= 0.0 && p->materialize().param &&
             p->materialize().param->table &&
             MaterializeIsDoingDeduplication(p->materialize().param->table)) ||
            (p->type ==
             AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL) ||
            (p->type == AccessPath::WEEDOUT) ||
            (p->type == AccessPath::REMOVE_DUPLICATES)) {
          ret = true;
          return true;
        } else {
          return false;
        }
      },
      /*post_order_traversal=*/false);
  return ret;
}

static double get_full_access_path_row_nums(AccessPath *path) {
  double num_output_rows = 1.0;
  WalkAccessPathsProxy(
      path, /*cross_query_blocks=*/false,
      [&num_output_rows](AccessPath *p, const JOIN *) {
        double path_rows = p->num_output_rows();
        if (path_rows >= 0.0) {
          if (p->type == AccessPath::MATERIALIZE &&
              p->materialize().table_path != nullptr) {
            num_output_rows = p->materialize().table_path->num_output_rows();
          } else {
            num_output_rows = path_rows;
          }
          assert(num_output_rows >= 0.0);
          return true;
        } else if (p->type == AccessPath::SORT ||
                   p->type == AccessPath::STREAM) {
          return false;  // SORT/STREAM does not change the record count,
                         // continue to traverse downwards
        } else {  // can not obtain the exact record count, treat it as 1.0
          return true;
        }
      },
      /*post_order_traversal=*/false);
  return num_output_rows;
}

static void rewrite_nestloop_join_access_path_cost(AccessPath *path, THD *thd,
                                                   table_map div_table_map) {
  if (path->num_output_rows() < 0) return;
  const POSITION *pos_inner = nullptr;
  if (path->type == AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL) {
    pos_inner = path->nested_loop_semijoin_with_duplicate_removal().pos_inner;
  } else if (path->type == AccessPath::NESTED_LOOP_JOIN) {
    pos_inner = path->nested_loop_join().pos_inner;
  } else {
    assert(0);
  }
  SetCostOnNestedLoopAccessPath(*thd->cost_model(), pos_inner, path,
                                div_table_map);
}

static void rewrite_hash_join_access_path_cost(THD *thd, JOIN *join,
                                               table_map div_table_map,
                                               AccessPath *path) {
  assert(path->type == AccessPath::HASH_JOIN);
  if (path->num_output_rows() < 0) return;
  AccessPath *outer = path->hash_join().outer;  // probe table;
  AccessPath *inner = path->hash_join().inner;  // build table

  if (outer->num_output_rows() == -1.0 || inner->num_output_rows() == -1.0) {
    return;
  }
  double joined_rows = outer->num_output_rows() * inner->num_output_rows();
  /*
    The hash table is shared among the workers. Since the child operator
    divides the num_output_rows of the cut table by pq_dop, it is necessary
    to restore the original table size.
  */
  if (IsPartialResultsInputHashJoin(join, path)) {
    joined_rows *= thd->pq_dop;
  }
  AdjustOutLefJoinRows(path, inner->num_output_rows(), joined_rows);

  POSITION *pos_outer = path->hash_join().outer_qep_tab->position();
  /*
    HASH-JOIN estimate total cost of reading probe table:
      const double buffer_count =
          1.0 + ((double)cache_record_length(join, idx) * prefix_rowcount /
                 (double)thd->variables.join_buff_size);
      scan_and_filter_cost =
          buffer_count *
          (single_scan_read_cost + cost_model->row_evaluate_cost(
                                       tab->records() - *rows_after_filtering));
    prefix_rowcount corresponds to the build table row count.
    single_scan_read_cost corresponds to the probe table scan cost. Therefore,
    if there is a div table in the inner path or outer path, we need to divide
    pos_inner->read_cost by pq_dop.

    The above division is an ideal result value; in reality, the PQ runtime
    depends on the longest-running worker thread. Considering the preference for
    PQ, we simplify the calculation.
  */
  double outer_read_cost = pos_outer->read_cost;
  if ((GetUsedTableMap(inner, true) & div_table_map) ||
      (GetUsedTableMap(outer, true) & div_table_map)) {
    outer_read_cost /= thd->pq_dop;
  }
  path->set_num_output_rows(joined_rows * pos_outer->filter_effect);
  path->cost = inner->cost + outer_read_cost +
               thd->cost_model()->row_evaluate_cost(joined_rows);
}

static void rewrite_access_path_cost_by_copy(AccessPath *path) {
  AccessPath *copy_path = nullptr;
  if (path->type == AccessPath::WEEDOUT) {
    copy_path = path->weedout().child;
  } else if (path->type == AccessPath::CACHE_INVALIDATOR) {
    copy_path = path->cache_invalidator().child;
  } else {
    assert(0);
  }
  if (copy_path != nullptr) {
    CopyBasicProperties(*copy_path, path);
  }
}

static void rewrite_filter_access_path_cost(THD *thd, AccessPath *path) {
  assert(path->type == AccessPath::FILTER);
  QEP_TAB *qep_tab = path->filter().qep_tab;
  if (qep_tab != nullptr) {
    POSITION *pos = qep_tab->position();
    if (pos != nullptr)
      SetCostOnTableAccessPath(*thd->cost_model(), pos,
                               /*is_after_filter=*/true, path, true);
  } else if (path->filter().child != nullptr) {
    CopyBasicProperties(*path->filter().child, path);
  }
}

/**
 * Rewrite the cost and output row values in the parallel query
 * physical execution plan.
 *
 * @thd : THD
 * @join : origin serial query plan
 * @path : worker's physical query plan
 * @orig_costs : Used to back up the serial execution plan, so that after
 * completing the parallel execution plan transformation and parallel execution
 * cost estimation, it can be restored to the original serial execution plan
 */
bool pq_rewrite_full_access_path_cost(THD *thd, JOIN *join, AccessPath *path,
                                      PathCosts *orig_costs) {
  table_map div_table_map = 0;
  for (uint i = join->const_tables; i < join->primary_tables; ++i) {
    if (join->qep_tab[i].pq_div_tab) {
      div_table_map = join->qep_tab[i].table_ref->map();
      break;
    }
  }

  int ret = false;
  std::vector<AccessPath *> pq_materialize_paths;
  WalkAccessPathsProxy(
      path, /*cross_query_blocks=*/false,
      [thd, join, div_table_map, orig_costs, &ret, &pq_materialize_paths](
          AccessPath *p, const JOIN *) {
        if (orig_costs) {
          // Backup the original sequential execution plan
          orig_costs->push_back(*p);
          // Replace the AccessPath that scans the partitioned table with the
          // PQ_BLOCK_SCAN/PQ_REF_SCAN type.
          if (replace_with_pq_table_access_path(thd, p)) {
            ret = true;
            return true;
          }
        }
        switch (p->type) {
          case AccessPath::PQ_BLOCK_SCAN:
            SetCostOnTableAccessPath(*thd->cost_model(),
                                     p->pq_block_scan().qep_tab->position(),
                                     /*is_after_filter=*/false, p, true);
            break;
          case AccessPath::PQ_REF_SCAN:
            SetCostOnTableAccessPath(*thd->cost_model(),
                                     p->pq_ref_scan().qep_tab->position(),
                                     /*is_after_filter=*/false, p, true);
            break;
          case AccessPath::FILTER:
            rewrite_filter_access_path_cost(thd, p);
            break;
          case AccessPath::NESTED_LOOP_JOIN:
          case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
            rewrite_nestloop_join_access_path_cost(p, thd, div_table_map);
            break;
          case AccessPath::HASH_JOIN:
            rewrite_hash_join_access_path_cost(thd, join, div_table_map, p);
            break;
          case AccessPath::LIMIT_OFFSET:
            EstimateLimitOffsetCost(p, true);
            break;
          case AccessPath::MATERIALIZE:
            EstimateMaterializeCost(thd, p);
            // Since the upper-level AccessPath does not use
            // table_path->num_output_rows() when referencing the row count of
            // the lower-level MATERIALIZE, this needs to be corrected. After
            // the upper operator finishes computing the cost and row count,
            // we should then traverse pq_materialize_paths to restore it.
            if (thd->pq_dop && p->materialize().table_path->type ==
                                   AccessPath::PQ_BLOCK_SCAN) {
              p->set_num_output_rows(p->num_output_rows() / thd->pq_dop);
              pq_materialize_paths.push_back(p);
            }
            break;
          case AccessPath::STREAM:
            EstimateStreamCost(p);
            break;
          case AccessPath::SORT:
            EstimateSortCost(p);
            break;
          case AccessPath::WEEDOUT:
          case AccessPath::CACHE_INVALIDATOR:
            rewrite_access_path_cost_by_copy(p);
            break;
          case AccessPath::AGGREGATE:
            p->set_num_output_rows(kUnknownRowCount);
            EstimateAggregateCost(p, join->query_block);
            break;
          default:
            break;
        }
        return false;
      },
      /*post_order_traversal=*/true);
  for (auto p : pq_materialize_paths) {
    p->set_num_output_rows(p->num_output_rows() * thd->pq_dop);
  }
  return ret;
}

void pq_resore_full_access_path_cost(AccessPath *path, PathCosts &orig_costs) {
  WalkAccessPathsProxy(
      path, /*cross_query_blocks=*/false,
      [&orig_costs](AccessPath *p, const JOIN *) {
        *p = orig_costs.front();
        orig_costs.pop_front();
        return false;
      },
      /*post_order_traversal=*/true);
}

/*
  Equivalent to the logic of generating PQblockScan/PQrefScan operators in
  the QEP_TAB::access_path function, any changes in the QEP_TAB::access_path
  logic require adaptation.
*/
bool replace_with_pq_table_access_path(THD *thd, AccessPath *path) {
  TABLE *base_table = nullptr;
  switch (path->type) {
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN:
    case AccessPath::REF:
    case AccessPath::REF_OR_NULL:
    case AccessPath::INDEX_RANGE_SCAN:
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
    case AccessPath::FOLLOW_TAIL:
      base_table = GetBasicTable(path);
      break;
    default:
      return false;
  }

  QEP_TAB *qep_tab = base_table->reginfo.qep_tab;
  if (!qep_tab || (!qep_tab->pq_div_tab && !qep_tab->pq_cut_tab)) return false;

  const TABLE *pushed_root = qep_tab->table()->file->member_of_pushed_join();
  const bool is_pushed_child = (pushed_root && pushed_root != qep_tab->table());
  auto type = qep_tab->type();
  if ((type == JT_REF && !is_pushed_child) || (type == JT_INDEX_SCAN) ||
      ((type == JT_ALL || type == JT_RANGE || type == JT_INDEX_MERGE) &&
       (!qep_tab->using_dynamic_range) &&
       (!qep_tab->table_ref ||
        !qep_tab->table_ref->is_recursive_reference()))) {
    bool non_constant_look_up = false;
    if (type == JT_REF) {
      for (uint part_no = 0; part_no < qep_tab->ref().key_parts; part_no++) {
        if (qep_tab->ref().key_copy[part_no]) {
          non_constant_look_up = true;
          break;
        }
      }
    }

    PQTabType tabType = qep_tab->pq_div_tab ? DIV_TAB : CUT_TAB;
    AccessPath *table_path = nullptr;
    if (non_constant_look_up)
      table_path = NewPQrefScanAccessPath(
          thd, qep_tab->table(), nullptr, tabType, false, &qep_tab->ref(),
          qep_tab->m_reversed_access, qep_tab->use_order(), qep_tab);
    else
      table_path = NewPQblockScanAccessPath(thd, qep_tab->table(), nullptr,
                                            tabType, qep_tab, false);
    if (table_path != nullptr) {
      *path = *table_path;
      return false;
    }
  }
  return true;
}

/**
 * Determine whether a parallel execution is preferable by serial execution
 * plan's cost and count.
 *
 * This function compares the cost and row count of a serial execution plan
 * against the configured parallel execution thresholds.
 *
 * 1. If the serial plan cost is less than `parallel_cost_threshold`, return
 *    true.
 *    - This indicates the serial plan cost is already distorted, because
 *      earlier checks have shown that the parallel scan cost exceeded the
 *      threshold. In this case, we conservatively allow parallel execution.
 *
 * 2. Compute the minimum number of rows (`min_rows_num`) that can be
 *    transmitted between leader and worker processes while still making
 *    parallel execution cheaper than serial execution:
 *
 *       parallel_cost_threshold =
 *           parallel_cost_threshold / parallel_default_dop
 *           + parallel_setup_cost
 *           + parallel_tuple_cost * min_rows_num
 *
 *    - Here, `parallel_cost_threshold` represents the minimal serial plan cost.
 *    - `parallel_cost_threshold / parallel_default_dop` approximates the
 * parallel plan cost.
 *
 *    If the number of rows transmitted under the minimal serial cost is less
 *    than min_row_num, then the total parallel cost is definitely lower than
 *    the serial cost, which means parallel execution can be chosen. In other
 *    words, if the serial cost is greater than 'parallel_cost_threshold', then
 *    having the transmitted row count less than min_row_num makes it even more
 *    favorable to use parallel execution.
 *  @param thd       Thread context.
 *  @param cost      Cost of the serial execution plan.
 *  @param rows_num  The output row count of the serial execution plan is
                     approximately equal to the number of rows transferred
                     between the gather and worker.
 *
 *  @return false means to continue with the subsequent checks, while true
 *          indicates that parallel execution should be selected.
 */
static bool check_serial_execution_cost(THD *thd, double cost,
                                        double rows_num) {
  if (cost < thd->variables.parallel_cost_threshold) return true;
  assert(thd->variables.parallel_default_dop != 0);
  if (thd->variables.parallel_tuple_cost == 0.0) return false;
  double min_rows_num = (thd->variables.parallel_cost_threshold -
                         (thd->variables.parallel_setup_cost +
                          thd->variables.parallel_cost_threshold /
                              thd->variables.parallel_default_dop)) /
                        thd->variables.parallel_tuple_cost;
  if (min_rows_num < 0) return false;
  if (rows_num < min_rows_num) return true;
  return false;
}

/**
 * Compare the cost of parallel query plan and serial query plan
 * to determine whether parallel execution is appropriate.
 *
 * @return
 *
 *    true:  parallel execution is appropriate.
 *    false: serial execution  is appropriate.
 */
bool JOIN::is_suit_for_pq_based_cost() {
  if (is_untrust_full_access_path_row_nums(m_root_access_path)) return true;
  double seq_cost = get_full_access_path_cost(m_root_access_path);
  double seq_rows_num = get_full_access_path_row_nums(m_root_access_path);
  if (check_serial_execution_cost(thd, seq_cost, seq_rows_num)) return true;
  PathCosts orig_costs;
  if (pq_rewrite_full_access_path_cost(thd, this, m_root_access_path,
                                       &orig_costs)) {
    return false;
  }
  double exchange_rows_num =
      get_full_access_path_row_nums(m_root_access_path) * thd->pq_dop;
  double parl_cost = get_full_access_path_cost(m_root_access_path);
  pq_resore_full_access_path_cost(m_root_access_path, orig_costs);
  /*
    In the following situations, it is possible that the optimizer cannot
    provide a cost estimate or the calculation is incorrect.
  */
  if (parl_cost < 0.0 || seq_cost < 0.0 || parl_cost > seq_cost) return true;
  parl_cost += (exchange_rows_num * thd->variables.parallel_tuple_cost +
                thd->variables.parallel_setup_cost);
  if (seq_cost < parl_cost) return false;
  return true;
}

/**
 * make parallel query leader's physical query plan
 *
 * @join : origin serial query plan
 * @dop : degree of parallel
 * @return
 *
 *    SEQ_EXEC:  can not run in parallel mode, due to RBO.
 *    PARL_EXEC: successfully run  in parallel mode
 *    ABORT_EXEC: run error in parallal mode and then drop it
 */
PQ_exec_status make_pq_leader_plan(JOIN *join, THD *thd) {
  mem_root_deque<Item *> *fields_old = join->fields;
  QEP_TAB *tab = nullptr;
  Gather_operator *gather = nullptr;
  ulong saved_thd_want_privilege = thd->want_privilege;
  thd->want_privilege = 0;

  Opt_trace_context *const trace = &thd->opt_trace;
  Opt_trace_object trace_wrapper(trace);
  Opt_trace_object trace_exec(trace, "make_parallel_query_plan");
  trace_exec.add_select_number(join->query_block->select_number);
  Opt_trace_array trace_detail(trace, "detail");

  bool graceful_fallback = thd->variables.parallel_graceful_fallback;
  std::vector<uint32_t> orig_digests =
      backup_leader_plan(thd, graceful_fallback, join);
  MEM_ROOT *saved_mem_root = thd->mem_root;
  thd->mem_root = thd->pq_mem_root;
  Query_block *saved_select = thd->lex->current_query_block();
  thd->lex->set_current_query_block(join->query_block);

  for (uint i = join->const_tables; i < join->primary_tables; i++) {
    if (join->qep_tab[i].pq_div_tab) {
      join->idx_div_tab = i;
      join->qep_tab[i].table()->file->do_parallel_scan = true;
    }

    if (join->qep_tab[i].pq_cut_tab) {
      join->idx_cut_tab = i;
      join->qep_tab[i].table()->file->do_parallel_scan = true;
    }
  }
  assert(join->idx_div_tab < (int)join->primary_tables);
  Table_ref *table_ref = join->qep_tab[join->idx_div_tab].table_ref;
  join->pq_stable_sort = pq_check_stable_sort(join);
  Opt_trace_object trace_one_table(trace);
  trace_one_table.add_utf8_table(table_ref)
      .add("degree of parallel", thd->pq_dop)
      .add("merge_sort", join->pq_stable_sort);

  // describes what to do in case of failure:
  PQ_exec_status exec_code =
      graceful_fallback ? PQ_exec_status::SEQ_EXEC : PQ_exec_status::ABORT_EXEC;

  gather = make_pq_gather_operator(join, thd->pq_dop);
  if (!gather || !gather->m_template_join ||
      DBUG_EVALUATE_IF("pq_leader_abort1", true, false)) {
    trace_wrapper.add_alnum("failed reason",
                            "Error happened during make gather.");
    goto err;
  }

  if (thd->lex->is_explain())
    pq_rewrite_full_access_path_cost(
        thd, join, gather->m_template_join->m_root_access_path);

  /*
    Currently, graceful fallback is only performed for failures in the PQ
    clone and refix_fields processes for the following reasons:
     1. Some PQ RBO rules are implemented in the PQ clone and refix_fields
        processes, making graceful fallback valuable in this area.
     2. Subsequent processes primarily involve modifications to the execution
        plan on the leader. These can only fail in cases of memory allocation
        failure, making graceful fallback less valuable.
     3. Modifications to the execution plan on the leader involve many related
        data structures. Implementing backup/restore would require significant
        code changes and would be difficult to maintain, with a low return on
        investment.
     4. Subsequent processes involve modifications to dynamic memory data
        structures such as vector/deque in the serial execution plan. It's not
        possible to perform memory consistency checks
        (thd->mem_root->CalcMemDigest()) and ensure quality, making it difficult
        to maintain.
  */
  graceful_fallback = false;
  exec_code = PQ_exec_status::ABORT_EXEC;

  if (join->make_leader_rewritten_tab() ||
      DBUG_EVALUATE_IF("pq_leader_abort3", true, false)) {
    trace_wrapper.add_alnum("failed reason",
                            "Error happened during make rewritten tab.");
    goto err;
  }

  tab = join->qep_tab;
  tab->gather = gather;
  thd->pq_gathers.push_back(gather);

  // here we collect the reverse-sorted groups, and later this
  // collection is used to set up the merge sort's order properly
  mark_desc_groups(join, gather);

  if (join->restore_optimized_vars() || join->make_leader_tmp_table() ||
      join->make_tmp_tables_info() ||
      DBUG_EVALUATE_IF("pq_leader_abort2", true, false)) {
    trace_wrapper.add_alnum("failed reason",
                            "Error happened during restore optimized vars or "
                            "make leader tmp table.");
    goto err;
  }

  // Update the m_accum_properties for all items based on the latest
  // ref_items, to ensure accurate evaluation of Item::has_aggregation in
  // Exchange::convert_mq_data_to_record.
  for (Item *item : join->tmp_fields[REF_SLICE_PQ_TMP])
    item->update_used_tables();
  assert(tab->table()->s->table_category == TABLE_CATEGORY_TEMPORARY);
  tab->table()->pos_in_table_list = tab->table_ref;
  join->create_access_paths();
  pq_stmt_executed++;
  thd->pq_executed = true;
  thd->mem_root = saved_mem_root;
  thd->want_privilege = saved_thd_want_privilege;
  // Plan cache doesn't support parallel query plan. Plan caching was decided
  // during optimization, before PQ was decided. But PQ wins.
  plan_cache::invalidate_cached_plan(join->query_block);
  plan_cache::set_uncacheable(join->query_block);
  return PQ_exec_status::PARL_EXEC;

err:
  thd->mem_root = saved_mem_root;
  // Before falling back to serial execution, roll back
  // derived->m_query_blocks_to_materialize to avoid affecting the execution of
  // the serial execution plan.
  reset_derived_materialize_query_blocks(thd, join->query_block,
                                         MatStatus::FINISHED);
  thd->lex->set_current_query_block(saved_select);
  if (gather) {
    pq_free_gather(gather);
    if (tab) tab->gather = nullptr;
  }

  join->fields = fields_old;
  thd->want_privilege = saved_thd_want_privilege;
  restore_leader_plan(thd, graceful_fallback, orig_digests, join);

  if (exec_code == PQ_exec_status::ABORT_EXEC)
    my_error(ER_PARALLEL_FAIL_INIT, MYF(0));
  return exec_code;
}

/**
 * make a parallel query worker's physics query plan.
 */
static JOIN *make_pq_worker_plan(PQ_worker_manager *mngr) {
  JOIN *join = nullptr, *top_join = nullptr;
  Gather_operator *gather = mngr->m_gather;
  JOIN *template_join = gather->m_template_join;
  Query_result *mq_result = nullptr;
  MQueue_handle *msg_handler = mngr->m_handle;
  std::vector<Query_block *> query_blocks_to_clone;

  // duplicate a query plan from template join, which is used in PQ workers
  THD *new_thd = pq_new_thd(template_join->thd);
  if (!new_thd) goto err;
  new_thd->pq_leader = mngr->thd_leader;
  new_thd->mem_root = new_thd->pq_mem_root;
  new_thd->pq_worker_info = mngr;

  // A worker is created to execute one Query_block and dies when it's done.
  // Its Query_block is a clone of the original Query_block, but its
  // Query_expression is a single-query-block one without resemblance with the
  // original one (see pq_make_join() below).
  join = pq_make_join(new_thd, template_join);
  top_join = join;
  if (!join || pq_dup_tabs(join, template_join, false) ||
      DBUG_EVALUATE_IF("pq_worker_abort1", true, false)) {
    goto err;
  }

  join->need_tmp_pq = true;
  join->m_msg_handler = msg_handler;
  query_blocks_to_clone = list_query_blocks_to_clone(join->query_block);

  for (auto query_block : query_blocks_to_clone) {
    Switch_for_resolution_of_query_block switch_resol(
        new_thd->lex, query_block, query_block != join->query_block);
    auto join2 = query_block->join;
    if (!join2) goto err;
    auto template_join2 = query_block->pq_is_clone_of()->join;

    assert(new_thd == join2->thd);

    if (join2->setup_tmp_table_info(template_join2) ||
        (join2->qep_tab && join2->make_tmp_tables_info()) ||
        pq_make_join_readinfo(
            join2,
            /*gather=*/
            (query_block == top_join->query_block) ? gather : nullptr, true) ||
        DBUG_EVALUATE_IF("pq_worker_abort2", true, false)) {
      sql_print_warning(
          "[Parallel query] Create worker tmp tables or make join read info "
          "failed");
      goto err;
    }
    query_block->pq_unlink_clone();
  }

  /** set query result */
  mq_result = new (join->thd->pq_mem_root)
      Query_result_mq(join, msg_handler, join->pq_stable_sort);
  if (!mq_result || DBUG_EVALUATE_IF("pq_worker_error2", true, false)) {
    sql_print_warning("[Parallel query] Create worker result mq failed");
    goto err;
  }

  join->query_expression()->set_query_result(mq_result);
  join->query_block->set_query_result(mq_result);

  return join;

err:
  if (new_thd) {
    // If make worker plan failed, mark pq_error, so that if will not wait at
    // the barrier when free join later.
    new_thd->pq_error = true;
    new_thd->store_globals();
  }
  pq_free_join(new_thd, join);
  pq_free_thd(new_thd);
  mngr->thd_leader->store_globals();
  return nullptr;
}

/**
 * main function of parallel worker.
 *
 */
void *pq_worker_exec(void *arg) {
  if (my_thread_init()) {
    my_thread_exit(0);
    return nullptr;
  }

  Diagnostics_area *da;
  const Sql_condition *cond;
  /** only for single query block */
  Query_result *result = nullptr;
  THD *thd = nullptr, *leader_thd = nullptr;

  PQ_worker_manager *mngr = static_cast<PQ_worker_manager *>(arg);
  assert(mngr->m_gather);
  DBUG_PQ_ENTER(*mngr->m_gather->m_code_state);

  leader_thd = mngr->thd_leader;
  THD *temp_thd = mngr->m_gather->m_template_join->thd;
  MQueue_handle *msg_handler = mngr->m_handle;
  bool res = true;
  bool need_innodb_snapshot = false;

  /*
    The leader has a loop of "launch a worker / wait for it to signal it's
    ready / launch another worker / etc".
    So only one worker may be in make_pq_worker_plan() at a time, as the
    "ready" signal is sent only later in this function.
  */

  JOIN *join = make_pq_worker_plan(mngr);
  if (!join || DBUG_EVALUATE_IF("pq_worker_error1", true, false)) {
    sql_print_warning("[Parallel query] Make worker plan failed");
    goto err;
  }

  thd = join->thd;
#ifndef NDEBUG
  assert(current_thd == join->thd && thd->pq_leader == leader_thd);
  thd->pq_skip_fetch_ctx = leader_thd->pq_skip_fetch_ctx;
#endif  // NDEBUG

  // Only when this query block contains innodb tables, we need consistent
  // snapshot; otherwise, the cloned snapshot will be not free and thus
  // leads to memory leak.
  for (uint i = 0; i < join->primary_tables; i++) {
    auto table = join->qep_tab[i].table();
    if (table->file->ht->db_type == DB_TYPE_INNODB &&
        table->s->tmp_table == NO_TMP_TABLE) {
      need_innodb_snapshot = true;
      break;
    }
  }

  // Clone leader's read_view to generate consistent read. If we can't clone
  // read_view from leader (e.g., leader has not generated read_view), then each
  // worker will generate its own read_view when reading first record in
  // "row_search_mvcc". This means that each worker may have different
  // read_view, and the final query result is inconsistent with normal query.
  if (need_innodb_snapshot && pq_clone_innodb_snapshot(thd, leader_thd))
    goto err;

  mngr->signal_status(thd, PQ_worker_state::READY);
  thd->killed.store(thd->pq_leader->killed.load());

  {
    Ignore_error_handler ignore_handler;
    if (thd->lex->is_ignore())  // Leader is executing INSERT IGNORE SELECT
      thd->push_internal_handler(&ignore_handler);
    res = join->query_expression()->ExecuteIteratorQuery(thd);
    if (thd->lex->is_ignore()) thd->pop_internal_handler();
  }
  mngr->signal_status(thd, PQ_worker_state::OVER);
  if (thd->lex->is_explain_analyze) {
    JOIN *leader_template_join = thd->pq_worker_info->m_gather->m_template_join;
    CollectWorkerIterTimingInfo(join->query_expression()->root_access_path(),
                                join, leader_template_join->m_root_access_path,
                                leader_template_join);
  }

err:
  /* s1: send error msg to MQ */
  if (res) {
    assert(msg_handler && leader_thd);
    assert(mngr->m_status == PQ_worker_state::INIT ||
           (thd->is_error() || thd->killed || thd->pq_error));
    leader_thd->pq_error = true;
    msg_handler->send_exception_msg(ERROR_MSG);
  }
  msg_handler->set_detached_status(MQ_HAVE_DETACHED);
  /** during pq_make_join, join->thd may have been created */
  thd = (join && join->thd) ? join->thd : thd;

  /* s2: release resource */
  result =
      join && join->query_block ? join->query_block->query_result() : nullptr;
  if (result) {
    result->cleanup();
    destroy(result);
  }
  pq_free_join(thd, join);

  /* s3: collect error message */
  if (thd) {
    // When thd has a error, raise_condition will report to error to audit_log
    // plugin and print query in audit log file. In this process, it will call
    // THD::rewritten_query to get the rewritten query. If done from a different
    // thread (from the one that the rewritten_query is set on), the caller must
    // hold LOCK_thd_query while calling this! So in pq worker thread, it must
    // firstly hold LOCK_thd_query and call raise_condition function.
    mysql_mutex_lock(&temp_thd->LOCK_thd_query);
    temp_thd->pq_merge_status(thd);
    // each worker updates "examined_rows" into gather's template_join.
    mngr->m_gather->m_template_join->examined_rows +=
        thd->get_examined_row_count();

    da = thd->get_stmt_da();

    DBUG_EXECUTE_IF("pq_worker_failed",
                    da->set_error_status(temp_thd, ER_QUERY_INTERRUPTED););

    if (thd->is_error()) {
      temp_thd->raise_condition(da->mysql_errno(), da->returned_sqlstate(),
                                Sql_condition::SL_ERROR, da->message_text(),
                                false);
    }
    if (da->cond_count() > 0) {
      Diagnostics_area::Sql_condition_iterator it = da->sql_conditions();
      while ((cond = it++)) {
        temp_thd->raise_condition(cond->mysql_errno(), NULL, cond->severity(),
                                  cond->message_text(), false);
      }
    }
    mysql_mutex_unlock(&temp_thd->LOCK_thd_query);
    pq_free_thd(thd);
  }

  DBUG_PQ_RESET();
  my_thread_end();
  /* s4: send last status to leader */
  PQ_worker_state status =
      res ? PQ_worker_state::ERROR : PQ_worker_state::COMPELET;
  mngr->signal_status(NULL, status);
  my_thread_exit(0);
  return nullptr;
}

/**
Plan refinement stage: do various setup things for the executor, including
  - setup join buffering use
  - push index conditions
  - increment relevant counters
  - etc

@return false if successful, true if error (Out of memory)
*/

bool pq_make_join_readinfo(JOIN *join, Gather_operator *gather,
                           bool pq_worker) {
  const bool prep_for_pos =
      join->need_tmp_before_win || join->select_distinct ||
      (join->group_list.order != nullptr) || (join->order.order != nullptr) ||
      join->m_windows.elements > 0;

  for (uint i = join->const_tables; i < join->primary_tables; i++) {
    QEP_TAB *const qep_tab = &join->qep_tab[i];
    TABLE *const table = qep_tab->table();
    if (prep_for_pos ||
        // PQ's stable sort needs row IDs => calls position()
        (qep_tab->pq_div_tab && join->pq_stable_sort))
      table->prepare_for_position();
  }
  std::vector<Item *> predicates_below_join;
  std::vector<PendingCondition> predicates_above_join;

  if (join->qep_tab) {
    for (uint i = 0; i < join->tables; i++) {
      QEP_TAB *qep_tab = &join->qep_tab[i];
      if (qep_tab->pq_div_tab || qep_tab->pq_cut_tab) {
        PQTabType tabType = qep_tab->pq_div_tab ? DIV_TAB : CUT_TAB;

        qep_tab->table()->file->pq_table_scan =
            gather->pq_tabs[tabType].table_scan;
        qep_tab->table()->file->do_parallel_scan = true;
        if (pq_worker || join->thd->lex->is_explain_analyze)
          qep_tab->gather = gather;

        /* index push down */
        uint keyno = gather->pq_tabs[tabType].keyno;
        if (!(keyno == qep_tab->table()->s->primary_key &&
              qep_tab->table()->file->primary_key_is_clustered()) &&
            qep_tab->pq_cond) {
          TABLE *tbl = qep_tab->table();
          Item *cond = qep_tab->pq_cond;
          Item *idx_cond = make_cond_for_index(cond, tbl, keyno, false);
          if (idx_cond) {
            Item *idx_remainder_cond =
                tbl->file->idx_cond_push(keyno, idx_cond);
            if (idx_remainder_cond != idx_cond)
              qep_tab->ref().disable_cache = true;

            Item *row_cond = make_cond_remainder(qep_tab->pq_cond, true);
            if (row_cond) {
              if (idx_remainder_cond)
                and_conditions(&row_cond, idx_remainder_cond);
              idx_remainder_cond = row_cond;
            }
            qep_tab->set_condition(idx_remainder_cond);
          }
        }
      } else if (join->pq_last_sort_idx == int(i) &&
                 i >= join->primary_tables) { /** set order by */
        assert(qep_tab->filesort);
        // join->m_select_limit indicates whether to perform a full table scan,
        // and join->unit->select_limit_cnt indicates how may rows to send,
        // if the worker is not going to send all rows to leader, it cannot
        // skip the filesort.
        if (join->query_expression()->select_limit_cnt == HA_POS_ERROR) {
          destroy(qep_tab->filesort);
          qep_tab->filesort = nullptr;
        }
        // group by is implemented with index-scan. Then, we should
        // add sorting on the last tmp table to ensure the partial results
        // send to leader is sorted with group-field. Consequently, leader
        // can employ merge sort to merge these partial results and then
        // re-aggregate them in a streaming manner.
        if (join->pq_rebuilt_group) {
          assert(join->saved_join_group_list);
          assert(join->m_select_limit == HA_POS_ERROR);
          SQL_I_List<ORDER> orig_list;

          restore_list(join->saved_join_group_list, orig_list);
          ORDER *order = restore_optimized_group_order(
              orig_list, join->saved_optimized_vars.optimized_group_flags);
          if (order) {
            ORDER_with_src group_list = ORDER_with_src(order, ESC_GROUP_BY);
            join->add_sorting_to_table(i, &group_list, false);
          }
        }
      }
    }
  }  // end of if(join->qep_tab)

  join->set_optimized();
  join->query_expression()->set_optimized();

  /** generate execution tree */
  if (join->zero_result_cause != nullptr)
    join->create_access_paths_for_zero_rows();
  else
    join->create_access_paths();

  if (join->query_expression()->create_access_paths(join->thd)) {
    return true;
  }

  // MarkPartialInputsForHashJoin must be called after we have marked the "cut"
  // table and before create iterator.
  if (MarkPartialInputsForHashJoin(join->m_root_access_path, join, gather))
    return true;

  if (pq_worker)
    join->query_expression()->create_iterator_from_accesspath(join->thd);
  else if (join->thd->lex->is_explain_analyze) {
    // For explain analyze scenaio, pq leader need to create iterator of
    // workers' query plan, then leader can gather worker's execution timing
    // info.
    join->thd->pq_leader_create_fake_iter = true;
    // Pretend this is pq woker thread when create iterator.
    if (gather && gather->m_workers) {
      join->thd->pq_worker_info = gather->m_workers[0];
    }
    join->query_expression()->create_iterator_from_accesspath(join->thd);
    join->thd->pq_leader_create_fake_iter = false;
    join->thd->pq_worker_info = nullptr;
  }

  return false;
}

/**
   @returns true if "stable sort" is needed.

   @param join  JOIN object
*/
bool pq_check_stable_sort(JOIN *join) {
  if ((join->pq_last_sort_idx >= (int)join->primary_tables) ||
      (join->need_tmp_before_win &&
       join->m_ordered_index_usage != JOIN::ORDERED_INDEX_ORDER_BY)) {
    return false;
  }

  // We have no need to implicit sort rows from hash join (HJ）, as these
  // rows are shuffled during HJ. Conversely, we may encounter deadlock (ISSUE
  // 627) if we force using stable sort on HJ rows, as follows:
  //   1. worker1 read all rows from probe table and wait for worker2 which is
  //      still in the probe phase.
  //   2. worker2 waits for the leader to consume data in the message queue in
  //      a blocking-mode as MQ is full.
  //   3. leader wait to read from worker1.
  if (join->order.order == nullptr) {
    // check whether HJ (or BKA) exists in this query. For BKA, it also shuffles
    // rows, thus force stable sorting rows on leader is useless.
    for (uint i = join->const_tables; i < join->primary_tables; i++) {
      if (join->qep_tab[i].op_type == QEP_TAB::OT_BNL ||
          join->qep_tab[i].op_type == QEP_TAB::OT_BKA) {
        return false;
      }
    }
  }

  return true;
}

/*
 * record the mapping:
 *      L = join->query_block->group_list -------> join->group_list = R
 *
 * @result:
 *    if L[i] \in R, then optimized_flags[i] = 0; otherwise, optimized_flags[i]
 * = 1 (i.e., L[i] is optimized away in JOIN::optimize()). Correspondingly, we
 * can use L and optimized_flags to retrieve R.
 *
 */
void record_optimized_group_order(Group_list_ptrs *ptr,
                                  ORDER_with_src &new_list,
                                  std::vector<bool> &optimized_flags) {
  optimized_flags.clear();
  // group_list (or order) is optimized to NULL.
  if (new_list.order == nullptr || ptr == nullptr) {
    return;
  }

  optimized_flags.resize(ptr->size(), true);
  int i = 0;
  auto order = new_list.order;

  for (auto ptr_it : *ptr) {
    if (!order) break;
    // find this item, i.e., this item is not optimized away
    if (ptr_it->item[0] && order->item[0] &&
        (ptr_it->item[0]->eq(order->item[0], false))) {
      optimized_flags[i] = false;
      order = order->next;
    } else {
      optimized_flags[i] = true;
    }
    i++;
  }
}

/*
 * restore the optimized group/order list, using original and optimized_flags
 */
ORDER *restore_optimized_group_order(SQL_I_List<ORDER> &orig_list,
                                     std::vector<bool> &optimized_flags) {
  int size = optimized_flags.size();
  if (0 == size) return nullptr;

  ORDER *header = orig_list.first;
  ORDER **prev_ptr = &header;

  int idx = 0;

  for (auto &iterator : orig_list) {
    if (!optimized_flags[idx]) {
      *prev_ptr = &iterator;
      prev_ptr = &iterator.next;
    }
    idx++;
  }
  *prev_ptr = 0;

  return header;
}

void restore_list(Group_list_ptrs *ptr, SQL_I_List<ORDER> &orig_list) {
  orig_list.clear();
  if (!ptr) return;

  for (auto order : *ptr) {
    orig_list.link_in_list(order, &order->next);
  }
}

void pq_free_thd(THD *thd) {
  if (!thd) return;
  close_thread_tables(thd);
  thd->mdl_context.release_transactional_locks();
  trans_commit_stmt(thd);
  end_connection(thd);
  close_connection(thd, 0, false, false);
  thd->get_stmt_da()->reset_diagnostics_area();
  cleanup_items(thd->item_list());
  thd->free_items();
  // Non-pq query will cleanup ptrc in THD::cleanup_after_query. Here we need
  // call it independently.
  ptrc::cleanup(thd);
  thd->release_resources();
  destroy(thd);
}

void pq_free_join(THD *thd, JOIN *join) {
  if (join) {
    for (auto query_block : list_query_blocks_to_clone(join->query_block)) {
      auto join2 = query_block->join;
      if (join2) {
        join2->query_expression()->m_root_iterator.reset();
        join2->destroy();
        destroy(join2);
      }
    }
  }

  if (thd && thd->lex) {
    thd->lex->destroy();
  }
}

Field *pq_get_rebuilt_field(Item *item) {
  Field *field = nullptr;
  if (item->type() == Item::FIELD_ITEM) {
    field = down_cast<Item_field *>(item)->field;
  } else if (item->type() == Item::SUM_FUNC_ITEM) {
    Item_sum *item_sum = down_cast<Item_sum *>(item);
    assert(item_sum->argument_count() == 1);  // only support one parameter now
    Item *arg_item = item_sum->get_arg(0);
    assert(arg_item->type() == Item::FIELD_ITEM);
    field = down_cast<Item_field *>(arg_item)->field;
  }
#ifndef NDEBUG
  else {
    /** support partial computation rebuilt */
    assert(item->skip_send_to_mq);
  }
#endif
  return field;
}

/**
 * fetch the used key
 * @table: the first non-const table
 * @key: the index of the key in table->key_info
 * @key_parts: #fields in this key
 * @res_fields: the set of all fields in this key
 */
static void get_key_fields(TABLE *table, int key, uint key_parts,
                           std::vector<std::string> &key_fields) {
  assert(table && table->key_info);

  KEY_PART_INFO *kp = table->key_info[key].key_part;
  for (uint i = 0; i < key_parts; i++, kp++) {
    key_fields.emplace_back(kp->field->field_name);
  }
}

/**
 * fetch the key fields used in the tab, by which the output is implicitely
 * sorted due to the access method.
 * @tab
 * @res_fields
 *
 * @retval:
 *  false for success, and otherwise true
 */
bool get_table_key_fields(QEP_TAB *tab, std::vector<std::string> &key_fields) {
  key_fields.clear();
  assert(tab);
  auto type = tab->type();

  // the original table in leader_join's qep_tab
  TABLE *table = tab->table();
  Index_lookup *ref = &tab->ref();
  if (!table || !ref) return true;

  // consider the following cases to obtain the sort filed:
  // (1) the case of explicitly using primary key
  if (tab->ref().key >= 0) {
    get_key_fields(table, ref->key, ref->key_parts, key_fields);
  }
  // (2) the case of index scan
  else if (type == JT_INDEX_SCAN) {
    int key = tab->index();
    uint key_parts = table->key_info[key].user_defined_key_parts;
    get_key_fields(table, key, key_parts, key_fields);
  }
  // (3) the case of index range.
  else if (type == JT_RANGE || (type == JT_REF && tab->range_scan())) {
    auto range_scan_path = tab->range_scan();
    if (!range_scan_path) return true;

    uint keyno = used_index(range_scan_path);
    if (keyno != MAX_KEY)
      get_key_fields(table, keyno, get_used_key_parts(range_scan_path),
                     key_fields);
  }
  // (4) the case of implicitly using primary key
  else {
    if (table->s->primary_key != MAX_KEY) {
      int key = table->s->primary_key;
      int key_parts = table->key_info[key].user_defined_key_parts;
      get_key_fields(table, key, key_parts, key_fields);
    }
    // (5) other cases
  }
  return false;
}

bool set_key_order(QEP_TAB *tab, std::vector<std::string> &key_fields,
                   ORDER **order_ptr, Ref_item_array *ref_ptrs) {
  JOIN *join = tab->join();
  assert(join && join->order.empty());
  if (!key_fields.size()) {
    *order_ptr = NULL;
    return false;
  }

  std::map<std::string, Item *> fields_map;  // map[field] = item
  std::map<std::string, Item *>::iterator iter;
  std::vector<Item *> order_items;

  Ref_item_array ref_items = *ref_ptrs;
  /** (1) build the map: {name} -> {item} */
  for (uint i = 0; i < join->fields->size(); i++) {
    Item *item = ref_items[i];
    if (item && item->type() == Item::FIELD_ITEM) {
      std::string field_name =
          static_cast<Item_field *>(item)->field->field_name;
      fields_map[field_name] = item;
    }
  }

  /** (2) find the item whose name in res_fields */
  for (std::string key : key_fields) {
    iter = fields_map.find(key);
    if (iter != fields_map.end()) {
      order_items.push_back(iter->second);
    }
  }

  /** (3) generate sort order */
  THD *thd = join->thd;
  SQL_I_List<ORDER> order_list;

  for (Item *item : order_items) {
    ORDER *order = new (thd->pq_mem_root) ORDER();
    if (!order) {
      *order_ptr = NULL;
      return true;
    }

    order->item_initial = item;
    order->item = &order->item_initial;
    order->in_field_list = 1;
    order->is_explicit = 0;
    add_to_list(order_list, order);
  }

  *order_ptr = order_list.first;
  return false;
}

void EstimatePQGatherOperatorCost(AccessPath *path, THD *thd) {
  AccessPath *worker_access_path =
      path->parallel_scan().gather->m_template_join->m_root_access_path;
  if (worker_access_path) {
    // Keep same logic with JOIN::is_suit_for_pq_based_cost,
    // use same interface with JOIN::is_suit_for_pq_based_cost later.
    double rows =
        get_full_access_path_row_nums(worker_access_path) * thd->pq_dop;
    double parl_cost = get_full_access_path_cost(worker_access_path);
    if (rows > 0 && parl_cost > 0) {
      path->set_num_output_rows(rows);
      parl_cost += (rows * thd->variables.parallel_tuple_cost +
                    thd->variables.parallel_setup_cost);
      path->cost = parl_cost;
      path->init_cost = worker_access_path->init_cost;
      path->init_once_cost = worker_access_path->init_once_cost;
    }
  }
}
