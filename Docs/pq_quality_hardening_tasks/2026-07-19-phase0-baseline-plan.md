# Parallel Query Quality Hardening Phase 0 Plan

> **For agentic workers:** Follow systematic root-cause investigation. Do not
> modify product or MTR source before a failing command and its complete output
> are captured for the baseline branch.

**Goal:** Produce a reproducible Release/Debug build and PQ MTR failure matrix
for `pq-quality-hardening-baseline`, rooted at `9667919bfa9` and excluding the
range-memory and cursor-restore fixes.

**Architecture:** The worktree owns two independent Ninja build directories.
Raw compiler and MTR logs stay under those build directories; a committed report
stores commands, outcomes, and failure classifications. Product changes are out
of scope for this phase: they begin only in a subsequent root-cause plan.

**Tech Stack:** CMake, Ninja, MySQL Test Run, MySQL 8.0.41 / TaurusDB source.

## Global Constraints

- Use only `/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore-pq-quality-hardening-baseline`.
- Never checkout, reset, stash, or edit the original `range_oom` worktree.
- Do not cherry-pick `2b4de0feb42` or `85e16ee8646`.
- Use `build-ninja-release/` and `build-ninja-debug/`; never reuse a CMake cache
  created for a different source directory.
- Run MTR only from each build directory's `mysql-test/` directory.
- Do not label missing Dstore or audit plugins as a product test pass.

### Task 1: Record the immutable baseline

**Files:**
- Create: `Docs/pq_quality_hardening_tasks/phase0-baseline-report.md`

- [ ] Capture branch name, HEAD, `git status --short`, and `git merge-base`
  checks proving the two excluded commits are not ancestors.
- [ ] Record the exact CMake inputs from `AGENTS.md`, including Boost, Bison,
  OpenSSL, install prefix, and build type.
- [ ] Record whether the Dstore engine/plugin and `audit_log.so` are present in
  the baseline build environment; this is an availability observation, not a
  test result.

### Task 2: Configure and compile both build modes

**Files:**
- Modify: `Docs/pq_quality_hardening_tasks/phase0-baseline-report.md`
- Build artifact only: `build-ninja-release/`
- Build artifact only: `build-ninja-debug/`

- [ ] Configure Release with `-DCMAKE_BUILD_TYPE=Release`, `-DWITH_UNIT_TESTS=0`,
  `-DWITH_ASAN=0`, and the dependency paths required by `AGENTS.md`.
- [ ] Run `ninja mysqld -j 24`; capture the complete first failure or successful
  command output path in the report.
- [ ] Configure Debug with `-DCMAKE_BUILD_TYPE=Debug` and the same dependency
  paths; run `ninja mysqld -j 24` and record the result.
- [ ] If either mode fails, classify the failing translation unit, symbol, and
  candidate current-worktree patch. Do not backport a patch in this task.

### Task 3: Establish MTR failure inventory

**Files:**
- Modify: `Docs/pq_quality_hardening_tasks/phase0-baseline-report.md`

- [ ] From Release `build-ninja-release/mysql-test`, run
  `./mtr --suite=parallel_query --force --retry=0` after a successful build.
- [ ] From Debug `build-ninja-debug/mysql-test`, run the same command after a
  successful build.
- [ ] For each non-pass result, record test name, configuration, status,
  first diagnostic, and one classification: build/configuration, optional
  component unavailable, debug-only injection, expected-result drift, product
  behavior, test defect, or timeout/flaky.

### Task 4: Define the next fix waves

**Files:**
- Modify: `Docs/pq_quality_hardening_tasks/phase0-baseline-report.md`
- Create: `Docs/pq_quality_hardening_tasks/README.md`

- [ ] Group failures by shared first failing frame or infrastructure cause.
- [ ] Put build portability first, Dstore gate adaptation second, then one
  product/MTR root-cause family per subsequent plan.
- [ ] For every group, name the exact focused MTR command that must be observed
  failing before any production-code change and passing after its minimal fix.
- [ ] Mark Phase 0 complete only after both build attempts and every runnable
  suite attempt have an evidence-backed result.

## Phase 0 verification

```bash
git merge-base --is-ancestor 2b4de0feb42 HEAD; test $? -ne 0
git merge-base --is-ancestor 85e16ee8646 HEAD; test $? -ne 0
cd build-ninja-release && ninja mysqld -j 24
cd build-ninja-debug && ninja mysqld -j 24
cd build-ninja-release/mysql-test && ./mtr --suite=parallel_query --force --retry=0
cd build-ninja-debug/mysql-test && ./mtr --suite=parallel_query --force --retry=0
```

Expected outcome: the report contains fresh command outputs and an explicit
classification for every non-pass result. It does not claim that the suite is
stable until subsequent fix waves close those results.
