/* Copyright (c) 2014-2016 Percona LLC and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include <cstdio>
#include <cstring>
#include <ctime>

#include <m_ctype.h>
#include <my_sys.h>
#include <mysql/components/services/component_sys_var_service.h>
#include <mysql/plugin.h>
#include <mysql/plugin_audit.h>
#include <mysql/psi/mysql_memory.h>
#include <mysql/service_security_context.h>
#include <mysql_com.h>
#include <mysql_version.h>
#include <mysqld_error.h>
#include <syslog.h>
#include <typelib.h>

#include "audit_handler.h"
#include "audit_log.h"
#include "buffer.h"
#include "filter.h"
#include "logger.h"
#include "my_securec.h"

#define PLUGIN_VERSION 0x0002

enum audit_log_policy_t { ALL, NONE, LOGINS, QUERIES };
enum audit_log_strategy_t {
  ASYNCHRONOUS,
  PERFORMANCE,
  SEMISYNCHRONOUS,
  SYNCHRONOUS
};
enum audit_log_format_t { CSV2, OLD, NEW, JSON, CSV };

enum audit_log_handler_t { HANDLER_FILE, HANDLER_SYSLOG };

using escape_buf_func_t = void (*)(const char *, size_t *, char *, size_t *);

#define AUDIT_LOG_STR_COUNT 9
using audit_record = struct {
  size_t row_size;
  size_t datalen[AUDIT_LOG_STR_COUNT];
  char data[4];
};

#define row_size(row_addr) (((audit_record *)(row_addr))->row_size)
#define row_put_data(buff, data, size)                                       \
  {                                                                          \
    my_memcpy(reinterpret_cast<char *>(buff) + row_size(buff), (size), data, \
              (size));                                                       \
    row_size(buff) = row_size(buff) + (size);                                \
  }

static audit_handler_t *log_handler = nullptr;
static ulonglong record_id = 0;
#define LOG_FILE_TIME_BUF_SIZE 32
static char log_file_time_str[LOG_FILE_TIME_BUF_SIZE] = "\0";
char *audit_log_file;
char default_audit_log_file[] = "audit.log";
ulong audit_log_policy = ALL;
ulong audit_log_strategy = ASYNCHRONOUS;
ulonglong audit_log_buffer_size = 1048576;
ulonglong audit_log_rotate_on_size = 0;
ulonglong audit_log_rotations = 0;
bool audit_log_thread_safe = true;
bool audit_log_flush = false;
bool audit_log_force_rotate = false;
bool audit_log_csv2_truncation = true;
bool audit_log_csv2_escape = false;
bool audit_log_csv2_old_separated_format = false;
ulong audit_log_format = CSV2;
ulong audit_log_handler = HANDLER_FILE;
char *audit_log_syslog_ident;
char default_audit_log_syslog_ident[] = "taurus-audit";
ulong audit_log_syslog_facility = 0;
ulong audit_log_syslog_priority = 0;
static char *audit_log_exclude_accounts = nullptr;
static char *audit_log_include_accounts = nullptr;
static char *audit_log_exclude_databases = nullptr;
static char *audit_log_include_databases = nullptr;
static char *audit_log_exclude_commands = nullptr;
static char *audit_log_include_commands = nullptr;
static char *audit_log_anonymized_ip = nullptr;

PSI_memory_key key_memory_audit_log_logger_handle;
PSI_memory_key key_memory_audit_log_handler;
PSI_memory_key key_memory_audit_log_buffer;
PSI_memory_key key_memory_audit_log_accounts;
PSI_memory_key key_memory_audit_log_databases;
PSI_memory_key key_memory_audit_log_commands;
PSI_memory_key key_memory_audit_log_ip;

static PSI_memory_info all_audit_log_memory[] = {
    {&key_memory_audit_log_logger_handle, "audit_log_logger_handle", 0,
     PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME},
    {&key_memory_audit_log_handler, "audit_log_handler", 0,
     PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME},
    {&key_memory_audit_log_buffer, "audit_log_buffer", 0,
     PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME},
    {&key_memory_audit_log_accounts, "audit_log_accounts", 0,
     PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME},
    {&key_memory_audit_log_databases, "audit_log_databases", 0,
     PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME},
    {&key_memory_audit_log_commands, "audit_log_commands", 0,
     PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME},
    {&key_memory_audit_log_ip, "audit_log_ip", 0, PSI_VOLATILITY_UNKNOWN,
     PSI_DOCUMENT_ME}};

static int audit_log_syslog_facility_codes[] = {
    LOG_USER,     LOG_AUTHPRIV, LOG_CRON,   LOG_DAEMON, LOG_FTP,    LOG_KERN,
    LOG_LPR,      LOG_MAIL,     LOG_NEWS,
#if (defined LOG_SECURITY)
    LOG_SECURITY,
#endif
    LOG_SYSLOG,   LOG_AUTH,     LOG_UUCP,   LOG_LOCAL0, LOG_LOCAL1, LOG_LOCAL2,
    LOG_LOCAL3,   LOG_LOCAL4,   LOG_LOCAL5, LOG_LOCAL6, LOG_LOCAL7, 0};

static const char *audit_log_syslog_facility_names[] = {
    "LOG_USER",     "LOG_AUTHPRIV", "LOG_CRON",   "LOG_DAEMON", "LOG_FTP",
    "LOG_KERN",     "LOG_LPR",      "LOG_MAIL",   "LOG_NEWS",
#if (defined LOG_SECURITY)
    "LOG_SECURITY",
#endif
    "LOG_SYSLOG",   "LOG_AUTH",     "LOG_UUCP",   "LOG_LOCAL0", "LOG_LOCAL1",
    "LOG_LOCAL2",   "LOG_LOCAL3",   "LOG_LOCAL4", "LOG_LOCAL5", "LOG_LOCAL6",
    "LOG_LOCAL7",   nullptr};

static const int audit_log_syslog_priority_codes[] = {
    LOG_INFO,   LOG_ALERT, LOG_CRIT,  LOG_ERR, LOG_WARNING,
    LOG_NOTICE, LOG_EMERG, LOG_DEBUG, 0};

static const char *audit_log_syslog_priority_names[] = {
    "LOG_INFO",   "LOG_ALERT", "LOG_CRIT",  "LOG_ERR", "LOG_WARNING",
    "LOG_NOTICE", "LOG_EMERG", "LOG_DEBUG", nullptr};

static MYSQL_PLUGIN plugin_ptr;

extern bool exclude_commands_arr[static_cast<uint>(SQLCOM_END) + 1];
extern bool include_commands_arr[static_cast<uint>(SQLCOM_END) + 1];
extern int check_func_str(THD *thd, SYS_VAR *, void *save,
                          st_mysql_value *value);
extern void update_func_str(THD *, SYS_VAR *, void *tgt, const void *save);
extern const CHARSET_INFO *get_thd_charset_info(THD *thd);
static void init_record_id(off_t size) { record_id = size; }

static ulonglong next_record_id() {
  return __sync_add_and_fetch(&record_id, 1);
}

#define MAX_RECORD_ID_SIZE 50
#define MAX_TIMESTAMP_SIZE 25

void plugin_thdvar_safe_update(MYSQL_THD thd, struct SYS_VAR *var, char **dest,
                               const char *value);
static char *make_timestamp(char *buf, size_t buf_len, time_t t) {
  struct tm tm;

  my_memset(&tm, sizeof(tm), 0, sizeof(tm));
  strftime(buf, buf_len, "%FT%T UTC", gmtime_r(&t, &tm));

  return buf;
}

static char *make_record_id(char *buf, size_t buf_len) {
  my_snprintf(buf, buf_len, buf_len, "%llu_%s", next_record_id(),
              log_file_time_str);  // NOLINT
  return buf;
}

using escape_rule_t = struct {
  char character;
  size_t length;
  const char *replacement;
};

static void escape_buf(const char *in, size_t *inlen, char *out, size_t *outlen,
                       const escape_rule_t *control_escape_rules,
                       const escape_rule_t *other_escape_rules) {
  char *outstart = out;
  const char *base = in;
  char *outend = out + *outlen;  // NOLINT
  const char *inend;
  const escape_rule_t *replace_rule = nullptr;

  inend = in + (*inlen);  // NOLINT

  while ((in < inend) && (out < outend)) {
    replace_rule = nullptr;
    if (static_cast<unsigned char>(*in) < 32) {
      if (control_escape_rules[static_cast<unsigned int>(*in)].character != 0) {
        replace_rule = &control_escape_rules[static_cast<unsigned int>(*in)];
      }
    } else {
      const escape_rule_t *rule = nullptr;
      for (rule = other_escape_rules; rule->character != 0; rule++) {
        if (*in == rule->character) {
          replace_rule = rule;
          break;
        }
      }
    }
    if (replace_rule != nullptr) {
      if ((outend - out) < static_cast<ptrdiff_t>(replace_rule->length)) {
        break;
      }
      my_memcpy(out, replace_rule->length, replace_rule->replacement,
                replace_rule->length);
      out += replace_rule->length;  // NOLINT
    } else {
      *out++ = *in;  // NOLINT
    }
    ++in;  // NOLINT
  }
  *outlen = out - outstart;
  *inlen = in - base;
}

static void xml_escape(const char *in, size_t *inlen, char *out,
                       size_t *outlen) {
  // Most control sequences aren't supported before XML 1.1, and most
  // tools only support 1.0. Our output is 1.0. Escaping them wouldn't make
  // the output more valid.
  static const escape_rule_t control_rules[] = {
      {0, 0, nullptr}, {0, 0, nullptr},    {0, 0, nullptr},    {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr},    {0, 0, nullptr},    {0, 0, nullptr},
      {0, 0, nullptr}, {'\t', 5, "&#9;"},  {'\n', 6, "&#10;"}, {0, 0, nullptr},
      {0, 0, nullptr}, {'\r', 6, "&#13;"}, {0, 0, nullptr},    {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr},    {0, 0, nullptr},    {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr},    {0, 0, nullptr},    {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr},    {0, 0, nullptr},    {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr},    {0, 0, nullptr},    {0, 0, nullptr},
  };
  static const escape_rule_t other_rules[] = {{'<', 4, "&lt;"},
                                              {'>', 4, "&gt;"},
                                              {'&', 5, "&amp;"},
                                              {'"', 6, "&quot;"},
                                              {0, 0, nullptr}};

  escape_buf(in, inlen, out, outlen,
             static_cast<const escape_rule_t *>(control_rules),
             static_cast<const escape_rule_t *>(other_rules));
}

static void json_escape(const char *in, size_t *inlen, char *out,
                        size_t *outlen) {
  static const escape_rule_t control_rules[] = {
      {0, 6, "\\u0000"},  {1, 6, "\\u0001"},  {2, 6, "\\u0002"},
      {3, 6, "\\u0003"},  {4, 6, "\\u0004"},  {5, 6, "\\u0005"},
      {6, 6, "\\u0006"},  {7, 6, "\\u0007"},  {'\b', 2, "\\b"},
      {'\t', 2, "\\t"},   {'\n', 2, "\\n"},   {11, 6, "\\u000B"},
      {'\f', 2, "\\f"},   {'\r', 2, "\\r"},   {14, 6, "\\u000E"},
      {15, 6, "\\u000F"}, {16, 6, "\\u0010"}, {17, 6, "\\u0011"},
      {18, 6, "\\u0012"}, {19, 6, "\\u0013"}, {20, 6, "\\u0014"},
      {21, 6, "\\u0015"}, {22, 6, "\\u0016"}, {23, 6, "\\u0017"},
      {24, 6, "\\u0018"}, {25, 6, "\\u0019"}, {26, 6, "\\u001A"},
      {27, 6, "\\u001B"}, {28, 6, "\\u001C"}, {29, 6, "\\u001D"},
      {30, 6, "\\u001E"}, {31, 6, "\\u001F"},
  };

  static const escape_rule_t other_rules[] = {
      {'\\', 2, "\\\\"}, {'"', 2, "\\\""}, {'/', 2, "\\/"}, {0, 0, nullptr}};

  escape_buf(in, inlen, out, outlen,
             static_cast<const escape_rule_t *>(control_rules),
             static_cast<const escape_rule_t *>(other_rules));
}

static void csv_escape(const char *in, size_t *inlen, char *out,
                       size_t *outlen) {
  // We do not have any standard control escape rules for CSVs
  static const escape_rule_t control_rules[] = {
      {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr},
      {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr}, {0, 0, nullptr},
  };

  static const escape_rule_t other_rules[] = {{'"', 2, "\"\""},
                                              {0, 0, nullptr}};

  escape_buf(in, inlen, out, outlen,
             static_cast<const escape_rule_t *>(control_rules),
             static_cast<const escape_rule_t *>(other_rules));
}

static const escape_buf_func_t format_escape_func[] = {
    csv_escape, xml_escape, xml_escape, json_escape, csv_escape};

/*
  Calculate the size of the output buffer needed to escape the string.

  @param[in]  in           Input string
  @param[in]  len          Length of the input string

  @return
    size of the otput bufer including trailing zero
*/
static size_t calculate_escape_string_buf_len(const char *in, size_t len) {
  char tmp[128];
  size_t full_outlen = 0;

  while (len > 0) {
    size_t tmp_size = sizeof(tmp);
    size_t inlen = len;
    format_escape_func[audit_log_format](in, &inlen, tmp, &tmp_size);  // NOLINT
    in += inlen;                                                       // NOLINT
    len -= inlen;
    full_outlen += tmp_size;
  }
  return full_outlen + 1;
}

