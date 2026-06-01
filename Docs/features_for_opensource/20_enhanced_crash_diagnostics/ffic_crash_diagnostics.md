# 设计文档：增强 `mysqld` 崩溃诊断信息（v2）

## 1. 背景与动机

当 `mysqld` 收到致命信号（`SIGSEGV`、`SIGBUS`、`SIGILL`、`SIGFPE`、`SIGABRT`）时，现有处理
程序仅向 stderr 写入一段调用栈信息。这虽然有所帮助，但在没有 core dump 和 GDB 会话的情况下，
通常不足以诊断根本原因——而在生产环境中，这些条件往往无法满足。

本特性的目标是生成一份**自包含、可直接用于排查**的崩溃报告，使有经验的开发者，乃至操作系统知
识有限的 DBA，都能仅凭错误日志定位根本原因。

本版本设计借鉴了 openGauss `fatal_err.cpp` 中的经验（指令字节转储、操作系统信息），并为寄存
器/栈内存节增加了详细的平易近人的使用说明，专门面向不熟悉操作系统或 CPU 底层知识的工程师。

### 设计原则

| 原则 | 原因 |
|------|------|
| **异步信号安全** | 进程处于未定义状态；禁止 `malloc`、`printf` 及任何可能重入互斥锁的库调用。仅使用 `write(2)`、`read(2)`、`open(2)`、`close(2)`。 |
| **对上游代码侵入最小** | 所有新诊断函数**追加**到现有文件末尾。现有函数中仅修改三处关键行（信号注册和处理函数签名）。这使 MySQL 版本升级合并保持简洁。 |
| **配置开关** | 该特性**默认关闭**。用户通过 `--ffic-verbose-crash-diagnostics` 主动启用。关闭时，现有崩溃输出完全不变。 |
| **优雅降级** | 缺失上下文（`nullptr`）、不支持的架构或无法读取的 `/proc` 文件，均不得导致二次崩溃。每个输出节均独立保护。 |
| **平易近人的说明** | 每个输出节包含简短的"如何解读"说明，使没有操作系统或 CPU 专业知识的工程师也能利用这些信息。 |

---

## 2. 范围

本文档涵盖以下内容：

1. **调用栈**（含文件名和行号）——已实现，保持不变。
2. **CPU 寄存器转储**——x86-64 和 AArch64，每个寄存器附带诊断提示。
3. **栈顶内存转储**——原始字节数据，附带平易近人的解读指南。
4. **故障 PC 附近的指令字节**——用于高级分析，借鉴自 openGauss。
5. **当前 SQL 查询**和线程状态——消除手动关联查询日志的需要。
6. **操作系统信息**——内核版本，用于确认平台相关问题。
7. **进程资源快照**——捕获内存耗尽和文件描述符耗尽问题。
8. **已加载段地址**——支持离线 ASLR 调整后的符号解析。

---

## 3. 架构：最小侵入设计

核心架构约束是：MySQL 由上游持续开发维护。为使版本合并保持简单，实现遵循三条规则：

1. **只追加，不重写。** 所有新函数均添加到各自文件的**末尾**，位于原有代码的最后一行之后。不
   修改任何现有函数体。

2. **对现有代码仅做三处定点修改，不超过此数：**

   | 文件 | 修改内容 | 不可避免的原因 |
   |------|---------|--------------|
   | `sql/mysqld.cc` | `my_init_signals()`：使用 `sa_sigaction` + `SA_SIGINFO` 标志；添加全局变量 `ffic_verbose_crash_diagnostics` | 若不使用 `SA_SIGINFO`，内核不会传递 `ucontext_t`，寄存器转储无从实现 |
   | `sql/signal_handler.cc` | `handle_fatal_signal` 签名：在非 Win32 平台添加 `siginfo_t*`、`void*` 参数 | 信号处理函数必须匹配 `SA_SIGINFO` 原型才能接收内核提供的 `ucontext_t` |
   | `sql/signal_handler.cc` | 在 `handle_fatal_signal` 末尾追加一个 `if`，调用 `ffic_print_enhanced_crash_info()` | 将新特性接入现有流程；由配置开关保护 |

3. **配置开关默认关闭。** 关闭时，代码路径变化仅相当于检查一个全局布尔值的单分支；
   `mysqld` 的可观测行为与未打补丁的版本完全相同。

### 调用关系图（仅新代码）

