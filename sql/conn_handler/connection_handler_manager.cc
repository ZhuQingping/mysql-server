/*
   Copyright (c) 2013, 2024, Oracle and/or its affiliates.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "sql/conn_handler/connection_handler_manager.h"

#include <assert.h>
#include <ctime>
#include <new>

#include "my_macros.h"
#include "my_psi_config.h"
#include "my_sys.h"
#include "mysql/components/services/bits/mysql_cond_bits.h"
#include "mysql/components/services/bits/mysql_mutex_bits.h"
#include "mysql/components/services/bits/psi_bits.h"
#include "mysql/components/services/bits/psi_cond_bits.h"
#include "mysql/components/services/bits/psi_mutex_bits.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/service_thd_wait.h"
#include "mysqld_error.h"  // ER_*
#include "sql/auth/auth_acls.h"
#include "sql/conn_handler/channel_info.h"             // Channel_info
#include "sql/conn_handler/connection_handler_impl.h"  // Per_thread_connection_handler
#include "sql/conn_handler/plugin_connection_handler.h"  // Plugin_connection_handler
#include "sql/current_thd.h"
#include "sql/log.h"
#include "sql/mysqld.h"        // max_connections
#include "sql/sql_callback.h"  // MYSQL_CALLBACK
#include "sql/sql_class.h"     //THD
#include "thr_lock.h"
#include "thr_mutex.h"

struct Connection_handler_functions;

// Initialize static members
uint Connection_handler_manager::connection_count = 0;
// current connections count from admin port
uint Connection_handler_manager::rds_admin_connection_count = 0;
// current connections count from reserved users
uint Connection_handler_manager::rds_reserved_connection_count = 0;
ulong Connection_handler_manager::max_used_connections = 0;
ulong Connection_handler_manager::max_used_connections_time = 0;
mysql_mutex_t Connection_handler_manager::LOCK_connection_count;
mysql_cond_t Connection_handler_manager::COND_connection_count;
Connection_handler_manager *Connection_handler_manager::m_instance = nullptr;
ulong Connection_handler_manager::thread_handling =
    SCHEDULER_ONE_THREAD_PER_CONNECTION;
uint Connection_handler_manager::max_threads = 0;

namespace {
/**
  current connecion's user type.
*/
enum class Conn_user_t : uint8_t {
  /*
    Not rds reserved user, not admin user, and without CONNTION_ADMIN
    and SERVICE_CONNECTION_ADMIN priveliges.
   */
  CONN_MYSQL_ORDINARY_USER = 0,

  /*
    user with CONNTION_ADMIN priveliges, or with
    SERVICE_CONNECTION_ADMIN but connect to database
    not through admin port.
  */
  CONN_MYSQL_CONN_ADMIN_USER = 1,

  /*
    User with SERVICE_CONNECTION_ADMIN priveliges, and
    connect to database through admin port.
  */
  CONN_MYSQL_SERVICE_ADMIN_USER = 2,

  /*
    rds reserved user.
  */
  CONN_RDS_RESERVED_USER = 3,

  CONN_INVALID_USER = 4
};

Conn_user_t get_thd_conn_user(THD *thd) {
  assert(thd != nullptr);
  assert(current_thd != nullptr);

  if (thd->is_admin_connection()) {
    return Conn_user_t::CONN_MYSQL_SERVICE_ADMIN_USER;
  } else if (thd->is_reserved_user_session()) {
    return Conn_user_t::CONN_RDS_RESERVED_USER;
  } else if (thd->m_main_security_ctx.check_access(SUPER_ACL) ||
             thd->m_main_security_ctx
                 .has_global_grant(STRING_WITH_LEN("CONNECTION_ADMIN"))
                 .first ||
             thd->m_main_security_ctx
                 .has_global_grant(STRING_WITH_LEN("SERVICE_CONNECTION_ADMIN"))
                 .first) {
    return Conn_user_t::CONN_MYSQL_CONN_ADMIN_USER;
  }

  return Conn_user_t::CONN_MYSQL_ORDINARY_USER;
}
}  // namespace

/**
  Helper functions to allow mysys to call the thread scheduler when
  waiting for locks.
*/

static void scheduler_wait_lock_begin() {
  THD *thd = current_thd;
  if (likely(thd))
    MYSQL_CALLBACK(thd->conn_event_functions, thd_wait_begin,
                   (thd, THD_WAIT_TABLE_LOCK));
}

