# Statement Outline Feature Specification

> Version: 1.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

MySQL supports optimizer hints and index hints embedded in SQL text (e.g., `SELECT /*+ MAX_EXECUTION_TIME(1000) */ *`, `SELECT * FROM t FORCE INDEX(idx)`), but these hints must be written directly in application SQL. This creates problems in production environments where:

1. Application SQL cannot be easily modified to add/modify hints.
2. A query plan regression may occur after an upgrade, requiring immediate intervention without changing application code.
3. Different schemas or query patterns may need different optimizer directives without code changes.

### 1.2 Solution

Implement a Statement Outline system that binds optimizer hints and index hints to SQL statements by their DIGEST pattern. Outlines are managed through `dbms_outln` stored procedures and persisted in the `mysql.outline` system table. When a SQL statement is parsed, the system computes its digest hash and looks up matching outlines in a partitioned in-memory cache. If found, the outline hints are injected into the query's `Table_ref` (for index hints) or `Query_block` (for optimizer hints) before optimization.

### 1.3 Execution Model

```
SQL Text
  |
  v
Parser (digest computed)
  |
  v
invoke_outlines(thd, db, digest_storage)
  |
  +--> Index hints --> Table_ref::outline_index_hints
  |
  +--> Optimizer hints --> Query_block::outline_optimizer_list
  |       |
  |       v
  |    contextualize_optimizer_hint()
  |       (parses hint text, applies to query block)
  |
  v
Optimizer (uses injected hints)
```

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| SQL layer (`sql/outline/`) | 14 | ~4,191 |
| Common infrastructure (`sql/common/`) | 2 | ~97 |
| Package integration (`sql/package/`) | partial | ~shared |
| MySQL core modifications | ~15 | ~3,000 |
| MTR tests | 12 test files | ~2,000 |
| **Total** | **~33** | **~9,288** |

---

## 2. New Files

### 2.1 Outline Core (`sql/outline/`)

| File | Lines | Purpose |
|------|-------|---------|
| `outline.h` | 480 | Core data structures: `Outline`, `Statement_outline`, `System_outline`, `Outline_group`, `Lex_optimizer_hint`, `Outline_stats`, `Outline_show_result`, `Outline_preview_result` |
| `outline.cc` | 538 | Outline object lifecycle, index/optimizer hint evocation, `System_outline` map operations, `find_and_fill_hints()` template |
| `outline_common.h` | 72 | Common types: `String_outline`, `Malloc_outline_vector`, `Outline_alloc`, PSI memory/rwlock keys |
| `outline_common.cc` | 67 | String allocator implementation for outline objects |
| `outline_digest.h` | 207 | Digest computation: `Thd_parser_context`, `Parser_error_handler`, `generate_statement_digest()`, `parse_optimizer_hint()`, `contextualize_optimizer_hint()`, `preview_statement_outline()`, `calculate_strip_length_for_explain()` |
| `outline_digest.cc` | 255 | Implementation of digest generation, hint parsing, and preview logic |
| `outline_interface.h` | 75 | Public API: `outline_init()`, `outline_destroy()`, `statement_outline_init()`, `invoke_outlines()`, `enable_digest_by_outline()` |
| `outline_table.h` | 179 | Table I/O: `Outline_reader`, `Outline_writer`, `open_outline_table()`, `reload_outlines()`, `refresh_outline_cache()`, `add_outline()`, `del_outline()` |
| `outline_table.cc` | 524 | Table read/write implementation, `mysql.outline` DDL definition, record serialization |
| `outline_table_common.h` | 143 | Table field enums, `Outline_record`, `Outline_type`, `Outline_scope`, `Outline_state` |
| `outline_table_common.cc` | 107 | Type conversion: `to_outline_type()`, `to_outline_scope()` |
| `outline_proc.h` | 580 | `dbms_outln` procedure definitions: `add_index_outline`, `add_optimizer_outline`, `del_outline`, `flush_outline`, `show_outline`, `preview_outline` |
| `outline_proc.cc` | 595 | Procedure execution implementations |

### 2.2 Common Infrastructure (`sql/common/`)

| File | Lines | Purpose |
|------|-------|---------|
| `component.h` | 200 | Base classes: `PSI_memory_base`, `Disable_copy_base`, `Disable_unnamed_object`, `allocate_object<>()`, `destroy_object<>()`, `Malloc_vector`, `String_template`, `Pair_key_unordered_map`, `split()`, `trim()` |
| `component.cc` | 55 | `Pair_key_comparator` implementation, `std::hash` specialization for `Pair_key_type` |

