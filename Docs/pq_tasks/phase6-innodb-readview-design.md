# Phase 6 - InnoDB/read-view 设计定稿

## Goal

在真正编写 Phase 6 InnoDB PQ 代码前，定稿 V1-MVP 的 InnoDB clustered full scan、read-view、handler 专有 API 和资源释放契约。

本任务是设计任务，默认不改源码。

## Scope

- 明确 leader 如何创建/固定 statement read view。
- 明确 worker 如何使用同一语义的 read view，避免每个 worker 独立 snapshot。
- 明确 `Parallel_reader` 可复用点和必须新增的薄 adapter。
- 明确 `ha_innobase` 专有 PQ API 签名、生命周期、错误码映射。
- 明确 Phase 6B 允许修改的 InnoDB 文件清单。

## Allowed Files

- `Docs/pq_tasks/phase6-innodb-readview-design.md`
- 可引用和阅读 `storage/innobase/**`、`sql/handler.h`、`sql/parallel_query/pq_handler.h`

## Forbidden Files

- `sql/**` 源码修改
- `storage/**` 源码修改
- `mysql-test/**`
- CMake 修改

## Required Questions

必须回答：

- `Parallel_reader::Scan_ctx` 的 partition / visibility 哪些可以直接复用？
- SQL worker pull model 是否需要独立 `PQ_Ctx::read_record()` 游标循环？
- read view 是 clone/copy，还是由 leader scan ctx 统一持有并由 worker 访问？
- `pq_leader_scan_init()` / `pq_worker_scan_init()` / `pq_worker_scan_next()` / `pq_worker_scan_end()` / `pq_leader_scan_end()` 最终签名是什么？
- InnoDB `dberr_t` 如何映射为 handler 返回码？
- worker handler/table/record buffer 的所有权和关闭顺序是什么？
- Phase 6B 最小代码改动清单是什么？

## Claude Code 任务书

### Worktree / 调度信息

```text
Worktree: /Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase6-innodb-design
Branch: claude-phase6-innodb-design
Baseline: c2a2a537b64 Add PQ phase 5 optimizer eligibility hook
Prompt: /private/tmp/phase6-innodb-readview-design-prompt.txt
Log: /Users/zhuqingping/Work/Database/MySQL/mysql-server/Docs/pq_tasks/phase6.claude.log
PID file: /Users/zhuqingping/Work/Database/MySQL/mysql-server/Docs/pq_tasks/phase6.claude.pid
Exit file: /Users/zhuqingping/Work/Database/MySQL/mysql-server/Docs/pq_tasks/phase6.claude.exit
Expected patch: /Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase6-innodb-readview-design.patch
```

查看后台进度：

```bash
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server
screen -ls
ps -p "$(cat Docs/pq_tasks/phase6.claude.pid)" -o pid,ppid,stat,etime,command
tail -f Docs/pq_tasks/phase6.claude.log
git -C /Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase6-innodb-design status --short
```

后台调度命令：

```bash
LOG=/Users/zhuqingping/Work/Database/MySQL/mysql-server/Docs/pq_tasks/phase6.claude.log
PIDFILE=/Users/zhuqingping/Work/Database/MySQL/mysql-server/Docs/pq_tasks/phase6.claude.pid
EXITFILE=/Users/zhuqingping/Work/Database/MySQL/mysql-server/Docs/pq_tasks/phase6.claude.exit
PROMPT=/private/tmp/phase6-innodb-readview-design-prompt.txt
WT=/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase6-innodb-design
SESSION=phase6_pq_claude

: > "$LOG"
rm -f "$EXITFILE"
screen -dmS "$SESSION" /bin/zsh -lc "cd '$WT' && claude -p \"\$(cat '$PROMPT')\" > '$LOG' 2>&1; echo \$? > '$EXITFILE'"
screen -ls | awk '/\.phase6_pq_claude[[:space:]]/ {split($1,a,"."); print a[1]}' > "$PIDFILE"
```

### Agent Prompt

```text
请先阅读 AGENTS.md，并以 CLAUDE.md 为唯一权威上下文。

你的角色是 Design Agent。
主控 Agent 是 Codex。
当前任务是 W2-C / Phase 6：InnoDB/read-view 设计定稿。

本任务默认不改源码，只改 Docs/pq_tasks/phase6-innodb-readview-design.md。

请阅读：
- CLAUDE.md
- Docs/pq_tasks/parallel_wave2_tasks.md
- Docs/pq_tasks/phaseA-interface-contract.md
- Docs/pq_tasks/phase2-handler-innodb-contract.md
- sql/parallel_query/pq_handler.h
- storage/innobase/include/row0pread.h
- storage/innobase/row/row0pread.cc
- storage/innobase/handler/ha_innodb.h
- storage/innobase/handler/ha_innodb.cc

任务目标：
1. 给出 InnoDB/read-view 最终方案；
2. 明确 Parallel_reader 复用点和薄 adapter 缺口；
3. 明确 ha_innobase 专有 PQ API 最终签名；
4. 明确 Phase 6B 允许改哪些文件；
5. 明确资源释放顺序和错误码映射；
6. 把完整设计结论写入 Phase 6 文档。

禁止修改：
- sql/**
- storage/**
- mysql-test/**
- CMake 文件

完成后生成 patch：
git diff > /Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase6-innodb-readview-design.patch
```

