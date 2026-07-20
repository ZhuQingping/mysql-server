# 05 InnoDB Range、Cursor 与 MVCC 契约

- 模块 ID：`PQ-MOD-005`
- 文档状态：`current-contract`，代码事实以当前 `stable_branch` 为准
- 代码基线：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
- 基线范围：manifest 中的 commit、`source_tree_sha256` 与 `test_tree_sha256` 仅覆盖其定义的 PQ source/test file set。
- 静态复核日期：2026-07-20
- 运行时验证：见 `current/quality/verification_evidence.md`；不把 suite 成功解释为 cursor 并发组合已全部证明
- 上游契约：`../00_architecture_overview.md`、`../01_current_support_matrix.md`

## 1. 职责与非职责

### 1.1 职责

本模块描述 InnoDB 作为 Parallel Query（PQ）DIV/CUT 扫描提供方时的当前契约：

- 把索引 B+Tree 的逻辑扫描区间切成可并发消费的半开区间 `[first, second)`；
- 在 leader 初始化阶段完成一级分片，在 worker 取任务时按需进行二级分片；
- 为 full/index/range/ref、正向/反向、聚簇/二级索引扫描建立持久游标；
- 在恢复游标后维持“当前记录尚未处理”与“当前记录已经处理”两种位置语义；
- 使用 leader 事务上下文判定 DIV/CUT 扫描记录的可见性，并在需要时构造旧版本；
- 在二级索引路径中组合 ICP、二级页可见性信息、聚簇回表和 delete-mark 过滤；
- 为普通 worker 执行计划中的非 DIV/CUT InnoDB 访问分配 worker 自己的事务/read view，并从 leader snapshot 克隆逻辑快照；
- 在 leader、worker、scan context、slice、cursor 与 record buffer 之间建立所有权和清理边界。

### 1.2 非职责

本模块不负责：

- 选择是否执行 PQ、选择 DIV_TAB/CUT_TAB 或计算 DOP；
- clone SQL 执行计划、启动线程或汇总结果；
- 定义 MQ 行协议；
- 证明 purge 与跨线程 read view 生命周期在所有时序下安全；当前代码只提供静态线索，该问题保留为质量风险；
- 跨多个 used partition 的并行扫描；当前入口只接受一个 used partition；
- 把 worker 的普通 InnoDB read view 与 DIV/CUT scan context 共享为同一个对象；当前实现明确不是这一模型。

## 2. 入口、输入与输出

### 2.1 入口

| 层次 | 入口 | 作用 |
|---|---|---|
| SQL/handler | `Gather_operator::init()` / `InitPQTab()` | 调用 `handler::ha_pq_init()` 建立 leader 扫描上下文 |
| InnoDB leader | `ha_innobase::pq_leader_scan_init()` | 启动事务、分配 leader read view，并分派 index/range/ref 初始化 |
| InnoDB partition | `ha_innopart::pq_leader_scan_init()` | 取得首个 used partition，并在该 partition 内初始化扫描 |
| InnoDB worker | `ha_innobase::pq_worker_scan_init()` | 创建 worker 侧 `Parallel_worker`，引用 leader 的共享切片管理器 |
| 取任务 | `Parallel_worker::get_ctx()` | 从全局/本地队列获取或细分 range，生成 worker `PQ_Ctx` |
| 读记录 | `ha_innobase::pq_worker_scan_next()` → `PQ_Ctx::read_record()` | 恢复/推进游标，做可见性、ICP、回表、转换及 record buffer 处理 |
| 快照保障 | `pq_create_innodb_snapshot()` | gather 初始化之后确保 leader 对普通 InnoDB 访问具有可克隆 snapshot |
| worker 快照 | `pq_clone_innodb_snapshot()` | 在 worker 自己的事务上创建/克隆普通 InnoDB 访问所需 read view |

### 2.2 输入

- leader `THD`、`trx_t`、目标 `TABLE/handler`、索引号、DOP；
- full/index scan 的负无穷到正无穷边界，或 range/ref 的 key tuple 和查找标志；
- 正向/反向标志、ICP 条件、record buffer 配置；
- dependent ref 的运行时 key；
- 分区表的 used-partition bitmap；
- leader 已建立或可建立的事务一致性读上下文。

### 2.3 输出