### 2.3 Table Infrastructure (`sql/common/`)

| File | Purpose |
|------|---------|
| `table_common.h` | `Conf_record`, `Conf_reader`, `Conf_writer`, `Conf_error`, `Conf_records` base classes |
| `table_common.cc` | Base table I/O helpers |
| `table.h` | `open_conf_table()`, `commit_and_close_conf_table()`, `conf_end_trans()` |

---

## 3. Modifications to Existing MySQL Files

### 3.1 `sql/sql_parse.cc`

```c++
// Added includes:
#include "sql/outline/outline_digest.h"
#include "sql/outline/outline_interface.h"

// In parser entry point (around line 6001):
// Force digest computation if outline is enabled
if (im::opt_outline_enabled)
  im::enable_digest_by_outline(parser_state);

// After query parsing (around line 7970):
// Invoke outlines by matching digest
if (!ret_value && thd->m_digest && im::opt_outline_enabled) {
  im::invoke_outlines(thd, thd->db().str,
                      &thd->m_digest->m_digest_storage,
                      strip_length);
}
```

### 3.2 `sql/mysqld.cc`

```c++
#include "sql/outline/outline_interface.h"

// During server initialization (around line 8003):
im::outline_init();

// After DD initialization (around line 8659):
im::statement_outline_init(opt_initialize);

// During server shutdown (around line 3067):
im::outline_destroy();
```

### 3.3 `sql/sql_lex.h` (Query_block class)

```c++
// New members in Query_block:
List<im::Lex_optimizer_hint> *outline_optimizer_list;     // optimizer hints from outline
Query_block *outline_next_query_block{nullptr};           // linked list for outline iteration
```

### 3.4 `sql/table.h` (Table_ref class)

```c++
// New member in Table_ref:
List<Index_hint> *outline_index_hints{nullptr};           // index hints from outline
```

### 3.5 `sql/system_variables.h`

```c++
// New member in System_variables:
bool outline_allowed_sql_digest_truncate;                 // allow truncated digest in add_outline
```

### 3.6 `sql/sys_vars.cc`

```c++
// New system variables:
static Sys_var_ulong Sys_outline_partitions(
    "rds_outline_partitions",
    "How many partition of system outline structure.",
    READ_ONLY GLOBAL_VAR(im::outline_partitions),
    CMD_LINE(REQUIRED_ARG), DEFAULT(16));

static Sys_var_bool Sys_opt_outline_enabled(
    "rds_opt_outline_enabled",
    "it will invoke statement outline when execute sql",
    GLOBAL_VAR(im::opt_outline_enabled),
    CMD_LINE(OPT_ARG), DEFAULT(false));

static Sys_var_bool Sys_outline_allowed_sql_digest_truncate(
    "rds_outline_allowed_sql_digest_truncate",
    "Whether allow the incomplete of sql digest when add outline",
    SESSION_VAR(outline_allowed_sql_digest_truncate),
    CMD_LINE(OPT_ARG), DEFAULT(true));
```

### 3.7 `include/my_sqlcommand.h`

```c++
enum enum_sql_command {
  ...
  SQLCOM_ADMIN_PROC,   // New command type for native procedures
  ...
};
```

### 3.8 Other Modified Files

- `sql/sql_optimizer.cc`: Integrates with `outline_optimizer_list` for optimizer hint application
- `sql/sql_view.cc`: Outline compatibility with views
- `sql/error_handler.cc`: Internal error handler support for outline digest parsing
- `sql/handler.h`: Digest hash size constant usage
- `sql/sql_digest.h`: `compute_digest_hash()` with strip_length parameter
- `share/messages_to_clients.txt`: Error messages ER_OUTLINE_* (10 new error codes)

---

## 4. Core Data Structures

### 4.1 Outline_type Enum

```c++
enum class Outline_type {
  INDEX_IGNORE = 0,
  INDEX_USE,
  INDEX_FORCE,
  OPTIMIZER,
  UNKNOWN
};
```

### 4.2 Outline_scope Structure