```
handle_fatal_signal(sig, info, context)     ← 现有函数，签名已扩展
  [现有代码保持不变]
  if (ffic_verbose_crash_diagnostics)       ← 新增：追加在末尾的一个 if 语句
    ffic_print_enhanced_crash_info(sig, info, context)
      ffic_print_crash_banner()             ← sql/signal_handler.cc（追加）
      ffic_print_signal_info()              ← sql/signal_handler.cc（追加）
      ffic_print_active_query()             ← sql/signal_handler.cc（追加）
      ffic_print_registers()               ← mysys/stacktrace.cc（追加）
      ffic_print_register_hints()          ← mysys/stacktrace.cc（追加）
      ffic_print_stack_memory()            ← mysys/stacktrace.cc（追加）
      ffic_print_instruction_context()     ← mysys/stacktrace.cc（追加）
      ffic_print_process_resources()       ← mysys/stacktrace.cc（追加）
      ffic_print_loaded_segments()         ← mysys/stacktrace.cc（追加）
      ffic_print_os_info()                 ← mysys/stacktrace.cc（追加）
```

---

## 4. 配置开关

该特性通过一个**动态全局系统变量**控制——无需重启数据库：

```sql
-- 运行时启用（立即生效）：
SET GLOBAL rds_ffic_verbose_crash_diagnostics = ON;

-- 验证：
SHOW GLOBAL VARIABLES LIKE 'rds_ffic_verbose_crash_diagnostics';

-- 关闭：
SET GLOBAL rds_ffic_verbose_crash_diagnostics = OFF;
```

也可以在启动时设置：

```
mysqld --rds-ffic-verbose-crash-diagnostics[=ON|OFF]   （默认：OFF）
```

**实现**：在 `sql/sys_vars.cc` 中注册为 `Sys_var_bool`（`GLOBAL_VAR`），底层 C++ 变量
`bool rds_ffic_verbose_crash_diagnostics` 定义于 `sql/mysqld.cc`，在 `handle_fatal_signal`
内于崩溃时读取一次。由于该变量在崩溃发生前已被设置且不再改变，无需额外的同步原语。

**关闭时**（默认）：`handle_fatal_signal` 的行为与未修改的上游代码完全相同，唯一额外开销
是对一个全局布尔变量的单次读取（分支预测友好）。

**开启时**：在现有崩溃输出之后，`ffic_print_enhanced_crash_info()` 运行，向 stderr
输出所有附加诊断节。

---

## 5. 特性：崩溃摘要横幅

首先打印，位于所有详细节之前，使最关键的分类信息无需滚动即可看到。

```
=== MYSQLD CRASH SUMMARY ===
  Signal   : SIGSEGV (Segmentation fault)
  Reason   : SEGV_MAPERR -- address not mapped to object
  Fault @  : 0x0000000000000020  (NULL + 32 bytes -- likely null-pointer dereference)
  MySQL TID: 4    Query ID: 12345
             (MySQL internal thread ID, shown in SHOW PROCESSLIST)
  PID      : 12345
  OS TID   : 12348
             (kernel thread ID -- matches 'ps -L -p 12345 | grep 12348')
  Pthread  : 0x7f1234560700
             (in GDB: 'thread find 139712345959168' or 'info threads')
```

**线程 ID 说明（面向不熟悉操作系统的工程师）**：

| ID 类型 | 含义 | 查询方式 |
|---------|------|---------|
| MySQL TID | MySQL 内部连接 ID | `SHOW PROCESSLIST`、`INFORMATION_SCHEMA.PROCESSLIST` |
| PID | Linux 进程 ID | `ps aux \| grep mysqld`、`/proc/<pid>/` |
| OS TID | Linux 内核线程 ID（LWP） | `ps -L -p <pid>`、`top -H`、GDB `info threads` |
| Pthread | POSIX 线程句柄（不透明整数） | GDB `thread find <value>` |

OS TID 是与外部工具（`gdb`、`strace -p <tid>`、`perf`）结合使用最有价值的线程标识。

**实现**：通过字符串表解码 `siginfo_t::si_code`；`si_addr` 与 4095 比较生成"NULL + 偏移量"
提示；OS TID 通过 `syscall(SYS_gettid)` 获取，Pthread ID 通过 `pthread_self()` 获取——
均为异步信号安全调用。

---

## 6. 特性：信号与故障信息

扩展现有的简短信号打印，附带人类可读的 `si_code` 描述。

```
=== Signal Information ===
  si_signo = 11 (SIGSEGV)
  si_code  = 1  (SEGV_MAPERR: address not mapped to object)
  si_addr  = 0x0000000000000020
```

信号码查找表涵盖 SIGSEGV、SIGBUS、SIGILL、SIGFPE、SIGABRT 及最常见的 `si_code` 值及其中文含义。

