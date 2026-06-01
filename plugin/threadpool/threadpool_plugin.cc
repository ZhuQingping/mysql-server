/* Copyright (c) 2019, 2021, Huawei and/or its affiliates. All rights reserved.

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

#include <my_sys.h>
#include <mysql/plugin.h>
#include <mysql/thread_pool_priv.h>
#include <sql/field.h>
#include <sql/set_var.h>
#include <sql/sql_class.h>
#include <sql/sql_show.h>
#include <sql/table.h>
#include <unistd.h>
#include "mysql/components/services/log_builtins.h"
#include "mysqld_error.h"
#include "threadpool.h"

#define MAX_CONNECTIONS 100000
#define MY_MAX(a, b) ((a) > (b) ? (a) : (b))

/** declare system global variables for threadpool
 */

static const char *g_schedule_mode_names[] = {"statement", "transaction",
                                              nullptr};
static TYPELIB g_schedule_mode_typelib = {
    array_elements(g_schedule_mode_names) - 1, "g_schedule_mode_typelib",
    g_schedule_mode_names, nullptr};

/** thread plugin init
@param[in]      p unused

@return 0 if success
*/
static int threadpool_plugin_init(void *p);

/** thread plugin deinit
@param[in]      p unused

@return 0 if success
*/
static int threadpool_plugin_deinit(void *p);

/** get cup number can be used

@return cun number
*/
static uint getncpus() {
#if defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  if (sched_getaffinity(0, sizeof(cpuset), &cpuset) != 0) return 1;
  uint ncpus = CPU_COUNT(&cpuset);
#elif defined(_SC_NPROCESSORS_ONLN)
  uint ncpus = sysconf(_SC_NPROCESSORS_ONLN);
#else
  uint ncpus = 1;
#endif
  return MY_MAX(ncpus, 1);
}

/** check function for system variable threadpool_size

@param[in] value the system variable value to be set
*/
static void fix_threadpool_size(THD *, SYS_VAR *, void *, const void *value) {
  threadpool_size = *static_cast<const uint *>(value);
  tp_set_threadpool_size(threadpool_size);
}

/** check function for system variable threadpool_stall_limit

@param[in] value the system variable value to be set
*/
static void fix_threadpool_stall_limit(THD *, SYS_VAR *, void *,
                                       const void *value) {
  threadpool_stall_limit = *static_cast<const uint *>(value);
  tp_set_threadpool_stall_limit(threadpool_stall_limit);
}

/** check function for system variable threadpool_enabled

@param[in] value the system variable value to be set
*/
static void fix_threadpool_enabled(MYSQL_THD, SYS_VAR *, void *,
                                   const void *value) {
  bool new_val = *static_cast<const bool *>(value);
  if (new_val != threadpool_enabled) {
    threadpool_enabled = new_val;
    if (threadpool_enabled) {
      threadpool_plugin_init(NULL);
    } else {
      /* when online disable threadpool, just restore the default connection
       * handler */
      my_connection_handler_reset();
      /* when connection handler is reset, the thread pool's timer shall sleep,
       * because there is nothing to do when all connections in thread pool are
       * closed */
      pause_pool_timer();
    }
  }
}

/**
Check whether cpu bind info is valid.

@param[in]  thd      session
@param[in]  self     the system variable we're checking
@param[in]  save     where to save the resulting intermediate (char *) value
@param[in]  value    the value we're validating

@return   0    value OK, go ahead and update system variable (from "save")
          1    value rejected, do not update variable
*/
static int check_cpu_bindinfo(MYSQL_THD thd [[maybe_unused]],
                              SYS_VAR *self [[maybe_unused]], void *save,
                              struct st_mysql_value *value) {
  int value_len = 0;
  const char *proposed_value;
  if (value == nullptr) {
    return 1;
  }

  proposed_value = value->val_str(value, nullptr, &value_len);
  if (proposed_value == nullptr) {
    return 1;
  }

  *static_cast<const char **>(save) = proposed_value;

  return parse_and_check_cpubind_info(proposed_value);
}

