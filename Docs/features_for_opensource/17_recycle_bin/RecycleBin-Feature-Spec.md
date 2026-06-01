# Recycle Bin Feature Specification

> Version: 1.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

When a user executes `DROP TABLE` or `TRUNCATE TABLE`, the table and its data are permanently deleted. Accidental drops can cause data loss that is difficult or impossible to recover from, especially in production environments. MySQL provides no built-in mechanism to recover dropped tables.

### 1.2 Solution

Implement a recycle bin that intercepts `DROP TABLE` and `TRUNCATE TABLE` operations. Instead of permanently deleting the table, it is renamed into a special `__recyclebin__` schema. The table remains there until either the user explicitly purges it or the retention period expires, at which point a background scheduler thread automatically purges it. Users can restore tables from the recycle bin via stored procedures.

The recycle bin is controlled by the session variable `rds_recycle_bin_mode`. When enabled, `DROP TABLE` becomes a `RENAME TABLE` into `__recyclebin__`, and `TRUNCATE TABLE` becomes: (1) rename original table to `__recyclebin__`, (2) create a new empty table with the same definition.

### 1.3 Execution Model

```
DROP TABLE db1.t1 (with recycle bin ON):
  1. Check if table supports recycle bin
  2. Generate recycle name: "db1@t1@<timestamp_hex>"
  3. RENAME TABLE db1.t1 -> __recyclebin__.db1@t1@<timestamp_hex>
  4. Store origin_schema and origin_table in dd::Table options

TRUNCATE TABLE db1.t1 (with recycle bin ON):
  1. Check if table supports recycle bin
  2. RENAME TABLE db1.t1 -> __recyclebin__.db1@t1@<timestamp_hex>
  3. Reset all DD object IDs (table, columns, indexes, etc.)
  4. CREATE TABLE db1.t1 with original definition (empty)

RESTORE: CALL dbms_recyclebin.restore_table("db1@t1@hex", "new_db", "new_table")
  -> RENAME TABLE __recyclebin__.db1@t1@hex -> new_db.new_table

PURGE: CALL dbms_recyclebin.purge_table("db1@t1@hex")
  -> DROP TABLE __recyclebin__.db1@t1@hex (permanent deletion)
```

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| New SQL layer (`sql/recyclebin/`) | 10 | ~3,000 |
| MySQL core modifications | ~20 | ~500 |
| MTR tests | ~32 | ~5,000 |
| **Total** | **~62** | **~8,500** |

---

## 2. New Files

### 2.1 Recycle Bin Module (`sql/recyclebin/`)

| File | Lines | Purpose |
|------|-------|---------|
| `recycle.h` | 139 | Core types: `Recycle_state`, `Recycle_lex`, `Thd_recycle_state_guard`, `recycle_bin_cmd_type` enum |
| `recycle_table.h` | 352 | Table-level operations: `recycle_base_table`, `recycle_truncate_table`, `recycle_purge_table`, `restore_db_inner`, `check_support_logical_drop`, `is_recycle_bin_work`, `get_recycle_tables` |
| `recycle_table.cc` | 1180 | Implementation of all table recycle/restore/purge logic |
| `recycle_proc.h` | 409 | Stored procedure definitions: `Recycle_proc_show`, `Recycle_proc_purge`, `Recycle_proc_restore`, `Recycle_proc_restore_db` and their SQL command classes |
| `recycle_proc.cc` | 689 | Implementation of `dbms_recyclebin` stored procedures |
| `recycle_scheduler.h` | 185 | Background purge thread: `Recycle_scheduler` class |
| `recycle_scheduler.cc` | 524 | Scheduler thread implementation: start/stop/run/purge_table |
| `recycle_cache.cc` | 133 | PSI instrumentation initialization: mutex/cond/thread/memory keys |
| `recycle_auth.h` | 52 | `Recycle_internal_schema_access`: ACL control for `__recyclebin__` schema |
| `recycle_auth.cc` | 63 | Access control implementation |

---

## 3. Modifications to Existing MySQL Files

### 3.1 `sql/sql_class.h` (THD class)

```c++
// Forward declaration
class Recycle_state;

// In THD class:
std::unique_ptr<im::recycle_bin::Recycle_state> recycle_state;
```

Each THD owns a `Recycle_state` that tracks whether the current command is a recycle, restore, or purge operation.

### 3.2 `sql/system_variables.h`

