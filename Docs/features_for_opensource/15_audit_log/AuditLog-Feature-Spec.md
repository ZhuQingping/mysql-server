# Audit Log Feature Specification

> Version: 2.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

MySQL lacks a built-in audit logging capability that records all database activities (logins, queries, DDL/DML operations) for security compliance and forensic analysis. The general query log is not suitable because it lacks structured formatting, filtering capabilities, and is not designed for security auditing.

### 1.2 Solution

Implement an Audit Log plugin that intercepts MySQL audit events (general, connection, and table access events) and writes them to a structured log file. The plugin supports multiple output formats (CSV2, OLD XML, NEW XML, JSON, CSV), asynchronous buffering, log file rotation, and comprehensive filtering by account, database, command, and IP address.

### 1.3 Execution Model

```
MySQL audit event occurs (login, query, table access)
  -> Audit plugin notification callback
     -> audit_log_notify()
        -> Check audit_log_policy (ALL/NONE/LOGINS/QUERIES)
        -> Check include/exclude filters (accounts, databases, commands, IP)
        -> Format record according to audit_log_format
           -> CSV2: Fast row-based format
           -> OLD/NEW: XML format
           -> JSON: JSON format
           -> CSV: Traditional CSV format
        -> audit_log_write() -> audit_handler_write()
           -> If buffered: write to ring buffer, flushed by writer thread
           -> If synchronous: write directly to file
           -> If performance mode: lock-free buffer write
```

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| Plugin core (`plugin/audit_log/`) | 11 | ~4,062 |
| SQL layer modifications | ~5 | ~30 |
| MTR tests | ~84 | ~5,000 |
| **Total** | **~100** | **~9,092** |

---

## 2. New Files

### 2.1 Plugin Core (`plugin/audit_log/`)

| File | Lines | Purpose |
|------|-------|---------|
| `audit_log.h` | 36 | PSI memory keys, plugin category macro |
| `audit_log.cc` | 2170 | Main plugin: audit notification callback, record formatting, system variables |
| `audit_handler.h` | 100 | `audit_handler_t` interface: write/flush/close/rotate/set_option |
| `audit_file.cc` | 205 | File-based audit handler implementation |
| `audit_syslog.cc` | 93 | Syslog-based audit handler implementation |
| `logger.h` | 83 | Logger service: file rotation, thread-safe writing |
| `file_logger.cc` | 366 | File logger implementation with rotation support |
| `buffer.h` | 37 | Ring buffer interface: init/write/pause/resume |
| `buffer.cc` | 207 | Ring buffer implementation for asynchronous logging |
| `filter.h` | 44 | Filter interface: include/exclude accounts/databases/commands/IP |
| `filter.cc` | 721 | Filter implementation with hash-based lookup |
| `CMakeLists.txt` | 48 | Build configuration |

---

## 3. Modifications to Existing Files

### 3.1 SQL Layer

Minimal modifications to the SQL layer since this is a plugin:

- `sql/sql_audit.h` / `sql/sql_audit.cc`: MySQL's existing audit infrastructure dispatches events to registered audit plugins including this one
- Audit event classes used: `MYSQL_AUDIT_GENERAL_ALL`, `MYSQL_AUDIT_CONNECTION_ALL`, `MYSQL_AUDIT_TABLE_ACCESS_ALL`

---

## 4. Core Data Structures

### 4.1 audit_handler_t

Polymorphic handler interface for writing audit records:

```c++
typedef struct audit_handler_struct {
  int (*write)(audit_handler_t *, const char *, size_t);
  int (*flush)(audit_handler_t *);
  int (*close)(audit_handler_t *);
  int (*rotate)(audit_handler_t *);
  void (*set_option)(audit_handler_t *, audit_handler_option_t, void *);
  audit_handler_data_t data;
} audit_handler_t;

enum audit_handler_option_t { OPT_ROTATE_ON_SIZE, OPT_ROTATIONS };

struct audit_handler_file_config_t {
  const char *name;
  size_t rotate_on_size;
  size_t rotations;
  bool sync_on_write;
  bool use_buffer;
  size_t buffer_size;
  bool can_drop_data;
  logger_prolog_func_t header;
  logger_epilog_func_t footer;
};
```

### 4.2 LOGGER_HANDLE

File logger with rotation support:

```c++
typedef struct logger_handle_st LOGGER_HANDLE;

// API:
LOGGER_HANDLE *logger_open(const char *path, unsigned long long size_limit,
                           unsigned int rotations, int thread_safe,
                           logger_prolog_func_t header);
int logger_close(LOGGER_HANDLE *log, logger_epilog_func_t footer);
int logger_write(LOGGER_HANDLE *log, const char *buffer, size_t size,
                 log_record_state_t state);
int logger_rotate(LOGGER_HANDLE *log);
int logger_sync(LOGGER_HANDLE *log);
```