```c++
struct Outline_scope {
  LEX_CSTRING str;
  uchar mask;         // INDEX_HINT_MASK_ALL, INDEX_HINT_MASK_JOIN,
                      // INDEX_HINT_MASK_ORDER, INDEX_HINT_MASK_GROUP
};

const Outline_scope outline_scopes[] = {
    {{STRING_WITH_LEN("")}, INDEX_HINT_MASK_ALL},
    {{STRING_WITH_LEN("FOR JOIN")}, INDEX_HINT_MASK_JOIN},
    {{STRING_WITH_LEN("FOR ORDER BY")}, INDEX_HINT_MASK_ORDER},
    {{STRING_WITH_LEN("FOR GROUP BY")}, INDEX_HINT_MASK_GROUP}};
```

### 4.3 Outline_state Enum

```c++
enum class Outline_state {
  OUTLINE_INACTIVE = 0,
  OUTLINE_ACTIVE
};
```

### 4.4 Outline_record Structure

Row-level representation of a `mysql.outline` table record:

```c++
struct Outline_record : public Conf_record {
  ulonglong id;
  LEX_CSTRING schema;
  LEX_CSTRING digest;
  LEX_CSTRING digest_text;
  Outline_type type;
  Outline_scope scope;
  Outline_state state;
  ulonglong pos;        // Position: table position for index hints,
                        // query block number for optimizer hints
  LEX_CSTRING hint;     // Hint text (index names or optimizer hint string)
  LEX_CSTRING query;    // Original SQL (used for digest computation)

  bool check_valid(const char **msg) const override;
  bool optimizer_hint_valid(THD *thd);
  bool index_hint_valid(const char **msg) const;
  ulonglong get_id() const override { return id; }
  void set_id(ulonglong value) override { id = value; }
  bool check_active() const override {
    return state == Outline_state::OUTLINE_ACTIVE;
  }
};
```

### 4.5 Outline Class

In-memory representation of a single outline rule:

```c++
class Outline {
  using Index_array = Prealloced_array<String_outline, 10>;
  static constexpr const char *INDEX_HINT_SEPARATOR = ",";

 public:
  explicit Outline(Outline_record *record);
  explicit Outline(const Outline &other);

  Outline_type get_type() const { return m_type; }
  ulonglong get_pos() const { return m_pos; }
  const String_outline &get_digest() const { return m_digest; }
  const String_outline &get_schema() const { return m_schema; }
  const String_outline &get_hint() const { return m_hint; }
  ulonglong get_id() const { return m_id; }
  const Outline_stats &get_stats() const { return m_stats; }
  Outline_scope get_scope() const { return m_scope; }

  void inc_hit() { m_stats.hit++; }
  void inc_overflow() { m_stats.overflow++; }

  void evoke_index_hint(MEM_ROOT *mem_root, List<Index_hint> *list);
  void evoke_optimizer_hint(MEM_ROOT *mem_root, List<Lex_optimizer_hint> *list);

 private:
  void build_index_hint();  // Splits hint by INDEX_HINT_SEPARATOR

  ulonglong m_id;
  String_outline m_schema;
  String_outline m_digest;
  String_outline m_digest_text;
  Outline_type m_type;
  Outline_scope m_scope;
  Outline_state m_state;
  ulonglong m_pos;
  String_outline m_hint;
  Index_array m_indexes;     // Parsed index names for INDEX type
  Outline_stats m_stats;     // Hit/overflow counters
};
```

### 4.6 Statement_outline Class

Groups all outlines for a single statement (identified by schema + digest):

```c++
class Statement_outline : public Disable_copy_base {
 public:
  using Outline_vector = Malloc_outline_vector<Outline *>;

  explicit Statement_outline(const char *schema, const char *digest);
  void insert(Outline *outline);         // Insert and sort by position
  size_t delete_by_id(ulonglong id);     // Delete outline by ID
  const Outline_vector &outlines() const { return m_outlines; }

 private:
  String_outline m_schema;
  String_outline m_digest;
  Outline_vector m_outlines;             // Sorted by position
};
```

### 4.7 System_outline Class (Singleton Cache)

Partitioned hash map cache with two categories (INDEX and OPTIMIZER):

