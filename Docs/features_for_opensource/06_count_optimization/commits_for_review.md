# COUNT Optimization Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: 63c4346880e18bf02268bdfd0097426e8e4cdb6e
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main COUNT Optimization feature commit
| Field | Value |
|-------|-------|
| **Hash** | 63c4346880e18bf02268bdfd0097426e8e4cdb6e |
| **Merge date** | 2025-07-01 |
| **Author** | Guilhem Bichot |
| **Subject** | convert COUNT(column) to COUNT(*) if column is not nullable |
| **Files changed** | 31 files, +790 / -36 |
| **Scope** | Full COUNT Optimization feature: convert COUNT(not_null_column) to COUNT(*) for better performance |
| **Patch** | `count_optimization-0001-USERSTORY-convert-COUNT-column-to-COUNT-if-col.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | 53191dcfb87a5957b5caea36af3c5a565e4bc9aa |
| **Merge date** | 2025-07-04 |
| **Author** | zhuqingping |
| **Subject** | Count not null column optimization [post-fix] |
| **Files changed** | 36 files, +522 / -193 |
| **Scope** | Post-fix: type fix in allocator, optimization for COUNT(not_null_col) with plan cache |
| **Patch** | `count_optimization-0002-USERSTORY-Count-not-null-column-optimization-p.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 1 follow-up)** | **2** |

### Notes
- #2 (53191dcfb87) is shared with the Plan Cache feature — it also fixes allocator issues in plan_cache.h

### Apply patches:
```bash
git am patches/count_optimization-*.patch
```