static MYSQL_SYSVAR_UINT(idle_timeout, threadpool_idle_timeout,
                         PLUGIN_VAR_RQCMDARG,
                         "Timeout in seconds for an idle thread in the thread "
                         "pool. Worker thread will be shut down after timeout",
                         NULL, NULL, 60, 1, UINT_MAX, 1);

static MYSQL_SYSVAR_UINT(oversubscribe, threadpool_oversubscribe,
                         PLUGIN_VAR_RQCMDARG,
                         "How many additional active worker threads in a "
                         "thread group are all allowed.",
                         NULL, NULL, 3, 1, 1000, 1);

static MYSQL_SYSVAR_UINT(
    size, threadpool_size, PLUGIN_VAR_RQCMDARG,
    "Number of thread groups in the pool."
    "This parameter is roughly equivalent to maximum number of concurrently "
    "executing threads (threads in a waiting state do not count as executing).",
    NULL, fix_threadpool_size, (uint)getncpus(), 1, MAX_THREAD_GROUPS, 1);

static MYSQL_SYSVAR_UINT(
    stall_limit, threadpool_stall_limit, PLUGIN_VAR_RQCMDARG,
    "Maximum query execution time in milliseconds,"
    "before an executing non-yielding thread is considered stalled."
    "If a worker thread is stalled, additional worker thread "
    "may be created to handle remaining clients.",
    NULL, fix_threadpool_stall_limit, 500, 10, UINT_MAX, 1);

static MYSQL_SYSVAR_UINT(
    max_threads, threadpool_max_threads, PLUGIN_VAR_RQCMDARG,
    "Maximum allowed number of worker threads in the thread pool", NULL, NULL,
    MAX_CONNECTIONS, 1, MAX_CONNECTIONS, 1);

static MYSQL_SYSVAR_UINT(
    prio_kickup_timer, threadpool_prio_kickup_timer, PLUGIN_VAR_RQCMDARG,
    "Timeout in milliseconds for moving a connection from low priority queue "
    "to high priority queue after timeout",
    NULL, NULL, 1000, 1, UINT_MAX, 1);

static MYSQL_SYSVAR_UINT(
    high_prio_tickets, threadpool_high_prio_tickets, PLUGIN_VAR_RQCMDARG,
    "Number of tickets to enter the high priority event queue for each "
    "transaction.",
    NULL, NULL, UINT_MAX, 0, UINT_MAX, 1);

static MYSQL_SYSVAR_BOOL(enabled, threadpool_enabled, PLUGIN_VAR_RQCMDARG,
                         "Enable threadpool.", NULL, fix_threadpool_enabled,
                         false);

static MYSQL_SYSVAR_BOOL(exact_stats, threadpool_exact_stats,
                         PLUGIN_VAR_RQCMDARG,
                         "If set to true, provides better statistics in "
                         "information_schema threadpool tables",
                         NULL, NULL, false);

static MYSQL_SYSVAR_BOOL(
    dedicated_listener, threadpool_dedicated_listener, PLUGIN_VAR_RQCMDARG,
    "If set to true, listener thread will not pick up queries", NULL, NULL,
    false);

static MYSQL_SYSVAR_BOOL(
    enable_active_threads_check, threadpool_enable_active_threads_check,
    PLUGIN_VAR_RQCMDARG,
    "If set to false then enable threadpool to create additional thread when "
    "the threadpool_active_threads >= threadpool_oversubscribe + 1",
    NULL, NULL, true);

static MYSQL_SYSVAR_ENUM(
    schedule_mode, threadpool_schedule_mode, PLUGIN_VAR_RQCMDARG,
    "Set the query schedule mode, "
    "the parmaters can be 'STATEMENT', 'TRANSACTION'"
    "STATEMENT: statements in one transaction can run in different workers"
    "TRANSACTION: statements in one transaction can only run in a single "
    "worker.",
    nullptr, nullptr, TP_SCHEDULE_MODE_TRANSACTION, &g_schedule_mode_typelib);

