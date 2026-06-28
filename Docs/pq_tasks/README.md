# Parallel Query V1 Progress Board

本文件是 PQ V1 的总进度看板。各阶段详细说明保存在对应 phase 文档中。

## Current Summary

- Last synced: 2026-06-28
- Latest local update: 2026-06-28 D1.8 visible fullscan void-pull gate 已完成
  本地实现、targeted 验证和独立 review，并已提交为 `3a040d891bd`。该批次新增
  `pq_visible_void_pull_fullscan_path` DBUG hook，让用户可见 clustered fullscan
  query 在 `PQTableScanIterator::Read()` 中逐行调用 worker handler
  `ha_pq_next(void*)`，并保持默认 PQWR fullscan path 不变。验证已通过：
  `mysqld` build、`pq_commercial_fullscan pq_read_threaded_pqwr_record_gather pq_stats`
  targeted MTR。任务书和完成报告见
  [commercial-port-d18-visible-void-pull-fullscan.md](commercial-port-d18-visible-void-pull-fullscan.md)。
- Current task selected: D2 default threaded fullscan void-pull 已完成本地编码、
  targeted 验证和独立 review，并修复 review 提出的 InnoDB worker-init
  double-free 与 worker THD kill propagation 问题。普通 eligible DOP2
  clustered fullscan 已从 D1.9 的 leader 同步 `ha_pq_next(void*)` path 推进到
  真实 worker-thread producer topology：worker 线程调用 `ha_pq_next(void*)`
  生产 record-image row，leader 通过现有 Exchange/MQ `Read()` loop 消费。
  任务书和完成报告见
  [commercial-port-d20-threaded-fullscan-void-pull.md](commercial-port-d20-threaded-fullscan-void-pull.md)。
- Previous task selected: D1.9 default fullscan void-pull gate 已完成本地编码、
  targeted 验证和独立 review。该任务将普通 eligible DOP2 clustered
  fullscan 默认路径切到 D1.8 已验证的 `ha_pq_next(void*)` visible
  void-pull path，旧 PQWR record-gather 路径保留为显式 DBUG regression hook。
  任务书和完成报告见
  [commercial-port-d19-default-fullscan-void-pull.md](commercial-port-d19-default-fullscan-void-pull.md)。
- Current M11-F update: F6d-1 no-row worker TABLE / handler contract smoke 已
  提交为 `1a75dea8a67`，targeted build/MTR passed，独立 review accepted。
  F6d-2 positive ref path design split 已完成本地文档：下一步 F6e-1 不直接
  打开 commercial `ha_pq_next(void*)`，先在当前 typed
  `PQ_Worker_context*` bridge 中迁移商用 exact ref range-build 语义，做
  worker-local / DBUG-only / no-MQ constant-ref row smoke。production
  `PQRefIterator::Read()`、`PQblockScanIterator::Read()`、`ha_pq_next()`、
  `pq_worker_scan_next()`、worker MQ row 和 `Query_result_mq::send_data()`
  仍保持关闭。F6d-2 independent design review accepted；F6e-1 worker-local
  constant-ref row smoke 已完成本地编码和 targeted build/MTR，独立
  code/docs/test review accepted。F6e-1 只证明 worker TABLE / handler 上的
  DBUG-only exact ref local row access，不打开 commercial `ha_pq_next(void*)`
  或 visible worker-side ref execution。F6e-2 test/docs-only last-key edge
  hardening 已完成，targeted MTR passed，独立 test/docs review accepted。
  F6e-3 visible / MQ route design 已完成并通过复审；F6e-4 debug-only
  `Exchange_nosort` / `PQRM` typed row-image carrier probe 已提交为
  `4f41c6da895`，targeted build/MTR passed，独立 review accepted。F6e-4
  只通过 DBUG hook 复用现有 typed record-image carrier helper 验证 ROW、
  FINISH、ERROR 可被 leader materialize/observe；`PQWR` /
  `Query_result_mq::send_data()`、production `Exchange_nosort` / `PQRM`
  行为和 visible worker-side ref gate 继续 deferred。F6e-5a visible
  worker-side constant-ref gate contract 已启动为 design-only：两个只读
  Explorer 均确认当前还缺 commercial `pq_ref_build_ranges()`、`ha_pq_next`
  / `pq_worker_scan_next(void*)` 正路径和 visible lifecycle/counter 合同，
  因此 F6e-5a 不直接编码用户可见 worker-side ref gate。
- Active update: 商用 Parallel Query 全量迁移冲刺已启动，权威任务板为
  [commercial-full-port-sprint.md](commercial-full-port-sprint.md)。当前目标
  已从 F6b 小步 contract/smoke 推进切换为：以
  `/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore` 商用实现为准，
  尽量全量平移支持场景；只有依赖私有模块、8.0.41 到 8.0.46 API 短期
  无法安全适配、会破坏 mysqld 基础行为或依赖本地不可构造环境的点，才允许
  临时 skip，并必须记录原因。已并行启动 SQL/PQ 模块、SQL 主干 hook、
  InnoDB/handler、MTR 测试套四个只读 Explorer；编码集成由主控串行合并，
  每个批次完成后启动独立 review Agent 并运行 build/MTR 验证。
- Execution policy update: 后续每个可独立验收的小任务必须单独 commit，避免
  后续形成大 commit；开发阶段只跑当前功能相关的 build/MTR 目标验证，完整
  `parallel_query` suite 和更大范围回归留到阶段收尾或开发结束后统一执行。
- Superseded history below: 下方 F6b/M11 小步状态只保留为历史记录，不再作为
  当前任务依据；任何“当前进入 F6b”“仍不打开 ORDER BY/ref/ICP”等旧表述均被
  [commercial-full-port-sprint.md](commercial-full-port-sprint.md) 覆盖。新的
  默认策略是商用全量迁移，只有大风险才允许临时 skip 并记录。
- Previous active update before full-port switch: M11-F6b worker constant-ref private row-token handoff design.
  E6g-3 已提交为 `8051cb9dd13`，证明 worker `position(record)` ref 可经
  private `PQWR` stable-ref frame decode 后 deep-copy 为 owned row-id bytes，
  并用 leader handler exact `ref_length` 做严格长度校验。E6g-4 已提交为
  `3679a6d317c`，新增 DBUG-only
  `pq_worker_result_stable_ref_pair_cmp_smoke`：两条真实
  worker refs 先经 stable-ref pair helper decode/deep-copy 成 owned
  vectors，再由 still-open leader handler 调用 existing
  `pq_orderby_handler_ref_adapter_smoke()` 验证 equal sort-key 下 `cmp_ref()`
  forward/reverse 非零且反对称。仍不替换 default comparator / heap reader，
  不改 production `Query_result_mq::send_data()` / `m_stable_output`，不打开
  readiness 或 visible ORDER BY gate。E6g-4 Code / Docs / Test Review Agent
  `019ef24a-b2ea-7392-99c5-2e9042f51f3f` 首轮要求修正：pair flag 不能依赖
  旧 two-row direct `cmp_ref()` smoke。已拆出独立
  `collect_two_worker_refs()` 收集路径，并新增 pair-only MTR 窗口验证旧
  two-row/direct adapter counters 不增长。fresh `mysqld` build passed，
  `pq_worker_attach_contract_smoke` + `pq_stats` targeted MTR passed，full
  `parallel_query` suite passed 89/89；re-review accepted，已提交为
  `3679a6d317c`。E6g-5 design-only 已提交为 `2b9a2a23d30`，定义
  fail-closed default comparator adapter 的输入/输出、不变量和拒绝原因。
  E6g-5b DBUG-only adapter shape 已提交为 `0de54003003`；E6g-5c tie-break
  contract smoke extension 已提交为 `22cc7777c88`，新增独立
  `pq_orderby_ref_adapter_contract_smoke`，验证 stable-ref owned refs 的方向
  与既有 adapter 一致，并覆盖 null handler、ref length mismatch、equal ref
  三类 fail-closed reject；继续禁止接入默认 comparator / heap reader，禁止
  打开 ORDER BY visible PQ gate。E6g-5d fail-closed audit 已提交为
  `8991c90dc04`：复用现有 `pq_commercial_order_by` 护栏确认
  `HAS_ORDER_BY`、preflight ready=0、default ordered read/materializer
  disabled，以及 visible ORDER BY 不增长 executed/workers/ranges counters。
  M11-F6 design-only positive-path phase selection 已提交为
  `31bdc66fb36`，选择 worker-side constant covering ref 作为后续最小正向
  候选。F6a design-only worker-side constant covering ref contract 已提交为
  `69d629abc7a`。F6a-1 worker constant-ref context shape 已提交为
  `9727d8be489`，只添加 private/DBUG-only owned ref-key context shape 和诊断
  计数。F6a-2 no-row/no-MQ ownership and cleanup smoke design 已提交为
  `18cfa6a7414`。F6a-2 no-row/no-MQ ownership and cleanup smoke 已提交为
  `d3320c7a4bf`。当前进入 F6b private row-token handoff design；F6b 仍不打开
  `pq_worker_scan_next()`、`PQRefIterator::Read()`、production
  `Query_result_mq::send_data()`、native `Record_buffer`、partition、MVI、
  reverse 或任何 ORDER BY visible gate。
- Current phase correction: M11-E5d-5e-1 已提交为 `f54474bda65`；M11-E5d-5e-2 在 5e-1 candidate-disabled contract 后新增 central preflight blocker，仍保持现有 `HAS_ORDER_BY` serial boundary。下方超长历史摘要中的 5c 旧尾句不作为当前状态来源。
- Current M11-E correction: M11-E5r closure 和 Post-E5r handoff 是当前
  ORDER BY 权威状态；真实执行仍 blocked，source work stopped。下方超长历史
  摘要如仍出现 5g-2 waiting final review 或 E5g-4f 之后缺失 E5r closure，
  不作为当前状态来源。
