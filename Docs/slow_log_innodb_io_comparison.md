# Slow Log Comparison: Community MySQL, Percona, MariaDB, and Our Implementation

## Overview

This document compares slow query log capabilities across:

- Community MySQL 8.0.41
- Percona Server
- MariaDB Server
- our current implementation on top of Community MySQL 8.0.41

The focus is specifically on whether slow log output can help diagnose
IO-related latency, especially in cold-data scenarios where buffer pool misses
trigger reads from underlying storage.

## Executive Summary

At a high level:

- Community MySQL provides standard slow log fields and `log_slow_extra`, but
  does not provide statement-level InnoDB storage-read attribution.
- Percona extends slow log with InnoDB-specific metrics and is strong for
  InnoDB diagnosis, but its implementation is more invasive and is tied to a
  broader extended slow-log framework.
- MariaDB exposes a broader engine-statistics view in slow log, including page
  access and page-read timing, and it also updates its own slow-log parsing
  tooling.
- Our implementation deliberately chooses a narrower scope than Percona and
  MariaDB: it focuses on storage reads caused by buffer pool misses, aims for
  low code churn, and keeps compatibility risk lower by preserving the standard
  slow log header lines.

## Comparison Table

| Dimension | Community MySQL 8.0.41 | Percona Server | MariaDB Server | Our Implementation |
|-----------|-------------------------|----------------|----------------|--------------------|
| Standard slow log support | Yes | Yes | Yes | Yes |
| Extra slow log fields | `log_slow_extra` | Broader slow log extensions | `log_slow_verbosity` extensions | New dedicated extension |
| InnoDB / engine IO attribution | No statement-level InnoDB IO counters in slow log | Yes, InnoDB-specific | Yes, engine-level | Yes, focused on InnoDB storage reads caused by buffer pool misses |
| Scope of engine metrics | Minimal | Medium, InnoDB-oriented | Broad, engine-oriented | Narrow, IO diagnosis-oriented |
| Output style | Standard lines, optional extra fields | Standard lines plus extension lines | Standard lines plus extension lines | Standard lines plus one extension line |
| Modifies standard `Query_time` line | No | No for InnoDB line itself | No for engine line itself | No |
| Default enabled | No | No | Depends on verbosity | No |
| Slow log table support for added fields | No | Primarily file-oriented extensions | Primarily file-oriented extensions | No |
| Official parser support for extended fields | Limited | Good in Percona tooling ecosystem | Good in MariaDB tooling ecosystem | Not yet extended in MySQL official tooling |
| Implementation invasiveness | Baseline only | Higher | Higher | Lower |

## Community MySQL 8.0.41

### What It Has

Community MySQL 8.0.41 provides:

- standard slow log output
- `log_slow_extra` for richer generic statement diagnostics
- good baseline compatibility with existing tools

Typical fields include:

- `Query_time`
- `Lock_time`
- `Rows_sent`
- `Rows_examined`
- and, when `log_slow_extra=ON`, additional generic counters such as handler
  reads, sort counters, temp-table counters, and timestamps

### What It Does Not Have

Community MySQL does not provide statement-level InnoDB storage-read
attribution in slow log.

Therefore, it does not directly answer:

- Was this query slow because the needed pages were not in the buffer pool?
- Did the query wait on underlying storage reads triggered by buffer pool
  misses?

### Strengths

- simplest behavior
- strong compatibility with existing log consumers
- lowest maintenance surface

### Weaknesses

- no direct IO root-cause signal for cold-data scenarios
- hard to distinguish IO-bound latency from CPU-bound or plan-bound latency

## Percona Server

### What Percona Adds

Percona extends slow log with InnoDB-focused fields such as:

- `InnoDB_IO_r_ops`
- `InnoDB_IO_r_bytes`
- `InnoDB_IO_r_wait`
- `InnoDB_rec_lock_wait`
- `InnoDB_queue_wait`
- `InnoDB_pages_distinct`

This is a strong fit for diagnosing InnoDB behavior from slow log alone.

### Design Characteristics

Percona's implementation:

- integrates deeply with THD and InnoDB statistics paths
- collects statement-level InnoDB-specific counters
- is part of a broader extended slow log framework

### Strengths

- excellent diagnostic value for InnoDB-heavy workloads
- direct visibility into IO and selected wait dimensions
- mature ecosystem alignment with Percona tooling

### Weaknesses

- more invasive than a narrow, targeted implementation
- larger implementation surface and greater review cost
- broader scope than needed if the only goal is "is this slow because of IO?"

## MariaDB Server

### What MariaDB Adds

MariaDB exposes engine-related slow log information through
`log_slow_verbosity`, including engine-oriented fields such as:

- `Pages_accessed`
- `Pages_read`
- `Pages_prefetched`
- `Pages_updated`
- `Old_rows_read`
- `Pages_read_time`
- `Engine_time`

MariaDB therefore exposes a broader engine profile rather than a strictly
InnoDB-physical-read-only view.

### Design Characteristics

