# PGO/LTO/BOLT Compilation Feature Specification

> Version: 2.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Main Commit: 43b6a0f590c7eedd6b2cc2aa9e5e8ed643d821f8
> Follow-up Commit: cf357619b8eb35c707d7233438fc8b64c1bd34b3
> Authors: wuxiliang (main), qinwei (follow-up)
> Merge Dates: 2025-08-30, 2025-09-19
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

MySQL's default build configuration produces binaries that are not fully optimized for the specific hardware and workload they will run on. Profile-Guided Optimization (PGO), Link-Time Optimization (LTO), and BOLT (Binary Optimization and Layout Tool) are three complementary compiler-level optimization techniques that can significantly improve performance:

- **PGO**: Uses runtime profiling data to guide code layout, inlining, and branch prediction decisions
- **LTO**: Enables whole-program optimization across compilation units at link time
- **BOLT**: Post-link binary optimizer that reorders code layout based on profiling data

The upstream MySQL 8.0 has partial PGO/LTO support in its CMake build system, but the Huawei RDS branch needs a fully adapted pipeline that integrates all three techniques with the additional storage engine (DStore) and supports automated sysbench/TPCC-based profile generation for both DStore and InnoDB.

### 1.2 Solution

Extend and adapt the CMake build system and build scripts to provide a complete PGO/LTO/BOLT compilation pipeline:

1. **PGO Pipeline**: Two-phase build (generate profile -> use profile) with automatic sysbench and TPCC workload training
2. **LTO Integration**: Full `-flto` support with parallel linking and correct AR/RANLIB tool selection
3. **BOLT Integration**: Post-link optimization with `--emit-relocs` and `-fno-reorder-blocks-and-partition`
4. **Build Script**: Enhanced `BUILD/build.sh` with fine-grained control over training workloads (sysbench, TPCC, MTR)
5. **Packaging Script**: Updated `local_ci/script/build_package.sh` with PGO/LTO/BOLT options
6. **Training Suite**: Dedicated MTR test suite (`mysql-test/suite/pgo/`) with sysbench and TPCC workloads for both InnoDB and DStore

### 1.3 Pipeline Overview