/*
  Escape string according to audit_log_format.

  @param[in]  in           Input string
  @param[in]  inlen        Length of the input string
  @param[in]  out          Output buffer
  @param[in]  outlen       Length of the output buffer
  @param[out] endptr       A pointer to the character after the
                           last escaped character in the output
                           buffer
  @param[out] full_outlen  Length of the output buffer that would
                           be needed to store complete non-truncated
                           escaped input buffer

  @return
    pointer to the beginning of the output buffer
*/
static char *escape_string(const char *in, size_t inlen, char *out,
                           size_t outlen, char **endptr, size_t *full_outlen) {
  if (outlen == 0) {
    if (endptr != nullptr) {
      *endptr = out;
    }
    if (full_outlen != nullptr) {
      *full_outlen += calculate_escape_string_buf_len(in, inlen);
    }
  } else if (in != nullptr) {
    size_t inlen_res = inlen;
    --outlen;
    format_escape_func[audit_log_format](in, &inlen_res, out,  // NOLINT
                                         &outlen);
    out[outlen] = 0;  // NOLINT
    if (endptr != nullptr) {
      *endptr = out + outlen + 1;  // NOLINT
    }
    if (full_outlen != nullptr) {
      *full_outlen += outlen;
      *full_outlen += calculate_escape_string_buf_len(
          in + inlen_res, inlen - inlen_res);  // NOLINT
    }
  } else {
    *out = 0;  // NOLINT
    if (endptr != nullptr) {
      *endptr = out + 1;  // NOLINT
    }
    if (full_outlen != nullptr) {
      ++(*full_outlen);
    }
  }
  return out;
}

static void my_plugin_perror() {
  char errbuf[MYSYS_STRERROR_SIZE];
  my_strerror(static_cast<char *>(errbuf), sizeof(errbuf), errno);
  my_plugin_log_message(&plugin_ptr, MY_ERROR_LEVEL, "Error: %s",
                        static_cast<char *>(errbuf));
}

static void audit_log_write(const char *buf, size_t len) {
  static int write_error = 0;

  if (audit_handler_write(log_handler, buf, len) < 0) {
    if (write_error == 0) {
      write_error = 1;
      my_plugin_log_message(&plugin_ptr, MY_ERROR_LEVEL,
                            "Error writing to file %s.", audit_log_file);
      my_plugin_perror();
    }
  } else {
    write_error = 0;
  }
}

/* Defined in MySQL server */
extern int orig_argc;
extern char **orig_argv;
extern char server_version[SERVER_VERSION_LENGTH];

static char *make_argv(char *buf, size_t len, int argc, char **argv) {
  size_t left = len;
  int ret;
  buf[0] = 0;  // NOLINT
  while (argc > 0 && left > 0) {
    ret = snprintf(buf + len - left, left, "%s%c", *argv, argc > 1 ? ' ' : 0);
    if (ret < 0 || static_cast<size_t>(ret) >= left) break;
    left -= ret;
    argc--;
    argv++;
  }
  return buf;
}

/*
 Allocate and return buffer of given size.
 */
static char *get_record_buffer(MYSQL_THD thd, size_t size);

static bool is_fast_log_format() { return (CSV2 == audit_log_format); }

static char *audit_log_audit_record(char *buf, size_t buflen, const char *name,
                                    time_t t, size_t *outlen) {
  char id_str[MAX_RECORD_ID_SIZE];
  char timestamp[MAX_TIMESTAMP_SIZE];
  char arg_buf[512];
  const char *format_string[] = {
      "\"%llu\",\"%s\",\"%s\",\"%s\",\"%s\","
      "\"" MACHINE_TYPE "-" SYSTEM_TYPE "\"\n",

      "<AUDIT_RECORD\n"
      "  NAME=\"%s\"\n"
      "  RECORD=\"%s\"\n"
      "  TIMESTAMP=\"%s\"\n"
      "  MYSQL_VERSION=\"%s\"\n"
      "  STARTUP_OPTIONS=\"%s\"\n"
      "  OS_VERSION=\"" MACHINE_TYPE "-" SYSTEM_TYPE
      "\"\n"
      "/>\n",

      "<AUDIT_RECORD>\n"
      "  <NAME>%s</NAME>\n"
      "  <RECORD>%s</RECORD>\n"
      "  <TIMESTAMP>%s</TIMESTAMP>\n"
      "  <MYSQL_VERSION>%s</MYSQL_VERSION>\n"
      "  <STARTUP_OPTIONS>%s</STARTUP_OPTIONS>\n"
      "  <OS_VERSION>" MACHINE_TYPE "-" SYSTEM_TYPE
      "</OS_VERSION>\n"
      "</AUDIT_RECORD>\n",

      "{\"audit_record\":{\"name\":\"%s\",\"record\":\"%s\","
      "\"timestamp\":\"%s\",\"mysql_version\":\"%s\","
      "\"startup_optionsi\":\"%s\","
      "\"os_version\":\"" MACHINE_TYPE "-" SYSTEM_TYPE "\"}}\n",

      "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\","
      "\"" MACHINE_TYPE "-" SYSTEM_TYPE "\"\n"};
  if (unlikely(audit_log_csv2_old_separated_format)) {
    format_string[CSV2] =
        "\"%llu\", \"%s\", \"%s\", \"%s\", \"%s\", "
        "\"" MACHINE_TYPE "-" SYSTEM_TYPE "\"\n";
  }

  int ret;
  if (is_fast_log_format()) {
    ret = snprintf(
        buf, buflen, format_string[audit_log_format], next_record_id(), name,
        make_timestamp(timestamp, sizeof(timestamp), t), server_version,
        make_argv(arg_buf, sizeof(arg_buf), orig_argc - 1, orig_argv + 1));
  } else {
    ret = snprintf(
        buf, buflen, format_string[audit_log_format], name,
        make_record_id(id_str, sizeof(id_str)),
        make_timestamp(timestamp, sizeof(timestamp), t), server_version,
        make_argv(arg_buf, sizeof(arg_buf), orig_argc - 1, orig_argv + 1));
  }

  if (unlikely(ret < 0 || static_cast<size_t>(ret) > buflen)) {
    ret = (ret < 0) ? 0 : buflen;
  }
  *outlen = static_cast<size_t>(ret);

  /* make sure that record is not truncated */
  assert(buf + *outlen <= buf + buflen);

  return buf;
}

/********************************************************************
 the function is used only when audit log format is CSV2:
   record_id, connection_id, staus, name, timestamp, command_class,
   sqltext, user, host, os_user, ip, db
*********************************************************************/
static size_t audit_log_general_buf_len(const char *name,
                                        const struct mysql_event_general *event,
                                        const char *default_db) {
  size_t buflen_estimated = 0;
  const char *separate_string = "\",\"";
  const char *format_string = "\"%llu\", \"%lu\", \"%d\", \"";  // NOLINT
  if (unlikely(audit_log_csv2_old_separated_format)) {
    format_string = "\"%llu\", \"%lu\", \"%d\", \"";
    separate_string = "\", \"";
  }

  /* formatted_log_size is not the real size of formatted log, but make sure it
   * is not less than the real size*/
  buflen_estimated =
      strlen(format_string) + MAX_RECORD_ID_SIZE + 20 + /* general_thread_id */
      20 +                                              /* status */
      strlen(name) + MAX_TIMESTAMP_SIZE +
      event->general_sql_command.length + /* command_class */
      my_charset_utf8mb4_general_ci.mbmaxlen *
          event->general_query.length +     /* sqltext, the sql string before
                                               formatted will be stored in this
                                               buffer */
      event->general_user.length +          /* user */
      event->general_host.length +          /* host */
      event->general_external_user.length + /* os_user */
      event->general_ip.length +            /* ip */
      strlen(default_db) +                  /* db */
      strlen(separate_string) * AUDIT_LOG_STR_COUNT +
      strlen("\"\n"); /*blank after each string field */

  /*
   When escape is enabled, more memory space is required.
   The extra 1024 or 512 bytes are enough to include the "\0" at the end and
   other auxiliary data structures.
   */
  if (unlikely(audit_log_csv2_escape)) {
    buflen_estimated +=
        my_charset_utf8mb4_general_ci.mbmaxlen * event->general_query.length +
        1024;
  } else {
    buflen_estimated += 512;
  }
  return buflen_estimated;
}