## Design Decision Summary

Phase 6 设计结论：V1-MVP 采用 **InnoDB clustered full table scan + SQL worker pull model + leader statement read view**。

核心决策：

- V1-MVP 只支持 InnoDB clustered index full scan，不支持 secondary index、range/ref、partition、ORDER BY、GROUP BY、aggregation pushdown。
- 不 clone/copy `ReadView`。leader 的 InnoDB `trx_t` 创建并持有 statement read view，PQ scan ctx 使用同一事务语义做 visibility check。
- worker 不独立创建 snapshot。如果 worker 不能安全绑定 leader scan ctx/read view，则本查询 fallback serial。
- 复用 upstream `Parallel_reader` 的 thread budget、B+tree partition、visibility 代码路径，但不直接使用其 push-row execution。
- Phase 6B 新增薄 adapter，把 `Parallel_reader::Scan_ctx` / `Ctx` 的 partition + cursor traversal 思路转换为 SQL worker 可逐行调用的 `PQ_Ctx::read_record()`。
- handler API 仍保持 InnoDB 专有，不改通用 `handler.h`。SQL PQ 通过 `ha_innobase`/InnoDB adapter 调用专有接口。

## Read View Ownership

### 最终方案

read view 由 leader 对应的 InnoDB transaction 持有：

```text
leader THD -> ha_innobase::m_prebuilt->trx -> trx_t::read_view
```

leader 在 `pq_leader_scan_init()` 中确保：

1. `trx_t *trx = m_prebuilt->trx` 可用；
2. `trx_start_if_not_started_xa(trx, false, UT_LOCATION_HERE)` 已执行；
3. 对 consistent read，`trx_assign_read_view(trx)` 已执行；
4. `trx->read_view` 在整个 PQ scan 生命周期内保持 active；
5. `pq_leader_scan_end()` 才释放 PQ scan ctx，不主动提前关闭 statement read view。

worker 不创建自己的 InnoDB transaction snapshot，不调用 `trx_assign_read_view()`。worker 通过 `PQ_Ctx` 访问 leader scan ctx 中保存的 `trx_t *` / `ReadView` 语义。visibility check 走与 upstream `Parallel_reader::Scan_ctx::check_visibility()` 等价的逻辑：

```text
rec trx id -> trx->read_view->changes_visible(...) -> old version build / skip
```

### 不 clone/copy ReadView 的原因

- InnoDB `ReadView` 生命周期由 MVCC/trx 系统管理，复制语义不是公共契约。
- clone/copy 会引入 view close、undo visibility、creator trx id 等一致性风险。
- V1-MVP 的目标是 statement snapshot 一致，不是每个 worker 独立 snapshot。

### Fallback

以下情况必须 fallback serial：

- leader `trx_t` 不可用；
- `trx->read_view` 无法创建或不 active；
- worker 需要独立 snapshot 才能继续；
- isolation/locking read 语义不满足 Phase 1/5 eligibility；
- visibility check 需要 secondary index 路径。

## Parallel_reader Reuse And Adapter Gap

### 可直接复用的能力

源码确认：

- `storage/innobase/include/row0pread.h` 定义 `Parallel_reader::available_threads()` / `release_threads()`。
- `Parallel_reader::add_scan()` 创建 `Scan_ctx`，调用 `Scan_ctx::partition()`，再由 `create_contexts()` 生成 work contexts。
- `Parallel_reader::Scan_ctx::check_visibility()` 已实现 clustered index consistent read visibility。
- `Parallel_reader::Scan_ctx::partition()` / `create_ranges()` 已实现 B+tree range 切分。

Phase 6B 应复用：

- thread budget：`Parallel_reader::available_threads()` / `release_threads()`；
- clustered index partition 算法；
- start/end border 表示方式：`Parallel_reader::Scan_range`；
- visibility 规则；
- `DB_*` error code 体系。

### 不能直接复用的部分

upstream `Parallel_reader` 的 execution 是 push-row callback：

```cpp
using F = std::function<dberr_t(const Ctx *)>;
```

worker thread 内部调用 `Ctx::traverse()` / `traverse_recs()`，每读到一行就调用 callback。这不适合 SQL PQ worker：

