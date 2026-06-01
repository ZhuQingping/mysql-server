/* Copyright (c) 2001, 2024, Oracle and/or its affiliates.

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

#ifndef MY_STACKTRACE_INCLUDED
#define MY_STACKTRACE_INCLUDED

/**
  @file include/my_stacktrace.h
*/

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "my_compiler.h"
#include "my_config.h"
#include "my_inttypes.h"
#include "my_macros.h"

/*
  HAVE_BACKTRACE - Linux, FreeBSD, OSX, Solaris
  _WIN32 - Windows
  HAVE_EXT_BACKTRACE - Unixes without backtrace(3)
*/
#if defined(HAVE_BACKTRACE) || defined(_WIN32) || defined(HAVE_EXT_BACKTRACE)
#define HAVE_STACKTRACE 1
void my_init_stacktrace();
void my_print_stacktrace(const uchar *stack_bottom, ulong thread_stack);
void my_safe_puts_stderr(const char *val, size_t max_len);
#if defined(HAVE_EXT_BACKTRACE)
/**
  loading symbols is slow, we can warm it at startup
 */
void my_warmup_ext_backtrace();
#endif

#ifdef _WIN32
void my_set_exception_pointers(EXCEPTION_POINTERS *ep);
void my_create_minidump(const char *name, HANDLE process, DWORD pid);
#endif
#endif /* HAVE_BACKTRACE || _WIN32 || HAVE_EXT_BACKTRACE */

void my_write_core(int sig);

/**
  Async-signal-safe utility functions used by signal handler routines.
  Declared here in order to unit-test them.
  These are not general-purpose, but tailored to the signal handling routines.
*/
/**
  Converts a longlong value to string.
  @param   base 10 for decimal, 16 for hex values (0..9a..f)
  @param   val  The value to convert
  @param   buf  Assumed to point to the *end* of the buffer.
  @returns Pointer to the first character of the converted string.
           Negative values:
           for base-10 the return string will be prepended with '-'
           for base-16 the return string will contain 16 characters
  Implemented with simplicity, and async-signal-safety in mind.
*/
char *my_safe_itoa(int base, longlong val, char *buf);

/**
  Converts a ulonglong value to string.
  @param   base 10 for decimal, 16 for hex values (0..9a..f)
  @param   val  The value to convert
  @param   buf  Assumed to point to the *end* of the buffer.
  @returns Pointer to the first character of the converted string.
  Implemented with simplicity, and async-signal-safety in mind.
*/
char *my_safe_utoa(int base, ulonglong val, char *buf);

/**
  A (very) limited version of snprintf.
  @param   to   Destination buffer.
  @param   n    Size of destination buffer.
  @param   fmt  printf() style format string.
  @returns Number of bytes written, including terminating '\0'
  Supports 'd' 'i' 'u' 'x' 'p' 's' conversion.
  Supports 'l' and 'll' modifiers for integral types.
  Does not support any width/precision.
  Implemented with simplicity, and async-signal-safety in mind.
*/
size_t my_safe_snprintf(char *to, size_t n, const char *fmt, ...)
    MY_ATTRIBUTE((format(printf, 3, 4)));

/**
  A (very) limited version of snprintf, which writes the result to STDERR.
  @sa my_safe_snprintf
  Implemented with simplicity, and async-signal-safety in mind.
  @note Has an internal buffer capacity of 512 bytes,
  which should suffice for our signal handling routines.
*/
size_t my_safe_printf_stderr(const char *fmt, ...)
    MY_ATTRIBUTE((format(printf, 1, 2)));

/**
  Writes up to count bytes from buffer to STDERR.
  Implemented with simplicity, and async-signal-safety in mind.
  @param   buf   Buffer containing data to be written.
  @param   count Number of bytes to write.
  @returns Number of bytes written.
*/
size_t my_write_stderr(const void *buf, size_t count);

/**
  Writes system time to STDERR without allocating new memory.
*/
void my_safe_print_system_time();

/* -----------------------------------------------------------------------
 * FFIC: Enhanced Crash Diagnostics
 * All declarations below are new additions.  No existing code was changed.
 * ----------------------------------------------------------------------- */
#if defined(__linux__) && !defined(_WIN32)
#if defined(__x86_64__) || defined(__aarch64__)
/**
 * Print a full CPU register dump (x86_64 or AArch64) with a brief legend.
 * @param ucontext_ptr  Pointer to ucontext_t from the SA_SIGINFO handler,
 *                      or nullptr (no-op).
 */
void ffic_print_registers(const ucontext_t *ucontext_ptr);

/**
 * Print plain-language diagnostic hints derived from register values
 * (null-pointer detection, stack misalignment, frame-pointer corruption,
 * write-fault detection).
 * @param ucontext_ptr  Pointer to ucontext_t, or nullptr.
 */
void ffic_print_register_hints(const ucontext_t *ucontext_ptr);

/**
 * Print a hex dump of the top-of-stack memory with a full interpretation
 * guide explaining what the stack is and how to recognise common problems.
 * @param sp        Stack pointer at crash time.
 * @param num_words Number of pointer-sized words to dump (suggested: 32).
 */
void ffic_print_stack_memory(const uint64_t *sp, size_t num_words);

/**
 * Print the raw instruction bytes around the faulting PC.
 * Memory validity is verified with dladdr() before any reads.
 * @param ucontext_ptr  Pointer to ucontext_t, or nullptr.
 */
void ffic_print_instruction_context(const ucontext_t *ucontext_ptr);

#endif /* __x86_64__ || __aarch64__ */

/** Print selected /proc/self/status lines (VmRSS, VmPeak, VmStk, Threads). */
void ffic_print_process_resources();

/**
 * Print system-wide memory from /proc/meminfo.
 * Reports MemTotal, MemFree, MemAvailable, Buffers, Cached, SwapTotal,
 * SwapFree. A very low MemAvailable suggests an OOM condition caused the crash.
 */
void ffic_print_memory_info();

/**
 * Print system load averages from /proc/loadavg (1/5/15 min).
 * High sustained load may indicate resource saturation before the crash.
 */
void ffic_print_system_load();

/**
 * Print key process resource limits from /proc/self/limits.
 * Reports Max stack size, Max core file size, Max open files, and
 * Max address space. These help diagnose stack overflow, missing core
 * dumps, and file-descriptor exhaustion.
 */
void ffic_print_process_limits();

/** Print executable segments from /proc/self/maps for ASLR resolution. */
void ffic_print_loaded_segments();

/** Print OS kernel version and architecture via uname(2). */
void ffic_print_os_info();

/**
 * Print raw signal information from a siginfo_t pointer.
 * @param sig      Signal number.
 * @param info_ptr Pointer to siginfo_t, or nullptr (no-op).
 */
void ffic_print_signal_info(int sig, const siginfo_t *info_ptr);

/**
 * Print actionable post-crash diagnostic tips (c++filt, addr2line, GDB).
 * Printed last in the crash report so it is immediately visible.
 */
void ffic_print_post_crash_tips();
#endif /* __linux__ && !_WIN32 */

/**
 * Print the complete FFIC enhanced crash report (all sections).
 * Called from handle_fatal_signal() when rds_ffic_verbose_crash_diagnostics
 * is true.
 * @param sig      Signal number.
 * @param info_ptr Pointer to siginfo_t, or nullptr.
 * @param context  Pointer to ucontext_t, or nullptr.
 */
void ffic_print_enhanced_crash_info(int sig, const siginfo_t *info_ptr,
                                    const ucontext_t *context);

#endif  // MY_STACKTRACE_INCLUDED