static void row_anonymized_ip(char *next_buff, size_t length) {
  char *right_boundary = next_buff - 1;
  while (length--) {
    if (*right_boundary == '@') {
      break;
    }
    if (*right_boundary >= '0' && *right_boundary <= '9') {
      *right_boundary = '*';
    }
    right_boundary--;
  }
}
/********************************************************************
 the function is used only when audit log format is CSV2:
   record_id, connection_id, staus, name, timestamp, command_class,
   sqltext, user, host, os_user, ip, db
  "\"%llu\", \"%lu\", \"%d\", \"%s\", \"%s\", \"%s\", "
  "\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\""
*********************************************************************/
static char *audit_log_general_record_fast(
    MYSQL_THD thd, char *buf, size_t buflen, const char *name, time_t t,
    int status, const struct mysql_event_general *event, const char *default_db,
    size_t *outlen, bool is_anonymized_ip) {
  char timestamp[MAX_TIMESTAMP_SIZE];
  char *final_log;
  char *endptr, *endbuf;
  char *limit_addr;
  char *query_start_addr;
  char *escaped_sql, *escaped_sql_addr, *escaped_sql_end;
  size_t buflen_estimated;
  size_t format_len;
  size_t blank_length, end_length, max_query_length;
  size_t escaped_sql_length;
  uint errors = 0;
  size_t full_outlen = 0;
  audit_record *audit_rec;
  const char *format_string = "\"%llu\",\"%lu\",\"%d\",\"";
  const char *separate_string = "\",\"";
  const CHARSET_INFO *general_charset = NULL;
  if (unlikely(audit_log_csv2_old_separated_format)) {
    format_string = "\"%llu\", \"%lu\", \"%d\", \"";
    separate_string = "\", \"";
  }

  DBUG_EXECUTE_IF("simulate_audit_log_buffer_malloc_fail", {
    if (!audit_log_csv2_escape) buflen = 1024;
  });
  DBUG_EXECUTE_IF("simulate_audit_malloc_fail", {
    if (!audit_log_csv2_escape) buflen = 1024;
  });

  make_timestamp(timestamp, sizeof(timestamp), t);

  /* Estimate the required buffer length for generating a complete record. */
  buflen_estimated = audit_log_general_buf_len(name, event, default_db);
  if (buflen_estimated > buflen) {
    char *new_buffer = get_record_buffer(thd, buflen_estimated);
    if (new_buffer != nullptr) {
      buf = new_buffer;
      buflen = buflen_estimated;
    }
  }

  endptr = buf;
  endbuf = buf + buflen;

  /* Start to generate audit log record. */
  audit_rec = (audit_record *)endptr;
  audit_rec->row_size =
      sizeof(audit_rec->row_size) + sizeof(audit_rec->datalen);

  format_len = snprintf(audit_rec->data, endbuf - endptr, format_string,
                        next_record_id(), event->general_thread_id, status);
  audit_rec->row_size += format_len;
  endptr = audit_rec->data;

  audit_rec->datalen[0] = strlen(name);
  audit_rec->datalen[1] = strlen(timestamp);
  audit_rec->datalen[2] = event->general_sql_command.length;
  audit_rec->datalen[4] = event->general_user.length;
  audit_rec->datalen[5] = event->general_host.length;
  audit_rec->datalen[6] = event->general_external_user.length;
  audit_rec->datalen[7] = event->general_ip.length;
  audit_rec->datalen[8] = strlen(default_db);
  blank_length = strlen(separate_string);
  end_length = strlen("\"\n") + 1;  // include '\0'

  /*
   Ensuring that sufficient space is reserved for the fields after the SQL
   statement.
   */
  limit_addr = endbuf - end_length - blank_length * 5 - audit_rec->datalen[8] -
               audit_rec->datalen[7] - audit_rec->datalen[6] -
               audit_rec->datalen[5] - audit_rec->datalen[4];

  row_put_data(audit_rec, name, audit_rec->datalen[0]);
  row_put_data(audit_rec, separate_string, blank_length);

  row_put_data(audit_rec, timestamp, audit_rec->datalen[1]);
  row_put_data(audit_rec, separate_string, blank_length);

  row_put_data(audit_rec, event->general_sql_command.str,
               audit_rec->datalen[2]);
  row_put_data(audit_rec, separate_string, blank_length);

  /* Start position of the utf8mb4 encoded SQL statement. */
  query_start_addr = (char *)(audit_rec) + audit_rec->row_size;
  /* Maximum number of bytes allowed for the utf8mb4 encoded statement. */
  max_query_length = limit_addr - query_start_addr;

  general_charset = get_thd_charset_info(thd);
  if (unlikely(audit_log_csv2_escape)) {
    /*
     After ensuring that sufficient space is reserved for other fields except
     the SQL statement, the escaped SQL statement is written to the last
     quarter of the remaining space. Then, the escaped SQL statement is
     converted into utf8mb4 encoded SQL statement and written to the entire
     remaining space. The mathematical proportion ensures that no characters
     that have not been converted are overwritten during the entire process.
     Finally, the remaining fields are appended to the utf8mb4 encoded
     statement with the ending '\0'.
     */

    /* Maximum number of bytes allowed for the escaped SQL statement. */
    escaped_sql_length =
        max_query_length / my_charset_utf8mb4_general_ci.mbmaxlen;
    /* Start position of the escaped SQL statement. */
    escaped_sql_addr = limit_addr - escaped_sql_length;
    /* Generate the escaped SQL statement. */
    escaped_sql = escape_string(
        event->general_query.str, event->general_query.length, escaped_sql_addr,
        escaped_sql_length, &escaped_sql_end, &full_outlen);
    /* Ensure that the escape SQL statement does not exceed the limit. */
    assert(escaped_sql_end <= limit_addr);
    /*
     Convert the escaped SQL statement into utf8mb4_general_ci encoded
     statement and save it to the buffer starting with endptr. Return the
     number of occupied bytes.
     */
    audit_rec->datalen[3] = my_convert(
        query_start_addr, max_query_length, &my_charset_utf8mb4_general_ci,
        escaped_sql, escaped_sql_end - escaped_sql_addr - 1,
        general_charset ? general_charset : event->general_charset, &errors);
  } else {
    /*
     Convert the original SQL statement into utf8mb4_general_ci encoded
     statement and save it to the buffer starting with endptr. Return the
     number of occupied bytes.
     */
    audit_rec->datalen[3] = my_convert(
        query_start_addr, max_query_length, &my_charset_utf8mb4_general_ci,
        event->general_query.str, event->general_query.length,
        general_charset ? general_charset : event->general_charset, &errors);
  }

  audit_rec->row_size += audit_rec->datalen[3];
  row_put_data(audit_rec, separate_string, blank_length);

  row_put_data(audit_rec, event->general_user.str, audit_rec->datalen[4]);
  if (is_anonymized_ip) {
    row_anonymized_ip((char *)audit_rec + audit_rec->row_size,
                      audit_rec->datalen[4]);
  }
  row_put_data(audit_rec, separate_string, blank_length);

  row_put_data(audit_rec, event->general_host.str, audit_rec->datalen[5]);
  row_put_data(audit_rec, separate_string, blank_length);

  row_put_data(audit_rec, event->general_external_user.str,
               audit_rec->datalen[6]);
  row_put_data(audit_rec, separate_string, blank_length);

  row_put_data(audit_rec, event->general_ip.str, audit_rec->datalen[7]);
  if (is_anonymized_ip) {
    row_anonymized_ip((char *)audit_rec + audit_rec->row_size,
                      audit_rec->datalen[7]);
  }
  row_put_data(audit_rec, separate_string, blank_length);

  row_put_data(audit_rec, default_db, audit_rec->datalen[8]);
  row_put_data(audit_rec, "\"\n", end_length);

  /* Ensure that the current record does not exceed the limit. */
  assert((char *)audit_rec + row_size(audit_rec) <= endbuf);

  final_log = endptr;  // final_log points to the header of this record.
  *outlen = row_size(audit_rec) - sizeof(audit_rec->row_size) -
            sizeof(audit_rec->datalen) - 1;  // exclude '\0'
  return final_log;
}

