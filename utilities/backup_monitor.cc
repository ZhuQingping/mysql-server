/*****************************************************************************

Copyright (c) 2025, Huawei and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

#include "my_config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <zlib.h>
#include <iostream>

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/filewritestream.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include "m_string.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_dir.h"
#include "my_getopt.h"
#include "my_io.h"
#include "my_macros.h"
#include "my_time.h"
#include "print_version.h"
#include "typelib.h"

#include "storage/dstore/backup/local_backup.h"
#include "ut0crc32.h"
#include "wal/dstore_wal_read_buffer.h"

constexpr const char *BACKUP_MONITOR_WELCOME_COPYRIGHT_NOTICE =
    "Copyright (c) 2025, Huawei and/or its affiliates. All Rights Reserved.\n";

const uint64 DEFAULT_WAL_FILE_SIZE = (128UL << 20);  // 128m
const uint64_t LB_INVALID_LSN = 0xFFFFFFFFFFFFFFFF;

/** Global options structure. Option values passed at command line are
stored in this structure */
struct Monitor_options {
  my_time_t monitor_timestamp;
  bool is_monitor_timestamp;
  char *restore_meta_path;
  bool is_restore_meta_path;
  uint32_t wal_file_size;
  bool is_wal_file_size;
  bool is_time_point_list;
  uint32_t max_expect_count;
  uint64_t parent_offset;
  uint64_t child_offset;
  bool is_wal_archive_child_meta;
  bool is_binlog;
  bool is_json_crc;
  const char *dbug_setting;
  bool is_dump_file;
  char *dump_filename;
  bool pretty;
};
struct Monitor_options opts;

