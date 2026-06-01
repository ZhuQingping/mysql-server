# Partial Table Result Cache (PTRC) Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: de8b6a55df87f90ed74ac5b8ae6e97bfffb340a8
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main PTRC feature commit
| Field | Value |
|-------|-------|
| **Hash** | de8b6a55df87f90ed74ac5b8ae6e97bfffb340a8 |
| **Merge date** | 2025-08-07 |
| **Author** | wuwenbin |
| **Subject** | Backport PTRC (Partial Table Result Cache) |
| **Files changed** | 102 files, +10,224 / -320 |
| **Scope** | Full PTRC feature: caching partial query results for nested loop joins, LRU cache, hash join buffer adaptation |
| **Patch** | `partial_result_cache-0001-backport-ptrc-this-mr-primarily-includes-the.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | 1600c3ddd8dbf4605be839062afcc1149e622686 |
| **Merge date** | 2025-08-27 |
| **Author** | wuwenbin |
| **Subject** | Turn on the PTRC switch |
| **Scope** | Enable PTRC by default |
| **Patch** | `partial_result_cache-0002-turn-on-the-ptrc-switch.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | f8325f6074b2e912142297681212ae344a001cd9 |
| **Merge date** | 2025-09-02 |
| **Author** | wuwenbin |
| **Subject** | Crash caused by TABLE::in_use being a null pointer |
| **Scope** | Crash fix: null pointer dereference in PTRC |
| **Patch** | `partial_result_cache-0003-crash-caused-by-table-in-use-being-a-null.patch` |

### #4
| Field | Value |
|-------|-------|
| **Hash** | dada643bc8ae335f44114e16aa1764b42224f7e3 |
| **Merge date** | 2025-09-25 |
| **Author** | wuwenbin |
| **Subject** | Fix codecheck & AT |
| **Scope** | Codecheck and AT fixes in partial_result_cache.h |
| **Patch** | `partial_result_cache-0004-fix-codecheck-at.patch` |

### #5
| Field | Value |
|-------|-------|
| **Hash** | ea619894188993d7876abe180ba534afabce1001 |
| **Merge date** | 2025-12-08 |
| **Author** | Guilhem Bichot |
| **Subject** | Execute query with partial_result_cache + left_join_elimination crashes |
| **Scope** | Crash fix: CreateNestedLoopAccessPath null pointer |
| **Patch** | `partial_result_cache-0005-execute-query-and-enable-partial-result-cache-and-left-join-elimination-crash.patch` |

### #6
| Field | Value |
|-------|-------|
| **Hash** | d053c95e8613b2483603f369eb7cab68886b7c97 |
| **Merge date** | 2025-12-12 |
| **Author** | wangjin |
| **Subject** | Fix PTRC result error in parallel_query.pq_reverse_index_scan |
| **Scope** | Wrong result fix: PTRC interaction with PQ reverse index scan |
| **Patch** | `partial_result_cache-0006-fix-ptrc-result-error-in.patch` |

### #7
| Field | Value |
|-------|-------|
| **Hash** | 47029cac349033bb3cf51b99fd57817011782256 |
| **Merge date** | 2026-01-08 |
| **Author** | wuwenbin |
| **Subject** | Fix PTRC crash caused by using QEP_shared::m_position before assignment |
| **Scope** | Crash fix: unassigned m_position in nested subquery |
| **Patch** | `partial_result_cache-0007-fix-a-ptrc-crash-caused-by-using.patch` |

### #8
| Field | Value |
|-------|-------|
| **Hash** | b9b14efbac6ead0c3db0a581c981e4dfc09c9776 |
| **Merge date** | 2026-01-05 |
| **Author** | gebinbin |
| **Subject** | Fix temporary table query crash |
| **Scope** | Crash fix: PTRC with temporary tables |
| **Patch** | `partial_result_cache-0008-fix-temporary-table-query-crash.patch` |

### #9
| Field | Value |
|-------|-------|
| **Hash** | ed4274c30ff090f711131a75d41a6ad69a6a5786 |
| **Merge date** | 2026-01-13 |
| **Author** | wangjin |
| **Subject** | Fix PQ review comments (includes PTRC test result updates) |
| **Scope** | PTRC test result and source fix (shared with PQ) |
| **Patch** | `partial_result_cache-0009-fix-pq-review-comments.patch` |

### #10
| Field | Value |
|-------|-------|
| **Hash** | 42d2a21c9f4e81bb913cb887d45da8c813a1ccc6 |
| **Merge date** | 2026-01-16 |
| **Author** | wuwenbin |
| **Subject** | Add use cases for PTRC basic scenarios and problem scenarios |
| **Scope** | Test case additions |
| **Patch** | `partial_result_cache-0010-add-use-cases-for-ptrc-basic-scenarios-and.patch` |

### #11
| Field | Value |
|-------|-------|
| **Hash** | fed79850f94b9f810beceae7d70ef32a4824f808 |
| **Merge date** | 2026-01-19 |
| **Author** | wuwenbin |
| **Subject** | Fix unstable case: execution plan for partial_result_cache is unstable |
| **Scope** | MTR test stability fix |
| **Patch** | `partial_result_cache-0011-fix-unstable-case-issue-the.patch` |

### #12
| Field | Value |
|-------|-------|
| **Hash** | 083448b8e8f1fa5d6fa132f7a0c6158ad32d8963 |
| **Merge date** | 2026-02-20 |
| **Author** | wuwenbin |
| **Subject** | Fix partial_result_cache test case |
| **Scope** | MTR test fix |
| **Patch** | `partial_result_cache-0012-fix-partial-result-cache-test-case.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 11 follow-up)** | **12** |

### Note
- #9 (`ed4274c30ff`) is shared with the PQ feature — it modifies both PQ and PTRC code/test files

### Apply patches:
```bash
git am patches/partial_result_cache-*.patch
```