- SQL worker 需要自己的 THD/JOIN/RowIterator 执行状态；
- 每个 worker 需要把下一行拉到 `table->record[0]`；
- worker 执行 WHERE/filter/project/MQ send 的节奏由 SQL 层控制；
- 不能让 InnoDB 内部线程绕过 SQL worker 生命周期。

### Adapter 缺口

Phase 6B 需要新增 InnoDB PQ adapter，提供 pull-row API：

```text
PQ_Leader_context -> build clustered ranges
PQ_Worker_context -> dispatch range
PQ_Ctx::read_record(record[0], ha_innobase*) -> pull next visible row
```

adapter 不应复制完整 `row0pread.cc`。但由于 `Parallel_reader::Scan_ctx`、`Ctx`、`Range`、`Iter` 多数是 private，Phase 6B 有两种实现路径：

1. **推荐最小侵入路径**：在 `row0pread.h/.cc` 为 PQ adapter 增加受控 public/protected wrapper，暴露 partition result 和 cursor traversal helper。
2. **临时 MVP 路径**：新增 `row0pread_pq.h/.cc`，复用 `Parallel_reader::Config`、`Scan_range`、thread budget、visibility 逻辑，但只复制最小 cursor loop。复制范围必须小于完整 fork，并在 Phase 8 前回收为 shared helper。

优先选择路径 1。如果 private 边界导致实现风险过大，Phase 6B 可以采用路径 2，但必须在代码注释和文档中标记为 temporary adapter。

## SQL Worker Pull Model

结论：需要独立 `PQ_Ctx::read_record()` 游标循环。

原因：

- `Parallel_reader` callback 模型无法表达 SQL worker 每次拉一行、执行 SQL plan step、再发送 MQ 的流程。
- Phase 4 的 worker lifecycle 是 SQL worker model，不是 InnoDB 内部 worker thread model。
- 每个 SQL worker 需要自己的 handler/table/record buffer，不能共享 leader 的 `TABLE::record[0]`。

`PQ_Ctx::read_record()` 的行为：

```text
输入：worker handler + table->record[0]
输出：一行 MySQL record，或 range exhausted，或 fatal error
```

状态机：

```text
INIT -> RESTORE_CURSOR -> READ_VISIBLE_REC -> COPY_TO_MYSQL_RECORD -> ADVANCE
     -> READ_VISIBLE_REC ... -> END_OF_RANGE
```

任何 mtr/latch 必须在单次 read 或单页 traversal 内成对释放，不能跨 SQL 层调用长期持有。

## ha_innobase PQ API Final Signatures

Phase 6B 建议先使用 InnoDB 专有 API，不改 `handler.h`：

```cpp
int ha_innobase::pq_leader_scan_init(
    THD *leader_thd,
    PQ_Leader_context **leader_ctx,
    uint requested_dop,
    bool reverse);

int ha_innobase::pq_worker_scan_init(
    THD *worker_thd,
    PQ_Leader_context *leader_ctx,
    PQ_Worker_context **worker_ctx);

int ha_innobase::pq_worker_scan_next(
    PQ_Worker_context *worker_ctx,
    uchar *record,
    bool *eof);

int ha_innobase::pq_worker_scan_end(
    PQ_Worker_context *worker_ctx);

int ha_innobase::pq_leader_scan_end(
    PQ_Leader_context *leader_ctx);
```

说明：

- `leader_ctx` 持有 ranges、thread budget、scan ctx、error state。
- `worker_ctx` 持有当前 worker 的 range queue/cursor state，不拥有 leader read view。
- `record` 是 worker table 的 `table->record[0]`。
- `eof=true` 表示当前 worker 没有更多 rows，不是 fatal error。
- OOM before worker start 可 fallback serial；worker start 后 fatal error 走 worker/leader abort。

## dberr_t To Handler Error Mapping

建议映射：

| InnoDB error | Handler result | 说明 |
|---|---|---|
| `DB_SUCCESS` | `0` | 成功读到一行 |
| `DB_END_OF_INDEX` / `DB_NOT_FOUND` | `HA_ERR_END_OF_FILE` 或 `eof=true` | InnoDB scan/index 结束 |
| `PQ_DB_END_OF_RANGE` | `eof=true` | PQ adapter 内部 range 结束状态，不作为 InnoDB `dberr_t` 依赖 |
| `DB_OUT_OF_MEMORY` | `HA_ERR_OUT_OF_MEM` | worker 启动前可 fallback；启动后 fatal |
| `DB_INTERRUPTED` | `HA_ERR_QUERY_INTERRUPTED` | KILL / interrupted |
| `DB_LOCK_WAIT_TIMEOUT` | `HA_ERR_LOCK_WAIT_TIMEOUT` | 理论上 consistent full scan 不应常见 |
| `DB_DEADLOCK` | `HA_ERR_LOCK_DEADLOCK` | fatal |
| other `dberr_t` | `HA_ERR_INTERNAL_ERROR` | conservative fatal |

