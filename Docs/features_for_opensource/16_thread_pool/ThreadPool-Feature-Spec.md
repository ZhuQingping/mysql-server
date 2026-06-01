# Thread Pool Feature Specification

> Version: 2.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Main Commit: 21481fa93ed3fecab600633b76fc6f9c057d3d05
> Author: songliyong
> Merge Date: 2025-04-04
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

MySQL's default connection handling model uses one thread per connection. Under high concurrency, this leads to excessive thread creation, context switching overhead, and resource contention. Each thread consumes stack memory and kernel resources, making it difficult to scale to thousands of concurrent connections. Additionally, for the DStore storage engine, one transaction cannot run across workers, requiring transaction-granularity scheduling to ensure correctness.

### 1.2 Solution

Port and enhance the Percona Thread Pool plugin for MySQL 8.0 with the following key capabilities:

1. **Thread Pool Architecture**: Replace one-thread-per-connection with a pool of worker threads grouped by CPU core
2. **Transaction-granularity Scheduling**: For DStore, ensure a transaction is finished or aborted before scheduling it out; also support statement-level scheduling
3. **Thread-local Context Adaptation**: Handle THD running across different worker threads by initializing/cleaning up thread-locals per worker
4. **CPU Binding (Affinity)**: Bind thread groups to specific CPUs to reduce cache misses, especially effective on ARM
5. **Admin Port / Local Socket Per-Thread Handling**: Allow admin port connections and local socket connections to bypass the thread pool for emergency access
6. **Stall Detection**: Timer-based stall detection to prevent thread starvation
7. **Information Schema Integration**: Expose thread pool statistics via I_S tables

### 1.3 Execution Model