当 `si_code == SI_USER`（0）或 `SI_QUEUE`（-1）时，信号由另一进程显式发送
（例如 `kill -SIGSEGV <pid>`），而非来自 CPU 故障。此时额外打印发送方 PID 和 UID：

```
  Sender   : PID 1234, UID 1000
             (signal was sent by another process, not a CPU fault)
```

---

## 7. 特性：当前 SQL 查询

打印崩溃线程正在执行的查询文本。

```
=== Active Query ===
  Thread ID : 4
  Command   : Query
  Killed    : NOT_KILLED
  Query     : SELECT * FROM orders JOIN shipments ON orders.id = shipments.order_id
               WHERE customer_id = 99999 LIMIT 1000
  (output limited to 1024 bytes)
```

**实现**：读取 `current_thd->query()`（一个指向现有缓冲区的 `LEX_CSTRING`），不进行复制。
以 `current_thd != nullptr` 为前提。在未持锁的情况下读取指针（信号处理函数内加锁有死锁风险）；
若崩溃发生在查询解析过程中，数据可能只写了一部分，但即便如此也比没有有用。

---

## 8. 特性：带诊断提示的 CPU 寄存器转储

### 8.1 为什么寄存器很重要

寄存器是 CPU 中最快的存储单元——直接集成在处理器硅片中的极小变量。崩溃发生时，寄存器精确捕
获了 CPU 正在操作的内容：指令地址、栈位置、函数参数以及中间计算结果。正确解读寄存器值，往往
无需进一步调试即可直接揭示崩溃原因。

### 8.2 x86-64 寄存器输出

```
=== CPU Registers (x86_64) ===
  Explanation: Registers are the CPU's fastest storage locations.
  RIP = instruction pointer (where the crash happened).
  RSP = stack pointer; RBP = frame pointer.
  RDI = 1st argument / 'this' pointer; RSI = 2nd argument.
  RAX = return value / syscall result.

  RIP = 0x00007f3b4c1a2d40  RSP = 0x00007ffe1234ab00
  RBP = 0x00007ffe1234ab80  EFL = 0x0000000000010246
  TRAPNO = 0x000000000000000e  ERR = 0x0000000000000004
  ...
```

#### 各寄存器参考（x86-64，System V AMD64 ABI）

| 寄存器 | 用途 | 关键诊断信号 |
|--------|------|------------|
| `RIP` | 程序计数器——故障指令地址 | 传入 `addr2line -e mysqld -f -C -p <RIP>` 可获取源代码位置 |
| `RSP` | 栈指针 | 必须 16 字节对齐。`RSP % 16 ≠ 0` 表示栈损坏或 ABI 违规 |
| `RBP` | 帧指针 | `RBP < RSP` 表示帧指针损坏；调用栈可能显示错误 |
| `EFL` | CPU 标志位 | 位 0 进位，位 6 零，位 11 溢出。单独看意义不大 |
| `TRAPNO` | CPU 陷阱编号 | `0x0e` = 页故障（SIGSEGV）；`0x0d` = 通用保护故障；`0x00` = 除零 |
| `ERR` | 页故障错误码 | 位 1 置位 = 写入故障；位 0 = 保护违规（页存在但无访问权限） |
| `RAX` | 返回值 / 系统调用号 | 若崩溃在函数尾声，RAX 为返回值；若在系统调用包装中，负值 RAX 为 `−errno` |
| `RDI` | 第 1 个参数 / `this` 指针 | `RDI ≤ 4095` 时出现 SIGSEGV，是空指针解引用最强的指示 |
| `RSI` | 第 2 个参数 / 源缓冲区指针 | 通常是失败内存复制操作的源端 |
| `RDX` | 第 3 个参数 | `MUL`/`DIV` 结果的高 64 位；除法故障时检查 `RDX:RAX` 的被除数 |
| `RCX` | 第 4 个参数 | 也用作 `REP` 字符串指令的循环计数器 |
| `R8`–`R11` | 第 5–8 个参数；调用者保存 | 用于检查崩溃点的调用参数 |
| `R12`–`R15` | 被调用者保存 | 保存调用帧的值；与调用栈交叉对比 |

### 8.3 AArch64 寄存器输出

```
=== CPU Registers (AArch64) ===
  Explanation: Registers are the CPU's fastest storage locations.
  PC = program counter (where the crash happened).
  SP = stack pointer; x29 (FP) = frame pointer; x30 (LR) = return address.
  x0 = 1st argument / 'this' pointer (also holds return value after call).

  PC     = 0x0000ffff8c1a2d40  SP     = 0x0000ffffe234ab00
  LR(x30)= 0x0000ffff8c1a1c20  PSTATE = 0x0000000060000000
  ...
```

#### 各寄存器参考（AArch64，AAPCS64 ABI）

