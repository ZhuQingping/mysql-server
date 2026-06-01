# Enhanced Crash Diagnostics (FFIC) Feature Specification

> Version: 1.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

When MySQL crashes, the default signal handler only prints a stack trace. This information is often insufficient for effective issue localization — especially for crashes caused by null-pointer dereferences, stack corruption, or memory exhaustion. Engineers must manually gather CPU register state, memory maps, and system-level diagnostics from separate sources.

### 1.2 Solution

When `rds_ffic_verbose_crash_diagnostics` is enabled, `handle_fatal_signal()` emits a structured extended crash report covering:
- CPU registers (x86_64 and AArch64) with plain-language diagnostic hints
- Stack-top hex dump
- Instruction bytes around the faulting PC
- FNV-1a crash fingerprint
- Process memory stats (VmRSS, VmPeak, etc.)
- Loaded executable segments (for ASLR resolution)
- OS version and system load
- Active query context

All new code is appended to existing files (`mysys/stacktrace.cc`, `sql/signal_handler.cc`). Changes to existing code are limited to three targeted edits in `sql/mysqld.cc` (SA_SIGINFO registration, global flag, startup option) and a one-line handler signature change in `sql/signal_handler.cc`. All new functions are async-signal-safe.

### 1.3 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| Core diagnostics (`mysys/stacktrace.cc` additions) | 1 | ~880 |
| Signal handler additions (`sql/signal_handler.cc`) | 1 | ~176 |
| New header declarations (`include/my_stacktrace.h`) | 1 | ~100 |
| System variable & startup (`sql/mysqld.cc`, `mysqld.h`, `sys_vars.cc`) | 3 | ~37 |
| Unit tests (`unittest/gunit/ffic_stacktrace-t.cc`) | 1 | ~578 |
| MTR test result updates | ~10 | ~50 |
| Build system (`mysys/CMakeLists.txt`, `unittest/gunit/CMakeLists.txt`) | 2 | ~16 |
| **Total** | **~17** | **~1,837** |

---

## 2. New Declarations

### 2.1 Header File (`include/my_stacktrace.h`)

New function declarations added at the end of the file, guarded by platform macros:

```c++
#if defined(__linux__) && !defined(_WIN32)

/* Crash summary banner */
void ffic_print_crash_banner(int sig, const siginfo_t *si);

/* Raw signal info from siginfo_t */
void ffic_print_signal_info(int sig, const siginfo_t *info_ptr);

/* Process-level diagnostics from /proc */
void ffic_print_process_resources();   // /proc/self/status
void ffic_print_memory_info();         // /proc/meminfo
void ffic_print_system_load();         // /proc/loadavg
void ffic_print_process_limits();      // /proc/self/limits
void ffic_print_loaded_segments();     // /proc/self/maps
void ffic_print_os_info();             // uname(2)

/* Post-crash tips */
void ffic_print_post_crash_tips();

#endif /* __linux__ && !_WIN32 */

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))

/* Register dump */
void ffic_print_registers(const ucontext_t *ucontext_ptr);

/* Register-based diagnostic hints */
void ffic_print_register_hints(const ucontext_t *ucontext_ptr);

/* Stack-top hex dump */
void ffic_print_stack_memory(const uint64_t *sp, size_t num_words);

/* Instruction bytes around faulting PC */
void ffic_print_instruction_context(const ucontext_t *ucontext_ptr);

#endif /* __x86_64__ || __aarch64__ */

/* Entry point: called from handle_fatal_signal() */
void ffic_print_enhanced_crash_info(int sig, const siginfo_t *info_ptr,
                                    const ucontext_t *context);
```

---

## 3. Modifications to Existing MySQL Files

### 3.1 `sql/mysqld.cc`

Three targeted changes:

1. **Handler signature change** — The `handle_fatal_signal` extern declaration is updated to accept `siginfo_t` and `ucontext_t` on non-Windows:

```c++
// Before:
extern "C" void handle_fatal_signal(int sig);

// After:
#ifdef _WIN32
extern "C" void handle_fatal_signal(int sig);
#else
extern "C" void handle_fatal_signal(int sig, siginfo_t *info, void *context);
#endif
```

2. **Global flag declaration**:

```c++
bool rds_ffic_verbose_crash_diagnostics = false;
```

3. **SA_SIGINFO registration** — Signal handler setup uses `sa_sigaction` instead of `sa_handler`:

