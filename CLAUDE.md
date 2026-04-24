# MySQL Server — Claude 特性开发指南

本文档面向在 Cursor 中使用 Claude（或同类大模型）进行 **MySQL Server 特性开发** 的场景，用于补充 AGENTS.md，帮助模型更准确理解代码结构、构建与验证流程，以及常见改造点。

---

## 1. 使用前请先确认

- 你正在 **MySQL 8.0.41** 服务端源码仓库中做 **服务端特性开发**（例如优化器、执行器、ICP、索引、InnoDB 等）。
- **本机源码路径**: `/Users/zhuqingping/Work/Database/MySQL/mysql-server`。
- 构建与测试在本机完成；**完整 cmake/ninja 命令与依赖路径以 `AGENTS.md` 及参考脚本 `/Users/zhuqingping/Work/Database/MySQL/mysql-8.0.41/compile_source_code.sh` 为准**（脚本与文档冲突时以脚本为准）。

---

## 2. 代码结构与“从哪里改起”

### 2.1 按功能分层

- **SQL 层（server 层）**  
  - 解析、语义、优化、执行入口：`sql/` 下（如 `sql_select.cc`、`sql_optimizer.cc`、`sql_parse.cc`）。  
  - Item 系统（表达式、条件、函数）：`sql/item.cc`、`sql/item.h`。  
  - 范围优化、索引选择、ICP 在优化器中的决策：`sql/range_optimizer/`、`sql/sql_select.cc`、`sql/sql_optimizer.cc`。

- **存储引擎接口**  
  - 通用 handler 接口、ICP 的“比较当前 key 是否在 range 内”：`sql/handler.cc`（如 `compare_key_icp`、`end_range`、`range_scan_direction`）。  
  - 引擎对 ICP 的支持声明：各引擎的 `index_flags()`（如 InnoDB 的 `HA_DO_INDEX_COND_PUSHDOWN`）。

- **InnoDB 引擎**  
  - 接口与 flag：`storage/innobase/handler/ha_innodb.cc`。  
  - 索引扫描与 ICP 检查：`storage/innobase/row/row0sel.cc`（如 `row_search_idx_cond_check`、ICP 与 `end_range` 的配合）。  
  - 反向扫描 + ICP 的 range 边界：`sql/range_optimizer/reverse_index_range_scan.cc`。

当你说“改 ICP”“改范围扫描”“加一个优化”时，模型应优先在上述目录中定位，而不是盲目搜索整个仓库。

### 2.2 常见“特性开发”入口

- **加/改优化器逻辑**：先看 `sql/sql_optimizer.cc`、`sql/range_optimizer/`、`sql/join_optimizer/`（若用 hypergraph）。  
- **改 Item/表达式**：`sql/item.cc`、`sql/item.h`，以及可能引用它们的 `sql_select.cc`、条件推导等。  
- **改 ICP 或 range 行为**：  
  - 优化器是否允许 ICP、是否取消 ICP：`sql/sql_select.cc`（`push_index_cond`）、`sql/sql_optimizer.cc`（如 reverse Ref 时取消 ICP）。  
  - 引擎层：`sql/handler.cc`（`compare_key_icp`）、InnoDB `row0sel.cc`、`ha_innodb.cc` 的 `index_flags`。  
- **加 mtr 用例**：在 `mysql-test/suite/main/t/` 下新增或修改 `.test`，在 `mysql-test/suite/main/r/` 下对应 `.result`（或使用 `--record` 生成）。

---

## 3. 构建与测试（便于模型给出可执行命令）

- **构建目录**：与 AGENTS.md 一致，使用  
  `/Users/zhuqingping/Work/Database/MySQL/mysql-server/build-ninja`（由 `compile_source_code.sh` 在源码根目录下创建并使用）。仓库中**没有**其他可用构建目录，禁止新建 `build/` 或在其他位置调用 `make`。  
- **配置与编译**：完整命令见 **AGENTS.md**；与之一致的参考脚本为  
  `/Users/zhuqingping/Work/Database/MySQL/mysql-8.0.41/compile_source_code.sh`（含 `WITH_BOOST`、`BISON_EXECUTABLE`、`CMAKE_INSTALL_PREFIX`、`ninja install` 等）。  
- **必须从 build-ninja 目录跑 mtr**：  
  ```bash
  cd /Users/zhuqingping/Work/Database/MySQL/mysql-server/build-ninja/mysql-test
  ./mtr [test_name]
  ```  
  例如 `./mtr main.1st`、`./mtr main.icp_cost_based`、`./mtr main.icp_cost_based --record`。不要用源码根目录下的 `mysql-test-run.pl` 指向 build 外的二进制。  
- **增量编译**：在 `build-ninja/` 下执行 `ninja mysqld -j 16`，只编 mysqld，秒级~分钟级完成；改 1~4 个 TU 通常 10 秒内结束。**不允许 `make`**——仓库生成器是 Ninja。  
- **OpenSSL**：当前参考脚本未在 cmake 行中启用 SSL；若 CMake 报找不到 OpenSSL，可参考 `compile_source_code.sh` 顶部注释块，按需取消注释并传入库路径（与旧版 AGENTS.md 中显式 `OPENSSL_*` 用法相同）。
- **mtr 脚本注意事项**：  
  - mysqltest 注释使用 `#`，避免裸 `--` 注释。  
  - main 自定义测试优先自包含（建表/插数/清理尽量写在用例内），减少 include 依赖导致的路径问题。  
  - Cursor agent 跑 mtr 时需使用完整终端权限（非受限沙箱），以避免与本机直接执行结果不一致。  

---

## 4. 如何描述你的诉求（给模型的提示）

为减少反复澄清，可在对话中尽量包含：

- **目标**：例如“希望 ICP 在反向 Ref 扫描时也能生效”“为某类条件增加下推”。  
- **范围**：只改优化器、只改 InnoDB、还是 handler 接口也要动。  
- **约束**：是否必须保持与现有 mtr 结果兼容、是否允许改 ABI/插件接口等。  
- **验证**：你打算用哪些用例或手写 SQL 验证（例如“跑 main.1st 和 suite X 的 Y.test”）。

---

## 5. 模型回复时的约定

- **中文回复**：除非你明确要求英文，否则用中文解释思路、结论与风险。  
- **代码与注释**：与现有代码风格一致；新加代码以英文注释为主，必要时可中英双语。  
- **修改建议**：尽量给出具体文件与函数/行号（或稳定关键字），便于你跳转；若改动涉及多个模块，先列出清单再分步改。  
- **测试**：每次给出修改建议时，顺带说明“编译后建议运行哪条 mtr 命令”以验证。

---

## 6. 与 AGENTS.md 的关系

- **AGENTS.md**：通用 agent 上下文（项目是什么、怎么编、怎么测、关键目录、协作约定）。  
- **CLAUDE.md**：面向 Claude/大模型做 **特性开发** 的补充——更细的代码分层、常见改造点、描述诉求的方式、回复与测试约定。

两者一起使用，可以让模型在“特性开发”场景下更快理解你的场景并给出可落地的修改建议与测试命令。

