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
#ifndef THREADPOOL_INCLUDED
#define THREADPOOL_INCLUDED

#include <atomic>

#include <mysql/plugin.h>
#include "my_atomic.h"
#include "sql/conn_handler/connection_handler_manager.h"
#include "sql/sql_list.h"
#include "sql/sql_plist.h"

#define TP_NO_BIND "nobind"
#define TP_CPU_BIND_PREFIX "cpubind"

class TP_atomic_counter {
 private:
  std::atomic<uint64_t> m_val;
  char m_pad[CPU_LEVEL1_DCACHE_LINESIZE - sizeof(std::atomic<uint64_t>)];

 public:
  TP_atomic_counter() : m_val() { memset(&m_pad, 0, sizeof(m_pad)); }
  uint64_t inc() { return m_val.fetch_add(1, std::memory_order_relaxed); }
  operator uint64_t() { return m_val.load(std::memory_order_relaxed); }
  TP_atomic_counter &operator=(uint64_t v) {
    m_val.store(v, std::memory_order_relaxed);
    return *this;
  }
};

struct SHOW_VAR;

#define MAX_THREAD_GROUPS 512

enum tp_high_prio_mode_t {
  TP_HIGH_PRIO_MODE_TRANSACTIONS,
  TP_HIGH_PRIO_MODE_STATEMENTS,
  TP_HIGH_PRIO_MODE_NONE
};

enum tp_schedule_mode_t {
  TP_SCHEDULE_MODE_STATEMENT,
  TP_SCHEDULE_MODE_TRANSACTION
};

/** Threadpool parameters */
/** Shutdown idle worker threads  after this timeout */
extern uint threadpool_idle_timeout;
/** Number of parallel executing threads */
extern uint threadpool_size;
/** time interval in 10 ms units for stall checks*/
extern uint threadpool_stall_limit;
/** Maximum threads in pool */
extern uint threadpool_max_threads;
/** Maximum active threads in group */
extern uint threadpool_oversubscribe;
/** tickets for scheduling */
extern uint threadpool_high_prio_tickets;
/** Time before low prio item gets prio boost */
extern uint threadpool_prio_kickup_timer;
/** worker schedule in statment or transaction*/
extern ulong threadpool_schedule_mode;
/** Better queueing time stats for information_schema, at small
performance cost
*/
extern bool threadpool_exact_stats;
/** Listener thread does not pick up work items. */
extern bool threadpool_dedicated_listener;
extern bool threadpool_enabled;
/** flag for limiting thread oversubscribe*/
extern bool threadpool_enable_active_threads_check;
extern bool threadpool_running;
extern char *threadpool_cpubind_info;

extern ulonglong threadpool_cpu_timer;
extern TP_atomic_counter threadpool_events_handled_cur;

/** Possible values for thread_pool_high_prio_mode */
extern const char *threadpool_high_prio_mode_names[];

/* Common thread pool routines, suitable for different implementations */
extern void threadpool_remove_connection(THD *thd);
extern int threadpool_process_request(THD *thd);
extern int threadpool_add_connection(THD *thd);

/**
Functions used by scheduler.
OS-specific implementations are in
threadpool_unix.cc or threadpool_win.cc
*/

/**
Init the threadpool

@return true if success, false otherwise
*/
extern bool tp_init();

/**
MySQL scheduler callback: wait begin

@param[in]      thd   current thd.
@param[in]      type  the wait type, one of thd_wait_type
*/
extern void tp_wait_begin(THD *thd, int type);

/**
MySQL scheduler callback: wait end

@param[in]      thd   pointer to the THD object.
*/
extern void tp_wait_end(THD *thd);

/**
callback for notify kill operation

@param[in]      thd   current thd.
*/
extern void tp_post_kill_notification(THD *thd) noexcept;

/**
add a new connection the the threadpool

@param[in]      thd   pointer to the THD object.
*/
extern bool add_connection(Channel_info *channel_info);

/**
deinit the threadpool*/
extern void tp_end(void) noexcept;

extern THD_event_functions thd_event_functions;

/**
Calculate number of active connections in the pool.
It goes through the g_all_groups without any global lock.
It is ok here because this when we use the function result,
there will be no new connections come to the threadpool.
In the future, it should be considered whether it is safe to call as-is.

@return the total connection count
*/
extern int tp_get_active_connections();

/**
Sum active threads over all groups.
Don't do any locking, it is not required for stats.

@return the total active thread count
*/
extern int tp_get_active_thread_count();

/**
Calculate number of waiting threads in the pool.
Sum waiting threads over all groups.
Don't do any locking, it is not required for stats.

@return the thread count in waiting state
*/
extern int tp_get_waiting_thread_count();

/**
Sum idle threads over all groups.
Don't do any locking, it is not required for stats.

@return the thread count in idle state
*/
extern int tp_get_idle_thread_count() noexcept;

/**pause the backgoud timer
 */
extern void pause_pool_timer() noexcept;

/**Threadpool statistics
 */
struct TP_STATISTICS {
  /* Current number of worker thread. */
  std::atomic<int32> num_worker_threads;
};

