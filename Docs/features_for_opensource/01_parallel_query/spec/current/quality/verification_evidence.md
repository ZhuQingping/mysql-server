# Parallel Query Verification Evidence

> 文档 ID：`PQ-QUALITY-VERIFY-001`
> 状态：`partial-runtime-evidence`
> 稳定对照基线：`1b9ffd755d4` (`stable_branch`)
> 当前范围：`pq-local-refactor-clean` 上的 THD / Query_block / JOIN PQ context 重构
> 记录日期：2026-07-21

## 1. 证据使用边界

本文件区分历史稳定基线证据与本次 context 重构的针对性证据。每条记录只证明列出的
构建配置和测试集合；它不把尚未定向断言的 SQL 组合、故障路径或性能/并发性质自动升级为
`verified`。模块中的 Requirement 映射仍必须保留各自的 oracle、缺口和后续门禁。

## 2. 历史稳定基线证据

| 类别 | 配置/命令 | 结果 | 边界 |
|---|---|---|---|
| Release build | `ninja -C build-ninja-release` | 历史通过记录 | 编译与链接成功；不代表运行期测试。 |
| Debug build | `ninja -C build-ninja-debug` | 历史通过记录 | 编译与链接成功；不代表每个 DBUG 注入点已执行。 |
| ASAN build | `ninja -C build-ninja-asan` | 历史通过记录 | 编译与链接成功；不代表 ASAN 运行期压力覆盖。 |
| PQ MTR | `cd build-ninja-debug/mysql-test && ./mysql-test-run.pl --force --pq --suite=main,parallel_query --parallel=4` | 历史记录：878 tests successful；698 skipped，其中 616 由测试自身跳过。 | 该记录早于当前 `1b9ffd755d4`，不能作为本次重构的完整矩阵结论。 |

MTR 在 macOS `lower_case_table_names=2` 环境完成。`stable_branch` 中的 result 基线已包含
该环境下 PQ EXPLAIN/optimizer trace 的可移植性调整；这只是结果展示基线，不改变 PQ 的
SQL 执行语义。

## 3. Context 重构已完成的针对性证据

| 类别 | 配置/命令 | 结果 | 结论边界 |
|---|---|---|---|
| Release 编译 | `cmake --build build-ninja-release --target mysqld -j 8` | 通过 | 只验证本次变更可在 macOS arm64 AppleClang Release 配置编译和链接。 |
| Debug 编译 | `cmake --build build-ninja-debug --target mysqld -j 8` | 通过 | 验证同一源码可在 macOS arm64 AppleClang Debug 配置编译和链接。 |
| worker-to-leader 错误信号单元测试 | `cmake --build build-ninja-unit --target pq_context-t -j 8 && ./build-ninja-unit/runtime_output_directory/pq_context-t` | 1 个 GoogleTest 通过。worker 调用 `set_error()` 后 leader 可观察到 `has_error()`，worker join 后 `clear_error_after_workers_join()` 复位。 | 直接验证 context 的原子 API 和 statement 边界复位合同；不替代 TSAN 或完整并发压力验证。 |
| 原始字段遗留审计 | `rg -n 'pq_context\\(\\)\\.error\|m_pq_context\\.error' sql storage/temptable --glob '*.{cc,h}'` | 无匹配。 | 所有 PQ context 错误标志访问均已收敛到 `has_error()` / `set_error()` / `clear_error_after_workers_join()`。 |
| MQ 热路径静态审计 | `nm -nm build-ninja-release/runtime_output_directory/mysqld \| c++filt \| rg 'PQ_thd_context::(has_error\|set_error\|clear_error_after_workers_join\|is_error)\|MQueue_handle::(send_bytes\|receive_bytes)'` | 仅保留 `MQueue_handle::{send_bytes,receive_bytes}` 符号；不存在 PQ context error API wrapper。 | 配合 `THD::is_pq_error()` 头内联和源级调用点审阅，证明 MQ polling 路径未新增跨编译单元调用；不替代工作负载性能数据。 |
| Debug PQ 生命周期回归 | `./mtr --force --max-test-fail=0 --retry=0 --parallel=8 --mtr-port-base=25000 --pq parallel_query.pq_clone_item parallel_query.refactor_fix_fields parallel_query.pq_fallback parallel_query.pq_record_buffer parallel_query.pq_read_view parallel_query.pq_kill parallel_query.pq_kill_query parallel_query.pq_worker_error parallel_query.pq_mq_error parallel_query.pq_sp_trigger` | 10 个指定用例全部通过。 | 覆盖 context 生命周期、clone/restore、fallback、read view、KILL 和 MQ/worker 错误路径；不等价于全量矩阵。 |
| Debug PQ error-path 回归（本次原子化后） | `./mtr --force --max-test-fail=0 --retry=0 --parallel=4 --mtr-port-base=26000 --pq parallel_query.pq_worker_error parallel_query.pq_mq_error parallel_query.pq_kill parallel_query.pq_kill_query parallel_query.pq_fallback` | 5 个指定用例及 `shutdown_report` 全部通过。 | 覆盖此次变更直接涉及的 worker/MQ error、KILL 与 fallback 路径；不等价于全量矩阵。 |
| 隔离检查 | `git merge-base --is-ancestor 1b9ffd755d4 HEAD`；`git diff --check 1b9ffd755d4..HEAD` | 通过。 | 变更相对稳定基线收口；路径白名单在最终提交前再次核验。 |

## 4. 仍需单独取证的范围

- Prepared Statement binary protocol、reprepare、SP、stored function 和 trigger 的完整
  拒绝矩阵；当前全套成功不替代组合级 reason/oracle。
- cursor restore、purge、page split/merge、正反向扫描和 record buffer 的并发差分；
  `stable_branch` 不含 restore-window DEBUG SYNC 专用用例。
- Release、Debug、ASAN 三种构建的完整 `--suite=main` 与
  `--pq --suite=main,parallel_query` 矩阵；结果须与 `stable_branch` 同条件对照。
- 同机同负载的 stable/refactor 性能与内存对照：PQ 吞吐、P95/P99、短查询并发和每连接
  常驻内存。当前结论是 `pending-validation`，不得写成“性能无影响”。
- XA、READ UNCOMMITTED、完整事务隔离组合、DML/binlog exact-once、PTRC、资源泄漏与
  KILL/partial launch 的故障注入矩阵。

## 5. 维护规则

1. 任何 `verified` 结论必须引用目标 commit、构建类型、完整命令和可复现结果。
2. 新的失败、skip 原因变化或 result 基线变化必须更新本文件和受影响模块的 traceability。
3. 不提交本地 build 目录、MTR `var/`、原始日志或临时诊断文件；本文件只保留可审查摘要。