static char *audit_log_general_record(MYSQL_THD thd, char *buf, size_t buflen,
                                      const char *name, time_t t, int status,
                                      const struct mysql_event_general *event,
                                      const char *default_db, size_t *outlen,
                                      bool is_anonymized_ip) {
  char id_str[MAX_RECORD_ID_SIZE];
  char timestamp[MAX_TIMESTAMP_SIZE];
  char *query, *user, *host, *external_user, *ip, *db;
  char *endptr = buf, *endbuf = buf + buflen;  // NOLINT
  size_t full_outlen = 0, buflen_estimated;
  size_t query_length, row_length;
  int ret;
  const CHARSET_INFO *general_charset = NULL;

  const char *format_string[] = {
      "",  // it will not come into this function for CSV2 format
      "<AUDIT_RECORD\n"
      "  NAME=\"%s\"\n"
      "  RECORD=\"%s\"\n"
      "  TIMESTAMP=\"%s\"\n"
      "  COMMAND_CLASS=\"%s\"\n"
      "  CONNECTION_ID=\"%lu\"\n"
      "  STATUS=\"%d\"\n"
      "  SQLTEXT=\"%s\"\n"
      "  USER=\"%s\"\n"
      "  HOST=\"%s\"\n"
      "  OS_USER=\"%s\"\n"
      "  IP=\"%s\"\n"
      "  DB=\"%s\"\n"
      "/>\n",

      "<AUDIT_RECORD>\n"
      "  <NAME>%s</NAME>\n"
      "  <RECORD>%s</RECORD>\n"
      "  <TIMESTAMP>%s</TIMESTAMP>\n"
      "  <COMMAND_CLASS>%s</COMMAND_CLASS>\n"
      "  <CONNECTION_ID>%lu</CONNECTION_ID>\n"
      "  <STATUS>%d</STATUS>\n"
      "  <SQLTEXT>%s</SQLTEXT>\n"
      "  <USER>%s</USER>\n"
      "  <HOST>%s</HOST>\n"
      "  <OS_USER>%s</OS_USER>\n"
      "  <IP>%s</IP>\n"
      "  <DB>%s</DB>\n"
      "</AUDIT_RECORD>\n",

      "{\"audit_record\":"
      "{\"name\":\"%s\","
      "\"record\":\"%s\","
      "\"timestamp\":\"%s\","
      "\"command_class\":\"%s\","
      "\"connection_id\":\"%lu\","
      "\"status\":%d,"
      "\"sqltext\":\"%s\","
      "\"user\":\"%s\","
      "\"host\":\"%s\","
      "\"os_user\":\"%s\","
      "\"ip\":\"%s\","
      "\"db\":\"%s\"}}\n",

      ("\"%s\",\"%s\",\"%s\",\"%s\",\"%lu\",%d,\"%s\",\"%s\","
       "\"%s\",\"%s\",\"%s\",\"%s\"\n")};

  DBUG_ASSERT(!is_fast_log_format());  // NOLINT

  query_length =
      my_charset_utf8mb4_general_ci.mbmaxlen * event->general_query.length;
  general_charset = get_thd_charset_info(thd);

  if (query_length < static_cast<size_t>(endbuf - endptr)) {
    uint errors;
    query_length = my_convert(
        endptr, query_length, &my_charset_utf8mb4_general_ci,
        event->general_query.str, event->general_query.length,
        general_charset ? general_charset : event->general_charset, &errors);

    query = endptr;
    endptr += query_length;  // NOLINT

    full_outlen += query_length;

    query = escape_string(query, query_length, endptr, endbuf - endptr, &endptr,
                          &full_outlen);
  } else {
    endptr = endbuf;
    query = escape_string(event->general_query.str, event->general_query.length,
                          endptr, endbuf - endptr, &endptr, &full_outlen);
    full_outlen *= my_charset_utf8mb4_general_ci.mbmaxlen;
    full_outlen += query_length * my_charset_utf8mb4_general_ci.mbmaxlen;
  }

  user = escape_string(event->general_user.str, event->general_user.length,
                       endptr, endbuf - endptr, &endptr, &full_outlen);
  if (is_anonymized_ip) {
    row_length = strlen(user);
    row_anonymized_ip(user + row_length, row_length);
  }

  host = escape_string(event->general_host.str, event->general_host.length,
                       endptr, endbuf - endptr, &endptr, &full_outlen);
  external_user = escape_string(event->general_external_user.str,
                                event->general_external_user.length, endptr,
                                endbuf - endptr, &endptr, &full_outlen);
  ip = escape_string(event->general_ip.str, event->general_ip.length, endptr,
                     endbuf - endptr, &endptr, &full_outlen);
  if (is_anonymized_ip) {
    row_length = strlen(ip);
    row_anonymized_ip(ip + row_length, row_length);
  }

  db = escape_string(default_db, strlen(default_db), endptr, endbuf - endptr,
                     &endptr, &full_outlen);

  buflen_estimated = full_outlen * 2 + strlen(format_string[audit_log_format]) +
                     strlen(name) + event->general_sql_command.length +
                     20 + /* general_thread_id */
                     20 + /* status */
                     MAX_RECORD_ID_SIZE + MAX_TIMESTAMP_SIZE;
  if (buflen_estimated > buflen) {
    *outlen = buflen_estimated;
    return nullptr;
  }

  ret = snprintf(endptr, endbuf - endptr, format_string[audit_log_format], name,
                 make_record_id(id_str, sizeof(id_str)),
                 make_timestamp(timestamp, sizeof(timestamp), t),
                 event->general_sql_command.str, event->general_thread_id,
                 status, query, user, host, external_user, ip, db);
  if (unlikely(ret < 0)) {
    ret = 0;
  }
  *outlen = static_cast<size_t>(ret);
  /* make sure that record is not truncated */
  assert(endptr + *outlen <= buf + buflen);

  return endptr;
}
static char *audit_log_connection_record(
    char *buf, size_t buflen, const char *name, time_t t,
    const struct mysql_event_connection *event, size_t *outlen,
    bool is_anonymized_ip) {
  char id_str[MAX_RECORD_ID_SIZE];
  char timestamp[MAX_TIMESTAMP_SIZE];
  char *user, *priv_user, *external_user, *proxy_user, *host, *ip, *database;
  char *endptr = buf, *endbuf = buf + buflen;  // NOLINT
  int ret;
  size_t row_length;

  const char *format_string[] = {
      "\"%llu\",\"%lu\",\"%d\",\"%s\",\"%s\",\"%s\",\"%s\","
      "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\n",

      "<AUDIT_RECORD\n"
      "  NAME=\"%s\"\n"
      "  RECORD=\"%s\"\n"
      "  TIMESTAMP=\"%s\"\n"
      "  CONNECTION_ID=\"%lu\"\n"
      "  STATUS=\"%d\"\n"
      "  USER=\"%s\"\n"
      "  PRIV_USER=\"%s\"\n"
      "  OS_LOGIN=\"%s\"\n"
      "  PROXY_USER=\"%s\"\n"
      "  HOST=\"%s\"\n"
      "  IP=\"%s\"\n"
      "  DB=\"%s\"\n"
      "/>\n",

      "<AUDIT_RECORD>\n"
      "  <NAME>%s</NAME>\n"
      "  <RECORD>%s</RECORD>\n"
      "  <TIMESTAMP>%s</TIMESTAMP>\n"
      "  <CONNECTION_ID>%lu</CONNECTION_ID>\n"
      "  <STATUS>%d</STATUS>\n"
      "  <USER>%s</USER>\n"
      "  <PRIV_USER>%s</PRIV_USER>\n"
      "  <OS_LOGIN>%s</OS_LOGIN>\n"
      "  <PROXY_USER>%s</PROXY_USER>\n"
      "  <HOST>%s</HOST>\n"
      "  <IP>%s</IP>\n"
      "  <DB>%s</DB>\n"
      "</AUDIT_RECORD>\n",

      "{\"audit_record\":"
      "{\"name\":\"%s\","
      "\"record\":\"%s\","
      "\"timestamp\":\"%s\","
      "\"connection_id\":\"%lu\","
      "\"status\":%d,"
      "\"user\":\"%s\","
      "\"priv_user\":\"%s\","
      "\"os_login\":\"%s\","
      "\"proxy_user\":\"%s\","
      "\"host\":\"%s\","
      "\"ip\":\"%s\","
      "\"db\":\"%s\"}}\n",

      "\"%s\",\"%s\",\"%s\",\"%lu\",%d,\"%s\",\"%s\",\"%s\","
      "\"%s\",\"%s\",\"%s\",\"%s\"\n"};
  if (unlikely(audit_log_csv2_old_separated_format)) {
    format_string[CSV2] =
        "\"%llu\",\"%lu\",\"%d\",\"%s\",%s,\"%s\",\"%s\","
        "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\n";
  }

  user = escape_string(event->user.str, event->user.length, endptr,
                       endbuf - endptr, &endptr, nullptr);
  priv_user = escape_string(event->priv_user.str, event->priv_user.length,
                            endptr, endbuf - endptr, &endptr, nullptr);
  external_user =
      escape_string(event->external_user.str, event->external_user.length,
                    endptr, endbuf - endptr, &endptr, nullptr);
  proxy_user = escape_string(event->proxy_user.str, event->proxy_user.length,
                             endptr, endbuf - endptr, &endptr, nullptr);
  host = escape_string(event->host.str, event->host.length, endptr,
                       endbuf - endptr, &endptr, nullptr);
  ip = escape_string(event->ip.str, event->ip.length, endptr, endbuf - endptr,
                     &endptr, nullptr);
  if (is_anonymized_ip) {
    row_length = strlen(ip);
    row_anonymized_ip(ip + row_length, row_length);
  }

  database = escape_string(event->database.str, event->database.length, endptr,
                           endbuf - endptr, &endptr, nullptr);

  DBUG_ASSERT((endptr - buf) * 2 + strlen(format_string[audit_log_format]) +
                  strlen(name) + MAX_RECORD_ID_SIZE + MAX_TIMESTAMP_SIZE +
                  20 + /* event->thread_id */
                  20   /* event->status */
              < buflen);
  if (is_fast_log_format()) {
    ret = snprintf(endptr, endbuf - endptr, format_string[audit_log_format],
                   next_record_id(), event->connection_id, event->status, name,
                   make_timestamp(timestamp, sizeof(timestamp), t), user,
                   priv_user, external_user, proxy_user, host, ip, database);
  } else {
    ret = snprintf(endptr, endbuf - endptr, format_string[audit_log_format],
                   name, make_record_id(id_str, sizeof(id_str)),
                   make_timestamp(timestamp, sizeof(timestamp), t),
                   event->connection_id, event->status, user, priv_user,
                   external_user, proxy_user, host, ip, database);
  }

  if (unlikely(ret < 0 || static_cast<size_t>(ret) > buflen)) {
    ret = (ret < 0) ? 0 : buflen;
  }
  *outlen = static_cast<size_t>(ret);
  /* make sure that record is not truncated */
  assert(endptr + *outlen <= buf + buflen);

  return endptr;
}

static size_t audit_log_header(MY_STAT *stat, char *buf, size_t buflen) {
  const char *format_string[] = {"",
                                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                 "<AUDIT>\n",
                                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                 "<AUDIT>\n",
                                 "", ""};
  int ret;
  struct tm tm;

  assert(strcmp(system_charset_info->csname, "utf8mb3") == 0);

  memset(&tm, 0, sizeof(tm));
  strftime(log_file_time_str, LOG_FILE_TIME_BUF_SIZE, "%FT%T",
           gmtime_r(&stat->st_mtime, &tm));

  init_record_id(stat->st_size);

  if (buf == nullptr) {
    return 0;
  }
  ret = snprintf(buf, buflen, "%s", format_string[audit_log_format]);
  return ret < 0 ? 0 : ret;
}

static size_t audit_log_footer(char *buf, size_t buflen) {
  const char *format_string[] = {"", "</AUDIT>\n", "</AUDIT>\n", "", ""};
  int ret;

  if (buf == nullptr) {
    return 0;
  }
  ret = snprintf(buf, buflen, "%s", format_string[audit_log_format]);
  return ret < 0 ? 0 : ret;
}

static int init_new_log_file() {
  if (audit_log_handler == HANDLER_FILE) {
    audit_handler_file_config_t opts;
    opts.name = audit_log_file;
    opts.rotate_on_size = audit_log_rotate_on_size;
    opts.rotations = audit_log_rotations;
    opts.sync_on_write = audit_log_strategy == SYNCHRONOUS;
    opts.use_buffer = audit_log_strategy < SEMISYNCHRONOUS;
    opts.buffer_size = audit_log_buffer_size;
    opts.can_drop_data = audit_log_strategy == PERFORMANCE;
    opts.header = audit_log_header;
    opts.footer = audit_log_footer;

    log_handler = audit_handler_file_open(&opts);
    if (log_handler == nullptr) {
      my_plugin_log_message(&plugin_ptr, MY_ERROR_LEVEL, "Cannot open file %s.",
                            audit_log_file);
      my_plugin_perror();
      return (1);
    }
  } else {
    audit_handler_syslog_config_t opts;
    opts.facility = audit_log_syslog_facility_codes[audit_log_syslog_facility];
    opts.ident = audit_log_syslog_ident;
    opts.priority = audit_log_syslog_priority_codes[audit_log_syslog_priority];
    opts.header = audit_log_header;
    opts.footer = audit_log_footer;

    log_handler = audit_handler_syslog_open(&opts);
    if (log_handler == nullptr) {
      my_plugin_log_message(&plugin_ptr, MY_ERROR_LEVEL, "Cannot open syslog.");
      my_plugin_perror();
      return (1);
    }
  }

  return (0);
}