### 4.3 audit_log_buffer_t

Ring buffer for asynchronous logging:

```c++
typedef struct audit_log_buffer audit_log_buffer_t;
typedef int (*audit_log_write_func)(void *data, const char *buf, size_t len,
                                    log_record_state_t state);

audit_log_buffer_t *audit_log_buffer_init(size_t size, int drop_if_full,
                                          audit_log_write_func write_func,
                                          void *data);
void audit_log_buffer_shutdown(audit_log_buffer_t *log);
int audit_log_buffer_write(audit_log_buffer_t *log, const char *buf, size_t len);
void audit_log_buffer_pause(audit_log_buffer_t *log);
void audit_log_buffer_resume(audit_log_buffer_t *log);
```

### 4.4 Record Formats

#### CSV2 Format (Fast Path)

```
"record_id","connection_id","status","name","timestamp","command_class",
"sqltext","user","host","os_user","ip","db"
```

The CSV2 format uses a fast row-based approach where field data is written sequentially using `row_put_data()`:

```c++
using audit_record = struct {
  size_t row_size;
  size_t datalen[AUDIT_LOG_STR_COUNT];  // 9 fields
  char data[4];  // variable-length data follows
};
```

#### OLD XML Format

```xml
<AUDIT_RECORD
  NAME="..."
  RECORD="..."
  TIMESTAMP="..."
  MYSQL_VERSION="..."
  STARTUP_OPTIONS="..."
  OS_VERSION="..."
/>
```

#### NEW XML Format

```xml
<AUDIT_RECORD>
  <NAME>...</NAME>
  <RECORD>...</RECORD>
  <TIMESTAMP>...</TIMESTAMP>
  ...
</AUDIT_RECORD>
```

#### JSON Format

```json
{"audit_record":{"name":"...","record":"...","timestamp":"...","mysql_version":"...",...}}
```

### 4.5 Filter Infrastructure

```c++
// Account filtering
void audit_log_set_include_accounts(const char *val);  // comma-separated user@host
void audit_log_set_exclude_accounts(const char *val);
bool audit_log_check_account_included(const char *user, size_t user_length,
                                      const char *host, size_t host_length);
bool audit_log_check_account_excluded(const char *user, size_t user_length,
                                      const char *host, size_t host_length);

// Database filtering
void audit_log_set_include_databases(const char *val);
void audit_log_set_exclude_databases(const char *val);
bool audit_log_check_database_included(const char *name, size_t length);
bool audit_log_check_database_excluded(const char *name, size_t length);

// Command filtering
void audit_log_set_include_commands(const char *val);
void audit_log_set_exclude_commands(const char *val);
bool audit_log_check_command_included(const char *name, size_t length);
bool audit_log_check_command_excluded(const char *name, size_t length);

// IP anonymization
void audit_log_set_anonymized_ip(const char *val);
bool audit_log_check_ip_anonymized(const char *name, size_t length);
```

Include/exclude are mutually exclusive per category (setting include clears exclude and vice versa).

### 4.6 Per-THD Local Storage

```c++
typedef struct {
  char *record_buffer;
  size_t record_buffer_size;
  struct {
    query_stack_frame *frames;
    size_t size;
  } stack;
} audit_log_thd_local;
```

This is stored in THD variables via `THDVAR(local)` and `THDVAR(local_ptr)`.

---

## 5. Execution Flow

### 5.1 Plugin Initialization

```
audit_log_plugin_init()
  -> Register PSI memory keys
  -> Initialize filter subsystem: audit_log_filter_init()
  -> Determine handler type (FILE or SYSLOG):
     -> If HANDLER_FILE:
        -> Set up file config (path, rotation, buffering)
        -> audit_handler_file_open(&config)
           -> logger_open(path, size_limit, rotations, thread_safe, header)
           -> If use_buffer:
              -> audit_log_buffer_init(buffer_size, drop_if_full, write_func, data)
        -> Set log_handler
  -> Set audit_log_is_active = true
```

### 5.2 Audit Event Notification

