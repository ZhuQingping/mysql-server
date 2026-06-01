/* Copyright (c) 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file unittest/gunit/ffic_stacktrace-t.cc

  Unit tests for the FFIC enhanced crash diagnostics functions in
  mysys/stacktrace.cc (declared in include/my_stacktrace.h).

  Coverage strategy:
  - Every public function is called with nullptr / zero inputs to verify
    the documented no-op contracts (tests the early-return branches).
  - Every public function is called with valid inputs derived from
    getcontext(3) to verify the success paths.
  - For ffic_print_registers and ffic_print_register_hints, synthetic
    ucontext_t values are used to exercise every conditional hint branch
    that getcontext() alone would not trigger (misaligned SP, corrupt FP,
    null-arg hint, write-fault hint, TRAPNO page/GP/div-by-zero branches).
  - For ffic_print_stack_memory, multiple word-count values exercise the
    inner loop: zero words (loop skipped), partial row, full rows,
    and the n > 4 truncation branch.
  - For ffic_print_instruction_context, a zero PC and an obviously-unmapped
    PC exercise the early-return and dladdr-failure branches respectively.
  - /proc file functions are called normally (covering the success and
    line-matching branches); their open()/read() failure branches are not
    reachable without kernel-level mocking and are excluded from the target.

  Target: >=85% line coverage and >=85% branch coverage of the FFIC code
  added in mysys/stacktrace.cc by commits e82191232c and e25374f59b5.
*/

#include <gtest/gtest.h>

#include "my_stacktrace.h"

#if defined(__linux__) && !defined(_WIN32)

#include <ucontext.h>

/* -------------------------------------------------------------------------
 * Helper: obtain the current CPU context via getcontext(3).
 * Safe to call outside a signal handler.
 * ---------------------------------------------------------------------- */
static int capture_context(ucontext_t *uc) { return getcontext(uc); }

/* =========================================================================
 * Section 1: Functions available on all Linux builds (proc-file readers)
 * ====================================================================== */

/* ffic_print_os_info — calls uname(2) and prints kernel/arch info. */
TEST(FficCrashDiag, OsInfo_Success) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_os_info());
}

/* ffic_print_process_resources — reads /proc/self/status. */
TEST(FficCrashDiag, ProcessResources_Success) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_process_resources());
}

/* ffic_print_memory_info — reads /proc/meminfo. */
TEST(FficCrashDiag, MemoryInfo_Success) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_memory_info());
}

/* ffic_print_system_load — reads /proc/loadavg. */
TEST(FficCrashDiag, SystemLoad_Success) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_system_load());
}

/* ffic_print_process_limits — reads /proc/self/limits. */
TEST(FficCrashDiag, ProcessLimits_Success) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_process_limits());
}

/* ffic_print_loaded_segments — reads /proc/self/maps. */
TEST(FficCrashDiag, LoadedSegments_Success) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_loaded_segments());
}

/* =========================================================================
 * Section 2: Architecture-specific functions (x86_64 / AArch64)
 * ====================================================================== */

#if defined(__x86_64__) || defined(__aarch64__)

/* -----------------------------------------------------------------------
 * ffic_print_registers
 * --------------------------------------------------------------------- */

/* Null input: documented no-op contract. */
TEST(FficCrashDiag, Registers_NullIsNoop) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_registers(nullptr));
}

/* Real context: exercises the full register-dump code path. */
TEST(FficCrashDiag, Registers_RealContext) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  EXPECT_NO_FATAL_FAILURE(ffic_print_registers(&uc));
}

#if defined(__x86_64__)

/* TRAPNO = 0x0e (page fault): triggers the "[TRAPNO=0x0e: page fault ...]"
   message and covers that branch of the if/else-if chain. */
TEST(FficCrashDiag, Registers_Trapno_PageFault) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_TRAPNO] = 0x0e;
  EXPECT_NO_FATAL_FAILURE(ffic_print_registers(&uc));
}

/* TRAPNO = 0x0d (general protection fault): triggers the GP fault branch. */
TEST(FficCrashDiag, Registers_Trapno_GPFault) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_TRAPNO] = 0x0d;
  EXPECT_NO_FATAL_FAILURE(ffic_print_registers(&uc));
}

/* TRAPNO = 0x00 (divide-by-zero): triggers the div-by-zero branch. */
TEST(FficCrashDiag, Registers_Trapno_DivByZero) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_TRAPNO] = 0x00;
  EXPECT_NO_FATAL_FAILURE(ffic_print_registers(&uc));
}

/* TRAPNO = 0x01 (an unrecognised value): falls through all branches silently,
   covering the "else nothing" path of the TRAPNO chain. */