/* Command line argument for backup_monitor tool. */
static struct my_option backup_monitor_options[] = {
    {"help", 'h', "Display this help and exit.", nullptr, nullptr, nullptr,
     GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"version", 'v', "Display version information and exit.", nullptr, nullptr,
     nullptr, GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
#ifndef NDEBUG
    {"debug", '#', "Output debug log. See " REFMAN "dbug-package.html",
     &opts.dbug_setting, &opts.dbug_setting, nullptr, GET_STR, OPT_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
#endif /* !NDEBUG */
    {"dump-file", 'd',
     "Dump the monitor information into the file passed by user. "
     "Without the filename, it will default to stdout.",
     &opts.dump_filename, &opts.dump_filename, nullptr, GET_STR, REQUIRED_ARG,
     0, 0, 0, nullptr, 0, nullptr},
    {"timestamp", 't',
     "Timestamp equal to the argument. "
     "The restore meta file will be created based on this timestamp.",
     &opts.monitor_timestamp, &opts.monitor_timestamp, nullptr, GET_ULL,
     REQUIRED_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"restore-meta-path", 'r',
     "Restore meta file path. "
     "The restore meta file will be created based on this path.",
     &opts.restore_meta_path, &opts.restore_meta_path, nullptr, GET_STR,
     REQUIRED_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"wal-file-size", 's', "Wal segment file size.", &opts.wal_file_size,
     &opts.wal_file_size, nullptr, GET_UINT, REQUIRED_ARG, 0, 0, UINT_MAX,
     nullptr, 0, nullptr},
    {"list", 'l',
     "List time point recorded in full local backup meta file from one file "
     "offset.",
     &opts.is_time_point_list, &opts.is_time_point_list, nullptr, GET_BOOL,
     NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"wal-list", 'w',
     "List time point recorded in wal archive meta file from one file offset.",
     &opts.is_wal_archive_child_meta, &opts.is_wal_archive_child_meta, nullptr,
     GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"count", 'c', "Max count of the time point list displayed.",
     &opts.max_expect_count, &opts.max_expect_count, nullptr, GET_UINT,
     REQUIRED_ARG, 0, 0, UINT_MAX, nullptr, 0, nullptr},
    {"offset", 'o',
     "Begin offset during searching full backup/wal archive parent meta file.",
     &opts.parent_offset, &opts.parent_offset, nullptr, GET_ULL, REQUIRED_ARG,
     0, 0, ULLONG_MAX, nullptr, 0, nullptr},
    {"archive", 'a',
     "Begin offset during searching wal archive child meta file.",
     &opts.child_offset, &opts.child_offset, nullptr, GET_ULL, REQUIRED_ARG, 0,
     0, ULLONG_MAX, nullptr, 0, nullptr},
    {"binlog", 'b',
     "Show full local backup binlog meta based on binlog meta file.",
     &opts.is_binlog, &opts.is_binlog, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"json-crc", 'j',
     "Read full local backup meta json file, and verify if the crc is matched.",
     &opts.is_json_crc, &opts.is_json_crc, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"pretty", 'p',
     "Pretty format the monitor information output. "
     "If false, output would be not human readable but it will be of less "
     "size.",
     &opts.pretty, &opts.pretty, nullptr, GET_BOOL, OPT_ARG, 1, 0, 0, nullptr,
     0, nullptr},

    {nullptr, 0, nullptr, nullptr, nullptr, nullptr, GET_NO_ARG, NO_ARG, 0, 0,
     0, nullptr, 0, nullptr}};

/**
  Report a failed assertion.

  @param[in]      expr      the failed assertion if not NULL
  @param[in]      file      source file containing the assertion
  @param[in]      line      line number of the assertion
*/
[[noreturn]] void ut_dbg_assertion_failed(const char *expr, const char *file,
                                          uint64_t line) {
  fprintf(stderr,
          "backup_monitor: Assertion failure in file %s line " UINT64PF "\n",
          file, line);

  if (expr != nullptr) {
    fprintf(stderr, "backup_monitor: Failing assertion: %s\n", expr);
  }

  fflush(stderr);
  fflush(stdout);
  abort();
}

/**
  Tries to delete temporary file.

  @param[in]      temp_filename      The name of file to delete
*/
static void try_delete_temporary_filename(const char *temp_filename) {
  if (my_delete(temp_filename, MYF(0)) != 0) {
    ib::warn() << "Removal of temporary file " << temp_filename
               << " failed because of system error: " << strerror(errno);
  }
}

/**
  Create a file in a system's temporary directory.

  @param[in,out]      temp_file_buf       Buffer to hold the temporary file
                                          name generated
  @param[in]          dir                 directory used for creation of
                                          temporary file, nullptr if system
                                          tmpdir to be used
  @param[in]          prefix_pattern      the temp file name is prefixed with
                                          this string

  @return             File pointer of the created file
*/
static FILE *create_tmp_file(char *temp_file_buf, const char *dir,
                             const char *prefix_pattern) {
  FILE *file = nullptr;
  File fd = create_temp_file(temp_file_buf, dir, prefix_pattern,
                             O_CREAT | O_RDWR, KEEP_FILE, MYF(0));

  if (fd >= 0) {
    file = my_fdopen(fd, temp_file_buf, O_RDWR, MYF(0));
  }

  DBUG_EXECUTE_IF("bm_tmp_file_fail", file = nullptr; errno = EACCES;);

  if (file == nullptr) {
    ib::error() << "Unable to create temporary file. err: " << strerror(errno);

    if (fd >= 0) {
      try_delete_temporary_filename(temp_file_buf);
      my_close(fd, MYF(0));
    }
  }

  return (file);
}

/** Print the backup_monitor tool usage. */
static void usage() {
#ifdef NDEBUG
  print_version();
#else
  print_version_debug();
#endif /* NDEBUG */
  puts(BACKUP_MONITOR_WELCOME_COPYRIGHT_NOTICE);
  printf(
      "Usage: %s [-v] [-d <dump file name>] filepath [-t <timestamp>] [-r "
      "<restore meta path>] [-s <wal file size>] [-l] [-w] [-c <max count>] "
      "[-o <offset>] [-a <child offset>] [-b] [-j]\n",
      my_progname);
  my_print_help(backup_monitor_options);
  my_print_variables(backup_monitor_options);
}

/**
  Convert timestamp to string.

  @param[in]       timestamp      timestamp to be converted.
  @param[out]      to             string of the specific timestamp.

  @return                         The length of the result string.
*/
static int convert_timestamp_to_str(my_time_t timestamp, char *to) {
  MYSQL_TIME time;
  struct tm tm_time;
  localtime_r(&timestamp, &tm_time);
  localtime_to_TIME(&time, &tm_time);
  time.time_type = MYSQL_TIMESTAMP_DATETIME;
  return my_TIME_to_str(time, to, 0);
}

/** Get RetStatus string. */
static std::string get_ret_string(DSTORE::RetStatus ret) {
  return ret == DSTORE::RetStatus::DSTORE_SUCC ? "Success" : "Fail";
}

/** Get meta type string. */
static std::string get_meta_type_string() {
  return opts.is_wal_archive_child_meta ? "Wal Archive" : "Full Local Backup";
}

/** Parse the options passed to tool. */
extern "C" bool backup_monitor_get_one_option(int optid,
                                              const struct my_option *opt
                                              [[maybe_unused]],
                                              char *argument [[maybe_unused]]) {
  switch (optid) {
#ifndef NDEBUG
    case '#':
      opts.dbug_setting =
          argument ? argument : IF_WIN("d:O,bm.trace", "d:o,/tmp/bm.trace");
      DBUG_PUSH(opts.dbug_setting);
      break;
#endif /* !NDEBUG */
    case 'v':
#ifdef NDEBUG
      print_version();
#else
      print_version_debug();
#endif /* DBUG */
      exit(EXIT_SUCCESS);
      break;
    case 'd':
      opts.is_dump_file = true;
      break;
    case 't':
      opts.is_monitor_timestamp = true;
      break;
    case 'r':
      opts.is_restore_meta_path = true;
      break;
    case 's':
      opts.is_wal_file_size = true;
      break;
    case 'w':
      opts.is_time_point_list = true;
      break;
    case 'l':
    case 'b':
    case 'j':
    case 'p':
      break;
    case 'h':
      usage();
      exit(EXIT_SUCCESS);
      break;
  }

  return (false);
}

/** Retrieve the options passed to the tool. */
static bool get_options(int *argc, char ***argv) {
  if (handle_options(argc, argv, backup_monitor_options,
                     backup_monitor_get_one_option)) {
    exit(true);
  }

  /* The next arg must be the filename */
  if (!*argc) {
    usage();
    return (true);
  }

  return (false);
}

/** Error logging classes. */
namespace ib {

logger::~logger() = default;

info::~info() {
  std::cerr << "[INFO] backup_monitor: " << m_oss.str() << "." << std::endl;
}

warn::~warn() {
  std::cerr << "[WARNING] backup_monitor: " << m_oss.str() << "." << std::endl;
}

error::~error() {
  std::cerr << "[ERROR] backup_monitor: " << m_oss.str() << "." << std::endl;
}

/*
MSVS complains: Warning C4722: destructor never returns, potential memory leak.
But, the whole point of using ib::fatal temporary object is to cause an abort.
*/
MY_COMPILER_DIAGNOSTIC_PUSH()
MY_COMPILER_MSVC_DIAGNOSTIC_IGNORE(4722)

fatal::~fatal() {
  std::cerr << "[FATAL] backup_monitor: " << m_oss.str() << "." << std::endl;
  ut_error;
}

// Restore the MSVS checks for Warning C4722, silenced for ib::fatal::~fatal().
MY_COMPILER_DIAGNOSTIC_POP()

class dbug : public logger {
 public:
  ~dbug() override {
    DBUG_PRINT("backup_monitor", ("%s", m_oss.str().c_str()));
  }
};
}  // namespace ib

/* Meta Type. */
enum class MetaType { RESTOREMETA, TIMEPOINTLIST, BINLOGMETA, METAJSON };

/** Struct to store the backup meta info. */
struct Meta_info {
 public:
  MetaType meta_type;
  DSTORE::RetStatus ret_create_restore_meta;
  DSTORE::RetStatus ret_show_restore_file_list;
  local_backup_point point;
  std::string full_dir;
  std::vector<std::string> wal_file_list;
  DSTORE::RetStatus ret_show_time_point_list;
  std::vector<std::shared_ptr<local_backup_point>> time_point_list;
  CDE::MetaFileCursor parent_meta_cursor;
  CDE::MetaFileCursor child_meta_cursor;
  DSTORE::RetStatus ret_read_full_local_backup_binlog;
  std::vector<CDE::LocalBackupBinlogPoint> binlog_points;
  DSTORE::RetStatus ret_verify_meta_json_crc;
  ha_checksum json_storage_crc;
  ha_checksum json_calc_crc;

