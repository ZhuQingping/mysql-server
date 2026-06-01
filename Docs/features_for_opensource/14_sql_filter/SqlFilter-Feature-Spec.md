# SQL Filter Feature Specification

> Version: 2.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

In high-concurrency database environments, certain SQL patterns can overwhelm the system. Without a mechanism to filter and control SQL execution by rules, a sudden spike in specific query types (e.g., heavy SELECTs or bulk UPDATEs) can degrade performance for all users. There is no built-in way to set concurrency limits on specific SQL patterns.

### 1.2 Solution

Implement a SQL Filter framework that allows administrators to define rules that limit the maximum concurrency of SQL statements matching specific keyword patterns and statement types. Rules are persisted in a MySQL system table (`mysql.rds_sql_filter_rules`) and loaded into an in-memory cache for fast runtime evaluation. SQL statements that exceed the concurrency limit for a matching rule are rejected (killed) immediately.

### 1.3 Execution Model

```
SQL statement arrives
  -> sql_parse.cc: Check opt_rds_sqlfilter_control
  -> limit_query_by_sqlfilter(thd)
     -> Obtain filter type from SQL command (SELECT/UPDATE/DELETE/INSERT)
     -> Check reserved user and system table exemptions
     -> find_matched_filter_and_update(thd, type)
        -> Iterate rules in reverse order for matching type
        -> Check node_id match (if applicable)
        -> Check keywords match in order (case-sensitive or not)
        -> If matched: CAS-increment m_cur_concur
           -> If m_cur_concur >= m_max_concurrency: REJECT query
           -> Otherwise: allow, store filter_id in THD
  -> [If allowed] Execute query
  -> dec_filter_item_conc(thd)
     -> CAS-decrement m_cur_concur for the matched filter
```

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| SQL filter core (`sql/sql_filter/`) | 8 | ~6,400 |
| SQL layer modifications | ~20 | ~4,900 |
| MTR tests | ~26 | ~1,000 |
| **Total** | **~54** | **~12,300** |

---

## 2. New Files

### 2.1 SQL Filter Core (`sql/sql_filter/`)

| File | Lines | Purpose |
|------|-------|---------|
| `sql_filter_common.h/.cc` | 63/32 | Memory allocator, string type, PSI keys for SQL filter |
| `sql_filter_interface.h/.cc` | 53/89 | Public API: `sqlfilter_init()`, `sqlfilter_destroy()`, `limit_query_by_sqlfilter()`, `dec_filter_item_conc()` |
| `sql_filter_table_common.h/.cc` | 106/160 | `Sql_filter_record` struct, `SqlFilterType` enum, validation, trim utilities |
| `sql_filter_cache.h/.cc` | 191/476 | `Sql_filter` class, `System_sql_filter` singleton, in-memory rule cache with RW-lock |
| `sql_filter_table.h/.cc` | 154/396 | `Sql_filter_reader/writer` for `mysql.rds_sql_filter_rules` table, CRUD operations |
| `sql_filter_proc.h/.cc` | 415/366 | `dbms_sqlfilter` package procedures: add/delete/show/flush/update |

---

## 3. Modifications to Existing Files

### 3.1 `sql/sql_class.h` (THD class)

```c++
// New member in THD:
uint filter_id{0};  // ID of the SQL filter rule matched by current query (0 = no match)
```

### 3.2 `sql/sql_parse.cc`

```c++
// In dispatch_command(), before query execution:
if (opt_rds_sqlfilter_control && !thd->lex->sphead && !thd->sp_runtime_ctx &&
    limit_query_by_sqlfilter(thd)) {
  // Query rejected by SQL filter
  sql_filter_block_num.fetch_add(1);
  // ... kill the query
}
```

After query execution:
```c++
if (thd->filter_id != 0) {
  dec_filter_item_conc(thd);
}
```

### 3.3 `sql/mysqld.h`

```c++
extern bool opt_rds_sqlfilter_control;
extern bool rds_sqlfilter_affect_admin_user;
extern bool opt_rds_sqlfilter_case_sensitive;
extern bool rds_sqlfilter_sys_table_control;
extern ulong rds_sqlfilter_rules_max_count;
```