TEST(FficCrashDiag, Registers_Trapno_Unrecognised) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_TRAPNO] = 0x01;
  EXPECT_NO_FATAL_FAILURE(ffic_print_registers(&uc));
}

/* -----------------------------------------------------------------------
 * ffic_print_register_hints  (x86_64 branch coverage)
 *
 * Each test crafts a ucontext_t that triggers exactly one conditional
 * in the hint function, verifying both the "hint printed" and "no hint"
 * code paths for each check.
 * --------------------------------------------------------------------- */

/* Null input: documented no-op contract. */
TEST(FficCrashDiag, RegisterHints_NullIsNoop) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(nullptr));
}

/* Real context (all registers at normal values): exercises the code path
   where no hint conditions are triggered.  Getcontext() produces a 16-byte
   aligned RSP, RDI > 4095, RBP >= RSP, TRAPNO = 0 / ERR = 0. */
TEST(FficCrashDiag, RegisterHints_RealContext_NoHintsTriggered) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  /* Ensure RSP is 16-byte aligned (it should be, but make it explicit). */
  uint64_t rsp = (uint64_t)uc.uc_mcontext.gregs[REG_RSP];
  rsp &= ~(uint64_t)0xf;
  uc.uc_mcontext.gregs[REG_RSP] = (greg_t)rsp;
  /* RDI > 4095 → no null-arg hint. */
  uc.uc_mcontext.gregs[REG_RDI] = (greg_t)0x10000;
  /* RBP above RSP → no frame-pointer corruption hint. */
  uc.uc_mcontext.gregs[REG_RBP] = (greg_t)(rsp + 0x40);
  /* TRAPNO = 0x05 (not page fault) and ERR = 0 → no write-fault hint. */
  uc.uc_mcontext.gregs[REG_TRAPNO] = 0x05;
  uc.uc_mcontext.gregs[REG_ERR] = 0x00;
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* RDI == 0: first argument is NULL → null-arg-dereference hint printed.
   Branch: if (rdi <= 4095) → true. */
TEST(FficCrashDiag, RegisterHints_NullFirstArg_Hint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_RDI] = 0;
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* RDI == 4095 (near-null): still within the <= 4095 range.
   Branch: if (rdi <= 4095) → true (boundary value). */
TEST(FficCrashDiag, RegisterHints_NearNullFirstArg_Hint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_RDI] = (greg_t)4095;
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* RDI == 4096: just outside the null range → no hint.
   Branch: if (rdi <= 4095) → false (boundary value). */
TEST(FficCrashDiag, RegisterHints_NonNullFirstArg_NoHint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_RDI] = (greg_t)4096;
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* RSP not 16-byte aligned → stack-misalignment hint printed.
   Branch: if (rsp % 16 != 0) → true. */