```c++
// In System_variables:
bool rds_recycle_bin_mode;
```

Session-level flag to enable/disable the recycle bin for the current session.

### 3.3 `sql/mysqld.h` / `sql/mysqld.cc`

```c++
// Global variables:
extern bool rds_recycle_bin_skip_acl_check;
extern bool extended_partitions_enabled;  // also used by Feature 18
```

### 3.4 `sql/sys_vars.cc`

New system variables:

```c++
// Session variable: enable/disable recycle bin
static Sys_var_bool Sys_rds_recycle_bin_mode(
    "rds_recycle_bin_mode", "Enable the recycle bin.",
    SESSION_VAR(rds_recycle_bin_mode), CMD_LINE(OPT_ARG), DEFAULT(false),
    NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(0), ON_UPDATE(recycle_bin_mode_update));

// Global variable: scheduler interval
static Sys_var_ulong Sys_recycle_scheduler_interval(
    "rds_recycle_scheduler_interval", "Interval in seconds for recycle scheduler.",
    GLOBAL_VAR(im::recycle_bin::rds_recycle_scheduler_interval),
    CMD_LINE(REQUIRED_ARG), VALID_RANGE(1, 60), DEFAULT(30), BLOCK_SIZE(1));

// Global variable: retention period
static Sys_var_ulong Sys_recycle_bin_retention(
    "rds_recycle_bin_retention", "Seconds before really purging the recycled table.",
    GLOBAL_VAR(im::recycle_bin::rds_recycle_bin_retention),
    CMD_LINE(REQUIRED_ARG), VALID_RANGE(1, 30 * 24 * 60 * 60),
    DEFAULT(3 * 24 * 60 * 60), BLOCK_SIZE(1), ...);

// Global variable: enable/disable scheduler
static Sys_var_bool Sys_recycle_scheduler(
    "rds_recycle_scheduler", "Enable the recycle scheduler.",
    GLOBAL_VAR(im::recycle_bin::rds_recycle_scheduler), ...);

// Global variable: scheduler verbose
static Sys_var_bool Sys_recycle_scheduler_purge_table_print(
    "rds_recycle_scheduler_purge_table_print", ...);

// Global variable: skip ACL check (for replica nodes)
static Sys_var_bool Sys_recycle_bin_skip_acl_check(
    "rds_recycle_bin_skip_acl_check", ...);
```

The `recycle_bin_mode_update` callback creates the `__recyclebin__` schema when the variable is set to ON:

```c++
static bool recycle_bin_mode_update(sys_var *self, THD *thd, enum_var_type type) {
  if (!read_only && sv->rds_recycle_bin_mode) {
    return im::recycle_bin::recreate_recycle_bin_schema(thd);
  }
  return false;
}
```

### 3.5 `sql/sql_table.cc` (DROP TABLE integration)

At the point where `DROP TABLE` processes each table in the list:

```c++
im::recycle_bin::Thd_recycle_state_guard state_guard(thd);
Recycle_check_result res =
    im::recycle_bin::check_support_logical_drop(thd, table);
if (res == im::recycle_bin::Recycle_check_result::SUPPORTED) {
  if (im::recycle_bin::recycle_base_table(thd, table, post_ddl_htons,
                                           rb_fk_invalidator)) {
    // Error during recycle, report to client
  }
} else if (res == im::recycle_bin::Recycle_check_result::CHECK_ERROR) {
  // Pre-check failed, report error
} else {
  // NOT_SUPPORTED: continue with normal DROP TABLE
  thd->recycle_state->reset();
}
```

Also in `mysql_rename_table()`: When the source is `__recyclebin__` (a restore operation), the `__origin_schema__` and `__origin_table__` options are removed from the DD table definition.

Also in `rm_table_do_discovery_and_rename_fk()`: During full synchronization, if the database is `__recyclebin__`, the origin name fields are populated from the table name format.

### 3.6 `sql/sql_truncate.cc` (TRUNCATE TABLE integration)

```c++
im::recycle_bin::Recycle_result res =
    im::recycle_bin::Recycle_result::CONTINUE;
if (im::recycle_bin::is_recycle_bin_work(thd)) {
  res = im::recycle_bin::recycle_truncate_table(
      thd, path, table_list, create_info, update_create_info,
      is_temp_table, table_def);
}
if (res == im::recycle_bin::Recycle_result::ERROR) {
  // report error
} else if (res == im::recycle_bin::Recycle_result::CONTINUE) {
  // continue with normal TRUNCATE
}
```