- leader 持有的 `Parallel_leader` 和一组 `shared_ptr<PQ_Scan_ctx_Base>`；
- `PQ_slices_mngr` 中可被多个 worker 领取的逻辑 ranges；
- 每个 worker 当前使用的 `PQ_Ctx`、持久游标和 MySQL record；
- 正常记录、`HA_ERR_END_OF_FILE`，或传播到 leader 的首个 handler 错误；
- 对普通 worker-plan InnoDB 访问而言，是 worker 事务中独立分配的 read view；对 DIV/CUT 游标而言，可见性仍读取 scan context 借用的 leader `trx_t`。

## 3. 主流程

### 3.1 leader 初始化与 snapshot 时序

当前时序必须按以下三类动作区分，不能把它们统称为“worker 共享 leader read view”：

1. `ParallelScanIterator::Init()` 先执行 `pq_init_record_gather()`；其中 `m_gather->init()` 进入 handler/InnoDB DIV/CUT 初始化。
2. `ha_innobase::pq_leader_scan_init()` 或 `ha_innopart::pq_leader_scan_init()` 启动/登记 leader 事务，并调用 `trx_assign_read_view(trx)`。随后建立 `Parallel_leader`、`PQ_Scan_ctx` 和初始 ranges。`PQ_Scan_ctx::m_trx` 保存的是 leader `trx_t` 的借用指针。
3. gather 初始化返回后，`ParallelScanIterator::Init()` 调用 `pq_create_innodb_snapshot(leader_thd)`。这是 leader 侧的补充保障：如果执行计划里还有未由 DIV/CUT 初始化触发 snapshot 的普通 InnoDB 访问，leader 此时必须具备可供 worker 克隆的 snapshot。
4. 每个 worker 在 `pq_worker_exec()` 中检查其 worker plan 是否含任意普通 InnoDB base table（InnoDB 且不是内部临时表）；仅在此条件成立时调用 `pq_clone_innodb_snapshot(worker_thd, leader_thd)`。该函数取得 worker 自己的 `trx_t`，通过 `trx_clone_read_view()` 在 worker 事务中分配 read view。它与 leader read view 逻辑上表示同一 snapshot，但不是同一个对象，也不把 leader `ReadView *` 直接挂到 worker 事务上。
5. 第 4 步的 clone 只供 worker plan 中的普通 InnoDB iterator 使用。DIV/CUT 路径的 `PQ_Ctx::read_record()` 经 `PQ_Scan_ctx::m_trx` 做可见性判断，仍由 leader scan context 的可见性处理；它不改用 worker read view。

InnoDB server read-only mode（`srv_read_only_mode`，即 `innodb_read_only`）是显式分支：`pq_create_innodb_snapshot()` 和 `pq_clone_innodb_snapshot()` 可令 read view 为空；`find_visible_record()` 对该 no-view 路径有专门处理。普通 `START TRANSACTION READ ONLY` 不可由此分支推导为已支持，当前状态为 `unknown`。文档和审计不得把空 read view 自动解释为错误。

### 3.2 一级 B+Tree 切分

1. `ha_innobase::pq_index_scan_init()`、`ha_innobase::pq_range_scan_init()` 或 `ha_innobase::pq_ref_scan_init()` 把扫描条件转换为 `PQ_Borders`。空 start/end 分别表示负无穷/正无穷；range 使用前闭后开语义承接相邻边界。
2. `Parallel_leader_Base::build_ranges()` 调用引擎 `make_pq_scan_ctx()`，并把返回的 `shared_ptr` 放入 leader 的 `m_scan_ctxs`，延长 scan context 生命周期。
3. 在 index S latch 保护下，`PQ_Scan_ctx::partition(level=0)` 遍历 B+Tree，构造第一级 `PQ_Range` 列表。
4. 每个 range 由两个 `Iter`/边界描述。相邻 range 复用逻辑边界，最后一个 range 保留原始 end，确保并集覆盖原区间且不重复越界。
5. ranges 进入共享 slice 管理器，更新深度和 slice 数量，然后释放 index S latch。

### 3.3 二级切分与任务领取