static void scheduler_wait_lock_end() {
  THD *thd = current_thd;
  if (likely(thd))
    MYSQL_CALLBACK(thd->conn_event_functions, thd_wait_end, (thd));
}

static void scheduler_wait_sync_begin() {
  THD *thd = current_thd;
  if (likely(thd))
    MYSQL_CALLBACK(thd->conn_event_functions, thd_wait_begin,
                   (thd, THD_WAIT_SYNC));
}

static void scheduler_wait_sync_end() {
  THD *thd = current_thd;
  if (likely(thd))
    MYSQL_CALLBACK(thd->conn_event_functions, thd_wait_end, (thd));
}

bool Connection_handler_manager::valid_connection_count(THD *thd) {
  bool connection_accepted = true;
  mysql_mutex_lock(&LOCK_connection_count);

  switch (get_thd_conn_user(thd)) {
    /*
      For MySQL's user, SERVICE_CONNECTION_ADMIN user's connections will
      affect SUPER user, ordinary user.
    */
    case Conn_user_t::CONN_MYSQL_ORDINARY_USER:
      if (connection_count > (max_connections + rds_reserved_connection_count +
                              rds_admin_connection_count)) {
        connection_accepted = false;
        m_connection_errors_max_connection++;
      }
      break;

    case Conn_user_t::CONN_MYSQL_CONN_ADMIN_USER:
      if (connection_count > (max_connections + rds_reserved_connection_count +
                              rds_admin_connection_count + 1)) {
        connection_accepted = false;
        m_connection_errors_max_connection++;
      }
      break;

    case Conn_user_t::CONN_MYSQL_SERVICE_ADMIN_USER:
      if (rds_admin_connection_count > opt_rds_admin_max_connections) {
        connection_accepted = false;
        m_connection_errors_max_connection++;
      }
      break;

    case Conn_user_t::CONN_RDS_RESERVED_USER:
      if (rds_reserved_connection_count > max_rds_reserved_connections) {
        connection_accepted = false;
        m_connection_errors_max_connection++;
      }
      break;
    default:
      assert(false);
  }

  if (connection_accepted == false) {
    auto sctx = thd->security_context();
    conn_info_print(
        ConnectLogType::ABNORMAL,
        "Too many connections, valid_connection_count check fail, "
        "user: '%-.48s'@'%-.64s', connection_count: %d, "
        "rds_admin_connection_count: %d, rds_reserved_connection_count: %d, "
        "max_connections: %ld, max_rds_reserved_connections: %ld, "
        "opt_rds_admin_max_connections: %ld.",
        sctx->user().str, sctx->host_or_ip().str, connection_count,
        rds_admin_connection_count, rds_reserved_connection_count,
        max_connections, max_rds_reserved_connections,
        opt_rds_admin_max_connections);
  }

  if (connection_accepted && connection_count > max_used_connections) {
    max_used_connections = connection_count;
    max_used_connections_time = time(nullptr);
  }

  mysql_mutex_unlock(&LOCK_connection_count);
  return connection_accepted;
}

bool Connection_handler_manager::check_and_incr_conn_count(
    bool ignore_max_connection_count, bool is_admin_connection) {
  bool connection_accepted = true;
  mysql_mutex_lock(&LOCK_connection_count);
  /*
    If ignore_max_connection_count is true(such as for MGR), we do not
    check connection count.

    Here we allow max_connections + max_rds_reserved_connections + 1 no admin
    clients to connect (by checking before we increment by 1).

    The last connection is reserved for SUPER users. This is
    checked later during authentication where valid_connection_count(THD *thd)
    is called for non-SUPER users only.

    And for admin port connection, we allow opt_rds_admin_max_connections
    clients to connect.
  */
  if (!ignore_max_connection_count &&
      ((!is_admin_connection &&
        connection_count > (max_connections + max_rds_reserved_connections +
                            rds_admin_connection_count)) ||
       (is_admin_connection &&
        rds_admin_connection_count >= opt_rds_admin_max_connections))) {
    connection_accepted = false;
    m_connection_errors_max_connection++;
    conn_info_print(ConnectLogType::ABNORMAL,
                    "Too many connections, check_and_incr_conn_count fail, "
                    "is_admin_connection: %d, connection_count: %d, "
                    "rds_admin_connection_count: %d, max_connections: %ld, "
                    "max_rds_reserved_connections: %ld, "
                    "opt_rds_admin_max_connections: %ld.",
                    is_admin_connection, connection_count,

                    rds_admin_connection_count, max_connections,
                    max_rds_reserved_connections,
                    opt_rds_admin_max_connections);
  } else {
    ++connection_count;
    if (is_admin_connection) ++rds_admin_connection_count;
    if (connection_count > max_used_connections) {
      max_used_connections = connection_count;
      max_used_connections_time = time(nullptr);
    }
  }
  mysql_mutex_unlock(&LOCK_connection_count);
  return connection_accepted;
}

