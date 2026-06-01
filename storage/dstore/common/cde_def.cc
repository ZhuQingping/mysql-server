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

#include <execinfo.h>
#include <stdio.h>
#include <string>
#include <thread>

#include "cde_def.h"
#include "mysqld_error.h"

void CdePrintBackTrace() {
  constexpr uint8_t backtraceMaxSize = 100;
  int backtraceSize;
  void *buffer[backtraceMaxSize];
  char **info;

  /* Obtain the backtrack information of the current function */
  backtraceSize = backtrace(buffer, backtraceMaxSize);

  /* Corresponds the return address to the specific function name. */
  info = backtrace_symbols(buffer, backtraceSize);

  for (int i = 0; i < backtraceSize; i++) {
    (void)fprintf(stderr, "[%d] %s\n", i, info[i]);
  }
  free(info);
}

std::string ThreadIdToString(std::thread::id thread_id) {
  try {
    std::stringstream ss;
    ss << thread_id;
    return ss.str();
  } catch (...) {
    return "invalid thread id";
  }
}

[[noreturn]] void AssertionFailed(const char *expr, const char *file,
                                  uint64_t line) {
  auto fileName = base_name(file);

  if (fileName == nullptr) {
    fileName = "null";
  }

  fprintf(stderr,
          "Dstore: Assertion failure: %s:%lu"
          "%s%s\n"
          "Dstore: thread %s\n",
          fileName, line, expr != nullptr ? ":" : "",
          expr != nullptr ? expr : "",
          ThreadIdToString(std::this_thread::get_id()).c_str());

  fflush(stderr);
  fflush(stdout);
  my_abort();
}

RegexConf g_regexConf[] = {
    {RegexType::SECURITY, "security:\\S*"},
    {RegexType::TOKEN,
     "[Tt][Oo][Kk][Ee][Nn]((\\S*)|(\\s*:\\s*)|(\\s*=\\s*)|(\\s*-\\s*)|(\\s*"
     "\\(\\s*)|(\\s*\\[\\s*)|(\\s*\\{\\s*))\\S*"},
    {RegexType::PASSWORD,
     "[Pp][Aa][Ss][Ss]([Ww]|[Ww][Dd]|[Ww][Oo][Rr][Dd])((\\S*)|(\\s*:\\s*)|("
     "\\s*=\\s*)|(\\s*-\\s*)|(\\s*\\(\\s*)|(\\s*\\[\\s*)|(\\s*\\{\\s*))"
     "\\S*"}  // password
};

/**
Replaces all substrings matching a regular expression with "*****" (for security
filtering). The operation is performed in-place on the input string. If the
filtered string exceeds CDE_LOG_SIZE limit, it will be truncated to fit. The
result is guaranteed to be null-terminated.

@param[in,out]  filterStr  String to be filtered (will be modified in-place)
@param[in]      regex      Regular expression pattern to match sensitive content
*/
void ReplaceAll(char *filterStr, const char *regex) {
  std::regex pattern(regex);
  std::string tempStr(filterStr);
  tempStr = std::regex_replace(tempStr, pattern, "*****");
  if (tempStr.length() >= CDE_LOG_SIZE) {
    tempStr = tempStr.substr(0, CDE_LOG_SIZE - 1);
  }
  strcpy_s(filterStr, CDE_LOG_SIZE, tempStr.c_str());
}

/**
Applies all registered security filters to a string by sequentially processing
it through all regular expressions in the global configuration. Each matching
pattern will be replaced with "*****".

@param[in,out]  filterStr  String to be processed (will be modified in-place)
*/
void DoSecurityFilter(char *filterStr) {
  for (auto &index : g_regexConf) {
    ReplaceAll(filterStr, index.m_regex);
  }
}

void CdeLogPrint(enum loglevel loglevel, bool isAbort, bool isSecurityFilter,
                 bool isAddDebug, const char *fmt, ...) {
  if (log_error_verbosity < loglevel) return;
  assert(fmt);

  /** Define a character array msgBuf of size CDE_LOG_SIZE to store the final
  log message to be output Initialize to all 0s to ensure that each element in
  the array is cleared to zero to avoid undefined characters. */
  char msgBuf[CDE_LOG_SIZE] = {0};
  /** Define a variable args of type va_list to process variable parameter lists
  va_list is a data type used to process variable parameters in C language. It
  will be combined with macros such as va_start and va_end to operate variable
  parameters. */
  va_list args;
  va_start(args, fmt);
  int res = 0;
  if (isAddDebug) {
    /* Add log prefix LOG_PREFIX_DEBUG. */
    int prefix_len = snprintf_s(msgBuf, sizeof(msgBuf), sizeof(msgBuf) - 1,
                                "%s ", LOG_PREFIX_DEBUG);
    /* Safely calculating remaining buffer size. */
    size_t remaining_size = sizeof(msgBuf) - prefix_len;
    res = vsnprintf_s(msgBuf + prefix_len, remaining_size, remaining_size - 1,
                      fmt, args);
  } else {
    res = vsnprintf_s(msgBuf, sizeof(msgBuf), sizeof(msgBuf) - 1, fmt, args);
  }
  va_end(args);
  /* Apply security filtering unless raw logging is enabled or
  the caller dose not want to filter.*/
  if (!opt_general_log_raw && isSecurityFilter) {
    DoSecurityFilter(msgBuf);
  }

  /* Truncate message if exceeding buffer size */
  if (res >= CDE_LOG_SIZE) {
    const size_t truncateMarkerLength = 3;
    memset_s(msgBuf + CDE_LOG_SIZE - (truncateMarkerLength + 1),
             truncateMarkerLength, '.', truncateMarkerLength);
  }

  LogErr(loglevel, ER_CDE_DEFAULT_LOG_ERROR, msgBuf);

  if (unlikely(isAbort)) {
    fflush(NULL);
    AssertionFailed(nullptr, __FILE__, __LINE__);
  }
}