1. worker 优先从全局队列领取 range；无现成任务时，可对已领取 range 调用 `PQ_Range::split()`。
2. `split()` 再次持有 index S latch，并执行 `partition(level=1)`，把较大的一级区间细分。
3. 有序扫描使用 `m_spliting_id` 串行化 split 发布顺序；未轮到的线程以 20 微秒间隔等待。该机制保护逻辑顺序，不等于线程公平性保证。
4. worker 为子 range 创建 `PQ_Ctx`，放入 worker 本地双队列；后续 read round 通过队列轮换和 `reset_for_next_read_round()` 复用游标模板。
5. dependent ref 在运行时 key 变化时重建/切换上下文；leader 用 `pq_key_map` 互斥去重同一个 ref key 的 range 构建。

### 3.4 ref 与 dependent ref 边界

- 常量 ref 可在 leader 初始化期直接构造 exact-key range。
- dependent ref 到 worker 执行时才获得 key；`PQRefIterator::Read()` 调用 `ha_innobase::pq_ref_build_ranges()`，在共享 key map 中查找或创建对应切片。
- leader 为确定真实边界临时关闭 ICP，避免在边界定位阶段因 worker 条件产生假阴性；边界确定后正常扫描仍可执行 ICP。
- ref key 被复制到 PQ 配置拥有的缓冲区，不能借用调用者易失的 key buffer。
- key 变化后 worker 必须清理旧 ctx 关联状态并使用匹配新 key 的 range；不得把旧 key 的游标继续推进。

### 3.5 持久游标定位、恢复与推进

1. `Iter` 持有复制后的 tuple、heap 和 persistent cursor；`PQ_Ctx` 从 scan context 的模板游标创建 worker 私有游标状态。
2. 当前分支的 `PQ_PCursor::restore_position()` 直接调用 `btr_pcur_t::restore_position()`，并按保存的 `m_rel_pos` 和 equality 结果调整位置；它不是 `sel_restore_position_for_mysql()` 包装。
3. `PQ_Ctx::read_record()` 首次 restore 后读取当前候选记录；已消费记录跨 mtr restart 时，`PQ_PCursor::yield()` restore 后按正/反向显式移动一条。该边界的“不重不漏”是当前契约，但尚无 restore-window 专项注入覆盖。
4. range end 必须结束当前 context，不得跨入相邻 worker range；infimum/supremum、purge 后 predecessor/successor 和反向页边界均属于高风险审计点。
5. record buffer 有可用行时先出队；buffer 填充跨越 range end 时必须记录结束状态，防止下一次调用继续读取越界行。
6. 每轮在需要的位置保存 pcur、提交 mtr；二级回表导致 latch 变化时，代码提交并重新开始 mtr，再按当前 `restore_position()` 和扫描方向恢复。

### 3.6 MVCC、二级索引与 ICP

`PQ_Scan_ctx::find_visible_record()` 的当前决策链是：

1. 聚簇索引记录：读取记录事务 ID；若 leader snapshot 不可见，则构造可见旧版本；找不到可见版本或 delete-mark 时过滤。
2. 二级索引记录：先执行 ICP，以便尽早丢弃明显不匹配的二级记录。
3. 若 read view 不能凭二级页 `PAGE_MAX_TRX_ID` 证明可见，或者查询不是覆盖索引，则查询聚簇记录并按 leader snapshot 做最终可见性判断。
4. 回表结果为 delete-mark、无可见版本或 ICP 不匹配时，该候选不输出。
5. 可见记录经 `row_sel_store_mysql_rec()` 等路径转换到 MySQL record；转换或引擎错误进入统一错误传播。

因此“支持二级索引”是当前代码事实；源码中个别历史注释仍写着不支持二级索引，不能据此覆盖实际分支。该注释漂移列入质量风险。

## 4. 所有权与生命周期