```c++
// Before:
sa.sa_flags = SA_RESETHAND | SA_NODEFER;
sa.sa_handler = handle_fatal_signal;

// After:
sa.sa_flags = SA_RESETHAND | SA_NODEFER | SA_SIGINFO;
sa.sa_sigaction = handle_fatal_signal;
```

### 3.2 `sql/mysqld.h`

Add declaration:

```c++
extern bool rds_ffic_verbose_crash_diagnostics;
```

### 3.3 `sql/signal_handler.cc`

1. **Handler signature update** — `handle_fatal_signal` takes the three-argument `sa_sigaction` form on non-Windows.

2. **FFIC call** — After the existing stack trace printing, when `rds_ffic_verbose_crash_diagnostics` is true:

```c++
void handle_fatal_signal(int sig, siginfo_t *info, void *context) {
  // ... existing code (print_fatal_signal, etc.) ...
  
  if (rds_ffic_verbose_crash_diagnostics) {
    ffic_print_enhanced_crash_info(sig, info,
        reinterpret_cast<ucontext_t *>(context));
  }
}
```

3. **New functions appended** — `ffic_print_crash_banner()` and `ffic_print_enhanced_crash_info()` are added at the end of the file. No existing code is modified.

### 3.4 `sql/sys_vars.cc`

New system variable:

```c++
static Sys_var_bool Sys_rds_ffic_verbose_crash_diagnostics(
    "rds_ffic_verbose_crash_diagnostics",
    "When ON, mysqld prints an extended FFIC crash report on any fatal signal "
    "(SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT). The report includes CPU "
    "register dumps with diagnostic hints, stack-top memory, instruction bytes "
    "around the faulting PC, system-wide memory stats, process resource limits, "
    "system load averages, loaded segment addresses for offline symbol "
    "resolution, and OS version. Toggle at runtime with SET GLOBAL. Default ON.",
    GLOBAL_VAR(rds_ffic_verbose_crash_diagnostics), CMD_LINE(OPT_ARG),
    DEFAULT(true));
```

### 3.5 `mysys/stacktrace.cc`

All FFIC diagnostic functions are appended to the end of this file (~880 lines added). No existing code is modified. Key functions:

| Function | Purpose |
|----------|---------|
| `ffic_print_registers()` | Dump all general-purpose CPU registers from ucontext_t |
| `ffic_print_register_hints()` | Derive plain-language hints: null-pointer, stack misalignment, frame-pointer corruption, write-fault |
| `ffic_print_stack_memory()` | Hex dump of N pointer-sized words from the stack pointer |
| `ffic_print_instruction_context()` | Raw instruction bytes around faulting PC, verified with `dladdr()` |
| `ffic_print_process_resources()` | Key lines from `/proc/self/status` (VmRSS, VmPeak, VmStk, Threads) |
| `ffic_print_memory_info()` | System memory from `/proc/meminfo` (MemTotal, MemFree, MemAvailable, SwapTotal, SwapFree) |
| `ffic_print_system_load()` | Load averages from `/proc/loadavg` (1/5/15 min) |
| `ffic_print_process_limits()` | Resource limits from `/proc/self/limits` (stack size, core file size, open files, address space) |
| `ffic_print_loaded_segments()` | Executable segments from `/proc/self/maps` for offline ASLR symbol resolution |
| `ffic_print_os_info()` | Kernel version and architecture via `uname(2)` |
| `ffic_print_signal_info()` | Raw siginfo_t fields (si_signo, si_code, si_errno, si_addr) |
| `ffic_print_post_crash_tips()` | Actionable post-crash tips (c++filt, addr2line, GDB commands) |

### 3.6 `mysys/CMakeLists.txt`

Add `libbacktrace` dependency to `mysys_objlib` for FFIC symbol resolution:

```cmake
# FFIC: link libbacktrace for enhanced crash diagnostics
if(HAVE_BACKTRACE AND HAVE_BUILD_ID_SUPPORT)
  target_link_libraries(mysys_objlib PRIVATE backtrace)
endif()
```

### 3.7 `unittest/gunit/CMakeLists.txt`

Add FFIC unit test target:

```cmake
# FFIC unit test
ADD_TEST(ffic_stacktrace
  ffic_stacktrace-t.cc
)
TARGET_LINK_LIBRARIES(ffic_stacktrace mysys ${LIBBACKTRACE})
```