### 3.7 `sql/handler.h` (Handler virtual method)

```c++
// In handler class:
virtual int ha_check_support_recyclebin(const char *name,
                                        bool &support_recyclebin,
                                        const dd::Table *dd_table);
```

Storage engines override this to indicate whether a specific table supports being recycled. InnoDB checks for FTS, data directory, and discarded tablespace.

### 3.8 `sql/sql_db.cc` (DROP DATABASE integration)

When dropping a database, each table in the database is recycled into `__recyclebin__` instead of being permanently deleted.

### 3.9 `sql/sql_show.cc` / `sql/sql_show.h`

Integration points for showing recycle bin tables and filtering `__recyclebin__` schema from normal table listings.

### 3.10 `sql/sql_rename.cc`

The `mysql_rename_tables()` function is reused for both recycling (moving tables into `__recyclebin__`) and restoring (moving tables out of `__recyclebin__`).

### 3.11 `sql/lock.cc`

MDL lock handling adjustments for recycle bin operations.

### 3.12 `sql/log_event.cc` / `sql/log_event.h`

Binlog event handling for recycle bin operations, including special handling for the `__recyclebin__` schema.

### 3.13 `sql/sql_parse.cc`

SQL parse integration for recycle bin stored procedure calls.

### 3.14 `sql/sql_hidden.cc`

Hidden table handling to avoid recycling system/hidden tables.

---

## 4. Core Data Structures

### 4.1 recycle_bin_cmd_type

Enum that labels the current thread state for special handling:

```c++
enum recycle_bin_cmd_type {
  RECYCLE_BIN_NONE = 0,
  RECYCLE_BIN_RECYCLE_TABLE = 1,   // DROP TABLE -> rename to __recyclebin__
  RECYCLE_BIN_RECYCLE_DB = 2,      // DROP DATABASE -> rename all tables
  RECYCLE_BIN_PURGE = 4,           // Permanently drop from __recyclebin__
  RECYCLE_BIN_RESTORE_TABLE = 8,   // Restore single table
  RECYCLE_BIN_RESTORE_DB = 16,     // Restore all tables from a DB
  RECYCLE_BIN_TRUNCATE_TABLE = 32  // TRUNCATE -> recycle original + create new
};
```

### 4.2 Recycle_state

Thread-local state attached to each THD:

```c++
class Recycle_state {
  recycle_bin_cmd_type m_type;
  bool m_has_autoinc = false;

  void set_type(recycle_bin_cmd_type type);
  recycle_bin_cmd_type get_type();
  bool is_priv_relax();   // true for PURGE, RESTORE_TABLE, RESTORE_DB
  bool is_recycle();      // true for RECYCLE_TABLE, RECYCLE_DB, TRUNCATE_TABLE
  bool is_restore();      // true for RESTORE_TABLE, RESTORE_DB
  bool is_purge();        // true for PURGE
  void set_autoinc_flag(bool has_autoinc);
  bool has_autoinc_col();
  void reset();
};
```

`is_priv_relax()` returns true for operations where normal ACL restrictions on the `__recyclebin__` schema are relaxed (purge and restore).

### 4.3 Thd_recycle_state_guard

RAII guard that saves and restores `THD::lex->sql_command` and `THD::recycle_state->m_type`:

```c++
class Thd_recycle_state_guard {
  THD *m_thd;
  enum_sql_command m_cmd;
  recycle_bin_cmd_type m_type;
  // Constructor saves, destructor restores
};
```

### 4.4 Recycle_lex

RAII guard that temporarily replaces the current LEX with a fresh one, used by the purge procedure:

```c++
class Recycle_lex {
  THD *m_thd;
  LEX *m_backed_up_lex;
  LEX m_lex;
  // Constructor: thd->lex = &m_lex; lex_start(thd);
  // Destructor: lex_end(&m_lex); thd->lex = m_backed_up_lex;
};
```

### 4.5 Recycle_show_result

Result structure for `dbms_recyclebin.show_tables()`:

```c++
struct Recycle_show_result {
  LEX_STRING schema;          // "__recyclebin__"
  LEX_STRING table;           // "db1@t1@abc12345"
  LEX_STRING origin_schema;   // "db1"
  LEX_STRING origin_table;    // "t1"
  ulonglong recycled_time;    // from dd::Table::last_altered()
};
```