| 对象/资源 | 创建者 | 所有者 | 借用者 | 生命周期终点/清理 |
|---|---|---|---|---|
| `Parallel_leader` | `ha_innobase::pq_index_scan_init()`、`ha_innobase::pq_range_scan_init()` 或 `ha_innobase::pq_ref_scan_init()` | leader handler/PQ ctx | gather、worker init | `ha_innobase::pq_leader_scan_end()` 删除 |
| `PQ_Scan_ctx` | 引擎 `make_pq_scan_ctx()` | `shared_ptr`；leader `m_scan_ctxs` 保活 | range、worker `PQ_Ctx` 以 raw pointer 访问 | leader 清空 `m_scan_ctxs` 后，且无其他 `shared_ptr` 时销毁 |
| leader `trx_t` | InnoDB/leader THD | leader THD/InnoDB | `PQ_Scan_ctx::m_trx` 借用 | 语句/事务清理；PQ 必须先结束所有借用者 |
| leader read view | `trx_assign_read_view()` | leader `trx_t` | DIV/CUT 可见性、worker clone 源 | 由 leader 事务生命周期管理；跨线程保活/purge 仍需专项证明 |
| worker read view | `pq_clone_innodb_snapshot()` | worker `trx_t` | worker 普通 InnoDB iterator | worker 事务/THD 清理；不是 DIV/CUT ctx 的所有者 |
| `PQ_Range` | scan context partition | `shared_ptr<PQ_Range>` | slice、worker ctx | 所有 range 引用释放后销毁 |
| `Iter` 边界 | `PQ_Scan_ctx::create_range()` | range/config | `PQ_Ctx` 只读使用 | range/config 销毁 |
| worker `PQ_Ctx` | `PQ_Scan_ctx::create_context()` | worker 本地队列/当前 ctx | handler next | worker scan end 清理本地状态 |
| pcur、heap、blob heap | `PQ_Ctx`/`Iter` | 对应 ctx/iter | InnoDB row read | ctx/iter 销毁或 reset |
| copied ref key | dependent-ref 配置 | `PQ_Config_Base` | range 查找 | 配置销毁时释放 |
| index S latch | partition/split 调用者 | InnoDB latch subsystem | 当前切分线程 | 每次 partition 退出前释放；计数辅助审计 |

关键生命周期关系：`PQ_Ctx` 内部保存 scan context raw pointer，因此 leader 的 `m_scan_ctxs` 必须覆盖所有 worker ctx；worker 退出并清空本地队列之前，leader 不得先销毁 scan contexts 或 leader `trx_t`。

## 5. 并发、锁与状态机

### 5.1 并发角色

- leader：创建 read view、scan contexts 和一级 ranges；持有全局生命周期根。
- worker：并发领取 range、可选二级 split、读记录，并把首错写入 leader 错误状态。
- InnoDB purge/DML：与 consistent read 并发，正确性依赖 leader snapshot 及其生命周期。

### 5.2 同步点

| 状态 | 同步原语 | 保护内容 |
|---|---|---|
| B+Tree partition/split | index S latch | 切分期间 B+Tree 结构与边界读取 |
| 全局 slice 分配 | atomic slice id、mutex、condition variable | range 发布、领取、完成计数 |
| ordered split | atomic `m_spliting_id` + 等待 | 二级切分按逻辑 range id 发布 |
| dependent ref key map | `pq_key_map` mutex | 同 key 的 manager/scan context 唯一创建 |
| 首错 | atomic error / compare-and-set 语义 | 保留第一个可报告错误 |
| worker 本地队列 | worker 私有 | 当前/standby ctx 轮转，无跨 worker 共享写 |

### 5.3 游标状态

```text
UNPOSITIONED
  -> POSITION_AT_START
  -> PENDING_RECORD | PROCESSED_RECORD
  -> VISIBLE_CHECK
  -> OUTPUT_OR_SKIP
  -> SAVE_POSITION
  -> NEXT/PREV
  -> RANGE_END
```

恢复后必须保留 pending/processed 区别；否则会造成首行丢失或重复。二级回表、mtr 重启和 record buffer 都不得绕过该状态语义。

## 6. 规范性不变量

