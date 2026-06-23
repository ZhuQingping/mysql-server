# Partial Result Cache Community Submission Notes

## Recommended Commit Scope

Include:

- PRC product source changes under `sql/`.
- New focused PRC MTR tests and results:
  - `mysql-test/t/ptrc_hints.test`
  - `mysql-test/t/ptrc_nested_loop.test`
  - `mysql-test/t/ptrc_subquery.test`
  - `mysql-test/t/ptrc_diagnostics.test`
  - `mysql-test/t/ptrc_acceptance.test`
  - `mysql-test/t/ptrc_followup.test`
  - `mysql-test/t/ptrc_semantics.test`
  - `mysql-test/t/ptrc_error_paths.test`
  - `mysql-test/t/ptrc_fault_injection.test`
  - matching `mysql-test/r/ptrc_*.result`
- Intentional existing MTR result updates caused by:
  - `optimizer_switch=partial_result_cache`;
  - new `rds_partial_result_cache_*` variables;
  - `Result cache : cache keys(...)` EXPLAIN output;
  - `partial_result_cache` optimizer trace output.
- Community-facing docs under `Docs/partial_result_cache/`.

Exclude:

- build directories and generated binaries
- `.DS_Store`
- `Docs/.DS_Store`
- benchmark data and logs
- generated patch files or scratch logs

## Commit Shape

The current branch is a packaging candidate, not a finalized staged community
commit. The final community submission should contain PRC product code, PRC
tests, result baselines, and public docs, plus the PTRC sanitizer cleanup fix
and follow-up regression tests currently present after `83c4ec666df`.

Use one community-facing feature commit. Squash the later sanitizer cleanup,
regression-test commits, and the `all_persisted_variables` baseline fix into
the feature commit before final submission. Do not include build directories,
`.DS_Store`, internal task-board logs, or unrelated environment
stabilizations.

## Proposed Commit Message

```text
WL#PTRC SQL: Add partial result cache

Problem/Background:
Repeated outer or dependent parameter values can make nested-loop
joins and correlated subqueries execute the same inner plan many times.
This is wasted work when the inner result for a parameter value is
deterministic and can be reused inside the statement.

Partial Result Cache (PRC/PTRC) addresses that pattern with a
statement-local cache above eligible inner work. In local TPC-H Q17
repeated-query benchmarks, changing only
optimizer_switch=partial_result_cache improved the SF1 200-query batch
mean from 14.166s to 5.432s and the SF10 50-query batch mean from
42.424s to 15.564s. Off/on outputs matched in both runs. This is local
engineering evidence for a high-repeat workload, not an official TPC-H
result.

Solution:
1. Add AccessPath::PARTIAL_RESULT_CACHE and ptrc::PtrcIterator for
   eligible nested-loop join inner paths and deterministic dependent
   subqueries.
2. Add optimizer_switch=partial_result_cache, rds_partial_result_cache_*
   session variables, and PRC_SUBQUERY/NO_PRC_SUBQUERY/PRC_JOIN/
   NO_PRC_JOIN hints.
3. Store cache keys and rows in bounded statement-local memory using
   row-packing and hash-join row-buffer helpers.
4. Evict complete key batches with LRU policy, count overflows, and
   enter pass-through bypass mode when caching is not useful or cannot
   initialize.
5. Preserve scalar, EXISTS, IN, ANY, and ALL correlated-subquery
   semantics by caching result item state, no-match state, and was_null
   state.
6. Add EXPLAIN TREE, EXPLAIN ANALYZE counters, and optimizer trace
   diagnostics.

Compatibility/Risk:
Unsupported shapes keep the original child path or enter bypass mode.
This includes non-deterministic expressions, unsupported dependent
subquery shapes, BLOB/TEXT cache keys, const-only paths, and low
estimated or runtime hit-ratio cases.

The cache is statement-local. It is not shared across statements,
sessions, or transactions, and it does not change table data visibility.

Test:
1. Built mysqld with:
   cmake --build <build-dir> --target mysqld -j 16
2. Ran focused PTRC MTR tests:
   ptrc_hints ptrc_nested_loop ptrc_subquery ptrc_diagnostics
   ptrc_acceptance ptrc_followup ptrc_semantics ptrc_error_paths
   ptrc_fault_injection
3. Updated and reran all_persisted_variables after PTRC added four
   persisted dynamic system variables.
4. Ran full-MTR integration validation and reconciled PTRC-visible
   optimizer_switch, system variable, EXPLAIN TREE, and optimizer trace
   result baselines.
5. Ran:
   git diff --check
6. Ran TaurusDB side-by-side runtime parity on a focused PTRC SQL suite.
   Output SHA256 matched the commercial reference binary.
7. Ran local TPC-H SF1 and SF10 Q17 on the standard PK/FK schema with
   only partial_result_cache switched off and on. SF1 used 200-query
   batches and improved from 14.166s to 5.432s. SF10 used 50-query
   batches with an 18G buffer pool and improved from 42.424s to
   15.564s. Off/on output SHA256 matched for both runs. This is
   engineering validation, not an official TPC-H result.
8. Ran targeted PTRC ASAN/UBSAN MTR. All targeted PTRC rows passed and
   no sanitizer report appeared.
```

## Final Review Checklist

- Confirm no build outputs or `.DS_Store` files are staged.
- Confirm public docs are staged, but internal agent task-board docs are not
  staged unless the release owner explicitly wants them.
- Confirm `AGENTS.md`, `CLAUDE.md`, and internal `Docs/ptrc_tasks/` files are
  handled according to the target audience: useful for local AI continuity,
  but not normally part of an upstream community patch.
- Confirm `git diff --cached --check` status and document the exact-output
  whitespace exception if it remains staged.
- Confirm focused PTRC tests and full-MTR integration validation are cited.
- Confirm performance report is limited to local SF1/SF10 Q17 engineering
  validation and does not overclaim official TPC-H or broad TPC-H impact.
- Confirm the final commit message includes completed validation.
- Confirm internal environment triage records are kept outside the
  community-facing document set.
