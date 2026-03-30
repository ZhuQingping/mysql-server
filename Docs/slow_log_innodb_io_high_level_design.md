# Slow Log InnoDB IO High-Level Design

## Overview

This document describes a high-level design for enhancing the MySQL 8.0.41
slow query log so that it can expose statement-level InnoDB storage-read
information caused by buffer pool misses. The goal is to help diagnose whether
a slow query was likely caused by IO, especially in cold-data scenarios where
the first execution is slow and later executions are faster after pages are
loaded into the buffer pool.

The design is inspired by Percona Server's slow log extension, but aims to be
smaller, lower-risk, and less invasive for upstream-style MySQL code.

## Problem Statement

Today, MySQL slow log provides general latency signals such as:

- `Query_time`
- `Lock_time`
- `Rows_sent`
- `Rows_examined`

These fields are useful, but they do not directly answer an important question:

- Was this statement slow because it had to perform storage reads triggered by
  buffer pool misses?

This gap is especially visible in the following scenario:

1. A query reads cold data.
2. The first execution is slow because pages are not in the buffer pool.
3. The second execution is much faster because the working set is now cached.

Without IO-specific slow log data, it is difficult to confirm from the log
alone that the root cause was likely buffer pool miss and the resulting
underlying storage-read cost.

## Why This Feature Is Valuable

This feature is worth adding because it improves first-line diagnosis for slow
queries without requiring the user to correlate multiple subsystems manually.

Expected benefits:

- Allows DBAs and developers to quickly judge whether IO was a major factor in
  slow query latency.
- Makes "first run slow, second run fast" patterns directly visible in the
  slow log.
- Reduces dependence on manual correlation across Performance Schema, InnoDB
  status, and repeated query experiments.
- Provides stronger evidence when investigating cold data and buffer pool
  sizing issues.

This feature should be seen as a diagnostic aid, not as a full root cause
analysis system.

## Non-Goals

The first implementation should not try to solve all performance attribution
problems. In particular, it should not:

- Replace Performance Schema or other observability tools.
- Attempt to explain all causes of slow queries.
- Introduce a large generalized slow-log extension framework.
- Extend `mysql.slow_log` table format in the first version.
- Collect broad InnoDB internal diagnostics unrelated to the main IO use case.

## Existing Baseline In MySQL 8.0.41

In MySQL 8.0.41, the slow log file output path is centralized in:

- `sql/log.cc`

The main file-based slow log formatter is:

- `File_query_log::write_slow()`

This is a good insertion point for additional output because:

- The slow log output is already centralized there.
- The code already supports optional extra fields such as `log_slow_extra`.
- Statement-level state can already be prepared at statement start in SQL-layer
  code paths such as `sql/sql_parse.cc` and `sql/srv_session.cc`.

## Reference From Percona Server

Percona Server implements a broader slow log extension in which:

- InnoDB collects per-statement statistics.
- The thread (`THD`) stores these counters.
- `File_query_log::write_slow()` prints InnoDB-specific fields when data is
  available.

Useful ideas to borrow:

- Gather statement-scoped statistics during execution.
- Store the results on the session thread object.
- Print the information only when writing the slow query log.

Important aspects not to copy mechanically:

- Percona's implementation is part of a larger slow-log extension framework.
- It adds more fields and more moving parts than this use case strictly needs.
- It introduces more THD state and more engine integration than is necessary
  for a first upstream-friendly implementation.

## Design Principles

The implementation should follow these principles:

1. Keep default overhead near zero when the feature is disabled.
2. Solve the main diagnostic gap first: attribution of storage reads caused by
   buffer pool misses.
3. Keep code boundaries clear between SQL layer and InnoDB.
4. Avoid changes to system table definitions in the first version.
5. Prefer small, isolated additions over broad refactoring.
6. Make the output easy to read and easy to compare with Percona behavior.

## Proposed User-Facing Behavior

### New Configuration Option

Introduce a new global boolean system variable:

- `log_slow_innodb_io`

Recommended behavior:

- Default: `OFF`
- Scope: `GLOBAL`
- Effective only when slow log output includes `FILE`
- If `log_output=TABLE` only, accept the setting but warn that it has no effect
  on table logging

This keeps the feature independent from `log_slow_extra` and avoids forcing
users to enable a broader set of extra fields just to get IO visibility.