- `PQ-INNODB-INV-001`：每个逻辑 range **必须**是半开区间 `[start, end)`；相邻 ranges **不得**重复输出边界记录，也**不得**留下未覆盖间隙。
- `PQ-INNODB-INV-002`：一级/二级切分读取 B+Tree 结构时**必须**持有 index S latch，并在所有返回路径释放。
- `PQ-INNODB-INV-003`：`PQ_Ctx` 借用的 `PQ_Scan_ctx` 和 leader `trx_t` **必须**存活到该 ctx 最后一次 read/end 之后。
- `PQ-INNODB-INV-004`：DIV/CUT 记录可见性**必须**基于 scan context 中的 leader 事务 snapshot；不得静默切换为各 worker 独立生成的较新 snapshot。
- `PQ-INNODB-INV-005`：worker 普通 InnoDB 访问需要 read view 时，**必须**在 worker 自己的 `trx_t` 上分配/克隆；不得把 leader `ReadView *` 作为共享所有权对象直接安装到 worker。
- `PQ-INNODB-INV-006`：leader snapshot creation 与 worker read-view allocation **必须**被视为两个不同生命周期事件；日志、文档和修复不得混淆。
- `PQ-INNODB-INV-007`：恢复 pcur 后，pending 记录**必须**在推进前处理；processed 记录**必须**在再次输出前推进。
- `PQ-INNODB-INV-008`：二级索引候选在无法仅凭二级页证明 snapshot 可见或需要非覆盖列时，**必须**回到聚簇记录做最终可见性判定。
- `PQ-INNODB-INV-009`：ICP 只可过滤当前候选；dependent-ref 边界定位阶段**不得**因 ICP 假阴性缩窄 exact-key range。
- `PQ-INNODB-INV-010`：range end、delete-mark、不可见旧版本和 `HA_ERR_KEY_NOT_FOUND` 等控制结果**必须**与真正 handler 错误区分；只有错误才写入首错状态。
- `PQ-INNODB-INV-011`：分区表仅在 `num_partitions_used() == 1` 时进入该路径；worker **不得**越过已选 partition。
- `PQ-INNODB-INV-012`：reverse scan **必须**同时反转游标推进和边界解释，而不是只反转其中一项。
- `PQ-INNODB-INV-013`：record buffer 中已经物化的记录**必须**遵守同一 range end 与 snapshot；buffer refill **不得**偷读相邻 range。
- `PQ-INNODB-INV-014`：首个非控制类 InnoDB 错误**必须**可被 leader 观察，后续 worker **不应**覆盖更早的根因。

## 7. 错误、回退、重试与清理

| 场景 | 当前传播 | 回退/重试 | 必需清理 |
|---|---|---|---|
| leader scan context/range 构造失败 | `ha_pq_init()` 返回 handler 错误，PQ 初始化失败 | 由上层 graceful fallback/retry 策略决定；引擎自身不重试切分 | 释放已建 scan contexts、latch、range/key buffer |
| leader read view 分配失败 | leader THD/事务错误，经 gather 初始化传播 | 仅初始化阶段可能由上层串行回退 | 不得启动依赖该 snapshot 的 worker |
| worker read view clone 失败 | `pq_clone_innodb_snapshot()` 令 worker 失败，leader `pq_error` | 上层可能整句串行重试，不能在原 worker 内换 snapshot 继续 | worker trx/THD、MQ、所有已启动 worker 均结束 |
| partition/split 错误 | range errno/leader atomic error | 不在同一 ctx 内盲目重切 | 释放 index S latch，唤醒等待者 |
| range 正常结束 | `HA_ERR_END_OF_FILE`/range-end 控制结果 | worker 领取下一 ctx | 保存/关闭当前 pcur，不能写首错 |
| key 未找到/不可见/delete-mark | 跳过候选或结束 exact range | 继续当前 range | 正确提交 mtr、释放回表 latch |
| 记录转换/回表/IO 错误 | `pq_worker_scan_next()` 首错上报并转换 MySQL handler code | 当前并行执行终止；是否全句重试由上层控制 | 当前 ctx、record buffer、blob heap、worker scan end |
| KILL/leader 提前结束 | worker 观察 THD/PQ 错误并停止 | 不恢复原 PQ 流 | leader 必须等待 worker，之后销毁 scan contexts/trx 借用 |
| dependent ref key 构造失败 | 当前 worker/leader error | 不得复用半初始化 key manager | 释放 copied key 和未发布 ranges |

清理顺序的规范方向是：停止/唤醒 worker → worker `ha_innobase::pq_worker_scan_end()` 清本地 ctx → join worker → leader `ha_innobase::pq_leader_scan_end()` 删除 `Parallel_leader` → 事务/THD 清理。静态代码存在这一路径，但异常时序完整性仍需 fault-injection 运行证明。

## 8. 支持范围、限制与开关

