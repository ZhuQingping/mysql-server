# MDL Pinbox Capacity Feature - Commit for Review

> Generated: 2026-06-18
> Branch: `1million_connection`
> Base commit: `2b4de0feb42a55028f248b3af2543e5f539545aa`

## Review Entry Points

| Document | Purpose |
|---|---|
| [MDLPinboxCapacity-Feature-Spec.md](MDLPinboxCapacity-Feature-Spec.md) | 问题场景、源码分析、简化方案、实现摘要、兼容性和风险。 |
| [TestReport.md](TestReport.md) | 缩小 pinbox 集成复现、本 MR 构建、gunit、参数解析和线上验证 SQL。 |

## Main Commit

| Field | Value |
|---|---|
| **Subject** | `mdl: expand LF pinbox index space` |
| **Scope** | Lift the MDL LF pinbox capacity ceiling by expanding the LF pinbox index/version encoding. |
| **Files changed** | `include/lf.h`, `mysys/lf_alloc-pin.cc`, `sql/sys_vars.h`, `unittest/gunit/mdl-t.cc`, `unittest/gunit/mysys_lf-t.cc`, and this document set. |

## Issue

The original LF pinbox stored the free-list index and ABA version in a 32-bit word. The low 16 bits stored the pin index and the high 16 bits stored the version. Since index 0 is reserved, only 65535 pin indexes were usable.

MDL uses a process-global LF_HASH map. Each connection's `MDL_context` lazily allocates LF pins when it first touches that map. Therefore a server configured with `max_connections` higher than 65535 can still fail with `ER_MDL_OUT_OF_RESOURCES` before reaching the normal max-connections path.

## Solution

- Extend LF pinbox stack state from 32 bit to 64 bit.
- Use low 32 bits for pin index and high 32 bits for ABA version.
- Keep index 0 reserved.
- Increase the hard-coded LF pinbox capacity to 2M level.
- Raise the existing `max_connections` upper bound to 1000000.
- Do not add a new sysvar, status variable, or startup warning.
- Add gunit coverage that allocates past the old 65535 usable-pin limit.

## Verification

```bash
ninja -C build-1m-mdl mysys_lf-t mdl-t mysqld -j 8
./build-1m-mdl/runtime_output_directory/mysys_lf-t \
  --gtest_filter='Mysys.LFPinboxAllocatesPastOldLimit'
./build-1m-mdl/runtime_output_directory/mdl-t \
  --gtest_filter='MDLTest.AllocatesPastOldPinboxLimit'
```

Additional recommended checks:

```bash
./build-1m-mdl/runtime_output_directory/mysqld \
  --no-defaults \
  --max-connections=1000000 \
  --verbose --help
cd build-1m-mdl/mysql-test && ./mtr 1st
git diff --check
```

See [TestReport.md](TestReport.md) for detailed results and reduced-limit integration reproduction data.

## Review Notes

- The change does not persist pinbox state to disk and does not introduce a data file format change.
- Downgrade only needs the usual configuration check for `max_connections`; this MR adds no new option that must be removed before downgrade.
- The MR removes the MDL pinbox 65535-pin hard ceiling. It does not by itself solve all OS, thread, memory, PFS, Mycat pool or workload-level limits needed for practical 1000000-connection operation.