```
Phase 1: Profile Generation
  cmake -DFPROFILE_GENERATE=1 ...
  make
  Run training workload (sysbench + TPCC via MTR)
  -> Produces .gcda profile files (GCC)

Phase 2: Profile Use (with LTO auto-enabled)
  cmake -DFPROFILE_USE=1 -DFPROFILE_GENERATE=0 -DWITH_UNIT_TESTS=0 ...
  make
  -> Produces PGO+LTO optimized binary

Optional Phase 3: BOLT (post-link)
  llvm-bolt --instrument mysqld.orig -o mysqld
  Run training workload (sysbench + TPCC via MTR)
  merge-fdata (merge BOLT profiles)
  llvm-bolt mysqld.orig -o mysqld -data=bolt.profile.full \
    -reorder-blocks=cache+ -split-functions=2 -reorder-functions=hfsort+ \
    -split-all-cold -split-eh -update-debug-sections
```

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| CMake build system (commit #1) | 2 | +126 |
| Build scripts (commit #1) | 2 | +134 |
| PGO training suite - DStore (commit #1) | 12 | +1,004 |
| PGO training suite - InnoDB + fixups (commit #2) | 16 | +316 / -164 |
| Git LFS config | 1 | +1 |
| **Total (2 commits)** | **16+ files** | **+1,577 / -170** |

---

## 2. New Files

### 2.1 PGO Training Suite (`mysql-test/suite/pgo/`)

| File | Lines | Purpose |
|------|-------|---------|
| `my.cnf` | 174 | Server configuration for PGO training (DStore and InnoDB) |
| `include/sysbench_prepare.inc` | 50 | Create and populate sysbench OLTP tables (1M rows per table) |
| `include/sysbench_run.inc` | 101 | Run sysbench OLTP read-write workload (500 transactions by default) |
| `include/sysbench_cleanup.inc` | 27 | Drop sysbench tables |
| `include/tpcc_prepare.inc` | 75 | Create and populate TPC-C tables from `tpcc10.zip` |
| `include/tpcc_run.inc` | 97 | Run TPC-C workload (new-order, payment, order-stat, delivery, stock-level) |
| `include/tpcc_cleanup.inc` | 30 | Drop TPC-C tables and database |
| `t/sysbench_oltp_rw_innodb.test` | 63 | InnoDB sysbench OLTP RW test (SSL + non-SSL, TCP + Unix socket) |
| `r/sysbench_oltp_rw_innodb.result` | 25 | Expected result for InnoDB sysbench |
| `t/sysbench_oltp_rw_dstore.test` | 66 | DStore sysbench OLTP RW test (SSL + non-SSL, TCP + Unix socket) |
| `r/sysbench_oltp_rw_dstore.result` | 34 | Expected result for DStore sysbench |
| `t/tpcc_oltp.test` | 17 | TPC-C OLTP test (DStore only) |
| `r/tpcc_oltp.result` | 454 | Expected result for TPC-C |
| `t/mysql_client_test.test` | 30 | MySQL client test for PGO coverage |
| `r/mysql_client_test.result` | 5 | Expected result for client test |
| `t/mysql_client_test.cnf` | 2 | Client test configuration |
| `t/mysql_client_test-master.opt` | 5 | Client test server options |

### 2.2 Test Data

| File | Purpose |
|------|---------|
| `mysql-test/std_data/tpcc10.zip` | TPC-C dataset (10 warehouses, ~430MB, stored in Git LFS) |

---

## 3. Modifications to Existing Files

### 3.1 `CMakeLists.txt` (root)

#### BOLT Support (New)

```cmake
IF (WITH_BOLT)
  # BOLT requires linking with --emit-relocs
  SET(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} -Wl,--emit-relocs")
  SET(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} -Wl,--emit-relocs")

  STRING(TOUPPER "${CMAKE_BUILD_TYPE}" FLAGS_SUFFIX)
  IF (CMAKE_COMPILER_IS_GNUCC AND NOT CMAKE_C_COMPILER_VERSION VERSION_LESS 8)
    SET(CMAKE_C_FLAGS_${FLAGS_SUFFIX}
        "${CMAKE_C_FLAGS_${FLAGS_SUFFIX}} -fno-reorder-blocks-and-partition")
    SET(CMAKE_C_FLAGS
        "${CMAKE_C_FLAGS} -fno-reorder-blocks-and-partition")
  ENDIF()

  IF (CMAKE_COMPILER_IS_GNUCXX AND NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8)
    SET(CMAKE_CXX_FLAGS_${FLAGS_SUFFIX}
        "${CMAKE_CXX_FLAGS_${FLAGS_SUFFIX}} -fno-reorder-blocks-and-partition")
    SET(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} -fno-reorder-blocks-and-partition")
  ENDIF()
ENDIF(WITH_BOLT)
```

#### LTO Diagnostic Messages (New)

```cmake
IF(WITH_LTO)
  MESSAGE(STATUS "CMAKE_NM: ${CMAKE_NM}")
  MESSAGE(STATUS "CMAKE_AR: ${CMAKE_AR}")
  MESSAGE(STATUS "CMAKE_RANLIB: ${CMAKE_RANLIB}")
ENDIF()
```

### 3.2 `BUILD/build.sh`

This is the main build script with significant PGO/LTO/BOLT enhancements.

#### New Command-Line Options

```bash
LONG_OPTS="...,pgo_sysbench,pgo_tpcc,pgo_mtr"

'--pgo_sysbench') BUILD_PGO_SYSBENCH=1 ;;
'--pgo_tpcc')     BUILD_PGO_TPCC=1 ;;
'--pgo_mtr')      BUILD_PGO_MTR=1 ;;
```

#### PGO Training (Enhanced)

```bash
do_pgo_training() {
  if [ -n "$BUILD_PGO_SYSBENCH" ]; then
    $mtr --suite=pgo --parallel=1 --testcase-timeout=300 \
      mysql_client_test sysbench_oltp_rw_innodb sysbench_oltp_rw_dstore
  fi

  if [ -n "$BUILD_PGO_TPCC" ]; then
    $mtr --suite=pgo --parallel=1 --testcase-timeout=300 tpcc_oltp
  fi
}
```

#### BOLT Training (New Function)

```bash
do_bolt_training() {
  # Check for llvm-bolt and merge-fdata
  if ! command -v llvm-bolt &> /dev/null || ! command -v merge-fdata &> /dev/null; then
    return 0
  fi

  DEST_BOLT_DIR=$BUILD_RESULT_DIR/bolt_tmp
  clean_dir "$DEST_BOLT_DIR"

  # Instrument binary
  mv -f ${BUILD_BUILD_DIR}/bin/mysqld ${BUILD_BUILD_DIR}/bin/mysqld.orig
  llvm-bolt --instrument \
    ${BUILD_BUILD_DIR}/bin/mysqld.orig -o ${BUILD_BUILD_DIR}/bin/mysqld \
    --instrumentation-file=${DEST_BOLT_DIR}/bolt.instr.profile \
    --instrumentation-binpath=${BUILD_BUILD_DIR}/bin/mysqld

  # Run sysbench training
  $mtr --suite=pgo --parallel=1 --testcase-timeout=300 \
    mysql_client_test sysbench_oltp_rw_innodb sysbench_oltp_rw_dstore
  mv -f ${DEST_BOLT_DIR}/bolt.instr.profile \
    ${DEST_BOLT_DIR}/bolt.instr.profile.sysbench_oltp_rw

  # Run TPCC training
  $mtr --suite=pgo --parallel=1 --testcase-timeout=300 tpcc_oltp
  mv -f ${DEST_BOLT_DIR}/bolt.instr.profile \
    ${DEST_BOLT_DIR}/bolt.instr.profile.tpcc_oltp

  # Merge profiles
  merge-fdata \
    ${DEST_BOLT_DIR}/bolt.instr.profile.sysbench_oltp_rw \
    ${DEST_BOLT_DIR}/bolt.instr.profile.tpcc_oltp > \
    ${DEST_BOLT_DIR}/bolt.profile.full

  # Optimize binary
  llvm-bolt ${BUILD_BUILD_DIR}/bin/mysqld.orig \
    -o ${DEST_BOLT_DIR}/mysqld -data=${DEST_BOLT_DIR}/bolt.profile.full \
    -reorder-blocks=cache+ -split-functions=2 -reorder-functions=hfsort+ \
    -split-all-cold -split-eh -update-debug-sections

  mv -f ${DEST_BOLT_DIR}/mysqld ${BUILD_BUILD_DIR}/bin/mysqld
  rm -f ${BUILD_BUILD_DIR}/bin/mysqld.orig
}
```

#### Profile Use Phase (Enhanced)

```bash
do_build() {
  # In the PGO use phase:
  cmake_opt_add+=(-DFPROFILE_DIR="$BUILD_PGO_TRAINING_DATA" -DFPROFILE_USE=1 \
                  -DFPROFILE_GENERATE=0 -DWITH_UNIT_TESTS=0)

  # After build, run BOLT if requested
  if [ -n "$BUILD_WITH_BOLT" ]; then
    do_bolt_training
  fi
}
```

#### Platform-Specific GCC Path Setup

```bash
if [ "$platform" == "aarch64" ]; then
    export LD_LIBRARY_PATH=$GCC_HOME/lib64:$GCC_HOME/lib:$GCC_HOME/lib/gcc/aarch64-linux-gnu/10/:$LD_LIBRARY_PATH
    export PATH=$GCC_HOME/bin:$GCC_HOME/aarch64-unknown-linux-gnu/bin/:$PATH
elif [ "$platform" == "x86_64" ]; then
    export LD_LIBRARY_PATH=$GCC_HOME/lib64:$GCC_HOME/lib:$GCC_HOME/lib/gcc/x86_64-linux-gnu/10/:$LD_LIBRARY_PATH
    export PATH=$GCC_HOME/bin:$GCC_HOME/x86_64-pc-linux-gnu/bin/:$PATH
fi
```

### 3.3 `local_ci/script/build_package.sh`

Extended with PGO/LTO/BOLT and training workload options:

```bash
build_type=$1
with_asan=$2
with_cov=${3:-0}
with_dstore=${4:-1}
with_pgo=${5:-0}
with_lto=${6:-0}
with_bolt=${7:-0}
train_sysbench=${8:-1}
train_tpcc=${9:-1}
train_mtr=${10:-1}

opt_option=()
if [[ "$with_pgo" == "1" ]]; then opt_option+=("--pgo"); fi
if [[ "$with_lto" == "1" ]]; then opt_option+=("--lto"); fi
if [[ "$with_bolt" == "1" ]]; then opt_option+=("--bolt"); fi

train_option=()
if [[ "$train_sysbench" == "1" ]]; then train_option+=("--pgo_sysbench"); fi
if [[ "$train_tpcc" == "1" ]]; then train_option+=("--pgo_tpcc"); fi
if [[ "$train_mtr" == "1" ]]; then train_option+=("--pgo_mtr"); fi
```

### 3.4 `.gitattributes`

```
mysql-test/std_data/tpcc10.zip filter=lfs diff=lfs merge=lfs -text
```

### 3.5 `mysql-test/suite/pgo/include/sysbench_prepare.inc` (Follow-up)

Changed default storage engine from `ENGINE=Dstore` to use the server default:

```sql
-- Before (commit #1):
PRIMARY KEY (id)) ENGINE=Dstore;

-- After (commit #2):
PRIMARY KEY (id));  -- Uses default_storage_engine
```

### 3.6 `mysql-test/suite/pgo/my.cnf` (Follow-up)

Major reconfiguration in the follow-up commit:

- Added `!include include/default_mysqld.cnf`
- Changed `innodb-buffer-pool-size` from 24M to 128M
- Set `performance_schema=OFF` (was fully enabled before)
- Set `default_storage_engine=InnoDB` (was Dstore)
- Added `threadpool_schedule_mode=statement` (for InnoDB compatibility)
- Made all DStore parameters `loose-` prefixed
- Reduced `dstore_buffer` from 131072 to 65536

---

## 4. Core Data Structures

### 4.1 CMake Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `FPROFILE_GENERATE` | BOOL | OFF | Add `-fprofile-generate` flags for profile generation phase |
| `FPROFILE_USE` | BOOL | OFF | Add `-fprofile-use` flags for profile use phase |
| `FPROFILE_DIR` | PATH | Auto-detected | Directory for profile data files |
| `WITH_LTO` | BOOL | OFF (ON if FPROFILE_USE) | Enable link-time optimization with `-flto` |
| `WITH_BOLT` | BOOL | OFF | Enable BOLT post-link optimization |

### 4.2 Build Script Variables

| Variable | Description |
|----------|-------------|
| `BUILD_PGO_SYSBENCH` | Run sysbench training workload during PGO/BOLT phase |
| `BUILD_PGO_TPCC` | Run TPCC training workload during PGO/BOLT phase |
| `BUILD_PGO_MTR` | Run MTR tests during PGO phase |
| `BUILD_WITH_BOLT` | Enable BOLT optimization after build |
| `BUILD_PGO_TRAINING_DATA` | Path to profile data directory |
| `BUILD_PGO_FIRST_STAGE_ONLY` | Only build the profile generation phase |

### 4.3 BOLT Optimization Flags

```bash
llvm-bolt ... \
  -reorder-blocks=cache+    \  # Reorder basic blocks for cache locality
  -split-functions=2        \  # Split hot/cold parts of functions
  -reorder-functions=hfsort+ \  # Reorder functions using HFSort+ algorithm
  -split-all-cold           \  # Split all cold basic blocks
  -split-eh                 \  # Split exception handling code
  -update-debug-sections       # Update DWARF debug info
```

---

## 5. Execution Flow

### 5.1 Full PGO+LTO Build Pipeline (GCC)

```bash
# Step 1: Profile Generation Build
sh BUILD/build_release.sh --pgo --pgo_sysbench --pgo_tpcc \
  --workdir=/path/to/build --result=/path/to/result

# Inside build.sh:
# Phase 1: cmake -DFPROFILE_GENERATE=1 ...
# Phase 2: make
# Phase 3: mysqld starts, MTR runs sysbench_oltp_rw_innodb, sysbench_oltp_rw_dstore, tpcc_oltp
# Phase 4: mysqld shutdown (profile data flushed to FPROFILE_DIR)

# Step 2: Profile Use Build (with LTO auto-enabled)
sh BUILD/build_release.sh --pgo-train=/path/to/profile-data \
  --workdir=/path/to/build --result=/path/to/result

# Inside build.sh:
# cmake -DFPROFILE_DIR="..." -DFPROFILE_USE=1 -DFPROFILE_GENERATE=0 -DWITH_UNIT_TESTS=0
# make
```

### 5.2 BOLT Optimization Pipeline

```bash
sh BUILD/build_release.sh --pgo --lto --bolt --pgo_sysbench --pgo_tpcc \
  --workdir=/path/to/build --result=/path/to/result

# After PGO+LTO build:
# 1. llvm-bolt --instrument mysqld.orig -o mysqld
# 2. MTR runs training workloads -> produces BOLT profiles
# 3. merge-fdata combines profiles
# 4. llvm-bolt optimizes mysqld.orig -> mysqld
```

### 5.3 Packaging Pipeline

```bash
sh local_ci/script/build_package.sh release 0 0 1 1 1 0 1 1 1
# Args: build_type asan cov dstore pgo lto bolt train_sysbench train_tpcc train_mtr
```

### 5.4 Sysbench Training Workload

The sysbench training exercises the following SQL patterns (500 transactions by default):

1. **Point SELECTs** (10 per transaction): `SELECT * FROM sbtestN WHERE id=?`
2. **Simple range**: `SELECT c FROM sbtestN WHERE id BETWEEN ? AND ?+100`
3. **Sum range**: `SELECT SUM(k) FROM sbtestN WHERE id BETWEEN ? AND ?+100`
4. **Order range**: `SELECT c FROM ... ORDER BY c`
5. **Distinct range**: `SELECT DISTINCT c FROM ... ORDER BY c`
6. **Index update**: `UPDATE sbtestN SET k=k+1 WHERE id=?`
7. **Non-index update**: `UPDATE sbtestN SET c=... WHERE id=?`
8. **DELETE + INSERT**: Maintain consistent row count

Connections tested: non-SSL Unix socket, SSL Unix socket, non-SSL TCP, SSL TCP.

### 5.5 TPC-C Training Workload

The TPC-C training exercises 5 transaction types (50 iterations by default):

1. **New-Order**: Insert order + order lines, update stock
2. **Payment**: Update warehouse/district/customer, insert history
3. **Order-Status**: Query customer and order info
4. **Delivery**: Update orders, order_line, new_orders, customer
5. **Stock-Level**: Count low-stock items

---

## 6. System Variables

No new MySQL system variables. The feature is entirely a build-time optimization.

---

## 7. EXPLAIN Output

Not applicable. This is a build-time feature with no runtime SQL-level changes.

---

## 8. Error Handling

| Error Condition | Handling |
|----------------|----------|
| Both `FPROFILE_GENERATE` and `FPROFILE_USE` set | `FATAL_ERROR`: "Cannot combine -fprofile-generate and -fprofile-use" |
| Compiler does not support `-flto` | `FATAL_ERROR`: "Compiler does not support -flto" |
| Missing profile data for some files | GCC `-fprofile-correction` handles missing data gracefully |
| LLD linker incompatibility | Automatically disabled (`USE_LD_LLD OFF`) when PGO is active |
| GCC AR/RANLIB not found | Falls back to default AR/RANLIB (may cause LTO issues) |
| llvm-bolt or merge-fdata not found | BOLT training silently skipped (`do_bolt_training` returns 0) |
| MTR training failure | `die "$mtr $?"` terminates the build |
| Invalid profile data | GCC `-fprofile-correction` attempts to correct; if severe, build fails with linker errors |
| Sysbench stall during training | MTR testcase-timeout=300 provides generous timeout |
| BOLT instrumentation failure | Build continues with non-BOLT binary (mysqld.orig) |

---

## 9. Test Coverage

### Build Verification Tests

| Category | Test Type | Description |
|----------|-----------|-------------|
| PGO generate | Manual | Successful build with `-DFPROFILE_GENERATE=1` |
| PGO use + LTO | Manual | Successful build with `-DFPROFILE_USE=1` (auto-enables LTO) |
| BOLT | Manual | Successful build with `-DWITH_BOLT=1` |
| Combined PGO+LTO+BOLT | Manual | Full optimization pipeline |

### MTR Training Suite (`mysql-test/suite/pgo/`)

| Test | Engine | Description |
|------|--------|-------------|
| `sysbench_oltp_rw_innodb` | InnoDB | OLTP read-write with SSL/non-SSL, TCP/Unix socket |
| `sysbench_oltp_rw_dstore` | DStore | OLTP read-write with SSL/non-SSL, TCP/Unix socket |
| `tpcc_oltp` | DStore | Full TPC-C workload (10 warehouses) |
| `mysql_client_test` | InnoDB | MySQL client API coverage |

### Regression Testing

- All existing MTR tests must pass with PGO+LTO build
- Training workloads themselves serve as correctness tests (result validation)

---

## 10. Limitations

- GCC8 and GCC9 have different profile data naming/location schemes, requiring different directory structures
- BOLT is Linux-only and requires `llvm-bolt` and `merge-fdata` tools
- `REPRODUCIBLE_BUILD` is forced ON when PGO is active (for profile sharing across builds)
- LLD linker is automatically disabled when PGO is active (compatibility issues)
- Parallel LTO linking uses `nproc` to determine thread count; may be excessive on large machines
- BOLT requires GCC 8+ or Clang 9+ for `-fno-reorder-blocks-and-partition`
- Profile data from one workload may not be optimal for a different workload
- The TPC-C test is DStore-only (InnoDB TPC-C test not provided)
- `--nowarnings` removed from MTR runs in follow-up commit (more verbose output)
- `performance_schema=OFF` in PGO my.cnf to reduce overhead during training
- Training is sequential (`--parallel=1`) to avoid profile data corruption
- The `sysbench_prepare.inc` creates 2^20 = 1,048,576 rows per table, which is slow on low-end hardware
- TPC-C dataset (~430MB) is stored in Git LFS, requiring LFS support to clone
- `clean_dir` in build.sh now actually removes files (was previously commented out)

---

## 11. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **`CMakeLists.txt`** -- BOLT support (`--emit-relocs`, `-fno-reorder-blocks-and-partition`)
2. **`CMakeLists.txt`** -- LTO diagnostic messages (CMAKE_NM, CMAKE_AR, CMAKE_RANLIB)
3. **`BUILD/build.sh`** -- New command-line options (`--pgo_sysbench`, `--pgo_tpcc`, `--pgo_mtr`)
4. **`BUILD/build.sh`** -- Enhanced `do_pgo_training()` with selective workload support
5. **`BUILD/build.sh`** -- New `do_bolt_training()` function
6. **`BUILD/build.sh`** -- Enhanced `do_build()` with BOLT call and PGO use phase flags
7. **`BUILD/build.sh`** -- Platform-specific GCC path setup
8. **`BUILD/build.sh`** -- `clean_dir` fix (uncomment rm command)
9. **`local_ci/script/build_package.sh`** -- PGO/LTO/BOLT and training options
10. **`.gitattributes`** -- Git LFS for tpcc10.zip
11. **`mysql-test/std_data/tpcc10.zip`** -- TPC-C test data (Git LFS)
12. **`mysql-test/suite/pgo/my.cnf`** -- Server configuration for training
13. **`mysql-test/suite/pgo/include/`** -- Shared include files (sysbench_prepare/run/cleanup, tpcc_prepare/run/cleanup)
14. **`mysql-test/suite/pgo/t/` and `r/`** -- Test cases and results for InnoDB sysbench, DStore sysbench, TPCC, mysql_client_test
15. **Follow-up: InnoDB training** -- Separate DStore and InnoDB sysbench tests, update my.cnf for dual-engine support
