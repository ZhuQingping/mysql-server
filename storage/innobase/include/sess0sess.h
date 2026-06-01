/*****************************************************************************

Copyright (c) 2013, 2024, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/sess0sess.h
 InnoDB session state tracker.
 Multi file, shared, system tablespace implementation.

 Created 2014-04-30 by Krunal Bauskar
 *******************************************************/

#ifndef sess0sess_h
#define sess0sess_h

#include <sql_thd_internal_api.h>
#include "dict0mem.h"
#include "log0meb.h"
#include "srv0tmp.h"
#include "trx0trx.h"
#include "univ.i"
#include "ut0new.h"

#include <map>

class dict_intrinsic_table_t {
 public:
  /** Constructor
  @param[in,out]        handler         table handler. */
  dict_intrinsic_table_t(dict_table_t *handler) : m_handler(handler) {
    /* Do nothing. */
  }

  /** Destructor */
  ~dict_intrinsic_table_t() { m_handler = nullptr; }

 public:
  /* Table Handler holding other metadata information commonly needed
  for any table. */
  dict_table_t *m_handler;
};

/** InnoDB private data that is cached in THD */
typedef std::map<
    std::string, dict_intrinsic_table_t *, std::less<std::string>,
    ut::allocator<std::pair<const std::string, dict_intrinsic_table_t *>>>
    table_cache_t;

class innodb_session_t {
 public:
  /** Constructor */
  innodb_session_t()
      : m_trx(),
        m_open_tables(),
        m_index(),
        m_clustered_index(),
        m_usr_temp_tblsp(),
        m_intrinsic_temp_tblsp() {
    /* Do nothing. */
  }

  /** Destructor */
  ~innodb_session_t() {
    m_trx = nullptr;
    m_index = nullptr;
    m_clustered_index = nullptr;

    for (table_cache_t::iterator it = m_open_tables.begin();
         it != m_open_tables.end(); ++it) {
      delete (it->second);
    }

    meb::redo_log_archive_session_end(this);

    if (m_usr_temp_tblsp != nullptr) {
      ibt::free_tmp(m_usr_temp_tblsp);
    }

    if (m_intrinsic_temp_tblsp != nullptr) {
      ibt::free_tmp(m_intrinsic_temp_tblsp);
    }
  }

  /** Cache table handler.
  @param[in]    table_name      name of the table
  @param[in,out]        table           table handler to register */
  void register_table_handler(const char *table_name, dict_table_t *table) {
    ut_ad(lookup_table_handler(table_name) == nullptr);
    m_open_tables.insert(table_cache_t::value_type(
        table_name, new dict_intrinsic_table_t(table)));
  }

  /** Lookup for table handler given table_name.
  @param[in]    table_name      name of the table to lookup */
  dict_table_t *lookup_table_handler(const char *table_name) {
    table_cache_t::iterator it = m_open_tables.find(table_name);
    return ((it == m_open_tables.end()) ? nullptr : it->second->m_handler);
  }

  /** Remove table handler entry.
  @param[in]    table_name      name of the table to remove */
  void unregister_table_handler(const char *table_name) {
    table_cache_t::iterator it = m_open_tables.find(table_name);
    if (it == m_open_tables.end()) {
      return;
    }

    delete (it->second);
    m_open_tables.erase(table_name);
  }

  /** Count of register table handler.
  @return number of register table handlers */
  uint count_register_table_handler() const {
    return (static_cast<uint>(m_open_tables.size()));
  }

  /** Register the active index and its related clustered index
  @param[in]	index		index in active, local one
  @param[in]	clustered_index	related clustered index, local one */
  void register_active_indexes(dict_index_t *index,
                               dict_index_t *clustered_index) {
    m_index = index;
    m_clustered_index = clustered_index;
  }

  /** Unregister the active index and its related clustered index */
  void unregister_active_indexes() {
    m_index = nullptr;
    m_clustered_index = nullptr;
  }