extern TP_STATISTICS g_tp_stats;

/** Functions to set threadpool parameters */

/**
set the max threadgroup size

@param[in]      size   the max threadgroup size.
*/
extern void tp_set_threadpool_size(uint size) noexcept;

/**
set the threadpool stall time threshold

@param[in]      val   the stall time threshold.
*/
extern void tp_set_threadpool_stall_limit(uint val) noexcept;

struct thread_group_t;

/** Per-thread structure for workers */
struct worker_thread_t {
  ulonglong event_count; /* number of request handled by this thread */
  thread_group_t *thread_group;
  worker_thread_t *next_in_list;
  worker_thread_t **prev_in_list;

  mysql_cond_t cond;
  bool woken;
  std::atomic_bool exit;
};

typedef I_P_List<
    worker_thread_t,
    I_P_List_adapter<worker_thread_t, &worker_thread_t::next_in_list,
                     &worker_thread_t::prev_in_list>,
    I_P_List_counter>
    worker_list_t;

struct connection_t {
  typedef uint32 Connection_flags;

  static const Connection_flags CONNECTION_LOGGED_IN = 1;
  static const Connection_flags CONNECTION_BOUND_TO_POOL = 1 << 1;

  THD *thd;
  thread_group_t *thread_group;
  connection_t *next_in_queue;
  connection_t **prev_in_queue;
  ulonglong abs_wait_timeout;
  Connection_flags conn_flags;
  thd_wait_type wait_type;
  int num_nested_waits;
  uint tickets;
  ulonglong enqueue_time;
  ulonglong enqueue_htime;
  ulong kickup;

  bool is_logged_in() const { return (conn_flags & CONNECTION_LOGGED_IN); }
  void set_logged_in() { conn_flags |= CONNECTION_LOGGED_IN; }

  bool is_bound_to_poll_descriptor() const {
    return (conn_flags & CONNECTION_BOUND_TO_POOL);
  }
  void set_bound_to_poll_descriptor() {
    conn_flags |= CONNECTION_BOUND_TO_POOL;
  }
  void clear_bound_to_poll_descriptor() {
    conn_flags &= ~CONNECTION_BOUND_TO_POOL;
  }
};

typedef I_P_List<connection_t,
                 I_P_List_adapter<connection_t, &connection_t::next_in_queue,
                                  &connection_t::prev_in_queue>,
                 I_P_List_counter, I_P_List_fast_push_back<connection_t>>
    connection_queue_t;

enum worker_creation_reason_t {
  WORKER_CREATION_REASON_STALL = 0,
  WORKER_CREATION_REASON_QUEUE_PUT_CONNECTION,
  WORKER_CREATION_REASON_WAIT_BEGIN,
  WORKER_CREATION_REASON_SINGLE_LISTENER,
  WORKER_CREATION_REASON_ARRAY_SIZE
};
struct thread_group_counters_t {
  ulonglong thread_creations;
  ulonglong thread_creations_by_reason[WORKER_CREATION_REASON_ARRAY_SIZE];
  ulonglong thread_creation_attempts;
  ulonglong thread_removes;
  ulonglong wakes;
  ulonglong wakes_due_to_stall;
  ulonglong throttles;
  ulonglong stalls;
  ulonglong dequeues_by_worker;
  ulonglong dequeues_by_listener;
  ulonglong polls_by_listener;
  ulonglong polls_by_worker;
  ulonglong kickups;
};

struct alignas(512) thread_group_t {
  mysql_mutex_t mutex;
  connection_queue_t queue;
  connection_queue_t high_prio_queue;
  worker_list_t idle_threads;
  worker_thread_t *listener;
  pthread_attr_t *pthread_attr;
  int pollfd;
  int thread_count;
  int active_thread_count;
  int connection_count;
  int waiting_thread_count;
  /* Stats for the deadlock detection timer routine.*/
  int io_event_count;
  int dequeued_event_count;
  ulonglong last_thread_creation_time;
  int shutdown_pipe[2];
  bool shutdown;
  bool stalled;
  thread_group_counters_t counters;
  ulonglong cpu_watch, cpu_time, wait_watch, wait_time;
  ulonglong event_count;
  std::atomic_bool exit_one_worker;
  int index;
};

#define TP_INCREMENT_GROUP_COUNTER(group, var) group->counters.var++;

extern thread_group_t g_all_groups[MAX_THREAD_GROUPS];

/** for threadpool cpu bind
 */

enum class Tp_cpu_bind_mode_t {
  TP_NOBIND_MODE,
  TP_CPUBIND_MODE,
  TP_BIND_INVALID
};

/**
parse user passed cpubind parameters, and check
whether it is valid

@param[in] proposed_value user passed parameters

@return  0 if success, non-zero otherwise
*/
extern void set_current_thread_affinity(thread_group_t *thread_group);

/** Bind current thread to cpu

@param[in] thread_group current thread group
*/
extern int parse_and_check_cpubind_info(const char *proposed_value);

#endif /* THREADPOOL_INCLUDED */