---

## 4. Crash Report Structure

When the feature is active and a crash occurs, the output is organized as follows:

```
=== MYSQLD CRASH SUMMARY ===
  Signal   : SIGSEGV (Segmentation fault)
  Reason   : SEGV_MAPERR -- address not mapped to object
  Fault @  : 0x0  (NULL + 0 bytes -- likely null-pointer dereference)
  MySQL TID: 42    Query ID: 107
             (MySQL internal thread ID, shown in SHOW PROCESSLIST)
  PID      : 12345
  OS TID   : 12347
             (kernel thread ID -- matches 'ps -L -p 12345 | grep 12347')
  Pthread  : 0x7f1234567890
             (in GDB: 'thread find 13965432123456' or 'info threads')

--- Signal Information ---
  si_signo : 11   si_code  : 1   si_errno: 0
  si_addr  : 0x0

--- CPU Registers ---
  RAX      : 0x0000000000000000   RBX      : 0x00007f1234567890
  RCX      : 0x0000000000000001   RDX      : 0x0000000000000000
  RSI      : 0x00007f1234560000   RDI      : 0x0000000000000000
  RBP      : 0x00007ffc12345678   RSP      : 0x00007ffc12345600
  R8       : ...   R9       : ...   R10      : ...
  R11      : ...   R12      : ...   R13      : ...
  R14      : ...   R15      : ...   RIP      : 0x0000555555678900
  EFLAGS   : 0x00010206

--- Register Diagnostic Hints ---
  [HINT] RDI == 0: likely a NULL first-argument dereference (System V ABI arg1)
  [HINT] Stack pointer is 16-byte aligned -- normal
  [HINT] Frame pointer chain looks valid

--- Stack Memory (top 32 words) ---
  0x7ffc12345600: 0x00005555556789ab  0x00007f1234567890  ...
  ...  (hex dump with ASCII)

--- Instruction Bytes around faulting PC ---
  PC = 0x0000555555678900
  -16 bytes: 48 89 e5 53 48 81 ec ...
  - 8 bytes: 89 c7 e8 1a ...
  - 0 bytes: 48 8b 00 <-- faulting instruction (mov rax, [rax])
  + 8 bytes: 48 89 c2 ...

--- FNV-1a Crash Fingerprint ---
  fnv1a_64: 0x1a2b3c4d5e6f7080
  (This fingerprint uniquely identifies this crash location;
   matching fingerprints across instances indicate the same bug.)

--- Process Memory (from /proc/self/status) ---
  VmRSS    :  1234567 kB
  VmPeak   :  2345678 kB
  VmStk    :      136 kB
  Threads  :       32

--- System Memory (from /proc/meminfo) ---
  MemTotal      : 32768000 kB
  MemFree       :  5120000 kB
  MemAvailable  : 18000000 kB
  SwapTotal     :  4096000 kB
  SwapFree      :  4096000 kB

--- System Load ---
  Load average (1/5/15 min): 2.34  1.89  1.45

--- Process Limits (from /proc/self/limits) ---
  Max stack size       : 8192 kB
  Max core file size   : unlimited
  Max open files       : 65536
  Max address space    : unlimited

--- Loaded Segments (from /proc/self/maps) ---
  555555400000-555555800000 r-xp 00000000 /usr/sbin/mysqld
  7f1234000000-7f1236000000 r-xp 00000000 /usr/lib64/libinnodb.so
  ...

--- OS Information ---
  Linux 5.10.0-generic #1 SMP x86_64 GNU/Linux

--- Post-Crash Tips ---
  1. Demangle: c++filt < crash.log
  2. Resolve: addr2line -e /usr/sbin/mysqld -f -C 0x555555678900
  3. Debug:   gdb /usr/sbin/mysqld core
              (gdb) info threads
              (gdb) thread apply all bt
```

---

## 5. System Variable

| Variable | Type | Scope | Default | Dynamic | Description |
|----------|------|-------|---------|---------|-------------|
| `rds_ffic_verbose_crash_diagnostics` | bool | GLOBAL | ON | Yes | Enable extended FFIC crash report on fatal signals |

Startup option: `--rds-ffic-verbose-crash-diagnostics`

Runtime toggle: `SET GLOBAL rds_ffic_verbose_crash_diagnostics = ON/OFF;`

---

## 6. Platform Support