static int reopen_log_file() {
  if (audit_handler_flush(log_handler) != 0) {
    my_plugin_log_message(&plugin_ptr, MY_ERROR_LEVEL, "Cannot open file %s.",
                          audit_log_file);
    my_plugin_perror();
    return (1);
  }

  return (0);
}

using query_stack_frame = struct {
  /* number of included databases */
  int databases_included;
  /* number of excluded databases */
  int databases_excluded;
  /* number of accessed databases */
  int databases_accessed;
  /* query */
  const char *query;
};

using query_stack = struct {
  size_t size;
  size_t top;
  query_stack_frame *frames;
};

/*
 Struct to store various THD specific data
 */
using audit_log_thd_local = struct {
  /* size of allocated large buffer for record formatting */
  size_t record_buffer_size;
  /* large buffer for record formatting */
  char *record_buffer;
  /* skip session logging */
  bool skip_session;
  /* skip logging for the next query */
  bool skip_query;
  /* anonymizing ip in the log */
  bool anonymized_ip;
  /* default database */
  char db[NAME_LEN + 1];
  /* default database candidate */
  char init_db_query[NAME_LEN + 1];
  /* call stack */
  query_stack stack;
};

/*
 Return pointer to THD specific data.
 */
static audit_log_thd_local *get_thd_local(MYSQL_THD thd);

/*
 Allocate and return given number of stack frames.
 */
static query_stack_frame *realloc_stack_frames(MYSQL_THD thd, size_t size);

static int audit_log_plugin_init(MYSQL_PLUGIN plugin_info) {
  char buf[1024];
  size_t len;
  int count;

  plugin_ptr = plugin_info;

  count = array_elements(all_audit_log_memory);
  mysql_memory_register(AUDIT_LOG_PSI_CATEGORY,
                        static_cast<PSI_memory_info *>(all_audit_log_memory),
                        count);
  logger_init_mutexes();

  audit_log_filter_init();

  if (audit_log_exclude_accounts != nullptr &&
      audit_log_include_accounts != nullptr) {
    my_plugin_log_message(&plugin_ptr, MY_ERROR_LEVEL,
                          "Both 'audit_log_exclude_accounts' and "
                          "'audit_log_include_accounts' are not NULL\n");
    return 1;
  }

  if (audit_log_exclude_commands != nullptr &&
      audit_log_include_commands != nullptr) {
    my_plugin_log_message(&plugin_ptr, MY_ERROR_LEVEL,
                          "Both 'audit_log_exclude_commands' and "
                          "'audit_log_include_commands' are not NULL\n");
    return 1;
  }

  if (audit_log_exclude_databases != nullptr &&
      audit_log_include_databases != nullptr) {
    my_plugin_log_message(&plugin_ptr, MY_ERROR_LEVEL,
                          "Both 'audit_log_exclude_databases' and "
                          "'audit_log_include_databases' are not NULL\n");
    return 1;
  }

  if (audit_log_exclude_accounts != nullptr) {
    audit_log_set_exclude_accounts(audit_log_exclude_accounts);
  }
  if (audit_log_include_accounts != nullptr) {
    audit_log_set_include_accounts(audit_log_include_accounts);
  }
  if (audit_log_exclude_commands != nullptr) {
    audit_log_set_exclude_commands(audit_log_exclude_commands);
  }
  if (audit_log_include_commands != nullptr) {
    audit_log_set_include_commands(audit_log_include_commands);
  }
  if (audit_log_exclude_databases != nullptr) {
    audit_log_set_exclude_databases(audit_log_exclude_databases);
  }
  if (audit_log_include_databases != nullptr) {
    audit_log_set_include_databases(audit_log_include_databases);
  }
  if (audit_log_anonymized_ip != nullptr) {
    audit_log_set_anonymized_ip(audit_log_anonymized_ip);
  }

  if (init_new_log_file()) return (1);

  if (audit_log_audit_record(buf, sizeof(buf), "Audit", time(nullptr), &len))
    audit_log_write(buf, len);

  return 0;
}

static int audit_log_plugin_deinit(void *arg MY_ATTRIBUTE((unused))) {
  char buf[1024];
  size_t len;

  if (audit_log_audit_record(static_cast<char *>(buf), sizeof(buf), "NoAudit",
                             time(nullptr), &len) != nullptr) {
    audit_log_write(static_cast<const char *>(buf), len);
  }

  audit_handler_close(log_handler);

  audit_log_filter_destroy();

  return (0);
}

static int is_event_class_allowed_by_policy(mysql_event_class_t event_class,
                                            enum audit_log_policy_t policy) {
  static unsigned int class_mask[] = {
      /* ALL */
      (1 << MYSQL_AUDIT_GENERAL_CLASS) | (1 << MYSQL_AUDIT_CONNECTION_CLASS),
      0,                                   /* NONE */
      (1 << MYSQL_AUDIT_CONNECTION_CLASS), /* LOGINS */
      (1 << MYSQL_AUDIT_GENERAL_CLASS),    /* QUERIES */
  };

  return (class_mask[policy] & (1 << event_class)) != 0;  // NOLINT
}

static bool audit_log_update_thd_local(MYSQL_THD thd,
                                       audit_log_thd_local *local,
                                       unsigned int event_class,
                                       const void *event) {
  DBUG_ASSERT(audit_log_include_accounts == nullptr ||
              audit_log_exclude_accounts == nullptr);

  DBUG_ASSERT(audit_log_include_databases == nullptr ||
              audit_log_exclude_databases == nullptr);

  DBUG_ASSERT(audit_log_include_commands == nullptr ||
              audit_log_exclude_commands == nullptr);

  if (event_class == MYSQL_AUDIT_CONNECTION_CLASS) {
    const auto *event_connection =
        static_cast<const struct mysql_event_connection *>(event);
    LEX_STRING priv_user, priv_host;
    MYSQL_SECURITY_CONTEXT ctx;

    if (thd_get_security_context(thd, &ctx)) {
      my_message(ER_AUDIT_API_ABORT, "Error: can not get security context",
                 MYF(0));
      return false;
    }

    if (security_context_get_option(ctx, "priv_user", &priv_user)) {
      my_message(ER_AUDIT_API_ABORT,
                 "Error: can not get priv_user from "
                 "security context",
                 MYF(0));
      return false;
    }
    if (priv_user.length == 0 && event_connection->user.str) {
      priv_user.str = const_cast<char *>(event_connection->user.str);
      priv_user.length = event_connection->user.length;
    }
    if (security_context_get_option(ctx, "priv_host", &priv_host)) {
      my_message(ER_AUDIT_API_ABORT,
                 "Error: can not get priv_host from "
                 "security context",
                 MYF(0));
      return false;
    }

    local->skip_session = false;
    local->anonymized_ip = false;
    if (priv_user.length == 0 && priv_host.length == 0)
      local->skip_session = true;
    if (audit_log_include_accounts != nullptr &&
        !audit_log_check_account_included(priv_user.str, priv_user.length,
                                          priv_host.str, priv_host.length)) {
      local->skip_session = true;
    }
    if (audit_log_exclude_accounts != nullptr &&
        audit_log_check_account_excluded(priv_user.str, priv_user.length,
                                         priv_host.str, priv_host.length))
      local->skip_session = true;
    if (!local->skip_session && audit_log_anonymized_ip != nullptr &&
        audit_log_check_ip_anonymized(event_connection->ip.str,
                                      event_connection->ip.length)) {
      local->anonymized_ip = true;
    }

    if (!local->skip_session &&
        strcmp(priv_user.str, "skip-grants user") == 0 &&
        strcmp(priv_host.str, "skip-grants host") == 0) {
      local->skip_session = true;
    }

    if (event_connection->status == 0) {
      /* track default DB change */
      DBUG_ASSERT(event_connection->database.length <= sizeof(local->db));
      my_memcpy(
          static_cast<char *>(local->db), event_connection->database.length,
          event_connection->database.str, event_connection->database.length);
      local->db[event_connection->database.length] = 0;  // NOLINT
    }
  } else if (event_class == MYSQL_AUDIT_GENERAL_CLASS) {
    const auto *event_general =
        static_cast<const struct mysql_event_general *>(event);

    if (event_general->event_subclass == MYSQL_AUDIT_GENERAL_STATUS) {
      local->skip_query = false;

      if (local->stack.frames[local->stack.top].query ==  // NOLINT
          event_general->general_query.str) {
        local->skip_query |= static_cast<int>(
            audit_log_include_databases != nullptr &&
            local->stack.frames[local->stack.top].databases_accessed > 0 &&
            local->stack.frames[local->stack.top].databases_included == 0);

        local->skip_query |= static_cast<int>(
            audit_log_exclude_databases != nullptr &&
            local->stack.frames[local->stack.top].databases_accessed > 0 &&
            local->stack.frames[local->stack.top].databases_excluded ==
                local->stack.frames[local->stack.top].databases_accessed);

        local->stack.frames[local->stack.top].databases_included = 0;  // NOLINT
        local->stack.frames[local->stack.top].databases_accessed = 0;  // NOLINT
        local->stack.frames[local->stack.top].databases_excluded = 0;  // NOLINT
        local->stack.frames[local->stack.top].query = nullptr;         // NOLINT

        if (local->stack.top > 0) {
          --local->stack.top;
        }
      }

      if (is_fast_log_format() != 0) {
        DBUG_ASSERT(event_general->general_sql_cmd_enum <  // NOLINT
                    (static_cast<int>(SQLCOM_END) + 1));
        local->skip_query |= static_cast<int>(
            audit_log_include_commands != nullptr &&
            !include_commands_arr[event_general                  // NOLINT
                                      ->general_sql_cmd_enum]);  // NOLINT

        local->skip_query |= static_cast<int>(
            audit_log_exclude_commands != nullptr &&
            exclude_commands_arr[event_general                  // NOLINT
                                     ->general_sql_cmd_enum]);  // NOLINT
      } else {
        local->skip_query |=
            static_cast<int>(audit_log_include_commands != nullptr &&
                             !audit_log_check_command_included(
                                 event_general->general_sql_command.str,
                                 event_general->general_sql_command.length));

        local->skip_query |=
            static_cast<int>(audit_log_exclude_commands != nullptr &&
                             audit_log_check_command_excluded(
                                 event_general->general_sql_command.str,
                                 event_general->general_sql_command.length));
      }
      if (!local->skip_query &&
          ((event_general->general_command.length == 4 &&
            strncmp(event_general->general_command.str, "Quit", 4) == 0) ||
           (event_general->general_command.length == 11 &&
            strncmp(event_general->general_command.str, "Change user", 11) ==
                0)))
        local->skip_query = true;
      /* track default DB */
      if (event_general->general_default_db.str) {
        assert(event_general->general_default_db.length < sizeof(local->db));
        memcpy(local->db, event_general->general_default_db.str,
               event_general->general_default_db.length);
        local->db[event_general->general_default_db.length] = '\0';
      } else {
        local->db[0] = '\0';
      }
    }

    if (event_general->event_subclass == MYSQL_AUDIT_GENERAL_LOG &&
        event_general->general_command.length == 7 &&
        strncmp(event_general->general_command.str, "Init DB", 7) == 0 &&
        event_general->general_query.str != nullptr &&
        strpbrk("\n\r\t ", event_general->general_query.str) == nullptr) {
      /* Database is about to be changed. Server doesn't provide database
      name in STATUS event, so remember it now. */

      assert(event_general->general_query.length <= sizeof(local->db));
      memcpy(local->db, event_general->general_query.str,
             event_general->general_query.length);
      local->db[event_general->general_query.length] = 0;
    }
  } else if (event_class == MYSQL_AUDIT_TABLE_ACCESS_CLASS) {
    /*
      Both audit_log_include_databases and audit_log_exclude_databases are
      nullptr, it is not need to realloc_stack_frames.
      1. If audit_log_include_databases is nullptr,
      local->stack.frames[local->stack.top].databases_included will always
      zero.
      2. If audit_log_exclude_databases is nullptr,
      local->stack.frames[local->stack.top].databases_excluded will always zero.
      3. Both audit_log_include_databases and audit_log_exclude_databases are
      nullptr, local->stack.frames[local->stack.top].databases_accessed will not
      be used.
    */
    if (audit_log_include_databases == nullptr &&
        audit_log_exclude_databases == nullptr)
      return true;

    const auto *event_table =
        static_cast<const struct mysql_event_table_access *>(event);

    if (local->stack.frames[local->stack.top].query != event_table->query.str &&
        local->stack.frames[local->stack.top].query != nullptr) {  // NOLINT
      if (++local->stack.top >= local->stack.size) {
        realloc_stack_frames(thd, local->stack.size * 2);
      }
    }
    local->stack.frames[local->stack.top].query = event_table->query.str;

    ++local->stack.frames[local->stack.top].databases_accessed;  // NOLINT

    if (audit_log_include_databases != nullptr &&
        audit_log_check_database_included(event_table->table_database.str,
                                          event_table->table_database.length)) {
      ++local->stack.frames[local->stack.top].databases_included;  // NOLINT
    }

    if (audit_log_exclude_databases != nullptr &&
        audit_log_check_database_excluded(event_table->table_database.str,
                                          event_table->table_database.length)) {
      ++local->stack.frames[local->stack.top].databases_excluded;  // NOLINT
    }
  }
  return true;
}