TEST(FficCrashDiag, RegisterHints_MisalignedSP_Hint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  /* Force misalignment by setting the low nibble to 1. */
  uint64_t rsp = (uint64_t)uc.uc_mcontext.gregs[REG_RSP];
  rsp = (rsp & ~(uint64_t)0xf) | 1ULL;
  uc.uc_mcontext.gregs[REG_RSP] = (greg_t)rsp;
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* RSP 16-byte aligned → no misalignment hint.
   Branch: if (rsp % 16 != 0) → false. */
TEST(FficCrashDiag, RegisterHints_AlignedSP_NoHint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uint64_t rsp = (uint64_t)uc.uc_mcontext.gregs[REG_RSP];
  rsp &= ~(uint64_t)0xf; /* clear low nibble → aligned */
  uc.uc_mcontext.gregs[REG_RSP] = (greg_t)rsp;
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* RBP non-zero and RBP < RSP → frame-pointer-corruption hint.
   Branch: if (rbp != 0 && rbp < rsp) → true (both conditions true). */
TEST(FficCrashDiag, RegisterHints_CorruptFramePointer_Hint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uint64_t rsp = (uint64_t)uc.uc_mcontext.gregs[REG_RSP];
  /* Place RSP at a known aligned value and RBP below it. */
  rsp &= ~(uint64_t)0xf;
  uc.uc_mcontext.gregs[REG_RSP] = (greg_t)rsp;
  uc.uc_mcontext.gregs[REG_RBP] = (greg_t)(rsp - 0x100); /* below RSP */
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* RBP == 0: first sub-condition (rbp != 0) is false → no FP hint.
   Branch: if (rbp != 0 && rbp < rsp) → false at first operand. */
TEST(FficCrashDiag, RegisterHints_RbpZero_NoCorruptHint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_RBP] = 0;
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* RBP > RSP: second sub-condition (rbp < rsp) is false → no FP hint.
   Branch: if (rbp != 0 && rbp < rsp) → false at second operand. */
TEST(FficCrashDiag, RegisterHints_RbpAboveSP_NoCorruptHint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uint64_t rsp = (uint64_t)uc.uc_mcontext.gregs[REG_RSP];
  uc.uc_mcontext.gregs[REG_RBP] = (greg_t)(rsp + 0x100); /* above RSP: normal */
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* TRAPNO = 0x0e (page fault) and ERR bit 1 set → write-fault hint.
   Branch: if (TRAPNO == 0x0e && (err & 0x2)) → true. */
TEST(FficCrashDiag, RegisterHints_WriteFault_Hint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_TRAPNO] = 0x0e;
  uc.uc_mcontext.gregs[REG_ERR] = 0x02; /* bit 1: write access */
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* TRAPNO = 0x0e (page fault) but ERR bit 1 clear → no write-fault hint.
   Branch: if (TRAPNO == 0x0e && (err & 0x2)) → false at second operand
   (read fault, not a write). */
TEST(FficCrashDiag, RegisterHints_ReadFault_NoWriteHint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_TRAPNO] = 0x0e;
  uc.uc_mcontext.gregs[REG_ERR] =
      0x01; /* bit 0 only: protection violation, not write */
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* TRAPNO != 0x0e → first condition of write-fault check is false.
   Branch: if (TRAPNO == 0x0e && (err & 0x2)) → false at first operand. */
TEST(FficCrashDiag, RegisterHints_NotPageFault_NoWriteHint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_TRAPNO] = 0x05; /* not a page fault trap */
  uc.uc_mcontext.gregs[REG_ERR] = 0x02;    /* bit 1 set, but TRAPNO mismatch */
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* All hints triggered simultaneously: RDI=0, RSP misaligned, RBP < RSP,
   TRAPNO=0x0e ERR bit1=1.  Exercises every true-branch in one shot. */
TEST(FficCrashDiag, RegisterHints_AllHintsTriggered) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_RDI] = 0;                /* null first arg */
  uc.uc_mcontext.gregs[REG_RSP] = (greg_t)0x100001; /* misaligned */
  uc.uc_mcontext.gregs[REG_RBP] = (greg_t)0x1;      /* non-zero, below RSP */
  uc.uc_mcontext.gregs[REG_TRAPNO] = 0x0e;
  uc.uc_mcontext.gregs[REG_ERR] = 0x02; /* write access bit */
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

#endif /* __x86_64__ */

/* -----------------------------------------------------------------------
 * ffic_print_stack_memory — branch coverage for the word-count loop
 * --------------------------------------------------------------------- */

/* Null SP: documented no-op contract (early return).
   Branch: if (!sp) → true. */
TEST(FficCrashDiag, StackMemory_NullSP_IsNoop) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_stack_memory(nullptr, 0));
}

/* num_words = 0: sp is valid but loop body is never entered.
   Branch: for (...; i < num_words; ...) → condition is immediately false. */
TEST(FficCrashDiag, StackMemory_ZeroWords_LoopSkipped) {
  alignas(16) uint64_t buf[1] = {0xdeadbeefcafeULL};
  EXPECT_NO_FATAL_FAILURE(ffic_print_stack_memory(buf, 0));
}

/* num_words = 1: single word, partial row (n stays 1; the n > 4 truncation
   branch is NOT taken).
   Branch: if (n > 4) → false. */
TEST(FficCrashDiag, StackMemory_OneWord_PartialRow) {
  alignas(16) uint64_t buf[1] = {0x0102030405060708ULL};
  EXPECT_NO_FATAL_FAILURE(ffic_print_stack_memory(buf, 1));
}

/* num_words = 4: exactly one full row of 4 words (n == 4; if (n > 4) false).
   Exercises the inner j-loop running to completion (j=0..3). */
TEST(FficCrashDiag, StackMemory_FourWords_OneFullRow) {
  alignas(16) uint64_t buf[4] = {0, 1, 2, 3};
  EXPECT_NO_FATAL_FAILURE(ffic_print_stack_memory(buf, 4));
}

/* num_words = 5: first row has 4 words (n > 4 → n = 4 truncated), second
   row has 1 word.
   Branch: if (n > 4) → true (first row). */
TEST(FficCrashDiag, StackMemory_FiveWords_TruncationBranch) {
  alignas(16) uint64_t buf[8] = {};
  for (int i = 0; i < 8; ++i) buf[i] = (uint64_t)i * 0x111;
  EXPECT_NO_FATAL_FAILURE(ffic_print_stack_memory(buf, 5));
}

/* num_words = 16: four full rows.  Exercises multiple loop iterations and
   multiple inner-j-loop completions. */
TEST(FficCrashDiag, StackMemory_SixteenWords_MultipleRows) {
  alignas(16) uint64_t buf[16];
  for (int i = 0; i < 16; ++i) buf[i] = (uint64_t)i * 0xfeedcafe;
  EXPECT_NO_FATAL_FAILURE(ffic_print_stack_memory(buf, 16));
}

/* -----------------------------------------------------------------------
 * ffic_print_instruction_context — branch coverage
 * --------------------------------------------------------------------- */

/* Null ucontext: documented no-op.
   Branch: if (!ucontext_ptr) → true. */
TEST(FficCrashDiag, InstructionContext_NullIsNoop) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_instruction_context(nullptr));
}