```
audit_log_notify(thd, event_class, event)
  -> if (!audit_log_is_active) return
  -> switch (event_class):
     case MYSQL_AUDIT_GENERAL_CLASS:
       -> Handle general events (LOG, ERROR, RESULT, STATUS)
       -> Check audit_log_policy:
          -> NONE: skip
          -> LOGINS: skip non-login events
          -> QUERIES: skip login events
       -> Check include/exclude filters:
          -> audit_log_check_account_included/excluded()
          -> audit_log_check_database_included/excluded()
          -> audit_log_check_command_included/excluded()
       -> Format record:
          -> CSV2: audit_log_general_record_fast()
             -> Allocate record buffer from THD local storage
             -> Build record using row_put_data()
             -> Escape SQL text if audit_log_csv2_escape is on
             -> Truncate if audit_log_csv2_truncation is on
          -> Other: audit_log_general_record()
             -> snprintf into buffer with format-specific template
       -> audit_log_write(buf, len)
          -> audit_handler_write(log_handler, buf, len)
             -> If buffered: audit_log_buffer_write()
             -> Else: logger_write() directly

     case MYSQL_AUDIT_CONNECTION_CLASS:
       -> Handle connection events (CONNECT, DISCONNECT, CHANGE_USER)
       -> Similar filtering and formatting as general class
       -> IP anonymization if configured:
          -> row_anonymized_ip() replaces digits with '*' after '@'

     case MYSQL_AUDIT_TABLE_ACCESS_CLASS:
       -> Handle table access events (READ, INSERT, UPDATE, DELETE)
       -> Format with table name, database, and query info
```

### 5.3 CSV2 Fast Format Flow

The CSV2 format uses a fast row-based approach:

```
1. Estimate buffer size: audit_log_general_buf_len()
2. Get record buffer: get_record_buffer(thd, size)
   -> If current buffer too small: reallocate via THDVAR
3. Format the record:
   a. Write header: "record_id,connection_id,status,"
   b. Write fields sequentially with escaping:
      - name (event type)
      - timestamp (UTC)
      - command_class
      - sqltext (escaped)
      - user, host, os_user, ip, db
   c. Apply IP anonymization if configured
   d. Apply truncation if audit_log_csv2_truncation and buffer too small
4. Write to handler
```

### 5.4 Log Rotation

```
Manual rotation: SET GLOBAL audit_log_force_rotate = ON
  -> audit_handler_rotate(log_handler)
     -> logger_rotate(LOGGER_HANDLE)
        -> Rename current file to file.1
        -> Shift existing rotated files (file.N-1 -> file.N)
        -> Open new file

Automatic rotation: When file size exceeds audit_log_rotate_on_size
  -> Same rotation logic triggered by logger_write()
```

### 5.5 IP Anonymization

```c++
static void row_anonymized_ip(char *next_buff, size_t length) {
  char *right_boundary = next_buff - 1;
  while (length--) {
    if (*right_boundary == '@') break;  // Stop at user@host separator
    if (*right_boundary >= '0' && *right_boundary <= '9')
      *right_boundary = '*';  // Replace digits with '*'
    right_boundary--;
  }
}
```

---

## 6. System Variables

| Variable | Type | Scope | Default | Description |
|----------|------|-------|---------|-------------|
| `audit_log_file` | charptr | global, readonly | `audit.log` | Path to audit log file |
| `audit_log_policy` | enum | global | `ALL` | Logging policy: ALL, NONE, LOGINS, QUERIES |
| `audit_log_strategy` | enum | global | `ASYNCHRONOUS` | Write strategy: ASYNCHRONOUS, PERFORMANCE, SEMISYNCHRONOUS, SYNCHRONOUS |
| `audit_log_format` | enum | global | `CSV2` | Output format: CSV2, OLD, NEW, JSON, CSV |
| `audit_log_buffer_size` | ulonglong | global, readonly | 1048576 | Buffer size for asynchronous mode (bytes) |
| `audit_log_rotate_on_size` | ulonglong | global | 0 | Rotate log file when it exceeds this size (0 = no rotation) |
| `audit_log_rotations` | ulonglong | global | 0 | Number of rotated log files to keep |
| `audit_log_flush` | bool | global | false | Force flush on every write |
| `audit_log_handler` | enum | global, readonly | `FILE` | Output handler: FILE or SYSLOG |
| `audit_log_syslog_ident` | charptr | global, readonly | `taurus-audit` | Syslog identity string |
| `audit_log_syslog_facility` | enum | global, readonly | `LOG_USER` | Syslog facility |
| `audit_log_syslog_priority` | enum | global, readonly | `LOG_INFO` | Syslog priority |
| `audit_log_exclude_accounts` | charptr | global | NULL | Comma-separated list of excluded accounts (user@host) |
| `audit_log_include_accounts` | charptr | global | NULL | Comma-separated list of included accounts |
| `audit_log_exclude_databases` | charptr | global | NULL | Comma-separated list of excluded databases |
| `audit_log_include_databases` | charptr | global | NULL | Comma-separated list of included databases |
| `audit_log_exclude_commands` | charptr | global | NULL | Comma-separated list of excluded commands |
| `audit_log_include_commands` | charptr | global, NULL | Comma-separated list of included commands |
| `audit_log_anonymized_ip` | charptr | global | NULL | Comma-separated list of IPs to anonymize |
| `audit_log_force_rotate` | bool | global | false | Setting to ON forces immediate log rotation |
| `audit_log_csv2_truncation` | bool | global | true | Truncate long SQL in CSV2 format |
| `audit_log_csv2_escape` | bool | global | false | Enable escaping in CSV2 format |
| `audit_log_csv2_old_separated_format` | bool | global | false | Use old CSV2 format with spaces |
| `audit_log_thread_safe` | bool | global, readonly | true | Enable thread-safe writing for FILE handler |

