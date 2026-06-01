/* Copyright (c) 2025, Huawei and/or its affiliates.

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

#include "sql/sql_filter/sql_filter_cache.h"
#include <string.h>
#include <boost/algorithm/string.hpp>
#include "sql/common/table.h"
#include "sql/debug_sync.h"
#include "sql/derror.h"  // ER_THD
#include "sql/error_handler.h"
#include "sql/opt_trace.h"
#include "sql/rds_permission_control.h"
#include "sql/sql_class.h"

/**
  Contructor of System_sql_filter

  Init all the sql filter rule map, locks.
*/
System_sql_filter::System_sql_filter() : m_rule_map(), m_lock() {
  /* Init rule map */
  for (size_t i = 0; i < static_cast<size_t>(SqlFilterType::LAST); ++i) {
    m_rule_map[i] = allocate_sqlfilter_object<Sql_filter_map>();
  }

  /* Init lock */
  for (size_t i = 0; i < static_cast<size_t>(SqlFilterType::LAST); ++i) {
    m_lock[i] = allocate_sqlfilter_object<mysql_rwlock_t>();
    mysql_rwlock_init(key_rwlock_sql_filter, m_lock[i]);
  }
}

/**
  Destroy all the container elements.
*/
System_sql_filter::~System_sql_filter() {
  for (size_t i = 0; i < static_cast<size_t>(SqlFilterType::LAST); ++i) {
    for (auto it = m_rule_map[i]->begin(); it != m_rule_map[i]->end();) {
      destroy_object(it->second);
      it = m_rule_map[i]->erase(it);
    }
  }

  for (size_t i = 0; i < static_cast<size_t>(SqlFilterType::LAST); ++i) {
    destroy_object(m_rule_map[i]);
    mysql_rwlock_destroy(m_lock[i]);
    destroy_object(m_lock[i]);
  }
}

/**
  Flush all sql filter rules into maps

  @param[in]      records       sql filter records container.
*/
void System_sql_filter::flush_records(Conf_records *records) {
  for (size_t i = 0; i < static_cast<size_t>(SqlFilterType::LAST); ++i) {
    Lock_helper lock(m_lock[i], true);
    DBUG_ASSERT(lock.effect());
    DEBUG_SYNC_C("flush_sql_filter_after_map_lock");
    clear(i);
    for (Conf_records::const_iterator it = records->cbegin();
         it != records->cend(); it++) {
      Sql_filter_record *r = dynamic_cast<Sql_filter_record *>(*it);
      size_t type = static_cast<size_t>(r->m_type);
      if (type != i) continue;

      Sql_filter *filter = allocate_sqlfilter_object<Sql_filter>(r);
      m_rule_map[type]->emplace(filter->m_id, filter);
    }
  }
}

/**
  Fill one sql filter rule into map

  @param[in]      records       sql filter records container.
*/
void System_sql_filter::add_records(Conf_records *records) {
  for (Conf_records::const_iterator it = records->cbegin();
       it != records->cend(); it++) {
    Sql_filter_record *r = dynamic_cast<Sql_filter_record *>(*it);
    Sql_filter *filter = allocate_sqlfilter_object<Sql_filter>(r);
    if (!filter) continue;

    size_t type = static_cast<size_t>(filter->m_type);
    Lock_helper lock(m_lock[type], true);
    DBUG_ASSERT(lock.effect());
    DEBUG_SYNC_C("add_sql_filter_after_map_lock");
    auto map_iter = m_rule_map[type]->find(filter->m_id);
    if (map_iter == m_rule_map[type]->end()) {
      m_rule_map[type]->emplace(filter->m_id, filter);
    } else {
      destroy_object(map_iter->second);
      m_rule_map[type]->erase(map_iter);
      m_rule_map[type]->emplace(filter->m_id, filter);
    }
  }
}

/**
  Clear the sql filter.
*/
void System_sql_filter::clear(size_t type) {
  for (auto it = m_rule_map[type]->begin(); it != m_rule_map[type]->end();) {
    destroy_object(it->second);
    it = m_rule_map[type]->erase(it);
  }
}

