# Left Join Elimination Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: bf20096d415b47664fc4a7685d4f0dd6ceaccfc3
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main Left Join Elimination feature commit
| Field | Value |
|-------|-------|
| **Hash** | bf20096d415b47664fc4a7685d4f0dd6ceaccfc3 |
| **Merge date** | 2025-11-13 |
| **Author** | Guilhem Bichot |
| **Subject** | LEFT JOIN elimination |
| **Files changed** | 66 files, +7,101 / -120 |
| **Scope** | Full Left Join Elimination feature: eliminate unnecessary left joins in query optimization |
| **Patch** | `left_join_elimination-0001-USERSTORY-LEFT-JOIN-elimination.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | b42b72aba8535d530018e674ce0e8db081922cdd |
| **Merge date** | 2025-11-19 |
| **Author** | Guilhem Bichot |
| **Subject** | Crash in JOIN::attach_join_condition_to_nest with SQLsmith |
| **Scope** | Crash fix: null pointer in join condition attachment |
| **Patch** | `left_join_elimination-0002-BUGFIX-When-100-SQLsmith-services-are-exec.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | 52683f8144a0823e0f5de3857d0e299f25603a8b |
| **Merge date** | 2025-12-08 |
| **Author** | Guilhem Bichot |
| **Subject** | The results of the left join query are inconsistent |
| **Scope** | Wrong result fix: left join elimination producing incorrect results |
| **Patch** | `left_join_elimination-0003-BUGFIX-The-results-of-the-left-join-query-.patch` |

### #4
| Field | Value |
|-------|-------|
| **Hash** | ea619894188993d7876abe180ba534afabce1001 |
| **Merge date** | 2025-12-08 |
| **Author** | Guilhem Bichot |
| **Subject** | Crash when partial_result_cache + left_join_elimination enabled simultaneously |
| **Scope** | Crash fix: CreateNestedLoopAccessPath null pointer |
| **Patch** | `left_join_elimination-0004-BUGFIX-Execute-query-and-enable-partial_re.patch` |

### #5
| Field | Value |
|-------|-------|
| **Hash** | 7b0e639b5c6a61d7b259534d61aa3801634c0615 |
| **Merge date** | 2026-03-19 |
| **Author** | Guilhem Bichot |
| **Subject** | Crash in JOIN::compare_costs_of_subquery_strategies |
| **Scope** | Crash fix: null pointer in subquery cost comparison with join elimination |
| **Patch** | `left_join_elimination-0005-BUGFIX-crash-in-JOIN-compare_costs_of_subq.patch` |

### #6
| Field | Value |
|-------|-------|
| **Hash** | 7b5ee97196637922a4f565ab5f28c15c25f26ea3 |
| **Merge date** | 2026-03-19 |
| **Author** | Guilhem Bichot |
| **Subject** | Crash in JOIN::compare_costs_of_subquery_strategies (continued) |
| **Scope** | Additional fix for same crash scenario |
| **Patch** | `left_join_elimination-0006-BUGFIX-crash-in-JOIN-compare_costs_of_subq.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 5 follow-up)** | **6** |

### Notes
- #4 (ea619894188) is shared with the Partial Result Cache feature — it fixes a crash that occurs when both features are enabled
- #5 and #6 have the same BUG ID but are different commits: #5 is the main fix (10 files, +195/-13), #6 is a follow-up refinement (4 files, +72/-1)

### Apply patches:
```bash
git am patches/left_join_elimination-*.patch
```