```c++
class System_outline {
  using Statement_outline_map =
      Pair_key_unordered_map<String_outline, String_outline, Statement_outline>;

  enum class Category {
    CATEGORY_INDEX = 0,
    CATEGORY_OPTIMIZER,
    CATEGORY_COUNT
  };

  class Lock_helper {
    // RAII rwlock wrapper (shared or exclusive)
    Lock_helper(mysql_rwlock_t *lock, bool exclusive);
    void unlock();
    ~Lock_helper();
  };

 public:
  explicit System_outline(size_t partition);
  ~System_outline();

  void fill_records(Conf_records *records, bool force_clean);
  template <typename T, Category outline_category>
  void find_and_fill_hints(THD *thd, T *all_objects,
                           String_outline &schema, String_outline &digest);
  size_t delete_outline(ulonglong id);
  void aggregate_outline(Outline_show_result_container *container);

  static System_outline *m_system_outline;    // Singleton
  static System_outline *instance() { return m_system_outline; }

 private:
  size_t m_partition;                          // Number of partitions (default 16)
  size_t m_elements;                           // Total = partition * CATEGORY_COUNT
  Container<Statement_outline_map *> m_maps;   // Hash maps
  Container<std::atomic_ullong *> m_sizes;     // Size counters
  Container<mysql_rwlock_t *> m_locks;         // Per-partition locks
};
```

**Partition layout**: Each category has `m_partition` partitions. Total elements = `m_partition * 2`. Thread ID determines which partition to access: `thread_id % m_partition + category_offset`.

### 4.8 Lex_optimizer_hint Class

Lightweight structure holding optimizer hint text for a query block:

```c++
class Lex_optimizer_hint {
 public:
  ulonglong query_block;     // Query block position
  LEX_CSTRING hint;          // Hint text (e.g., "MAX_EXECUTION_TIME(1000)")
};
```

### 4.9 Outline_stats Structure

```c++
struct Outline_stats {
  ulonglong hit;           // Number of times this outline was applied
  ulonglong overflow;      // Number of times position didn't match table/query block
};
```

### 4.10 mysql.outline Table Definition

| Field | Type | Description |
|-------|------|-------------|
| `Id` | bigint | Auto-increment primary key |
| `Schema_name` | varchar(64) | Database name (NULL = match all schemas) |
| `Digest` | varchar(64) | SHA-256 digest hash of the SQL statement |
| `Digest_text` | longtext | Normalized SQL text |
| `Type` | enum('IGNORE INDEX','USE INDEX','FORCE INDEX','OPTIMIZER') | Hint type |
| `Scope` | enum('','FOR JOIN','FOR ORDER BY','FOR GROUP BY') | Index hint scope |
| `State` | enum('N','Y') | Active/Inactive flag |
| `Position` | bigint | Table position (index hint) or query block number (optimizer hint) |
| `Hint` | text | Hint content: comma-separated index names or optimizer hint text |

---

## 5. Execution Flow

### 5.1 System Initialization

```
mysqld initialization:
  1. outline_init()
     - Register PSI memory and rwlock keys
     - Create System_outline singleton with outline_partitions partitions
     - Each partition: Statement_outline_map + atomic size + rwlock

  2. statement_outline_init(bootstrap)
     - If not bootstrap: create temporary THD, call reload_outlines()
     - reload_outlines() opens mysql.outline, reads all rows via Outline_reader
     - validate_optimizer_outline() removes invalid optimizer hints
     - refresh_outline_cache() populates System_outline maps
```

### 5.2 Outline Invocation (per SQL statement)

```
sql_parse.cc: parse_sql()

  1. Before parsing:
     if (opt_outline_enabled)
       enable_digest_by_outline(parser_state)
         // Sets parser_state->m_input.m_compute_digest = true

  2. After parsing succeeds:
     if (opt_outline_enabled && thd->m_digest)
       invoke_outlines(thd, db, &digest_storage, strip_length)
         // strip_length: non-zero for EXPLAIN queries to skip EXPLAIN tokens

  3. invoke_outlines() flow:
     a. compute_digest_hash(digest_storage, hash, strip_length)
     b. DIGEST_HASH_TO_STRING(hash, buff)  // 64-char hex string
     c. Create String_outline for schema and digest
     d. invoke_index_outlines():
        - System_outline::find_and_fill_hints<Table_ref, CATEGORY_INDEX>()
          - Locate partition: thread_id % partition
          - Acquire read lock
          - Find Statement_outline by (schema, digest) and ("", digest)
          - Merge matching outlines from both schema-specific and empty-schema entries
          - Walk Table_ref list and outlines together by position
          - For matching position: call Outline::evoke_index_hint()
            which creates Index_hint objects and adds to Table_ref::outline_index_hints
     e. invoke_optimizer_outlines():
        - Reverse query_block list (so position numbering matches)
        - System_outline::find_and_fill_hints<Query_block, CATEGORY_OPTIMIZER>()
          - Similar lookup and position matching
          - For matching position: call Outline::evoke_optimizer_hint()
            which creates Lex_optimizer_hint and adds to Query_block::outline_optimizer_list
        - contextualize_optimizer_hint() for each query_block:
          - Parse each Lex_optimizer_hint text via parse_optimizer_hint()
          - PT_hint_list->contextualize(&pc) applies to query block
```

