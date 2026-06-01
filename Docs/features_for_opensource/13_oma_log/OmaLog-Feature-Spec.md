# OMA Log Feature Specification

> Version: 2.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

Database administrators need a dedicated log that records DDL (Data Definition Language) statements for operational monitoring and auditing purposes. The existing slow query log and general log are not suitable because they log all statements (not just DDL), produce excessive volume, and lack DDL-specific metrics like lock time and rows examined.

### 1.2 Solution

Implement an OMA (Operations, Maintenance, and Administration) log that selectively records DDL statements to a dedicated file (`oma.log`). The OMA log follows the same file-based logging pattern as the slow query log, but applies different filtering criteria: only DDL commands are logged, and internal system users (e.g., binlog replay) are excluded.

### 1.3 Log Format

Each OMA log entry consists of:
```
# Time: <ISO8601 timestamp>
# User@Host: <user_host_info>  Id: <thread_id>
# Query_time: <seconds>  Lock_time: <seconds>  Rows_sent: <n>  Rows_examined: <n>
use <database>;
SET timestamp=<unix_timestamp>;
<DDL_statement>;
```

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| SQL layer modifications | ~6 | ~1,381 |
| MTR tests | ~2 | ~100 |
| **Total** | **~8** | **~1,481** |

---

## 2. New Files

No new source files are created. The OMA log feature is integrated into existing logging infrastructure (`sql/log.h`, `sql/log.cc`).

New MTR test file:
- `mysql-test/t/oma_log.test` - Tests for OMA log enable/disable and DDL recording

---

## 3. Modifications to Existing Files

### 3.1 `sql/log.h`

Added `QUERY_LOG_OMA` to the `enum_log_table_type` enumeration:

```c++
enum enum_log_table_type {
  QUERY_LOG_NONE = 0,
  QUERY_LOG_SLOW = 1,
  QUERY_LOG_GENERAL = 2,
  QUERY_LOG_OMA = 3    // NEW
};
```

Added `log_oma()` virtual method to `Log_event_handler` base class:

```c++
class Log_event_handler {
  // ... existing methods ...

  virtual bool log_oma(THD *thd, ulonglong current_utime,
                       ulonglong query_start_utime, const char *user_host,
                       ulonglong query_utime, ulonglong lock_utime,
                       const char *sql_text, size_t sql_text_len) = 0;
};
```

Added `log_oma()` implementation to `Log_to_csv_event_handler` (asserts false -- CSV not supported):

```c++
bool log_oma(THD *thd MY_ATTRIBUTE((unused)), ...) override {
  DBUG_ASSERT(false);  // CSV format not supported
  return false;
}
```

Added `oma_log_handler_list` to `Query_logger`:

```c++
class Query_logger {
  // ... existing members ...
  Log_event_handler *oma_log_handler_list[MAX_LOG_HANDLERS_NUM + 1];

  // New public method:
  bool oma_log_write(THD *thd, const char *query, size_t query_length);
};
```

Added free functions:

```c++
bool log_oma_applicable(THD *thd);
void log_oma(THD *thd, bool force_write = false);
```

### 3.2 `sql/log.cc`

#### 3.2.1 `File_query_log::write_oma()` Method

New method added to the `File_query_log` class (defined locally in `log.cc`) that writes OMA log entries:

```c++
bool File_query_log::write_oma(THD *thd, ulonglong current_utime,
                               ulonglong query_start_utime,
                               const char *user_host, ulonglong query_utime,
                               ulonglong lock_utime, const char *sql_text,
                               size_t sql_text_len) {
  // 1. Lock LOCK_log mutex
  // 2. Write "# Time: <timestamp>" line
  // 3. Write "# User@Host: <user_host>  Id: <thread_id>" line
  // 4. Write "# Query_time: <s>  Lock_time: <s>  Rows_sent: <n>  Rows_examined: <n>" line
  // 5. Write "use <db>;\n" if database changed
  // 6. Write "SET timestamp=<unix_ts>;\n"
  // 7. Write the SQL text + ";\n"
  // 8. Flush IO cache
}
```

#### 3.2.2 `Log_to_file_event_handler` Class

Added `mysql_oma_log` member:

```c++
class Log_to_file_event_handler : public Log_event_handler {
  File_query_log mysql_general_log;
  File_query_log mysql_slow_log;
  File_query_log mysql_oma_log;  // NEW

  bool log_oma(...) override;    // NEW - delegates to mysql_oma_log.write_oma()
};
```