| 寄存器 | 用途 | 关键诊断信号 |
|--------|------|------------|
| `PC` | 程序计数器——故障指令地址 | 传入 `addr2line -e mysqld -f -C -p <PC>` 可获取源代码位置 |
| `SP` | 栈指针 | 必须 16 字节对齐（ARMv8.1+ 硬件强制执行；未对齐产生 `SIGBUS BUS_ADRALN`） |
| `x30`（LR） | 链接寄存器——当前函数的返回地址 | 应作为调用栈第 #1 帧出现；若不符，说明已损坏 |
| `x29`（FP） | 帧指针 | `x29 < SP` 表示帧指针损坏；调用栈可能被截断 |
| `PSTATE` | 处理器状态标志 | 位 31（N）负数，位 30（Z）零，位 29（C）进位，位 28（V）溢出。位 [3:0] = EL，用户态应为 0 |
| `x0`–`x7` | 参数 / 返回值 | `x0 ≤ 4095` 时出现 SIGSEGV，强烈表明空指针解引用 |
| `x8` | 间接结果位置 | 指向大型返回值存储区域的指针 |
| `x9`–`x15` | 调用者保存的临时寄存器 | 用于追踪故障函数中的计算过程 |
| `x16`–`x17` | PLT/链接器暂存寄存器 | 通常无参考价值 |
| `x19`–`x28` | 被调用者保存 | 保存调用帧的值；与调用栈交叉对比 |

### 8.4 寄存器诊断提示

在原始寄存器表之后打印第二个子节，将关键寄存器值翻译成中文诊断结论：

```
=== Register Diagnostic Hints ===
  This section translates key register values into actionable clues.
  Even without OS expertise, these hints can point to the root cause.

  [RIP = 0x00007f3b4c1a2d40] -- Faulting instruction address.
    To locate it in source code, run:
      addr2line -e mysqld -f -C -p 0x7f3b4c1a2d40

  [RDI = 0x0000000000000000] -- First function argument is NULL or near-NULL.
    This is the strongest indicator of a null-pointer dereference.
    ...
```

提示生成逻辑（按优先级排序）：

1. 始终为 `RIP`/`PC` 打印 `addr2line` 命令。
2. 若第一个参数寄存器（`RDI`/`x0`）在 `[0, 4095]` 范围内 → 空指针解引用提示。
3. 若 `RSP`/`SP` 不是 16 字节对齐 → 栈损坏/ABI 违规提示。
4. 若帧指针（`RBP`/`x29`）非零且低于 `RSP`/`SP` → 调用栈完整性警告。
5. 仅限 x86-64：若 `TRAPNO == 0x0e` 且 `ERR` 位 1 置位 → 写入故障提示。

---

## 9. 特性：带解读指南的栈顶内存转储

### 9.1 是什么

栈顶转储打印崩溃时栈顶附近（`RSP`/`SP` 附近）的原始字节内容。"顶部"是最近分配的区域：函数
的局部变量、已保存的返回地址以及溢出到栈上的寄存器内容。

### 9.2 什么是栈（面向非操作系统专家）

栈是程序用于临时存储的内存区域。每次函数调用时：
- CPU 将**返回地址**（函数返回后 CPU 跳回的位置）压入栈。
- 函数压入**帧指针**（用于找到上一个栈帧），并保存它将要覆盖的寄存器。
- 局部变量（例如 `int x = 5;`）存储在栈上。

函数返回时，上述内容全部弹出并丢弃。栈**向低地址方向增长**，因此 `RSP`/`SP` 指向当前使用的
**最低地址**。

### 9.3 能识别哪些问题

| 问题 | 查找特征 |
|------|---------|
| **栈缓冲区溢出** | 返回地址之前或之后出现可识别的填充模式（`0x4141…` = 'A'，`0x4242…` = 'B'）。返回地址被相同模式覆盖。 |
| **栈破坏 / SSP** | 栈 canary（`__stack_chk_guard`）是编译器在已保存返回地址之前插入的随机值。若被覆盖，程序应已打印"stack smashing detected"。在 canary 位置看到非随机值即可确认溢出。 |
| **use-after-free（栈对象）** | 超出作用域的栈对象可能被毒化值覆盖：`0xdeadbeef…`、`0xbebebebe…`（AddressSanitizer）、`0xcc…`（MSVC 调试模式）。 |
| **野指针写入** | 写入的受害者有时是栈变量。在意外偏移处看到已知数据值（如用户 ID、表名哈希），有助于追踪写操作来源。 |
| **错误的调用约定** | 已保存的返回地址指向非代码位置：内核地址（`0xffff…`）、数据节地址或零值。 |
| **无限递归** | 全部 32 个字看起来相同或高度相似（所有帧压入相同的局部变量布局）。`RSP` 也非常低（接近栈保护页）。 |