### 5.3 EXPLAIN Query Handling

EXPLAIN queries require special digest handling. The digest of `EXPLAIN SELECT * FROM t1` should match the outline for `SELECT * FROM t1`, not for the EXPLAIN statement itself.

```c++
uint calculate_strip_length_for_explain(const sql_digest_storage *digest_storage) {
  // Reads tokens from digest_storage to determine how many bytes
  // to strip for EXPLAIN, EXPLAIN ANALYZE, EXPLAIN FORMAT=...
  // Returns byte offset to skip the EXPLAIN prefix tokens
}
```

When `invoke_outlines()` is called with `strip_length > 0`, `compute_digest_hash()` uses the offset to compute the digest from the explained query portion only.

### 5.4 add_index_outline Procedure

```
CALL dbms_outln.add_index_outline(schema, digest, position, type, hint, scope, sql);

  1. Sql_cmd_index_outline_proc_add::pc_execute()
     a. get_record(thd) extracts parameters:
        - schema, digest, position, type (IGNORE/USE/FORCE INDEX),
          hint (index names), scope, sql
     b. If sql is provided:
        - outline_compute_digest():
          - generate_statement_digest() parses SQL and computes digest
          - Verifies computed digest matches provided digest parameter
          - If digest truncation occurred:
            - If outline_allowed_sql_digest_truncate is ON: warning
            - Otherwise: error
     c. Validate record (check_valid)
     d. add_outline(thd, record):
        - Open mysql.outline table for write
        - Disable binlog temporarily
        - Outline_writer::write_row() inserts into table
        - If active: also insert into in-memory cache
          System_outline::fill_records(&records, false)
        - Commit transaction
```

### 5.5 add_optimizer_outline Procedure

```
CALL dbms_outln.add_optimizer_outline(schema, digest, position, hint, sql);

  1. Sql_cmd_optimizer_outline_proc_add::pc_execute()
     a. get_record(thd) extracts parameters
     b. If sql is provided:
        - outline_compute_digest() as above
     c. Parse optimizer hint: parse_optimizer_hint(thd, hint)
        - If parse fails: ER_OUTLINE_OPTIMIZER_HINT_PARSE error
     d. Validate record
     e. add_outline(thd, record) as above
```

### 5.6 del_outline Procedure

```
CALL dbms_outln.del_outline(id);

  1. Sql_cmd_outline_proc_del::pc_execute()
     a. Create Outline_record with the given id
     b. del_outline(thd, record):
        - Open mysql.outline table for write
        - Disable binlog temporarily
        - Outline_writer::delete_row_by_id()
        - Delete from in-memory cache: System_outline::delete_outline(id)
        - If cache entries deleted < partition count: warning (not found in some partitions)
        - Commit transaction
```

### 5.7 flush_outline Procedure

```
CALL dbms_outln.flush_outline();

  1. Sql_cmd_outline_proc_flush::pc_execute()
     a. reload_outlines(thd):
        - Open mysql.outline table for read
        - Outline_reader reads all rows
        - validate_optimizer_outline() removes invalid entries
        - refresh_outline_cache(records, force_clean=true)
          - Clears all existing Statement_outline entries
          - Repopulates from fresh table data
```

### 5.8 show_outline Procedure

```
CALL dbms_outln.show_outline();

  1. Sql_cmd_outline_proc_show::pc_execute() returns false (no error)
  2. send_result() override:
     a. System_outline::aggregate_outline(&results)
        - Iterate all partitions with read lock
        - Merge Outline entries with same ID across partitions
        - Accumulate hit/overflow stats
     b. Send result set columns: ID, SCHEMA, DIGEST, TYPE, SCOPE, POS, HINT, HIT, OVERFLOW, DIGEST_TEXT
     c. Send rows via protocol
```