static MYSQL_SYSVAR_STR(
    cpubind_info, threadpool_cpubind_info,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_READONLY,
    "Bind threadpool worker threads to specified cpus, "
    "the parmaters can be 'nobind' and 'cpubind(1-30,40-70,...)'. "
    "nobind: worker threads do not bind to specified cpus. "
    "cpubind: bind thread group to one of the specified cpus by round robin.",
    check_cpu_bindinfo, nullptr, "nobind");

static SYS_VAR *system_variables[] = {MYSQL_SYSVAR(idle_timeout),
                                      MYSQL_SYSVAR(oversubscribe),
                                      MYSQL_SYSVAR(size),
                                      MYSQL_SYSVAR(stall_limit),
                                      MYSQL_SYSVAR(high_prio_tickets),
                                      MYSQL_SYSVAR(max_threads),
                                      MYSQL_SYSVAR(prio_kickup_timer),
                                      MYSQL_SYSVAR(enabled),
                                      MYSQL_SYSVAR(dedicated_listener),
                                      MYSQL_SYSVAR(exact_stats),
                                      MYSQL_SYSVAR(enable_active_threads_check),
                                      MYSQL_SYSVAR(schedule_mode),
                                      MYSQL_SYSVAR(cpubind_info),
                                      NULL};

/** idle threads stats for information_schema

@param[out] var the content of the idle thread stats

@return  0 if success, non-zero otherwise
*/
int show_threadpool_idle_threads(THD *thd MY_ATTRIBUTE((unused)), SHOW_VAR *var,
                                 char *buff) {
  var->type = SHOW_INT;
  var->value = buff;
  *(int *)buff = tp_get_idle_thread_count();
  return 0;
}

/** active threads stats for information_schema

@param[out] var the content of the idle thread stats

@return 0 if success, non-zero otherwise
*/
int show_threadpool_active_threads(THD *thd MY_ATTRIBUTE((unused)),
                                   SHOW_VAR *var, char *buff) {
  var->type = SHOW_INT;
  var->value = buff;
  *(int *)buff = tp_get_active_thread_count();
  return 0;
}

/** waiting threads stats for information_schema

@param[out] var the content of the idle thread stats

@return 0 if success, non-zero otherwise
*/
int show_threadpool_waiting_threads(THD *thd MY_ATTRIBUTE((unused)),
                                    SHOW_VAR *var, char *buff) {
  var->type = SHOW_INT;
  var->value = buff;
  *(int *)buff = tp_get_waiting_thread_count();
  return 0;
}
/** alive connections stats for information_schema

@param[out] var the content of the idle thread stats

@return 0 if success, non-zero otherwise
*/

int show_threadpool_alive_connections(THD *thd MY_ATTRIBUTE((unused)),
                                      SHOW_VAR *var, char *buff) {
  var->type = SHOW_INT;
  var->value = buff;
  *(int *)buff = tp_get_active_connections();
  return 0;
}

