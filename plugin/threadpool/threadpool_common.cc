/* Copyright (C) 2012 Monty Program Ab
   Copyright (c) 2019, 2021, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
   USA */
#define MYSQL_SERVER 1

#include "my_thread_local.h"
#include "mysql/plugin.h"
#include "mysql/psi/mysql_idle.h"
#include "mysql/psi/mysql_socket.h"
#include "mysql/thread_pool_priv.h"
#include "sql/conn_handler/channel_info.h"
#include "sql/conn_handler/connection_handler_manager.h"
#include "sql/debug_sync.h"
#include "sql/mysqld.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/protocol_classic.h"
#include "sql/sql_audit.h"
#include "sql/sql_class.h"
#include "sql/sql_connect.h"
#include "sql/sql_parse.h"
#include "threadpool.h"
#include "violite.h"
/** Threadpool parameters */

uint threadpool_idle_timeout;
uint threadpool_size;
uint threadpool_stall_limit;
uint threadpool_max_threads;
uint threadpool_oversubscribe;
uint threadpool_high_prio_tickets;
uint threadpool_prio_kickup_timer;
ulong threadpool_schedule_mode;
bool threadpool_enabled;
bool threadpool_enable_active_threads_check;
bool threadpool_running = false;
bool threadpool_exact_stats;
bool threadpool_dedicated_listener;
char *threadpool_cpubind_info = nullptr;

/** Stats */
TP_STATISTICS g_tp_stats;

/**
  Worker threads contexts, and THD contexts.
  =========================================

  Both worker threads and connections have their sets of thread local variables
  At the moment it is mysys_var (this has specific data for dbug, my_error and
  similar goodies), and PSI per-client structure.

  Whenever query is executed following needs to be done:

  1. Save worker thread context.
  2. Change TLS variables to connection specific ones using thread_attach(THD*).
     This function does some additional work.
  3. Process query
  4. Restore worker thread context.

  Connection login and termination follows similar schema w.r.t saving and
  restoring contexts.

  For both worker thread, and for the connection, mysys variables are created
  using my_thread_init() and freed with my_thread_end().

*/
class Worker_thread_context {
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_thread *const psi_thread;
#endif
#ifndef NDEBUG
  const my_thread_id thread_id;
#endif
 public:
  Worker_thread_context() noexcept
      :
#ifdef HAVE_PSI_THREAD_INTERFACE
        psi_thread(PSI_THREAD_CALL(get_thread)())
#endif
#ifndef NDEBUG
        ,
        thread_id(my_thread_var_id())
#endif
  {
  }

  ~Worker_thread_context() noexcept {
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_THREAD_CALL(set_thread)(psi_thread);
#endif
#ifndef NDEBUG
    set_my_thread_var_id(thread_id);
#endif
    THR_MALLOC = nullptr;
  }
};

/**
Attach/associate the connection with the OS thread,

@param[in]      thd current thd

@return true if success, false otherwise
*/
static bool thread_attach(THD *thd) {
#ifndef NDEBUG
  set_my_thread_var_id(thd->thread_id());
#endif
  thd->thread_stack = (char *)&thd;
  thd->store_globals();
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(set_thread)(thd->get_psi());
#endif
  mysql_socket_set_thread_owner(
      thd->get_protocol_classic()->get_vio()->mysql_socket);
  return 0;
}

#ifdef HAVE_PSI_STATEMENT_INTERFACE
extern PSI_statement_info stmt_info_new_packet;
#endif

/**
callback for notifing net status

@param[in]      net net be notified
@param[in]      user_data user private data
@param[in]      count event count
*/
static void threadpool_net_before_header_psi_noop(NET * /* net */,
                                                  void * /* user_data */,
                                                  size_t /* count */) {}

/**
Init net server extension

@param[in]      thd current thd
*/
static void threadpool_init_net_server_extension(THD *thd) {
#ifdef HAVE_PSI_INTERFACE
  // socket_connection.cc:init_net_server_extension should have been called
  // already for us. We only need to overwrite the "before" callback
  assert(thd->m_net_server_extension.m_user_data == thd);
  thd->m_net_server_extension.m_before_header =
      threadpool_net_before_header_psi_noop;
#else
  assert(thd->get_protocol_classic()->get_net()->extension == NULL);
#endif
}

/**
Process login in worker or listener thread

@param[in]      thd current thd

@return 0 if success, nonzero otherwise
*/
int threadpool_add_connection(THD *thd) {
  int retval = 1;
  Worker_thread_context worker_context;

  my_thread_init();

  /* Create new PSI thread for use with the THD. */
#ifdef HAVE_PSI_THREAD_INTERFACE
  thd->set_psi(PSI_THREAD_CALL(new_thread)(key_thread_one_connection, 0, thd,
                                           thd->thread_id()));
#endif

  /* Login. */
  thread_attach(thd);
  thd->start_utime = my_micro_time();

  thd->store_globals();

  if (thd_prepare_connection(
          thd)) {  // TODO thd_prepare_connection(thd,false) find out why
                   // percona check extra_port_connection
    goto end;
  }

  /*
    Check if THD is ok, as prepare_new_connection_state()
    can fail, for example if init command failed.
  */
  if (thd_connection_alive(thd)) {
    retval = 0;
    thd_set_net_read_write(thd, 1);
    if (threadpool_schedule_mode == TP_SCHEDULE_MODE_STATEMENT) {
      thd->skip_wait_timeout = true;
    } else {
      thd->skip_wait_timeout = false;
    }

    MYSQL_SOCKET_SET_STATE(thd->get_protocol_classic()->get_vio()->mysql_socket,
                           PSI_SOCKET_STATE_IDLE);
    thd->m_server_idle = true;
    threadpool_init_net_server_extension(thd);
  }

end:
  if (retval) {
    Connection_handler_manager *handler_manager =
        Connection_handler_manager::get_instance();
    handler_manager->inc_aborted_connects();
  }
  return retval;
}