  void clear() {
    ret_create_restore_meta = DSTORE::RetStatus::DSTORE_FAIL;
    ret_show_restore_file_list = DSTORE::RetStatus::DSTORE_FAIL;
    point.start_recovery_lsn = LB_INVALID_LSN;
    point.lsn_point = LB_INVALID_LSN;
    point.time_point = 0;
    full_dir.clear();
    wal_file_list.clear();
    ret_show_time_point_list = DSTORE::RetStatus::DSTORE_FAIL;
    time_point_list.clear();
    parent_meta_cursor.fileName.clear();
    parent_meta_cursor.beginOffset = 0;
    parent_meta_cursor.endFileLength = 0;
    child_meta_cursor.fileName.clear();
    child_meta_cursor.beginOffset = 0;
    child_meta_cursor.endFileLength = 0;
    ret_read_full_local_backup_binlog = DSTORE::RetStatus::DSTORE_FAIL;
    binlog_points.clear();
    ret_verify_meta_json_crc = DSTORE::RetStatus::DSTORE_FAIL;
    json_storage_crc = 0;
    json_calc_crc = 0;
  }
};

/** Class to monitor backup meta info. */
class Backup_monitor {
 public:
  /**
    Constructor.

    @param[in]          meta_file       Meta file of backup.
    @param[in,out]      out_stream      Stream to dump meta info.
  */
  Backup_monitor(FILE *out_stream, Meta_info meta)
      : m_out_stream(out_stream), m_meta(meta) {}

