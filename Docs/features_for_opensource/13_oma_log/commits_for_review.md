# OMA Log Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: 27b20ff70a8fa592da238055302c68efa3b739ef
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main OMA Log feature commit
| Field | Value |
|-------|-------|
| **Hash** | 27b20ff70a8fa592da238055302c68efa3b739ef |
| **Merge date** | 2025-07-08 |
| **Author** | kuangxiangbin |
| **Subject** | Add oma.log to record DDL statements |
| **Files changed** | 25 files, +1,381 / -31 |
| **Scope** | Full OMA Log feature: record DDL statements in oma.log for operational monitoring |
| **Patch** | `oma_log-0001-USERSTORY-Add-oma.log-to-record-DDL-statements.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | d4d6cf393caefbe73dba3e7efb7dada243040929 |
| **Merge date** | 2025-07-11 |
| **Author** | kuangxiangbin |
| **Subject** | Fix unstable oma.log mtr |
| **Scope** | MTR test stability fix |
| **Patch** | `oma_log-0002-USERSTORY-fix-unstable-oma.log-mtr.patch` |

### #3
| Field | Value |
|-------|-------|
| **Hash** | 0d5893eb28a5739c7e8a9b059a8b64051e501ea2 |
| **Merge date** | 2025-07-29 |
| **Author** | kuangxiangbin |
| **Subject** | Fix unstable oma.log mtr |
| **Scope** | MTR test stability fix |
| **Patch** | `oma_log-0003-BUGFIX-fix-unstable-oma.log-mtr.patch` |

### #4
| Field | Value |
|-------|-------|
| **Hash** | db0a65fa20a4182247e31c5fee02adaa46eaf075 |
| **Merge date** | 2025-08-07 |
| **Author** | kuangxiangbin |
| **Subject** | Open oma log |
| **Scope** | Enable oma log by configuration |
| **Patch** | `oma_log-0004-BUGFIX-open-oma-log.patch` |

### #5
| Field | Value |
|-------|-------|
| **Hash** | 58a8a8d84ab8264424dfbfc6b8383b18375a678f |
| **Merge date** | 2025-08-25 |
| **Author** | kuangxiangbin |
| **Subject** | Set oma log to be enabled by default |
| **Scope** | Enable OMA log by default |
| **Patch** | `oma_log-0005-USERSTORY-Set-oma-log-to-be-enabled-by-default.patch` |

### #6
| Field | Value |
|-------|-------|
| **Hash** | 475b5e8a0b73dd415239baa3c597dad30a2772dc |
| **Merge date** | 2025-10-13 |
| **Author** | zhuqingping |
| **Subject** | OMA log lock time not correct |
| **Scope** | Bug fix: OMA log lock time recording error |
| **Patch** | `oma_log-0006-BUGFIX-OMA-log-lock-time-not-correct.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 5 follow-up)** | **6** |

### Apply patches:
```bash
git am patches/oma_log-*.patch
```