### 4.6 Recycle_check_result / Recycle_result

Two enums for the two-phase check pattern:

```c++
enum class Recycle_check_result {
  SUPPORTED = 0,     // Table can be recycled
  NOT_SUPPORTED,     // Table doesn't support recycle, continue normal DROP
  CHECK_ERROR        // Pre-check failed, report error to client
};

enum class Recycle_result {
  OK,                // Recycle succeeded
  CONTINUE,          // Not supported, continue with normal operation
  ERROR              // Recycle operation failed
};
```

### 4.7 Recycle_scheduler

Background thread that periodically checks for expired recycled tables and purges them:

```c++
class Recycle_scheduler {
  enum class State { INITED, RUNNING, STOPPING };
  State m_state;
  THD *m_scheduler_thd;
  mysql_cond_t m_state_cond;
  mysql_mutex_t m_state_mutex;
  mysql_mutex_t m_sleep_mutex;
  mysql_cond_t m_sleep_cond;

  bool start();
  bool stop();
  void run(THD *thd);
  bool purge_table(Recycle_show_result *recycle_table);
  void wakeup();
  bool is_running();
  void reset(bool need_lock);

  static Recycle_scheduler *m_recycle_scheduler;
  static Recycle_scheduler *instance();
};
```

### 4.8 Recycle_internal_schema_access

ACL enforcement for the `__recyclebin__` schema:

```c++
class Recycle_internal_schema_access : public ACL_internal_schema_access {
  ACL_internal_access_result check(Access_bitmask want_access,
                                   Access_bitmask *save_priv,
                                   bool any_combination_will_do) const override;
  const ACL_internal_table_access *lookup(const char *name) const override;
};
```

For normal users: INSERT, UPDATE, DELETE, CREATE, DROP, REFERENCES, INDEX, ALTER, EXECUTE, CREATE_VIEW, CREATE_PROC, ALTER_PROC, EVENT, TRIGGER are all **denied** on `__recyclebin__`. SELECT and GRANT are allowed.

For SUPER_ACL users or when `recycle_state->is_priv_relax()` is true: all operations are allowed (needed for purge and restore).

### 4.9 Timestamp_timezone_guard

RAII guard that temporarily sets the session timezone to UTC (offset 0) for consistent timestamp handling during recycle table queries:

```c++
class Timestamp_timezone_guard {
  Time_zone *m_tz;
  THD *m_thd;
  // Constructor: saves tz, sets UTC
  // Destructor: restores original tz
};
```

---

## 5. Execution Flow

### 5.1 DROP TABLE with Recycle Bin

```
mysql_rm_table()  [sql/sql_table.cc]
  -> For each table in the drop list:
     -> Thd_recycle_state_guard (sets recycle_state type)
     -> check_support_logical_drop(thd, table)
         -> is_recycle_bin_work(thd)   // check mode + skip_acl_check
         -> is_support_logical_drop(thd, table)
             -> Server-layer checks:
                1. Table must not already be in __recyclebin__
                2. Not in locked_tables_mode
                3. Table must exist in DD and not already have __origin_schema__ option
                4. Not hidden (HT_HIDDEN_SE)
                5. Not in system databases (mysql, information_schema, performance_schema, sys)
                6. No secondary engine
                7. foreign_key_checks must be ON
                8. Not inside trigger execution
                9. Storage engine must be InnoDB or DStore
                10. Table/db name must not contain "@"
                11. Combined name length <= 54 chars (name) / 233 bytes (filename)
             -> Engine-layer checks (ha_check_support_recyclebin):
                1. No fulltext index
                2. No DATA DIRECTORY
                3. No discarded tablespace
     -> If SUPPORTED: recycle_base_table(thd, table)
         -> prepare_recycle_table(thd, table, &res)
             -> Build rename table list: [old_db.old_table -> __recyclebin__.new_name]
             -> generate_recycle_table_name()
                 Format: "<db>@<table>@<timestamp_hex>"
                 timestamp = thd->query_start_in_secs() & 0xFFFFFFFF
                 Retry: if name collision, sleep 1s and regenerate
             -> Acquire MDL Exclusive lock on new table name
             -> Check new name doesn't exist in DD
         -> mysql_rename_tables(thd, rename_table_list)
         -> On rename success: dd::Table options are set with
            __origin_schema__ and __origin_table__
     -> If NOT_SUPPORTED: continue with normal DROP TABLE
     -> If CHECK_ERROR: report error to client
```