---

## 7. EXPLAIN Output

Not applicable. Audit Log does not affect query execution plans.

---

## 8. Error Handling

- **File write error**: Logged once via `my_plugin_log_message()` with `MY_ERROR_LEVEL`; subsequent errors suppressed
- **Buffer full (performance mode)**: Records are dropped if buffer is full and `can_drop_data` is true
- **Memory allocation failure**: `check_memory_reallocate()` distinguishes between total failure (assert) and reuse-with-truncation (warning)
- **Invalid filter configuration**: Include and exclude are mutually exclusive; validation functions reject setting include if exclude is already set and vice versa
- **DBUG test hooks**: `simulate_audit_log_buffer_malloc_fail`, `simulate_audit_malloc_fail` for testing failure paths
- **Plugin init failure**: If logger cannot be opened, `audit_log_is_active` remains false and all events are silently skipped

---

## 9. Memory Management

- **Per-THD record buffer**: Allocated via `THDVAR(record_buffer)`, grows as needed, freed when THD ends
- **Query stack**: Allocated via `THDVAR(query_stack)`, tracks nested query execution
- **Filter hash tables**: Allocated with PSI instrumented keys (`key_memory_audit_log_accounts`, etc.), freed at plugin deinit
- **Logger handle**: Heap-allocated with rotation metadata, freed at `logger_close()`
- **Ring buffer**: Heap-allocated with configurable size, freed at `audit_log_buffer_shutdown()`
- **PSI memory instrumentation**: 7 PSI memory keys for tracking audit log allocations

---

## 10. Test Coverage

42 MTR test cases in `plugin/audit_log/tests/mtr/`:

| Category | Test Cases |
|----------|-----------|
| Format tests | `audit_log_csv2`, `audit_log_json`, `audit_log_charset`, `audit_log_csv2_truncation_with_escape`, `audit_log_csv2_truncation_without_escape` |
| Policy tests | `audit_log_policy` |
| Filtering tests | `audit_log_filter_only_user`, `audit_log_filter_db`, `audit_log_filter_events`, `audit_log_filter_db_events` |
| IP anonymization | `audit_log_anonymizied_ip`, `audit_log_anonymizied_ip_csv2` |
| Buffer tests | `audit_log_buffer` |
| Rotation tests | `audit_log_format_switch` |
| Bug fixes | `audit_log_bug_2021072001278`, `audit_log_charset_BUG2023080903059`, `audit_log_memory_out_of_bounds`, `audit_log_malloc_fail_crash` |
| Thread safety | `audit_log_thread_safe_2` |
| Plugin install | `audit_log_bp_prepare`, `audit_log_install_bug1435606` |

---

## 11. Limitations

- CSV/table output is not supported for OMA log; only FILE and SYSLOG handlers
- Include/exclude filters are mutually exclusive per category
- Syslog handler has limited configuration options and is read-only after plugin init
- CSV2 format truncation may lose SQL text for very long queries
- No built-in log compression for rotated files
- IP anonymization only replaces numeric digits, not full masking
- The ring buffer in performance mode may drop records under heavy load
- Plugin uses MySQL audit API v1 which has limited event types

---

## 12. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **logger.h/file_logger.cc** - File logging with rotation support
2. **audit_handler.h** - Handler interface definition
3. **audit_file.cc** - File-based handler implementation
4. **audit_syslog.cc** - Syslog-based handler implementation
5. **buffer.h/buffer.cc** - Ring buffer for asynchronous mode
6. **filter.h/filter.cc** - Include/exclude filter implementation
7. **audit_log.h** - PSI keys and shared definitions
8. **audit_log.cc** - Main plugin with notification callback, formatting, system variables
9. **CMakeLists.txt** - Build configuration
10. **MTR test suite** - Port all test cases