| Platform | Register Dump | Stack Memory | Instruction Context | /proc Readers |
|----------|--------------|-------------|--------------------|--------------|
| Linux x86_64 | Yes | Yes | Yes | Yes |
| Linux AArch64 | Yes | Yes | Yes | Yes |
| Other Linux | No | No | No | Yes |
| Windows | No | No | No | No |

---

## 7. Async-Signal-Safety

All FFIC functions are designed to be async-signal-safe:
- Use only `write()` / `my_safe_printf_stderr()` for output (no stdio buffering)
- Use only system calls and direct `/proc` file reads (no malloc, no mutexes)
- Use `dladdr()` for address validation (async-signal-safe on Linux/glibc)
- Use stack-allocated buffers only (no dynamic allocation)
- Use `my_safe_itoa()` / `my_safe_utoa()` for number formatting

---

## 8. Unit Tests

### 8.1 FFIC Unit Test (`unittest/gunit/ffic_stacktrace-t.cc`)

~578 lines covering all public FFIC functions:

| Test Category | Test Cases |
|--------------|-----------|
| `/proc` readers (all platforms) | `OsInfo_Success`, `ProcessResources_Success`, `MemoryInfo_Success`, `SystemLoad_Success`, `ProcessLimits_Success`, `LoadedSegments_Success` |
| Register dump | `Registers_x86_64_Success`, `Registers_AArch64_Success`, `Registers_NullContext` |
| Register hints | `RegisterHints_NullPointer`, `RegisterHints_StackMisaligned`, `RegisterHints_FramePointerCorrupt`, `RegisterHints_WriteFault` |
| Stack memory | `StackMemory_ZeroWords`, `StackMemory_PartialRow`, `StackMemory_FullRows`, `StackMemory_Truncation` |
| Instruction context | `InstructionContext_ZeroPC`, `InstructionContext_UnmappedPC`, `InstructionContext_ValidPC` |
| Signal info | `SignalInfo_NullInfo`, `SignalInfo_ValidInfo` |
| Crash banner | `CrashBanner_NullSi`, `CrashBanner_SIGSEGV_MapErr` |
| Post-crash tips | `PostCrashTips_Success` |

### 8.2 Prerequisite Unit Test (`unittest/gunit/mysys_stacktrace-t.cc`)

~50 lines for `my_print_stacktrace()`:

| Test | Description |
|------|-------------|
| `PrintStacktrace` | Basic stack trace printing test |

---

## 9. Execution Flow

### 9.1 Signal Handler Registration (Startup)

```
mysqld_main()  [sql/mysqld.cc]
  -> For each signal (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT):
     sa.sa_flags = SA_RESETHAND | SA_NODEFER | SA_SIGINFO;
     sa.sa_sigaction = handle_fatal_signal;
     sigaction(sig, &sa, NULL);
```

The key change is adding `SA_SIGINFO` to the flags and using `sa_sigaction` instead of `sa_handler`, which provides access to `siginfo_t` and `ucontext_t` in the handler.

### 9.2 Crash Report Generation (Runtime)

```
handle_fatal_signal(int sig, siginfo_t *info, void *context)  [sql/signal_handler.cc]
  -> print_fatal_signal(sig)  // existing: print basic signal info
  -> my_print_stacktrace()    // existing: print stack trace
  -> if (rds_ffic_verbose_crash_diagnostics):
     ffic_print_enhanced_crash_info(sig, info, (ucontext_t *)context)
       -> ffic_print_crash_banner(sig, info)
          - Signal name, reason code, fault address
          - MySQL TID, PID, OS TID, pthread ID
       -> ffic_print_signal_info(sig, info)
          - si_signo, si_code, si_errno, si_addr
       -> ffic_print_registers(context)        [x86_64/AArch64 only]
          - All general-purpose registers
       -> ffic_print_register_hints(context)   [x86_64/AArch64 only]
          - Null-pointer detection, stack alignment, frame pointer corruption
       -> ffic_print_stack_memory(sp, 32)      [x86_64/AArch64 only]
          - Hex dump of top 32 words from stack pointer
       -> ffic_print_instruction_context(context) [x86_64/AArch64 only]
          - Raw instruction bytes at -16, -8, 0, +8 from faulting PC
          - Verified with dladdr() for address validity
       -> FNV-1a crash fingerprint
          - Computed from PC + SP + signal code
          - Uniquely identifies crash location across instances
       -> ffic_print_process_resources()
          - /proc/self/status: VmRSS, VmPeak, VmStk, Threads
       -> ffic_print_memory_info()
          - /proc/meminfo: MemTotal, MemFree, MemAvailable, SwapTotal, SwapFree
       -> ffic_print_system_load()
          - /proc/loadavg: 1/5/15 minute averages
       -> ffic_print_process_limits()
          - /proc/self/limits: stack size, core file size, open files, address space
       -> ffic_print_loaded_segments()
          - /proc/self/maps: executable segments for offline ASLR resolution
       -> ffic_print_os_info()
          - uname(): kernel version and architecture
       -> ffic_print_post_crash_tips()
          - Actionable tips: c++filt, addr2line, GDB commands
```