- Current phase: Phase 0-9 已提交完成；PQ V1 风险收敛、V2-0 到 V2-12C-1 已推进出一个 MySQL 8.0.46 上的保守 PQ 适配基座，覆盖 worker lifecycle、read view、KILL、DOP2/DOP4 row stream、基础聚合和部分 GROUP BY partial aggregation；当前目标已切换为平移 `/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore` 商用 Parallel Query 实现。M1-M10 已完成当前支持子集和测试收口；M11 Post-M10 Commercial Main Architecture Restart 已启动，M11-A0/B0 design accepted；M11-A1-A5 plan clone/resolver contract 与 preflight 已完成；M11-B1/B2 PQWR leader decode adapter、M11-B3a/B3b/B3c worker-result wiring probes 已完成；M11-D0-D6 已完成并提交；M11-E0/E1/E2/E3 已完成并提交；M11-E4 user-visible ORDER BY gate design 已提交；M11-E5a real `Exchange_sort` worker-frame materialization design 已提交；M11-E5b-0 ORDER BY frame contract helper 已提交；M11-E5b-1 controlled frame K-way merge smoke 已提交；M11-E5b-2 merge-path empty-worker/ERROR edge smoke 已提交；M11-E5b-3 debug-only row materialization smoke 已提交；M11-E5c user-visible ORDER BY gate design 已提交；M11-E5d Filesort State Contract design 已提交；M11-E5d-0 read-only helper inventory 已提交；M11-E5d-S0 saved ORDER/GROUP helper design 已通过 review；M11-E5d-S1 saved state contract shape 已提交；M11-E5d-S2 leader save/restore smoke 已提交；M11-E5d-S3 clone-copy contract 已提交；M11-E5d-1 fail-closed Filesort contract shape 已提交；M11-E5d-2 design 已提交；M11-E5d-2a owned ORDER chain copy smoke 已提交；M11-E5d-2b optimized flag contract smoke 已提交；M11-E5d-2c restore-to-sidecar contract smoke 已提交；M11-E5d-2d clone-copy contract smoke 已提交；M11-E5d-3 post-sidecar Filesort boundary design 已提交；M11-E5d-3a restored ORDER Filesort contract 已提交；M11-E5d-3b Filesort constructor risk design 已提交；M11-E5d-3c debug-only Filesort construction smoke 已提交；M11-E5d-4 Sort_param / Exchange_sort initialization boundary design 已提交；M11-E5d-4a debug-only Sort_param init smoke 已提交；M11-E5d-4b Exchange_sort real-state adapter boundary design 已提交；M11-E5d-4b-1 debug-only Exchange_sort scalar sort-state adapter shape 已提交；M11-E5d-4c optimizer-side Filesort/Sort_param scalar handoff design 已提交；M11-E5d-4c-1 debug-only optimizer-to-Exchange_sort scalar handoff 已提交；M11-E5d-5 real `Exchange_sort` init / MQ / Read boundary design 已提交；M11-E5d-5a `Exchange_sort` real-init state owner shape 已提交；M11-E5d-5b sort-key buffer / record-group allocation smoke 已提交；M11-E5d-5c controlled ORDER BY MQ-to-record-group loader 已提交；M11-E5d-5d debug-only ordered leader Read shadow path 已提交；M11-E5d-5e-1 eligibility contract 已提交；M11-E5d-5e-2 execution preflight blocker 已提交；M11-E5d-5f runtime prerequisite diagnostics 已提交；M11-E5g-0 worker ORDER BY frame producer and streaming heap read boundary design 已提交；M11-E5g-1 DBUG-only worker ORDER BY `PQOF` producer smoke 已提交；M11-E5g-2 streaming `Exchange_sort` heap-read state-machine smoke 已完成本地实现和验证，等待 final review。
- Latest commits:
  - Worker row pull contract: `34e1428fdd2` Document PQ worker row pull contract
  - Worker typed pull bridge: `02237ec1d0f` Add PQ typed worker pull bridge
  - Worker Record_buffer diagnostic: `a5343dfc10f` Add PQ worker Record_buffer diagnostic
  - Commercial clustered range guard: `72eefafb3d4` Add PQ commercial clustered range test
  - Worker ExecuteIterator smoke plan: `7dfec0ec0f6` Plan PQ worker execute iterator smoke
  - Worker ExecuteIterator smoke gate: `206a5a9b48a` Add PQ worker execute iterator smoke gate
  - Worker readinfo smoke gate: `062e8fdfb7c` Advance PQ worker execute smoke to execute gate
  - Worker readinfo production risk closure: `c15da6be890` Keep PQ worker readinfo production gate closed
  - Worker iterator construction smoke: `098d9af3aa5` Add PQ worker iterator construction smoke
  - Worker table owned iterator smoke: `ad69503c511` Use worker table for PQ iterator smoke
  - Worker iterator Init/End smoke: `3a1907cccf1` Add PQ worker iterator init smoke
  - Worker iterator single-row Read smoke: `dea81de92e5` Add PQ worker iterator read smoke
  - Phase 9: `9c7e9aede42` Add PQ phase 9 test suite migration
  - V1 risk convergence: `69ed0ac66e7` Tighten PQ V1 risk boundaries
  - V2-2: `a123cdabe7c` Add PQ V2-2 handler context bridge
  - V2-3: `54bd78429cf` Tighten PQ V2-3 read view boundary
  - V2-4: `0b7f7485a7b` Add PQ V2-4 range planning export
  - V2-4 observable: `79d1355cf2f` Add PQ V2-4 range planning status
  - V2-5: `1e8078544e8` Add PQ V2-5 worker lifecycle smoke
  - V2-6: `d0b4988a584` Add PQ V2-6 exchange row stream smoke
  - V2-7: `1d39b510348` Add PQ V2-7 predicate projection boundary
  - V2-8A dispatch: `25f216d9a9d` Dispatch PQ V2-8A contract design
  - V2-8A range findings: `332361020f4` Record PQ V2-8A range dispatch findings
  - V2-8A complete: `8dc30b6a1d2` Complete PQ V2-8A contract design
  - V2-8B: `db14645bcf4` Add PQ V2-8B row image protocol
  - V2-8C plan: `75dc2e297db` Plan PQ V2-8C DOP1 real full scan
  - V2-8C contract/gate: `24209d09ce3` Add PQ V2-8C worker context gates
  - V2-8C leader mode: `6638e36def7` Add PQ V2-8C leader scan mode contract
  - V2-8C worker open carrier: `36b2d9a764c` Add PQ V2-8C worker open context carrier
  - V2-8C worker table helper: `09033e1c2a0` Add PQ V2-8C worker table open helper
  - V2-8C worker THD helper: `9c242c61cc7` Add PQ V2-8C worker THD lifecycle helper
  - V2-8C worker open smoke: `2dab52a2d9c` Add PQ V2-8C worker open table smoke helper
  - V2-8C worker open smoke run: `a79c2109d7a` Run PQ V2-8C worker open table smoke
  - V2-8C worker handler smoke: `13ce85e4d47` Run PQ V2-8C worker handler init smoke
  - V2-8C status: `cb34ece63ef` Record PQ V2-8C worker handler smoke status
  - V2-8D first row contract: `1d39beafe15` Tighten PQ V2-8D first row contract
  - V2-8D read-view strategy: `50f7ed3dbd5` Select PQ V2-8D read view strategy
  - V2-8E execute commit point: `657cc94eb9a` Add PQ V2-8E execute commit point
  - V2-8F pull adapter plan: `d5c004c9461` Plan PQ V2-8F pull adapter
  - V2-8F pull adapter gates: `e73c73f053f` Add PQ V2-8F pull adapter gates
  - V2-8F visibility helper: `379e3c60581` Extract PQ V2-8F visibility helper
  - V2-8F conversion helper: `62f8058375d` Add PQ V2-8F record conversion helper
  - V2-8F callback accessors: `e817a9176ec` Add PQ V2-8F callback row accessors
  - V2-8F callback conversion helper: `f23320fb5f7` Add PQ V2-8F callback conversion helper
  - V2-8F callback conversion smoke: `816a6808699` Add PQ V2-8F callback conversion smoke
  - V2-8G EXECUTE callback smoke: `03fb7eb4bf8` Add PQ V2-8G execute callback smoke
  - V2-8H row stream activation boundary: `ccc187e271c` Add PQ V2-8H row stream boundary helper
  - V2-8I iterator runtime state contract: `eb0cd14e659` Add PQ V2-8I iterator runtime state
  - V2-8J real read activation plan: planned，真实 `Read()` 打开前硬阻塞拆分
  - V2-8J-1 callback row conversion gate: 本轮提交，`Parallel_callback_smoke_rows >= 1` 稳定通过
  - V2-8J-1 typed MQ producer smoke: 本轮提交，callback-converted worker record image 经 MQ 被 leader materialize
  - V2-8J-2 worker producer ERROR smoke: 本轮提交，typed ERROR 可被 leader 观察并由 smoke 内部消费
  - V2-8J-2 worker producer abort smoke: 本轮提交，leader abort 可 detach MQ 并由 leader 观察 EOF
  - V2-8J-3 Read shadow path scaffold: 本轮提交，debug-only gate，默认不可达，受 PROBE-supported guard 保护
  - V2-8J-4 PROBE diagnostics: 本轮提交，新增 attempts/success/unsupported 状态变量
  - V2-8J-4 materialize status helper: 本轮提交，显式区分 ROW/EOF/WOULD_BLOCK/ERROR
  - V2-8J-4 ROW-only enqueue helper: 本轮提交，拆出可连续发送多行的 ROW helper
  - V2-8J-4 callback multi-row producer API: 本轮提交，新增 `PQ_row_sink` 和 InnoDB push producer
  - V2-8J-4 callback multi-row producer smoke: 本轮提交，SQL-owned sink 连续发送 2 条 ROW + FINISH，leader 通过 Exchange materialize
  - V2-8J-4 Read wait/kill policy: 本轮提交，WOULD_BLOCK 进入 bounded wait，leader kill 可传播到 worker/MQ
  - V2-8J-4 PROBE unsupported diagnostics: 本轮提交，细分 gate/thread-budget/range-init unsupported
  - V2-8J-4 first PROBE gate fix: 本轮提交，`m_prebuilt->index` 未设置时使用 first clustered index
  - V2-8J-4 shadow Read first-row MTR: 本轮提交，`pq_read_shadow_dop1` 验证 debug-only first-row no-fallback
  - V2-8J-4 shadow Read 2-row producer: 本轮提交，limited callback producer 写入当前 Exchange，shadow `Read()` 消费 2-row + EOF
  - V2-8K-1 worker thread scaffold: 本轮提交，新增 joinable `parallel_query_worker` scaffold、worker-thread 内部 THD create/destroy、wait/cleanup join 和原子 worker status CAS
  - V2-8K-2 threaded callback producer: 本轮提交，新增 `pq_read_threaded_shadow_path`，worker thread 写 Exchange，leader `Read()` 并发消费完整小表
  - V2-8K-3 threaded hardening: 本轮提交，新增 worker ERROR token 和 abort-after-start cleanup MTR，完整 suite 增至 23 个测试
  - V2-8L threaded semantics: 本轮提交，修复 worker read_set/prebuilt lifecycle、空表 EOF 和 EOF-before-first-row executed counter，完整 suite 增至 24 个测试
  - V2-8M experimental threaded gate: 本轮提交，新增 `parallel_query_experimental_threaded_dop1`，无 debug 下显式启用 DOP=1 threaded full scan，完整 suite 增至 25 个测试
  - V2-8N threaded external KILL: 本轮提交，新增 `pq_read_threaded_worker_started` debug sync 点和 external `KILL QUERY` cleanup MTR
  - V2-8O threaded aggregate coverage: 本轮提交，验证 experimental DOP=1 threaded row stream 可支撑上层 MySQL `COUNT/SUM/AVG/MIN/MAX`
  - V2-8P DOP multi-queue Exchange smoke: 本轮提交，新增 row-image smoke 专用 status counters，并精确验证 DOP=1/2/4 synthetic ROW/FINISH 消费
  - V2-8Q DOP2 experimental guard: 本轮提交，验证 experimental threaded gate 不会误启 DOP=2 真实执行
  - V2-9 DOP range correctness: 已生成任务书，拆分 V2-9A/B/C/D，等待 range-aware callback producer 最小实现确认
  - V2-9A Range Dispatch Contract: 本轮提交，新增 `Parallel_ranges_dispatched` / `Parallel_empty_worker_ranges`，DOP=1 worker scan init 可观测领取 range
  - V2-1: `9a58ff94cdf` Add PQ V2-1 iterator safe fallback
  - V2-0: `a420e8a3f26` Add PQ V2-0 execution state contract
  - Phase 8: `113d2ba44c1` Add PQ phase 8 V1 completion scaffolding
  - Phase 7: `9af8fe777d2` Add PQ phase 7 EXPLAIN and MVP tests
  - M11-B3b: `b4b88c30ac9` Add PQ M11B local worker result wiring smoke
  - M11-B3c design: `9e1b5608993` Plan PQ M11B worker result threaded probe
  - M11-B3c code: `8847eddaf0f` Add PQ M11B threaded worker result probe
  - M11-D0: `b44673ce822` Plan PQ M11D parallel scan lifecycle
  - M11-D1: `b81666299fc` Add PQ M11D parallel scan lifecycle skeleton
  - M11-D2: `63874ced340` Close PQ M11D lifecycle status smoke design
  - M11-D3 design: `68407a4089e` Plan PQ M11D lifecycle construction probe
  - M11-D3 code: `db12b782ba0` Add PQ M11D lifecycle construction probe
  - M11-D4 design: `160a80b0430` Plan PQ M11D positive path migration
  - M11-D4a design: `050aa2d62c` Plan PQ M11D worker attach smoke
  - M11-D4a code: `42d99f3e7b` Add PQ M11D worker attach smoke
  - M11-D4b design: `19875a6c72` Plan PQ M11D leader row stream smoke
  - M11-D4b code: `89b2fd187c` Add PQ M11D leader row stream smoke
  - M11-D4c design: `b2ac9d3dc7` Plan PQ M11D commit-point error smoke
  - M11-D4c code: `06ce7035e7` Add PQ M11D commit-point error smoke
  - M11-D5 design: `767eb6c138` Plan PQ M11D row-value correctness
  - M11-D5 code: `3c1970a5b` Add PQ M11D row-value smoke
  - M11-D6 design: `b7f6712eee` Plan PQ M11D ParallelScanIterator debug path
  - M11-D6 code: `ec873cffc8` Add PQ M11D ParallelScanIterator debug path
  - M11-E0 design: `e8645dbc2b` Plan PQ M11E order by exchange sort
  - M11-E1 code: `ee648d7ccd` Add PQ M11E exchange sort shape
  - M11-E2 code: `47138dcd8c` Add PQ M11E exchange sort adapter smoke
  - M11-E3 design: `d89b69b335` Plan PQ M11E order gather debug path
  - M11-E3 code: `eed682ed9c` Add PQ M11E order gather debug smoke
  - M11-E4 design: review accepted，保持 `HAS_ORDER_BY` serial boundary，不直接打开用户可见 ORDER BY PQ
  - M11-E5a design: review accepted，定义 ORDER BY worker-frame materialization contract，不改源码
  - M11-E5b-0 code: `da8b966035` Add PQ M11E order by frame contract
  - M11-E5b-1 code: `ff11e6787e8` Add PQ M11E order frame merge smoke
  - M11-E5b-2 code: `2f8aad71c9b` Add PQ M11E order frame merge edge smoke
  - M11-E5b-3 code: `eec7397eec0` Add PQ M11E order frame materialization smoke
  - M11-E5c design: `9d63833227c` Plan PQ M11E order by visible gate
  - M11-E5d design: `2a154522da8` Plan PQ M11E filesort state contract
  - M11-E5d-0 inventory: `1b7776df79d` Record PQ M11E filesort helper inventory
  - M11-E5d-S0 saved ORDER/GROUP helper design: Review Agent `ACCEPT`，sidecar-first，S1-S3 must precede Filesort construction
  - M11-E5d-S1 saved state contract shape: `5ad7ed85d8f` Add PQ M11E saved order group contract
  - M11-E5d-S2 leader save/restore smoke: `334c4ae48d0` Add PQ M11E saved state restore smoke
  - M11-E5d-S3 clone-copy contract: `c49fc064cca` Add PQ M11E saved state clone copy smoke
  - M11-E5d-1 fail-closed Filesort contract shape: `99526a6067d` Add PQ M11E filesort contract shape
  - M11-E5d-2 owned saved ORDER/GROUP helper state design: `61e074704e1` Plan PQ M11E owned order group state
  - M11-E5d-2a owned ORDER chain copy smoke: `bc55cff554f` Add PQ M11E owned order chain smoke
  - M11-E5d-2b optimized ORDER flag contract smoke: `6e815347ce5` Add PQ M11E order chain flag smoke
  - M11-E5d-2c restore-to-sidecar contract smoke: `424216c1753` Add PQ M11E order chain restore smoke
  - M11-E5d-2d clone-copy contract smoke: `a4cc361c759` Add PQ M11E order chain clone copy smoke
  - M11-E5d-2 completion status: `7caa73c9518` Record PQ M11E sidecar clone completion
  - M11-E5d-3 post-sidecar Filesort boundary design: `2665375de96` Plan PQ M11E filesort sidecar boundary
  - M11-E5d-3a restored ORDER Filesort contract: `952beeeafa1` Add PQ M11E restored order filesort contract
  - M11-E5d-3b Filesort constructor risk design: `1020b3b5c2b` Plan PQ M11E filesort construction smoke
  - M11-E5d-3c debug-only Filesort construction smoke: `c4b7a7953e1` Add PQ M11E filesort construction smoke
  - M11-E5d-4 Sort_param / Exchange_sort initialization boundary: `cfc1ad722cd` Plan PQ M11E sort param boundary
  - M11-E5d-4a debug-only Sort_param init smoke: `08579119b9b` Add PQ M11E sort param init smoke
  - M11-E5d-4b Exchange_sort real-state adapter boundary: `ea83b4c3c9c` Plan PQ M11E exchange sort state adapter
  - M11-E5d-4b-1 debug-only Exchange_sort scalar sort-state adapter shape: `7aaaba10777` Add PQ M11E exchange sort state shape
  - M11-E5d-4c optimizer-side Filesort/Sort_param scalar handoff: `01469aa124c` Plan PQ M11E optimizer sort state handoff
  - M11-E5d-4c-1 debug-only optimizer-to-Exchange_sort scalar handoff: `4dc34748200` Add PQ M11E optimizer sort state handoff
  - M11-E5d-5 real Exchange_sort init boundary design: `89dbb6143a5` Plan PQ M11E exchange sort init boundary
  - M11-E5d-5a Exchange_sort real-init state owner shape: `83d91fed0f5` Add PQ M11E exchange sort init state shape
  - M11-E5d-5b Exchange_sort allocation smoke: `45c1b4efd30` Add PQ M11E exchange sort allocation smoke
  - M11-E5d-5c controlled ORDER BY MQ-to-record-group loader: `4163db43197` Add PQ M11E order by frame loader smoke
  - M11-E5d-5d debug-only ordered leader Read shadow path: `06b68f558d3` Add PQ M11E ordered read shadow smoke
  - M11-E5d-5e visible ORDER BY eligibility gate design: `b6d964704e1` Plan PQ M11E order by eligibility gate
  - M11-E5d-5e-1 ORDER BY eligibility contract helper: `f54474bda65` Add PQ M11E order by eligibility contract
  - M11-E5d-5e-2 ORDER BY execution preflight blocker: `68b27d804ca` Add PQ M11E order by execution preflight