/**
  Delete the sqlfilter rule from all maps by id

  @param[in]        sqlfilter rule id

  @retval           false for successully deleted; true for not found
*/
bool System_sql_filter::delete_sql_filter(ulonglong id) {
  for (size_t i = 0; i < static_cast<size_t>(SqlFilterType::LAST); ++i) {
    Lock_helper lock(m_lock[i], true);
    DBUG_ASSERT(lock.effect());
    DEBUG_SYNC_C("delete_sql_filter_after_map_lock");
    auto it = m_rule_map[i]->find(id);
    if (it != m_rule_map[i]->end()) {
      destroy_object(it->second);
      m_rule_map[i]->erase(id);
      return false;
    }
  }

  return true;
}

Sqlfilter_update_error Sql_filter::update_sqlfilter_rule(
    const Sql_filter_record *record) {
  DBUG_ASSERT(record->m_id == m_id);
  if (m_type != record->m_type) {
    return Sqlfilter_update_error::TYPE_NOT_SAME;
  }

  m_max_concurrency = record->m_max_concurrency;

  String_sqlfilter new_key_str(record->m_key_str.str ? record->m_key_str.str
                                                     : "");
  String_sqlfilter new_node_id(record->m_node_id.str ? record->m_node_id.str
                                                     : "");

  /* reset the statistics when key_str is changed */
  if (m_key_str != new_key_str) {
    m_cur_concur.store(0);
    m_block_query_num.store(0);
    m_key_str = new_key_str;
    std::vector<std::string> keywords;
    boost::split(keywords, record->m_key_str.str, boost::is_any_of("~"));
    for (auto &key : keywords) trim_str(key);
    m_key_array = keywords;
  }
  m_node_id = new_node_id;

  return Sqlfilter_update_error::UPDATE_OK;
}

/**
  Update the sqlfilter rule from all maps by id

  @param[in]        sqlfilter     rule id
  @param[in]        records       sql filter records container.

  @retval           false for successully updated; true for error
*/
Sqlfilter_update_error System_sql_filter::update_sql_filter(
    ulonglong id, Conf_record *record) {
  for (size_t i = 0; i < static_cast<size_t>(SqlFilterType::LAST); ++i) {
    Lock_helper lock(m_lock[i], true);
    DBUG_ASSERT(lock.effect());
    DEBUG_SYNC_C("update_sql_filter_after_map_lock");
    auto it = m_rule_map[i]->find(id);
    if (it == m_rule_map[i]->end()) continue;

    Sql_filter_record *r = dynamic_cast<Sql_filter_record *>(record);
    Sql_filter *org_rule = it->second;
    return org_rule->update_sqlfilter_rule(r);
  }

  return Sqlfilter_update_error::CACHE_NOT_FIND;
}

void System_sql_filter::aggregate_sql_filters(
    Sqlfilter_show_result_container *container) {
  for (size_t i = 0; i < static_cast<size_t>(SqlFilterType::LAST); ++i) {
    Lock_helper lock(m_lock[i], false);
    DBUG_ASSERT(lock.effect());
    DEBUG_SYNC_C("show_sql_filter_after_map_lock");
    for (auto it = m_rule_map[i]->rbegin(); it != m_rule_map[i]->rend(); it++) {
      Sql_filter_show_result result(it->second);
      container->push_back(result);
    }
  }
}

/**
  obtain the filter type according to the sql command

  @param[in]  command  the type of sql command
  @retval SQL filter type
*/
static SqlFilterType obtain_filter_type(uint command) {
  auto filter_type = SqlFilterType::LAST;
  switch (command) {
    case SQLCOM_SELECT:
      filter_type = SqlFilterType::SELECT;
      break;

    case SQLCOM_UPDATE:
    case SQLCOM_UPDATE_MULTI:
      filter_type = SqlFilterType::UPDATE;
      break;

    case SQLCOM_DELETE:
    case SQLCOM_DELETE_MULTI:
      filter_type = SqlFilterType::DELETE;
      break;

    case SQLCOM_INSERT:
    case SQLCOM_INSERT_SELECT:
      filter_type = SqlFilterType::INSERT;
      break;

    default:
      break;
  }
  return filter_type;
}

