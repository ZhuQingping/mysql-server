# MySQL Server — AI Agent Context

本文档为在本仓库中进行特性开发、代码阅读与重构的 AI 助手提供统一上下文。请优先遵守此处的约定与路径说明。

## 项目概览

- **代码库**: MySQL Server 源码（Oracle MySQL 8.0.41 LTS）
- **语言**: C/C++（C++17），构建系统为 CMake
- **用途**: 数据库服务端核心（SQL 解析、优化器、执行器、存储引擎接口及 InnoDB 等）

## 构建与运行环境

- **源码根目录**: `/Users/zhuqingping/Work/Database/MySQL/mysql-server`（本仓库，与 `AGENTS.md` 所在目录一致）
- **构建规范来源**: 以 `/Users/zhuqingping/Work/Database/MySQL/mysql-8.0.41/compile_source_code.sh` 为准；下文与脚本保持一致，若脚本更新请先同步文档或仍以脚本为准。
- **构建目录**: 源码根目录下的 `build-ninja/`（脚本在配置前会 `cd` 到该目录；需要完全重配时可对 `build-ninja/` 内容执行清理后再 `cmake`）
- **安装前缀**（脚本中 `CMAKE_INSTALL_PREFIX`）: `/Users/zhuqingping/Work/DataBase/MySQL/mysql-8.0.41/install`（执行 `ninja install` 时使用）
- **依赖（与脚本一致）**:
  - **Boost**: `/Users/zhuqingping/Work/DataBase/MySQL/boost/boost_1_77_0`（`WITH_BOOST`）
  - **Bison**: `/opt/homebrew/opt/bison/bin/bison`（`BISON_EXECUTABLE`）
  - **OpenSSL**（可选）: 脚本顶部将相关 `cmake` 选项注释掉了；若需链接本机 OpenSSL，可参考该脚本中的注释块自行取消注释并设置 `OPENSSL_PATH` 等变量

### 编译类型说明

- 当前脚本使用 `-DCMAKE_BUILD_TYPE=debug`，便于调试。
- 若日常迭代希望更快或更小二进制，可将 `CMAKE_BUILD_TYPE` 改为 `RelWithDebInfo`（需重新 `cmake`）；是否 `ninja install` 按你是否需要安装树而定。

### 编译命令（与 compile_source_code.sh 一致）

在终端执行（或运行上述脚本）:

```bash
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server
mkdir -p build-ninja
cd build-ninja
# 可选：干净重配
# rm -rf ./*

cmake -G Ninja .. \
  -DWITH_BOOST=/Users/zhuqingping/Work/DataBase/MySQL/boost/boost_1_77_0 \
  -DBISON_EXECUTABLE=/opt/homebrew/opt/bison/bin/bison \
  -DCMAKE_INSTALL_PREFIX=/Users/zhuqingping/Work/DataBase/MySQL/mysql-8.0.41/install \
  -DCMAKE_BUILD_TYPE=debug \
  -DWITH_UNIT_TESTS=0 \
  -DWITH_ASAN=0

ninja -j 16
ninja install -j 16
```

- **增量编译**: 在 `build-ninja/` 目录下仅改服务端时，常用 `ninja mysqld`（或 `ninja -j 16` 增量构建目标）。**不要在源码根目录或其他生成器目录（如历史遗留的 Unix Makefiles 目录）里 `make`**——与文档约定不符且慢 100 倍以上。

## 测试

- **测试框架**: MySQL Test Run（mtr），入口为 **build 目录下** 的 `mysql-test/mysql-test-run.pl`
- **运行方式**: 在 **build-ninja 目录** 下执行，例如：
  ```bash
  cd /Users/zhuqingping/Work/Database/MySQL/mysql-server/build-ninja/mysql-test
  ./mtr 1st
  # 或 perl ./mysql-test-run.pl 1st
  ```
  不要从源码根目录的 `mysql-test/` 直接跑指向 build 外的二进制，以免用到错误或未更新的可执行文件。
- **快速验证**: 用例 `1st`（即 main.1st）用于确认构建与基础功能正常。

## 关键目录（特性开发时常用）

| 目录/文件 | 说明 |
|-----------|------|
| `sql/` | 解析、优化器、执行器、Item 等（如 `item.cc`/`item.h`、`sql_select.cc`、`sql_optimizer.cc`） |
| `sql/range_optimizer/` | 范围扫描、索引选择、ICP 相关 range 逻辑 |
| `sql/handler.cc` | 存储引擎接口；ICP 比较逻辑（如 `compare_key_icp`） |
| `storage/innobase/` | InnoDB 引擎（`handler/ha_innodb.cc`、`row/row0sel.cc` 等） |
| `mysql-test/` | mtr 用例（t/*.test, r/*.result） |
| `Docs/README.build` | 官方构建说明 |

## 与助手协作的约定

- **自然语言**: 用户与助手对话可使用中文；代码、注释、提交信息等保持与现有代码风格一致（一般为英文）。
- **修改范围**: 做特性或 bug 修复时，优先只改必要文件；涉及优化器/执行路径时注意是否影响 EXPLAIN、ICP、索引选择等。
- **验证**: 修改后应先能成功编译，再跑至少一个相关 mtr 用例（如 `1st` 或你添加的用例）确认无回归。

## 参考

- 更细的构建/测试步骤与「如何让大模型更好理解本仓库」见 **CLAUDE.md**。
- 版本号以仓库根目录下 **MYSQL_VERSION** 为准（当前 8.0.41）。