static int audit_log_notify(MYSQL_THD thd MY_ATTRIBUTE((unused)),
                            mysql_event_class_t event_class,
                            const void *event) {
  char buf[4096];
  char *log_rec = nullptr;
  char *allocated_buf = nullptr;
  size_t len = 0, buflen;
  audit_log_thd_local *local = nullptr;

  // if thd is null pointer we return to avoid referencing null pointers
  if (thd == NULL) return 1;

  allocated_buf = get_record_buffer(thd, 0);
  local = get_thd_local(thd);

  if (!audit_log_update_thd_local(thd, local, event_class, event)) return 1;

  if (!is_event_class_allowed_by_policy(  // NOLINT
          event_class,
          static_cast<enum audit_log_policy_t>(audit_log_policy))) {
    return 0;
  }

  if (local->skip_session != 0) {
    return 0;
  }

  if (event_class == MYSQL_AUDIT_GENERAL_CLASS) {
    const auto *event_general =
        static_cast<const struct mysql_event_general *>(event);
    switch (event_general->event_subclass) {
      case MYSQL_AUDIT_GENERAL_STATUS:
        if (local->skip_query != 0) {
          break;
        }

        /* use allocated buffer if available */
        if (allocated_buf != nullptr) {
          log_rec = allocated_buf;
          buflen = local->record_buffer_size;
        } else {
          log_rec = static_cast<char *>(buf);
          buflen = sizeof(buf);
        }

        if (is_fast_log_format() != 0) {
          log_rec = audit_log_general_record_fast(
              thd, log_rec, buflen, event_general->general_command.str,
              event_general->general_time, event_general->general_error_code,
              event_general, local->db, &len, local->anonymized_ip);  // NOLINT
        } else {
          log_rec = audit_log_general_record(
              thd, log_rec, buflen, event_general->general_command.str,
              event_general->general_time, event_general->general_error_code,
              event_general, static_cast<char *>(local->db), &len,
              local->anonymized_ip);
          if (len > buflen) {
            buflen = len + 1024;
            log_rec = audit_log_general_record(
                thd, get_record_buffer(thd, buflen), buflen,
                event_general->general_command.str, event_general->general_time,
                event_general->general_error_code, event_general,
                static_cast<char *>(local->db), &len, local->anonymized_ip);
            assert(log_rec);
          }
        }
        if (log_rec != nullptr) {
          audit_log_write(log_rec, len);
        }
        break;
      case MYSQL_AUDIT_GENERAL_LOG:
      case MYSQL_AUDIT_GENERAL_ERROR:
      case MYSQL_AUDIT_GENERAL_RESULT:
        break;
    }
  } else if (event_class == MYSQL_AUDIT_CONNECTION_CLASS) {
    const auto *event_connection =
        static_cast<const struct mysql_event_connection *>(event);
    switch (event_connection->event_subclass) {
      case MYSQL_AUDIT_CONNECTION_CONNECT:
        log_rec = audit_log_connection_record(
            static_cast<char *>(buf), sizeof(buf), "Connect", time(nullptr),
            event_connection, &len, local->anonymized_ip);
        break;
      case MYSQL_AUDIT_CONNECTION_DISCONNECT:
        log_rec = audit_log_connection_record(
            static_cast<char *>(buf), sizeof(buf), "Quit", time(nullptr),
            event_connection, &len, local->anonymized_ip);
        break;
      case MYSQL_AUDIT_CONNECTION_CHANGE_USER:
        log_rec = audit_log_connection_record(
            static_cast<char *>(buf), sizeof(buf), "Change user", time(nullptr),
            event_connection, &len, local->anonymized_ip);
        break;
      default:
        break;
    }
    if (log_rec != nullptr) {
      audit_log_write(log_rec, len);
    }
  }
  return 0;
}

/*
 * Plugin system vars
 */

static MYSQL_SYSVAR_STR(file, audit_log_file,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
                            PLUGIN_VAR_MEMALLOC,
                        "The name of the log file.", nullptr, nullptr,
                        static_cast<char *>(default_audit_log_file));

static const char *audit_log_policy_names[] = {"ALL", "NONE", "LOGINS",
                                               "QUERIES", nullptr};

static TYPELIB audit_log_policy_typelib = {
    array_elements(audit_log_policy_names) - 1, "audit_log_policy_typelib",
    static_cast<const char **>(audit_log_policy_names), nullptr};

static MYSQL_SYSVAR_ENUM(
    policy, audit_log_policy, PLUGIN_VAR_RQCMDARG,
    "The policy controlling the information written by the audit log "
    "plugin to its log file.",
    nullptr, nullptr, ALL, &audit_log_policy_typelib);

static const char *audit_log_strategy_names[] = {
    "ASYNCHRONOUS", "PERFORMANCE", "SEMISYNCHRONOUS", "SYNCHRONOUS", nullptr};
static TYPELIB audit_log_strategy_typelib = {
    array_elements(audit_log_strategy_names) - 1, "audit_log_strategy_typelib",
    static_cast<const char **>(audit_log_strategy_names), nullptr};

static MYSQL_SYSVAR_ENUM(strategy, audit_log_strategy,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "The logging method used by the audit log plugin, "
                         "if FILE handler is used.",
                         nullptr, nullptr, ASYNCHRONOUS,
                         &audit_log_strategy_typelib);

static const char *audit_log_format_names[] = {"CSV2", "OLD", "NEW",
                                               "JSON", "CSV", nullptr};
static TYPELIB audit_log_format_typelib = {
    array_elements(audit_log_format_names) - 1, "audit_log_format_typelib",
    static_cast<const char **>(audit_log_format_names), nullptr};

static MYSQL_SYSVAR_ENUM(format, audit_log_format,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "The audit log file format.", nullptr, nullptr, CSV2,
                         &audit_log_format_typelib);

static const char *audit_log_handler_names[] = {"FILE", "SYSLOG", nullptr};
static TYPELIB audit_log_handler_typelib = {
    array_elements(audit_log_handler_names) - 1, "audit_log_handler_typelib",
    static_cast<const char **>(audit_log_handler_names), nullptr};

static MYSQL_SYSVAR_ENUM(handler, audit_log_handler,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "The audit log handler.", nullptr, nullptr,
                         HANDLER_FILE, &audit_log_handler_typelib);

static MYSQL_SYSVAR_ULONGLONG(
    buffer_size, audit_log_buffer_size,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "The size of the buffer for asynchronous logging, "
    "if FILE handler is used.",
    nullptr, nullptr, 1048576UL, 4096UL, ULLONG_MAX, 4096UL);

static void audit_log_rotate_on_size_update(
    MYSQL_THD thd MY_ATTRIBUTE((unused)),
    struct SYS_VAR *var MY_ATTRIBUTE((unused)),
    void *var_ptr MY_ATTRIBUTE((unused)), const void *save) {
  ulonglong new_val = *(const ulonglong *)(save);

  audit_handler_set_option(log_handler, OPT_ROTATE_ON_SIZE, &new_val);

  audit_log_rotate_on_size = new_val;
}

static MYSQL_SYSVAR_ULONGLONG(
    rotate_on_size, audit_log_rotate_on_size, PLUGIN_VAR_RQCMDARG,
    "Maximum size of the log to start the rotation, if FILE handler is used.",
    nullptr, audit_log_rotate_on_size_update, 0UL, 0UL, ULLONG_MAX, 4096UL);

static void audit_log_rotations_update(
    MYSQL_THD thd MY_ATTRIBUTE((unused)),
    struct SYS_VAR *var MY_ATTRIBUTE((unused)),
    void *var_ptr MY_ATTRIBUTE((unused)), const void *save) {
  ulonglong new_val = *(const ulonglong *)(save);

  audit_handler_set_option(log_handler, OPT_ROTATIONS, &new_val);

  audit_log_rotations = new_val;
}