static SHOW_VAR status_variables[] = {
    {"Threadpool_idle_threads", (char *)&show_threadpool_idle_threads,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Threadpool_threads", (char *)&g_tp_stats.num_worker_threads, SHOW_INT,
     SHOW_SCOPE_GLOBAL},
    {"Threadpool_active_connections",
     (char *)&show_threadpool_alive_connections, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Threadpool_running", (char *)&threadpool_running, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {"Threadpool_active_threads", (char *)&show_threadpool_active_threads,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Threadpool_waiting_threads", (char *)&show_threadpool_waiting_threads,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

// we want run destructor manully. Register an empty function
static void tp_empty_fun() {}

static Connection_handler_functions tp_handler_functions = {
    threadpool_max_threads,
    add_connection,
    tp_empty_fun,
};

THD_event_functions thd_event_functions = {
    tp_wait_begin,
    tp_wait_end,
    tp_post_kill_notification,
};

/**
  Restore the threadpool_cpubind_info to default value
  if the user pass invalid cpubind info.show variables
  like 'threadpool_cpubind_info' will display 'nobind',
  not the user passed invalid value.

  @return 0 if cpubind info can be restored to default value
          non-zero means memory is insufficient.
*/
static inline int restore_default_cpubind_value() {
  /* Do not want to introduce secure c api here. It's only called at
     startup and the parameter is invalid. So just use my_free and
     my_strdup, the memory will be freed when mysqld is shutdown.
  */
  if (threadpool_cpubind_info != nullptr) {
    my_free(threadpool_cpubind_info);
  }
  threadpool_cpubind_info = my_strdup(PSI_NOT_INSTRUMENTED, TP_NO_BIND, MYF(0));
  if (threadpool_cpubind_info == nullptr) {
#ifdef NDEBUG
    LogErr(ERROR_LEVEL, ER_SERVER_OUTOFMEMORY, strlen(TP_NO_BIND));
#endif
    return 1;
  }
  return 0;
}

/** parse user passed cpubind parameters, and check
whether it is valid. Log an error if it is invalid.

For log_builtins.cc:1323 will assert if we log an error.
So we can not use mtr to test invalid param.

@param[in] cpubind_info user passed parameters

@return 0 if cpubind info is valid or can be restored to default value
       non-zero means memory is insufficient.
*/
int parse_cpu_bind_info(const char *cpubind_info) {
  if (cpubind_info != nullptr) {
    int ret = parse_and_check_cpubind_info(cpubind_info);
    if (ret != 0) {
// DEBUG will cause assert in LogErr
#ifdef NDEBUG
      LogErr(ERROR_LEVEL, ER_INVALID_PARAMETER_USE, cpubind_info);
#endif
      /* set cpubind to 'nobind' if user parameter is invalid.
         threadpool_cpubind_info will show 'nobind' */
      return restore_default_cpubind_value();
    }
  }
  return 0;
}

/** thread plugin init
update mysql scheduler functions point to our threadpool scheduler functions

@param[in]      p unused

@return 0 if success, non-zero otherwise
*/
static int threadpool_plugin_init(void *p MY_ATTRIBUTE((unused))) {
  DBUG_ENTER("threadpool_plugin_init");
  // skip init if threadpool is not enabled
  if (!threadpool_enabled) DBUG_RETURN(0);
  tp_init();
  int ret = parse_cpu_bind_info(threadpool_cpubind_info);
  my_connection_handler_set(&tp_handler_functions);
  DBUG_RETURN(ret);
}

/* when unload the thread pool plugin,
restore the scheduler to be the original one

@param[in]      p unused

@return 0 if success, non-zero otherwise
*/
static int threadpool_plugin_deinit(void *p MY_ATTRIBUTE((unused))) {
  DBUG_ENTER("threadpool_plugin_deinit");
  my_connection_handler_reset();
  tp_end();
  DBUG_RETURN(0);
}

/* check whether thread group is invalid, if so, we can
 not access the status of threadpool
 */
static bool inline threadgroup_invalid() {
  return !threadpool_enabled || !threadpool_running;
}

static ST_FIELD_INFO groups_fields_info[] = {
    {"GROUP_ID", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"CONNECTIONS", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"THREADS", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"ACTIVE_THREADS", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"STANDBY_THREADS", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"LOWPRIO_QUEUE_LENGTH", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"HIGHPRIO_QUEUE_LENGTH", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"HAS_LISTENER", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"IS_STALLED", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"THREADPOOL_SIZE", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"CPU_COUNT", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"CPU_TIME", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"WAIT_TIME", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"DEQUEUED EVENTS", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"EVENT_COUNT", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"IO_EVENT_COUNT", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}};

static int groups_fill_table(THD *thd, Table_ref *tables, Item *) {
  if (threadgroup_invalid()) {
    return 0;
  }
  TABLE *table = tables->table;
  for (uint i = 0; i < MAX_THREAD_GROUPS && g_all_groups[i].pollfd != -1; i++) {
    thread_group_t *group = &g_all_groups[i];
    /* ID */
    table->field[0]->store(i, true);
    /* CONNECTION_COUNT */
    table->field[1]->store(group->connection_count, true);
    /* THREAD_COUNT */
    table->field[2]->store(group->thread_count, true);
    /* ACTIVE_THREAD_COUNT */
    table->field[3]->store(group->active_thread_count, true);
    /* STANDBY_THREAD_COUNT */
    table->field[4]->store(group->idle_threads.elements(), true);
    /* QUEUE LENGTH */
    uint lowprio_queue_len = group->queue.elements();
    uint highprio_queue_len = group->high_prio_queue.elements();

    table->field[5]->store(lowprio_queue_len, true);
    table->field[6]->store(highprio_queue_len, true);
    /* HAS_LISTENER */
    table->field[7]->store((longlong)(group->listener != 0), true);
    /* IS_STALLED */
    table->field[8]->store(group->stalled, true);

    table->field[9]->store(threadpool_size);
    table->field[10]->store(getncpus());
    table->field[11]->store(group->cpu_time);
    table->field[12]->store(group->wait_time);

    table->field[13]->store(group->dequeued_event_count);
    table->field[14]->store(group->event_count);
    table->field[15]->store(group->io_event_count);

    if (schema_table_store_record(thd, table)) return 1;
  }
  return 0;
}

/* Init informa_schema group info

@param[in]      p schema

@return 0 if success, non-zero otherwise
*/
static int groups_init(void *p) {
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
  schema->fields_info = groups_fields_info;
  schema->fill_table = groups_fill_table;
  return 0;
}

static ST_FIELD_INFO queues_field_info[] = {
    {"GROUP_ID", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"POSITION", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"PRIORITY", 1, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"CONNECTION_ID", 19, MYSQL_TYPE_LONG, 0, 2, 0, 0},
    {"QUEUEING_TIME_MICROSECONDS", 19, MYSQL_TYPE_LONG, 0, 2, 0, 0},
    {"QUEUEING_HTIME_MICROSECONDS", 19, MYSQL_TYPE_LONG, 0, 2, 0, 0},
    {"TICKETS", 19, MYSQL_TYPE_LONGLONG, 0, 2, 0, 0},
    {"KICKUP", 1, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}};

typedef connection_queue_t::Iterator connection_queue_iterator;

/* Fill informa_schema queue info

@param[in]      thd  current thd
@param[in]      tables tables to be filled

@return 0 if success, non-zero otherwise
*/
static int queues_fill_table(THD *thd, Table_ref *tables, Item *) {
  if (threadgroup_invalid()) {
    return 0;
  }
  TABLE *table = tables->table;
  for (uint group_id = 0;
       group_id < MAX_THREAD_GROUPS && g_all_groups[group_id].pollfd != -1;
       group_id++) {
    thread_group_t *group = &g_all_groups[group_id];

    mysql_mutex_lock(&group->mutex);
    bool err = false;
    int pos = 0;
    connection_queue_t current_q;
    ulonglong now = my_getsystime() / 10;
    for (uint prio = 0; prio < 2 && !err; prio++) {
      if (prio == 0)
        current_q = group->high_prio_queue;
      else if (prio == 1)
        current_q = group->queue;

      connection_queue_iterator it(current_q);

      connection_t *c;
      while ((c = it++) != 0) {
        /* GROUP_ID */
        table->field[0]->store(group_id, true);
        /* POSITION */
        table->field[1]->store(pos++, true);
        /* PRIORITY */
        table->field[2]->store(prio, true);
        /* CONNECTION_ID */
        table->field[3]->store(c->thd->thread_id(), true);
        /* QUEUEING_TIME_MICROSECONDS */
        table->field[4]->store(now - c->enqueue_time, true);
        /* QUEUEING_HTIME_MICROSECONDS */
        table->field[5]->store(now - c->enqueue_htime, true);
        /* TICKETS */
        table->field[6]->store(c->tickets, true);
        /* KICKUP */
        table->field[7]->store(c->kickup, true);

        err = schema_table_store_record(thd, table);
        if (err) break;
      }
    }
    mysql_mutex_unlock(&group->mutex);
    if (err) return 1;
  }
  return 0;
}

/* Init informa_schema queue info

@param[in]      p  schema

@return 0 if success, non-zero otherwise
*/
static int queues_init(void *p) {
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
  schema->fields_info = queues_field_info;
  schema->fill_table = queues_fill_table;
  return 0;
}

static ST_FIELD_INFO stats_fields_info[] = {
    {"GROUP_ID", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
    {"THREAD_CREATIONS", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"THREAD_CREATIONS_DUE_TO_STALL", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"WAKES", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"WAKES_DUE_TO_STALL", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"THROTTLES", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"STALLS", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"POLLS_BY_LISTENER", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"POLLS_BY_WORKER", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"DEQUEUES_BY_LISTENER", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"DEQUEUES_BY_WORKER", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"KICKUPS", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"THREAD_CREATIONS_QUEUE_PUT_CONNECTION", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0,
     0},
    {"THREAD_CREATIONS_WAIT_BEGIN", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"THREAD_CREATIONS_SINGLE_LISTENER", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"THREAD_CREATION_ATTEMPTS", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"THREAD_REMOVES", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"CPU_TIMER", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {"EVENTS_HANDLED_CUR", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}};

/* Fill informa_schema stat info

@param[in]      thd  current thd
@param[in]      tables tables to be filled

@return 0 if success, non-zero otherwise
*/
static int stats_fill_table(THD *thd, Table_ref *tables, Item *) {
  if (threadgroup_invalid()) {
    return 0;
  }
  TABLE *table = tables->table;
  for (uint i = 0; i < MAX_THREAD_GROUPS && g_all_groups[i].pollfd != -1; i++) {
    table->field[0]->store(i, true);
    thread_group_t *group = &g_all_groups[i];

    mysql_mutex_lock(&group->mutex);
    thread_group_counters_t *counters = &group->counters;
    table->field[1]->store(counters->thread_creations, true);
    table->field[2]->store(
        counters->thread_creations_by_reason[WORKER_CREATION_REASON_STALL],
        true);
    table->field[3]->store(counters->wakes, true);
    table->field[4]->store(counters->wakes_due_to_stall, true);
    table->field[5]->store(counters->throttles, true);
    table->field[6]->store(counters->stalls, true);
    table->field[7]->store(counters->polls_by_listener, true);
    table->field[8]->store(counters->polls_by_worker, true);
    table->field[9]->store(counters->dequeues_by_listener, true);
    table->field[10]->store(counters->dequeues_by_worker, true);
    table->field[11]->store(counters->kickups, true);
    table->field[12]->store(counters->thread_creations_by_reason
                                [WORKER_CREATION_REASON_QUEUE_PUT_CONNECTION],
                            true);
    table->field[13]->store(
        counters->thread_creations_by_reason[WORKER_CREATION_REASON_WAIT_BEGIN],
        true);
    table->field[14]->store(counters->thread_creations_by_reason
                                [WORKER_CREATION_REASON_SINGLE_LISTENER],
                            true);
    table->field[15]->store(counters->thread_creation_attempts, true);
    table->field[16]->store(counters->thread_removes, true);
    table->field[17]->store(threadpool_cpu_timer, true);
    table->field[18]->store(threadpool_events_handled_cur, true);
    mysql_mutex_unlock(&group->mutex);
    if (schema_table_store_record(thd, table)) return 1;
  }
  return 0;
}

/* Init informa_schema stats info

@param[in]      p  schema

@return 0 if success, non-zero otherwise
*/
static int stats_init(void *p) {
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
  schema->fields_info = stats_fields_info;
  schema->fill_table = stats_fill_table;
  return 0;
}

static ST_FIELD_INFO waits_fields_info[] = {
    {"REASON", 16, MYSQL_TYPE_STRING, 0, 0, 0, 0},
    {"COUNT", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
    {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}};

/* See thd_wait_type enum for explanation*/
static const LEX_CSTRING wait_reasons[THD_WAIT_LAST] = {
    {STRING_WITH_LEN("UNKNOWN")},     {STRING_WITH_LEN("SLEEP")},
    {STRING_WITH_LEN("DISKIO")},      {STRING_WITH_LEN("ROW_LOCK")},
    {STRING_WITH_LEN("GLOBAL_LOCK")}, {STRING_WITH_LEN("META_DATA_LOCK")},
    {STRING_WITH_LEN("TABLE_LOCK")},  {STRING_WITH_LEN("USER_LOCK")},
    {STRING_WITH_LEN("BINLOG")},      {STRING_WITH_LEN("GROUP_COMMIT")},
    {STRING_WITH_LEN("SYNC")},        {STRING_WITH_LEN("NET")},
    {STRING_WITH_LEN("BL_DUMPER")},
};

extern TP_atomic_counter tp_waits[THD_WAIT_LAST];

/* Fill informa_schema waits info

@param[in]      thd  current thd
@param[in]      tables tables to be filled

@return 0 if success, non-zero otherwise
*/
static int waits_fill_table(THD *thd, Table_ref *tables, Item *) {
  if (threadgroup_invalid()) {
    return 0;
  }
  TABLE *table = tables->table;
  for (uint i = 0; i < THD_WAIT_LAST; i++) {
    table->field[0]->store(wait_reasons[i].str, wait_reasons[i].length,
                           system_charset_info);
    table->field[1]->store(tp_waits[i], true);
    if (schema_table_store_record(thd, table)) return 1;
  }
  return 0;
}

/* Init informa_schema waits info

@param[in]      p  schema

@return 0 if success, non-zero otherwise
*/
static int waits_init(void *p) {
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
  schema->fields_info = waits_fields_info;
  schema->fill_table = waits_fill_table;
  return 0;
}

static struct st_mysql_information_schema plugin_descriptor = {
    MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};

struct st_mysql_daemon threadpool_plugin_info = {
    MYSQL_DAEMON_INTERFACE_VERSION};

// clang-format off
mysql_declare_plugin(threadpool){
    MYSQL_DAEMON_PLUGIN,           /* type                            */
    &threadpool_plugin_info,       /* descriptor                      */
    "threadpool",                  /* name                            */
    PLUGIN_AUTHOR_HUAWEI,     /* author */
    "Thread pool from Percona 8.0 and enhanced", /* description        */
    PLUGIN_LICENSE_GPL,       /* plugin license                  */
    threadpool_plugin_init,   /* init function (when loaded)     */
    NULL,                     /* uninstall function              */
    threadpool_plugin_deinit, /* deinit function (when unloaded) */
    0x0001,                   /* version                         */
    status_variables,         /* status variables                */
    system_variables,         /* system variables                */
    NULL,
    0,
},
{
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &plugin_descriptor,
    "THREAD_POOL_GROUPS",
    "Vladislav Vaintroub",
    "Provides information about threadpool groups.",
    PLUGIN_LICENSE_GPL,
    groups_init,
    nullptr,
    nullptr,
    0x0100,
    nullptr,
    nullptr,
    nullptr,
    0,
},
{
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &plugin_descriptor,
    "THREAD_POOL_QUEUES",
    "Vladislav Vaintroub",
    "Provides information about threadpool queues.",
    PLUGIN_LICENSE_GPL,
    queues_init,
    nullptr,
    nullptr,
    0x0100,
    nullptr,
    nullptr,
    nullptr,
    0,
},
{
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &plugin_descriptor,
    "THREAD_POOL_STATS",
    "Vladislav Vaintroub",
    "Provides performance counter information for threadpool.",
    PLUGIN_LICENSE_GPL,
    stats_init,
    nullptr,
    nullptr,
    0x0100,
    nullptr,
    nullptr,
    nullptr,
    0,
},
{
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &plugin_descriptor,
    "THREAD_POOL_WAITS",
    "Vladislav Vaintroub",
    "Provides wait counters for threadpool.",
    PLUGIN_LICENSE_GPL,
    waits_init,
    nullptr,
    nullptr,
    0x0100,
    nullptr,
    nullptr,
    nullptr,
  0,
} mysql_declare_plugin_end;
// clang-format on