### 3.4 `sql/mysqld.cc`

- Global variable definitions for all SQL filter system variables
- Call to `sqlfilter_init()` during server startup
- Call to `statement_sqlfilter_init()` after bootstrap
- Call to `sqlfilter_destroy()` during server shutdown

### 3.5 `sql/sys_vars.cc`

Five new system variables defined (see Section 7).

---

## 4. Core Data Structures

### 4.1 SqlFilterType Enum

```c++
enum class SqlFilterType : int {
  SELECT = 0,
  UPDATE,
  DELETE,
  INSERT,
  LAST    // sentinel, not a valid type
};
```

### 4.2 Sql_filter_record

Represents a single SQL filter rule, used for both table I/O and cache manipulation:

```c++
struct Sql_filter_record : public Conf_record {
  ulonglong m_id;              // Unique rule ID (auto-increment)
  SqlFilterType m_type;        // SELECT/UPDATE/DELETE/INSERT
  ulonglong m_max_concurrency; // Maximum allowed concurrent executions
  LEX_CSTRING m_key_str;       // Keyword pattern, "~" separated
  LEX_CSTRING m_node_id;       // Optional node identifier for cluster awareness

  std::atomic<ulong> m_cur_concur;     // Current concurrent execution count
  std::atomic<ulong> m_block_query_num; // Number of queries rejected by this rule

  virtual bool check_valid(const char **msg) const override;
  virtual ulonglong get_id() const override { return m_id; }
  virtual void set_id(ulonglong value) override { m_id = value; }
};
```

### 4.3 Sql_filter

In-memory representation of a filter rule stored in the cache:

```c++
class Sql_filter {
 public:
  ulonglong m_id;
  SqlFilterType m_type;
  ulonglong m_max_concurrency;
  String_sqlfilter m_key_str;
  String_sqlfilter m_node_id;

  std::atomic<ulong> m_cur_concur;
  std::atomic<ulong> m_block_query_num;
  std::vector<std::string> m_key_array;  // Keywords split from m_key_str by "~"

  Sqlfilter_update_error update_sqlfilter_rule(const Sql_filter_record *record);
};
```

The `m_key_array` is constructed by splitting `m_key_str` on the `~` delimiter:
```
m_key_str = "SELECT~FROM~order"
m_key_array = ["SELECT", "FROM", "order"]
```

### 4.4 System_sql_filter

Singleton that manages all filter rules in memory:

```c++
class System_sql_filter {
  using Sql_filter_map = std::map<ulonglong, Sql_filter *>;

  // RAII lock helper for RW-lock
  class Lock_helper : public Disable_unnamed_object {
   public:
    explicit Lock_helper(mysql_rwlock_t *lock, bool exclusive) {
      if (exclusive) mysql_rwlock_wrlock(m_lock);
      else mysql_rwlock_rdlock(m_lock);
      m_locked = true;
    }
    void unlock();
    ~Lock_helper() { if (m_locked) mysql_rwlock_unlock(m_lock); }
   private:
    bool m_locked;
    mysql_rwlock_t *m_lock;
  };

  Sql_filter_map *m_rule_map[4];  // One map per SqlFilterType
  mysql_rwlock_t *m_lock[4];       // One RW-lock per SqlFilterType

  static System_sql_filter *m_system_sql_filter;

  void add_records(Conf_records *records);
  void flush_records(Conf_records *records);
  bool delete_sql_filter(ulonglong id);
  Sqlfilter_update_error update_sql_filter(ulonglong id, Conf_record *record);
  bool find_matched_filter_and_update(THD *thd, SqlFilterType type);
  bool limit_query_by_sqlfilter(THD *thd);
  void dec_filter_item_conc(THD *thd);
  void aggregate_sql_filters(Sqlfilter_show_result_container *container);
};
```

### 4.5 mysql.rds_sql_filter_rules Table

