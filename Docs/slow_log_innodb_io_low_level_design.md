# Slow Log InnoDB IO Low-Level Design

## Overview

This document refines the high-level design into concrete implementation
decisions for MySQL 8.0.41. The implementation goal is to add statement-level
slow log fields for InnoDB storage reads caused by buffer pool misses while
keeping the change set small, isolated, and off by default.

## Scope

Version 1 implements:

- a new global system variable: `log_slow_innodb_io`
- statement-scoped aggregation state on `THD`
- InnoDB-side reporting for synchronous storage reads and waits
- file slow log output of one additional extension line
- MTR coverage for:
  - feature disabled
  - feature enabled with positive stats
  - feature enabled with zero-valued stats
  - sysvar behavior and warnings

Version 1 does not implement:

- `mysql.slow_log` table extensions
- buffer pool logical read counters
- lock wait or queue wait metrics
- generalized slow-log verbosity integration

## Output Format

The implementation keeps the existing standard slow log lines unchanged and
adds a single standalone extension line:

```text
# Query_time: ...
# InnoDB_storage_read_ops: 1  InnoDB_storage_read_bytes: 16384  InnoDB_storage_read_wait: 0.001000
SELECT ...
```

Compatibility rule:

- never modify the standard `# Query_time ...` line
- only append a separate `# ...` line
- keep the feature disabled by default

## User-Facing Variable

### Variable

- Name: `log_slow_innodb_io`
- Scope: `GLOBAL`
- Type: `BOOL`
- Default: `OFF`

### Semantics

- Only affects `FILE` slow log output.
- When `log_output` does not include `FILE`, setting the variable is accepted
  but generates the same warning pattern as `log_slow_extra`.

### Files

- `sql/mysqld.h`
- `sql/mysqld.cc`
- `sql/sys_vars.cc`

## Statement-Scoped State

### Data Structure

Add a dedicated structure to `THD`:

- `innodb_used`
- `storage_read_ops`
- `storage_read_bytes`
- `storage_read_wait_us`

### Rationale

- Avoids scattering unrelated counters across `THD`.
- Keeps lifecycle management local and explicit.
- Supports zero-valued output for InnoDB statements that do not trigger storage
  reads.

### Methods

Add lightweight helpers on `THD`:

- `reset_slow_log_innodb_io_stats()`
- `note_slow_log_innodb_used()`
- `add_slow_log_storage_read_stats(bytes, wait_us)`

### Files

- `sql/sql_class.h`

## Statement Lifecycle

### Reset Point

Reset the statement-level aggregation state in:

- `THD::reset_for_next_command()` in `sql/sql_parse.cc`

This is preferred over wiring multiple ad hoc resets through slow-log setup
paths because:

- it already defines the top-level statement lifecycle
- it applies broadly to normal statement execution
- it reduces additional call-site churn

### Ownership

- State belongs to the top-level statement.
- Nested execution contributes to the same top-level counters.
- Version 1 does not add substatement save/restore handling.

## SQL Slow Log Output

### Output Rule

In `File_query_log::write_slow()`:

- if `log_slow_innodb_io=OFF`, print nothing
- if `log_slow_innodb_io=ON` and `innodb_used=false`, print nothing
- if `log_slow_innodb_io=ON` and `innodb_used=true`, print one extension line

This allows:

- no output for non-InnoDB statements
- zero-valued output for InnoDB statements that remained in cache
- positive values when storage reads were observed

### File

- `sql/log.cc`

## InnoDB Integration

The implementation uses a very small bridge exported from InnoDB handler code.

### Helper Functions

Add helper declarations in:

- `storage/innobase/include/ha_prototypes.h`

Add helper definitions in:

- `storage/innobase/handler/ha_innodb.cc`

Helpers:

- `innobase_collect_slow_log_io()`
- `innobase_register_slow_log_storage_read(bytes, wait_us)`

Responsibilities:

- decide whether collection is active for current foreground context
- route reported values into `current_thd`

## Marking InnoDB Usage

To distinguish:

- non-InnoDB statements
- InnoDB statements with zero storage reads

the implementation marks InnoDB usage in:

- `ha_innobase::update_thd(THD *thd)`

This is a low-intrusion place because:

- the handler already binds statement context here
- it is naturally on the execution path for InnoDB table access
- it avoids broad changes in planner or executor code

## Storage Read Accounting

Version 1 accounts only storage reads attributable to buffer pool misses.

### 1. Synchronous Storage Read Initiation

File:

- `storage/innobase/buf/buf0rea.cc`

Hook:

- `buf_read_page_low(...)`

Behavior:

- when `sync=true`, start a timer before the synchronous read
- after the read completes, report:
  - one storage read op
  - page-size bytes
  - measured wait time

This captures the primary case where the current statement itself initiates the
miss-driven storage read.

### 2. Wait For Read Already In Progress

File:

- `storage/innobase/buf/buf0buf.cc`

Hooks:

- compressed-page wait loop in `buf_page_get_zip(...)`
- `buf_wait_for_read(buf_block_t *block)`

Behavior:

- if the statement waits on a page already being read, report wait time only
- do not increment ops/bytes in that path

This avoids double counting the read operation while still capturing latency
seen by the current statement.

## Performance Constraints

### Disabled Path

When `log_slow_innodb_io=OFF`:

- only cheap gating checks should remain
- no counter updates should occur
- no extra slow-log output is produced

### Enabled Path

When enabled:

- work stays on synchronous miss-driven read paths
- updates are integer additions on `THD`
- formatting happens only during slow log output

The implementation deliberately avoids:

- global shared state
- broad instrumentation of all handler reads
- expensive synchronization
- large transaction- or engine-wide statistics frameworks

## Debug Hooks For Stable Tests

To keep MTR deterministic without relying on actual buffer pool state, version 1
adds debug-only support through `DBUG_EXECUTE_IF` in InnoDB helper paths:

- `innodb_slow_log_force_storage_read`
  - force one positive storage-read sample when InnoDB is used
- `innodb_slow_log_skip_storage_reads`
  - suppress storage-read reporting while still marking InnoDB as used

These hooks are test-only tools and do not affect normal production behavior.

## Test Plan

### New Tests

1. `main.slow_log_innodb_io`
   - verifies no line is printed when feature is OFF
   - verifies positive stats line when feature is ON and forced stats are used
   - verifies zero-valued line when feature is ON and reporting is suppressed

2. `suite/sys_vars.log_slow_innodb_io_basic`
   - validates type checking
   - validates scope rules
   - validates warning behavior when `log_output` is not `FILE`

### Regression Coverage

Run at least:

- `main.slow_log_innodb_io`
- `suite/sys_vars.log_slow_innodb_io_basic`
- `main.slow_log_extra`
- `main.mysqldumpslow`
- `main.1st`

If time and environment allow, broaden validation further across `main`.

## Open Future Work

- add buffer pool logical read counters as a complementary metric
- add optional lock wait and queue wait fields
- consider exposing metrics through `log_slow_extra` or a broader verbosity
  framework only after version 1 proves stable