### 5.9 preview_outline Procedure

```
CALL dbms_outln.preview_outline(schema, query);

  1. Sql_cmd_outline_proc_preview::pc_execute() returns false
  2. send_result() override:
     a. get_record() extracts schema and query
     b. preview_statement_outline():
        - Save and replace THD parser context (Thd_parser_context)
        - Parse the query to compute its digest
        - invoke_outlines() to apply matching outline hints
        - Apply SET_VAR hints if present
        - Walk Table_ref list: generate_outline_preview() for index hints
        - Walk Query_block list: generate_outline_preview() for optimizer hints
        - Restore SET_VAR hints
     c. Send result set columns: SCHEMA, DIGEST, BLOCK_TYPE, BLOCK_NAME, BLOCK, HINT
```

### 5.10 Digest Computation

```c++
bool generate_statement_digest(THD *thd, const LEX_CSTRING &db,
                               const LEX_CSTRING &query,
                               String *digest, String *digest_text,
                               bool *truncated) {
  // 1. Save and replace THD parser context (Thd_parser_context)
  // 2. Initialize Parser_state with the query
  // 3. Parse the SQL (thd->sql_parser())
  // 4. compute_digest_hash() -> SHA-256 hash of normalized tokens
  // 5. DIGEST_HASH_TO_STRING() -> 64-char hex string
  // 6. compute_digest_text() -> normalized SQL text
  // 7. Check if digest was truncated
}
```

---

## 6. System Variables

| Variable | Type | Scope | Default | Description |
|----------|------|-------|---------|-------------|
| `rds_outline_partitions` | ulong | global (read-only) | 16 | Number of partitions in the outline cache structure |
| `rds_opt_outline_enabled` | bool | global | false | Master switch to enable/disable statement outline |
| `rds_outline_allowed_sql_digest_truncate` | bool | session | true | Whether to allow (warning) or reject (error) truncated SQL digest in add_outline |

---

## 7. EXPLAIN Output

Statement outline does not add dedicated EXPLAIN output. However, when outlines are active:

- **Index hints** appear in EXPLAIN as `FORCE INDEX`, `USE INDEX`, or `IGNORE INDEX` in the `possible_keys` and `key` columns
- **Optimizer hints** affect the plan choice (e.g., `MAX_EXECUTION_TIME` is applied)

The `preview_outline` procedure serves as the primary diagnostic tool, showing which outlines would match a given query.

---

## 8. Error Handling

### Error Codes

| Error Code | Name | Message |
|-----------|------|---------|
| ER_OUTLINE_PRIVILEGE_REQUIRED | Outline privilege | "%s" |
| ER_OUTLINE_TABLE_OP_FAILED | Table operation failed | "The table operation failed due to the following error from SE: errcode %d - %.256s" |
| ER_OUTLINE_INVALID | Invalid outline | "Statement outline %d is not valid when %s since %s" |
| ER_STATEMENT_DIGEST_PARSE | Digest parse error | "Statement outline query parse error: %s" |
| ER_OUTLINE_DIGEST_COMPUTE | Digest compute error | "Statement outline digest compute error from %s" |
| ER_OUTLINE_DIGEST_MISMATCH | Digest mismatch | "Statement outline digest didn't match between %s and %s" |
| ER_OUTLINE_NOT_FOUND | Outline not found | "Statement outline %d is not found in %s" |
| ER_OUTLINE_OPTIMIZER_HINT_PARSE | Hint parse error | "Statement outline optimizer hint parse error: %s" |
| ER_OUTLINE_SQL_DIGEST_TRUNCATED | Digest truncated | "Statement outline sql text has been truncated when compute digest: %s" |
| ER_OUTLINE_APPLY_FAILED | Apply failed | "Outline apply failed, cause: %s" |
| ER_OUTLINE_PREVIEW_INVALID | Preview invalid | "Statement outline preview input is invalid, empty input detected" |

### Access Control

- `show_outline` and `preview_outline`: No special privileges required
- `add_index_outline`, `add_optimizer_outline`, `del_outline`, `flush_outline`: Require SUPER_ACL or `root@%` user

### Validation

- **Index outline**: Position must be a valid table number; hint must contain valid index names
- **Optimizer outline**: Hint text is validated by parsing it via `parse_optimizer_hint()` during both cache loading and insertion
- **Digest computation**: If SQL text is provided, it is parsed and the computed digest is verified against the provided digest parameter

