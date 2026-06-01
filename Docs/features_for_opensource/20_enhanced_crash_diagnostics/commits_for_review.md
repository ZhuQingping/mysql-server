# Enhanced Crash Diagnostics Feature - Commit List for Review

> Generated: 2026-05-25
> Main commit: 2ef131b5d1e4cbc0d8b2faeb625a7ef1ca5d6c55
> Branch: taurusondstore_develop

## Commit List (ordered by merge time / committer date)

### #1 - Stack trace truncation fix (prerequisite)
| Field | Value |
|-------|-------|
| **Hash** | eb31ce39214a28173c29a3d976c0021ac56dc515 |
| **Merge date** | 2025-01-31 |
| **Author** | Sven Sandberg |
| **Subject** | BUG#37543598: Do not truncate long function names in demangled stack traces |
| **Files changed** | 1 file, +10 / -6 |
| **Scope** | Bug fix: prevents truncation of long demangled function names in crash stack traces |
| **Patch** | `enhanced_crash_diagnostics-0001-BUGFIX-Do-not-truncate-long-function-names-in-de.patch` |

### #2 - Unit test for my_print_stacktrace (prerequisite)
| Field | Value |
|-------|-------|
| **Hash** | c8da08353b4f9811e1cb39ceff8c1c1caa485e39 |
| **Merge date** | 2025-07-24 |
| **Author** | Tor Didriksen |
| **Subject** | Bug#38235429 Add unit test for my_print_stacktrace |
| **Files changed** | 3 files, +66 / -7 |
| **Scope** | Add unit test for my_print_stacktrace and ensure mysys_objlib is built with libbacktrace |
| **Patch** | `enhanced_crash_diagnostics-0002-BUGFIX-Add-unit-test-for-my_print_stacktrace.patch` |

### #3 - Main Enhanced Crash Diagnostics feature commit
| Field | Value |
|-------|-------|
| **Hash** | 2ef131b5d1e4cbc0d8b2faeb625a7ef1ca5d6c55 |
| **Merge date** | 2026-03-27 |
| **Author** | songliyong |
| **Subject** | FFIC: Add enhanced crash diagnostics with rds_ffic_verbose_crash_diagnostics |
| **Files changed** | 17 files, +1,816 / -21 |
| **Scope** | Full Enhanced Crash Diagnostics feature: extended crash report with CPU registers, stack memory, instruction bytes, FNV-1a fingerprint, process resources, and query context |
| **Patch** | `enhanced_crash_diagnostics-0003-USERSTORY-Add-enhanced-crash-diagnostics-with-rds_.patch` |

---

## Summary

| Category | Count |
|----------|-------|
| **Total (1 main + 2 prerequisites)** | **3** |

### Apply patches:
```bash
git am patches/enhanced_crash_diagnostics-*.patch
```