| 能力 | 行为状态 | 证据 | 条件/限制 |
|---|---|---|---|
| InnoDB full/table scan | implemented | static-evidence | 表和 query block 通过 PQ 资格检查 |
| index scan | implemented | static-evidence | 支持正向/反向；边界按索引顺序解释 |
| range scan | implemented | static-evidence | `INDEX_RANGE_SCAN`；MRR 条件受优化器限制 |
| constant ref | implemented | static-evidence | exact-key range |
| dependent ref | partial | static-evidence | 运行时 key map、边界构造和 ctx reset 必须一致 |
| 聚簇索引 MVCC | implemented | static-evidence | 基于 leader scan-context 事务 |
| 二级索引/覆盖索引 | implemented | static-evidence | 视页可见性和列需求决定是否回表 |
| ICP | implemented | static-evidence | dependent-ref 边界定位期临时关闭 |
| reverse scan | implemented | static-evidence | range/ref/index 均有对应测试资产 |
| record buffer | implemented | static-evidence | 必须保持 range end/restore 语义 |
| 分区表 | partial | static-evidence | 只允许一个 used partition；不是跨 partition PQ |
| READ COMMITTED / REPEATABLE READ | partial | static-evidence | 静态路径存在；并发 DML/purge 时序未在本任务运行证明 |
| InnoDB server read-only mode (`innodb_read_only`) | partial | static-evidence | `srv_read_only_mode` 令 read view 为空，走 no-view 可见性路径 |
| 普通 `START TRANSACTION READ ONLY` | unknown | none | 不得从 `srv_read_only_mode` 分支推导支持 |
| SERIALIZABLE | serial-fallback | static-evidence | 上层资格检查转串行 |
| XA/复杂显式事务 | unknown | none | 不应从静态代码推导完整支持 |

本模块没有单独的 InnoDB PQ 开关；它受全局/会话 PQ 主开关、hint、DOP、事务隔离级别、表类型和优化器资格检查共同约束。

## 9. 可观测性

- EXPLAIN/optimizer trace 可显示 PQ 是否选中、DIV/CUT 表和 access path；它不展示具体 read view 对象身份。
- handler/worker 错误最终进入 leader diagnostics；首错保存意在避免后续噪声覆盖根因。
- `PQ_Scan_ctx` 的 index S-lock 计数、range/slice 数量和 worker sent/read 行数可作为调试线索，但不是完整用户接口。
- DBUG/fault points 覆盖 range reset、handler read、worker 初始化等路径；只有实际运行才能证明唤醒和清理。
- 审计 snapshot 问题时至少同时记录：leader `trx_t`、leader read view、worker `trx_t`、worker read view、scan context 指向的 `trx_t`，否则容易把逻辑同快照误判为对象共享。

## 10. 源码与测试映射

### 10.1 源码锚点

| 契约 | 源码锚点 |
|---|---|
| range/border/config/slice 所有权 | `sql/parallel_query/pq_handler.h`：`PQ_Range`、`PQ_Borders`、`PQ_Config_Base`、`PQ_slices_mngr`、`PQ_Ctx_Base`、`Parallel_worker`、`Parallel_leader_Base` |
| 一级/二级切分和 ctx 领取 | `sql/parallel_query/pq_handler.cc`：`PQ_Range::split()`、`Parallel_leader_Base::build_ranges()`、`Parallel_worker::get_ctx()` |
| InnoDB 对象模型 | `storage/innobase/include/row0pread_pq.h`：`Iter`、`PQ_Config`、`PQ_Scan_ctx`、`PQ_Ctx` |
| leader/worker snapshot | `storage/innobase/handler/ha_innodb_pq.cc`：`pq_create_innodb_snapshot()`、`pq_clone_innodb_snapshot()`、`ha_innobase::pq_leader_scan_init()`、`ha_innopart::pq_leader_scan_init()` |
| index/range/ref/dependent-ref 初始化 | `storage/innobase/handler/ha_innodb_pq.cc`：`ha_innobase::pq_index_scan_init()`、`ha_innobase::pq_range_scan_init()`、`ha_innobase::pq_ref_scan_init()`、`ha_innobase::pq_ref_build_ranges()`；`sql/parallel_query/pq_iterators.cc`：`PQRefIterator::Read()` |
| worker next/end | `storage/innobase/handler/ha_innodb_pq.cc`：`ha_innobase::pq_worker_scan_next()`、`ha_innobase::pq_leader_scan_end()`、`ha_innobase::pq_worker_scan_end()` |
| cursor/MVCC/二级索引 | `storage/innobase/row/row0pread_pq.cc`：`PQ_PCursor::restore_position()`、`PQ_Scan_ctx::partition()`、`find_visible_record()`、`PQ_Ctx::read_record()` |
| snapshot 调用顺序 | `sql/parallel_query/pq_iterators.cc`：`ParallelScanIterator::Init()`；`sql/parallel_query/sql_parallel.cc`：`pq_worker_exec()` |
| 单分区 gate | `sql/table.cc`：`TABLE::suite_for_pq_division()`；`storage/innobase/handler/ha_innodb_pq.cc`：`ha_innopart::pq_leader_scan_init()` |