### Error Recovery

- If `add_outline()` fails writing to `mysql.outline`, the transaction is rolled back and the cache is not modified
- If `del_outline()` cache deletion finds fewer entries than partition count, a warning is issued but not an error
- If `reload_outlines()` encounters an invalid optimizer outline, it logs a warning, removes the record from the in-memory set, and continues

---

## 9. Memory Management

- All outline objects are allocated via `allocate_outline_object<T>()` which uses `my_malloc(key_memory_outline, ...)`
- Objects are freed via `destroy_object<T>()` which calls the destructor then `my_free()`
- `String_outline` uses `Outline_alloc` allocator backed by `key_memory_outline`
- The `System_outline` singleton is created at server startup and destroyed at shutdown
- Per-statement hint objects (e.g., `Index_hint`, `Lex_optimizer_hint`) are allocated on the statement's `MEM_ROOT` and freed when the statement completes
- Partition rwlocks protect concurrent access: read locks for hint lookup, write locks for cache modification

---

## 10. Test Coverage

12 MTR test cases in `mysql-test/suite/statement_outline/t/`:

| Test Case | Description |
|-----------|-------------|
| `outline_index_hint` | Index hint outlines: IGNORE, USE, FORCE with various scopes |
| `outline_optimizer_hint` | Optimizer hint outlines: various hint types |
| `outline_sql` | SQL-based outline addition with digest computation |
| `outline_match_empty_schema` | Outlines with empty schema match all databases |
| `outline_no_rpl` | Outline operations do not generate binlog events |
| `outline_for_explain_analyze` | EXPLAIN and EXPLAIN ANALYZE digest handling |
| `statement_outline_proc` | dbms_outln procedure interface testing |
| `statement_outline_variables` | System variable testing |
| `statement_outline_debug` | Debug-mode only features |
| `statement_outline_view` | Outline compatibility with views |
| `statement_outline_bugfix` | Regression tests for fixed bugs |
| `BUG2024022302941` | Specific bug regression test |

### DBUG Hooks

- `outline_simulate_oom`: Simulates OOM in `generate_statement_digest()` for testing error paths

---

## 11. Limitations

- Outline matching is based on digest only; two semantically different queries with the same normalized form will match the same outline
- Index hint position is based on table order in the query; changes to table order may break outline matching
- Optimizer hint position is based on query block numbering; complex subquery restructuring may affect matching
- Empty schema ("") matches all schemas; schema-specific outlines take priority but both are applied
- Digest truncation for very long queries may prevent accurate outline matching
- The `mysql.outline` table operations bypass binlog to avoid replication of outline management
- Outline application happens during parsing; it cannot affect decisions already made in the prepare phase

---

## 12. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **Common infrastructure** (`sql/common/component.h/.cc`) - Base classes, allocators, Pair_key_unordered_map
2. **Common table infrastructure** (`sql/common/table_common.h/.cc`, `sql/common/table.h/.cc`) - Conf_record, Conf_reader, Conf_writer base classes
3. **Outline type definitions** (`sql/outline/outline_common.h/.cc`, `sql/outline/outline_table_common.h/.cc`) - Enums, Outline_record, type conversions
4. **Outline class** (`sql/outline/outline.h`, `sql/outline/outline.cc`) - Outline, Statement_outline, Outline_group, System_outline
5. **Outline cache** (`sql/outline/outline_cache.cc`) - Same as outline.cc (init, destroy, invoke, refresh)
6. **Outline table I/O** (`sql/outline/outline_table.h/.cc`) - Outline_reader, Outline_writer, mysql.outline DDL
7. **Outline digest** (`sql/outline/outline_digest.h/.cc`) - Digest computation, hint parsing, preview
8. **Outline interface** (`sql/outline/outline_interface.h`) - Public API declaration
9. **Package framework integration** - Proc, Sql_cmd_proc, Package classes (if not already present)
10. **Outline procedures** (`sql/outline/outline_proc.h/.cc`) - dbms_outln stored procedures
11. **MySQL core modifications** - sql_parse.cc, mysqld.cc, sql_lex.h, table.h, sys_vars.cc, system_variables.h
12. **Error messages** - share/messages_to_clients.txt
13. **Test suite** - MTR test cases