```sql
CREATE TABLE mysql.rds_sql_filter_rules (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  type VARCHAR(32) NOT NULL,           -- SELECT/UPDATE/DELETE/INSERT
  max_concurrency BIGINT UNSIGNED NOT NULL,
  keywords VARCHAR(1024) NOT NULL,     -- "~" separated keyword pattern
  node_id VARCHAR(64) DEFAULT NULL,    -- Optional cluster node identifier
  PRIMARY KEY (id)
) ENGINE=InnoDB;
```

### 4.6 Sqlfilter_show_result

Used to return filter state in `show_sql_filter()`:

```c++
class Sql_filter_show_result {
  ulonglong m_id;
  SqlFilterType m_type;
  ulonglong m_max_concurrency;
  String_sqlfilter m_key_str;
  String_sqlfilter m_node_id;
  std::atomic<ulong> m_cur_concur;
  std::atomic<ulong> m_block_query_num;
  std::vector<std::string> m_key_array;
};
```

---

## 5. Execution Flow

### 5.1 SQL Filter Initialization

```
Server startup:
  -> sqlfilter_init()
     -> Register PSI RW-lock keys
     -> Allocate System_sql_filter singleton
     -> Allocate per-type Sql_filter_map and mysql_rwlock_t

  -> statement_sqlfilter_init(bootstrap)
     -> reload_sqlfilter_rules(thd)
        -> Open mysql.rds_sql_filter_rules table
        -> Read all rows into Conf_records
        -> System_sql_filter::flush_records(records)
           -> For each SqlFilterType:
              -> Wr-lock m_lock[type]
              -> Clear m_rule_map[type]
              -> For each record of matching type:
                 -> Create Sql_filter from record
                 -> Insert into m_rule_map[type]
              -> Unlock
```

### 5.2 Query Filtering Flow

```
sql_parse.cc: dispatch_command()
  -> if opt_rds_sqlfilter_control && !sphead && !sp_runtime_ctx:
     -> limit_query_by_sqlfilter(thd)
        -> System_sql_filter::limit_query_by_sqlfilter(thd)
           -> obtain_filter_type(thd->lex->sql_command)
              -> SQLCOM_SELECT -> SELECT
              -> SQLCOM_UPDATE/UPDATE_MULTI -> UPDATE
              -> SQLCOM_DELETE/DELETE_MULTI -> DELETE
              -> SQLCOM_INSERT/INSERT_SELECT -> INSERT
              -> Other -> LAST (skip filtering)
           -> Check reserved user exemption:
              -> is_rds_reserved_user() or root (if !rds_sqlfilter_affect_admin_user)
           -> Check system table exemption (if !rds_sqlfilter_sys_table_control):
              -> If query accesses mysql/information_schema/performance_schema/sys DB, skip
           -> find_matched_filter_and_update(thd, filter_type)
              -> Rd-lock m_lock[type]
              -> Iterate m_rule_map[type] in reverse order
              -> For each filter:
                 -> Check node_id match:
                    -> If both thd->node_id and rule->node_id are non-empty and non-equal: skip
                 -> Check keyword match:
                    -> contains_keywords_in_order(thd, filter->m_key_array)
                       -> For each keyword in m_key_array:
                          -> find_first() in thd->query() string
                          -> Search continues from position after last match
                          -> Case sensitivity controlled by opt_rds_sqlfilter_case_sensitive
                       -> Return true only if ALL keywords found in order
                 -> If matched:
                    -> If m_max_concurrency == 0: REJECT immediately (always block)
                    -> CAS loop on m_cur_concur:
                       -> Load current value
                       -> If >= m_max_concurrency: REJECT, increment m_block_query_num
                       -> Otherwise: CAS increment, store filter_id in THD, ALLOW
              -> Unlock

  -> [If REJECTED]
     -> Set KILL_QUERY flag on THD
     -> sql_filter_block_num.fetch_add(1)
     -> Return error to client

  -> [If ALLOWED]
     -> Execute query normally
     -> After execution:
        -> if thd->filter_id != 0:
           -> dec_filter_item_conc(thd)
              -> Rd-lock m_lock[type]
              -> Find filter by thd->filter_id
              -> CAS loop on m_cur_concur:
                 -> Decrement if > 0 (protect against underflow from flush)
              -> thd->filter_id = 0
              -> Unlock
```