### Slow Log Output Format

When enabled and data is available, print one additional line in the slow log:

```text
# InnoDB_IO_r_ops: 128  InnoDB_IO_r_bytes: 2097152  InnoDB_IO_r_wait: 0.842731
```

To maximize compatibility with common community tools that parse MySQL slow
logs, the implementation should follow a strict compatibility-oriented rule:

- Do not modify the existing standard slow log header lines.
- In particular, do not append new fields to the standard
  `# Query_time: ... Lock_time: ... Rows_sent: ... Rows_examined: ...` line.
- Emit the InnoDB IO information only as an additional standalone comment line.
- Keep the feature disabled by default so existing deployments see no format
  change unless they explicitly opt in.

Recommended layout:

```text
# Query_time: 1.238451  Lock_time: 0.000071 Rows_sent: 10 Rows_examined: 100000
# InnoDB_IO_r_ops: 128  InnoDB_IO_r_bytes: 2097152  InnoDB_IO_r_wait: 0.842731
SELECT ...
```

Not recommended:

```text
# Query_time: 1.238451  Lock_time: 0.000071 Rows_sent: 10 Rows_examined: 100000 InnoDB_IO_r_ops: 128 ...
```

Field meanings:

- `InnoDB_IO_r_ops`
  - Number of storage read operations attributable to the statement, where the
    read was triggered because the required page was not already present in the
    buffer pool.
- `InnoDB_IO_r_bytes`
  - Total bytes read from underlying storage for those buffer-pool-miss reads.
- `InnoDB_IO_r_wait`
  - Total wait time spent on those storage reads, in seconds with microsecond
    precision.

These three fields are sufficient for the primary use case:

- Detect whether the statement likely paid real storage IO cost.
- Distinguish cold reads from warm-cache reads.

### Compatibility Principle With Community Tools

Many open-source tools parse MySQL slow logs with fixed regular expressions or
state machines. Some parsers tolerate unknown comment lines, while others are
more strict and may fail when standard lines are changed.

Therefore, compatibility should be treated as a first-class design principle:

- Preserve the standard slow log entry structure as-is.
- Add only one extra extension line, prefixed with `#`.
- Never change the meaning, order, or field list of the standard
  `Query_time` header line.
- Treat this feature as an opt-in slow log extension, not as a new default log
  format.
- Document clearly that some third-party parsers may ignore the new line, and
  a small subset of strict parsers may still require updates.

This approach does not guarantee zero impact for every parser in the ecosystem.
Instead, it is the lowest-risk option identified so far, and it materially
reduces compatibility risk compared with modifying the existing standard
fields.

### Representative Tool Compatibility Observations

The ecosystem contains multiple parser styles, and they behave differently when
the slow log format is extended.

- `mysqldumpslow`
  - Uses fixed regular expressions for standard header lines.
  - High risk if the standard `# Query_time` line is modified.
  - Even with a standalone extension line, behavior should be treated as
    cautionary rather than guaranteed-safe.
- `pt-query-digest`
  - Uses a more generic slow log parser and key/value extraction model.
  - Likely to tolerate a standalone `# InnoDB_IO_...` line better than
    fixed-format tools.
  - Strong evidence exists that its event model already understands Percona
    extended fields such as `InnoDB_IO_r_ops`.
- `percona/go-mysql` and PMM agent parsers
  - Use generic header detection and metric extraction.
  - Likely to accept a standalone extension line and place values into metric
    maps.
- Strict third-party parsers
  - Some explicitly do not support `log_slow_extra` or other non-standard
    formats.
  - These tools demonstrate that not all parsers are extension-tolerant.

Therefore, the design target should be phrased as:

- maximize compatibility with common tooling

and not as:

- guarantee zero compatibility impact across all tools

## Recommended Scope For Version 1

Version 1 should focus only on:

- Storage read count caused by buffer pool misses
- Storage read bytes caused by buffer pool misses
- Storage read wait time caused by buffer pool misses

Version 1 should not include:

- Lock wait statistics
- InnoDB queue wait statistics
- Distinct page counters
- Extended page classification
- Broad slow-log verbosity framework changes

This keeps the implementation narrow and aligned with the immediate diagnosis
goal.

## Architecture Overview

