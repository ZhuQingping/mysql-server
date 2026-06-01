# Recycle Bin Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: 143ed22e468405b2cc01b35b46ee6975d4422628
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main Recycle Bin feature commit
| Field | Value |
|-------|-------|
| **Hash** | 143ed22e468405b2cc01b35b46ee6975d4422628 |
| **Merge date** | 2026-01-27 |
| **Author** | husishu |
| **Subject** | Dstore supports recyclebin |
| **Files changed** | 262 files, +11,823 / -124 |
| **Scope** | Full Recycle Bin feature: support for dropping tables into recycle bin instead of permanent deletion |
| **Patch** | `recycle_bin-0001-USERSTORY-Dstore-supports-recyclebin.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | 654f5f786c1f292d3777cf99db81f415525e61a8 |
| **Merge date** | 2026-02-10 |
| **Author** | wangyunhua |
| **Subject** | Fix crash in foreign key cases with table recycle bin enabled |
| **Files changed** | 3 files, +61 / -1 |
| **Scope** | Crash fix: foreign key interaction with recycle bin |
| **Patch** | `recycle_bin-0002-BUGFIX-Fix-crash-in-foreign-key-cases-with.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | e4f34bdce7a21d25b0e876d866cc33ec0efc4e5e |
| **Merge date** | 2026-02-14 |
| **Author** | wanghan |
| **Subject** | Fix crash on truncate table with instant drop column when recycle bin enabled |
| **Files changed** | 18 files, +670 / -11 |
| **Scope** | Crash fix: truncate with instant drop column and recycle bin |
| **Patch** | `recycle_bin-0003-BUGFIX-Fix-crash-on-truncate-table-with-in.patch` |

### #4
| Field | Value |
|-------|-------|
| **Hash** | 1e0934bca756449b4f46e4ae7f6bef405e95295b |
| **Merge date** | 2026-04-02 |
| **Author** | wangyunhua |
| **Subject** | Fix drop table failure when recycle bin is enabled |
| **Files changed** | 6 files, +226 / -1 |
| **Scope** | Bug fix: drop table fails when recycle bin is enabled |
| **Patch** | `recycle_bin-0004-BUGFIX-Fix-drop-table-failure-when-recycle.patch` |

### #5
| Field | Value |
|-------|-------|
| **Hash** | f908a9dd2246d28795a7c80a139347a0035a6fdd |
| **Merge date** | 2026-04-08 |
| **Author** | wanghan |
| **Subject** | Fix memory leak caused by truncate when recycle bin is enabled |
| **Files changed** | 1 file, +1 / -1 |
| **Scope** | Bug fix: memory leak in truncate with recycle bin |
| **Patch** | `recycle_bin-0005-BUGFIX-Fixed-a-memory-leak-issue-caused-by.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 4 follow-up)** | **5** |

### Apply patches:
```bash
git am patches/recycle_bin-*.patch
```
