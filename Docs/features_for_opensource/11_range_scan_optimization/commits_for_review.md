# Range Scan Optimization Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: cca19c65133ed279469d0c719a566d1b7dad4d46
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main Range Scan Optimization commit
| Field | Value |
|-------|-------|
| **Hash** | cca19c65133ed279469d0c719a566d1b7dad4d46 |
| **Merge date** | 2025-03-19 |
| **Author** | Guilhem Bichot |
| **Subject** | Range scan optimization: reduce unnecessary index cell scanning |
| **Files changed** | 10 files, +68 / -11 |
| **Scope** | Optimization: reduce range optimizer scanning too many index cells |
| **Patch** | `range_scan_optimization-0001-BUGFIX.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | ea82404780ae813d2e4db64f8d4924e4acdf0bd9 |
| **Merge date** | 2026-03-20 |
| **Author** | wangyunhua |
| **Subject** | Fix incorrect result caused by rds_empty_redundant_check_in_range_scan |
| **Files changed** | 7 files, +411 / -7 |
| **Scope** | Wrong result fix: incorrect results caused by redundant condition check in range scan |
| **Patch** | `range_scan_optimization-0002-BUGFIX-Fix-the-incorrect-result-issue-caus.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 1 follow-up)** | **2** |

### Notes
- #1 main commit subject in git is just the BUG ID (BUGFIX). Descriptive subject inferred from code changes.
- #2 (ea82404780a) is shared with the Redundant Condition Removal feature

### Apply patches:
```bash
git am patches/range_scan_optimization-*.patch
```