Phase 6B 应提供一个 InnoDB-local helper：

```cpp
static int pq_map_dberr_to_handler_error(dberr_t err, bool *eof);
```

## Worker Handler/Table/Record Buffer Ownership

所有权原则：

- leader owns `PQ_Leader_context`。
- leader context owns scan ranges、thread budget、global error state。
- worker owns `PQ_Worker_context`。
- SQL worker THD owns worker-side TABLE/handler open/close 生命周期。
- `table->record[0]` 属于 worker TABLE，不允许多个 worker 共享。
- `PQ_Ctx` owns cursor/range state，不能持有指向已关闭 TABLE/handler 的裸指针超过 worker scan 生命周期。

建议关闭顺序：

```text
worker normal path:
  pq_worker_scan_next(... eof=true)
  pq_worker_scan_end(worker_ctx)
  close worker handler/table
  worker THD cleanup

leader normal path:
  wait all workers finished
  pq_leader_scan_end(leader_ctx)
  release thread budget/ranges/scan ctx

error path:
  set leader error state
  stop dispatching new ranges
  workers observe error and exit current read loop
  worker_scan_end for each worker
  leader_scan_end releases shared state last
```

## Phase 6B Minimal Source File Plan

允许的最小文件清单：

- `storage/innobase/handler/ha_innodb.h`
  - 声明 InnoDB PQ 专有 API。
- `storage/innobase/handler/ha_innodb.cc`
  - 实现 API glue、trx/read view init、error mapping。
- `storage/innobase/include/row0pread.h`
  - 如选择最小侵入路径，暴露 PQ adapter 需要的 wrapper/helper。
- `storage/innobase/row/row0pread.cc`
  - 实现 wrapper/helper 或复用 partition/visibility helper。
- `storage/innobase/include/row0pread_pq.h`
  - 可选：定义 `InnoDB_pq_scan_ctx` / `InnoDB_pq_ctx`。
- `storage/innobase/row/row0pread_pq.cc`
  - 可选：实现 pull-row adapter。
- `storage/innobase/CMakeLists.txt`
  - 仅当新增 `.cc` 时修改。
- `sql/parallel_query/pq_handler.h/.cc`
  - 仅允许做接口与 Phase 6B adapter 对齐的小改动。

禁止在 Phase 6B 做：

- 修改通用 `handler.h`；
- 接 optimizer plan rewrite；
- 支持 secondary index/range/ref；
- 支持 aggregation/order/group/window；
- 引入 MTR 大测试套。

## Deferred Items And Fallback Rules

Deferred：

- secondary index PQ；
- range/ref PQ；
- partitioned table PQ；
- aggregation parallel；
- ORDER BY / Gather Merge；
- EXPLAIN reason storage；
- full test-suite migration。

Fallback：

- read view 不 active -> serial；
- worker 需要独立 snapshot -> serial；
- non-clustered access -> serial；
- range/ref/index access -> serial；
- private `Parallel_reader` helper 无法安全复用 -> Phase 6B 只实现 temporary minimal adapter，并记录后续重构。

## Design Agent Completion Report

### Codex Orchestrator Fallback - 2026-06-02

Claude Code Phase 6 后台和前台调度多次无输出，主控 Agent 中断空转进程后完成本设计草案。保留的关键日志：

- `Docs/pq_tasks/phase6.claude.log`
- `Docs/pq_tasks/phase6.short.claude.log`
- `Docs/pq_tasks/phase6.bypass.claude.log`

Changed files:

- `Docs/pq_tasks/phase6-innodb-readview-design.md`

Validation:

- 已通过 `rg` 和源码阅读核对 `Parallel_reader::available_threads()`、`release_threads()`、`add_scan()`、`Scan_ctx::partition()`、`Scan_ctx::check_visibility()`、`trx_assign_read_view()` 等符号/路径存在。
- 本任务不改源码，不需要构建。

Risks:

- `Parallel_reader::Scan_ctx` / `Ctx` 的核心能力当前多为 private，Phase 6B 需要在最小 wrapper 与 temporary adapter 之间做实现权衡。
- read view 共享必须严格限制在 leader scan ctx 生命周期内；worker 不得独立创建 snapshot。

## Acceptance Checklist

- [x] read-view 方案明确。
- [x] handler API 签名明确。
- [x] Parallel_reader 复用/缺口明确。
- [x] Phase 6B 文件清单明确。
- [x] 不改源码。

## Current Status

已完成 Codex 主控 final review。Phase 6 作为设计定稿文档完成，后续 Phase 5B/6B 以本文档约束为准。