### 10.2 现有测试资产

| 主题 | MTR 资产 | 证据 |
|---|---|---|
| full/range/secondary | `pq_fullscan`、`pq_range_clust`、`pq_range_sec` | test-present |
| reverse/index/ref | `pq_range_scan_reverse`、`pq_reverse_index_scan`、`pq_ref_reverse_scan` | test-present |
| constant/dependent ref | `pq_jt_ref`、`pq_depend_ref`、`pq_ref_build_range` | test-present |
| ICP/record buffer | `pq_icp`、`pq_record_buffer` | test-present |
| MVCC/可见性/InnoDB server read-only mode | `pq_read_view`、`pq_rec_visible`、`pq_readonly` | test-present |
| partition | `pq_partition` | test-present |
| cursor 回归 | `pq_read_record_crash`、`pq_restart_after_select` | test-present；缺 restore-window 专项注入 |
| purge/并发事故探针 | `none` | 原 customer probes 未进入 `stable_branch`；需重新引入可维护的定向用例 |
| 初始化/范围错误 | `pq_range_exception`、`pq_worker_error`、`pq_abort` | test-present |

测试映射只证明资产存在，不等于当前基线通过。需要运行时验证时必须从 `build-ninja/mysql-test/mysql-test-run.pl` 执行。

## 11. 已知缺口、变更影响与质量风险

### 11.1 当前质量风险

| ID | 风险 | 证据与影响 | 建议验证 |
|---|---|---|---|
| `PQ-INNODB-RISK-001` | leader `trx_t`/read view 被 worker scan context 跨线程借用 | `PQ_Scan_ctx::m_trx` 为 raw pointer；正确性依赖 leader 清理晚于全部 worker | ASAN/TSAN + KILL/早退/purge 压测，核对 purge view 注册周期 |
| `PQ-INNODB-RISK-002` | snapshot 术语混淆导致错误修复 | leader snapshot creation、post-gather safeguard、worker read-view allocation 是三个事件 | 增加对象身份与 view low-limit 调试日志，做 RC/RR 差分 |
| `PQ-INNODB-RISK-003` | cursor restore 边界窗口出现丢行/重行 | 当前 `btr_pcur_t::restore_position()` + 显式方向移动与 mtr restart、reverse、buffer 交叉复杂 | 正反向、页分裂、purge、record buffer 组合测试；补 restore-window 专项注入 |
| `PQ-INNODB-RISK-004` | 二级索引历史注释与实现漂移 | 实现包含 ICP/回表/MVCC，注释仍称不支持 | 代码审查时以分支和测试为准，并清理误导注释（不属于本任务修改范围） |
| `PQ-INNODB-RISK-005` | ordered split 忙等和异常发布造成停滞 | 依赖 `m_spliting_id` 顺序推进，等待采用短周期轮询 | 注入 split worker 失败/KILL，确认所有等待者可退出 |
| `PQ-INNODB-RISK-006` | 单 partition 被误表述为多 partition 并行 | gate 与 `get_first_used_partition()` 只支持一个 used partition | EXPLAIN + 多 used partition 负向资格用例 |
| `PQ-INNODB-RISK-007` | dependent-ref key/context 复用错误 | key map、ICP 临时关闭、copied key、read-round reset 交织 | 多 key、NULL key、反向 ref、并发 worker 差分 |

### 11.2 变更影响清单

修改以下任一位置时，必须重新审查本模块：

- `trx_assign_read_view()`、`trx_clone_read_view()` 或 read view 注册/释放语义；
- `ParallelScanIterator::Init()` 或 `pq_worker_exec()` 的初始化顺序；
- `PQ_Scan_ctx`/`PQ_Ctx` 的指针所有权；
- `sel_restore_position_for_mysql()`、pcur store/restore 状态；
- B+Tree partition 算法、range 边界表示或 ordered split 发布；
- 二级索引可见性、ICP、回表、record buffer；
- partition 资格 gate；
- leader/worker scan end、KILL 或 retry 清理顺序。

任何此类变更都应同时复核 `PQ-INNODB-INV-*`、架构级 `PQ-ARCH-INV-*`、支持矩阵和上述 MTR 映射。