  /** Get the active index
  @param[in]	is_clustered	true to get clustered index,
                                otherwie, the active index
  @return the active index to get */
  dict_index_t *get_active_index(bool is_clustered) const {
    return is_clustered ? m_clustered_index : m_index;
  }

  ibt::Tablespace *get_usr_temp_tblsp() {
    if (m_usr_temp_tblsp == nullptr) {
      my_thread_id id = thd_thread_id(m_trx->mysql_thd);
      m_usr_temp_tblsp = ibt::tbsp_pool->get(id, ibt::TBSP_USER);
    }

    return (m_usr_temp_tblsp);
  }

  ibt::Tablespace *get_instrinsic_temp_tblsp() {
    if (m_intrinsic_temp_tblsp == nullptr) {
      my_thread_id id = thd_thread_id(m_trx->mysql_thd);
      m_intrinsic_temp_tblsp = ibt::tbsp_pool->get(id, ibt::TBSP_INTRINSIC);
    }

    return (m_intrinsic_temp_tblsp);
  }

 public:
  /** transaction handler. */
  trx_t *m_trx;

  /** Handler of tables that are created or open but not added
  to InnoDB dictionary as they are session specific.
  Currently, limited to intrinsic temporary tables only. */
  table_cache_t m_open_tables;

 private:
  /** There is an optimization for intrinsic table whose index record is of
  fixed length, the dict_index_t::rec_cache is used to store the unchanged
  rec_size, offsets, etc. It is accessible like this:

  ha_innobase -> m_prebuilt -> index -> rec_cache

  This is based on the assumption that an intrinsic table can only be accessed
  by a single thread. After PQ is supported, an intrinsic table could be
  accessed by several worker threads, and each worker thread, using the same
  dict_table_t ('table' pointer above) as the leader thread (@see the comment
  of ha_innobase::m_old_share), would use the same rec_cache_t, and there
  would be a race on the offsets of the rec_cache_t, etc. And this happens
  when ha_innobase::is_sharing_data() is true.

  So, with the assumption that the leader thread of PQ will create the main
  dict_table_t table for DML and SCAN, and the other workers will all create
  duplicate local tables but not use them actually to read or write, the
  solution is that each worker should leverage the record cache available in
  its own duplicate dict_table_t, rather than the one created by the leader
  thread. To reach to its own rec_cache, it must access the 'index' object
  connected to its duplicate dict_table_t (available in 'm_old_table'). The
  module which knows what this 'index' is, is responsible for calling
  register_active_indexes() which will store the active index and its related
  clustered index in the members below.

  The user of this who is populate_offsets(), should check if the active index
  is nullptr or not, if nullptr, simply use the original index which it got
  passed as argument, which must be already a local one. */

  /** Current index in use */
  dict_index_t *m_index;

  /** The clustered index related to m_index. */
  dict_index_t *m_clustered_index;

  /** Current session's user temp tablespace */
  ibt::Tablespace *m_usr_temp_tblsp;

  /** Current session's optimizer temp tablespace */
  ibt::Tablespace *m_intrinsic_temp_tblsp;
};

/// RAII class to wrap innodb_session_t::[un]register_active_indexes()
class RegisterActiveIndexes {
 public:
  /**
     @param[in,out] session  InnoDB session to register to
     @param[in] index    New active index
     @note if 'index' is nullptr this object does nothing.
  */
  RegisterActiveIndexes(innodb_session_t *session, dict_index_t *index) {
    if (index) {
      m_session = session;
      m_session->register_active_indexes(index, index->table->first_index());
    } else {
      m_session = nullptr;  // let us be a dummy object
    }
  }
  ~RegisterActiveIndexes() {
    if (m_session) {
      m_session->unregister_active_indexes();
    }
  }

 private:
  innodb_session_t *m_session;
};

#endif /* sess0sess_h */