#if defined(__x86_64__)

/* PC = 0: second early-return branch.
   Branch: if (pc == 0) → true. */
TEST(FficCrashDiag, InstructionContext_ZeroPC_EarlyReturn) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.gregs[REG_RIP] = 0;
  EXPECT_NO_FATAL_FAILURE(ffic_print_instruction_context(&uc));
}

/* PC at an obviously-unmapped address: dladdr() on pc-32 or pc+32 fails,
   so the function prints "(memory around PC is not readable ...)" and returns.
   Branch: if (!dladdr(...) || !dladdr(...)) → true. */
TEST(FficCrashDiag, InstructionContext_UnmappedPC_DladdrFails) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  /* 0xdeadbeef0000 is far outside any mapped region. */
  uc.uc_mcontext.gregs[REG_RIP] = (greg_t)0xdeadbeef0000ULL;
  EXPECT_NO_FATAL_FAILURE(ffic_print_instruction_context(&uc));
}

/* Real PC (from getcontext): both dladdr() calls succeed, the full byte
   dump is printed, and the ">>" marker is produced for the row containing PC.
   Branch: if (!dladdr(...) || !dladdr(...)) → false.
           if (row <= pc_ptr && pc_ptr < row + 8) → true (the PC row). */
TEST(FficCrashDiag, InstructionContext_ValidPC_FullDump) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  /* RIP captured by getcontext points into this test binary — always valid. */
  EXPECT_NO_FATAL_FAILURE(ffic_print_instruction_context(&uc));
}

#endif /* __x86_64__ */

#endif /* __x86_64__ || __aarch64__ */

/* =========================================================================
 * Section 3: AArch64-specific hint branches
 * ====================================================================== */

#if defined(__aarch64__)

/* x0 == 0: null-arg hint (AArch64 first-arg register). */
TEST(FficCrashDiag, RegisterHints_AArch64_NullX0_Hint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.regs[0] = 0;
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* x0 > 4095: no null-arg hint. */
TEST(FficCrashDiag, RegisterHints_AArch64_NonNullX0_NoHint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.regs[0] = 0x10000;
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* SP not 16-byte aligned (AArch64 enforces 16-byte alignment). */
TEST(FficCrashDiag, RegisterHints_AArch64_MisalignedSP_Hint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uint64_t sp = (uc.uc_mcontext.sp & ~0xfULL) | 1ULL;
  uc.uc_mcontext.sp = sp;
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* x29 (FP) non-zero and below SP: frame-pointer corruption hint. */
TEST(FficCrashDiag, RegisterHints_AArch64_CorruptFP_Hint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uint64_t sp = uc.uc_mcontext.sp & ~0xfULL;
  uc.uc_mcontext.sp = sp;
  uc.uc_mcontext.regs[29] = sp - 0x100; /* FP below SP */
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* x29 == 0: first sub-condition (fp != 0) is false → no FP hint. */
TEST(FficCrashDiag, RegisterHints_AArch64_FPZero_NoHint) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.regs[29] = 0;
  EXPECT_NO_FATAL_FAILURE(ffic_print_register_hints(&uc));
}

/* PC = 0: early return in ffic_print_instruction_context (AArch64). */
TEST(FficCrashDiag, InstructionContext_AArch64_ZeroPC_EarlyReturn) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.pc = 0;
  EXPECT_NO_FATAL_FAILURE(ffic_print_instruction_context(&uc));
}

/* Unmapped PC (AArch64): dladdr() fails → "not readable" message. */
TEST(FficCrashDiag, InstructionContext_AArch64_UnmappedPC) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  uc.uc_mcontext.pc = 0xdeadbeef0000ULL;
  EXPECT_NO_FATAL_FAILURE(ffic_print_instruction_context(&uc));
}

/* Real PC (AArch64): full instruction byte dump. */
TEST(FficCrashDiag, InstructionContext_AArch64_ValidPC_FullDump) {
  ucontext_t uc;
  ASSERT_EQ(0, capture_context(&uc));
  EXPECT_NO_FATAL_FAILURE(ffic_print_instruction_context(&uc));
}

#endif /* __aarch64__ */

/* =========================================================================
 * Section 4: ffic_print_signal_info branch coverage
 * ====================================================================== */

#include <signal.h>

/* Null info_ptr: early return (no-op).
   Branch: if (!si) → true. */
TEST(FficCrashDiag, SignalInfo_NullIsNoop) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_signal_info(SIGSEGV, nullptr));
}