bool Connection_handler_manager::check_and_incr_reserved_conn_count(
    THD *rds_reserved_thd) {
  assert(rds_reserved_thd != nullptr);
  bool connection_accepted = true;
  mysql_mutex_lock(&LOCK_connection_count);

  /*
    This function was called after the connection has been process after
    check_and_incr_conn_count, so we only add rds_reserved_connection_count.
    If we do following work in check_and_incr_conn_count, we must handshake
    with client and parse the raw network packet in advance (more detail see
    server_mpvio_update_thd). However, check_and_incr_conn_count is very
    simplely check, after thd was updated by mpvio, we will validate the
    connection again.
  */
  if (rds_reserved_connection_count >= max_rds_reserved_connections) {
    connection_accepted = false;
    m_connection_errors_max_connection++;

    auto sctx = rds_reserved_thd->security_context();
    conn_info_print(
        ConnectLogType::ABNORMAL,
        "Too many connections, check_and_incr_reserved_conn_count fail, "
        "user: '%-.48s'@'%-.64s', rds_reserved_connection_count: %d, "
        "max_rds_reserved_connections: %ld.",
        sctx->user().str, sctx->host_or_ip().str, rds_reserved_connection_count,
        max_rds_reserved_connections);
  } else {
    ++rds_reserved_connection_count;
    rds_reserved_thd->mark_as_reserved_user_session();
  }

  mysql_mutex_unlock(&LOCK_connection_count);
  return connection_accepted;
}

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_connection_count;

static PSI_mutex_info all_conn_manager_mutexes[] = {
    {&key_LOCK_connection_count, "LOCK_connection_count", PSI_FLAG_SINGLETON, 0,
     PSI_DOCUMENT_ME}};

static PSI_cond_key key_COND_connection_count;

static PSI_cond_info all_conn_manager_conds[] = {
    {&key_COND_connection_count, "COND_connection_count", PSI_FLAG_SINGLETON, 0,
     PSI_DOCUMENT_ME}};
#endif

bool Connection_handler_manager::init() {
  /*
    This is a static member function.
    Per_thread_connection_handler's static members need to be initialized
    even if One_thread_connection_handler is used instead.
  */
  Per_thread_connection_handler::init();

  Connection_handler *connection_handler = nullptr;
  switch (Connection_handler_manager::thread_handling) {
    case SCHEDULER_ONE_THREAD_PER_CONNECTION:
      connection_handler = new (std::nothrow) Per_thread_connection_handler();
      break;
    case SCHEDULER_NO_THREADS:
      connection_handler = new (std::nothrow) One_thread_connection_handler();
      break;
    default:
      assert(false);
  }

  if (connection_handler == nullptr) {
    // This is a static member function.
    Per_thread_connection_handler::destroy();
    return true;
  }

  Connection_handler *per_thread_connection_handler =
      new (std::nothrow) Per_thread_connection_handler();

  if (per_thread_connection_handler == nullptr) {
    delete connection_handler;
    // This is a static member function.
    Per_thread_connection_handler::destroy();
    return true;
  }

  m_instance = new (std::nothrow) Connection_handler_manager(
      connection_handler, per_thread_connection_handler);

  if (m_instance == nullptr) {
    delete connection_handler;
    delete per_thread_connection_handler;
    // This is a static member function.
    Per_thread_connection_handler::destroy();
    return true;
  }

#ifdef HAVE_PSI_INTERFACE
  int count = static_cast<int>(array_elements(all_conn_manager_mutexes));
  mysql_mutex_register("sql", all_conn_manager_mutexes, count);

  count = static_cast<int>(array_elements(all_conn_manager_conds));
  mysql_cond_register("sql", all_conn_manager_conds, count);
#endif

  mysql_mutex_init(key_LOCK_connection_count, &LOCK_connection_count,
                   MY_MUTEX_INIT_FAST);

  mysql_cond_init(key_COND_connection_count, &COND_connection_count);

  max_threads = connection_handler->get_max_threads();

  // Init common callback functions.
  thr_set_lock_wait_callback(scheduler_wait_lock_begin,
                             scheduler_wait_lock_end);
  thr_set_sync_wait_callback(scheduler_wait_sync_begin,
                             scheduler_wait_sync_end);
  return false;
}

