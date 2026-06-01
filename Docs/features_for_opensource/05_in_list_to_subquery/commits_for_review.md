# IN-List to Subquery Conversion Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: 8eb87e1268aa537fed7f7e5f404686b1ea67a249
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main IN-List to Subquery feature commit
| Field | Value |
|-------|-------|
| **Hash** | 8eb87e1268aa537fed7f7e5f404686b1ea67a249 |
| **Merge date** | 2025-08-07 |
| **Author** | zhangxinyu |
| **Subject** | backport Transform in-list to subquery |
| **Files changed** | 31 files, +4,618 / -14 |
| **Scope** | Full IN-List to Subquery feature: transform IN predicates to subqueries for optimizer flexibility |
| **Patch** | `in_list_to_subquery-0001-USERSTORY-backport-Transform-in-list-to-subque.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | 558c15d12e5cf8989983a57aa38baa34342ad1b9 |
| **Merge date** | 2025-08-25 |
| **Author** | gebinbin |
| **Subject** | Add support for temporary table |
| **Scope** | Extension: IN-List to Subquery support for temporary tables |
| **Patch** | `in_list_to_subquery-0002-FEATURE-add-support-for-temporary-table.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | 4d31398db3d40a5198e05772da5b4248b7fc98c3 |
| **Merge date** | 2025-09-09 |
| **Author** | zhuqingping |
| **Subject** | Crash for in predicate to temp table + PS protocol |
| **Scope** | Crash fix: IN predicate to temporary table with prepared statement protocol |
| **Patch** | `in_list_to_subquery-0003-BUGFIX-Crash-occur-for-in-predicate-to-tem.patch` |

### #4
| Field | Value |
|-------|-------|
| **Hash** | 3d14cb3e754a1d80ed69ada0288cc85c2bd68294 |
| **Merge date** | 2025-11-29 |
| **Author** | niuxiangnan |
| **Subject** | Fix mtr of change max tuple length to 950 |
| **Scope** | MTR test fix: adjust test for max tuple length change |
| **Patch** | `in_list_to_subquery-0004-BUGFIX-fix-mtr-of-change-max-tuple-length-.patch` |

### #5
| Field | Value |
|-------|-------|
| **Hash** | becb27e275ec7091b27fafb6e688e64eaa0842b0 |
| **Merge date** | 2025-12-02 |
| **Author** | pengnian |
| **Subject** | Fix AT mtr of changing max tuple length to 950 |
| **Scope** | MTR test fix: additional test adjustment for AT pipeline |
| **Patch** | `in_list_to_subquery-0005-BUGFIX-fix-AT-mtr-of-changing-max-tuple-le.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 4 follow-up)** | **5** |

### Apply patches:
```bash
git am patches/in_list_to_subquery-*.patch
```
