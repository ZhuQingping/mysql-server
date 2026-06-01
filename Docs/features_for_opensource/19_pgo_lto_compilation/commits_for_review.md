# PGO/LTO Compilation Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: 43b6a0f590c7eedd6b2cc2aa9e5e8ed643d821f8
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Main PGO/LTO Compilation feature commit
| Field | Value |
|-------|-------|
| **Hash** | 43b6a0f590c7eedd6b2cc2aa9e5e8ed643d821f8 |
| **Merge date** | 2025-08-30 |
| **Author** | wuxiliang |
| **Subject** | PGO/LTO/BOLT pipeline adaptation |
| **Files changed** | 16 files, +1,261 / -6 |
| **Scope** | Full PGO/LTO/BOLT Compilation feature: profile-guided optimization and link-time optimization build pipeline |
| **Patch** | `pgo_lto_compilation-0001-USERSTORY-PGO-LTO-BOLT-pipeline-adaptation.patch` |

### #2
| Field | Value |
|-------|-------|
| **Hash** | cf357619b8eb35c707d7233438fc8b64c1bd34b3 |
| **Merge date** | 2025-09-19 |
| **Author** | qinwei |
| **Subject** | Use sysbench PGO/LTO optimize InnoDB storage engine |
| **Files changed** | 18 files, +316 / -164 |
| **Scope** | Enhancement: PGO/LTO optimization with sysbench workload for InnoDB |
| **Patch** | `pgo_lto_compilation-0002-BUGFIX-Use-sysbench-PGO-LTO-optimize-InnoD.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 1 follow-up)** | **2** |

### Apply patches:
```bash
git am patches/pgo_lto_compilation-*.patch
```