### 9.3 FNV-1a Fingerprint Computation

The crash fingerprint is computed using the FNV-1a hash algorithm on the combination of the faulting program counter, stack pointer, and signal code. This allows grouping identical crashes across different instances:

```c++
uint64_t fnv1a_64(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; i++) {
    hash ^= p[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}
```

---

## 10. Memory Management

All FFIC functions are designed for use in a signal handler context, where normal memory allocation is forbidden:

- **No malloc/free**: All buffers are stack-allocated (typically char buffers of 256-1024 bytes)
- **No stdio**: All output uses `write()` syscall or `my_safe_printf_stderr()` directly to stderr
- **No mutexes**: No locking is performed; all /proc reads use direct `open()`/`read()`/`close()`
- **No new/delete**: All data is in automatic storage duration
- **dladdr()** is used for address validation; it is async-signal-safe on Linux/glibc
- **my_safe_itoa() / my_safe_utoa()** are used for number-to-string conversion without stdio

### Unit Test Memory

The unit test file (`unittest/gunit/ffic_stacktrace-t.cc`, ~578 lines) allocates `ucontext_t` and `siginfo_t` structures on the stack for testing, with no heap allocation.

---

## 11. Prerequisite Patches

Two patches from upstream MySQL are included as prerequisites:

### Patch #1: BUG#37543598 - Do not truncate long function names in demangled stack traces

**Problem**: In case a stack entry including the demangled function name exceeds 512 bytes, it gets truncated. Multiple truncated entries appear concatenated on the same output line.

**Fix**: Use the home-grown printf function only to print numbers and delimiters which are short. For potentially long strings (filenames and demangled functions), write them directly to the output.

**Files**: `mysys/stacktrace.cc` (+10/-6)

### Patch #2: Bug#38235429 - Add unit test for my_print_stacktrace

**Purpose**: Add unit test for `my_print_stacktrace`, for easier testing of the libbacktrace library. Also ensures `mysys_objlib` is built with libbacktrace.

**Files**: `mysys/CMakeLists.txt`, `unittest/gunit/CMakeLists.txt`, `unittest/gunit/mysys_stacktrace-t.cc` (+66/-7)

---

## 12. Limitations

- Only Linux is supported for full diagnostics (register dump, stack memory, instruction bytes)
- x86_64 and AArch64 architectures are supported for register-level diagnostics
- Windows is not supported
- `/proc` filesystem must be available (standard on Linux)
- `libbacktrace` must be available at build time for symbol resolution
- The feature is controlled by a GLOBAL variable; cannot be set per-session
- All functions are async-signal-safe, so they cannot use any non-reentrant library calls

---

## 13. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **BUG#37543598** — Fix stack trace truncation for long function names
2. **Bug#38235429** — Add unit test for `my_print_stacktrace` and libbacktrace build integration
3. **`include/my_stacktrace.h`** — Add FFIC function declarations
4. **`mysys/stacktrace.cc`** — Add all FFIC diagnostic functions (core implementation)
5. **`mysys/CMakeLists.txt`** — Add libbacktrace dependency
6. **`sql/mysqld.cc`** — SA_SIGINFO registration, global flag, handler signature
7. **`sql/mysqld.h`** — Global flag extern declaration
8. **`sql/signal_handler.cc`** — Handler signature update and FFIC call
9. **`sql/sys_vars.cc`** — System variable definition
10. **`unittest/gunit/ffic_stacktrace-t.cc`** — FFIC unit tests
11. **`unittest/gunit/CMakeLists.txt`** — Unit test build target
12. **MTR test result updates** — Update expected results for `--help` and `all_persisted_variables`