- Phase 8 validation:
  - `cmake --build build-ninja --target mysqld -j 16` 通过
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help` 通过
  - `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt` 通过，完整 `parallel_query` suite 共 10 个测试通过
- Phase 9 validation:
  - `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt` 通过，实际 `parallel_query` 测试 12 个，加 `shutdown_report` 共 13 项通过
- Current V1 capability boundary: 当前分支具备 PQ 系统变量、保守 eligibility、EXPLAIN 注解、fallback/status 统计、SQL/InnoDB/worker/MQ 框架和 V1 测试闭环；真实并行执行仍未启用，eligible 查询最终仍保守走串行路径。
- Active orchestration:
  - [v1-risk-convergence.md](v1-risk-convergence.md): V1 风险收敛已提交，覆盖 locking read、fallback counter、PQUnsuiteInfo 生命周期、best_ref/qep_tab 访问安全和 V1 测试缺口。
  - [v2-0-execution-state-contract.md](v2-0-execution-state-contract.md): V2-0 已实现，定义 PQ execution state contract，供 V2-1 iterator ownership 使用。
  - [v2-1-iterator-safe-fallback.md](v2-1-iterator-safe-fallback.md): V2-1 已完成验证，建立 PQ iterator ownership 和 Init 安全回退。
  - [v2-2-handler-innodb-context-bridge.md](v2-2-handler-innodb-context-bridge.md): V2-2 已提交，补齐 handler virtual API 与 InnoDB leader context bridge。
  - [v2-3-read-view-trx-contract.md](v2-3-read-view-trx-contract.md): V2-3 已提交，收敛 InnoDB read view / trx / row_prebuilt 边界。
  - [v2-4-parallel-reader-range-partition.md](v2-4-parallel-reader-range-partition.md): V2-4 已提交，完成 Parallel_reader thin adapter、range planning observable 和 DOP>1 range gate。
  - [v2-5-worker-thd-minimal-scan.md](v2-5-worker-thd-minimal-scan.md): V2-5 已提交，完成 worker lifecycle smoke observable，真实 row stream 仍未打开。
  - [v2-6-exchange-gather-row-stream.md](v2-6-exchange-gather-row-stream.md): V2-6 已完成验证，先用 synthetic MQ token/row stream 验证 Exchange/Gather，不接真实 InnoDB row。
  - [v2-7-predicate-projection-boundary.md](v2-7-predicate-projection-boundary.md): V2-7 已提交，锁定 leader-side predicate/projection contract，避免提前引入 worker-side Item/JOIN clone。
  - [v2-8-single-table-fullscan-closure.md](v2-8-single-table-fullscan-closure.md): V2-8 当前任务书，准备真实单表 clustered full scan 闭环。
  - [v2-8a-worker-handler-prebuilt-contract.md](v2-8a-worker-handler-prebuilt-contract.md): V2-8A 已完成，确认 worker 独立 TABLE/handler/prebuilt、leader-pinned read view、typed worker wrapper、DOP=1 first gate。
  - [v2-8b-row-image-protocol.md](v2-8b-row-image-protocol.md): V2-8B 已完成，定义 typed MQ row message、fixed record image copy 和 synthetic materialization smoke。
  - [v2-8c-dop1-real-fullscan.md](v2-8c-dop1-real-fullscan.md): V2-8C 已完成两个只读确认、contract/gate 第一段、leader probe/execute mode API、worker open context carrier 生命周期、worker THD/TABLE helper 边界、safe-window open-table smoke 和 handler init/end smoke；已落地 worker open context、typed InnoDB worker wrapper、DOP=1/blob/independent TABLE/record gate，真实 row scan 仍未打开。
  - [v2-8d-first-row-read-view-contract.md](v2-8d-first-row-read-view-contract.md): V2-8D 已完成方案选型：首行定位按 `index_first()` 等价协议，真实读取走 `Parallel_reader` visibility adapter 路线，不直连 worker-local `row_search_mvcc()`；`pq_worker_scan_next()` 继续 disabled，直到 EXECUTE commit point 和 pull adapter 完成。
  - [v2-8e-execute-commit-point.md](v2-8e-execute-commit-point.md): V2-8E 已完成 InnoDB leader `EXECUTE` commit point primitive；`PROBE` 仍 fallback-safe，`EXECUTE` 在 leader 线程绑定 active read view，但当前 SQL iterator 尚不调用 EXECUTE；build 和完整 `parallel_query` suite 通过。
  - [v2-8f-parallel-reader-pull-adapter.md](v2-8f-parallel-reader-pull-adapter.md): V2-8F callback conversion smoke primitive 已完成，`InnoDB_pq_scan_ctx` 可用 `Parallel_reader(0)` 在当前线程转换最多一行 visible clustered record；不接 SQL `Read()`，不设置 EXECUTED；build 和完整 `parallel_query` suite 通过。
  - [v2-8g-execute-callback-smoke.md](v2-8g-execute-callback-smoke.md): V2-8G 已完成，SQL iterator 在 safe fallback window 内尝试固定 DOP=1 EXECUTE callback smoke；新增 `Parallel_callback_smoke_attempts` / `Parallel_callback_smoke_rows`；失败不影响 serial fallback；完整 `parallel_query` suite 通过。
  - [v2-8h-row-stream-activation.md](v2-8h-row-stream-activation.md): V2-8H 已完成，抽出 `Exchange_nosort::materialize_next_record_image()`，不接真实 `Read()`；完整 `parallel_query` suite 通过。
  - [v2-8i-iterator-runtime-state.md](v2-8i-iterator-runtime-state.md): V2-8I 已完成，增加 iterator runtime state contract，不打开真实 `Read()`；完整 `parallel_query` suite 通过。
  - [v2-8j-real-read-activation-plan.md](v2-8j-real-read-activation-plan.md): V2-8J 已拆分真实 `Read()` 激活前的硬阻塞；V2-8J-1 callback row producer smoke、V2-8J-2 FINISH/EOF/ERROR/abort skeleton、V2-8J-3 `Read()` shadow scaffold、V2-8J-4 PROBE diagnostics、Exchange materialize status helper、ROW-only enqueue helper、callback multi-row producer API、SQL 层 limited multi-row producer smoke、`Read()` wait/kill policy、PROBE unsupported 细分诊断、首次 PROBE gate 修复、debug shadow first-row MTR 和 debug shadow 2-row/EOF producer 已完成。
  - [v2-8k-worker-thread-producer.md](v2-8k-worker-thread-producer.md): V2-8K-1/2/3 已完成 worker thread lifecycle scaffold、debug-only threaded callback producer、worker ERROR 和 abort/EOF cleanup hardening。
  - [v2-8l-threaded-semantics-boundary.md](v2-8l-threaded-semantics-boundary.md): V2-8L 已完成 debug-only threaded projection/WHERE/empty EOF 语义边界。
  - [v2-8m-experimental-threaded-gate.md](v2-8m-experimental-threaded-gate.md): V2-8M 已完成默认 OFF 的实验变量保护路径。
  - [v2-8n-threaded-external-kill.md](v2-8n-threaded-external-kill.md): V2-8N 已完成 debug-only threaded external `KILL QUERY` cleanup 验证。
  - [v2-8o-threaded-aggregate-coverage.md](v2-8o-threaded-aggregate-coverage.md): V2-8O 已完成 experimental threaded DOP=1 基础聚合覆盖。
  - [v2-8p-dop-multiqueue-exchange-smoke.md](v2-8p-dop-multiqueue-exchange-smoke.md): V2-8P 已完成 DOP=1/2/4 synthetic row-image 多队列 smoke 验证。
  - [v2-8q-dop2-experimental-guard.md](v2-8q-dop2-experimental-guard.md): V2-8Q 已完成 experimental gate 的 DOP=2 fallback guard。
  - [v2-9-dop-range-correctness.md](v2-9-dop-range-correctness.md): V2-9 已启动 DOP>1 range correctness 任务拆分。
  - [v2-9a-range-dispatch-contract.md](v2-9a-range-dispatch-contract.md): V2-9A 已完成 range dispatch observable。
  - [v2-9b-range-aware-callback-producer.md](v2-9b-range-aware-callback-producer.md): V2-9B 已完成 range-aware callback producer。
  - [v2-9c-debug-dop2-threaded-shadow.md](v2-9c-debug-dop2-threaded-shadow.md): V2-9C 已完成 debug-only DOP=2 threaded shadow path、worker ERROR 和 external KILL 覆盖；完整 suite 32 项通过。
  - [v2-9d-multirange-callback-drain.md](v2-9d-multirange-callback-drain.md): V2-9D 已完成 ranges > workers 的 callback drain，DOP=2 debug shadow 完整 1024 行集合验证；完整 suite 33 项通过。
  - [v2-9e-experimental-dop2-gate.md](v2-9e-experimental-dop2-gate.md): V2-9E 已新增默认 OFF 的 `parallel_query_experimental_threaded_dop`，无 debug 下初始只允许 DOP=2，并补齐基础聚合和 DOP1/DOP4 负向 gate 覆盖；完整 suite 36 项通过。
  - [v2-execution-path-roadmap.md](v2-execution-path-roadmap.md): V2 真实执行路径拆分，覆盖 SQL iterator、worker THD、Exchange/Gather row 流、InnoDB 分片扫描、full scan 闭环和基础聚合。
  - [v2-test-matrix.md](v2-test-matrix.md): V1/V2 阶段化 MTR 测试矩阵，明确 DOP=1 first 和 DOP>1 range-partition gate。
- Commercial port validation:
  - M1: `cmake --build build-ninja --target mysqld -j 16` 通过
  - M1: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_vars --parallel=1 --vardir=/tmp/pqv_m1_vars --tmpdir=/tmp/pqt_m1_vars` 通过
  - M2: `cmake --build build-ninja --target mysqld -j 16` 通过
  - M2: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_vars --parallel=1 --vardir=/tmp/pqv_m2_vars --tmpdir=/tmp/pqt_m2_vars` 通过
  - M2: `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m2_full --tmpdir=/tmp/pqt_m2_full` 通过，完整当前 suite 69 项成功
  - M3: `cmake --build build-ninja --target mysqld -j 16` 通过
  - M3: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_clone_diagnostics --parallel=1 --vardir=/tmp/pqv_m3_clone --tmpdir=/tmp/pqt_m3_clone` 通过
  - M3: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_stats --parallel=1 --vardir=/tmp/pqv_m3_stats --tmpdir=/tmp/pqt_m3_stats` 通过
  - M3: `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m3_full --tmpdir=/tmp/pqt_m3_full` 通过，完整当前 suite 70 项成功
  - M4a: `cmake --build build-ninja --target mysqld -j 16` 通过
  - M4a: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_worker_result pq_stats pq_read_threaded_external_kill pq_read_threaded_worker_error pq_read_threaded_row_image_datatypes --parallel=1 --vardir=/tmp/pqv_m4_target --tmpdir=/tmp/pqt_m4_target` 通过
  - M4a: `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m4_full --tmpdir=/tmp/pqt_m4_full` 通过，完整当前 suite 71 项成功
  - M4b: `cmake --build build-ninja --target mysqld -j 16` 通过
  - M4b: `./mtr --suite=parallel_query pq_commercial_worker_result pq_stats` 通过
  - M4b: `./mtr --suite=parallel_query` 通过，完整当前 suite 73 项成功
  - M5: `cmake --build build-ninja --target mysqld -j 16` 通过
  - M5: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_dop2_read_view pq_read_threaded_dop2_external_kill pq_read_threaded_dop4_external_kill pq_read_threaded_mdl_concurrency pq_locking_read_fallback --parallel=1 --vardir=/tmp/pqv_m5 --tmpdir=/tmp/pqt_m5` 通过
  - M5: `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=4 --vardir=/tmp/pqv_m5_full --tmpdir=/tmp/pqt_m5_full` 通过，完整当前 suite 71 项成功
  - M6: `cmake --build build-ninja --target mysqld -j 16` 通过
  - M6: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_fullscan --parallel=1 --vardir=/tmp/pqv_m6 --tmpdir=/tmp/pqt_m6` 通过
  - M6: `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=4 --vardir=/tmp/pqv_m6_full2 --tmpdir=/tmp/pqt_m6_full2` 通过，完整当前 suite 72 项成功
  - M7: Reconcile PQ aggregation counters
  - M7: `cmake --build build-ninja --target mysqld -j 16` 通过
  - M7: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_groupby_dop_partial_counters --parallel=1 --vardir=/tmp/pqv_m7_counter2 --tmpdir=/tmp/pqt_m7_counter2` 通过
  - M7: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_stats --parallel=1 --vardir=/tmp/pqv_m7_stats --tmpdir=/tmp/pqt_m7_stats` 通过
  - M7: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_groupby_diagnostics pq_groupby_dop1_sum_min_max pq_groupby_dop2_partial_count_min_max pq_groupby_dop4_partial_count_sum_min_max pq_groupby_dop_partial_count_star pq_groupby_dop_partial_count_nullable pq_groupby_dop2_partial_worker_error pq_groupby_dop2_partial_external_kill --parallel=1 --vardir=/tmp/pqv_m7b --tmpdir=/tmp/pqt_m7b` 通过，9 项成功
  - M7: `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m7_full2 --tmpdir=/tmp/pqt_m7_full2` 通过，完整当前 suite 72 项成功
  - M8: `cmake --build build-ninja --target mysqld -j 16` 通过
  - M8: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_order_by --parallel=1 --vardir=/tmp/pqv_m8_order2 --tmpdir=/tmp/pqt_m8_order2` 通过
  - M8: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_stats --parallel=1 --vardir=/tmp/pqv_m8_stats --tmpdir=/tmp/pqt_m8_stats` 通过
  - M8: `TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_order_by pq_explain_fallback pq_read_threaded_dop2_multirange pq_groupby_dop2_partial_count_min_max --parallel=1 --vardir=/tmp/pqv_m8b --tmpdir=/tmp/pqt_m8b` 通过，5 项成功
  - M8: `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m8_full2 --tmpdir=/tmp/pqt_m8_full2` 通过，完整当前 suite 73 项成功
  - M9-A: `./mtr --suite=parallel_query pq_commercial_ref_icp pq_not_support pq_stats` 通过
  - M9-A: `./mtr --suite=parallel_query` 通过，完整当前 suite 74 项成功
  - M9-B0: design-only，确认 M9-B 必须拆为 candidate probe、secondary partition、callback row production 三段
  - M9-B1: `cmake --build build-ninja --target mysqld -j 16` 通过
  - M9-B1: `./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support` 通过
  - M9-B1: `./mtr --suite=parallel_query` 通过，完整当前 suite 74 项成功
  - M9-B2: `cmake --build build-ninja --target mysqld -j 16` 通过
  - M9-B2: `./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support` 通过
  - M9-B2: `./mtr --suite=parallel_query` 通过，完整当前 suite 74 项成功
  - M9-B3: design-only 任务书已生成，Review Agent 返回 `ACCEPT WITH RISKS`
  - M9-B3a: 编码入口检查发现 upstream `Parallel_reader` secondary visibility 仍 unsupported，按设计硬停止条件未做源码改动；blocked-result Review Agent 返回 `ACCEPT`
  - M9-B3a-0: Secondary Visibility Helper fail-closed contract 已实现，代码/任务 Review Agent 返回 `APPROVE`
  - M9-B3a-1: Secondary Visibility Fast-path helper 已实现，`mysqld` build、targeted MTR、完整 `parallel_query` suite 74 项通过；code/task Review Agent 返回 `APPROVE`
  - M9-B3a-2: clustered lookup for visibility helper 已实现；`mysqld` build、targeted MTR、完整 `parallel_query` suite 74 项通过；code/task Review Agent 返回 `APPROVE`
  - M9-B3a-3: one-record secondary visibility smoke 已实现；`mysqld` build、targeted MTR、完整 `parallel_query` suite 74 项通过；code/task Review Agent 返回 `APPROVE`
  - M9-B3b: debug-only covering secondary one-record materialization smoke 已完成；Code Review Agent 两轮 `REVISE` 后最终返回 `ACCEPT`；SQL covering gate、generated/hidden 字段拒绝、InnoDB `m_prebuilt` template 状态恢复和 ICP-off DBUG 负例已落地；`mysqld` build、targeted MTR、完整 `parallel_query` suite 74 项通过
  - M9-B3c: debug-only covering secondary range multi-row materialization smoke 已完成；B3c-0 contract Review Agent 返回 `ACCEPT`；code/task Review Agent 返回 `ACCEPT`；实现采用单 mtr、不 restart/无 bookmark、fast-path-only、逐记录 offsets、错误全量 fail-closed；`mysqld` build、targeted MTR、完整 `parallel_query` suite 74 项通过
  - M9-B3d: 用户可见 covering secondary range gate 已实现；只允许 strict covering integer secondary forward range；运行时 `HA_ERR_UNSUPPORTED` 回退串行 `IndexRangeScanIterator`；`SELECT k ... WHERE k >= 20 AND k < 40` 返回 3 行并使 `Parallel_secondary_rows_produced` 增长 3；初审发现 composite child hook 和 unsafe keypart 两个问题，已修复为 root/FILTER-only gate 和 whole-keypart safety gate；`mysqld` build、targeted MTR、完整 `parallel_query` suite 74 项通过；Review Agent 最终复核 `ACCEPT`
  - M9-B3 commit: `233e8f039df Add PQ M9B3 secondary range row production gate`
  - M9-C0: Constant JT_REF minimal design taskbook 已创建并提交；C1 先做 debug-only ref equality endpoint bridge，C2 再打开用户可见 constant covering ref gate；dependent ref、ICP、non-covering、multi-table、worker/MQ 后置
  - M9-C1: debug-only constant covering secondary ref smoke 已实现；验证 duplicate/empty/last-key equality endpoint；`c1_ref_materialized_smoke_delta=3`；不增长 `Parallel_queries_executed` 或 `Parallel_secondary_rows_produced`；Review Agent `ACCEPT`
  - M9-C1 commit: `faca19c680f Add PQ M9C ref equality smoke`
  - M9-C2: user-visible constant covering ref gate 已实现；`c2_ref_rows_produced_delta=2`；总 runtime `executed_delta=3`、`secondary_rows_produced_runtime_delta=5`、`workers_delta=0`；完整 `parallel_query` suite 74/74 通过；Review Agent `ACCEPT`
  - M9-C2 commit: `e65817cc693 Add PQ M9C covering ref gate`
  - M9-D0: dependent ref / per-ref-key dispatch design taskbook created；首轮 Design Review `CHANGES REQUESTED`，补强 per-statement/per-probe counter、repeated outer key、per-probe fallback、D2 forbidden scope 后复审 `ACCEPT`
  - M9-D3d: user-visible leader-local dependent ref gate 已完成；只允许 two-table/simple/no group/having/no reverse/covering secondary dependent ref；`d3d_ref_probe_attempts_delta=5`、`d3d_ref_empty_probes_delta=1`、`d3d_ref_rows_produced_delta=7`、`d3d_executed_delta=1`；`workers/ranges=0`；新增 serial baseline / unsorted D3d order check 和 fallback-after-buffer debug MTR；`mysqld` build、targeted record/replay、完整 `parallel_query` suite 74/74 通过；Review Agent 复审 `ACCEPT`。
  - M9-D3d commit: `6944472cb7e Add PQ M9D dependent ref leader gate`
  - M9-E: ICP Pushdown taskbook 已创建并通过 Design Review；M9-E0 ICP negative guard 已完成编码和验证：secondary range ICP `EXPLAIN` 稳定显示 `Using index condition`，constant ref / dependent ref 保留 adjacent boundary guard，negative window `executed/workers/ranges/secondary_rows = 0`；`mysqld` build、targeted record/replay、完整 `parallel_query` suite 74/74 通过；Code/Task Review Agent 首轮 `REVISE`，修正文档残留后复审 `ACCEPT`；M9-E1a 两个只读 Explorer 均建议先做 leader-local ICP contract/blocking design，不直接编码，Design Review Agent 返回 `ACCEPT`；M9-E1b coding taskbook 已通过 review，但 covering `k_v_idx` / `k_pad_idx` 候选均只产生 `Using where; Using index`，没有 stable strict-covering `Using index condition` 正例；源码探测改动已移除；M9-E1c explorer 建议下一步做 non-covering ICP + clustered lookup contract design，不直接编码，Design Review Agent 返回 `ACCEPT`；M9-E1c-0 detailed contract 通过 Design Review；M9-E1c-1 debug-only one-record smoke 已完成；M9-E1c-2a user-visible non-covering ICP range gate 已完成并通过 Code/Task Review；完整 `parallel_query` suite 74/74 通过；M9-E2 constant covering ref ICP 设计任务书已通过 Design Review；M9-E2-0 access-shape read-only confirmation 已完成，覆盖 ref 候选无法稳定产生 `Using index condition`，非覆盖 ref 才能产生 `type=ref` + `Using index condition`，因此 E2-1/E2-2 编码 blocked。
- M11-F subline current update: M11-F6b-0 source inventory / contract
  confirmation 已提交；M11-F6b-1 local constant-ref token transport helper 已
  提交；M11-F6b-2 constant-ref context to token smoke 已完成、review accepted、
  提交。F6b-2 只把 F6a-owned constant-ref key bytes 接到 F6b-1 private
  token helper，仍不把 constant-ref key bytes 当作 handler row-id /
  stable-ref。M11-F6c user-visible migration decision 已完成 docs-only 决策并
  通过独立 code/docs/test review；结论是暂不把 M9-C2 leader-local visible
  ref gate 迁移到 worker-side visible execution，下一步先做 F6d / F6c-1
  worker TABLE / handler source inventory and contract，并明确区分 typed
  `pq_worker_scan_next(PQ_Worker_context*, ...)` 与 commercial
  `pq_worker_scan_next(void*, ...)` / `handler::ha_pq_next()` 路径。M11-F6d
  worker-side ref execution inventory / contract 已完成 docs-only 任务书并通过
  独立 review；建议后续先做 F6d-1 no-row worker TABLE / handler
  contract smoke，不直接打开 visible worker-side ref row path。M11-F6d-1
  no-row worker TABLE / handler contract smoke 已完成本地编码和 targeted
  验证，并通过独立 review；该 smoke 不调用 typed `pq_worker_scan_init()`，不
  dispatch range，不生产 row。后续进入 F6d-2 / F6e minimal positive
  worker-side constant covering ref row path 设计拆分。继续禁止在当前
  F5/F6a/F6b/F6c/F6d/F6d-1
  closure 后直接打开
  `PQRefIterator::Read()`、`PQblockScanIterator::Read()`、
  `pq_worker_scan_next()`、worker MQ row production、worker-side ICP + native
  `Record_buffer`、MVI unique filter、partition、reverse、secondary MIN
  positive path 和 M11-E visible ORDER BY positive path。
- Commercial port taskbooks:
  - [commercial-port-m3-plan-clone-resolver.md](commercial-port-m3-plan-clone-resolver.md)
  - [commercial-port-m4-worker-result-path.md](commercial-port-m4-worker-result-path.md)
  - [commercial-port-m5-innodb-path-alignment.md](commercial-port-m5-innodb-path-alignment.md)
  - [commercial-port-m6-fullscan-execution-gate.md](commercial-port-m6-fullscan-execution-gate.md)
  - [commercial-port-m7-aggregation-reconciliation.md](commercial-port-m7-aggregation-reconciliation.md)
  - [commercial-port-m8-order-by-gather-merge.md](commercial-port-m8-order-by-gather-merge.md)
  - [commercial-port-m9-ref-icp.md](commercial-port-m9-ref-icp.md)
  - [commercial-port-m11-ref-icp-worker-path.md](commercial-port-m11-ref-icp-worker-path.md)
  - [m9-d-dependent-ref-design.md](m9-d-dependent-ref-design.md)
  - [m9-b2-secondary-range-partition.md](m9-b2-secondary-range-partition.md)
  - [m9-b3-secondary-range-row-production.md](m9-b3-secondary-range-row-production.md)
  - [m9-c-jt-ref-minimal.md](m9-c-jt-ref-minimal.md)
  - [m9-d3-dependent-ref-gate-plan.md](m9-d3-dependent-ref-gate-plan.md)
  - [m9-e-icp-pushdown.md](m9-e-icp-pushdown.md)
  - [commercial-port-m9-f-edge-cases.md](commercial-port-m9-f-edge-cases.md)
  - [commercial-port-m10-test-suite-gap-closure.md](commercial-port-m10-test-suite-gap-closure.md)
  - [commercial-port-m11-main-architecture-restart.md](commercial-port-m11-main-architecture-restart.md)
  - [commercial-port-m11-plan-clone-resolver.md](commercial-port-m11-plan-clone-resolver.md)
  - [commercial-port-m11-worker-result-path.md](commercial-port-m11-worker-result-path.md)
  - [commercial-port-m11-order-by-exchange-sort.md](commercial-port-m11-order-by-exchange-sort.md)
- Next risk closure board: [v2-next-risk-closure.md](v2-next-risk-closure.md)
- Commercial port board: [commercial-port-gap-analysis.md](commercial-port-gap-analysis.md)
- Parallel-ready task overview: [parallel_wave2_tasks.md](parallel_wave2_tasks.md)
- Remaining risk: 基础 implicit aggregate 已可通过 DOP=1/DOP=2/DOP=4 threaded row stream 由上层 MySQL 聚合算子消费；DOP1 GROUP BY typed-state temp-table 输出已覆盖 COUNT/SUM/MIN/MAX；locking read EXPLAIN fallback、DOP2 read-view concurrency、aggregate fallback read-view cleanup、DOP4 correctness/hardening/no-debug gate 和性能基线材料已覆盖；release/目标环境性能采样、DOP>1 GROUP BY worker partial merge、ORDER BY、secondary index/ICP、partition table 仍未完成。

## Prepared Claude Code Worktrees

Baseline patch applied:

```text
/private/tmp/pq-phase0-1-baseline.patch
```

Worktrees:

| Task | Branch | Path | Prompt source |
|------|--------|------|---------------|
| Phase 2 - Handler/InnoDB contract | `claude-phase2` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase2` | [phase2-handler-innodb-contract.md](phase2-handler-innodb-contract.md) |
| Phase 3 - MQueue/Exchange MVP | `claude-phase3` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase3` | [phase3-mqueue-exchange.md](phase3-mqueue-exchange.md) |
| Test suite classification | `claude-test-suite` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-test-suite` | [parallel_query_suite_manifest.md](parallel_query_suite_manifest.md) |
| Phase 6B-2 - InnoDB pull-row adapter | `claude-phase6b2-innodb-pull-adapter` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase6b2-innodb-pull-adapter` | [phase6b2-innodb-pull-adapter.md](phase6b2-innodb-pull-adapter.md) |
| Test manifest v2 fix | `claude-test-manifest-v2` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-test-manifest-v2` | [test-manifest-v2-fix.md](test-manifest-v2-fix.md) |

