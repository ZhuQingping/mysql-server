# Redundant Condition Removal Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: b0ecd8c2a2aa90e8d2fe8816bad2625330047d6d
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main Redundant Condition Removal feature commit
| Field | Value |
|-------|-------|
| **Hash** | b0ecd8c2a2aa90e8d2fe8816bad2625330047d6d |
| **Merge date** | 2025-05-14 |
| **Author** | andong |
| **Subject** | port removal of redundant conditions |
| **Files changed** | 220 files, +5,184 / -1,344 |
| **Scope** | Full Redundant Condition Removal: optimize away redundant conditions in range scans |
| **Patch** | `redundant_condition_removal-0001-USERSTORY-port-removal-of-redundant-conditions.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | 07d56cc6acb856281724bdef0b8833aaec710024 |
| **Merge date** | 2025-08-12 |
| **Author** | zhuqingping |
| **Subject** | Removal of redundant conditions for range scan [post-fix] |
| **Files changed** | 31 files, +88 / -79 |
| **Scope** | Post-fix for redundant condition removal in range scan |
| **Patch** | `redundant_condition_removal-0002-BUGFIX-Removal-of-redundant-conditions-for.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | ea82404780ae813d2e4db64f8d4924e4acdf0bd9 |
| **Merge date** | 2026-03-20 |
| **Author** | wangyunhua |
| **Subject** | Fix incorrect result caused by rds_empty_redundant_check_in_range_scan |
| **Files changed** | 7 files, +411 / -7 |
| **Scope** | Wrong result fix: incorrect results caused by redundant condition check in range scan |
| **Patch** | `redundant_condition_removal-0003-BUGFIX-Fix-the-incorrect-result-issue-caus.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 2 follow-up)** | **3** |

### Notes
- #3 (ea82404780a) is shared with the Range Scan Optimization feature — it fixes an issue caused by rds_empty_redundant_check_in_range_scan

### Apply patches:
```bash
git am patches/redundant_condition_removal-*.patch
```