### 9.4 输出格式

```
=== Stack Memory Dump ===
  The stack holds local variables, saved return addresses, and
  register spills for the currently executing function chain.

  How to read this dump:
    - Each row: [SP+offset]  value0  value1  value2  value3
    - Values are 8-byte words in little-endian (native) byte order.
    - Return addresses look like code pointers (e.g. 0x00007f...).
    - A repeating pattern like 0x4141414141414141 ('AAAAAAAA') or
      0xdeadbeefdeadbeef indicates a buffer overflow or memory poison.
    ...

  SP = 0x00007ffe1234ab00
  [SP+  0]  0x0000000000000000  0x00007f3b4c2b3000  ...
```

---

## 10. 特性：故障 PC 附近的指令字节

借鉴自 openGauss `fatal_err.cpp` 的 `print_inst_ctx()`。

```
=== Instruction Bytes Around Fault PC ===
  Shows the raw machine code bytes near the instruction that caused
  the crash. This is primarily for advanced analysis.
  How to use: paste the bytes into a disassembler, or run:
    objdump -d mysqld | grep -A5 '<addr>'
  The faulting instruction is at PC = 0x00007f3b4c1a2d40

  >> 0x00007f3b4c1a2d38: 48 8b 40 20 48 8b 00 eb   <-- 故障 PC 所在行
```

包含 PC 所在行用 `>>` 标记。读取前通过 `dladdr()` 验证内存有效性，与 `fatal_err.cpp` 的方式
一致。

---

## 11. 特性：进程资源快照

```
=== Process Resources (/proc/self/status) ===
  Shows memory and thread usage at the time of the crash.
  VmRSS  = actual RAM used (resident set size).
  VmPeak = peak virtual memory used.
  A very high VmRSS (close to system RAM) suggests an OOM condition.
  ...

  VmPeak:  4350 MB
  VmRSS:   4201 MB
  Threads: 42
```

**实现**：对 `/proc/self/status` 使用 `open(2)` + `read(2)`。仅打印匹配 `VmPeak`、`VmRSS`、
`VmData`、`VmStk`、`Threads` 的行，其余跳过以保持输出紧凑。

---

## 11a. 特性：系统内存信息

```
=== System Memory (/proc/meminfo) ===
  System-wide memory at crash time.
  MemAvailable < 5% of MemTotal -> possible OOM-related crash.
  SwapFree << SwapTotal         -> system was actively swapping.

  MemTotal:       32768000 kB
  MemFree:          512000 kB
  MemAvailable:    2048000 kB    <-- 极低：存在 OOM 压力
  Buffers:          128000 kB
  Cached:          8192000 kB
  SwapTotal:       8388608 kB
  SwapFree:          65536 kB    <-- Swap 几乎耗尽
```

**解读方法**：

| 指标 | 含义 |
|------|------|
| `MemTotal` | 机器安装的物理内存总量 |
| `MemFree` | 完全未使用的页面（**不含**可回收的缓存，不是 OOM 的直接指标） |
| `MemAvailable` | 内核估算的、不需要交换即可分配给新请求的内存量——**OOM 的首要判断指标** |
| `Buffers` | 块设备元数据的页缓存（通常较小） |
| `Cached` | 文件数据的页缓存，内存紧张时可回收 |
| `SwapTotal/Free` | 若 `SwapFree << SwapTotal`，说明系统在崩溃前正在大量使用 Swap |

**OOM 诊断**：若 `MemAvailable < MemTotal 的 5%`，Linux 内核 OOM Killer 可能
已 SIGKILL 了其他进程，或者 mysqld 自身的内存分配失败导致了崩溃。
交叉检验：`dmesg | grep -i "out of memory"`。

**实现**：对 `/proc/meminfo` 使用 `open(2)` + `read(2)`，打印匹配
`MemTotal`、`MemFree`、`MemAvailable`、`Buffers`、`Cached`、`SwapTotal`、`SwapFree` 的行。

---

## 11b. 特性：系统负载

```
=== System Load (/proc/loadavg) ===
  Format: 1-min 5-min 15-min  running/total  last-pid
  Compare load values to CPU count (see OS Information section).
  load >> CPUs -> CPU saturation may have contributed to the crash.

  3.42 2.87 1.93 8/1142 12350
```

**解读方法**：

