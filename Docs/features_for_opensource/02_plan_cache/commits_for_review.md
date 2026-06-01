# Query Plan Cache Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: 1f21818e9a061b1a77adca3352ac264c94fb3099
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main plan cache feature commit
| Field | Value |
|-------|-------|
| **Hash** | 1f21818e9a061b1a77adca3352ac264c94fb3099 |
| **Merge date** | 2025-06-03 |
| **Author** | Guilhem Bichot |
| **Subject** | Port of Taurus feature "session plan cache" |
| **Files changed** | 49 files, +12,815 / -123 |
| **Scope** | Full plan cache feature: caching of execution plans for prepared statements |
| **Patch** | `plan_cache-0001-port-of-taurus-feature-session-plan-cache.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | 53191dcfb87a5957b5caea36af3c5a565e4bc9aa |
| **Merge date** | 2025-07-04 |
| **Author** | zhuqingping |
| **Subject** | Count not null column optimization [post-fix] |
| **Scope** | Type fix in plan_cache.h allocator, optimization for COUNT(not_null_col) |
| **Patch** | `plan_cache-0002-count-not-null-column-optimization-post-fix.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | 2728779036c95c1b6c4eba98f18f544e5f8d0edf |
| **Merge date** | 2025-08-20 |
| **Author** | Guilhem Bichot |
| **Subject** | After enable rds_plan_cache, sysbench range query performance fluctuates |
| **Scope** | Performance fix: plan cache causing unstable performance |
| **Patch** | `plan_cache-0003-after-enable-rds-plan-cache-using-the.patch` |

### #4
| Field | Value |
|-------|-------|
| **Hash** | 187844389881597ab4ca46ea56ecdefcdc5e12a9 |
| **Merge date** | 2025-08-20 |
| **Author** | Guilhem Bichot |
| **Subject** | Window function + ORDER BY with plan_cache returns incorrectly sorted results |
| **Scope** | Wrong result fix |
| **Patch** | `plan_cache-0004-while-using-windown-function-and-order-by.patch` |

### #5
| Field | Value |
|-------|-------|
| **Hash** | 577811071583aa0943000d5119bd30c3a1fbbcc8 |
| **Merge date** | 2025-08-20 |
| **Author** | Guilhem Bichot |
| **Subject** | Scalar subquery + length(BLOB)<? condition returns wrong result with cached plan |
| **Scope** | Wrong result fix |
| **Patch** | `plan_cache-0005-enable-plan-cache-a-query-incluing-a-scalar.patch` |

### #6
| Field | Value |
|-------|-------|
| **Hash** | b6ae57df07bd0d74b4b309dfe9e81e54930e77ec |
| **Merge date** | 2025-08-20 |
| **Author** | Guilhem Bichot |
| **Subject** | Scalar subquery + length(BLOB)<? condition returns wrong result (continued) |
| **Scope** | Wrong result fix (continued) |
| **Patch** | `plan_cache-0006-enable-plan-cache-a-query-incluing-a-scalar.patch` |

### #7
| Field | Value |
|-------|-------|
| **Hash** | f8aaa06a88a764ee706501bfc029ec7b5abb310f |
| **Merge date** | 2025-08-20 |
| **Author** | Guilhem Bichot |
| **Subject** | Debug assert failure for plan cache on DStore environment |
| **Scope** | Assert failure fix |
| **Patch** | `plan_cache-0007-debug-assert-failure-for-plan-cache-on.patch` |

### #8
| Field | Value |
|-------|-------|
| **Hash** | 060d032e42709e03f95c6301d73ec10ed0e82dab |
| **Merge date** | 2025-08-20 |
| **Author** | Guilhem Bichot |
| **Subject** | Prepared query with "order by int, tinytext" coredumps on cached plan |
| **Scope** | Crash fix |
| **Patch** | `plan_cache-0008-while-executing-a-prepared-query-including.patch` |

### #9
| Field | Value |
|-------|-------|
| **Hash** | d1a11b1734b7149cbf4bad86cef0d0712b27dffb |
| **Merge date** | 2025-08-20 |
| **Author** | Guilhem Bichot |
| **Subject** | Prepared query with many aggregate functions coredumps on cached plan |
| **Scope** | Crash fix |
| **Patch** | `plan_cache-0009-while-executing-a-prepared-query-including.patch` |

### #10
| Field | Value |
|-------|-------|
| **Hash** | cb5f7a27ccbc376614268adffcd7d3d76ca964d2 |
| **Merge date** | 2025-08-20 |
| **Author** | Guilhem Bichot |
| **Subject** | Time/timestamp type conditions return wrong result on second cached execution |
| **Scope** | Wrong result fix |
| **Patch** | `plan_cache-0010-enable-plan-cache-set-differents-values-and.patch` |

### #11
| Field | Value |
|-------|-------|
| **Hash** | 269a715946bbe04478568f8c7f5db693e2876eaa |
| **Merge date** | 2025-09-05 |
| **Author** | Guilhem Bichot |
| **Subject** | ASan coredump in CTE query with plan_cache enabled |
| **Scope** | Crash fix (ASan) |
| **Patch** | `plan_cache-0011-plan-cache-asan-using-the-asan-pkg-to.patch` |

### #12
| Field | Value |
|-------|-------|
| **Hash** | f423351bc185f234e539ae60dd430741f38e4e07 |
| **Merge date** | 2025-09-18 |
| **Author** | Guilhem Bichot |
| **Subject** | Wrong sort result with tinyblob IS NOT UNKNOWN and NOT BETWEEN in plan_cache |
| **Scope** | Wrong result fix |
| **Patch** | `plan_cache-0012-plan-cache-wrong-result-after-enable.patch` |

### #13-#18 (memory footprint optimization series)
| # | Hash | Author | Subject | Note |
|---|------|--------|---------|------|
| 13 | fa355c84418 | Guilhem Bichot | Add Cached_plan_invalidations status variable | |
| 14 | ac7b9c789c5 | Guilhem Bichot | Do not allocate TABLE_LIST when caching a plan | |
| 15 | 9ac621ef5ac | Guilhem Bichot | Allocate cached plan into dedicated MEM_ROOT | |
| 16 | 03750ed13a1 | Guilhem Bichot | Unify session_plan_cache_debug test with include file | |
| **17** | **20b5993337e** | **Roy Lyseng (Oracle)** | **Backport Bug#36458177 from MySQL community** | **Community commit — original message preserved, no sanitization** |
| 18 | 49ad914fee7 | Guilhem Bichot | Fix Valgrind errors after previous commits | |

### #19
| Field | Value |
|-------|-------|
| **Hash** | 2025fb96fcd3753835016e69ee6ff5db7c7e7210 |
| **Merge date** | 2025-12-03 |
| **Author** | husishu |
| **Subject** | Fix some codecheck |
| **Scope** | Codecheck fix in sql_plan_cache.cc |
| **Patch** | `plan_cache-0019-fix-some-codecheck.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 18 follow-up)** | **19** |

### Apply patches:
```bash
git am patches/plan_cache-*.patch
```