### 5.2 TRUNCATE TABLE with Recycle Bin

```
Sql_cmd_truncate_table::truncate_table()  [sql/sql_truncate.cc]
  -> If is_recycle_bin_work(thd):
     -> recycle_truncate_table(thd, path, table_list, create_info, ...)
         -> Set recycle_state type to RECYCLE_BIN_TRUNCATE_TABLE
         -> check_support_logical_drop()
         -> If SUPPORTED:
            1. recycle_base_table(thd, table_list)
               (rename original table to __recyclebin__)
            2. Reset all DD object IDs:
               - dd::Table::id = INVALID_OBJECT_ID
               - All dd::Column IDs
               - All dd::Index IDs
               - All dd::Foreign_key IDs
               - All dd::Trigger IDs
               - All dd::Check_constraint IDs
               - All dd::Partition and subpartition IDs
            3. Sql_command_backup: temporarily set sql_command = SQLCOM_CREATE_TABLE
            4. ha_create_table(): create new empty table with same definition
               Pass original_create_info to preserve SE attributes (data_file_name, tablespace)
         -> If NOT_SUPPORTED: return CONTINUE (normal truncate)
         -> If ERROR: report error
```

### 5.3 Restore Table

```
CALL dbms_recyclebin.restore_table("recycle_name" [, "new_db", "new_table"])

Sql_cmd_recycle_proc_restore::prepare()
  1. Check rds_recycle_bin_mode is ON
  2. Parse parameters (1 or 3 arguments)
  3. If new_db is __recyclebin__: error
  4. Acquire MDL locks on recycle bin table
  5. Look up table in DD, verify it has __origin_schema__ and __origin_table__
  6. If no new_db/new_table: use original schema/table from DD options
  7. Build table_list: [__recyclebin__.old_table -> new_db.new_table]
  8. Check access: ALTER_ACL|DROP_ACL on old table, INSERT_ACL|CREATE_ACL on new

Sql_cmd_recycle_proc_restore::pc_execute()
  -> Set sql_command = SQLCOM_RENAME_TABLE
  -> mysql_rename_tables(thd, first_table)
  -> On rename: __origin_schema__ and __origin_table__ options are removed
```

### 5.4 Restore Database

```
CALL dbms_recyclebin.restore_db("origin_db" [, "new_db"])

Sql_cmd_recycle_proc_restore_db::pc_execute()
  -> restore_db_inner(thd, origin_db, new_db, &restored_tables, ...)
     1. lock_recycle_schema()
     2. get_recycle_tables(): fetch all recycled tables
     3. For each table where origin_schema matches:
        a. Acquire MDL Exclusive lock
        b. prepare_restore_table(): build rename list
        c. check_table_access()
        d. mysql_rename_tables()
        e. Increment restored_tables counter
     4. write_bin_log()
  -> trans_commit_stmt() / trans_commit()
  -> Post-DDL hooks for handlerton
  -> Foreign key parent invalidation
```

### 5.5 Purge Table

```
CALL dbms_recyclebin.purge_table("recycle_name")

Sql_cmd_recycle_proc_purge::pc_execute()
  -> Set sql_command = SQLCOM_DROP_TABLE
  -> recycle_purge_table(thd, table)
     1. Recycle_lex: fresh LEX context
     2. Disable_autocommit_guard
     3. check_readonly()
     4. lock_recycle_schema()
     5. Acquire MDL Exclusive on table
     6. is_recycled_table(): verify table exists in __recyclebin__
     7. drop_base_recycle_table(thd, tables)
        -> mysql_rm_table() with RECYCLE_BIN_PURGE state
```

### 5.6 Show Recycled Tables

```
CALL dbms_recyclebin.show_tables()

Sql_cmd_recycle_proc_show::send_result()
  1. get_recycle_tables(thd, mem_root, &container)
     - Fetch __recyclebin__ schema from DD
     - For each table in schema:
       a. Acquire MDL Shared lock
       b. Check table has __origin_schema__ and __origin_table__ options
       c. Check SELECT_ACL access (skip for scheduler thread)
       d. Build Recycle_show_result
  2. Send result set columns:
     SCHEMA | TABLE | ORIGIN_SCHEMA | ORIGIN_TABLE | RECYCLED_TIME | PURGE_TIME
  3. PURGE_TIME = RECYCLED_TIME + rds_recycle_bin_retention
```

