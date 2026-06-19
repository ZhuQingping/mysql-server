# M5 InnoDB Commercial PQ Path Alignment Taskbook

## 状态

Completed。

## 目标

迁移商用 `ha_innodb_pq.cc` 形态或等价适配层，收敛当前 InnoDB PQ 逻辑与商用实现差异，同时保留 MySQL 8.0.46 已验证的 read-view / KILL / cleanup 语义。

本阶段拆成三个提交：

- M5a: handler contract alignment；
- M5b: `ha_innodb_pq.cc` file split，行为等价；
- M5c: clustered full-scan row producer alignment。

## 允许修改

- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/handler/ha_innodb.cc`
- `sql/handler.h`（仅当 M5a 必须补齐已有 handler PQ contract 的声明边界时允许；不得打开 ref/ICP）
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/CMakeLists.txt`
- `Docs/pq_tasks/commercial-port-m5-innodb-path-alignment.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

## 设计要求

- 保留当前 `PROBE` fallback-safe / `EXECUTE` commit-point 语义；
- M5a 优先对齐当前已有 SQL handler PQ contract 与 InnoDB 侧实现；如需改 `sql/handler.h`，只能补声明/字段边界，ref/ICP 仍留给 M9；
- 不强搬商用 `uint keyno, void *scan_ctx` 签名覆盖当前安全桥接模型；
- `PROBE` 不创建不可回退状态，`EXECUTE` 才是 commit point；
- clustered full scan 先完成；
- secondary index、ICP、partition 不在 M5 打开；
- M6 前只能有一个真实 row source：当前 callback producer 或商用 pull-row，不能双轨执行；
- 不破坏当前 read-view concurrency 和 KILL cleanup 测试。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_dop2_read_view pq_read_threaded_dop2_external_kill pq_read_threaded_dop4_external_kill pq_read_threaded_mdl_concurrency pq_locking_read_fallback --parallel=1 --vardir=/tmp/pqv_m5 --tmpdir=/tmp/pqt_m5
```

## Completion Report

### 2026-06-19 Codex Orchestrator

本阶段完成 M5b 文件形态对齐，并顺带收敛 leader init 的 read-view 失败清理风险。

Changed files:

- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/handler/ha_innodb.cc`
- `storage/innobase/CMakeLists.txt`

实现说明:

- 新增 `handler/ha_innodb_pq.cc`，把当前 InnoDB PQ handler 方法从 `ha_innodb.cc` 拆出到独立编译单元，贴近商用实现的文件形态；
- 保留当前 MySQL 8.0.46 安全桥接签名，不切换到商用旧 `uint keyno, void *scan_ctx` 签名；
- 未打开 secondary index、ICP、partition、ref path；
- `PROBE` 仍可为 smoke 创建临时 leader context/read view，但该状态由 `pq_leader_scan_end()` 统一回收；`EXECUTE` 才设置 `m_prebuilt->sql_stat_start=false`；
- 调整 `pq_leader_scan_init()` 失败出口，确保 read view、thread budget、leader ctx 在 init/sql leader 分配失败时释放。

验证:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_dop2_read_view pq_read_threaded_dop2_external_kill pq_read_threaded_dop4_external_kill pq_read_threaded_mdl_concurrency pq_locking_read_fallback --parallel=1 --vardir=/tmp/pqv_m5 --tmpdir=/tmp/pqt_m5
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=4 --vardir=/tmp/pqv_m5_full --tmpdir=/tmp/pqt_m5_full
```

结果:

- `mysqld` build 通过；
- M5 指定测试 6 项成功；
- 完整 `parallel_query` suite 71 项成功。

Review:

- Review Agent 首轮指出 PROBE/read-view cleanup 和新文件未跟踪风险；
- 已补齐失败出口 cleanup，并在提交时显式纳入 `ha_innodb_pq.cc`；
- 复核后无 blocking lifecycle/resource leak 问题，剩余风险为 PROBE 临时 read view 依赖调用方正确调用 `pq_leader_scan_end()`，当前 SQL iterator 已覆盖该清理路径。