  /** Destructor. */
  ~Backup_monitor() {}

  /**
    Dump one record for restore meta info.

    @param[out]      obj            Single record object of rapidjson.
    @param[out]      allocator      Allocator of rapidjson.
  */
  void dump_restore_meta_rec(rapidjson::Value &obj,
                             rapidjson::Document::AllocatorType &allocator);

  /**
    Dump time point list.

    @param[out]      obj            Single record object of rapidjson.
    @param[out]      allocator      Allocator of rapidjson.
  */
  void dump_time_point_list(rapidjson::Value &obj,
                            rapidjson::Document::AllocatorType &allocator);

  /**
    Dump one record for time point list.

    @param[in]       point          Single record for time point list.
    @param[out]      obj            Single record object of rapidjson.
    @param[out]      allocator      Allocator of rapidjson.
  */
  void dump_time_point_rec(const local_backup_point *point,
                           rapidjson::Value &obj,
                           rapidjson::Document::AllocatorType &allocator);

  /**
    Dump one record for full local backup binlog meta.

    @param[out]      obj            Single record object of rapidjson.
    @param[out]      allocator      Allocator of rapidjson.
  */
  void dump_full_local_backup_binlog(
      rapidjson::Value &obj, rapidjson::Document::AllocatorType &allocator);

  /**
    Dump one record for full local backup meta json file crc check.

    @param[out]      obj            Single record object of rapidjson.
    @param[out]      allocator      Allocator of rapidjson.
  */
  void dump_meta_json_crc(rapidjson::Value &obj,
                          rapidjson::Document::AllocatorType &allocator);

  /** Dump meta info. */
  void dump();