static MYSQL_SYSVAR_ULONGLONG(
    rotations, audit_log_rotations, PLUGIN_VAR_RQCMDARG,
    "Maximum number of rotations to keep, if FILE handler is used.", nullptr,
    audit_log_rotations_update, 0UL, 0UL, 999UL, 1UL);

static void audit_log_flush_update(MYSQL_THD thd MY_ATTRIBUTE((unused)),
                                   struct SYS_VAR *var MY_ATTRIBUTE((unused)),
                                   void *var_ptr MY_ATTRIBUTE((unused)),
                                   const void *save) {
  char new_val = *(const char *)(save);

  if (new_val != static_cast<int>(audit_log_flush) && new_val != 0) {
    audit_log_flush = true;
    reopen_log_file();
    audit_log_flush = false;
  }
}

static MYSQL_SYSVAR_BOOL(flush, audit_log_flush, PLUGIN_VAR_OPCMDARG,
                         "Flush the log file.", nullptr, audit_log_flush_update,
                         0);
/*
  Update function of the system variable audit_log_csv2_truncation.

  @param[in]  thd       Thread object, unused parameter.
  @param[in]  var       Struct related to the system variable, unused parameter.
  @param[in]  var_ptr   Pointer to the system variable, unused parameter.
  @param[in]  save      New value of the saved system variable.
 */
static void audit_log_csv2_truncation_update(
    MYSQL_THD thd MY_ATTRIBUTE((unused)),
    struct SYS_VAR *var MY_ATTRIBUTE((unused)),
    void *var_ptr MY_ATTRIBUTE((unused)), const void *save) {
  char new_val = *(const char *)(save);
  audit_log_csv2_truncation = new_val;
}

static MYSQL_SYSVAR_BOOL(csv2_truncation, audit_log_csv2_truncation,
                         PLUGIN_VAR_OPCMDARG,
                         "When the memory space is insufficient and the "
                         "file format is CSV2, the SQL statement is "
                         "truncated and outputted.",
                         nullptr, audit_log_csv2_truncation_update, 1);

/*
  Update function of the system variable audit_log_csv2_escape.

  @param[in]  thd       Thread object, unused parameter.
  @param[in]  var       Struct related to the system variable, unused parameter.
  @param[in]  var_ptr   Pointer to the system variable, unused parameter.
  @param[in]  save      New value of the saved system variable.
 */
static void audit_log_csv2_escape_update(
    MYSQL_THD thd MY_ATTRIBUTE((unused)),
    struct SYS_VAR *var MY_ATTRIBUTE((unused)),
    void *var_ptr MY_ATTRIBUTE((unused)), const void *save) {
  char new_val = *(const char *)(save);
  audit_log_csv2_escape = new_val;
}

static MYSQL_SYSVAR_BOOL(csv2_escape, audit_log_csv2_escape,
                         PLUGIN_VAR_OPCMDARG,
                         "When audit logs are output in CSV2 format, "
                         "double quotation marks in SQL statements are "
                         "escaped.",
                         nullptr, audit_log_csv2_escape_update, 0);

/*
  Update function of the system variable audit_log_csv2_old_separated_format.

  @param[in]  thd       Thread object, unused parameter.
  @param[in]  var       Struct related to the system variable, unused parameter.
  @param[in]  var_ptr   Pointer to the system variable, unused parameter.
  @param[in]  save      New value of the saved system variable.
 */
static void audit_log_csv2_old_separated_format_update(
    MYSQL_THD thd MY_ATTRIBUTE((unused)),
    struct SYS_VAR *var MY_ATTRIBUTE((unused)),
    void *var_ptr MY_ATTRIBUTE((unused)), const void *save) {
  char new_val = *(const char *)(save);
  audit_log_csv2_old_separated_format = new_val;
}

static MYSQL_SYSVAR_BOOL(csv2_old_separated_format,
                         audit_log_csv2_old_separated_format,
                         PLUGIN_VAR_OPCMDARG,
                         "When audit logs are output in CSV2 format, "
                         "restore the old CSV2 format.",
                         nullptr, audit_log_csv2_old_separated_format_update,
                         0);

static MYSQL_SYSVAR_STR(
    syslog_ident, audit_log_syslog_ident,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
    "The string that will be prepended to each log message, "
    "if SYSLOG handler is used.",
    nullptr, nullptr, static_cast<char *>(default_audit_log_syslog_ident));

static TYPELIB audit_log_syslog_facility_typelib = {
    array_elements(audit_log_syslog_facility_names) - 1,
    "audit_log_syslog_facility_typelib",
    static_cast<const char **>(audit_log_syslog_facility_names), nullptr};

static MYSQL_SYSVAR_ENUM(
    syslog_facility, audit_log_syslog_facility, PLUGIN_VAR_RQCMDARG,
    "The syslog facility to assign to messages, if SYSLOG handler is used.",
    nullptr, nullptr, 0, &audit_log_syslog_facility_typelib);

static TYPELIB audit_log_syslog_priority_typelib = {
    array_elements(audit_log_syslog_priority_names) - 1,
    "audit_log_syslog_priority_typelib",
    static_cast<const char **>(audit_log_syslog_priority_names), nullptr};

static MYSQL_SYSVAR_ENUM(
    syslog_priority, audit_log_syslog_priority, PLUGIN_VAR_RQCMDARG,
    "Priority to be assigned to all messages written to syslog.", nullptr,
    nullptr, 0, &audit_log_syslog_priority_typelib);

static MYSQL_THDVAR_STR(record_buffer,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC |
                            PLUGIN_VAR_NOSYSVAR | PLUGIN_VAR_NOCMDOPT,
                        "Buffer for query formatting.", nullptr, nullptr, "");

static MYSQL_THDVAR_STR(query_stack,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC |
                            PLUGIN_VAR_NOSYSVAR | PLUGIN_VAR_NOCMDOPT,
                        "Query stack.", nullptr, nullptr, "");

static int audit_log_exclude_accounts_validate(
    MYSQL_THD thd MY_ATTRIBUTE((unused)),
    struct SYS_VAR *var MY_ATTRIBUTE((unused)), void *save,
    struct st_mysql_value *value) {
  if (audit_log_include_accounts) return 1;

  return check_func_str(thd, var, save, value);
}

static void audit_log_exclude_accounts_update(
    MYSQL_THD thd MY_ATTRIBUTE((unused)),
    struct SYS_VAR *var MY_ATTRIBUTE((unused)),
    void *var_ptr MY_ATTRIBUTE((unused)), const void *save) {
  assert(audit_log_include_accounts == nullptr);

  update_func_str(thd, var, var_ptr, save);

  if (audit_log_exclude_accounts != nullptr) {
    audit_log_set_exclude_accounts(audit_log_exclude_accounts);
  } else {
    audit_log_set_exclude_accounts("");
  }
}

static MYSQL_SYSVAR_STR(exclude_accounts, audit_log_exclude_accounts,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Comma separated list of accounts "
                        "for which events should not be logged.",
                        audit_log_exclude_accounts_validate,
                        audit_log_exclude_accounts_update, nullptr);

static int audit_log_include_accounts_validate(MYSQL_THD thd,
                                               struct SYS_VAR *var, void *save,
                                               struct st_mysql_value *value) {
  if (audit_log_exclude_accounts) return 1;

  return check_func_str(thd, var, save, value);
}

static void audit_log_include_accounts_update(MYSQL_THD thd,
                                              struct SYS_VAR *var,
                                              void *var_ptr, const void *save) {
  assert(audit_log_exclude_accounts == nullptr);

  update_func_str(thd, var, var_ptr, save);

  if (audit_log_include_accounts != nullptr) {
    audit_log_set_include_accounts(audit_log_include_accounts);
  } else {
    audit_log_set_include_accounts("");
  }
}

static MYSQL_SYSVAR_STR(include_accounts, audit_log_include_accounts,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Comma separated list of accounts for "
                        "which events should be logged.",
                        audit_log_include_accounts_validate,
                        audit_log_include_accounts_update, nullptr);

static int audit_log_exclude_databases_validate(MYSQL_THD thd,
                                                struct SYS_VAR *var, void *save,
                                                struct st_mysql_value *value) {
  if (audit_log_include_databases) return 1;

  return check_func_str(thd, var, save, value);
}

static void audit_log_exclude_databases_update(MYSQL_THD thd,
                                               struct SYS_VAR *var,
                                               void *var_ptr,
                                               const void *save) {
  assert(audit_log_include_databases == nullptr);

  update_func_str(thd, var, var_ptr, save);

  if (audit_log_exclude_databases != nullptr) {
    audit_log_set_exclude_databases(audit_log_exclude_databases);
  } else {
    audit_log_set_exclude_databases("");
  }
}

static MYSQL_SYSVAR_STR(exclude_databases, audit_log_exclude_databases,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Comma separated list of databases "
                        "for which events should not be logged.",
                        audit_log_exclude_databases_validate,
                        audit_log_exclude_databases_update, nullptr);

static int audit_log_include_databases_validate(MYSQL_THD thd,
                                                struct SYS_VAR *var, void *save,
                                                struct st_mysql_value *value) {
  if (audit_log_exclude_databases) return 1;

  return check_func_str(thd, var, save, value);
}

static void audit_log_include_databases_update(
    MYSQL_THD thd MY_ATTRIBUTE((unused)),
    struct SYS_VAR *var MY_ATTRIBUTE((unused)),
    void *var_ptr MY_ATTRIBUTE((unused)), const void *save) {
  assert(audit_log_exclude_databases == nullptr);
  update_func_str(thd, var, var_ptr, save);
  if (audit_log_include_databases != nullptr) {
    audit_log_set_include_databases(audit_log_include_databases);
  } else {
    audit_log_set_include_databases("");
  }
}

static MYSQL_SYSVAR_STR(include_databases, audit_log_include_databases,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Comma separated list of databases for which "
                        "events should be logged.",
                        audit_log_include_databases_validate,
                        audit_log_include_databases_update, nullptr);

static int audit_log_exclude_commands_validate(MYSQL_THD thd,
                                               struct SYS_VAR *var, void *save,
                                               struct st_mysql_value *value) {
  if (audit_log_include_commands) return 1;

  return check_func_str(thd, var, save, value);
}

static void audit_log_exclude_commands_update(MYSQL_THD thd,
                                              struct SYS_VAR *var,
                                              void *var_ptr, const void *save) {
  assert(audit_log_include_commands == nullptr);

  update_func_str(thd, var, var_ptr, save);

  if (audit_log_exclude_commands != nullptr) {
    audit_log_set_exclude_commands(audit_log_exclude_commands);
  } else {
    audit_log_set_exclude_commands("");
  }
}

