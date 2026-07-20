# Stable Branch Verification Matrix (2026-07-21)

## Scope

This record is the pre-refactor reference for `stable_branch` on macOS with
`lower_case_table_names=2`, no DStore engine, and no thread-pool plugin.  The
MTR commands were run from the matching Ninja build directory with
`--parallel=8` unless a focused rerun states otherwise.

The DStore capability policy is provided by commit `10e377d4bd3`:
`not_have_only_dstore.inc` permits non-DStore tests to execute when DStore is
absent; `have_dstore.inc` continues to skip DStore-only tests.  Consequently,
DStore-only skips below are expected capability skips, not passes.

## Full discovery runs

| Build | Command | Executable result | Skipped | Self-skipped | Notes |
| --- | --- | --- | ---: | ---: | --- |
| Release | `./mtr --force --max-test-fail=0 --retry=0 --parallel=8 --suite=main` | 1,125/1,126 passed | 359 | 111 | `main.func_in_icp_mrr` differed only in an EXPLAIN row estimate; four clean reruns passed. |
| Release | `./mtr --force --max-test-fail=0 --retry=0 --parallel=8 --pq --suite=main,parallel_query` | 1,162/1,162 passed | 416 | 116 | PQ tests requiring Debug correctly self-skip in Release. |
| Debug | `./mtr --force --max-test-fail=0 --retry=0 --parallel=8 --suite=main` | 1,287/1,288 passed | 197 | 116 | `main.sp-threads` sampled an event-scheduler status string; three clean reruns passed. |
| Debug | `./mtr --force --max-test-fail=0 --retry=0 --parallel=8 --pq --suite=main,parallel_query` | 1,375/1,376 passed | 202 | 120 | `parallel_query.pq_agg_distinct` differed only in an EXPLAIN row estimate; three clean reruns passed. |
| ASAN | `./mtr --sanitize --force --max-test-fail=0 --retry=0 --parallel=8 --suite=main` | 1,235/1,249 passed in discovery | 236 | 156 | Fourteen failures were subsequently rerun after their targeted build/fix actions; see below. |
| ASAN | `./mtr --sanitize --force --max-test-fail=0 --retry=0 --parallel=8 --pq --suite=main,parallel_query` | 1,358/1,361 passed in discovery | 217 | 136 | Three failures were subsequently classified and rerun; `sanitize_report` passed. |

`--pq` also has intentional main-suite gates for scenarios that explicitly
cannot run under Parallel Query.  These are counted as self-skips and must not
be compared to non-PQ execution as regressions.

## Focused closure evidence

### Expected-result/statistics sampling

- `main.func_in_icp_mrr` (Release): full-run estimate `7/28.57%` versus
  `6/33.33%`; identical access path and warning text.  `--repeat=3` plus the
  initial focused rerun passed (four clean executions total).
- `main.sp-threads` (Debug): scheduler state text `Waiting on empty queue`
  versus `Waiting for next activation`; query/thread behavior was unchanged.
  `--repeat=3` passed, and the Debug PQ full run also passed.
- `parallel_query.pq_agg_distinct` (Debug PQ): table cardinality estimate
  `5000` versus `5001`; all plan operators and query output were identical.
  `./mtr --force --retry=0 --parallel=1 --pq --repeat=3
  parallel_query.pq_agg_distinct` passed.
- `main.group_by` (ASAN PQ): Gather row estimate `4` versus `5`; the Gather
  structure and worker count were unchanged.  A clean focused rerun passed.

No result file was changed for any of the estimate/state sampling cases above.

### ASAN findings

- `main.mysqlpump_extended` exposed a real heap-use-after-free after an error
  path left object-queue callbacks running while the crawler's dump tasks were
  destroyed.  `client/dump/program.cc` now destroys the chain maker (which
  joins queues) before the crawler.  Focused ASAN rerun passed, including
  `sanitize_report`.
- `main.rds_admin_port_using_per_thread` was a full-run MTR startup timeout;
  a clean rerun self-skipped through `have_threadpool.inc` because this build
  has no thread-pool plugin.  It is not a server crash or PQ failure.
- The remaining ASAN discovery failures were caused by test executables,
  components, or plugins that had not yet been built when MTR collected
  capability checks.  After building the required targets, the 15-test
  closure batch and a 14-test plugin closure batch both passed with
  `sanitize_report` clean (the example-plugin capability test remained an
  intentional self-skip when its plugin-dir option is absent).

### Build verification

- Release and Debug: `cmake --build . --target mysqld mysqlpump -j8` passed.
- ASAN: the corresponding targets passed with the macOS
  `-fsanitize-address-use-odr-indicator` configuration.  A full product build
  remains outside this matrix because unrelated Router and Group Replication
  sources fail with the current Apple Clang diagnostics; this does not affect
  the server/MTR targets exercised here.

## Baseline interpretation

The stable branch has no unreconciled PQ functional failure in the verified
matrix.  The discovery counts are retained rather than overwritten so a later
refactor comparison can distinguish: a genuine new failure, an intentional
capability skip, a missing test artifact, and a known EXPLAIN/state sampling
variance.  Refactor validation must use the same commands and must treat any
new Gather/plan-structure delta as a defect until root-caused.