  /** Output stream to dump the parsed meta info. */
  FILE *m_out_stream;
  Meta_info m_meta;
};

/**
  Dump one record for restore meta info.

  @param[out]      obj            Single record object of rapidjson.
  @param[out]      allocator      Allocator of rapidjson.
*/
void Backup_monitor::dump_restore_meta_rec(
    rapidjson::Value &obj, rapidjson::Document::AllocatorType &allocator) {
  obj.AddMember(
      "createRestoreMeta",
      rapidjson::Value(get_ret_string(m_meta.ret_create_restore_meta).c_str(),
                       allocator)
          .Move(),
      allocator);
  obj.AddMember(
      "showRestoreFileList",
      rapidjson::Value(
          get_ret_string(m_meta.ret_show_restore_file_list).c_str(), allocator)
          .Move(),
      allocator);
  obj.AddMember("timePoint", m_meta.point.time_point, allocator);
  char print_buff[MAX_DATE_STRING_REP_LENGTH];
  convert_timestamp_to_str(m_meta.point.time_point, print_buff);
  obj.AddMember("timePointStr",
                rapidjson::Value(
                    std::string(print_buff, MAX_DATE_STRING_REP_LENGTH).c_str(),
                    allocator)
                    .Move(),
                allocator);

  obj.AddMember("lsnPoint", m_meta.point.lsn_point, allocator);
  /* if wal-file-size is not specified, use the default value: 128M. */
  uint32_t wal_file_size =
      opts.is_wal_file_size ? opts.wal_file_size : DEFAULT_WAL_FILE_SIZE;
  uint64 last_wal_file_size = m_meta.point.lsn_point % wal_file_size;
  obj.AddMember("lastWalFileSize", last_wal_file_size, allocator);
  obj.AddMember("startRecoveryLsn", m_meta.point.start_recovery_lsn, allocator);
  obj.AddMember("backupDir",
                rapidjson::Value(m_meta.full_dir.c_str(), allocator).Move(),
                allocator);

  rapidjson::Value arr(rapidjson::kArrayType);
  for (const auto &str : m_meta.wal_file_list) {
    rapidjson::Value val;
    val.SetString(str.c_str(), str.size(), allocator);
    arr.PushBack(val, allocator);
  }
  obj.AddMember("walFileList", arr, allocator);
}

/**
  Dump time point list.

  @param[out]      obj            Single record object of rapidjson.
  @param[out]      allocator      Allocator of rapidjson.
*/
void Backup_monitor::dump_time_point_list(
    rapidjson::Value &obj, rapidjson::Document::AllocatorType &allocator) {
  obj.AddMember(
      "showTimePointList",
      rapidjson::Value(get_ret_string(m_meta.ret_show_time_point_list).c_str(),
                       allocator)
          .Move(),
      allocator);
  obj.AddMember(
      "metaType",
      rapidjson::Value(get_meta_type_string().c_str(), allocator).Move(),
      allocator);
  if (!opts.is_wal_archive_child_meta) {
    obj.AddMember(
        "fileName",
        rapidjson::Value(m_meta.parent_meta_cursor.fileName.c_str(), allocator)
            .Move(),
        allocator);
    obj.AddMember("beginOffset", m_meta.parent_meta_cursor.beginOffset,
                  allocator);
    obj.AddMember("endFileLength", m_meta.parent_meta_cursor.endFileLength,
                  allocator);
  } else {
    obj.AddMember(
        "parentFileName",
        rapidjson::Value(m_meta.parent_meta_cursor.fileName.c_str(), allocator)
            .Move(),
        allocator);
    obj.AddMember("parentBeginOffset", m_meta.parent_meta_cursor.beginOffset,
                  allocator);
    obj.AddMember("parentEndFileLength",
                  m_meta.parent_meta_cursor.endFileLength, allocator);
    obj.AddMember(
        "childFileName",
        rapidjson::Value(m_meta.child_meta_cursor.fileName.c_str(), allocator)
            .Move(),
        allocator);
    obj.AddMember("childBeginOffset", m_meta.child_meta_cursor.beginOffset,
                  allocator);
    obj.AddMember("childEndFileLength", m_meta.child_meta_cursor.endFileLength,
                  allocator);
  }

  rapidjson::Value arr(rapidjson::kArrayType);
  for (const auto &point : m_meta.time_point_list) {
    rapidjson::Value sub_obj(rapidjson::kObjectType);
    dump_time_point_rec(point.get(), sub_obj, allocator);
    arr.PushBack(sub_obj, allocator);
  }
  obj.AddMember("timePointList", arr, allocator);
}

/**
  Dump one record for time point list.

  @param[in]       point          Single record for time point list.
  @param[out]      obj            Single record object of rapidjson.
  @param[out]      allocator      Allocator of rapidjson.
*/
void Backup_monitor::dump_time_point_rec(
    const local_backup_point *point, rapidjson::Value &obj,
    rapidjson::Document::AllocatorType &allocator) {
  obj.AddMember("timePoint", point->time_point, allocator);
  char print_buff[MAX_DATE_STRING_REP_LENGTH];
  convert_timestamp_to_str(point->time_point, print_buff);
  obj.AddMember("timePointStr",
                rapidjson::Value(
                    std::string(print_buff, MAX_DATE_STRING_REP_LENGTH).c_str(),
                    allocator)
                    .Move(),
                allocator);

  obj.AddMember("lsnPoint", point->lsn_point, allocator);
  if (!opts.is_wal_archive_child_meta) {
    obj.AddMember("startRecoveryLsn", point->start_recovery_lsn, allocator);
  } else {
    obj.AddMember("isDisjoin", ((local_backup_arch_point *)point)->is_disjoin,
                  allocator);
  }
}

/**
  Dump one record for full local backup binlog meta.

  @param[out]      obj            Single record object of rapidjson.
  @param[out]      allocator      Allocator of rapidjson.
*/
void Backup_monitor::dump_full_local_backup_binlog(
    rapidjson::Value &obj, rapidjson::Document::AllocatorType &allocator) {
  obj.AddMember(
      "readFullLocalBackupBinlogMeta",
      rapidjson::Value(
          get_ret_string(m_meta.ret_read_full_local_backup_binlog).c_str(),
          allocator)
          .Move(),
      allocator);

  rapidjson::Value arr(rapidjson::kArrayType);
  for (const auto &point : m_meta.binlog_points) {
    rapidjson::Value sub_obj(rapidjson::kObjectType);
    sub_obj.AddMember(
        "binlogFile",
        rapidjson::Value(point.binlogFile.c_str(), allocator).Move(),
        allocator);
    sub_obj.AddMember("binlogFilePos", point.binlogFilePos, allocator);

    sub_obj.AddMember("gtidSet",
                      rapidjson::Value(point.gtidSet.c_str(), allocator).Move(),
                      allocator);
    sub_obj.AddMember("timePoint", point.lsnPoint.time_point, allocator);
    char print_buff[MAX_DATE_STRING_REP_LENGTH];
    convert_timestamp_to_str(point.lsnPoint.time_point, print_buff);
    sub_obj.AddMember(
        "timePointStr",
        rapidjson::Value(
            std::string(print_buff, MAX_DATE_STRING_REP_LENGTH).c_str(),
            allocator)
            .Move(),
        allocator);

    sub_obj.AddMember("lsnPoint", point.lsnPoint.lsn_point, allocator);
    sub_obj.AddMember("startRecoveryLsn", point.lsnPoint.start_recovery_lsn,
                      allocator);
    arr.PushBack(sub_obj, allocator);
  }

  obj.AddMember("binlogPointList", arr, allocator);
}

/**
  Dump one record for full local backup meta json file crc check.

  @param[out]      obj            Single record object of rapidjson.
  @param[out]      allocator      Allocator of rapidjson.
*/
void Backup_monitor::dump_meta_json_crc(
    rapidjson::Value &obj, rapidjson::Document::AllocatorType &allocator) {
  obj.AddMember(
      "checkResult",
      rapidjson::Value(get_ret_string(m_meta.ret_verify_meta_json_crc).c_str(),
                       allocator)
          .Move(),
      allocator);
  obj.AddMember("storageCrc", m_meta.json_storage_crc, allocator);
  obj.AddMember("calculateCrc", m_meta.json_calc_crc, allocator);
}

/** Dump meta info. */
void Backup_monitor::dump() {
  rapidjson::Document d(rapidjson::kObjectType);
  rapidjson::Document::AllocatorType &allocator = d.GetAllocator();

  if (m_meta.meta_type == MetaType::RESTOREMETA) {
    dump_restore_meta_rec(d, allocator);
  } else if (m_meta.meta_type == MetaType::TIMEPOINTLIST) {
    dump_time_point_list(d, allocator);
  } else if (m_meta.meta_type == MetaType::BINLOGMETA) {
    dump_full_local_backup_binlog(d, allocator);
  } else {
    dump_meta_json_crc(d, allocator);
  }

  rapidjson::StringBuffer buffer;

  if (opts.pretty) {
    rapidjson::PrettyWriter<rapidjson::StringBuffer> pretty_writer(buffer);
    d.Accept(pretty_writer);
  } else {
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
  }

  fprintf(m_out_stream, "%s", buffer.GetString());
  fprintf(m_out_stream, "\n");
  fflush(m_out_stream);
}

int main(int argc, char **argv) {
  /* Buffer to hold temporary file name. */
  char tmp_filename_buf[FN_REFLEN];
  FILE *dump_file;

  ut_crc32_init();
  MY_INIT(argv[0]);
  DBUG_TRACE;
  DBUG_PROCESS(argv[0]);

  if (get_options(&argc, &argv)) {
    return 1;
  }

  if (opts.is_dump_file) {
    char dir_buf[FN_REFLEN];
    size_t dir_length;
    memset(dir_buf, 0, FN_REFLEN);
    if (dirname_part(dir_buf, opts.dump_filename, &dir_length) == 0) {
      sprintf(dir_buf, "./");
    }

    dump_file = create_tmp_file(tmp_filename_buf, dir_buf, "backup_monitor");

    if (dump_file == nullptr) {
      ib::error() << "Invalid Dumpfile passed";
      return 1;
    }
  } else {
    dump_file = stdout;
  }

  std::string meta_path(argv[0]);
  Meta_info meta;
  meta.clear();
  if (opts.is_monitor_timestamp) {
    meta.meta_type = MetaType::RESTOREMETA;
    std::string restore_meta_path(meta_path);
    if (opts.is_restore_meta_path) {
      restore_meta_path = opts.restore_meta_path;
    }
    if (opts.is_wal_file_size &&
        (opts.wal_file_size < DEFAULT_WAL_FILE_SIZE ||
         opts.wal_file_size % DSTORE::WAL_READ_BUFFER_BLOCK_SIZE != 0)) {
      ib::error() << "Wal file size is invalid, it should be greater than 128M "
                  << "and divisible by 512K. wal-file-size: "
                  << opts.wal_file_size;
      return 1;
    }
    meta.ret_create_restore_meta =
        CDE::CreateRestoreMeta(opts.monitor_timestamp, meta_path, meta.point,
                               restore_meta_path, opts.is_restore_meta_path);
    meta.ret_show_restore_file_list = CDE::ShowRestoreFileList(
        meta.point.time_point, restore_meta_path, meta.point, meta.full_dir,
        meta.wal_file_list, opts.is_restore_meta_path);
  } else if (opts.is_binlog) {
    meta.meta_type = MetaType::BINLOGMETA;
    meta.ret_read_full_local_backup_binlog =
        CDE::ReadFullLocalBackupBinlogMeta(meta_path, meta.binlog_points);
  } else if (opts.is_json_crc) {
    meta.meta_type = MetaType::METAJSON;
    meta.ret_verify_meta_json_crc = CDE::VerifyFullBackupMetaJsonCrc(
        meta_path, meta.json_storage_crc, meta.json_calc_crc);
  } else {
    meta.meta_type = MetaType::TIMEPOINTLIST;
    meta.parent_meta_cursor.fileName = "";
    meta.parent_meta_cursor.beginOffset = opts.parent_offset;
    if (!opts.is_wal_archive_child_meta) {
      meta.ret_show_time_point_list =
          CDE::ShowTimePointFromFullBackupMetaOffset(
              meta.time_point_list, meta_path, opts.max_expect_count,
              meta.parent_meta_cursor);
    } else {
      meta.child_meta_cursor.fileName = "";
      meta.child_meta_cursor.beginOffset = opts.child_offset;
      meta.ret_show_time_point_list =
          CDE::ShowTimePointFromWalArchiveMetaOffset(
              meta.time_point_list, meta_path, opts.max_expect_count,
              meta.parent_meta_cursor, meta.child_meta_cursor);
    }
  }

  Backup_monitor monitor(dump_file, meta);
  monitor.dump();

  if (opts.is_dump_file) {
    /* Rename file can fail if the source and destination
    are across partitions. */
    if (my_rename(tmp_filename_buf, opts.dump_filename, MYF(0)) == -1) {
      if (my_copy(tmp_filename_buf, opts.dump_filename, MYF(0)) != 0) {
        ib::error() << "Copy failed: from: " << tmp_filename_buf
                    << " to: " << opts.dump_filename
                    << " because of system error: " << strerror(errno);

        ib::error() << "Please check contents of"
                    << " temporary file " << tmp_filename_buf
                    << " and delete it manually";
        return 1;
      }
      try_delete_temporary_filename(tmp_filename_buf);
    }
  }

  return 0;
}
