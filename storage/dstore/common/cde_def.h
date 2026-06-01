/*
  Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef __CDE_DEF_H__
#define __CDE_DEF_H__

/* Undefine branch prediction macros to avoid potential conflicts with
other implementations. These are commonly defined in Linux kernel
headers for __builtin_expect optimization, but we undefine here to
ensure clean slate before potential redefinition. */
#undef likely
#undef unlikely

#include <assert.h>
#include <stdio.h>
#include <chrono>
#include <climits>
#include <regex>

#include <mysql/components/services/log_builtins.h>
#include "my_dbug.h"
#include "sql/mysqld.h"

#include "cde_error.h"

#include "log/dstore_log_interface.h"
#include "securec.h"
#include "types/data_types.h"

/** The macro (log_line_init) defined in log_builtins.h depends on log_bi
defined in sql/log.h. */
#include <mysql/components/services/log_builtins.h>

/** Add log prefix [DEBUG].*/
constexpr const char *LOG_PREFIX_DEBUG = "[DSTORE-DEBUG]";
/** Maximum log size (bytes). */
constexpr int CDE_LOG_SIZE = 1024;

/**
To unify the success and failure values in all handler functions, the success
value is defined as CDE_SUCC, and the failure value is defined as CDE_FAIL.
Do not use (ret)/(!ret) or (ret == true/false) as the judgment condition.
Use (ret == CDE_SUCC/CDE_FAIL) instead.
*/
constexpr bool CDE_SUCC = false;
constexpr bool CDE_FAIL = true;

/**
Prints the current thread's backtrace to assist in debugging. This function is
typically used when an assertion fails to provide context about where the error
occurred.
*/
void CdePrintBackTrace();

/** Macro to print the current backtrace. */
#define CDE_PRINT_BACKTRACE() CdePrintBackTrace()

/**
Terminates the program due to a failed assertion, logging the condition,
file, and line number where the failure occurred.

@param[in] expr  The assertion expression that failed
@param[in] file  Source file where the assertion failed
@param[in] line  Line number in the source file where the assertion failed
*/
[[noreturn]] void AssertionFailed(const char *expr, const char *file,
                                  uint64_t line);