MariaDB's implementation:

- uses statement-level `handler_stats`
- decides at statement scope whether engine stats should be collected
- outputs engine extension lines in slow log
- updates its own official parser tooling (`mariadb-dumpslow`) to understand
  the added fields

### Strengths

- broader engine observability than Community MySQL
- strong statement-level collection model
- official tooling alignment is better than "log only and hope parsers cope"

### Weaknesses

- broader metric scope increases implementation and semantic complexity
- not all fields directly target the specific "buffer pool miss caused storage
  read latency" question
- heavier than necessary for a first, low-risk MySQL 8.0.41 enhancement

## Our Implementation

### Design Goal

Our implementation intentionally chooses a narrower and more conservative scope.

It focuses on:

- InnoDB only
- statement-level metrics
- storage reads caused by buffer pool misses
- one extra slow log line
- default OFF
- no change to the standard `# Query_time ...` line

### Implemented Fields

The current implementation logs:

- `InnoDB_storage_read_ops`
- `InnoDB_storage_read_bytes`
- `InnoDB_storage_read_wait`

These fields are designed to answer the central operational question:

- Did the statement become slow because it had to wait for storage reads caused
  by buffer pool misses?

### Why This Differs From Percona

Compared with Percona, our implementation is deliberately smaller:

- no broader InnoDB wait taxonomy in version 1
- no lock wait or queue wait metrics
- no large THD statistics expansion
- no direct adoption of Percona's wider slow-log verbosity model

### Why This Differs From MariaDB

Compared with MariaDB, our implementation is more focused:

- we do not expose broad engine page activity
- we do not yet expose logical read or page-access counters
- we do not yet update official parser tooling to consume the new fields

## Advantages of Our Implementation

### 1. Better Focus For The Target Problem

The implementation is optimized for a very specific and common question:

- is this slow query IO-bound because of buffer pool misses?

That makes it easier to reason about than broader engine telemetry.

### 2. Lower Invasiveness

Compared with Percona and MariaDB, the code change is smaller in scope.

Benefits:

- easier review
- lower regression risk
- less pressure on unrelated slow-log behavior

### 3. Lower Compatibility Risk

We preserve the standard slow log header structure and only append one
standalone extension line.

This is better for compatibility than changing the standard `Query_time` line.

### 4. Lower Performance Risk

The collection path is intentionally narrow:

- default OFF
- synchronous storage-read related paths only
- lightweight statement-level accumulation

### 5. Better Fit For Upstream-Style Incremental Development

The implementation is a realistic first step for Community MySQL 8.0.41:

- minimal viable diagnostic value first
- broader metrics later only if proven useful

## Disadvantages of Our Implementation

### 1. Narrower Diagnostic Scope Than Percona

We currently do not expose:

- lock wait extensions
- queue wait extensions
- page distinctness metrics

Percona remains stronger for full InnoDB slow-log diagnosis.

### 2. Narrower Engine Visibility Than MariaDB

We currently do not expose:

- page access counts
- page prefetch counts
- page update counts
- engine time

MariaDB remains broader if the goal is engine profiling rather than targeted
IO attribution.

### 3. Tooling Ecosystem Is Not Yet Fully Updated

Unlike MariaDB, we have not updated MySQL official parsing tools such as
`mysqldumpslow` to consume the new fields.

This means our implementation currently aims to:

- minimize compatibility risk

rather than:

- provide full official-tool support for the new fields

### 4. Version 1 Uses A Focused Rather Than Generalized Collection Model

This is a strength for simplicity, but a limitation if future requirements
expand rapidly.

## What We Should Borrow Conceptually

### From Percona

- statement-level InnoDB metric aggregation
- direct slow log visibility into IO behavior

### From MariaDB

- statement-level "should collect engine stats" gating
- tighter official parser/tooling alignment when the feature matures

## What We Should Not Borrow Yet

### From Percona

- the full wider InnoDB slow-log extension framework
- all extra wait categories in version 1

### From MariaDB

- the broader engine-statistics scope in version 1
- a more invasive handler-stats framework just to solve the initial IO problem

## Final Assessment

If the evaluation criterion is:

- fastest way to gain practical slow-query IO diagnosis in Community MySQL with
  limited risk

then our implementation is a good fit.

If the evaluation criterion is:

- richest InnoDB slow-log diagnostics immediately

then Percona is functionally stronger.

If the evaluation criterion is:

- broader engine-statistics observability and tooling support

then MariaDB has stronger ideas in that direction.

Our implementation occupies a deliberate middle ground:

- much more useful than Community MySQL baseline for IO diagnosis
- much less invasive than adopting the broader Percona or MariaDB models
- well suited as a version 1 upstream-style enhancement

## Future Directions

The most promising future improvements are:

- add optional buffer pool logical read counters
- consider statement-level active gating similar to MariaDB
- consider extending official parsing tools if the feature is accepted as a
  long-term slow-log capability
- consider selective adoption of additional Percona-style wait fields only if
  operational value clearly outweighs extra complexity