/**
  find the first occurrence of target as a substring in source.

  @param[in]  source           the source string
  @param[in]  target           the target string (ie, the splitted rule)
  @param[in]  slen             the length of source string
  @param[in]  tlen             the length of target string
  @param[in]  case_sensitive   whether matches with case sensitive

  @retval: -1 if there is no occurrence of target in source; otherwise returns
  the first occurrence.
*/
template <bool case_sensitive>
static int find_first(const char *source, int slen, const char *target,
                      int tlen) {
  DBUG_ASSERT(source && target);

  /*
    variable i is the position of source string, when left length of source
    less than target, no need to check anymore because of can not find out
    matched substring. For example, source string is "FROM order" and target
    string is "customer", i increased to 3, left source string is "M order"
    and length is 7(10 - 3), target string length is 8, we can never find out
    matched target for subsequent cycle, break in advance.
  */
  for (int i = 0; tlen <= (slen - i); i++) {
    int j = 0;
    int match = i;
    while (j < tlen) {
      auto sch = !case_sensitive ? ::toupper(source[match]) : source[match];
      auto tch = !case_sensitive ? ::toupper(target[j]) : target[j];
      if (sch != tch) {
        break;
      }

      match++;
      j++;
    }

    // found one match
    if (j == tlen) {
      return i;
    }
  }

  return -1;
}

// check query whether contains all keywords in order
static bool contains_keywords_in_order(
    THD *thd, const std::vector<std::string> &keywords) {
  char *query_str = const_cast<char *>(thd->query().str);
  int query_len = thd->query().length;
  size_t nums = 0;

  int (*find_func)(const char *, int, const char *, int);
  if (opt_rds_sqlfilter_case_sensitive) {
    find_func = &find_first<true>;
  } else {
    find_func = &find_first<false>;
  }

  for (const auto &keyword : keywords) {
    if (keyword.empty()) continue;
    int item_key_len = (int)keyword.size();
    if (query_len < item_key_len) break;
    int pos = find_func(query_str, query_len, keyword.c_str(), item_key_len);
    if (pos != -1) {
      query_str = (query_str + pos + item_key_len);
      query_len = (query_len - pos - item_key_len);
      nums++;
    } else {
      return false;
    }
  }
  return (nums == keywords.size()) ? true : false;
}