The constructor initializes `mysql_oma_log` with `QUERY_LOG_OMA`:

```c++
Log_to_file_event_handler()
    : mysql_general_log(QUERY_LOG_GENERAL),
      mysql_slow_log(QUERY_LOG_SLOW),
      mysql_oma_log(QUERY_LOG_OMA) {}
```

#### 3.2.3 `Query_logger` OMA Support

```c++
// In init_query_log():
case QUERY_LOG_OMA:
  return (opt_oma_log && (log_output_options & LOG_TABLE));

// In get_query_log():
else if (log_type == QUERY_LOG_OMA)
  return &mysql_oma_log;

// In set_handlers():
else if (log_type == QUERY_LOG_OMA)
  // Sets up oma_log_handler_list based on log_output_options
```

#### 3.2.4 `log_oma_applicable()` Function

Determines whether the current statement should be logged to OMA log:

```c++
static bool log_oma_applicable(THD *thd, bool &log_rewritten_query) {
  // Skip failed or rolled back queries
  if (thd->transaction_rollback_request || thd->is_error()) return false;

  // Always-log cases:
  // - SQLCOM_CALL, SQLCOM_LOCK_TABLES, SQLCOM_UNLOCK_TABLES
  // - SQLCOM_CREATE_PROCEDURE/CREATE_SPFUNCTION (only if sphead != nullptr)
  // - SQLCOM_CREATE_EVENT (only if event_parse_data != nullptr)
  // - SQLCOM_FLUSH (only if REFRESH_READ_LOCK or REFRESH_FOR_EXPORT)

  // Classify using get_ddl_command_type():
  ddl_command_type ddl_type = get_ddl_command_type(thd);
  log_rewritten_query = (ddl_type == DDLCOM_PRIVI);  // Use rewritten query for privilege commands
  return (ddl_type != DDLCOM_NONE);
}
```

#### 3.2.5 `log_oma_is_filtered_user()` Function

Filters out internal system users:

```c++
static bool log_oma_is_filtered_user(THD *thd) {
  // Skip:
  // 1. mysqld process initialization (user="" and host="")
  // 2. Binlog replay ("skip-grants user")
  // 3. System memory table initialization ("skip-grants user")
  const LEX_CSTRING cur_user = thd->security_context()->priv_user();
  const LEX_CSTRING cur_host = thd->security_context()->priv_host();
  bool is_sysuser = (cur_user.str[0] == 's' && !strcmp(cur_user.str, "skip-grants user"))
                    || (cur_user.length == 0);
  bool is_syshost = (cur_host.str[0] == 's' && !strcmp(cur_host.str, "skip-grants host"))
                    || (cur_host.length == 0);
  return (is_sysuser && is_syshost);
}
```

#### 3.2.6 `log_oma()` Function (Top-level Entry Point)

```c++
void log_oma(THD *thd, bool force_write) {
  if (!opt_oma_log) return;

  if (unlikely(force_write)) {
    THD_STAGE_INFO(thd, stage_logging_oma);
    query_logger.oma_log_write(thd, thd->query().str, thd->query().length);
    return;
  }

  bool log_rewritten_query = false;
  if (log_oma_applicable(thd, log_rewritten_query)) {
    if (log_oma_is_filtered_user(thd)) return;
    THD_STAGE_INFO(thd, stage_logging_oma);
    if (log_rewritten_query && thd->rewritten_query().length())
      query_logger.oma_log_write(thd, thd->rewritten_query().ptr(),
                                 thd->rewritten_query().length());
    else
      query_logger.oma_log_write(thd, thd->query().str, thd->query().length);
  }
}
```

### 3.3 `sql/mysqld.h`

```c++
extern bool opt_general_log, opt_slow_log, opt_general_log_raw, opt_oma_log;
extern char *opt_oma_logname;
```

### 3.4 `sql/mysqld.cc`

- Global variable definitions:
  - `bool opt_oma_log = true;` (enabled by default)
  - `char oma_logname_path[FN_REFLEN];`
  - `char *opt_oma_logname;`

- Server startup: Initialize OMA log file path and open the log
  ```c++
  query_logger.set_log_file(QUERY_LOG_OMA);
  if (opt_oma_log && query_logger.reopen_log_file(QUERY_LOG_OMA))
    opt_oma_log = false;
  ```

