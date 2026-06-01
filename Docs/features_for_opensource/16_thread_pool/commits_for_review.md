# Thread Pool Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: 21481fa93ed3fecab600633b76fc6f9c057d3d05
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main Thread Pool feature commit
| Field | Value |
|-------|-------|
| **Hash** | 21481fa93ed3fecab600633b76fc6f9c057d3d05 |
| **Merge date** | 2025-04-04 |
| **Author** | songliyong |
| **Subject** | threadpool for dstore engine |
| **Files changed** | 68 files, +6,732 / -107 |
| **Scope** | Full Thread Pool feature: thread pool plugin for DStore engine |
| **Patch** | `thread_pool-0001-USERSTORY-threadpool-for-dstore-engine.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | 5be8af3c4442f14b187c0d29247ee379254687f9 |
| **Merge date** | 2025-04-04 |
| **Author** | songliyong |
| **Subject** | Support threadpool cpu bind |
| **Files changed** | 9 files, +333 / -23 |
| **Scope** | Extension: CPU binding support for thread pool |
| **Patch** | `thread_pool-0002-USERSTORY-support-threadpool-cpu-bind.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | 411af99bdf6882100395d8cd16f5e42e68c62b4a |
| **Merge date** | 2025-04-10 |
| **Author** | songliyong |
| **Subject** | Fix threadpool related mtr testcases |
| **Files changed** | 3 files, +282 |
| **Scope** | MTR test fix |
| **Patch** | `thread_pool-0003-USERSTORY-fix-threadpool-related-mtr-testcases.patch` |

### #4
| Field | Value |
|-------|-------|
| **Hash** | 4ad65eecbee05d1988909da425b787907b92a0c1 |
| **Merge date** | 2025-05-16 |
| **Author** | songliyong |
| **Subject** | Admin port and local socket use per thread |
| **Files changed** | 33 files, +949 / -36 |
| **Scope** | Extension: admin port and local socket connection handling in thread pool |
| **Patch** | `thread_pool-0004-USERSTORY-admin-port-and-local-socket-use-per-.patch` |

### #5
| Field | Value |
|-------|-------|
| **Hash** | 34e631aeb417d16484b454aea5a32815c2f83983 |
| **Merge date** | 2025-05-17 |
| **Author** | songliyong |
| **Subject** | Set cpubind to 'nobind' if passed cpubind is invalid |
| **Files changed** | 5 files, +68 / -9 |
| **Scope** | Bug fix: handle invalid CPU binding gracefully |
| **Patch** | `thread_pool-0005-BUGFIX-set-cpubind-to-nobind-if-use-passed.patch` |

### #6
| Field | Value |
|-------|-------|
| **Hash** | 7cf534ee20776828ad8e7af19cb06c26395b8f2b |
| **Merge date** | 2025-08-22 |
| **Author** | songliyong |
| **Subject** | Fix threadpool sysbench stall issue |
| **Files changed** | 3 files, +3 / -5 |
| **Scope** | Bug fix: thread pool stall under sysbench load |
| **Patch** | `thread_pool-0006-BUGFIX-fix-threadpool-sysbench-stall-issue.patch` |

### #7
| Field | Value |
|-------|-------|
| **Hash** | 5704a73548f0d7c965cd8b334fdd2039cf47d757 |
| **Merge date** | 2025-09-04 |
| **Author** | zhuqingping |
| **Subject** | Update plugin author for huawei new added plugins |
| **Files changed** | 5 files, +15 / -3 |
| **Scope** | Metadata: update plugin author information |
| **Patch** | `thread_pool-0007-BUGFIX-Update-plugin-author-for-huawei-new.patch` |

### #8
| Field | Value |
|-------|-------|
| **Hash** | 851041ee3df6bcfb9057ae709ebce4ceb47916fc |
| **Merge date** | 2025-09-18 |
| **Author** | songliyong |
| **Subject** | Fix kill wrong thread bug |
| **Files changed** | 8 files, +230 / -5 |
| **Scope** | Bug fix: incorrect thread killed in thread pool |
| **Patch** | `thread_pool-0008-BUGFIX-fix-kill-wrong-thrd-bug.patch` |

### #9
| Field | Value |
|-------|-------|
| **Hash** | e4de738980b3bb9269107672a7df033ba7dfc23f |
| **Merge date** | 2025-12-12 |
| **Author** | songliyong |
| **Subject** | Fix query information_schema table abort issue |
| **Files changed** | 11 files, +96 / -27 |
| **Scope** | Bug fix: query on information_schema causes abort in thread pool |
| **Patch** | `thread_pool-0009-BUGFIX-fix-query-information_schema-table-.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 8 follow-up)** | **9** |

### Notes
- #4 (admin port and local socket) may be a separate feature extension rather than a pure thread pool fix
- #7 (update plugin author) touches multiple plugins, not just thread pool

### Apply patches:
```bash
git am patches/thread_pool-*.patch
```
