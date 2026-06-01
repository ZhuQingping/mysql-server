# Offset Pushdown Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: 6bb266806f33dd1b019c96124c9bd9b86eaff474
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main Offset Pushdown feature commit
| Field | Value |
|-------|-------|
| **Hash** | 6bb266806f33dd1b019c96124c9bd9b86eaff474 |
| **Merge date** | 2025-07-26 |
| **Author** | songliyong |
| **Subject** | Support offset pushdown |
| **Files changed** | 67 files, +6,898 / -151 |
| **Scope** | Full Offset Pushdown feature: push OFFSET operations down to storage engine level |
| **Patch** | `offset_pushdown-0001-USERSTORY-Support-offset-pushdown.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | 89600f53af34b91d16189ce01e6893203aa5931b |
| **Merge date** | 2025-11-11 |
| **Author** | songliyong |
| **Subject** | Dstore support offset pushdown |
| **Files changed** | 27 files, +3,336 / -35 |
| **Scope** | Extension: offset pushdown support for DStore storage engine |
| **Patch** | `offset_pushdown-0002-USERSTORY-dstore-support-offset-pushdown.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | b85d7ab78a5d021b12a6eaf3139ede4d08812a11 |
| **Merge date** | 2025-11-20 |
| **Author** | songliyong |
| **Subject** | Fix offset pushdown conflicts with MRR issue |
| **Files changed** | 7 files, +109 / -1 |
| **Scope** | Bug fix: offset pushdown conflicting with Multi-Range Read |
| **Patch** | `offset_pushdown-0003-BUGFIX-fix-offset-pushdown-conflicts-with-.patch` |

### #4
| Field | Value |
|-------|-------|
| **Hash** | 32a7b1888429d74abeebaa4b3936f2959afde4d8 |
| **Merge date** | 2025-12-22 |
| **Author** | songliyong |
| **Subject** | Fix temp table offset pushdown issue |
| **Files changed** | 3 files, +25 / -1 |
| **Scope** | Bug fix: offset pushdown with temporary tables |
| **Patch** | `offset_pushdown-0004-BUGFIX-fix-temp-table-offset-pushdown-issu.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 3 follow-up)** | **4** |

### Apply patches:
```bash
git am patches/offset_pushdown-*.patch
```
