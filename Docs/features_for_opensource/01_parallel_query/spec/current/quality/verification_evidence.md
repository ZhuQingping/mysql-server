# Parallel Query Stable Branch Verification Evidence

> 文档 ID：`PQ-QUALITY-VERIFY-001`
> 状态：`commit-bound-runtime-evidence`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 记录日期：2026-07-20

## 1. 证据使用边界

本文件记录已经在 `stable_branch` 上完成的构建和 MTR 门禁。它只证明命令所覆盖的
构建配置和测试集合在该提交上通过；它不把尚未定向断言的 SQL 组合、故障路径或性能/并发
性质自动升级为 `verified`。模块中的 Requirement 映射仍必须保留各自的 oracle、缺口和
后续门禁。

## 2. 已完成门禁

| 类别 | 配置/命令 | 结果 | 边界 |
|---|---|---|---|
| Release build | `ninja -C build-ninja-release` | 通过 | 编译与链接成功；不代表运行期测试。 |
| Debug build | `ninja -C build-ninja-debug` | 通过 | 编译与链接成功；不代表每个 DBUG 注入点已执行。 |
| ASAN build | `ninja -C build-ninja-asan` | 通过 | 编译与链接成功；不代表 ASAN 运行期压力覆盖。 |
| PQ MTR | `cd build-ninja-debug/mysql-test && ./mysql-test-run.pl --force --pq --suite=main,parallel_query --parallel=4` | 878 tests successful；698 skipped，其中 616 由测试自身跳过。 | 覆盖 `--pq` 下的 `main` 和 `parallel_query` 集合；跳过项、平台条件和未定向的组合场景不因此成为已验证能力。 |

MTR 在 macOS `lower_case_table_names=2` 环境完成。`stable_branch` 中的 result 基线已包含
该环境下 PQ EXPLAIN/optimizer trace 的可移植性调整；这只是结果展示基线，不改变 PQ 的
SQL 执行语义。

## 3. 仍需单独取证的范围

- Prepared Statement binary protocol、reprepare、SP、stored function 和 trigger 的完整
  拒绝矩阵；当前全套成功不替代组合级 reason/oracle。
- cursor restore、purge、page split/merge、正反向扫描和 record buffer 的并发差分；
  `stable_branch` 不含 restore-window DEBUG SYNC 专用用例。
- XA、READ UNCOMMITTED、完整事务隔离组合、DML/binlog exact-once、PTRC、资源泄漏与
  KILL/partial launch 的故障注入矩阵。

## 4. 维护规则

1. 任何 `verified` 结论必须引用目标 commit、构建类型、完整命令和可复现结果。
2. 新的失败、skip 原因变化或 result 基线变化必须更新本文件和受影响模块的 traceability。
3. 不提交本地 build 目录、MTR `var/`、原始日志或临时诊断文件；本文件只保留可审查摘要。