### 5.7 Recycle Scheduler (Background Thread)

```
recycle_scheduler_thread()
  -> pre_init_recycle_thread(thd)   // set SUPER_ACL, localhost, COM_DAEMON
  -> post_init_recycle_thread(thd)  // store_globals, add to THD manager
  -> Recycle_scheduler::run(thd)
     Loop:
       1. If not read_only and not skip_acl_check:
          get_recycle_tables()
          For each recycled table:
            delta = current_time - recycled_time
            If delta >= rds_recycle_bin_retention:
              purge_table()   // permanent deletion
            Else:
              min_future = min(min_future, retention - delta)
       2. Sleep until min(min_future, scheduler_interval)
          - Can be woken up by stop() or wakeup() (retention change)
       3. Continue loop until state == STOPPING
  -> deinit_recycle_thread(thd)
```

The scheduler calculates the minimum wait time as the earliest purge deadline. It wakes up earlier if `rds_recycle_bin_retention` is changed (via `recycle_scheduler_loop_version` increment).

---

## 6. System Variables

| Variable | Type | Scope | Default | Dynamic | Description |
|----------|------|-------|---------|---------|-------------|
| `rds_recycle_bin_mode` | bool | SESSION | false | Yes | Enable/disable recycle bin for this session |
| `rds_recycle_bin_retention` | ulong | GLOBAL | 259200 (3 days) | Yes | Seconds before auto-purging recycled tables |
| `rds_recycle_scheduler` | bool | GLOBAL | false | Yes | Enable/disable the background purge scheduler |
| `rds_recycle_scheduler_interval` | ulong | GLOBAL | 30 | Yes | Scheduler loop interval in seconds (1-60) |
| `rds_recycle_scheduler_purge_table_print` | bool | GLOBAL | false | Yes | Print detailed info when purging tables |
| `rds_recycle_bin_skip_acl_check` | bool | GLOBAL | false | Yes | Skip ACL checks (for replica nodes), also disables recycle bin |

**Startup options**: `--rds-recycle-bin-mode`, `--rds-recycle-bin-retention=N`, `--rds-recycle-scheduler`, etc.

---

## 7. Stored Procedures

All procedures are in the `dbms_recyclebin` schema:

| Procedure | Parameters | Description |
|-----------|-----------|-------------|
| `dbms_recyclebin.show_tables()` | none | List all recycled tables with origin info, recycled time, and purge time |
| `dbms_recyclebin.purge_table(table_name)` | 1: recycle table name | Permanently delete a recycled table |
| `dbms_recyclebin.restore_table(table_name [, new_db, new_table])` | 1 or 3 | Restore a table; optional destination db and table name |
| `dbms_recyclebin.restore_db(db_name [, new_db])` | 1 or 2 | Restore all tables from a dropped database |

All procedures inherit from `Recycle_proc_base` (which extends `Proc`) and use `Sql_cmd_recycle_proc_base` (which extends `Sql_cmd_admin_proc` with `PRIV_NONE_ACL`).

---

## 8. Recycle Bin Table Naming Convention

The recycle bin table name encodes the original schema, table, and a timestamp ID:

```
Format: <original_db>@<original_table>@<hex_timestamp>
Example: db1@t1@abc12345

Constraints:
  - db + table character length <= 54 characters
  - db + table filename bytes <= 233 bytes
  - Neither db nor table name may contain "@"
  - hex_timestamp = query_start_in_secs() & 0xFFFFFFFF (8 hex digits)
  - If name collision: sleep 1s and regenerate with new timestamp
  - Slave threads use the master's timestamp directly (no regeneration)
```

The `parse_origin_table_by_table_name()` function reverses this encoding to extract the original schema and table name.

---

## 9. DD Table Options for Recycle Bin

When a table is moved to `__recyclebin__`, two options are stored in `dd::Table::options()`:

```
__origin_schema__  =  original database name
__origin_table__   =  original table name
```

These are set during `rm_table_do_discovery_and_rename_fk()` in `sql/sql_table.cc`. When a table is restored, these options are removed from the DD definition.

The `last_altered` timestamp of the DD table is used as the `recycled_time` (set via `dd::my_time_t_to_ull_datetime`).