- 三个数字分别是过去 1、5、15 分钟内处于可运行或不可中断（D 状态）状态的平均线程数。
- 与 CPU 核心数对比。`负载 / CPU 数 > 1.0` 表示系统存在 CPU 饱和。
- 趋势递增（15 分钟 > 5 分钟 > 1 分钟）说明负载持续升高；
  1 分钟远高于 15 分钟说明崩溃前存在突发压力。
- `running/total` 表示文件读取时刻正在运行的线程数。

**实现**：对 `/proc/loadavg`（单行文件）使用 `open(2)` + `read(2)`。

---

## 11c. 特性：进程资源限制

```
=== Process Limits (/proc/self/limits, selected) ===
  Key resource limits for this process (soft / hard / units).
  Max stack size   small -> possible stack overflow (see VmStk above).
  Max core file sz 0     -> no core dump will be written on crash.
  Max open files   small -> FD exhaustion may cause socket failures.

  Max stack size           8388608              unlimited            bytes
  Max core file size       0                    unlimited            bytes
  Max open files           65536                1048576              files
  Max address space        unlimited            unlimited            bytes
```

**解读方法**：

| 限制项 | 过低时的影响 |
|--------|-------------|
| Max stack size | 若 `VmStk`（进程资源节）接近该值，则栈溢出是崩溃原因。通过 `ulimit -s unlimited` 或 systemd 的 `LimitSTACK=` 调整。 |
| Max core file size | **为 0 时不会生成 core dump。** 启用方式：mysqld 启动前执行 `ulimit -c unlimited`，或在 service 文件中设置 `LimitCORE=infinity`。 |
| Max open files | 每个连接、表、日志均消耗文件描述符。过低会导致 `EMFILE` 错误或崩溃。生产环境建议 ≥ 65535。 |
| Max address space | 若有上限可能导致 `mmap()` / 内存分配失败，应设为 `unlimited`。 |

**实现**：对 `/proc/self/limits` 使用 `open(2)` + `read(2)`，打印以
`Max stack size`、`Max core file size`、`Max open files`、`Max address space` 开头的行。

---

## 12. 特性：已加载段地址

```
=== Loaded Segments (/proc/self/maps, selected) ===
  Shows base addresses of key loaded binaries and libraries.
  With ASLR enabled these addresses change every run. Use the base
  address to convert an absolute address from the register dump to a
  file-relative address for offline symbol resolution:
    relative_addr = absolute_addr - segment_base_addr
    addr2line -e mysqld -f -C -p <relative_addr>
  Only executable (r-xp) segments are shown.

  7f3b4c000000-7f3b4c300000 r-xp  /usr/sbin/mysqld
  7f3b4c600000-7f3b4c700000 r-xp  /usr/lib/mysql/plugin/ha_innodb.so
```

**实现**：对 `/proc/self/maps` 使用 `open(2)` + `read(2)`，以 4096 字节为块分批读取。仅打印
包含 `r-xp` 且路径以 `.so` 结尾或名称为 `mysqld` 的行。在典型部署中，输出约 20 行。

---

## 13. 特性：操作系统信息

```
=== OS Information ===
  OS kernel version and architecture. Useful to confirm whether
  the crash is kernel-version-specific.

  Sysname : Linux
  Release : 5.15.0-91-generic
  Version : #101-Ubuntu SMP Tue Nov 14 13:30:08 UTC 2023
  Machine : x86_64
```

**实现**：`uname(2)` 系统调用（在 Linux 上为异步信号安全操作）。

---


## 14. 特性：崩溃后诊断提示

作为**最后一节**打印，使工程师滚动到崩溃报告末尾时即可看到可操作的步骤。

```
=== Post-Crash Diagnostic Tips ===
  The following commands help decode information in this report.

  1. Demangle a C++ symbol shown in the stack trace:
       c++filt <mangled_name>
     Example:
       c++filt _ZN3foo3barEv
       -> foo::bar()

  2. Map an absolute crash address to source file and line number:
       Step 1: find segment_base in the 'Loaded Segments' section above.
       Step 2: relative_addr = absolute_addr - segment_base
       Step 3: addr2line -e /path/to/mysqld -f -C -p <relative_addr>
     Example (RIP = 0x7f1234abcd, base = 0x7f1200000):
       addr2line -e mysqld -f -C -p 0x34abcd

  3. Open a core dump in GDB (requires 'Max core file size' > 0):
       gdb /path/to/mysqld core
     Useful GDB commands:
       bt full          -- full stack trace with local variables
       info registers   -- all CPU register values
       x/32xg $rsp      -- hex dump of stack top (x86_64)
       x/32xg $sp       -- hex dump of stack top (AArch64)

  4. Enable verbose crash diagnostics at runtime (no restart needed):
       SET GLOBAL rds_ffic_verbose_crash_diagnostics = ON;
```

