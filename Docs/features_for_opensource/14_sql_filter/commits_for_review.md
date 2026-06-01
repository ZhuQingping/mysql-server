# SQL Filter Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: b71519c3804e18b56a4b2f6953a24f732d53ae9f
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main SQL Filter feature commit
| Field | Value |
|-------|-------|
| **Hash** | b71519c3804e18b56a4b2f6953a24f732d53ae9f |
| **Merge date** | 2025-08-22 |
| **Author** | huangmenghong |
| **Subject** | backport sqlfilter feature from TaurusDB 930 |
| **Files changed** | 132 files, +11,314 / -166 |
| **Scope** | Full SQL Filter feature: filter and control SQL execution by rules |
| **Patch** | `sql_filter-0001-USERSTORY-backport-sqlfilter-feature-from-Taur.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | 3097bf82c474fe65804565e3d919446f998dd27b |
| **Merge date** | 2025-09-18 |
| **Author** | zhangzhiqiang |
| **Subject** | Fix m_cur_concur reduced to ULONG_MAX in high-concurrency |
| **Files changed** | 3 files, +60 / -1 |
| **Scope** | Bug fix: concurrency counter overflow in SQL Filter |
| **Patch** | `sql_filter-0002-BUGFIX-In-high-concurrency-scenarios-m_cur.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | dff61c79cfc6cb1247c16eaa95530b59055aeb51 |
| **Merge date** | 2025-09-25 |
| **Author** | zhangzhiqiang |
| **Subject** | flush_sql_filter() not trim keywords' spaces for key_str |
| **Files changed** | 3 files, +76 |
| **Scope** | Bug fix: keyword space trimming in flush_sql_filter() |
| **Patch** | `sql_filter-0003-BUGFIX-flush_sql_filter-not-trim-keywords-.patch` |

### #4
| Field | Value |
|-------|-------|
| **Hash** | 75026ab31ec09dad601b78b3b36620544f55ad10 |
| **Merge date** | 2026-03-24 |
| **Author** | tangxiangbing |
| **Subject** | Fix memory leak in SQL Filter feature |
| **Files changed** | 7 files, +16 / -21 |
| **Scope** | Bug fix: memory leak in SQL Filter |
| **Patch** | `sql_filter-0004-BUGFIX-Fix-memory-leak-in-SQL-Filter-featu.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 3 follow-up)** | **4** |

### Apply patches:
```bash
git am patches/sql_filter-*.patch
```
