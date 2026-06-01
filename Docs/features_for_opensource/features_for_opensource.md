# Feature List for Open Source

> Generated: 2026-05-25
> Branch: taurusondstore_develop

## Feature Summary

| # | Feature Name | Main Commit | Review Document | Patches |
|---|-------------|-------------|-----------------|---------|
| 1 | Parallel Query | dd979fe3406 | [commits_for_review.md](01_parallel_query/commits_for_review.md) | 9 |
| 2 | Plan Cache | 1f21818e9a06 | [commits_for_review.md](02_plan_cache/commits_for_review.md) | 19 |
| 3 | Partial Result Cache | de8b6a55df87 | [commits_for_review.md](03_partial_result_cache/commits_for_review.md) | 12 |
| 4 | Left Join Elimination | bf20096d415b | [commits_for_review.md](04_left_join_elimination/commits_for_review.md) | 6 |
| 5 | IN-List to Subquery Conversion | 8eb87e1268aa | [commits_for_review.md](05_in_list_to_subquery/commits_for_review.md) | 5 |
| 6 | COUNT Optimization | 63c4346880e1 | [commits_for_review.md](06_count_optimization/commits_for_review.md) | 2 |
| 7 | Multi-Join Distinct Optimization | 7362a553f138 | [commits_for_review.md](07_multi_join_distinct/commits_for_review.md) | 1 |
| 8 | Redundant Condition Removal | b0ecd8c2a2aa | [commits_for_review.md](08_redundant_condition_removal/commits_for_review.md) | 3 |
| 9 | Offset Pushdown | 6bb266806f33 | [commits_for_review.md](09_offset_pushdown/commits_for_review.md) | 4 |
| 10 | Statement Outline | 2d32e008f275 | [commits_for_review.md](10_statement_outline/commits_for_review.md) | 1 |
| 11 | Range Scan Optimization | cca19c65133e | [commits_for_review.md](11_range_scan_optimization/commits_for_review.md) | 2 |
| 12 | Native Package Framework | 42618fa556a5 | [commits_for_review.md](12_native_package_framework/commits_for_review.md) | 1 |
| 13 | OMA Log | 27b20ff70a8f | [commits_for_review.md](13_oma_log/commits_for_review.md) | 6 |
| 14 | SQL Filter | b71519c3804e | [commits_for_review.md](14_sql_filter/commits_for_review.md) | 4 |
| 15 | Audit Log | e1a26db52688 | [commits_for_review.md](15_audit_log/commits_for_review.md) | 3 |
| 16 | Thread Pool | 21481fa93ed3 | [commits_for_review.md](16_thread_pool/commits_for_review.md) | 9 |
| 17 | Recycle Bin | 143ed22e4684 | [commits_for_review.md](17_recycle_bin/commits_for_review.md) | 5 |
| 18 | Extended Secondary Partition Types | 9fd5bbe38853 | [commits_for_review.md](18_extended_secondary_partition/commits_for_review.md) | 3 |
| 19 | PGO/LTO Compilation | 43b6a0f590c7 | [commits_for_review.md](19_pgo_lto_compilation/commits_for_review.md) | 2 |
| 20 | Enhanced Crash Diagnostics | 2ef131b5d1e | [commits_for_review.md](20_enhanced_crash_diagnostics/commits_for_review.md) | 3 |

## Status

| Status | Count |
|--------|-------|
| Completed | 20 |
| Pending | 0 |

## Directory Structure

```
features_for_opensource/
├── features_for_opensource.md           # This file
├── 01_parallel_query/
│   ├── commits_for_review.md
│   └── patches/
├── 02_plan_cache/
│   ├── commits_for_review.md
│   └── patches/
├── ...
└── 19_pgo_lto_compilation/
    ├── commits_for_review.md
    └── patches/
├── 20_enhanced_crash_diagnostics/
│   ├── commits_for_review.md
│   ├── EnhancedCrashDiagnostics-Feature-Spec.md
│   └── patches/
```

## Cross-Feature Shared Commits

| Commit | Features | Note |
|--------|----------|------|
| ea619894188 | PTRC #3, LJE #4 | Crash with partial_result_cache + left_join_elimination |
| 53191dcfb87 | Plan Cache #2, COUNT #6 | Count not null column optimization [post-fix] |
| ea82404780a | Redundant Condition #8, Range Scan #11 | Fix rds_empty_redundant_check_in_range_scan |

## Total Patch Count: 100

## Apply All Patches

```bash
for dir in features_for_opensource/*/patches; do
  git am "$dir"/*.patch
done
```

## Apply Single Feature

```bash
# Example: apply Parallel Query feature
git am features_for_opensource/01_parallel_query/patches/*.patch
```