## Second Wave

Recommended worktrees:

| Task | Branch | Path | Prompt source |
|------|--------|------|---------------|
| W2-A - Phase 4 worker lifecycle | `claude-phase4-worker` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase4-worker` | [phase4-worker-lifecycle.md](phase4-worker-lifecycle.md) |
| W2-B - Phase 5 optimizer hook | `claude-phase5-optimizer-hook` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase5-optimizer-hook` | [phase5-optimizer-hook.md](phase5-optimizer-hook.md) |
| W2-C - Phase 6 InnoDB/read-view design | `claude-phase6-innodb-design` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase6-innodb-design` | [phase6-innodb-readview-design.md](phase6-innodb-readview-design.md) |
| W2-D - test manifest v2 fix | `claude-test-manifest-v2` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-test-manifest-v2` | [test-manifest-v2-fix.md](test-manifest-v2-fix.md) |
| Phase 5B - SQL plan rewrite / iterator MVP | `claude-phase5b-plan-rewrite-iterator` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase5b-plan-rewrite-iterator` | [phase5b-plan-rewrite-iterator.md](phase5b-plan-rewrite-iterator.md) |

## Phase Status

| Phase | Status | Owner | Log | Notes |
|------|--------|-------|-----|-------|
| Phase A - V1-MVP interface contract | Completed | Design Agent Russell / Orchestrator | [phaseA-interface-contract.md](phaseA-interface-contract.md) | Hybrid InnoDB route accepted as working direction |
| Phase 0 - System variables and minimal context fields | Completed | Claude Code / Orchestrator | [phase0-system-vars.md](phase0-system-vars.md) | Commit `cb3253f97a8`; system variables and minimal context fields committed |
| Phase 1 - Conservative eligibility and fallback | Completed | Claude Code / Orchestrator | [phase1-eligibility-fallback.md](phase1-eligibility-fallback.md) | Main worktree build passed; new `sql/parallel_query/pq_optimizer.*` must be included in final patch/commit |
| Phase 2 - Handler/InnoDB contract and route validation | Completed | Claude Code / Orchestrator | [phase2-handler-innodb-contract.md](phase2-handler-innodb-contract.md) | v2 fixed `PQ_ref_key`; `mysqld` build passed |
| Phase 3 - MQueue and Exchange MVP | Completed | Claude Code / Orchestrator | [phase3-mqueue-exchange.md](phase3-mqueue-exchange.md) | `exchange.*` and `msg_queue.*`; `mysqld` build passed |
| Phase 4 - Worker lifecycle | Completed | Claude Code / Orchestrator | [phase4-worker-lifecycle.md](phase4-worker-lifecycle.md) | Commit `68bcdefebbb`; no optimizer/InnoDB changes |
| Phase 5 - Optimizer eligibility hook | Completed | Claude Code / Orchestrator | [phase5-optimizer-hook.md](phase5-optimizer-hook.md) | Commit `c2a2a537b64`; `mysqld` build passed |
| Phase 5B - Plan rewrite and iterator MVP | Completed | Claude Code / Orchestrator | [phase5b-plan-rewrite-iterator.md](phase5b-plan-rewrite-iterator.md) | Commit `af895e3edaf`; build passed; guarded SQL iterator hook remains serial-only in V1 |
| Phase 6 - InnoDB/read-view design | Completed | Codex Orchestrator fallback | [phase6-innodb-readview-design.md](phase6-innodb-readview-design.md) | Design-only; Claude Code dispatch stalled; no source changes |
| Phase 6B-1 - InnoDB PQ API skeleton | Completed | Codex Orchestrator fallback | [phase6b-innodb-fullscan.md](phase6b-innodb-fullscan.md) | Commit `f99f3aa7b5d`; `mysqld` build passed |
| Phase 6B-2 - InnoDB pull-row adapter | Completed | Claude Code / Orchestrator | [phase6b2-innodb-pull-adapter.md](phase6b2-innodb-pull-adapter.md) | Commit `f001e1631f7` (base) + `b0ac9740cfd` (review-fix); build passed |
| Phase 7 - EXPLAIN and MVP tests | Completed | Claude Code / Orchestrator | [phase7-explain-mvp-tests.md](phase7-explain-mvp-tests.md) | Commit `9af8fe777d2`; EXPLAIN annotation and MVP MTR committed |
| Phase 8 - V1-complete scaffolding | Completed | Claude Code + Codex Orchestrator fix | [phase8-v1-complete.md](phase8-v1-complete.md) | Commit `113d2ba44c1`; aggregation infrastructure, error path/stat counters, Phase 8 MTR, and macOS `Aligned_atomic` startup fix; full `parallel_query` suite passed |
| Phase 9 - parallel_query suite migration | Completed | Claude Code Test Agent + Codex Orchestrator review | [phase9-parallel-query-suite.md](phase9-parallel-query-suite.md), [parallel_query_suite_manifest.md](parallel_query_suite_manifest.md) | Commit `9c7e9aede42`; 参考测试套 97 个测试已分类；新增 3 个 V1 测试；完整 suite 通过 |
| V1 risk convergence | Completed | Codex Orchestrator + Review Agents | [v1-risk-convergence.md](v1-risk-convergence.md) | Commit `69ed0ac66e7`; EXPLAIN 文案收敛为 candidate/fallback-disabled；fallback counter 不被 EXPLAIN 污染；完整 suite 通过 |
| V2-0 - Execution State Contract | Completed | Codex Orchestrator | [v2-0-execution-state-contract.md](v2-0-execution-state-contract.md) | Commit `a420e8a3f26`; 定义 execution state contract；不启用真实 PQ iterator；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-1 - Iterator Ownership + Safe Fallback | Completed | Codex Orchestrator | [v2-1-iterator-safe-fallback.md](v2-1-iterator-safe-fallback.md) | Commit `9a58ff94cdf`; 允许 eligible execution 返回 PQ iterator，并在 Init 安全窗口串行 fallback；完整 suite 通过 |
| V2-2 - Handler/InnoDB Context Bridge | Completed | Codex Orchestrator + parallel explorers | [v2-2-handler-innodb-context-bridge.md](v2-2-handler-innodb-context-bridge.md) | Commit `a123cdabe7c`; DOP=1 leader init/end bridge；不启动 worker；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-3 - Read View / Trx Contract | Completed | Codex Orchestrator + parallel explorers | [v2-3-read-view-trx-contract.md](v2-3-read-view-trx-contract.md) | Commit `54bd78429cf`; leader probe 不再提前分配 read view；worker row read 明确 unsupported；新增 `pq_read_view_dop1`；完整 suite 通过 |
| V2-4 - Parallel_reader Thin Adapter + Range Partition | Completed | Codex Orchestrator + parallel explorers | [v2-4-parallel-reader-range-partition.md](v2-4-parallel-reader-range-partition.md) | Commit `0b7f7485a7b` + `79d1355cf2f`；`Parallel_reader::export_scan_ranges()`、`Parallel_ranges_built` 和 `pq_range_planning_dop`；完整 suite 通过 |
| V2-5 - Worker THD + Minimal Worker Scan | Completed | Codex Orchestrator + parallel explorers | [v2-5-worker-thd-minimal-scan.md](v2-5-worker-thd-minimal-scan.md) | Commit `1e8078544e8`; Worker lifecycle smoke 已接入；不打开真实 row stream；完整 suite 通过 |
| V2-6 - Exchange/Gather Row Stream | Completed | Codex Orchestrator + parallel explorers | [v2-6-exchange-gather-row-stream.md](v2-6-exchange-gather-row-stream.md) | Commit `d0b4988a584`; Synthetic MQ ROW/FINISH stream smoke 已接入；不进入 `Read()`；完整 suite 通过 |
| V2-7 - Predicate/Projection Boundary | Completed | Codex Orchestrator | [v2-7-predicate-projection-boundary.md](v2-7-predicate-projection-boundary.md) | Commit `1d39b510348`; Boundary MTR 已完成；不打开真实 worker row；完整 suite 通过 |
| V2-8 - Single Table Full Scan Closure | In Progress | Codex Orchestrator | [v2-8-single-table-fullscan-closure.md](v2-8-single-table-fullscan-closure.md) | 当前任务拆分：真实 row materialization / `Read()` 接管前的硬 gate |
| V2-8A - Worker Handler/Prebuilt Contract Design | Completed | Codex Orchestrator + Design Explorers | [v2-8a-worker-handler-prebuilt-contract.md](v2-8a-worker-handler-prebuilt-contract.md) | Commit `8dc30b6a1d2`; 完成 SQL/handler、InnoDB read-view/prebuilt、range dispatch 三项设计收敛；后续先做 V2-8B Row Image Protocol |
| V2-8B - Row Image Protocol | Completed | Codex Orchestrator + Design Explorers | [v2-8b-row-image-protocol.md](v2-8b-row-image-protocol.md) | typed MQ header、fixed record image synthetic materialization 已完成；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8C - DOP=1 Real Full Scan | Contract/Gate Implemented | Codex Orchestrator + Design Explorers | [v2-8c-dop1-real-fullscan.md](v2-8c-dop1-real-fullscan.md) | Commits `24209d09ce3`, `6638e36def7`, `36b2d9a764c`, `09033e1c2a0`, `9c242c61cc7`, `2dab52a2d9c`, `a79c2109d7a`, `13ce85e4d47`; 已落地 `PQ_Worker_open_context`、typed worker wrapper、leader PROBE/EXECUTE mode API、worker_info carrier、worker THD/TABLE helpers、safe-window open-table smoke、handler init/end smoke、DOP=1/blob/independent record gates；真实 row scan 仍未打开 |
| V2-8D - First Row / Read View Contract | Design Selected | Codex Orchestrator + Explorer Agents | [v2-8d-first-row-read-view-contract.md](v2-8d-first-row-read-view-contract.md) | 已确认 `index_first()` 等价首行参数为 `PAGE_CUR_G + match_mode 0`；真实读取选择 `Parallel_reader` visibility adapter 路线，避免 worker-local `row_search_mvcc()` 创建独立 read view；真实 row read 继续 disabled，直到 EXECUTE commit point 和 pull adapter 完成 |
| V2-8E - EXECUTE Commit Point | Completed | Codex Orchestrator | [v2-8e-execute-commit-point.md](v2-8e-execute-commit-point.md) | InnoDB leader `EXECUTE` mode 已能在 leader 线程绑定 active read view；当前 SQL iterator 仍只调用 `PROBE`，不打开真实 row stream；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8F - Parallel_reader Pull Adapter | Callback conversion smoke primitive completed | Codex Orchestrator | [v2-8f-parallel-reader-pull-adapter.md](v2-8f-parallel-reader-pull-adapter.md) | `InnoDB_pq_scan_ctx::smoke_callback_conversion()` 已实现，可用同步 `Parallel_reader` 转换最多一行；先不接 SQL `Read()`，不打开真实 row stream；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8G - EXECUTE Callback Smoke | Completed | Codex Orchestrator | [v2-8g-execute-callback-smoke.md](v2-8g-execute-callback-smoke.md) | SQL iterator safe fallback window 内尝试固定 DOP=1 EXECUTE callback smoke；新增 attempts/rows 状态变量；不打开真实 row stream；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8H - Row Stream Activation Boundary | Completed | Codex Orchestrator | [v2-8h-row-stream-activation.md](v2-8h-row-stream-activation.md) | 抽出 Exchange row/eof/error materialization helper；不接真实 `Read()`；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8I - Iterator Runtime State Contract | Completed | Codex Orchestrator | [v2-8i-iterator-runtime-state.md](v2-8i-iterator-runtime-state.md) | 显式记录 safe-fallback / started / row-returned 边界；不接真实 `Read()`；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8J - Real Read Activation Plan | In Progress | Codex Orchestrator | [v2-8j-real-read-activation-plan.md](v2-8j-real-read-activation-plan.md) | 已完成 callback row producer smoke、worker producer FINISH/EOF/ERROR/abort skeleton、`Read()` shadow scaffold、PROBE diagnostics、materialize status helper、ROW-only enqueue helper、callback multi-row producer API、SQL 层 limited multi-row producer smoke、`Read()` wait/kill policy、PROBE unsupported 细分诊断、首次 PROBE gate 修复、debug shadow first-row MTR 和 debug shadow 2-row/EOF producer；下一步评估真实 DOP=1 full scan gate |
| V2-8K - Worker Thread Producer | V2-8K-3 Completed | Codex Orchestrator + Explorer | [v2-8k-worker-thread-producer.md](v2-8k-worker-thread-producer.md) | 已完成 joinable worker thread scaffold、debug-only threaded callback producer、worker ERROR token、abort-after-start cleanup 和 EOF join 验证；默认真实 DOP=1 full scan 仍未启用 |
| V2-8L - Threaded Semantics Boundary | Completed | Codex Orchestrator | [v2-8l-threaded-semantics-boundary.md](v2-8l-threaded-semantics-boundary.md) | 已完成 worker read_set/prebuilt lifecycle、projection/WHERE/empty EOF、EOF-before-first-row executed counter；完整 suite 24 个测试通过 |
| V2-8M - Experimental Threaded Gate | Completed | Codex Orchestrator | [v2-8m-experimental-threaded-gate.md](v2-8m-experimental-threaded-gate.md) | 新增默认 OFF 的 `parallel_query_experimental_threaded_dop1`；无 debug 下可显式启用 DOP=1 threaded full scan；完整 suite 25 个测试通过 |
| V2-8N - Threaded External KILL | Completed | Codex Orchestrator + Explorer | [v2-8n-threaded-external-kill.md](v2-8n-threaded-external-kill.md) | 新增 worker-started debug sync 点和 external `KILL QUERY` MTR，覆盖 worker 已启动后的 abort/join/cleanup |
| V2-8O - Threaded Aggregate Coverage | Completed | Codex Orchestrator + Explorer | [v2-8o-threaded-aggregate-coverage.md](v2-8o-threaded-aggregate-coverage.md) | 新增 no-debug experimental DOP=1 聚合 MTR，验证 row stream 可由上层聚合算子自然消费；不接 partial aggregation |
| V2-8P - DOP Multi-Queue Exchange Smoke | Completed | Codex Orchestrator | [v2-8p-dop-multiqueue-exchange-smoke.md](v2-8p-dop-multiqueue-exchange-smoke.md) | 新增 row-image smoke 专用 status counters，精确验证 DOP=1/2/4 synthetic ROW/FINISH 多队列消费；不打开 InnoDB DOP>1 |
| V2-8Q - DOP2 Experimental Guard | Completed | Codex Orchestrator | [v2-8q-dop2-experimental-guard.md](v2-8q-dop2-experimental-guard.md) | 新增 no-debug guard MTR，验证 experimental threaded path 只允许 DOP=1，DOP=2 仍 serial fallback |
| V2-9 - DOP Range Correctness | In Progress | Codex Orchestrator + Explorer | [v2-9-dop-range-correctness.md](v2-9-dop-range-correctness.md) | 已拆分 V2-9A/B/C/D；先做 range dispatch contract，再做 range-aware callback producer |
| V2-9A - Range Dispatch Contract | Completed | Codex Orchestrator | [v2-9a-range-dispatch-contract.md](v2-9a-range-dispatch-contract.md) | 新增 range dispatched/empty worker counters，`pq_worker_scan_init()` 可观测领取 assigned range；DOP>1 gate 仍关闭 |
| V2-9B - Range-Aware Callback Producer | Completed | Codex Orchestrator | [v2-9b-range-aware-callback-producer.md](v2-9b-range-aware-callback-producer.md) | handler 已接入 assigned range；修复 exported boundary `n_fields_cmp` 过大导致的 InnoDB debug assertion；DOP>1 gate 仍关闭 |
| V2-9C - Debug DOP2 Threaded Shadow | Completed | Codex Orchestrator | [v2-9c-debug-dop2-threaded-shadow.md](v2-9c-debug-dop2-threaded-shadow.md) | 新增 debug-only DOP=2 threaded shadow path；多 worker 启动、empty worker FINISH、Exchange 消费、小表结果正确性、worker ERROR 和 external KILL 已覆盖 |
| V2-9D - Multi-range Callback Drain | Completed | Codex Orchestrator | [v2-9d-multirange-callback-drain.md](v2-9d-multirange-callback-drain.md) | worker 完成当前 range 后继续领取后续 range；DOP=2 debug shadow 验证 1024 行完整集合、`rows_delta=1024`、`ranges_dispatched>=2` |
| V2-9E - Experimental DOP2 Gate | Completed | Codex Orchestrator | [v2-9e-experimental-dop2-gate.md](v2-9e-experimental-dop2-gate.md) | 新增默认 OFF 的 `parallel_query_experimental_threaded_dop`；无 debug 下只打开 DOP=2 threaded full scan，并覆盖基础聚合和 DOP1/DOP4 负向 gate；完整 suite 36 项通过 |
| V2 next P0-A - Locking Read EXPLAIN Fix | Completed | Codex Orchestrator | [v2-next-risk-closure.md](v2-next-risk-closure.md) | `EXPLAIN SELECT ... FOR UPDATE/SHARE` 稳定显示 `Not parallel LOCKING_READ`；真实执行不污染 fallback/executed counters；完整 suite 37 项通过 |
| V2 next P0-B - DOP2 Read-view Concurrency | Completed | Codex Orchestrator | [v2-next-risk-closure.md](v2-next-risk-closure.md) | no-debug DOP2 threaded row stream 覆盖 RR/RC 并发 insert/update/delete；完整 suite 38 项通过 |
| V2 next P0-C - DOP4 Correctness/Hardening | Completed | Codex Orchestrator | [v2-next-risk-closure.md](v2-next-risk-closure.md) | debug-only DOP4 多 range 聚合、worker ERROR、external KILL 和 no-debug DOP4 experimental gate 通过；完整 suite 43 项通过 |
| V2 next P0-D - Performance Baseline | Materials Completed | Codex Orchestrator + Explorer Agents | [v2-p0d-performance-baseline.md](v2-p0d-performance-baseline.md), [v2-next-risk-closure.md](v2-next-risk-closure.md) | 新增 SQL baseline script 和 counter contract MTR；完整 suite 44 项通过；release/目标环境性能采样待执行 |
| V2 next P1-E - Aggregate Fallback Read-view Cleanup | Completed | Codex Orchestrator | [v2-next-risk-closure.md](v2-next-risk-closure.md) | PROBE/EXECUTE 绑定 active read view，autocommit 下关闭 PQ-owned read view 并恢复 `sql_stat_start`；新增 aggregate fallback 后 DOP2 threaded LIMIT 回归；完整 suite 54 项通过 |
| V2-11A/B/C/D/E - Regression Expansion | Completed | Codex Orchestrator + Worker Agents | [v2-11-regression-expansion.md](v2-11-regression-expansion.md) | 新增 prepare state、row image datatypes、LIMIT/counter、large external KILL、MDL concurrency 和 experimental vars noop 回归 |
| V2-12A - GROUP BY Partial Aggregation Design | Completed | Codex Orchestrator + Explorer Agent | [v2-12a-groupby-partial-aggregation-design.md](v2-12a-groupby-partial-aggregation-design.md) | 结论：worker partial aggregate state + leader merge；V2-12A-1 gate/diagnostics 已完成 |
| V2-12A-1 - GROUP BY Gate And Diagnostics | Completed | Codex Orchestrator | [v2-12a-groupby-partial-aggregation-design.md](v2-12a-groupby-partial-aggregation-design.md) | GROUP BY fallback reason 已细分；新增 `pq_groupby_diagnostics`；完整 suite 48 项通过 |
| V2-12A-2 - GROUP BY Wire Protocol Smoke | Completed | Codex Orchestrator | [v2-12a-groupby-partial-aggregation-design.md](v2-12a-groupby-partial-aggregation-design.md) | 新增 `PARTIAL_GROUP` typed MQ message 和 synthetic smoke；完整 suite 49 项通过 |
| V2-12A-3 - DOP1 Partial Group Execution | V2-12A-3.11 SUM Typed-State Completed | Codex Orchestrator + Explorer Agent | [v2-12a-3-dop1-partial-group-execution.md](v2-12a-3-dop1-partial-group-execution.md) | PQ wrapper 已在 supported shape 下接管 temp-table aggregate iterator ownership，并由 PQ typed-state 写 output temp table；当前支持单整数 COUNT/SUM/MIN/MAX typed path，并覆盖主要 unsupported/fallback shape；DOP>1 worker partial merge 仍待实现；完整 suite 58 项通过 |
| V2-12B - DOP Partial Group Merge | DOP2/DOP4 COUNT/SUM/MIN/MAX Result Path Completed | Codex Orchestrator + Explorer Agents | [v2-12b-dop-partial-group-merge.md](v2-12b-dop-partial-group-merge.md) | 已完成 DOP partial counters、payload v1 schema smoke、malformed payload smoke、leader in-memory merge helper、worker local partial producer smoke、DOP2/DOP4 COUNT/SUM/MIN/MAX、COUNT(*) 和 COUNT(nullable field) temp-table result path、unsupported shape fallback、worker error 和 external kill 覆盖；显式 experimental gate；完整 suite 69 项通过 |
| V2-12C - GROUP BY Shape Expansion | Design Created | Codex Orchestrator | [v2-12c-groupby-shape-expansion.md](v2-12c-groupby-shape-expansion.md) | 已完成复杂 GROUP BY shape 优先级拆分；推荐先做 multi aggregate contract，再评估 ORDER BY same group key；本阶段不改源码 |
| V2-12C-1 - Multi Aggregate Contract | Completed | Codex Orchestrator | [v2-12c-1-multi-aggregate-contract.md](v2-12c-1-multi-aggregate-contract.md) | 只读调研确认首段复用 payload v1，新增内部 aggregate descriptor array，V2-12C-2 先做两个 aggregate 的 DOP2/DOP4 result path |

## Decisions

- Phase A working decision: use a hybrid InnoDB route. Reuse upstream `Parallel_reader` where possible and add only a thin adapter for SQL PQ gaps.
- Do not directly apply the reference `row0pread_pq.*` implementation as a full fork.
- MVP eligibility must be conservative and fallback serial on uncertainty.
- OOM before worker start may fallback; OOM after worker start is fatal.

## Open Decisions

- release/目标环境性能采样、GROUP BY partial aggregation、ORDER BY、复杂 join、二级索引/ICP 等仍需后续阶段拆分。

## How To Update

- Update this board whenever a phase starts, completes, is blocked, or changes owner.
- Keep detailed logs in the phase-specific files.
- Do not store full chat transcripts here; store only structured status and decisions.
- Use Chinese by default for phase logs and this board. Keep English terms when they are code identifiers, commands, MySQL concepts, or clearer than a translated phrase.