static MYSQL_SYSVAR_STR(exclude_commands, audit_log_exclude_commands,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Comma separated list of commands "
                        "for which events should not be logged.",
                        audit_log_exclude_commands_validate,
                        audit_log_exclude_commands_update, nullptr);

static int audit_log_include_commands_validate(MYSQL_THD thd,
                                               struct SYS_VAR *var, void *save,
                                               struct st_mysql_value *value) {
  if (audit_log_exclude_commands) return 1;

  return check_func_str(thd, var, save, value);
}

static void audit_log_include_commands_update(MYSQL_THD thd,
                                              struct SYS_VAR *var,
                                              void *var_ptr, const void *save) {
  assert(audit_log_exclude_commands == nullptr);

  update_func_str(thd, var, var_ptr, save);

  if (audit_log_include_commands != nullptr) {
    audit_log_set_include_commands(audit_log_include_commands);
  } else {
    audit_log_set_include_commands("");
  }
}

static MYSQL_SYSVAR_STR(include_commands, audit_log_include_commands,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Comma separated list of commands for which "
                        "events should be logged.",
                        audit_log_include_commands_validate,
                        audit_log_include_commands_update, nullptr);

static MYSQL_THDVAR_STR(local,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC |
                            PLUGIN_VAR_NOSYSVAR | PLUGIN_VAR_NOCMDOPT,
                        "Local store.", nullptr, nullptr, "");

static MYSQL_THDVAR_ULONG(local_ptr,
                          PLUGIN_VAR_READONLY | PLUGIN_VAR_NOSYSVAR |
                              PLUGIN_VAR_NOCMDOPT,
                          "Local store ptr.", nullptr, nullptr, 0, 0, ULONG_MAX,
                          0);
static void audit_log_force_rotate_update(
    MYSQL_THD thd MY_ATTRIBUTE((unused)),
    struct SYS_VAR *var MY_ATTRIBUTE((unused)),
    void *var_ptr MY_ATTRIBUTE((unused)), const void *save) {
  char new_val = *(const char *)(save);

  if ((new_val != audit_log_force_rotate) && new_val) {
    audit_log_force_rotate = true;
    audit_handler_rotate(log_handler);
    audit_log_force_rotate = false;
  }
}
static MYSQL_SYSVAR_BOOL(force_rotate, audit_log_force_rotate,
                         PLUGIN_VAR_OPCMDARG, "force rotate the log file.",
                         nullptr, audit_log_force_rotate_update, 0);
static void audit_log_anonymized_ip_update(MYSQL_THD thd, struct SYS_VAR *var,
                                           void *var_ptr, const void *save) {
  update_func_str(thd, var, var_ptr, save);

  if (audit_log_anonymized_ip != nullptr) {
    audit_log_set_anonymized_ip(audit_log_anonymized_ip);
  } else {
    audit_log_set_anonymized_ip("");
  }
}
static int audit_log_anonymized_ip_validate(MYSQL_THD thd, struct SYS_VAR *var,
                                            void *save,
                                            struct st_mysql_value *value) {
  return check_func_str(thd, var, save, value);
}

static MYSQL_SYSVAR_STR(anonymized_ip, audit_log_anonymized_ip,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Comma separated list of ip for which "
                        "events should be anonymized.",
                        audit_log_anonymized_ip_validate,
                        audit_log_anonymized_ip_update, nullptr);

static MYSQL_SYSVAR_BOOL(
    thread_safe, audit_log_thread_safe,
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
    "Write audit log thread safe or not, if FILE handler is used.", nullptr,
    nullptr, true);

static struct SYS_VAR *audit_log_system_variables[] = {
    MYSQL_SYSVAR(file),
    MYSQL_SYSVAR(policy),
    MYSQL_SYSVAR(strategy),
    MYSQL_SYSVAR(format),
    MYSQL_SYSVAR(buffer_size),
    MYSQL_SYSVAR(rotate_on_size),
    MYSQL_SYSVAR(rotations),
    MYSQL_SYSVAR(flush),
    MYSQL_SYSVAR(handler),
    MYSQL_SYSVAR(syslog_ident),
    MYSQL_SYSVAR(syslog_priority),
    MYSQL_SYSVAR(syslog_facility),
    MYSQL_SYSVAR(record_buffer),
    MYSQL_SYSVAR(query_stack),
    MYSQL_SYSVAR(exclude_accounts),
    MYSQL_SYSVAR(include_accounts),
    MYSQL_SYSVAR(exclude_databases),
    MYSQL_SYSVAR(include_databases),
    MYSQL_SYSVAR(exclude_commands),
    MYSQL_SYSVAR(include_commands),
    MYSQL_SYSVAR(local),
    MYSQL_SYSVAR(local_ptr),
    MYSQL_SYSVAR(force_rotate),
    MYSQL_SYSVAR(anonymized_ip),
    MYSQL_SYSVAR(csv2_truncation),
    MYSQL_SYSVAR(csv2_escape),
    MYSQL_SYSVAR(csv2_old_separated_format),
    MYSQL_SYSVAR(thread_safe),
    nullptr};

char thd_local_init_buf[sizeof(audit_log_thd_local)];

void MY_ATTRIBUTE((constructor)) audit_log_so_init() {
  memset(thd_local_init_buf, 1, sizeof(thd_local_init_buf) - 1);
  thd_local_init_buf[sizeof(thd_local_init_buf) - 1] = 0;
}

/*
 Return pointer to THD specific data.
 */
static audit_log_thd_local *get_thd_local(MYSQL_THD thd) {
  audit_log_thd_local *local = (audit_log_thd_local *)THDVAR(thd, local_ptr);

  static_assert(sizeof(THDVAR(thd, local_ptr)) >= sizeof(void *),
                "sizeof(THDVAR(thd, local_ptr) < sizeof(void *)");

  if (unlikely(local == nullptr)) {
    THDVAR_SET(thd, local, thd_local_init_buf);
    local = (audit_log_thd_local *)THDVAR(thd, local);
    memset(local, 0, sizeof(audit_log_thd_local));
    THDVAR(thd, local_ptr) = (ulong)local;

    realloc_stack_frames(thd, 4);
  }

  return local;
}
/*
  Check whether the memory is successfully reallocated.

  @param[in]  before_buffer     Pointer to the previous buffer.
  @param[in]  new_buffer        Pointer to the newly allocated buffer.

  @return
    true on success, false if the memory reallocation fails.
 */
static bool check_memory_reallocate(const char *before_buffer,
                                    const char *new_buffer) {
  if (before_buffer == new_buffer &&
      (!audit_log_csv2_truncation || !is_fast_log_format())) {
    /* The program stops running actively. */
    sql_print_error("Out of memory in Audit Log, memory request failed!");
    assert(0);
  } else if (before_buffer == new_buffer) {
    sql_print_warning(
        "Audit Log: The new memory required cannot be obtained. "
        "The old memory will be reused and the SQL statement may be "
        "truncated.");
  }
  return before_buffer != new_buffer;
}
/*
 Allocate and return buffer of given size.
 */
static char *get_record_buffer(MYSQL_THD thd, size_t size) {
  audit_log_thd_local *local = get_thd_local(thd);
  if (local->record_buffer_size < size) {
    char *current_record_buffer, *new_record_buffer;

    char *buf = (char *)DBUG_EVALUATE_IF(
        "simulate_audit_malloc_fail", nullptr,
        my_malloc(PSI_NOT_INSTRUMENTED, size, MYF(MY_FAE)));
    if (!check_memory_reallocate(buf, nullptr)) {
      return local->record_buffer;
    }

    memset(buf, 1, size - 1);
    buf[size - 1] = 0;
    current_record_buffer = local->record_buffer;
    THDVAR_SET(thd, record_buffer, buf);
    new_record_buffer = THDVAR(thd, record_buffer);
    my_free(buf);
    if (!check_memory_reallocate(current_record_buffer, new_record_buffer)) {
      return local->record_buffer;
    }

    local->record_buffer = new_record_buffer;
    local->record_buffer_size = size;
  }
  return local->record_buffer;
}

/*
 Allocate and return given number of stack frames.
 */
static query_stack_frame *realloc_stack_frames(MYSQL_THD thd, size_t size) {
  audit_log_thd_local *local = get_thd_local(thd);
  query_stack_frame *stack = (query_stack_frame *)THDVAR(thd, query_stack);

  if (local->stack.size < size) {
    char *buf = (char *)my_malloc(
        PSI_NOT_INSTRUMENTED,
        (local->stack.size + size) * sizeof(query_stack_frame), MYF(MY_FAE));
    memset(buf + local->stack.size * sizeof(query_stack_frame), 1,
           size * sizeof(query_stack_frame) - 1);
    buf[(local->stack.size + size) * sizeof(query_stack_frame) - 1] = 0;
    if (local->stack.size > 0)
      memcpy(buf, stack, local->stack.size * sizeof(query_stack_frame));
    THDVAR_SET(thd, query_stack,
               buf + local->stack.size * sizeof(query_stack_frame));
    stack = (query_stack_frame *)THDVAR(thd, query_stack);
    memset(stack, 0, size * sizeof(query_stack_frame));
    if (local->stack.size > 0)
      memcpy(stack, buf, local->stack.size * sizeof(query_stack_frame));
    local->stack.frames = stack;
    local->stack.size = size;
    my_free(buf);
  }

  return stack;
}

/*
  Plugin type-specific descriptor
*/
static struct st_mysql_audit audit_log_descriptor = {
    MYSQL_AUDIT_INTERFACE_VERSION, /* interface version    */
    nullptr,                       /* release_thd function */
    audit_log_notify,              /* notify function      */
    {MYSQL_AUDIT_GENERAL_ALL, MYSQL_AUDIT_CONNECTION_ALL, 0, 0,
     MYSQL_AUDIT_TABLE_ACCESS_ALL, 0, 0, 0, 0, 0} /* class mask           */
};

/*
  Plugin status variables for SHOW STATUS
*/

static struct SHOW_VAR audit_log_status_variables[] = {
    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

/*
  Plugin library descriptor
*/

mysql_declare_plugin(audit_log){
    MYSQL_AUDIT_PLUGIN,                   /* type                            */
    &audit_log_descriptor,                /* descriptor                      */
    "audit_log",                          /* name                            */
    "Percona LLC and/or its affiliates.", /* author                          */
    "Audit log",                          /* description                     */
    PLUGIN_LICENSE_GPL,
    audit_log_plugin_init,   /* init function (when loaded)     */
    nullptr,                 /* check uninstall function        */
    audit_log_plugin_deinit, /* deinit function (when unloaded) */
    PLUGIN_VERSION,          /* version                         */
    static_cast<SHOW_VAR *>(audit_log_status_variables), /* status variables */
    static_cast<SYS_VAR **>(audit_log_system_variables), /* system variables */
    nullptr,
    0,
} mysql_declare_plugin_end;
