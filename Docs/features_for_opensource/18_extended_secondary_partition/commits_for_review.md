# Extended Secondary Partition Types Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: 9fd5bbe38853e06f1f450379954ea56b8d823e96
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main Extended Secondary Partition Types feature commit
| Field | Value |
|-------|-------|
| **Hash** | 9fd5bbe38853e06f1f450379954ea56b8d823e96 |
| **Merge date** | 2025-08-19 |
| **Author** | xurong |
| **Subject** | Support more combinations of partition types and extended DD_VERSION |
| **Files changed** | 122 files, +63,658 / -1,509 |
| **Scope** | Full Extended Secondary Partition Types: more subpartition type combinations (range-range, list-list, etc.) and DD version upgrade |
| **Patch** | `extended_secondary_partition-0001-USERSTORY-support-more-combinations-of-all-kin.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | 26e6bcf724e08c329b976eb9f4a2cfa4a10a8e91 |
| **Merge date** | 2025-08-26 |
| **Author** | xurong |
| **Subject** | Fix two bugs of subpartition types support |
| **Files changed** | 5 files, +326 / -60 |
| **Scope** | Bug fix: two subpartition type support issues |
| **Patch** | `extended_secondary_partition-0002-BUGFIX-fix-two-bugs-of-subpartition-types-.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | e9f2cfcb48d55d8ec98e2fe540476ace253aee36 |
| **Merge date** | 2026-03-24 |
| **Author** | xuemengjiao |
| **Subject** | Fix crash when subpartition type is list/range with no part_def defined |
| **Files changed** | 7 files, +626 / -1 |
| **Scope** | Crash fix: crash when subpartition type is list/range with no partition definition |
| **Patch** | `extended_secondary_partition-0003-BUGFIX-when-subpartition-type-is-list-rang.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 2 follow-up)** | **3** |

### Apply patches:
```bash
git am patches/extended_secondary_partition-*.patch
```