- Validation: Check that OMA log file name is valid and that log_output includes FILE

### 3.5 `sql/sys_vars.cc`

Two new system variables:

```c++
// File path variable
static Sys_var_charptr Sys_rds_oma_log_path(
    "rds_oma_log_file",
    "Log file path for OMA log (record DDL statements).",
    GLOBAL_VAR(opt_oma_logname), CMD_LINE(REQUIRED_ARG), IN_FS_CHARSET,
    DEFAULT(nullptr), ON_UPDATE(fix_oma_log_file));

// Enable/disable variable
static Sys_var_bool Sys_rds_oma_log(
    "rds_oma_log", "Record DDL statements to a log file oma.log.",
    GLOBAL_VAR(opt_oma_log), CMD_LINE(OPT_ARG), DEFAULT(true), NO_MUTEX_GUARD,
    NOT_IN_BINLOG, ON_CHECK(nullptr), ON_UPDATE(fix_oma_log_state));
```

Update functions:
- `fix_oma_log_file()`: Handles path changes, reopens the log file
- `fix_oma_log_state()`: Handles enable/disable, activates or deactivates the log handler

### 3.6 `sql/sql_parse.cc`

Call to `log_oma(thd)` after statement execution:

```c++
// In dispatch_command(), after query execution completes:
log_oma(thd);
```

### 3.7 `sql/sql_reload.cc`

Added `REFRESH_OMA_LOG` handling for `FLUSH LOGS`:

```c++
options |= REFRESH_OMA_LOG;
// ...
if ((options & REFRESH_OMA_LOG) && opt_oma_log &&
    query_logger.reopen_log_file(QUERY_LOG_OMA))
  result = true;
```

---

## 4. Core Data Structures

### 4.1 DDL Command Classification (`sql/ddl_info.h`)

The `ddl_command_type` enum classifies SQL commands for OMA log filtering:

```c++
enum ddl_command_type {
  DDLCOM_NONE = 0,   // Not a DDL command
  DDLCOM_PRIVI,       // Privilege-related (CREATE/DROP USER, GRANT, REVOKE)
  DDLCOM_ACL,         // ACL-related (stored procedure/function ops)
  DDLCOM_OTHER        // Other DDL (CREATE/ALTER/DROP TABLE, INDEX, etc.)
};

ddl_command_type get_ddl_command_type(THD *thd);
```

### 4.2 Log Infrastructure Reuse

The OMA log reuses the existing `File_query_log` class, which provides:
- Thread-safe writes via `LOCK_log` mutex
- IO_CACHE-based buffered file I/O
- File rotation via `FLUSH LOGS`
- Same file naming and path resolution as slow/general logs

---

## 5. Execution Flow

### 5.1 OMA Log Write Flow

```
Statement execution completes
  -> sql/sql_parse.cc: log_oma(thd)
     -> Check opt_oma_log is enabled
     -> log_oma_applicable(thd, log_rewritten_query)
        -> Skip failed/rolled-back queries
        -> Special cases: CALL, LOCK/UNLOCK TABLES always logged
        -> get_ddl_command_type(thd) -> classify as DDLCOM_*
        -> Return true if not DDLCOM_NONE
     -> log_oma_is_filtered_user(thd)
        -> Skip skip-grants users and empty user/host
     -> query_logger.oma_log_write(thd, query, length)
        -> Iterate oma_log_handler_list
        -> handler->log_oma(thd, ...)
           -> Log_to_file_event_handler::log_oma()
              -> mysql_oma_log.write_oma(thd, ...)
                 -> Lock LOCK_log
                 -> Write timestamp, user/host, metrics, SQL text
                 -> Flush IO cache
                 -> Unlock LOCK_log
```

### 5.2 DDL Command Classification Flow

```
get_ddl_command_type(thd) called from log_oma_applicable()
  -> Switch on thd->lex->sql_command
     -> Privilege commands (SQLCOM_CREATE_USER, SQLCOM_DROP_USER,
        SQLCOM_GRANT, SQLCOM_REVOKE, SQLCOM_ALTER_USER, etc.)
        -> DDLCOM_PRIVI
     -> ACL commands (SQLCOM_CREATE_PROCEDURE, SQLCOM_CREATE_SPFUNCTION,
        SQLCOM_DROP_PROCEDURE, SQLCOM_DROP_FUNCTION)
        -> DDLCOM_ACL
     -> Standard DDL (SQLCOM_CREATE_TABLE, SQLCOM_ALTER_TABLE,
        SQLCOM_DROP_TABLE, SQLCOM_CREATE_INDEX, SQLCOM_DROP_INDEX, etc.)
        -> DDLCOM_OTHER
     -> Everything else
        -> DDLCOM_NONE
```

