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

#ifndef SQL_SQL_FILTER_CACHE_INCLUDED
#define SQL_SQL_FILTER_CACHE_INCLUDED

#include <boost/algorithm/string.hpp>
#include "sql/common/table_common.h"
#include "sql/sql_filter/sql_filter_common.h"
#include "sql/sql_filter/sql_filter_table_common.h"

const int MAX_NODE_ID = 36;

class Sql_filter {
 public:
  explicit Sql_filter(Sql_filter_record *record)
      : m_id(record->get_id()),
        m_type(record->m_type),
        m_max_concurrency(record->m_max_concurrency),
        m_key_str(record->m_key_str.str ? record->m_key_str.str : ""),
        m_node_id(record->m_node_id.str ? record->m_node_id.str : "") {
    m_cur_concur.store(record->m_cur_concur);
    m_block_query_num.store(record->m_block_query_num);
    std::vector<std::string> keywords;
    boost::split(keywords, record->m_key_str.str, boost::is_any_of("~"));
    for (auto &key : keywords) trim_str(key);
    m_key_array = keywords;
  }

  virtual ~Sql_filter() {}

  Sqlfilter_update_error update_sqlfilter_rule(const Sql_filter_record *record);

  ulonglong m_id;
  SqlFilterType m_type;
  ulonglong m_max_concurrency;
  String_sqlfilter m_key_str;
  String_sqlfilter m_node_id;

  std::atomic<ulong> m_cur_concur;
  std::atomic<ulong> m_block_query_num;
  std::vector<std::string> m_key_array;
};

/* Sql filter show result structure */
class Sql_filter_show_result {
 public:
  explicit Sql_filter_show_result(const Sql_filter *filter)
      : m_id(filter->m_id),
        m_type(filter->m_type),
        m_max_concurrency(filter->m_max_concurrency),
        m_key_str(filter->m_key_str),
        m_node_id(filter->m_node_id),
        m_key_array(filter->m_key_array) {
    m_cur_concur.store(filter->m_cur_concur);
    m_block_query_num.store(filter->m_block_query_num);
  }

  explicit Sql_filter_show_result(const Sql_filter_show_result &other)
      : m_id(other.m_id),
        m_type(other.m_type),
        m_max_concurrency(other.m_max_concurrency),
        m_key_str(other.m_key_str),
        m_node_id(other.m_node_id),
        m_key_array(other.m_key_array) {
    m_cur_concur.store(other.m_cur_concur);
    m_block_query_num.store(other.m_block_query_num);
  }

  ulonglong m_id;
  SqlFilterType m_type;
  ulonglong m_max_concurrency;
  String_sqlfilter m_key_str;
  String_sqlfilter m_node_id;

  std::atomic<ulong> m_cur_concur;
  std::atomic<ulong> m_block_query_num;
  std::vector<std::string> m_key_array;
};

using Sqlfilter_show_result_container = std::vector<Sql_filter_show_result>;

class System_sql_filter {
  using Sql_filter_map = std::map<ulonglong, Sql_filter *>;

  /* Lock helper class */
  class Lock_helper : public Disable_unnamed_object {
   public:
    explicit Lock_helper(mysql_rwlock_t *lock, bool exclusive)
        : m_locked(false), m_lock(lock) {
      if (exclusive)
        mysql_rwlock_wrlock(m_lock);
      else
        mysql_rwlock_rdlock(m_lock);

      m_locked = true;
    }

    void unlock() {
      mysql_rwlock_unlock(m_lock);
      m_locked = false;
    }

    ~Lock_helper() {
      if (m_locked) mysql_rwlock_unlock(m_lock);
    }

   private:
    bool m_locked;
    mysql_rwlock_t *m_lock;
  };

 public:
  explicit System_sql_filter();

  virtual ~System_sql_filter();

  /**
    Fill one sql filter into maps

    @param[in]      records       sql filter records container.
  */
  void add_records(Conf_records *records);

  /**
    flush all sql filters into maps

    @param[in]      records       sql filter records container.
  */
  void flush_records(Conf_records *records);

  /**
    Delete the sql filter from map by id

    @param[in]        sql filter id
  */
  bool delete_sql_filter(ulonglong id);

  /**
    update one sql filter into map

    @param[in]      id            sql filter id
    @param[in]      records       sql filter record container

    @returns        update sql filer error code
  */
  Sqlfilter_update_error update_sql_filter(ulonglong id, Conf_record *record);

  /* The system sql filter singleton instance */
  static System_sql_filter *m_system_sql_filter;
  static System_sql_filter *instance() { return m_system_sql_filter; }

  size_t map_size();
  bool find_matched_filter_and_update(THD *thd, SqlFilterType type);
  bool limit_query_by_sqlfilter(THD *thd);

  void dec_filter_item_conc(THD *thd);

  void aggregate_sql_filters(Sqlfilter_show_result_container *container);

 private:
  /**
    Clear the sql filter objects from m_rule_map.
  */
  void clear(size_t type);

 private:
  Sql_filter_map *m_rule_map[static_cast<size_t>(SqlFilterType::LAST)];
  mysql_rwlock_t *m_lock[static_cast<size_t>(SqlFilterType::LAST)];
};

#endif