### 5.3 Keyword Matching Algorithm

```c++
static bool contains_keywords_in_order(THD *thd, const std::vector<std::string> &keywords) {
  char *query_str = const_cast<char *>(thd->query().str);
  int query_len = thd->query().length;
  size_t nums = 0;

  int (*find_func)(const char *, int, const char *, int);
  find_func = opt_rds_sqlfilter_case_sensitive ? &find_first<true> : &find_first<false>;

  for (const auto &keyword : keywords) {
    if (keyword.empty()) continue;
    int pos = find_func(query_str, query_len, keyword.c_str(), keyword.size());
    if (pos != -1) {
      query_str = query_str + pos + keyword.size();
      query_len = query_len - pos - keyword.size();
      nums++;
    } else {
      return false;
    }
  }
  return (nums == keywords.size());
}
```

Key properties:
- Keywords must appear in the SQL text **in order** (not necessarily consecutively)
- Matching starts from the position after the previous match
- If `rds_sqlfilter_case_sensitive` is OFF (default), matching is case-insensitive

### 5.4 dbms_sqlfilter Procedures

| Procedure | Parameters | Description |
|-----------|-----------|-------------|
| `dbms_sqlfilter.add_sql_filter(type, max_concurrency, key_str, node_id)` | VARCHAR, BIGINT, VARCHAR, VARCHAR | Add a new filter rule |
| `dbms_sqlfilter.delete_sql_filter(id)` | BIGINT | Delete a filter rule by ID |
| `dbms_sqlfilter.show_sql_filter()` | (none) | Show all filter rules with current statistics |
| `dbms_sqlfilter.flush_sql_filter()` | (none) | Reload all rules from `mysql.rds_sql_filter_rules` table |
| `dbms_sqlfilter.update_sql_filter(id, type, max_concurrency, key_str, node_id)` | BIGINT, VARCHAR, BIGINT, VARCHAR, VARCHAR | Update an existing filter rule |

**Add flow**:
```
CALL dbms_sqlfilter.add_sql_filter("SELECT", 10, "SELECT~FROM~order", "")
  -> Sql_cmd_sqlfilter_proc_add::pc_execute(thd)
     -> get_record(thd)  // Parse parameters
     -> record->check_record_valid(&msg)  // Validate
     -> add_sql_filter(thd, record)
        -> Open mysql.rds_sql_filter_rules table for write
        -> Sql_filter_writer::store_attributes()  // Insert row
        -> System_sql_filter::add_records(records)
           -> Wr-lock m_lock[type]
           -> Create Sql_filter from record
           -> Insert or replace in m_rule_map[type]
           -> Unlock
```

**Flush flow**:
```
CALL dbms_sqlfilter.flush_sql_filter()
  -> reload_sqlfilter_rules(thd)
     -> Open mysql.rds_sql_filter_rules table for read
     -> Sql_filter_reader: read all rows into Conf_records
     -> System_sql_filter::flush_records(records)
        -> For each type: Wr-lock, clear map, repopulate, unlock
```

---

## 6. System Variables

| Variable | Type | Scope | Default | Description |
|----------|------|-------|---------|-------------|
| `rds_sqlfilter_control` | bool | global | false | Enable/disable SQL filter functionality |
| `rds_sqlfilter_affect_admin_user` | bool | global | false | Whether SQL filter applies to admin (root) user |
| `rds_sqlfilter_case_sensitive` | bool | global | false | Whether keyword matching is case-sensitive |
| `rds_sqlfilter_sys_table_control` | bool | global | false | Whether SQL filter applies to system table queries |
| `rds_sqlfilter_rules_max_count` | ulong | global | 100 | Maximum number of SQL filter rules allowed |

---

## 7. EXPLAIN Output

Not applicable. SQL Filter does not affect query execution plans; it either allows or rejects queries before optimization.

---

## 8. Error Handling