**`c++filt` 详解**：MySQL 栈跟踪已对大多数符号名进行反混淆（demangling），但偶尔会出现
以 `_Z` 开头的混淆名称。`c++filt` 可将其还原：
- `_ZN5mysql3sql5Query9executeEv` → `mysql::sql::Query::execute()`
- `_ZSt9terminatev` → `std::terminate()`

该工具属于 binutils 软件包，在所有安装了开发工具链的 Linux 系统上均可使用。

---
## 15. 特性： 显示文件路径/行号和demangle后的函数名
适配时有如下几处改动：

 a. 本来mysql-server8.0.41已经实现了该功能，但是由于该版本CMakeLists.txt有bug，导致编译宏未打开，在mysql-server8.0.44版本已修复该功能，回合该bugfix Bug#38235429 Add unit test for my_print_stacktrace · mysql/mysql-server@996824e。

 b. 回合后，mysql显示的函数名没有demangle，可读性不强，stacktrace.cc 自带的demangle里面有内存分配，非异步信号安全，可能会出现死锁等问题，因此引入absl::debugging_internal::Demangle异步信号安全的函数。

 c. 原有实现在文件路径上是从'mysql/' 这里截断，但是我们平常编译时路径为'mysql-server'和'dstore'，这块进行适配
 
---
## 16. 实现计划

### 16.1 文件变更汇总

| 文件 | 变更类型 | 变更内容 |
|------|---------|---------|
| `sql/mysqld.cc` | **定点修改**（3 处）| 信号注册 `SA_SIGINFO`；将全局变量重命名为 `rds_ffic_verbose_crash_diagnostics`；删除已废弃的 `my_long_options` 条目 |
| `sql/sys_vars.cc` | **追加** | 为 `rds_ffic_verbose_crash_diagnostics` 注册动态 `Sys_var_bool` |
| `sql/signal_handler.cc` | **定点修改**（4 处）+ **追加** | 处理函数签名；条件变量重命名；线程 ID 扩展；信号发送方信息；崩溃后诊断提示 |
| `mysys/stacktrace.cc` | **仅追加** | 11 个新 `ffic_print_*` 辅助函数 |
| `include/my_stacktrace.h` | **仅追加** | 所有新函数的声明 |

### 16.2 新函数及其位置

| 函数 | 文件 | 平台保护宏 |
|------|------|----------|
| `ffic_print_enhanced_crash_info()` | `sql/signal_handler.cc` | 全平台 |
| `ffic_print_crash_banner()` | `sql/signal_handler.cc` | 非 Win32 |
| `ffic_print_signal_info()` | `sql/signal_handler.cc` | 非 Win32 |
| `ffic_print_post_crash_tips()` | `sql/signal_handler.cc` | 非 Win32 |
| `ffic_print_active_query()` | `sql/signal_handler.cc` | 全平台 |
| `ffic_print_registers()` | `mysys/stacktrace.cc` | Linux x86-64 / AArch64 |
| `ffic_print_register_hints()` | `mysys/stacktrace.cc` | Linux x86-64 / AArch64 |
| `ffic_print_stack_memory()` | `mysys/stacktrace.cc` | Linux x86-64 / AArch64 |
| `ffic_print_instruction_context()` | `mysys/stacktrace.cc` | Linux x86-64 / AArch64 |
| `ffic_print_process_resources()` | `mysys/stacktrace.cc` | Linux |
| `ffic_print_memory_info()` | `mysys/stacktrace.cc` | Linux |
| `ffic_print_system_load()` | `mysys/stacktrace.cc` | Linux |
| `ffic_print_process_limits()` | `mysys/stacktrace.cc` | Linux |
| `ffic_print_loaded_segments()` | `mysys/stacktrace.cc` | Linux |
| `ffic_print_os_info()` | `mysys/stacktrace.cc` | Linux |

### 16.3 建议实现顺序（风险最低优先）

1. 向 `mysys/stacktrace.cc` 追加辅助函数——不触及现有代码
2. 向 `include/my_stacktrace.h` 追加声明——不触及现有代码
3. 在 `sql/sys_vars.cc` 末尾追加 `Sys_var_bool` 注册
4. 修改 `sql/mysqld.cc`：重命名变量、删除 `my_long_options` 条目
5. 修改 `my_init_signals()` 中的信号注册（`SA_SIGINFO`）
6. 修改 `handle_fatal_signal` 签名并向 `sql/signal_handler.cc` 追加新函数

---

## 17. 测试策略

### 17.1 单元测试（`unittest/gunit/`）

