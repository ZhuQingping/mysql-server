# Partial Result Cache Test Report

## Scope

This report summarizes validation completed for the MySQL 8.0.46 Partial
Result Cache implementation.

The report focuses on tests and measurements that are complete and directly
relevant to PTRC behavior.

## Build Validation

| Gate | Evidence | Result |
|---|---|---|
| Release build target | `cmake --build <build-dir> --target mysqld -j 16` | Passed |
| Install target smoke | `DESTDIR=<install-staging-dir> ninja install -j 16` | Passed |

Canonical build command:

```bash
cd <source-tree>
cmake --build <build-dir> --target mysqld -j 16
```

## Focused PTRC MTR

Focused PTRC tests:

- `main.ptrc_hints`
- `main.ptrc_nested_loop`
- `main.ptrc_subquery`
- `main.ptrc_diagnostics`
- `main.ptrc_acceptance`
- `main.ptrc_followup`
- `main.ptrc_semantics`
- `main.ptrc_error_paths`
- `main.ptrc_fault_injection`

Canonical focused PTRC command:

```bash
cd <build-dir>/mysql-test
./mtr --force --timer --report-times \
  ptrc_hints ptrc_nested_loop ptrc_subquery \
  ptrc_diagnostics ptrc_acceptance ptrc_followup \
  ptrc_semantics ptrc_error_paths ptrc_fault_injection
```

Result: all focused PTRC tests pass in the relevant build modes.
`ptrc_fault_injection` requires debug binaries and passes in the
debug/sanitizer targeted run.

Coverage summary:

| Area | Test Coverage |
|---|---|
| Optimizer switch and system variables | `ptrc_hints`, sys-vars result coverage |
| PRC hints | `ptrc_hints`, `ptrc_nested_loop`, `ptrc_subquery` |
| Nested-loop join cache | `ptrc_nested_loop`, `ptrc_acceptance` |
| Correlated subquery cache | `ptrc_subquery`, `ptrc_diagnostics`, `ptrc_acceptance`, `ptrc_followup` |
| `EXISTS`, `IN`, `ANY`, `ALL` semantics | `ptrc_subquery`, `ptrc_acceptance`, `ptrc_semantics` |
| EXPLAIN and EXPLAIN ANALYZE counters | `ptrc_diagnostics`, `ptrc_acceptance` |
| Optimizer trace chosen/rejected/disabled state | `ptrc_diagnostics`, `ptrc_acceptance` |
| Runtime low-hit-ratio bypass | `ptrc_acceptance` |
| LRU eviction and overflow-safe behavior | `ptrc_acceptance` |
| BLOB/TEXT key rejection | `ptrc_acceptance` |
| Non-deterministic expression rejection | `ptrc_acceptance` |
| HAVING and unsupported shape fallback | `ptrc_acceptance` |
| TaurusDB follow-up crash and semantic regressions | `ptrc_followup`, `ptrc_semantics` |
| Error-path cleanup and propagation | `ptrc_error_paths` |
| Debug fault-injection coverage | `ptrc_fault_injection` |

## Full MTR Integration

Full MTR was used during integration validation to catch product-wide result
baseline changes caused by PTRC-visible optimizer output, system variables, and
optimizer trace entries.

Canonical full-MTR command shape:

```bash
cd <build-dir>/mysql-test
./mtr --force --parallel=8 --timer --report-times \
  --max-test-fail=0 \
  --vardir=<short-vardir>
```

PTRC-driven baseline updates were reconciled and rerun without `--record`.
The reconciled categories were:

- `optimizer_switch` output including `partial_result_cache=on`;
- `mysqld --verbose --help` output including four
  `rds_partial_result_cache_*` variables;
- EXPLAIN TREE output including `Result cache : cache keys(...)` for eligible
  dependent paths;
- optimizer trace output including `partial_result_cache` objects.

Focused PTRC tests passed in the full-MTR integration runs that reached them.

## Static Checks

Static diff check command:

```bash
git diff --check
```

Result: passed for the PTRC source, tests, and documentation changes.

Packaging checks confirmed that build outputs, `.DS_Store` files, generated
patches, benchmark logs, and task-board artifacts are not part of the
community-facing PTRC patch set.

## TaurusDB Runtime Parity

TaurusDB side-by-side runtime parity was run on a focused PTRC SQL suite. The
suite covered scalar correlated subqueries, `EXISTS`, `IN`, `ANY`, `ALL`,
nested-loop join PRC, TEXT key fallback, and low hit-ratio bypass.

The output SHA256 matched between the commercial TaurusDB reference binary and
this port:

```text
2fc7754abbe79397e431cf2347b7ebd17dcc7847e98fb7605cb096f043e8f767
```

Both binaries produced a `Result cache : cache keys(o.k)` EXPLAIN plan for the
focused scalar subquery.

## Sanitizer Validation

Targeted PTRC ASAN/UBSAN MTR was run for the focused PTRC suite. The focused
PTRC tests passed and no ASAN/UBSAN report appeared.

## TPC-H Q17 Performance Validation

Local TPC-H SF 1 and SF 10 Q17 were run on the standard primary-key and
foreign-key schema. The only optimizer setting changed between the two runs was
`optimizer_switch=partial_result_cache`.

These are engineering validation results, not official TPC-H benchmark results.

| Scale | Repeated-query batch | PTRC off mean | PTRC on mean | Correctness |
|---|---:|---:|---:|---|
| SF 1 | 200 queries | `14.166s` | `5.432s` | output SHA256 matched |
| SF 10 | 50 queries | `42.424s` | `15.564s` | output SHA256 matched |

The PTRC-on SF 1 plan showed `4936` cache hits and `168` misses. The PTRC-on
SF 10 plan showed `56630` cache hits and `1958` misses.

## Acceptance Summary

The completed validation covers:

- release build and install smoke;
- focused PTRC MTR behavior coverage;
- product-wide MTR result-baseline reconciliation for PTRC-visible output;
- static diff checks;
- focused TaurusDB runtime parity;
- targeted PTRC ASAN/UBSAN validation;
- local TPC-H SF 1 and SF 10 Q17 performance validation.