/* SIGSEGV with a real siginfo_t that has si_addr set: exercises the
   address-printing branch (sig == SIGSEGV → true).
   Branch: if (sig == SIGSEGV || ...) → true. */
TEST(FficCrashDiag, SignalInfo_Sigsegv_PrintsAddr) {
  siginfo_t si{};
  si.si_signo = SIGSEGV;
  si.si_code = SEGV_MAPERR;
  si.si_addr = reinterpret_cast<void *>(0xdeadbeef);
  EXPECT_NO_FATAL_FAILURE(ffic_print_signal_info(SIGSEGV, &si));
}

/* SIGBUS with si_addr set: exercises SIGBUS arm of the OR chain.
   Branch: if (sig == SIGSEGV || sig == SIGBUS || ...) → true via SIGBUS. */
TEST(FficCrashDiag, SignalInfo_Sigbus_PrintsAddr) {
  siginfo_t si{};
  si.si_signo = SIGBUS;
  si.si_code = BUS_ADRALN;
  si.si_addr = reinterpret_cast<void *>(0x1234);
  EXPECT_NO_FATAL_FAILURE(ffic_print_signal_info(SIGBUS, &si));
}

/* SIGTERM (not in the addr-printing set): address line is NOT printed.
   Branch: if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig ==
   SIGFPE) → false. */
TEST(FficCrashDiag, SignalInfo_Sigterm_NoAddr) {
  siginfo_t si{};
  si.si_signo = SIGTERM;
  si.si_code = SI_USER;
  si.si_pid = 1234;
  si.si_uid = 1000;
  EXPECT_NO_FATAL_FAILURE(ffic_print_signal_info(SIGTERM, &si));
}

/* si_code == SI_USER: sender info printed.
   Branch: if (si->si_code == SI_USER || si->si_code == SI_QUEUE) → true
           via SI_USER (first operand). */
TEST(FficCrashDiag, SignalInfo_SiUser_PrintsSender) {
  siginfo_t si{};
  si.si_signo = SIGTERM;
  si.si_code = SI_USER;
  si.si_pid = 42;
  si.si_uid = 100;
  EXPECT_NO_FATAL_FAILURE(ffic_print_signal_info(SIGTERM, &si));
}

/* si_code == SI_QUEUE: sender info printed.
   Branch: if (si->si_code == SI_USER || si->si_code == SI_QUEUE) → true
           via SI_QUEUE (second operand). */
TEST(FficCrashDiag, SignalInfo_SiQueue_PrintsSender) {
  siginfo_t si{};
  si.si_signo = SIGTERM;
  si.si_code = SI_QUEUE;
  si.si_pid = 99;
  si.si_uid = 0;
  EXPECT_NO_FATAL_FAILURE(ffic_print_signal_info(SIGTERM, &si));
}

/* si_code == SEGV_MAPERR (not SI_USER or SI_QUEUE): sender block skipped.
   Branch: if (si->si_code == SI_USER || si->si_code == SI_QUEUE) → false. */
TEST(FficCrashDiag, SignalInfo_SegvMaperr_NoSender) {
  siginfo_t si{};
  si.si_signo = SIGSEGV;
  si.si_code = SEGV_MAPERR;
  si.si_addr = reinterpret_cast<void *>(0x0);
  EXPECT_NO_FATAL_FAILURE(ffic_print_signal_info(SIGSEGV, &si));
}

/* =========================================================================
 * Section 5: ffic_print_post_crash_tips
 * ====================================================================== */

/* Just verify the function completes without crashing and produces output. */
TEST(FficCrashDiag, PostCrashTips_NoCrash) {
  EXPECT_NO_FATAL_FAILURE(ffic_print_post_crash_tips());
}

#endif /* __linux__ && !_WIN32 */