/**
Close user connection

@param[in]      thd current thd
*/
void threadpool_remove_connection(THD *thd) {
  Worker_thread_context worker_context;
  thread_attach(thd);
  thd_set_net_read_write(thd, 0);

  end_connection(thd);
  close_connection(thd, 0, false, false);

  thd->release_resources();

#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(delete_thread)(thd->get_psi());
#endif

  Global_THD_manager::get_instance()->remove_thd(thd);

  Connection_handler_manager::dec_connection_count(
      thd->is_admin_connection(), thd->is_reserved_user_session());
  delete thd;
}

/**
Checking whether the current thread can be scheduled out.

​​For the DSTORE engine​​: Since transaction information, locks, and
some state information are stored in thread-local variables, it's necessary to
ensure these statements run in the same thread. Therefore, if any of the
following conditions are met, the current thread ​​cannot​​ be scheduled
out: The thread is in a multi-statement transaction, It holds locks, or It is
executing a HANDLER statement. In all other cases, the thread can be scheduled
out.

​​For non-DSTORE engines​​: If the current scheduling mode is
TP_SCHEDULE_MODE_TRANSACTION, the thread ​​cannot​​ be scheduled out
when in a multi-statement transaction. In all other states, it can be scheduled
out.

​​Definitions:​​
​​Multi-statement transaction​​:
  BEGIN; ...statements... COMMIT; or
  SET autocommit=0; SELECT * FROM table; ... COMMIT;
​​Holding locks​​:
  FLUSH TABLES WITH READ LOCK;
  ...statements...
  UNLOCK TABLES;
​​Executing a HANDLER statement​​:
  HANDLER xx_table OPEN;
  ...
  HANDLER xx_table READ;
  ...
  HANDLER xx_table CLOSE;

@param[in]      thd current thd
@return   true     can process req of other  connections,
          false    still need to process the req of current
                   connecton.
*/
bool can_schedule_out(THD *thd) {
  if (get_instance_storage_engine_type() == DB_TYPE_DSTORE) {
    if (thd_in_active_multi_stmt_transaction(thd) ||
        !thd->handler_tables_hash.empty() || thd->mdl_context.has_locks()) {
      return false;
    }
    return true;
  } else {
    if (threadpool_schedule_mode == TP_SCHEDULE_MODE_STATEMENT) {
      return true;
    }
    return !thd_in_active_multi_stmt_transaction(thd);
  }
}
/**
Process a single client request or a single batch.

@param[in]      thd current thd

@return 0 if success, nonzero otherwise
*/
int threadpool_process_request(THD *thd) {
  int retval = 0;
  Worker_thread_context worker_context;

  thread_attach(thd);

  if (thd->killed == THD::KILL_CONNECTION) {
    /*
      killed flag was set by timeout handler
      or KILL command. Return error.
    */
    retval = 1;
    goto end;
  }

  /*
    In the loop below, the flow is essentially the copy of thead-per-connections
    logic, see do_handle_one_connection() in sql_connect.c

    The goal is to execute a single query, thus the loop is normally executed
    only once. However for SSL connections, it can be executed multiple times
    (SSL can preread and cache incoming data, and vio->has_data() checks if it
    was the case).
  */
  for (;;) {
    Vio *vio;
    thd_set_net_read_write(thd, 0);

    if ((retval = do_command(thd)) != 0 && can_schedule_out(thd)) goto end;

    if (!thd_connection_alive(thd)) {
      retval = 1;
      goto end;
    }

    vio = thd->get_protocol_classic()->get_vio();
    if (!vio->has_data(vio) && can_schedule_out(thd)) {
      /* More info on this debug sync is in sql_parse.cc*/
      DEBUG_SYNC(thd, "before_do_command_net_read");
      thd_set_net_read_write(thd, 1);
      goto end;
    }

    /** Idle, waiting for the next command. */
    if (!thd->m_server_idle) {
      MYSQL_SOCKET_SET_STATE(vio->mysql_socket, PSI_SOCKET_STATE_IDLE);
      MYSQL_START_IDLE_WAIT(thd->m_idle_psi, &thd->m_idle_state);
      thd->m_server_idle = true;
    }
  }

end:
  if (!retval && !thd->m_server_idle) {
    MYSQL_SOCKET_SET_STATE(thd->get_protocol_classic()->get_vio()->mysql_socket,
                           PSI_SOCKET_STATE_IDLE);
    MYSQL_START_IDLE_WAIT(thd->m_idle_psi, &thd->m_idle_state);
    thd->m_server_idle = true;
  }

  return retval;
}