- **Rule validation**: `Sql_filter_record::check_valid()` verifies type is valid, max_concurrency is non-negative, key_str is non-empty
- **Invalid record**: `my_error(ER_SQLFILTER_INVALID, ...)` with descriptive message
- **Flush failure**: `my_error(ER_LOAD_SQL_FILTER_RULE, ...)` if table read fails
- **Concurrency underflow protection**: In `dec_filter_item_conc()`, CAS loop only decrements if `m_cur_concur > 0` to prevent underflow when `flush_sql_filter()` resets counters during high concurrency
- **Memory allocation failure**: `allocate_sqlfilter_object<T>()` returns nullptr, checked by callers
- **Rule count exceeded**: `rds_sqlfilter_rules_max_count` limits the number of rules; attempts to add beyond limit are rejected

---

## 9. Memory Management

- All SQL filter objects are allocated via `allocate_sqlfilter_object<T>()` which uses `im::allocate_object<T>()` with `key_memory_Sql_filter` PSI key
- Strings use `String_sqlfilter` (aliased from `im::String_template<Sqlfilter_alloc>`)
- `Sql_filter_map` is `std::map<ulonglong, Sql_filter *>` -- filter objects are heap-allocated
- Destruction: `System_sql_filter::~System_sql_filter()` iterates all maps, destroys each `Sql_filter` object, then destroys map and lock objects
- `System_sql_filter` singleton is allocated at init and freed at destroy, never per-query

---

## 10. Test Coverage

MTR test cases in `mysql-test/suite/sql_filter/t/`:

| Test | Description |
|------|-------------|
| `sqlfilter_basic` | Basic add/delete/show/flush operations |
| `sqlfilter_add` | Add filter rules with various parameters |
| `sqlfilter_update` | Update existing filter rules |
| `sqlfilter_binlog` | Verify filter operations not written to binlog |
| `sqlfilter_concurrency` | Concurrency limiting behavior |
| `sqlfilter_debug` | Debug sync point testing |
| `sqlfilter_enhance` | Enhanced features (node_id, case sensitivity) |
| `sqlfilter_memory` | Memory usage and leak testing |
| `sqlfilter_rules_max` | Maximum rule count enforcement |
| `sqlfilter_table_basic` | Table-level CRUD operations |
| `sqlfilter_table_modify_save` | Table modifications persist across restart |
| `sqlfilter_bugfix` | Regression tests for known bugs |
| `sqlfilter_BUG2024053003932` | Specific bug regression test |

---

## 11. Limitations

- Only four SQL command types are filterable: SELECT, UPDATE, DELETE, INSERT
- Keyword matching is substring-based, not parse-tree-based (may match inside string literals or comments)
- Keywords must appear in order in the SQL text
- No support for regular expressions in keyword patterns
- Reserved users (rds_reserved_user, optionally root) are always exempt
- System table queries are optionally exempt (controlled by `rds_sqlfilter_sys_table_control`)
- `node_id` filtering requires application to set `node_id_ptr` global variable
- `flush_sql_filter()` resets `m_cur_concur` to 0, which may temporarily cause under-counting
- Maximum 100 rules by default (configurable up to `rds_sqlfilter_rules_max_count`)

---

## 12. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **sql_filter_common** - Memory allocator, string types, PSI keys
2. **sql_filter_table_common** - `Sql_filter_record`, `SqlFilterType` enum, validation
3. **sql_filter_cache** - `Sql_filter` class, `System_sql_filter` singleton, rule matching
4. **sql_filter_table** - Reader/writer for `mysql.rds_sql_filter_rules` table
5. **sql_filter_interface** - Public API (`sqlfilter_init`, `limit_query_by_sqlfilter`, etc.)
6. **sql_filter_proc** - `dbms_sqlfilter` package procedures
7. **THD modification** - Add `filter_id` member to `THD` class
8. **sql_parse.cc integration** - Add filter check before query execution
9. **mysqld.cc** - Add global variable definitions, init/destroy calls
10. **sys_vars.cc** - Add system variable definitions
11. **Create system table** - `mysql.rds_sql_filter_rules` DDL
12. **MTR test suite** - Port all test cases