The recommended architecture is:

1. SQL layer owns statement-level slow-log IO state.
2. InnoDB reports relevant IO events through a small bridge API.
3. Slow log formatter prints the final aggregated values at log-write time.

### SQL Layer Responsibilities

The SQL layer should:

- Define the user-facing system variable.
- Own the statement-scoped aggregation state.
- Reset the aggregation state at top-level statement start.
- Print the fields in `File_query_log::write_slow()` when appropriate.

Likely files:

- `sql/sql_class.h`
- `sql/sql_class.cc`
- `sql/sql_parse.cc`
- `sql/srv_session.cc`
- `sql/sys_vars.cc`
- `sql/log.cc`

### InnoDB Responsibilities

InnoDB should:

- Detect the relevant storage read events caused by buffer pool misses.
- Report only the minimal data needed for aggregation.
- Avoid learning slow-log formatting details.

Likely files:

- `storage/innobase/handler/ha_innodb.cc`
- `storage/innobase/buf/buf0buf.cc`
- `storage/innobase/row/row0pread.cc`
- `storage/innobase/include/ha_prototypes.h`

## Proposed Internal State Model

Instead of adding many individual counters directly as unrelated `THD` fields,
the preferred approach is to introduce a small dedicated statement-level
structure owned by `THD`.

Conceptually, the structure should hold:

- whether the feature is enabled for the current statement
- whether InnoDB was involved
- storage read operation count caused by buffer pool misses
- storage read byte count caused by buffer pool misses
- storage read wait time in microseconds

Benefits of a dedicated structure:

- Better isolation from unrelated THD members
- Easier lifecycle management
- Easier future extension if more InnoDB slow-log fields are added later
- Smaller risk of turning THD into an unstructured collection of counters

## Statement Lifecycle

The statistics must be statement-scoped, not transaction-scoped.

Recommended lifecycle:

1. At top-level statement start, reset the aggregation state.
2. During statement execution, InnoDB reports storage-read activity caused by
   buffer pool misses.
3. At slow-log write time, the SQL layer prints the aggregated values.
4. The next top-level statement starts from a clean state.

Recommended reset points should mirror existing slow-log extra handling:

- `sql/sql_parse.cc`
- `sql/srv_session.cc`

This reduces code churn because the infrastructure for statement-start setup is
already present there.

## Sub-Statement Handling

For version 1, use a simple rule:

- Attribute all relevant InnoDB IO performed during execution of a top-level
  statement to that top-level statement, including work done inside stored
  procedures, functions, or triggers executed on its behalf.

This is acceptable because:

- Slow log entries are written for top-level statements.
- The diagnostic question is about the observed latency of the top-level SQL.
- It avoids immediately introducing more complex backup/restore logic for
  sub-statement state.

If later testing exposes correctness issues in nested execution paths, the
design can be extended with explicit save/restore support.

## How InnoDB Should Report Data

The preferred design is a minimal bridge from InnoDB to the SQL layer.

Recommended characteristics:

- Small and purpose-built
- No slow-log formatting knowledge in InnoDB
- Fast enablement check
- Integer accumulation only on hot paths

Conceptually, the bridge should support reporting:

- one storage read operation caused by a buffer pool miss
- N bytes read from storage for those buffer-pool-miss reads
- M microseconds of wait time for those storage reads

This can be implemented either:

- as one typed reporting function, or
- as a very small set of specialized helper functions

The exact API shape is less important than keeping the dependency direction
clean:

- InnoDB reports events
- SQL layer owns aggregation

## Where To Instrument In InnoDB

For the first implementation, focus on front-end paths that most directly
represent statement-attributable storage-read activity caused by buffer pool
misses.

Recommended areas to inspect and instrument:

- `buf0buf.cc`
  - Physical page read initiation / completion counters
- `row0pread.cc`
  - Wait time attributable to synchronous or user-visible pread activity
- `ha_innodb.cc`
  - Fast enablement gate and helper bridge integration

Instrumentation should avoid:

- Background IO threads
- Read-ahead that cannot be safely attributed to the foreground statement
- Broader internal counters that would blur the meaning of the logged values

The most important requirement is semantic correctness of attribution, not
collecting every possible IO-related event.

## Logging Rules

Recommended output rules:

- If the statement did not use InnoDB, print nothing.
- If the statement used InnoDB but performed no attributable storage reads
  caused by buffer pool misses,
  print the IO line with zeros.
- If the statement used InnoDB and performed such storage reads, print the
  actual values.

This is preferable to emitting a verbose "no statistics available" message
because:

- it keeps the log cleaner
- it simplifies machine parsing
- a zero-value line is already informative

## Why Not Extend `mysql.slow_log` Table In Version 1

Supporting table output would require changing the system log table definition,
including:

- table metadata definition
- compatibility behavior
- tests for `mysql.slow_log`
- upgrade and operational considerations

That change is materially larger than the file-log enhancement and does not
directly improve the main target use case, which is operational diagnosis from
the slow log file.

Therefore, version 1 should support:

- `FILE` output only

And should not support:

- additional `mysql.slow_log` table columns

## Performance Considerations

This feature must be designed to minimize overhead.

### When Disabled

When `log_slow_innodb_io=OFF`, the extra cost should be limited to:

- one cheap enablement check
- no significant additional state updates

### When Enabled

When enabled, overhead should remain small because:

- only a few integer counters are updated
- the work is concentrated around physical read paths
- additional cost appears primarily on already slow IO paths

The implementation should avoid:

- heavy locking
- string formatting during execution
- expensive cross-layer lookups on each access
- broad instrumentation on all handler read paths

## Correctness Risks

The main risks are not about syntax or formatting; they are about attribution
accuracy.

Key risks:

- Counting background IO that should not be attributed to the statement
- Counting read-ahead in a misleading way
- Missing some relevant synchronous reads and under-reporting IO cost
- Incorrect lifecycle reset leading to counter leakage across statements
- Nested execution paths causing state contamination

For this reason, it is better to start with a narrow but trustworthy definition
of "statement-attributable physical reads" than with a broader but ambiguous
one.

## Comparison With Percona-Inspired Full Extension

This proposal intentionally does less than Percona's broader slow-log extension.

Advantages of the reduced-scope design:

- smaller code surface
- lower review risk
- lower chance of behavioral regression
- better fit for an upstream-style incremental change
- easier validation and rollout

What is intentionally deferred:

- lock wait fields
- queue wait fields
- page distinctness fields
- generalized slow-log verbosity expansion

## Recommended Implementation Phases

### Phase 1: Minimal Viable Diagnostic Feature

Deliver:

- `log_slow_innodb_io`
- statement-level THD aggregation state
- InnoDB bridge hooks for storage-read ops/bytes/wait caused by buffer pool
  misses
- file slow log output line
- targeted MTR coverage

Success criterion:

- The slow log clearly distinguishes cold-read and warm-read execution
  behavior for InnoDB queries by showing storage reads caused by buffer pool
  misses.

### Phase 2: Optional Extensions

Consider only after Phase 1 proves useful and safe:

- lock wait information
- queue wait information
- richer InnoDB diagnostics
- possible unification with broader slow-log verbosity design
- optional logical read counters from buffer pool access paths, to complement
  the storage-read metrics and help distinguish IO-bound queries from
  cache-hit-heavy queries

## Validation Strategy

At minimum, testing should cover:

1. Feature disabled
   - No new slow-log fields are printed.
2. Feature enabled, cold read
   - `InnoDB_IO_r_ops > 0`
   - `InnoDB_IO_r_wait > 0`
3. Same query executed again on warm data
   - IO counters drop to zero or significantly lower values
4. Non-InnoDB query
   - No InnoDB IO line is printed
5. `log_output=TABLE`
   - Setting is accepted with warning
   - No table-format extension is attempted

Suggested validation command after implementation:

```bash
cd build/mysql-test
./mtr main.slow_log_innodb_io
```

## Final Recommendation

The feature is worth implementing.

The best implementation strategy for MySQL 8.0.41 is not to copy Percona's
full slow-log extension, but to adopt its core idea in a narrower form:

- collect statement-level InnoDB storage-read statistics caused by buffer pool
  misses
- keep the aggregation state in SQL-layer thread context
- print a single compact IO line in the slow log file
- avoid table-format changes
- keep the first version focused on storage read ops, storage read bytes, and
  storage read wait

This approach provides high diagnostic value while keeping performance risk,
code churn, and compatibility impact under control.