---

## 10. ACL / Permission Model

- `__recyclebin__` schema is registered as an internal schema with `Recycle_internal_schema_access`
- Normal users can only SELECT and GRANT on tables in `__recyclebin__`
- Write operations (INSERT, UPDATE, DELETE, CREATE, DROP, ALTER, etc.) are denied for normal users
- SUPER_ACL users bypass the restriction
- `recycle_state->is_priv_relax()` also bypasses the restriction for PURGE and RESTORE operations
- `rds_recycle_bin_skip_acl_check = true` disables the recycle bin entirely and bypasses ACL

---

## 11. Error Handling

| Error Condition | Handling |
|----------------|----------|
| Recycle bin name collision | Sleep 1s, regenerate timestamp, retry once |
| `__recyclebin__` schema doesn't exist | Auto-created when `rds_recycle_bin_mode` is set to ON |
| Table not support recycle | Fall through to normal DROP/TRUNCATE |
| Foreign key checks OFF | Skip recycle, continue normal DROP |
| Table in system database | Skip recycle, continue normal DROP |
| Table has FTS / DATA DIRECTORY / discarded tablespace | Skip recycle, continue normal DROP |
| Engine check returns error | Clear error, skip recycle |
| Restore to `__recyclebin__` | Report error |
| Purge table that doesn't exist | Report error (except for slave threads: silently succeed) |
| Scheduler purge error | Log warning, continue to next table |
| Scheduler thread creation fails | Log error, set scheduler to not running |

Key error codes:
- `ER_RECYCLEBIN_SWITCH_OFF`: recycle bin is disabled
- `ER_PREPARE_RECYCLE_TABLE_ERROR`: generic recycle bin error
- `ER_RECYBIN_NOT_RECYCLED_TABLE`: table is not a recycled table
- `ER_RECYBIN_NO_MATCH`: no matching tables found for restore

---

## 12. Memory Management

- All recycle bin allocations use `MEM_ROOT` (thd->mem_root or dedicated mem_root)
- `allocate_recycle_object<T>()` allocates with PSI instrumentation (`key_memory_recycle`)
- `Recycle_show_result` objects allocated on the caller's `MEM_ROOT`
- Scheduler thread uses its own `MEM_ROOT` that is cleared each loop iteration
- `Thd_recycle_state_guard` and `Timestamp_timezone_guard` are stack-allocated RAII guards
- `Recycle_lex` owns a stack LEX object; does not heap-allocate

---

## 13. MDL Lock Handling

Recycle bin operations require careful MDL lock management:

1. **Recycle (DROP -> rename to __recyclebin__)**:
   - MDL Exclusive on the original table (acquired by DROP TABLE)
   - MDL Exclusive on the new table name in `__recyclebin__` (acquired in `generate_recycle_table_name`)
   - MDL Intention Exclusive on `__recyclebin__` schema

2. **Restore (rename from __recyclebin__)**:
   - MDL Exclusive on `__recyclebin__` table (acquired in prepare)
   - MDL Exclusive on destination table
   - MDL Intention Exclusive on both schemas

3. **Purge (DROP from __recyclebin__)**:
   - MDL Exclusive on the recycle bin table
   - Schema-level lock on `__recyclebin__`

4. **Scheduler**:
   - Acquires MDL locks per table during purge
   - Releases all locks after each purge

`Recycle_table_mdl_guard` is an RAII guard that releases an explicit MDL lock when destroyed.

---

## 14. Binlog / Replication

- Recycle and restore operations are written to the binary log as `RENAME TABLE` statements
- On replica nodes, `rds_recycle_bin_skip_acl_check` is used to control behavior
- Slave threads use the master's timestamp directly (no regeneration of recycle names)
- If a slave purge finds the table doesn't exist, it silently succeeds
- The `rds_recycle_bin_mode` variable is binlogged (IN_BINLOG flag)

---

## 15. Test Coverage

32 MTR test files in `mysql-test/suite/recyclebin/t/`:

| Category | Test Cases |
|----------|-----------|
| Core DROP TABLE | `recycle_table`, `recycle_table-dstore` |
| TRUNCATE TABLE | `recyclebin_instant_ddl_truncate`, `recyclebin_instant_ddl_truncate-dstore` |
| Schema management | `recycle_create_schema`, `check_recyclebin_schema` |
| Restore | `restore_proc`, `restore_case_sensitive`, `restore_case_insensitive` (plus -dstore variants) |
| Purge | `purge_proc`, `purge_proc-dstore` |
| Scheduler | `recycle_scheduler`, `recycle_scheduler-dstore` |
| Foreign key / trigger | `recyclebin_fk_trigger`, `recyclebin_fk_trigger-dstore` |
| Fulltext search | `recycle_fulltext_key` |
| Rollback | `recyclebin_rollback`, `recyclebin_rollback-dstore` |
| Binlog | `recyclebin_binlog`, `recyclebin_binlog-dstore` |

Key DBUG hooks:
- `recyclebin_check_acquire_table_failed`
- `recyclebin_slave_purge_table_not_exist`
- `recyclebin_check_support_recyclebin`
- `recyclebin_truncate_crash_after_rename`
- `recyclebin_truncate_fail_after_rename`
- `recyclebin_truncate_crash_after_create_new_table`
- `recyclebin_truncate_fail_after_create_new_table`
- `recyclebin_copy_with_cs_fail_02..05`
- `recyclebin_restore_alloc_fail_01..04`
- `recyclebin_restore_table_old_db_null`
- `recyclebin_restore_db_fail_before_commit`
- `recyclebin_restore_db_crash_before_commit`
- `recyclebin_restore_db_fail_after_commit`
- `recyclebin_restore_db_crash_after_commit`
- `logical_drop_table_error_01..02`

Key DEBUG_SYNC points:
- `recyclebin_recycle_after_mdl_lock_old_table`
- `recyclebin_recycle_after_mdl_lock_new_table`
- `create_recyclebin_schema_after_lock_schema`

---

## 16. Limitations

- Only InnoDB and DStore storage engines are supported
- Tables with fulltext indexes cannot be recycled
- Tables with DATA DIRECTORY or discarded tablespace cannot be recycled
- System databases (mysql, information_schema, performance_schema, sys) are excluded
- Tables with secondary engine cannot be recycled
- `foreign_key_checks=OFF` disables recycle for the operation
- Operations inside triggers cannot use recycle bin
- Locked tables mode (`LOCK TABLES`) cannot use recycle bin
- Table/db names containing "@" are not supported (name delimiter conflict)
- Combined db+table name must be <= 54 characters (Unicode) and 233 bytes (filename)
- `__recyclebin__` is a reserved schema name; users cannot create it manually
- The recycle bin schema must use utf8mb4 character set
- Downgrade is not supported after recycle bin is used (DD schema created)

---

## 17. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **`sql/recyclebin/recycle.h`** — Core types: `recycle_bin_cmd_type`, `Recycle_state`, `Thd_recycle_state_guard`, `Recycle_lex`
2. **`sql/recyclebin/recycle_cache.cc`** — PSI instrumentation keys and init/deinit
3. **`sql/sql_class.h`** — Add `recycle_state` member to THD
4. **`sql/system_variables.h`** — Add `rds_recycle_bin_mode` to `System_variables`
5. **`sql/mysqld.h` / `sql/mysqld.cc`** — Global variables (`rds_recycle_bin_skip_acl_check`)
6. **`sql/recyclebin/recycle_auth.h/.cc`** — ACL control for `__recyclebin__` schema
7. **`sql/recyclebin/recycle_table.h/.cc`** — All table operations: recycle, restore, purge, check
8. **`sql/handler.h`** — Add `ha_check_support_recyclebin()` virtual method
9. **`sql/sql_table.cc`** — Integrate `check_support_logical_drop()` and `recycle_base_table()` into DROP TABLE
10. **`sql/sql_truncate.cc`** — Integrate `recycle_truncate_table()` into TRUNCATE TABLE
11. **`sql/sql_db.cc`** — Integrate recycle into DROP DATABASE
12. **`sql/recyclebin/recycle_proc.h/.cc`** — Stored procedure definitions and implementations
13. **`sql/recyclebin/recycle_scheduler.h/.cc`** — Background purge scheduler thread
14. **`sql/sys_vars.cc`** — System variable definitions with update callbacks
15. **`sql/sql_rename.cc`** — Handle option cleanup on restore (remove __origin_schema__, __origin_table__)
16. **`sql/sql_show.cc`** — Register `__recyclebin__` as internal schema
17. **`sql/log_event.cc/.h`** — Binlog integration
18. **MTR test suite** — Port test cases