/**
Asserts that a condition is true. If the condition evaluates to false, stops the
log adapter instance and triggers an assertion failure which terminates the
program.

@param cond  The condition to check
*/
#define CDE_ASSERT(cond)                          \
  do {                                            \
    if (unlikely(!(cond))) {                      \
      AssertionFailed(#cond, __FILE__, __LINE__); \
    }                                             \
  } while (0)

#ifdef NDEBUG
#define CDE_ASSERT_DEBUG(EXPR)
#else
#define CDE_ASSERT_DEBUG(EXPR) CDE_ASSERT(EXPR)
#endif

#define UNUSED_PARAM(x) (void)(x)

#define CDE_MIN(x, y) ((x) < (y) ? (x) : (y))
#define CDE_MAX(x, y) ((x) < (y) ? (y) : (x))

#ifndef MAX_U8
#define MAX_U8 ((uint8_t)~0)
#endif

#ifndef MAX_U16
#define MAX_U16 ((uint16_t)~0)
#endif

#ifndef MAX_U32
#define MAX_U32 ((uint32_t)~0)
#endif

#ifndef MAX_U64
#define MAX_U64 ((uint64_t)~0)
#endif

#define CDE_UNUSED __attribute__((unused))

#define STRING_CONST_WITH_LEN(s) (s), ((sizeof(s) - 1))
#define STRING_VAR_WITH_LEN(s) (s), (strlen(s))

class PerfException : public std::runtime_error {
 public:
  PerfException(int code, const char *message, const char *file, const int line)
      : std::runtime_error(message), _code(code), _message(message) {
    std::ostringstream o;
    o << message << ",file=" << file << ":" << line;
    _message = o.str();
  }

  int getCode() const { return _code; }

  const char *what() const noexcept override { return _message.c_str(); }
  ~PerfException() {}

 private:
  int _code;             // Error code
  std::string _message;  // Error message
};

#define CDE_THROW(code, message)                                            \
  {                                                                         \
    std::stringstream __msg_sum;                                            \
    __msg_sum << message;                                                   \
                                                                            \
    throw PerfException(code, __msg_sum.str().c_str(), __FILE__, __LINE__); \
  }

//#define PACKED __attribute__((packed))

using sal_clock = std::chrono::steady_clock;
using sal_timep_t = sal_clock::time_point;

/** Define the regular expression type used for log filtering. */
enum class RegexType {
  /** Line-based regular expression filtering. */
  LINE,
  /** Security-related content filtering. */
  SECURITY,
  /** Token-based sensitive information filtering. */
  TOKEN,
  /** Password or passphrase filtering. */
  PASSWORD
};

/** A structure containing regular expression type and corresponding pattern. */
struct RegexConf {
  /** Maximum length of regular expression string. */
  static constexpr int M_REGEX_SIZE = 256;
  /** Regular expression matching type. */
  RegexType m_regexType;
  /** Fixed-length character array storing regular expressions. */
  const char m_regex[M_REGEX_SIZE];
};

/** Global array defining regular expressions for different security filtering
types. Each entry specifies a regex type and its corresponding pattern. */
extern RegexConf g_regexConf[];

/**
This function outputs a formatted message to the MySQL Server's error log
and can record it according to the specified log level.
If isAbort is true, the program will terminate after logging.

@param[in] loglevel         Selects the log level for message printing
@param[in] isAbort          When true, triggers program termination after
                            logging
@param[in] isSecurityFilter When true, the caller wants to security filtering
@param[in] isAddDebug       When true, the caller wants to add [DEBUG-DSTORE] to
                            the log
@param[in] fmt              printf-style format string specifying message format
@param[in] ...              Variable arguments corresponding to the format
                            string
*/
void __attribute__((format(printf, 5, 6)))
CdeLogPrint(enum loglevel loglevel, bool isAbort, bool isSecurityFilter,
            bool isAddDebug, const char *fmt, ...);

#ifndef IS_DSTORE_BACKUP_TOOL

/**
System level logging macro. Used for critical system messages that require
immediate attention.

@param[in]  fmt   Format string for the log message (printf-style)
@param[in]  args  Variable arguments corresponding to the format string
*/
#define CDE_LOG_SYSTEM(fmt, args...) \
  CdeLogPrint(SYSTEM_LEVEL, false, false, false, fmt, ##args)

/**
Error level logging macro. Used for error conditions that need to be logged.

@param[in]  fmt   Format string for the error message
@param[in]  args  Variable arguments corresponding to the format string
*/
#define CDE_LOG_ERROR(fmt, args...) \
  CdeLogPrint(ERROR_LEVEL, false, false, false, fmt, ##args)

/**
Error level logging macro with automatic Dstore error context.
Used for error conditions that need to be logged
with additional Dstore error information.
Automatically appends Dstore error code and message to
the provided log message.

@param[in]  fmt   Format string for the error message (printf-style)
@param[in]  args  Variable arguments corresponding to the format string
*/
#define CDE_LOG_ERROR_WITH_DSTORE_ERROR(fmt, args...)              \
  do {                                                             \
    /** Temporary variables generates error messages. */           \
    long long errCode = GetDstoreErrcode();                        \
    /** Temporary cache area. */                                   \
    char errMsgBuf[CDE_LOG_SIZE] = {0};                            \
    if (errCode == STORAGE_OK || errCode == CDE_ERROR) {           \
      snprintf_s(errMsgBuf, sizeof(errMsgBuf), sizeof(errMsgBuf),  \
                 "[dstore didn't report an error.]");              \
    } else {                                                       \
      snprintf_s(errMsgBuf, sizeof(errMsgBuf), sizeof(errMsgBuf),  \
                 "[error code: %lld, error message: %s]", errCode, \
                 GetDstoreErrmsg());                               \
    }                                                              \
    /* Calling basic logging macros */                             \
    CDE_LOG_ERROR(fmt "%s", ##args, errMsgBuf);                    \
  } while (0)

/**
Warning level logging macro. Used for potentially problematic situations.

@param[in]  fmt   Format string for the warning message
@param[in]  args  Variable arguments corresponding to the format string
*/
#define CDE_LOG_WARN(fmt, args...) \
  CdeLogPrint(WARNING_LEVEL, false, false, false, fmt, ##args)

/**
Warning level logging macro with automatic Dstore error context.
Used for error conditions that need to be logged
with additional Dstore error information.
Automatically appends Dstore error code and message to
the provided log message.

@param[in]  fmt   Format string for the error message (printf-style)
@param[in]  args  Variable arguments corresponding to the format string
*/
#define CDE_LOG_WARN_WITH_DSTORE_ERROR(fmt, args...)               \
  do {                                                             \
    /** Temporary variables generates error messages. */           \
    long long errCode = GetDstoreErrcode();                        \
    /** Temporary cache area. */                                   \
    char errMsgBuf[CDE_LOG_SIZE] = {0};                            \
    if (errCode == STORAGE_OK || errCode == CDE_ERROR) {           \
      snprintf_s(errMsgBuf, sizeof(errMsgBuf), sizeof(errMsgBuf),  \
                 "[dstore didn't report an error.]");              \
    } else {                                                       \
      snprintf_s(errMsgBuf, sizeof(errMsgBuf), sizeof(errMsgBuf),  \
                 "[error code: %lld, error message: %s]", errCode, \
                 GetDstoreErrmsg());                               \
    }                                                              \
    /* Calling basic logging macros */                             \
    CDE_LOG_WARN(fmt "%s", ##args, errMsgBuf);                     \
  } while (0)

/**
Information level logging macro. Used for general informational messages.

@param[in]  fmt   Format string for the info message
@param[in]  args  Variable arguments corresponding to the format string
*/
#define CDE_LOG_INFO(fmt, args...) \
  CdeLogPrint(INFORMATION_LEVEL, false, false, false, fmt, ##args)

/**
Information level logging macro. Used for general informational messages.
this interface does not perform security filtering,
so this kind of error log have [DSTORE-DEBUG] as prefix,
and it won't be exposed to end users, but just for debugging purpose.

@param[in]  fmt   Format string for the info message
@param[in]  args  Variable arguments corresponding to the format string
*/
#define CDE_LOG_INFO_NOT_SECURITY_FILTER(fmt, args...) \
  CdeLogPrint(INFORMATION_LEVEL, false, false, true, fmt, ##args)

/**
Fatal error logging macro. Logs the message and terminates the program by
calling AssertionFailed.

@param[in]  fmt   Format string for the fatal error message
@param[in]  args  Variable arguments corresponding to the format string
*/
#define CDE_LOG_FATAL(fmt, args...) \
  CdeLogPrint(ERROR_LEVEL, true, false, false, fmt, ##args)

/**
Fatal error logging macro with automatic Dstore error context.
Used for error conditions that need to be logged
with additional Dstore error information.
Automatically appends Dstore error code and message to
the provided log message.

@param[in]  fmt   Format string for the error message (printf-style)
@param[in]  args  Variable arguments corresponding to the format string
*/
#define CDE_LOG_FATAL_WITH_DSTORE_ERROR(fmt, args...)              \
  do {                                                             \
    /** Temporary variables generates error messages. */           \
    long long errCode = GetDstoreErrcode();                        \
    /** Temporary cache area. */                                   \
    char errMsgBuf[CDE_LOG_SIZE] = {0};                            \
    if (errCode == STORAGE_OK || errCode == CDE_ERROR) {           \
      snprintf_s(errMsgBuf, sizeof(errMsgBuf), sizeof(errMsgBuf),  \
                 "[dstore didn't report an error.]");              \
    } else {                                                       \
      snprintf_s(errMsgBuf, sizeof(errMsgBuf), sizeof(errMsgBuf),  \
                 "[error code: %lld, error message: %s]", errCode, \
                 GetDstoreErrmsg());                               \
    }                                                              \
    /* Calling basic logging macros */                             \
    CDE_LOG_FATAL(fmt "%s", ##args, errMsgBuf);                    \
  } while (0)

/**
Debug level logging macro. Used only for debug builds.

@param[in]  fmt   Format string for the debug message
@param[in]  args  Variable arguments corresponding to the format string
*/
#define CDE_LOG_DEBUG(fmt, args...) DBUG_PRINT("cde", (fmt, ##args))

#else

#define BACKUP_TOOL_LOG(A, B) DBUG_PRINT(A, B)

/* System Level log */
#define CDE_LOG_SYSTEM(fmt, args...) \
  BACKUP_TOOL_LOG("cde_system", (fmt, ##args))

/* Error Level log */
#define CDE_LOG_ERROR(fmt, args...) BACKUP_TOOL_LOG("cde_error", (fmt, ##args))

/* Warning Level log */
#define CDE_LOG_WARN(fmt, args...) BACKUP_TOOL_LOG("cde_warn", (fmt, ##args))

/* note Level log */
#define CDE_LOG_INFO(fmt, args...) BACKUP_TOOL_LOG("cde_info", (fmt, ##args))

/* debug only Level log */
#define CDE_LOG_DEBUG(fmt, args...) BACKUP_TOOL_LOG("cde_debug", (fmt, ##args))

#endif

#endif  // __CDE_DEF_H__
