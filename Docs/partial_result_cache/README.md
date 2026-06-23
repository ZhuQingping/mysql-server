# Partial Result Cache

Partial Result Cache (PRC, also called PTRC in the implementation task
history) is an optimizer and executor feature for repeated parameterized inner
work in nested-loop joins and deterministic correlated subqueries.

The feature adds a per-statement cache node above an eligible inner access
path. The cache key is built from outer parameter values. If the same key is
seen again, cached rows are returned instead of rescanning the inner path.

This directory contains the community-facing design and validation material:

- [High Level Design](high-level-design.md)
- [Low Level Design](low-level-design.md)
- [Test Report](test-report.md)
- [Performance Report](performance-report.md)
- [Community Submission Notes](community-submission.md)

The SQL-visible feature name is `partial_result_cache`.
