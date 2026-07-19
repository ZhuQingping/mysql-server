/* Copyright (c) 2011, 2024, Oracle and/or its affiliates.

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

#include "my_config.h"

#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <algorithm>
#include <atomic>
#include <cinttypes>

#include "lex_string.h"
#include "my_inttypes.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "my_macros.h"
#include "my_stacktrace.h"
#include "my_sys.h"
#include "my_time.h"
#include "sql/mysqld.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"

#ifdef _WIN32
#include <crtdbg.h>

#define SIGNAL_FMT "exception 0x%x"
#else
#define SIGNAL_FMT "signal %d"
#endif

/*
  We are handling signals in this file.
  Any global variables we read should be 'volatile sig_atomic_t' or lock-free
  std::atomic.
 */

/**
  This is used to check if the signal handler is not called again while already
  handling the previous signal, as may happen when either another thread
  triggers it, or a bug in handling causes abort again.
*/
static std::atomic<bool> s_handler_being_processed{false};
/**
  Used to remember if the fatal info was already printed. The info can be
  printed from user threads, but in the fatal signal handler we want to print it
  if and only if the info was not yet printed. User threads after printing the
  info will call abort which will call the handler.
*/
static std::atomic<bool> s_fatal_info_printed{false};

/**
  This function will try to dump relevant debugging information to stderr and
  dump a core image.

  It may be called as part of the signal handler. This fact limits library calls
  that we can perform and much more, @see handle_fatal_signal

  @param sig Signal number
*/
void print_fatal_signal(int sig) {
  s_fatal_info_printed = true;
#ifdef _WIN32
  SYSTEMTIME utc_time;
  GetSystemTime(&utc_time);

  const long year = utc_time.wYear;
  const long month = utc_time.wMonth;
  const long day = utc_time.wDay;

  const long hrs = utc_time.wHour;
  const long mins = utc_time.wMinute;
  const long secs = utc_time.wSecond;
#else
  /* Using time() instead of my_time() to avoid looping */
  const time_t curr_time = time(nullptr);

  // Offset for the UNIX epoch.
  const ulong days_at_timestart = 719528;

  /* Calculate time of day */
  const long total_mins = curr_time / 60;
  const long total_hrs = total_mins / 60;
  const long total_days = (total_hrs / 24) + days_at_timestart;

  const long hrs = total_hrs % 24;
  const long mins = total_mins % 60;
  const long secs = curr_time % 60;

  uint year, month, day;

  get_date_from_daynr(total_days, &year, &month, &day);
#endif

  char hrs_buf[3] = "00";
  char mins_buf[3] = "00";
  char secs_buf[3] = "00";
  my_safe_itoa(10, hrs, &hrs_buf[2]);
  my_safe_itoa(10, mins, &mins_buf[2]);
  my_safe_itoa(10, secs, &secs_buf[2]);

  char year_buf[5] = "0000";
  char month_buf[3] = "00";
  char day_buf[3] = "00";
  my_safe_itoa(10, year, &year_buf[4]);
  my_safe_itoa(10, month, &month_buf[2]);
  my_safe_itoa(10, day, &day_buf[2]);

  my_safe_printf_stderr(
      "%s-%s-%sT%s:%s:%sZ UTC - mysqld got " SIGNAL_FMT " ;\n", year_buf,
      month_buf, day_buf, hrs_buf, mins_buf, secs_buf, sig);

  my_safe_printf_stderr(
      "%s",
      "Most likely, you have hit a bug, but this error can also "
      "be caused by malfunctioning hardware.\n");

#if defined(HAVE_BUILD_ID_SUPPORT)
  my_safe_printf_stderr("BuildID[sha1]=%s\n", server_build_id);
#endif

#ifdef HAVE_STACKTRACE
  THD *thd = current_thd;

  if (!(test_flags & TEST_NO_STACKTRACE)) {
    my_safe_printf_stderr("Thread pointer: 0x%p\n", thd);
    my_safe_printf_stderr(
        "%s",
        "Attempting backtrace. You can use the following "
        "information to find out\n"
        "where mysqld died. If you see no messages after this, something went\n"
        "terribly wrong...\n");
    my_print_stacktrace(
        thd ? pointer_cast<const uchar *>(thd->thread_stack) : nullptr,
        my_thread_stack_size);
  }
  if (thd) {
    const char *kreason = "UNKNOWN";
    switch (thd->killed.load()) {
      case THD::NOT_KILLED:
        kreason = "NOT_KILLED";
        break;
      case THD::KILL_CONNECTION:
        kreason = "KILL_CONNECTION";
        break;
      case THD::KILL_QUERY:
        kreason = "KILL_QUERY";
        break;
      case THD::KILL_TIMEOUT:
        kreason = "KILL_TIMEOUT";
        break;
      case THD::KILLED_NO_VALUE:
        kreason = "KILLED_NO_VALUE";
        break;
    }
    my_safe_printf_stderr(
        "%s",
        "\n"
        "Trying to get some variables.\n"
        "Some pointers may be invalid and cause the dump to abort.\n");

    my_safe_printf_stderr("Query (%p): ", thd->query().str);
    // Community mysql 8.0.42 change the limit to max_allowed_packet(1GB)
    // According to our tests, the print time is approximately 1 second per
    // 1MB of query length. To ensure the instance's RTO (Recovery Time
    // Objective), we limit the query length to less than 1MB.
    my_safe_puts_stderr(thd->query().str,
                        std::min(size_t{1024 * 1024}, thd->query().length));
    my_safe_printf_stderr("Connection ID (thread ID): %u\n", thd->thread_id());
    my_safe_printf_stderr("Status: %s\n\n", kreason);
  }
  my_safe_printf_stderr(
      "%s",
      "The manual page at "
      "http://dev.mysql.com/doc/mysql/en/crashing.html contains\n"
      "information that should help you find out what is causing the crash.\n");

#endif /* HAVE_STACKTRACE */
}