- **寄存器转储**：构造已知值的合成 `ucontext_t`；将 stderr 重定向到缓冲区后调用
  `ffic_print_registers()`；断言预期的寄存器名称和十六进制值出现在输出中。覆盖场景：
  `RDI`/`x0` 为空指针、`RSP`/`SP` 未对齐、FP 损坏。
- **栈转储**：用已知模式填充固定数组；调用 `ffic_print_stack_memory()`；断言解读标题和所有
  十六进制字出现在输出中。
- **提示生成**：对 `ffic_print_register_hints()` 使用精心构造的上下文进行独立测试。

### 17.2 信号注入集成测试（`crash_diag_selftest`）

一个独立程序，执行以下步骤：
1. 安装与 `mysqld` 相同的信号处理函数。
2. 故意解引用空指针。
3. 通过子进程+管道捕获崩溃输出。
4. 断言预期的节标题和已知模式出现在输出中。

不经过 MTR；是独立的 `ctest` 测试目标。

### 17.3 手动验证矩阵

| 场景 | 预期输出 |
|------|---------|
| `*(int*)0 = 1` | `RDI`/`x0` 空指针提示；信号信息中出现 `SEGV_MAPERR` |
| 栈溢出（用 `0x41` 填充） | 栈转储中可见 `0x4141…` |
| `abort()`（无上下文） | 打印横幅和查询；寄存器/栈节优雅跳过 |
| `SIGFPE` | `TRAPNO = 0x00` 被解码；显示除零提示 |
| ARM64 | 正确显示 `x0`–`x30` 布局；未编译 x86 代码 |
| `--ffic-verbose-crash-diagnostics=OFF` | 不打印新节；现有输出不变 |

---

## 18. 约束与非目标

- 信号处理函数中任何新代码路径**不得进行堆内存分配**。
- **不引入新的外部库依赖**。所有 `/proc` 解析均使用原始 `read(2)`。
- 本版本**不生成结构化输出格式**（JSON、protobuf）；仅输出纯文本到 stderr。
- **不自动上传崩溃报告**。运维人员使用现有的日志传输机制。
- **ARM32、MIPS、RISC-V、s390x**：代码可正常编译，但不产生寄存器输出（与当前行为一致）。
  基于 `/proc` 的节在任何 Linux 上均可工作。
- **Windows**：异常处理程序（`sql/nt_servc.cc`、`DbgHelp`）是独立代码路径；新函数均用
  `#ifndef _WIN32` 保护，明确超出本设计范围。
- **macOS**：此处未定义 `ucontext_t` 寄存器布局；寄存器节被跳过。
- **数据库配置（GUC）**：按项目要求排除（内容过多）。

---

## 19. 完整输出示例

```
2026-03-26T14:32:01.123456Z 4 [ERROR] [MY-013183] [Server] Aborting
... （现有 mysqld 崩溃输出） ...

=== MYSQLD CRASH SUMMARY ===
  Signal   : SIGSEGV (Segmentation fault)
  Reason   : SEGV_MAPERR -- attempt to access an address not mapped in the object
  Fault @  : 0x0000000000000020  (NULL + 32 bytes -- likely null-pointer dereference)
  Thread ID: 4    Query ID: 12345

=== Signal Information ===
  si_signo = 11 (SIGSEGV)
  si_code  = 1  (SEGV_MAPERR: address not mapped to object)
  si_addr  = 0x0000000000000020

=== Active Query ===
  Thread ID : 4
  Command   : Query
  Killed    : NOT_KILLED
  Query     : SELECT * FROM orders JOIN shipments ON orders.id = shipments.order_id LIMIT 1000

=== CPU Registers (x86_64) ===
  ...（寄存器值）...

=== Register Diagnostic Hints ===
  [RIP = 0x00007f3b4c1a2d40] -- Faulting instruction address.
    addr2line -e mysqld -f -C -p 0x7f3b4c1a2d40

  [RDI = 0x0000000000000000] -- First function argument is NULL or near-NULL.
    This is the strongest indicator of a null-pointer dereference.

=== Stack Memory Dump ===
  ...（栈内存十六进制）...

=== Instruction Bytes Around Fault PC ===
  >> 0x00007f3b4c1a2d38: 48 8b 40 20 48 8b 00 eb

=== Process Resources (/proc/self/status) ===
  VmRSS:   4201 MB
  Threads: 42

=== Loaded Segments (/proc/self/maps, selected) ===
  7f3b4c000000-7f3b4c300000 r-xp  /usr/sbin/mysqld

=== OS Information ===
  Sysname : Linux
  Release : 5.15.0-91-generic
  Machine : x86_64

Crash fingerprint: ab12cd34
```

---