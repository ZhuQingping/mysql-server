# MySQL-Server on Dstore

与 Dstore 存储引擎融合的 MySQL Server 层代码。本项目将 MySQL Server 与 Dstore 存储引擎对接，提供一个完整的数据库解决方案。

> **中文** | [English](README_EN.md)

---

## 项目结构

```
xxx/
├── mysql-server/               # MySQL Server 层代码
│   ├── BUILD/                  # 构建脚本目录
│   ├── storage/dstore/         # MySQL 与 Dstore 的对接层
│   ├── local_ci/               # CI 相关脚本
│   └── ...                     # 其他 MySQL 标准目录
└── dstore/
    ├── dstore/                 # Dstore 存储引擎核心代码
    │   ├── src/                # 存储引擎实现
    │   ├── interface/          # 公共 API 头文件
    │   ├── utils/              # 工具库 (libgsutils.so)
    │   └── tests/              # 单元测试
    └── local_libs/             # 依赖库目录
        ├── buildtools/         # 编译工具 (GCC 7.3)
        ├── secure/             # Huawei_Secure_C
        ├── lz4/                # LZ4 压缩库
        ├── cjson/              # cJSON 库
        ├── gtest/              # Google Test
        ├── mockcpp/            # MockCPP
        └── openssl/            # OpenSSL
```

---

# 环境配置与编译

## 编译环境要求

- **操作系统**: 无要求
- **编译器**: GCC 10.3 (mysql使用) 、 GCC 7.3（dstore使用）
- **CMake**: 3.20+
- **磁盘空间**: 约 10 GB

## 依赖库

Dstore 存储引擎依赖以下库（位于 `dstore/local_libs/` 目录）：

| 依赖库 | 建议版本 | 说明 |
|--------|----------|------|
| GCC | 7.3 | 编译器 |
| Huawei_Secure_C | 3.0.9 | 安全函数库 |
| LZ4 | 1.10.0 | 压缩库 |
| cJSON | 1.7.17 | JSON 解析库 |
| gtest | 1.10.0 | 单元测试框架 |
| mockcpp | master | Mock 框架 |
| OpenSSL | 3.0.9 | SSL/TLS 库 |

---

## 编译步骤

### 1. 设置环境变量

```bash
export WORK_DIR=/opt/workdir/jenkins/ccd_build/open_dstore/mysql-server
```

### 2. 编译 Dstore utils 模块

首先需要编译 Dstore 的 utils 模块：

```bash
cd ${WORK_DIR}/local_ci/script
sh build_utils.sh release
```

编译成功后，`dstore/utils/output/lib/` 目录下会生成 `libgsutils.so`。

### 3. 编译 MySQL Server (集成 Dstore)

```bash
sh +x ${WORK_DIR}/BUILD/build_release.sh \
    --workdir=${WORK_DIR}/hwsql-builder \
    --result=${WORK_DIR}/artis \
    --install=/home/ci/install/mysql \
    --jobs=64 \
    --package \
    --exclude-test \
    --enable-install \
    --dstore
```

**关键参数说明**：

| 参数 | 说明 |
|------|------|
| `--workdir` | 构建工作目录 |
| `--result` | 构建产物输出目录 |
| `--install` | 安装路径 |
| `--jobs` | 并行编译任务数 |
| `--package` | 构建完成后打包 |
| `--exclude-test` | 排除测试用例 |
| `--enable-install` | 启用安装步骤 |
| `--dstore` | **必须**：启用 Dstore 存储引擎集成 |

### 4. 编译产物

编译成功后，产物位于：

- **安装目录**: `/home/ci/install/mysql/`
  - `bin/` - MySQL 可执行文件 (mysqld, mysql, mysqladmin 等)
  - `lib/` - 库文件，包含 `libdstore.so`
  - `lib/dstore/` - Dstore 存储引擎库
  - `share/` - 共享文件

- **打包目录**: `${WORK_DIR}/artis/`
  - MySQL 安装包

---

## Debug 模式编译

如需 Debug 模式编译：

```bash
# 编译 Dstore utils (debug)
cd ${WORK_DIR}/local_ci/script
sh build_utils.sh debug

# 编译 MySQL Server (debug)
sh +x ${WORK_DIR}/BUILD/build_debug.sh \
    --workdir=${WORK_DIR}/hwsql-builder \
    --result=${WORK_DIR}/artis \
    --install=/home/ci/install/mysql \
    --jobs=64 \
    --dstore
```

---

## 增量编译

完成首次完整编译后，可进行增量编译：

```bash
# Dstore 增量编译
cd dstore/dstore/tmp_build
make -sj$(($(nproc)-2)) install

# MySQL 增量编译
cd ${WORK_DIR}/hwsql-builder/build
make -sj$(nproc) install
```

---

# 运行与测试

## 启动 MySQL Server

```bash
# 设置环境变量
export LD_LIBRARY_PATH=/home/ci/install/mysql/lib/dstore:/home/ci/install/mysql/lib:$LD_LIBRARY_PATH

# 启动 MySQL
/home/ci/install/mysql/bin/mysqld --defaults-file=/path/to/my.cnf --user=mysql
```

## Dstore 单元测试

Dstore 存储引擎单元测试位于 `dstore/dstore/tmp_build/` 目录：

```bash
cd dstore/dstore/tmp_build

# 运行各模块单元测试
make run_dstore_buffer_unittest     # 缓冲池测试
make run_dstore_xact_unittest       # 事务测试
make run_dstore_index_unittest      # 索引测试
make run_dstore_lock_unittest       # 锁管理器测试
make run_dstore_ha_unittest         # HA 测试
make run_dstore_framework_unittest  # 框架测试
make run_dstore_datamanager_unittest # 数据管理测试
```

## MySQL MTR 测试

```bash
cd /home/ci/install/mysql/mysql-test
./mysql-test-run.pl --suite=dstore --parallel=4
```

---

# Dstore 存储引擎

Dstore 是一个独立、可编译、可测试的数据库存储引擎组件。详细的 Dstore 使用说明请参考：

- [Dstore README](../dstore/dstore/README.md) - 英文版
- [Dstore README 中文版](../dstore/dstore/README_CN.md) - 中文版

### Dstore 核心模块

| 模块 | 功能 |
|------|------|
| Buffer Manager | 缓冲池管理、页面写入器 |
| Transaction Manager | 事务管理、CSN、Undo |
| Index Manager | B-tree 索引 |
| Lock Manager | 锁管理 |
| Heap Manager | 堆表管理 |
| XLog Manager | WAL 日志 |
| Catalog Manager | 系统表管理 |
| Undo Manager | Undo 管理 |
| Tablespace Manager | 表空间管理 |

---

# 常见问题

## 1. 编译找不到依赖库

确认 `dstore/local_libs/` 目录下各依赖库完整，检查路径配置：

```bash
source dstore/dstore/buildenv
```

## 2. 运行时找不到 libdstore.so

设置 LD_LIBRARY_PATH：

```bash
export LD_LIBRARY_PATH=/home/ci/install/mysql/lib/dstore:/home/ci/install/mysql/lib:$LD_LIBRARY_PATH
```

## 3. GCC 版本问题

 系统使用 GCC 10.3：

```bash
export GCC=/opt/hw/gcc-10.3
export PATH=$GCC/bin:$PATH
export LD_LIBRARY_PATH=$GCC/lib:$GCC/lib64:$LD_LIBRARY_PATH
```

---

# 更多信息

- [Dstore 存储引擎文档](../dstore/dstore/)
- [MySQL 官方文档](https://dev.mysql.com/doc/)
- [项目 CI 脚本](local_ci/)