/**
  Handler for fatal signals

  Fatal events (seg.fault, bus error etc.) will trigger this signal handler. The
  handler will try to dump relevant debugging information to stderr and dump a
  core image.

  Signal handlers can only use a set of 'safe' system calls and library
  functions.

  - A list of safe calls in POSIX systems are available at:
  http://pubs.opengroup.org/onlinepubs/009695399/functions/xsh_chap02_04.html
  - For MS Windows, guidelines are available in documentation of the `signal()`
  function:
  https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/signal?view=msvc-160

  @param sig Signal number
*/
#ifdef _WIN32
extern "C" void handle_fatal_signal(int sig) {
  const void *info = nullptr;
  const void *context = nullptr;
#else
extern "C" void handle_fatal_signal(int sig, siginfo_t *info, void *context) {
#endif
  if (s_handler_being_processed) {
    my_safe_printf_stderr("Fatal " SIGNAL_FMT " while backtracing\n", sig);
    _exit(MYSQLD_FAILURE_EXIT); /* Quit without running destructors */
  }

  s_handler_being_processed = true;

  if (!s_fatal_info_printed) {
    print_fatal_signal(sig);
  }

  /* FFIC: emit extended crash diagnostics when the feature is enabled. */
  if (rds_ffic_verbose_crash_diagnostics) {
    ffic_print_enhanced_crash_info(sig, info, (ucontext_t *)context);
  }

  if ((test_flags & TEST_CORE_ON_SIGNAL) != 0) {
    my_safe_printf_stderr("%s", "Writing a core file\n");
    my_write_core(sig);
  }

#ifndef _WIN32
#ifdef HAVE_GCOV
  my_safe_printf_stderr("%s", "Flushing gcov coverage data\n");
  _db_flush_gcov_();
#endif
  /*
     Quit, without running destructors (etc.)
     On Windows, do not terminate, but pass control to exception filter.
  */
  _exit(MYSQLD_FAILURE_EXIT);  // Using _exit(), since exit() is not async
                               // signal safe
#endif
}

/**
  This is a wrapper around abort() which ensures that abort() will be called
  exactly once, as calling it more than once might cause following problems:
  When original abort() is called there is a signal processing triggered, but
  only the first abort() causes the signal handler to be called, all other
  abort()s called by the other threads will cause immediate exit() call, which
  will also terminate the first abort() processing within the signal handler,
  aborting stacktrace printing, core writeout or any other processing.
*/
void my_server_abort() {
  static std::atomic_int aborts_pending{0};
  static std::atomic_bool abort_processing{false};
  /* Broadcast that this thread wants to print the signal info. */
  aborts_pending++;
  /*
    Wait for the exclusive right to print the signal info. This assures the
    output is not interleaved.
  */
  while (abort_processing.exchange(true)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  /*
    This actually takes some time, some or many other threads may call
    my_server_abort in meantime.
  */
  print_fatal_signal(SIGABRT);
  abort_processing = false;
  /*
    If there are no other threads pending abort then we call real abort as the
    last aborting thread. If that succeeds, we are left with a positive
    `aborts_pending`, and it will never go down to zero again. This effectively
    prevents any other thread from calling real `abort`.
  */
  auto left = --aborts_pending;
  if (!left && aborts_pending.compare_exchange_strong(left, 1)) {
    /*
      Wait again for the exclusive right to print the signal info by calling
      the real `abort`. This assures the output is not interleaved with any
      printing from a few lines above, that could start in the meantime.
    */
    while (abort_processing.exchange(true)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    abort();
  }
  /*
    Abort can't return, we will sleep here forever - the algorithm above
    assures exactly one thread, eventually, will call `abort()` and terminate
    the whole program.
  */
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

/* ============================================================
 * FFIC: Enhanced Crash Diagnostics
 * All functions below are new additions appended to this file.
 * No existing code was modified.
 * ============================================================ */

/* System headers required by FFIC functions below. */
#if defined(__linux__) && !defined(_WIN32)
#include <pthread.h>
#include <sys/syscall.h>
#endif
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <ucontext.h>
#endif

#ifndef _WIN32

/**
 * Print a crash summary banner: signal name, fault address, thread info.
 *
 * This is the first section printed, giving an immediate triage verdict
 * before the detailed register/stack sections.
 *
 * @param sig      Signal number.
 * @param si       Pointer to siginfo_t, or nullptr.
 */
static void ffic_print_crash_banner(int sig, const siginfo_t *si) {
  my_safe_printf_stderr("\n=== MYSQLD CRASH SUMMARY ===\n");

  /* Signal name */
  const char *signame = "UNKNOWN";
  if (sig == SIGSEGV)
    signame = "SIGSEGV (Segmentation fault)";
  else if (sig == SIGBUS)
    signame = "SIGBUS  (Bus error)";
  else if (sig == SIGILL)
    signame = "SIGILL  (Illegal instruction)";
  else if (sig == SIGFPE)
    signame = "SIGFPE  (Floating-point exception)";
  else if (sig == SIGABRT)
    signame = "SIGABRT (Abort)";
  my_safe_printf_stderr("  Signal   : %s\n", signame);

  if (si) {
    /* Human-readable si_code */
    const char *reason = nullptr;
    if (sig == SIGSEGV) {
      if (si->si_code == SEGV_MAPERR)
        reason = "SEGV_MAPERR -- address not mapped to object";
      else if (si->si_code == SEGV_ACCERR)
        reason = "SEGV_ACCERR -- access permission denied";
    } else if (sig == SIGBUS) {
      if (si->si_code == BUS_ADRALN)
        reason = "BUS_ADRALN -- invalid address alignment";
      else if (si->si_code == BUS_ADRERR)
        reason = "BUS_ADRERR -- non-existent physical address";
    } else if (sig == SIGFPE) {
      if (si->si_code == FPE_INTDIV)
        reason = "FPE_INTDIV -- integer divide by zero";
      else if (si->si_code == FPE_INTOVF)
        reason = "FPE_INTOVF -- integer overflow";
    } else if (sig == SIGILL) {
      if (si->si_code == ILL_ILLOPC) reason = "ILL_ILLOPC -- illegal opcode";
    }
    if (reason) my_safe_printf_stderr("  Reason   : %s\n", reason);

    /* Fault address with null-pointer hint */
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE) {
      static constexpr uint64_t NULLPTR_BOUND = 4095;
      long long unsigned int addr =
          (long long unsigned int)((uintptr_t)si->si_addr);
      my_safe_printf_stderr("  Fault @  : %p", (uint8_t *)si->si_addr);
      if (addr <= NULLPTR_BOUND)
        my_safe_printf_stderr(
            "  (NULL + %llu bytes -- likely null-pointer dereference)", addr);
      my_safe_printf_stderr("%s", "\n");
    }
  }

  /* Thread and process IDs */
  THD *thd = current_thd;
  if (thd)
    my_safe_printf_stderr(
        "  MySQL TID: %u    Query ID: %" PRIu64 "\n"
        "             (MySQL internal thread ID, shown in SHOW PROCESSLIST)\n",
        thd->thread_id(), (uint64_t)thd->query_id);

  my_safe_printf_stderr("  PID      : %d\n", (int)getpid());

#if defined(__linux__)
  /* Linux kernel thread ID: shown in 'ps -L', 'top -H', and GDB 'info threads'
   */
  pid_t os_tid = (pid_t)syscall(SYS_gettid);
  my_safe_printf_stderr(
      "  OS TID   : %d\n"
      "             (kernel thread ID -- matches 'ps -L -p %d | grep %d')\n",
      (int)os_tid, (int)getpid(), (int)os_tid);

  /* POSIX thread handle: opaque but useful for GDB 'thread find' */
  uint64_t pt = (uint64_t)pthread_self();
  my_safe_printf_stderr(
      "  Pthread  : 0x%lx\n"
      "             (in GDB: 'thread find %lu' or 'info threads')\n",
      pt, pt);
#endif /* __linux__ */
}

#endif /* !_WIN32 */

/**
 * Entry point for the FFIC extended crash report.
 *
 * Called from handle_fatal_signal() when rds_ffic_verbose_crash_diagnostics
 * is true.  Emits all FFIC sections to stderr in order.
 *
 * @param sig      Signal number.
 * @param info_ptr Pointer to siginfo_t (nullptr on Windows / abort path).
 * @param context  Pointer to ucontext_t (nullptr on Windows / abort path).
 */
void ffic_print_enhanced_crash_info(
    int sig, const siginfo_t *info_ptr,
    const ucontext_t *context MY_ATTRIBUTE((unused))) {
#ifndef _WIN32
  ffic_print_crash_banner(sig, info_ptr);
#endif /* !_WIN32 */

#if defined(__linux__) && !defined(_WIN32)
  ffic_print_signal_info(sig, info_ptr);
#endif /* Linux */

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
  /* Register dump and hints */
  ffic_print_registers(context);
  ffic_print_register_hints(context);

  /* Stack-top memory dump */
  if (context) {
    const uint64_t *sp = nullptr;
#if defined(__x86_64__)
    sp = (const uint64_t *)((uintptr_t)context->uc_mcontext.gregs[REG_RSP]);
#elif defined(__aarch64__)
    sp = (const uint64_t *)((uintptr_t)context->uc_mcontext.sp);
#endif
    ffic_print_stack_memory(sp, 32);
  }

  /* Instruction bytes around the faulting PC */
  ffic_print_instruction_context(context);

#endif /* Linux x86_64 / aarch64 */

#if defined(__linux__) && !defined(_WIN32)
  /* System and process diagnostics */
  ffic_print_memory_info();
  ffic_print_process_resources();
  ffic_print_process_limits();
  ffic_print_system_load();
  ffic_print_loaded_segments();
  ffic_print_os_info();
#endif /* Linux */

#if defined(__linux__) && !defined(_WIN32)
  ffic_print_post_crash_tips();
#endif /* Linux */
}

/* ffic_print_post_crash_tips() is now defined in mysys/stacktrace.cc
   and declared in include/my_stacktrace.h. */
