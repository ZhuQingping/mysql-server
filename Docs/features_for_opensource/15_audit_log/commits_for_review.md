# Audit Log Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: e1a26db5268845a98a5e3fcf03d77f9187865b77
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main Audit Log feature commit
| Field | Value |
|-------|-------|
| **Hash** | e1a26db5268845a98a5e3fcf03d77f9187865b77 |
| **Merge date** | 2025-08-18 |
| **Author** | zhangzhiqiang |
| **Subject** | Port Audit Log from TaurusDB |
| **Files changed** | 164 files, +15,702 / -29 |
| **Scope** | Full Audit Log feature: audit logging plugin ported from TaurusDB |
| **Patch** | `audit_log-0001-USERSTORY-Port-Audit-Log-from-TaurusDB.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | 153cd946cfc2a61d3e0f4c408a11cd9f1ddfc63e |
| **Merge date** | 2025-08-26 |
| **Author** | zhangzhiqiang |
| **Subject** | Fix bug: audit_log_include_commands not work |
| **Files changed** | 5 files, +54 / -2 |
| **Scope** | Bug fix: audit_log_include_commands option not functioning |
| **Patch** | `audit_log-0002-BUGFIX-Fix-bug-audit_log_include_commands-.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | 4973f70b46afe78ca9c4b32ea54a8ba4d3674f5f |
| **Merge date** | 2025-08-31 |
| **Author** | wangyunhua |
| **Subject** | User activities such as logins not recorded when audit logging not enabled |
| **Files changed** | 39 files, +911 / -9 |
| **Scope** | Bug fix: login events not recorded without audit logging enabled |
| **Patch** | `audit_log-0003-BUGFIX-User-activities-such-as-logins-are-.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 2 follow-up)** | **3** |

### Apply patches:
```bash
git am patches/audit_log-*.patch
```