/**
  Check if there are any sql filter matches the current query and update
  the cur_connection of the matched filter item

  @retval  false  no need to change state
  @retval  true   current thd need to be set KILL_QUERY status
*/
bool System_sql_filter::find_matched_filter_and_update(THD *thd,
                                                       SqlFilterType type) {
  Lock_helper lock(m_lock[static_cast<size_t>(type)], false);
  DBUG_ASSERT(lock.effect());
  DEBUG_SYNC_C("match_sql_filter_after_map_lock");

  for (auto it = m_rule_map[static_cast<size_t>(type)]->rbegin();
       it != m_rule_map[static_cast<size_t>(type)]->rend(); it++) {
    /*
      when node_id_ptr == rule_node_id, compare m_key_array
      when node_id_ptr is null or rule_node_id is null
      or node_id_ptr is "" or rule_node_id is "",
      means node_id not exist, don't compare node_id
    */
    const char *rule_node_id = it->second->m_node_id.c_str();
    if (node_id_ptr && rule_node_id &&
        strncmp(node_id_ptr, rule_node_id, MAX_NODE_ID) != 0 &&
        strncmp("", rule_node_id, MAX_NODE_ID) != 0 &&
        strncmp("", node_id_ptr, MAX_NODE_ID) != 0) {
      continue;
    }

    if (contains_keywords_in_order(thd, it->second->m_key_array)) {
      /*
        If the value of max_connection is equal to 0, then we don't need to
        check the value of cur_connection, return immediately.
      */
      if (it->second->m_max_concurrency == 0) {
        it->second->m_block_query_num.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      /*
        We find a sql filter rule that matches the query. In case of
        concurrently updating the m_cur_concur value of sql filter rule,
        we first atomically load this value, and then determine whether limit
        the query by comparing with m_max_concurrency. If this value is greater
        than or equal to the m_max_concurrency, then we should limit the query.
        Otherwise we update the m_cur_concur value. Note that, in order to
        prevent multiple threads from updating the m_cur_concur value at the
        same time, we use "compare_exchange_strong" rather than fetch_add(1).
      */
      ulong store_val =
          (ulong)it->second->m_cur_concur.load(std::memory_order_acquire);
      do {
        if (store_val >= (ulong)it->second->m_max_concurrency) {
          it->second->m_block_query_num.fetch_add(1, std::memory_order_relaxed);
          return true;
        }
      } while (!it->second->m_cur_concur.compare_exchange_weak(
          store_val, store_val + 1, std::memory_order_release,
          std::memory_order_relaxed));

      DEBUG_SYNC_C("sql_filter_read_find_matched_filter_and_update");

      thd->filter_id = it->second->m_id;

      return false;
    }
  }

  return false;
}

/**
  Check if current statement should be limited by the sql filter.

  @retval true if it is limited and otherwise return false
*/
bool System_sql_filter::limit_query_by_sqlfilter(THD *thd) {
  DBUG_ASSERT(nullptr != thd);

  auto filter_type = obtain_filter_type(thd->lex->sql_command);
  if (filter_type == SqlFilterType::LAST) return false;

  /* The reserved account should not be restricted. */
  if ((nullptr != thd->security_context()->user().str) &&
      (nullptr != thd->security_context()->host_or_ip().str) &&
      (is_rds_reserved_user(thd->security_context()->user().str) ||
       (!rds_sqlfilter_affect_admin_user &&
        !strcmp(thd->security_context()->user().str, RDS_ROOT_USER))))
    return false;

  if (!rds_sqlfilter_sys_table_control) {
    /* When rds_sqlfilter_sys_table_control set to off,
       No need to traffic control when the object of query is system table. */
    const Table_ref *tables = thd->lex->query_block->get_table_list();
    if (tables == nullptr || tables->db == nullptr) return false;
    while (tables && tables->db) {
      if (is_mysql_db(tables->db) || is_infoschema_db(tables->db) ||
          is_perfschema_db(tables->db) || is_sys_db(tables->db))
        return false;
      tables = tables->next_local;
    }
  }

  return find_matched_filter_and_update(thd, filter_type);
}

void System_sql_filter::dec_filter_item_conc(THD *thd) {
  DBUG_ASSERT(nullptr != thd);
  /* Valid sql flter item id starting from 1, never be 0. */
  DBUG_ASSERT(thd->filter_id != 0);
  DEBUG_SYNC(thd, "before_reduce_sqlfilter_item_concurrency");

  auto filter_type = obtain_filter_type(thd->lex->sql_command);
  if (filter_type != SqlFilterType::LAST) {
    size_t type = static_cast<size_t>(filter_type);
    Lock_helper lock(m_lock[type], false);
    DBUG_ASSERT(lock.effect());
    DEBUG_SYNC_C("dec_sql_filter_after_map_lock");

    auto it = m_rule_map[type]->find(thd->filter_id);
    // dbms_sqlfilter.flush_sql_filter() will set m_cur_concur to zero, in
    // high-concurrency scenarios, m_cur_concur may be reduced to ULONG_MAX.
    if (it != m_rule_map[type]->end()) {
      auto &concur = it->second->m_cur_concur;
      auto store_val = concur.load(std::memory_order_relaxed);
      while (store_val > 0) {
        if (concur.compare_exchange_weak(store_val, store_val - 1,
                                         std::memory_order_release,
                                         std::memory_order_relaxed)) {
          break;
        }
      }
    }
  }

  thd->filter_id = 0;
}

size_t System_sql_filter::map_size() {
  size_t size = 0;
  for (size_t i = 0; i < static_cast<size_t>(SqlFilterType::LAST); ++i) {
    size += m_rule_map[i]->size();
  }
  return size;
}