### 5.3 Log Rotation Flow

```
FLUSH LOGS command
  -> sql/sql_reload.cc
     -> REFRESH_OMA_LOG flag set
     -> query_logger.reopen_log_file(QUERY_LOG_OMA)
        -> Close current OMA log file
        -> Reopen with same path
```

---

## 6. System Variables

| Variable | Type | Scope | Default | Description |
|----------|------|-------|---------|-------------|
| `rds_oma_log` | bool | global | true | Enable/disable OMA logging |
| `rds_oma_log_file` | charptr | global | `<datadir>/oma.log` | Path to the OMA log file |

---

## 7. EXPLAIN Output

Not applicable. OMA log does not affect query execution plans.

---

## 8. Error Handling

- **File write error**: `check_and_print_write_error()` prints `ER_ERROR_ON_WRITE` once; subsequent errors are suppressed.
- **Invalid log file name**: Validated in `mysqld.cc` using `is_valid_log_name()`. Rejects `.ini` and `.cnf` extensions.
- **Log output mismatch**: If `opt_oma_log` is enabled but `log_output` does not include FILE, a warning is printed at startup.
- **Memory allocation failure**: `opt_oma_logname` allocation failure returns error and disables OMA logging.

---

## 9. Memory Management

- OMA log uses the existing `File_query_log` infrastructure which manages its own `IO_CACHE` and file handles.
- The `opt_oma_logname` string is allocated via `my_strdup()` with `key_memory_LOG_name`.
- The `LOCK_log` mutex protects concurrent writes; no additional memory management is needed.
- All log infrastructure is freed during server shutdown via `Query_logger::cleanup()`.

---

## 10. Test Coverage

MTR test cases:
- `mysql-test/t/oma_log.test` - Basic OMA log enable/disable, DDL recording verification
- `mysql-test/t/log_state.test` - OMA log file path changes, `FLUSH LOGS` behavior

Test categories:
| Category | Description |
|----------|-------------|
| Enable/disable | `rds_oma_log` ON/OFF switching |
| File path | `rds_oma_log_file` path changes |
| DDL recording | CREATE/DROP/ALTER TABLE logged |
| DML filtering | SELECT/INSERT/UPDATE/DELETE not logged |
| User filtering | skip-grants user excluded |
| Privilege commands | Rewritten query used for password desensitization |
| FLUSH LOGS | Log file rotation |

---

## 11. Limitations

- CSV/table output is not supported; OMA log only writes to file
- OMA log does not record DML statements (INSERT/UPDATE/DELETE/SELECT)
- No log rate throttling (unlike slow query log)
- No log file size limit or automatic rotation (only manual `FLUSH LOGS`)
- System users (binlog replay, initialization) are always excluded
- Failed or rolled-back statements are not logged

---

## 12. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **Add `QUERY_LOG_OMA` enum** in `sql/log.h`
2. **Add `log_oma()` virtual method** to `Log_event_handler` and both subclasses
3. **Add `write_oma()` method** to `File_query_log` class in `sql/log.cc`
4. **Add `mysql_oma_log` member** to `Log_to_file_event_handler`
5. **Add `oma_log_handler_list`** and `oma_log_write()` to `Query_logger`
6. **Implement `log_oma_applicable()`** using `get_ddl_command_type()` from `sql/ddl_info.h`
7. **Implement `log_oma_is_filtered_user()`** to exclude system users
8. **Implement `log_oma()`** top-level function
9. **Add system variables** `rds_oma_log` and `rds_oma_log_file` in `sql/sys_vars.cc`
10. **Add global variables** in `sql/mysqld.cc` and `sql/mysqld.h`
11. **Add `REFRESH_OMA_LOG`** handling in `sql/sql_reload.cc`
12. **Add `log_oma(thd)` call** in `sql/sql_parse.cc` after statement execution
13. **Add MTR test cases** for OMA log functionality
