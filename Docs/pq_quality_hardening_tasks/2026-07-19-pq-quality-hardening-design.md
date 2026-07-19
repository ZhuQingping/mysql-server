# Parallel Query Quality Hardening Design

**Goal:** Establish a reproducible Parallel Query quality-hardening branch from
`9667919bfa9`, then make its Release and Debug builds plus all applicable PQ MTR
tests stable without importing unrelated working-tree fixes.

## Baseline and isolation

- Branch: `pq-quality-hardening-baseline`.
- Start commit: `9667919bfa9cc42c13b905329f2b8c4538f076ff` (`Fix local source build`).
- Explicitly excluded commits: `2b4de0feb42` (`Guard range optimizer range
  memory`) and `85e16ee8646` (`pq: harden read_record cursor restore after mtr
  restart`).
- The source branch `range_oom` remains in its original worktree and is never
  checked out, stashed, reset, or modified by this work. Its dirty changes are
  retained there as a candidate-only reference for selective backports.

## Approach

1. Configure independent `build-ninja-release/` and `build-ninja-debug/`
   directories in this worktree using the repository's documented CMake inputs.
2. Build `mysqld` in both configurations. Record the first complete compiler
   error for each configuration and classify any current-worktree diff that
   addresses it as a candidate patch.
3. Backport only the smallest candidate patch whose causal relationship to a
   baseline compiler failure is established. Do not import either excluded PQ
   behavior fix or unrelated dirty files.
4. Run the PQ MTR suite separately in Release and Debug. Classify each failure
   as product behavior, expected-result drift, test defect, build/configuration
   defect, Debug-only injection requirement, or unavailable optional component.
5. Treat Dstore as an optional unavailable component in this environment. The
   `have_dstore.inc` and `have_only_dstore.inc` gates must test availability,
   skip Dstore-only tests with a clear reason when unavailable, and never turn
   a missing engine into a passing assertion.
6. For every product or test defect, add or correct the narrowest MTR regression
   test, observe its baseline failure, implement one root-cause fix, and rerun
   the focused test before the suite.

## Acceptance gates

- Both Release and Debug `mysqld` targets compile from this branch.
- Every applicable `mysql-test/suite/parallel_query` test passes in its intended
  configuration.
- Any skipped test has a deterministic capability/feature reason recorded in a
  baseline report; Dstore absence is not reported as a test pass.
- No commit contains the two explicitly excluded commits, their code hunks, or
  unrelated user changes from `range_oom`.
- Each code-fix commit contains its focused regression test and the exact build
  and MTR commands used for verification.

## Evidence and reporting

Raw build/MTR logs remain build artifacts and are not committed. The branch
tracks a concise failure matrix with command, configuration, test name,
classification, root cause, fix commit, and final result. A claim that the
branch is stable requires fresh Release and Debug build output plus a final
suite result, not only focused-test output.