void Connection_handler_manager::wait_till_no_connection() {
  mysql_mutex_lock(&LOCK_connection_count);
  while (connection_count > 0) {
    LogErr(INFORMATION_LEVEL, ER_WAITING_FOR_NO_CONNECTIONS, connection_count);
    mysql_cond_wait(&COND_connection_count, &LOCK_connection_count);
  }
  mysql_mutex_unlock(&LOCK_connection_count);
}

void Connection_handler_manager::destroy_instance() {
  Per_thread_connection_handler::destroy();

  if (m_instance != nullptr) {
    delete m_instance;
    m_instance = nullptr;
    mysql_mutex_destroy(&LOCK_connection_count);
    mysql_cond_destroy(&COND_connection_count);
  }
}

void Connection_handler_manager::reset_max_used_connections() {
  mysql_mutex_lock(&LOCK_connection_count);
  max_used_connections = connection_count;
  max_used_connections_time = time(nullptr);
  mysql_mutex_unlock(&LOCK_connection_count);
}

void Connection_handler_manager::load_connection_handler(
    Connection_handler *conn_handler) {
  // We don't support loading more than one dynamic connection handler
  assert(Connection_handler_manager::thread_handling != SCHEDULER_TYPES_COUNT);
  m_saved_connection_handler = m_connection_handler;
  m_saved_thread_handling = Connection_handler_manager::thread_handling;
  m_connection_handler = conn_handler;
  Connection_handler_manager::thread_handling = SCHEDULER_TYPES_COUNT;
  max_threads = m_connection_handler->get_max_threads();
}

bool Connection_handler_manager::unload_connection_handler() {
  // in case of disable threadpool plugin first, and then uninstall it
  if (m_saved_connection_handler == NULL) return true;
  Connection_handler *connection_handler_for_delete = m_connection_handler;
  m_connection_handler = m_saved_connection_handler;
  Connection_handler_manager::thread_handling = m_saved_thread_handling;
  m_saved_connection_handler = nullptr;
  m_saved_thread_handling = 0;
  max_threads = m_connection_handler->get_max_threads();
  delete connection_handler_for_delete;
  return false;
}

void Connection_handler_manager::process_new_connection(
    Channel_info *channel_info) {
  if (connection_events_loop_aborted() ||
      !check_and_incr_conn_count(false, channel_info->is_admin_connection())) {
    channel_info->send_error_and_close_channel(ER_CON_COUNT_ERROR, 0, true);
    delete channel_info;
    return;
  }

  Connection_handler *connection_handler =
      ((channel_info->is_admin_connection() &&
        opt_rds_admin_port_using_per_thread) ||
       (channel_info->is_local_socket() &&
        opt_rds_local_socket_using_per_thread))
          ? m_per_thread_connection_handler
          : m_connection_handler;

  if (connection_handler->add_connection(channel_info)) {
    inc_aborted_connects();
    delete channel_info;
  }
}

THD *create_thd(Channel_info *channel_info) {
  THD *thd = channel_info->create_thd();
  if (thd == nullptr)
    channel_info->send_error_and_close_channel(ER_OUT_OF_RESOURCES, 0, false);

  return thd;
}

void destroy_channel_info(Channel_info *channel_info) { delete channel_info; }

void dec_connection_count() {
  Connection_handler_manager::dec_connection_count();
}

void increment_aborted_connects() {
  Connection_handler_manager::get_instance()->inc_aborted_connects();
}

int my_connection_handler_set(Connection_handler_functions *chf) {
  assert(chf != nullptr);
  if (chf == nullptr) return 1;

  Plugin_connection_handler *conn_handler =
      new (std::nothrow) Plugin_connection_handler(chf);
  if (conn_handler == nullptr) return 1;

  Connection_handler_manager::get_instance()->load_connection_handler(
      conn_handler);
  return 0;
}

int my_connection_handler_reset() {
  return Connection_handler_manager::get_instance()
      ->unload_connection_handler();
}