```
Connection arrives
  -> Channel_info created by connection acceptor
  -> Thread_pool_connection_handler::add_connection()
     -> Assign connection to thread group (round-robin or by CPU affinity)
     -> Create connection_t structure
     -> If no listener in group:
        -> Create worker thread as listener
     -> Else:
        -> Queue connection for processing
           -> If idle worker available: wake it
           -> Else if under thread limit: create new worker
           -> Else: wait for stall detection

Worker thread loop:
  -> set_current_thread_affinity(thread_group)
  -> my_thread_init()
  -> Check high-priority queue
  -> Check regular queue
  -> If no work: park in idle list or become listener
  -> If listener: poll for I/O events (epoll)

Stall detection (timer-based):
  -> If group is stalled (no progress for threadpool_stall_limit ms):
     -> Create additional worker thread (up to oversubscribe limit)
     -> Move low-priority connections to high-priority queue
```

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| Plugin core (commit #1) | 5 core + 68 total | +6,732 / -107 |
| CPU binding (commit #2) | 9 | +333 / -23 |
| MTR test fixes (commit #3) | 3 | +282 |
| Admin port (commit #4) | 33 | +949 / -36 |
| CPU bind bugfix (commit #5) | 5 | +68 / -9 |
| Sysbench stall fix (commit #6) | 3 | +3 / -5 |
| Plugin author update (commit #7) | 5 | +15 / -3 |
| Kill wrong thread fix (commit #8) | 8 | +230 / -5 |
| Information_schema abort fix (commit #9) | 11 | +96 / -27 |
| **Total (9 commits)** | **~68 unique files** | **~8,735 / -220** |

---

## 2. New Files

### 2.1 Plugin Core (`plugin/threadpool/`)

| File | Lines | Purpose |
|------|-------|---------|
| `CMakeLists.txt` | 23 | Build configuration: `MYSQL_ADD_PLUGIN(threadpool ... DEFAULT STATIC_ONLY)` |
| `threadpool.h` | 352 | Core types: `thread_group_t`, `connection_t`, `worker_thread_t`, configuration variables, CPU binding types, `TP_atomic_counter` |
| `threadpool_plugin.cc` | 808 | Plugin init/deinit, system variables, `Connection_handler` implementation, I_S table fill functions |
| `threadpool_common.cc` | 365 | Common routines: `threadpool_add_connection()`, `threadpool_process_request()`, `threadpool_remove_connection()`, wait begin/end |
| `threadpool_unix.cc` | 2010 | Unix-specific implementation: epoll/kqueue, listener, worker, timer, stall detection, thread management |
| `threadpool_cpubind.cc` | 203 | CPU affinity: parse bind parameters, `set_current_thread_affinity()`, `parse_and_check_cpubind_info()` |

### 2.2 MTR Tests (`plugin/threadpool/tests/mtr/`)

| File | Purpose |
|------|---------|
| `suite.opt` | Suite options: `--threadpool_enabled=ON` |
| `pool_of_threads.cnf` | Configuration for basic pool test |
| `pool_of_threads.test` / `.result` | Comprehensive pool functionality test (2296 lines of result) |
| `threadpool_enable_disable.cnf` / `.test` / `.result` | Online enable/disable switching |
| `threadpool_cpubind.cnf` / `.test` / `.result` | CPU binding configuration test |
| `threadpool_killquery.opt` / `.test` / `.result` | KILL QUERY handling with fault injection |
| `threadpool_connection_handler.test` / `.result` | Connection handler integration |
| `threadpool_rds_enable_active_threads_check.cnf` / `.test` / `.result` | Active threads limit check |
| `thread_pool_information_table.test` / `.result` | I_S table queries with pool enabled/disabled |
| `hwsql178-master.opt` / `.result` / `.test` | Regression test |
| `percona_bug1201681.cnf` / `.test` / `.result` | Percona bug regression test |

---

## 3. Modifications to Existing Files

### 3.1 `include/mysql/psi/mysql_idle.h`

Added `#include "my_compiler.h"` for attribute annotations.

### 3.2 `include/mysql/service_thd_wait.h`

Extended `thd_wait_type` enum:

```c++
typedef enum _thd_wait_type_e {
  THD_WAIT_NONE = 0,       // NEW: no wait
  THD_WAIT_SLEEP = 1,
  THD_WAIT_DISKIO = 2,
  THD_WAIT_ROW_LOCK = 3,
  THD_WAIT_BINLOG = 4,
  THD_WAIT_BINLOG_DUMP = 5,
  THD_WAIT_TABLE_LOCK = 6,
  THD_WAIT_META_DATA_LOCK = 7,
  THD_WAIT_USER_LOCK = 8,
  THD_WAIT_GROUP_COMMIT = 9,
  THD_WAIT_SYNC = 10,
  THD_WAIT_NET = 11,       // NEW: network wait
  THD_WAIT_BL_DUMPER = 12, // NEW: binlog dumper wait
  THD_WAIT_LAST = 13
} thd_wait_type;
```

### 3.3 `include/mysql/service_thread_scheduler.h`

Modified the scheduler service to separate connection handler and event functions:

```c++
struct my_thread_scheduler_service {
  int (*connection_handler_set)(struct Connection_handler_functions *);
  int (*connection_handler_reset)();
};
```

### 3.4 `include/violite.h`

Added VIO thread pool support functions for asynchronous I/O operations.

### 3.5 `sql/conn_handler/connection_handler_manager.cc`

- Registration of `Thread_pool_connection_handler` when `threadpool_enabled` is true
- Per-connection handler selection for admin port / local socket
- Separate scheduler callback per connection handler type

### 3.6 `sql/sql_class.h` and `sql/sql_class.cc`

Added `is_thrd_killable` flag and `set_thrd_is_killable()` / `thrd_is_killable()` methods:

```c++
class THD {
  bool is_killable;
  bool is_thrd_killable;   // NEW: tracks whether this thd is currently active in a worker

  void set_thrd_is_killable(bool killable);
  bool thrd_is_killable();
};
```

This prevents killing the wrong session when two sessions share the same THD in the thread pool.

### 3.7 `sql/mysqld.cc` and `sql/mysqld.h`

- Thread pool initialization and teardown hooks
- `rds_admin_max_connections` variable
- `rds_admin_port_using_per_thread` variable
- `rds_local_socket_using_per_thread` variable

### 3.8 `sql/sys_vars.cc` and `sql/sys_vars.h`

Added system variable definitions for admin port and local socket per-thread handling.

### 3.9 `storage/dstore/handler/ha_cde.cc`

- `CdeKillConnection()`: Added `thrd_is_killable()` check before setting interrupt flag
- Thread context assignment to `cde_session` for binlog group commit

### 3.10 `vio/vio.cc`, `vio/viosocket.cc`, `vio/viossl.cc`, `vio/viopipe.cc`, `vio/vioshm.cc`

Modified VIO layer to support thread pool asynchronous I/O operations, including non-blocking read/write and notification callbacks.

### 3.11 `sql/psi_memory_key.cc` and `sql/psi_memory_key.h`

Added PSI memory key for thread pool allocations.

---

## 4. Core Data Structures

### 4.1 thread_group_t

Per-CPU-core thread group, the fundamental scheduling unit (cache-line aligned):

```c++
struct alignas(512) thread_group_t {
  mysql_mutex_t mutex;               // Protects all group state
  connection_queue_t queue;           // Regular priority connection queue
  connection_queue_t high_prio_queue; // High priority connection queue
  worker_list_t idle_threads;        // Idle worker threads
  worker_thread_t *listener;         // Current listener thread (or NULL)
  pthread_attr_t *pthread_attr;      // Thread attributes for CPU binding
  int pollfd;                        // epoll/kqueue file descriptor
  int thread_count;                  // Total threads in group
  int active_thread_count;           // Currently active threads
  int connection_count;              // Connections assigned to group
  int waiting_thread_count;          // Threads in wait state
  int io_event_count;                // I/O events received
  int dequeued_event_count;          // Events dequeued for processing
  ulonglong last_thread_creation_time;
  int shutdown_pipe[2];              // Pipe for shutdown notification
  bool shutdown;
  bool stalled;
  thread_group_counters_t counters;  // Statistics counters
  ulonglong cpu_watch, cpu_time;     // CPU time tracking
  ulonglong wait_watch, wait_time;   // Wait time tracking
  ulonglong event_count;
  std::atomic_bool exit_one_worker;  // Flag to signal one worker to exit
  int index;                         // Group index for CPU binding
};
```

### 4.2 connection_t

Per-connection state within the thread pool:

```c++
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
  uint tickets;              // High-priority tickets remaining
  ulonglong enqueue_time;    // Time when enqueued (for stats)
  ulonglong enqueue_htime;   // High-priority enqueue time
  ulong kickup;              // Count of priority kick-ups
};
```

### 4.3 worker_thread_t

Per-worker-thread state:

```c++
struct worker_thread_t {
  ulonglong event_count;          // Number of requests handled
  thread_group_t *thread_group;
  worker_thread_t *next_in_list;
  worker_thread_t **prev_in_list;
  mysql_cond_t cond;              // Wait condition for idle workers
  bool woken;                     // Whether worker has been woken
  std::atomic_bool exit;          // Flag to signal worker to exit
};
```

### 4.4 thread_group_counters_t

Statistics counters per thread group:

```c++
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
```

### 4.5 Tp_cpu_bind_mode_t

```c++
enum class Tp_cpu_bind_mode_t {
  TP_NOBIND_MODE,     // No CPU binding (default)
  TP_CPUBIND_MODE,    // Bind thread groups to specific CPU ranges
  TP_BIND_INVALID     // Invalid binding specification
};
```

### 4.6 Schedule Modes

```c++
static const char *g_schedule_mode_names[] = {"statement", "transaction", nullptr};
```

- **statement**: Schedule per statement, a connection can be served by different workers between statements
- **transaction**: Schedule per transaction, a connection stays on the same worker until the transaction completes (required for DStore)

### 4.7 pool_timer_t

Global timer for stall detection:

```c++
struct pool_timer_t {
  mysql_mutex_t mutex;
  mysql_cond_t cond;
  std::atomic<uint64> current_microtime;
  std::atomic<uint64> next_timeout_check;
  int tick_interval;
  bool shutdown;
  bool sleep;
};
```

---

## 5. Execution Flow

### 5.1 Thread Pool Initialization

```
threadpool_plugin_init()
  -> tp_init()
     -> Initialize g_all_groups[MAX_THREAD_GROUPS]
        -> For each group 0..threadpool_size-1:
           -> thread_group_init(index, &g_all_groups[i], pthread_attr)
              -> mysql_mutex_init(&group.mutex)
              -> Create epoll fd: group.pollfd
              -> group.shutdown_pipe[0..1] = pipe()
              -> group.pthread_attr = NULL
              -> group.listener = NULL
              -> group.index = index
     -> Start timer thread:
        -> mysql_thread_create(timer_thread_main)
           -> Loop: sleep for tick_interval, then check all groups for stalls
     -> g_threadpool_started = true
     -> threadpool_running = true
  -> Parse CPU bind info
  -> my_connection_handler_set(&tp_handler_functions)
```

### 5.2 Connection Addition

```
add_connection(Channel_info *channel_info)
  -> Create THD from channel_info
  -> Assign to thread group (round-robin by default)
  -> Create connection_t
  -> mysql_mutex_lock(&group->mutex)
  -> group->connection_count++
  -> If no listener in group:
     -> create_worker(group, WORKER_CREATION_REASON_SINGLE_LISTENER)
        -> mysql_thread_create(worker_main, group)
           -> worker becomes listener: epoll_wait(...)
  -> Else:
     -> queue_put_connection(group, connection)
        -> Add to group->queue
        -> wake_or_create_thread(group)
  -> mysql_mutex_unlock(&group->mutex)
```

### 5.3 Worker Thread Main Loop

```
worker_main(void *param)
  -> thread_group_t *group = (thread_group_t *)param
  -> set_current_thread_affinity(group)   // CPU binding
  -> my_thread_init()
  -> worker_thread_t worker
  -> Loop:
     -> mysql_mutex_lock(&group->mutex)
     // 1. Check high-priority queue first
     -> connection = queue_get(group)
        -> check high_prio_queue first
        -> then regular queue (if not too many busy threads)
     -> if (connection):
        -> mysql_mutex_unlock(&group->mutex)
        -> handle_event(connection)
        -> continue
     // 2. No work: become listener if needed
     -> if (group->listener == NULL && !threadpool_dedicated_listener):
        -> group->listener = &worker
        -> mysql_mutex_unlock(&group->mutex)
        -> poll_for_events(group)  // epoll_wait loop
        -> continue
     // 3. Park in idle list
     -> worker.woken = false
     -> group->idle_threads.push_front(&worker)
     -> mysql_mutex_unlock(&group->mutex)
     -> mysql_cond_wait(&worker.cond)
```

### 5.4 Event Handling (with kill safety)

```
handle_event(connection_t *connection)
  -> THD *thd = connection->thd
  -> thd->set_thrd_is_killable(true)   // Mark as active/killable
  -> if (!connection->is_logged_in()):
     -> threadpool_add_connection(thd)
        -> thd_prepare_connection(thd)
        -> connection->set_logged_in()
  -> else:
     -> threadpool_process_request(thd)
        -> do_handle_one_connection(thd)
  -> thd->set_thrd_is_killable(false)  // Mark as inactive/not killable
  -> set_wait_timeout(connection)
  -> re-add to group queue or poll set
```

### 5.5 Stall Detection

```
timer_thread_main()
  -> Loop (every tick_interval ms):
     -> current_time = my_microtime()
     -> For each active group:
        -> mysql_mutex_lock(&group->mutex)
        -> if (group has pending events or connections in queue):
           -> if (group->stalled):
              -> wake_or_create_thread(group, WORKER_CREATION_REASON_STALL)
           -> else:
              -> Check if last thread creation was long enough ago
              -> If yes: mark group as stalled
                 -> group->stalled = true
                 -> Move connections from queue to high_prio_queue
        -> mysql_mutex_unlock(&group->mutex)
     -> Check idle thread timeouts
```

### 5.6 CPU Binding

```
parse_and_check_cpubind_info("cpubind:0-3,8-11")
  -> Parse format: "cpubind:<start>-<end>,<start>-<end>"
  -> Validate CPU IDs against system's available CPUs (sched_getaffinity)
  -> Build g_available_cpus vector: [0, 1, 2, 3, 8, 9, 10, 11]
  -> Return 0 on success

set_current_thread_affinity(thread_group_t *thread_group)
  -> cpu_id = g_available_cpus[index % g_available_cpus.size()]
  -> CPU_ZERO(&cpuset); CPU_SET(cpu_id, &cpuset)
  -> pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset)

Invalid cpubind handling:
  -> If user parameter is invalid, restore to "nobind"
  -> my_free(threadpool_cpubind_info)
  -> threadpool_cpubind_info = my_strdup(PSI_NOT_INSTRUMENTED, TP_NO_BIND, MYF(0))
```

### 5.7 Admin Port / Local Socket Per-Thread

```
Connection arrives on admin port or local socket:
  -> If rds_admin_port_using_per_thread == ON (for admin port):
     -> Use Per_thread_connection_handler (bypasses thread pool)
  -> If rds_local_socket_using_per_thread == ON (for local socket):
     -> Use Per_thread_connection_handler (bypasses thread pool)
  -> This ensures admin access even when thread pool is exhausted
```

### 5.8 Kill Safety

```
tp_post_kill_notification(THD *thd):
  -> if (current_thd == thd || thd->system_thread || !thd->is_classic_protocol()):
     -> return  // Skip self-kill, system threads, non-classic connections
  -> vio = thd->get_protocol_classic()->get_vio()
  -> if (vio) vio_cancel(vio, SHUT_RD)  // Wake the connection

CdeKillConnection(handlerton *hton, THD *thd):
  -> if (!thd->thrd_is_killable()):
     -> return  // Don't kill if thd is idle (wrong session could be interrupted)
  -> session->m_setInterrupt = true
```

---

## 6. System Variables

| Variable | Type | Scope | Default | Description |
|----------|------|-------|---------|-------------|
| `threadpool_idle_timeout` | uint | global | 60 | Timeout in seconds for idle worker threads |
| `threadpool_oversubscribe` | uint | global | 3 | Additional active workers allowed per group beyond one |
| `threadpool_size` | uint | global | CPU count | Number of thread groups |
| `threadpool_stall_limit` | uint | global | 500 | Stall detection interval in milliseconds |
| `threadpool_max_threads` | uint | global | 100000 | Maximum worker threads in the pool |
| `threadpool_prio_kickup_timer` | uint | global | 1000 | Time in ms before low-prio connection gets priority boost |
| `threadpool_high_prio_tickets` | uint | global | UINT_MAX | Tickets for high-priority queue per transaction |
| `threadpool_enabled` | bool | global | false | Enable/disable thread pool (online switchable) |
| `threadpool_exact_stats` | bool | global | false | Enable precise statistics in I_S tables |
| `threadpool_dedicated_listener` | bool | global | **false** | Listener thread does not pick up work items (changed from true to false in commit #6) |
| `threadpool_schedule_mode` | enum | global | transaction | Scheduling mode: statement or transaction |
| `threadpool_cpubind_info` | charptr | global | "nobind" | CPU binding specification (READONLY; e.g., "cpubind:0-7" or "nobind") |
| `threadpool_enable_active_threads_check` | bool | global | true | Limit thread oversubscription |
| `rds_admin_max_connections` | uint | global | 0 | Max connections on admin port (0=unlimited, independent of max_connections) |
| `rds_admin_port_using_per_thread` | bool | global | false | Admin port connections use per-thread handler |
| `rds_local_socket_using_per_thread` | bool | global | false | Local socket connections use per-thread handler |

---

## 7. EXPLAIN Output

Not applicable. Thread Pool does not affect query execution plans; it only changes connection handling and scheduling.

---

## 8. Error Handling

| Error Condition | Handling |
|----------------|----------|
| Worker creation failure | If `create_worker()` fails (thread limit reached or OS error), the group may stall. Timer retries on next tick. |
| epoll/kqueue failure | Logged via `LogErr()`, group marked as shutdown |
| Pipe creation failure | `shutdown_pipe` failure is fatal for the group |
| CPU binding failure | `pthread_setaffinity_np()` failure logged as warning, thread continues without affinity |
| Invalid CPU bind format | `parse_and_check_cpubind_info()` returns non-zero, variable rejected, restored to "nobind" |
| Kill wrong thread | Fixed by checking `thd->thrd_is_killable()` before setting interrupt; if thd is idle, skip interrupt |
| Sysbench stall | Fixed by setting `threadpool_dedicated_listener=false` so listener also handles data; check for residual data after login |
| Information_schema abort | Fixed by returning early from I_S fill functions when `threadgroup_invalid()` (pool disabled/not running) |
| Thread pool disable during query | Existing connections continue in pool mode; new connections use per-thread handler |

---

## 9. Test Coverage

MTR test cases in `plugin/threadpool/tests/mtr/`:

| Test | Description |
|------|-------------|
| `pool_of_threads` | Basic thread pool functionality (2296 lines of result data) |
| `threadpool_enable_disable` | Online enable/disable switching |
| `threadpool_cpubind` | CPU binding configuration and verification |
| `threadpool_killquery` | KILL QUERY handling with debug fault injections |
| `threadpool_connection_handler` | Connection handler integration (pool + per-thread) |
| `threadpool_rds_enable_active_threads_check` | Active threads limit check |
| `thread_pool_information_table` | I_S tables (groups, stats, queues, waits) with pool enabled and disabled |
| `hwsql178` | Specific regression test |
| `percona_bug1201681` | Percona bug regression test |

Additional MTR tests in `mysql-test/t/`:
| Test | Description |
|------|-------------|
| `rds_admin_port_using_per_thread` | Admin port bypasses thread pool |
| `rds_local_socket_using_per_thread` | Local socket bypasses thread pool |
| `rds_admin_max_connections_basic` | Admin max connections variable |
| `rds_admin_port_using_per_thread_basic` | Variable definition test |
| `rds_local_socket_using_per_thread_basic` | Variable definition test |
| `huawei_add_plugins` | Plugin author verification |

Unit tests in `unittest/gunit/threadpool/`:
| Test | Description |
|------|-------------|
| `cpubind-t.cc` | CPU bind parsing, validation, invalid parameter handling, OOM injection |
| `conn_handler-t.cc` | Connection handler manager integration |

---

## 10. Limitations

- Maximum 512 thread groups (`MAX_THREAD_GROUPS`)
- CPU binding requires Linux `pthread_setaffinity_np()` (not portable to all platforms)
- Stall detection is timer-based (default 500ms), which may be too slow for some workloads
- `threadpool_dedicated_listener` defaults to false; setting to true can cause sysbench stall issues with EPOLLET + EPOLLONESHOT
- The `threadpool_schedule_mode=transaction` requires storage engine support for transaction state queries
- Online disable only pauses new connections; existing connections continue in thread pool mode
- `threadpool_cpubind_info` is read-only; requires server restart to change
- Admin port and local socket per-thread handling requires separate connection handler, adding complexity
- Thread-local context must be carefully managed when THD moves between workers
- Some MTR tests excluded: `per_thread_connection_handler`, `partition_locking`, `mem_cnt_common_debug`, etc. (resource limit features)
- Plugin is built as `STATIC_ONLY` (always linked in, cannot be dynamically loaded)

---

## 11. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **`include/mysql/service_thd_wait.h`** -- Extend `thd_wait_type` enum with THD_WAIT_NONE, THD_WAIT_NET, THD_WAIT_BL_DUMPER
2. **`include/mysql/service_thread_scheduler.h`** -- Modify scheduler service interface
3. **`include/violite.h`** -- Add VIO thread pool support functions
4. **`plugin/threadpool/threadpool.h`** -- Core type definitions, configuration variables, API declarations
5. **`plugin/threadpool/threadpool_common.cc`** -- Common routines: add_connection, process_request, remove_connection, wait begin/end
6. **`plugin/threadpool/threadpool_unix.cc`** -- OS-specific: epoll/kqueue, listener, worker, timer, stall detection, thread creation
7. **`plugin/threadpool/threadpool_cpubind.cc`** -- CPU affinity parsing and setting
8. **`plugin/threadpool/threadpool_plugin.cc`** -- MySQL plugin interface, system variables, Connection_handler, I_S tables
9. **`plugin/threadpool/CMakeLists.txt`** -- Build configuration
10. **`sql/conn_handler/connection_handler_manager.cc/.h`** -- Register Thread_pool_connection_handler, admin port/local socket per-thread support
11. **`sql/sql_class.h/.cc`** -- Add `is_thrd_killable` flag and methods
12. **`sql/mysqld.cc`** -- Thread pool init hooks, admin connection variables
13. **`sql/sql_parse.cc`** -- Integration with tp_wait_begin/tp_wait_end
14. **`sql/sys_vars.cc/.h`** -- System variable definitions for admin port/local socket
15. **`storage/dstore/handler/ha_cde.cc`** -- Kill connection safety check, thread context assignment
16. **`vio/*.cc`** -- VIO layer thread pool support
17. **MTR test suite** -- Port all test cases
18. **Unit tests** -- cpubind-t.cc, conn_handler-t.cc
