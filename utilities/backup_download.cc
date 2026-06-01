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
#include "my_macros.h"
#include "my_time.h"
#include "print_version.h"
#include "typelib.h"

#include "sql/local_backup/local_backup_file_utils.h"
#include "sql/local_backup/local_backup_obs_download.h"
#include "sql/local_backup/local_backup_obs_handler.h"
#include "sql/scc_util.h"
#include "ut0crc32.h"

bool rds_scc_initialized = false;
uint32_t rds_lb_obs_max_connected_time = 0;

constexpr const char *BACKUP_DOWNLOAD_WELCOME_COPYRIGHT_NOTICE =
    "Copyright (c) 2025, Huawei and/or its affiliates. All Rights Reserved.\n";

/** Global options structure. Option values passed at command line are
stored in this structure */
struct Download_options {
  char *prefix_key;
  char *config_name;
  char *trace_log_name;
  uint32_t worker_num;
  bool is_show_relation;
  bool is_remove;
  const char *dbug_setting;
  bool is_dump_file;
  char *dump_filename;
  bool pretty;
};
struct Download_options opts;

/* Command line argument for backup_download tool. */
static struct my_option backup_download_options[] = {
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
     "Dump the download information into the file passed by user. "
     "Without the filename, it will default to stdout.",
     &opts.dump_filename, &opts.dump_filename, nullptr, GET_STR, REQUIRED_ARG,
     0, 0, 0, nullptr, 0, nullptr},
    {"prefix-key", 'p', "Prefix key of the backup download.", &opts.prefix_key,
     &opts.prefix_key, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"config-name", 'c', "Config name of the backup download.",
     &opts.config_name, &opts.config_name, nullptr, GET_STR, REQUIRED_ARG, 0, 0,
     0, nullptr, 0, nullptr},
    {"trace-log-name", 't', "Trace log name of the backup download.",
     &opts.trace_log_name, &opts.trace_log_name, nullptr, GET_STR, REQUIRED_ARG,
     0, 0, 0, nullptr, 0, nullptr},
    {"worker-num", 'w', "Worker count for the backup download.",
     &opts.worker_num, &opts.worker_num, nullptr, GET_UINT, REQUIRED_ARG, 1, 1,
     UINT_MAX, nullptr, 0, nullptr},
    {"show", 's', "Show file object relation.", &opts.is_show_relation,
     &opts.is_show_relation, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"remove", 'r', "Remove objects from obs.", &opts.is_remove,
     &opts.is_remove, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"pretty", 'P',
     "Pretty format the objects relation information output. "
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
          "backup_download: Assertion failure in file %s line " UINT64PF "\n",
          file, line);

  if (expr != nullptr) {
    fprintf(stderr, "backup_download: Failing assertion: %s\n", expr);
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

  DBUG_EXECUTE_IF("bd_tmp_file_fail", file = nullptr; errno = EACCES;);

  if (file == nullptr) {
    ib::error() << "Unable to create temporary file. err: " << strerror(errno);

    if (fd >= 0) {
      try_delete_temporary_filename(temp_file_buf);
      my_close(fd, MYF(0));
    }
  }

  return (file);
}

/** Print the backup_download tool usage. */
static void usage() {
#ifdef NDEBUG
  print_version();
#else
  print_version_debug();
#endif /* NDEBUG */
  puts(BACKUP_DOWNLOAD_WELCOME_COPYRIGHT_NOTICE);
  printf(
      "Usage: %s [-v] [-d <dump file name>] [-p <prefix key>] [-c <config "
      "name>] [-t <trace log name>] [-w <worker num>] [-s] [-r]\n",
      my_progname);
  my_print_help(backup_download_options);
  my_print_variables(backup_download_options);
}

/** Parse the options passed to tool. */
extern "C" bool backup_download_get_one_option(int optid,
                                               const struct my_option *opt
                                               [[maybe_unused]],
                                               char *argument
                                               [[maybe_unused]]) {
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
    case 'p':
    case 'c':
    case 't':
    case 'w':
    case 's':
    case 'r':
    case 'P':
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
  /* Return usage() if no arg. */
  if (*argc == 1) {
    usage();
    return (false);
  }

  if (handle_options(argc, argv, backup_download_options,
                     backup_download_get_one_option)) {
    exit(true);
  }

  return (false);
}

/** Error logging classes. */
namespace ib {

logger::~logger() = default;

info::~info() {
  std::cerr << "[INFO] backup_download: " << m_oss.str() << "." << std::endl;
}

warn::~warn() {
  std::cerr << "[WARNING] backup_download: " << m_oss.str() << "." << std::endl;
}

error::~error() {
  std::cerr << "[ERROR] backup_download: " << m_oss.str() << "." << std::endl;
}

/*
MSVS complains: Warning C4722: destructor never returns, potential memory leak.
But, the whole point of using ib::fatal temporary object is to cause an abort.
*/
MY_COMPILER_DIAGNOSTIC_PUSH()
MY_COMPILER_MSVC_DIAGNOSTIC_IGNORE(4722)

fatal::~fatal() {
  std::cerr << "[FATAL] backup_download: " << m_oss.str() << "." << std::endl;
  ut_error;
}

// Restore the MSVS checks for Warning C4722, silenced for ib::fatal::~fatal().
MY_COMPILER_DIAGNOSTIC_POP()

class dbug : public logger {
 public:
  ~dbug() override {
    DBUG_PRINT("backup_download", ("%s", m_oss.str().c_str()));
  }
};
}  // namespace ib

/**
  Read config.

  @param[in]       config_name      Config file path.
  @param[out]      scc_conf         SCC config file path.
  @param[out]      obs_para         OBS parameters.

  @return false if read config successfully, true otherwise.
*/
bool read_config(std::string &config_name, std::string &scc_conf,
                 std::map<std::string, std::string> &obs_para) {
  std::ifstream ifs(config_name, std::ios::in | std::ios::binary);
  if (!ifs.is_open()) {
    ib::error() << "Failed to open config file";
    return true;
  }

  rapidjson::IStreamWrapper isw(ifs);
  rapidjson::Document doc;
  doc.ParseStream(isw);

  if (doc.HasParseError()) {
    ib::error() << "Failed to parse config file content";
    return true;
  }

  scc_conf.clear();
  if (doc.HasMember("scc_conf") && doc["scc_conf"].IsString()) {
    scc_conf = doc["scc_conf"].GetString();
  }
  std::string obs_url = doc["obs_url"].GetString();
  std::string ak = doc["ak"].GetString();
  std::string sk = doc["sk"].GetString();
  std::string bucket_name = doc["bucket_name"].GetString();
  std::string uri_style = "0";
  if (doc.HasMember("uri_style") && doc["uri_style"].IsUint()) {
    uri_style = doc["uri_style"].GetUint() == 1 ? "1" : "0";
  }
  if (doc.HasMember("max_connected_time") &&
      doc["max_connected_time"].IsUint()) {
    rds_lb_obs_max_connected_time = doc["max_connected_time"].GetUint();
  }
  if (doc.HasMember("sleep_interval") && doc["sleep_interval"].IsUint()) {
    rds_local_backup_sleep_interval = doc["sleep_interval"].GetUint();
  }
  if (doc.HasMember("sleep_time_ms") && doc["sleep_time_ms"].IsUint()) {
    rds_local_backup_sleep_time_ms = doc["sleep_time_ms"].GetUint();
  }
  std::string scc_decrypt = "false";
  if (doc.HasMember("scc_decrypt") && doc["scc_decrypt"].IsBool()) {
    scc_decrypt = doc["scc_decrypt"].GetBool() ? "true" : "false";
  }
  obs_para = {{"obs_url", obs_url},
              {"AK", ak},
              {"SK", sk},
              {"bucket_name", bucket_name},
              {"uri_style", uri_style},
              {"scc_decrypt", scc_decrypt},
              {"encryption", "0"},
              {"kms_key", ""}};

  return false;
}

/**
  Remove obs objects.

  @param[in]      prefix_key      Prefix key of the obs objects.
  @param[in]      obs_para        OBS parameters.

  @return false if remove successfully, true otherwise.
*/
bool remove_obs_objects(std::string &prefix_key,
                        std::map<std::string, std::string> &obs_para) {
  if (create_lb_object_handler_instance(true, 1024 * 1024, obs_para)) {
    ib::error() << "Failed to create OBS service handler instance";
    return true;
  }
  lb_object_handler *handler = fetch_lb_object_handler();
  if (handler == nullptr) {
    ib::error() << "Failed to fetch OBS object handler";
    return true;
  }
  std::vector<std::string> object_list;
  if (handler->list_object_list(prefix_key, object_list)) {
    ib::error() << "Failed to list OBS object list";
    return true;
  }
  for (uint i = 0; i < object_list.size(); i++) {
    if (handler->remove_object(&object_list[i])) {
      ib::error() << "Failed to remove object: " << object_list[i];
      return true;
    }
  }
  object_list.clear();
  come_back_lb_object_handler(handler);
  destroy_lb_object_handler_instance();

  return false;
}

void dump_objects_relation(std::vector<object_relation> &relation_list,
                           FILE *dump_file) {
  rapidjson::Document document(rapidjson::kObjectType);
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();

  rapidjson::Value arr(rapidjson::kArrayType);
  for (const auto &relation : relation_list) {
    rapidjson::Value sub_obj(rapidjson::kObjectType);
    sub_obj.AddMember(
        "file_name",
        rapidjson::Value(relation.file_name.c_str(), allocator).Move(),
        allocator);
    sub_obj.AddMember(
        "object_name",
        rapidjson::Value(relation.object_name.c_str(), allocator).Move(),
        allocator);
    sub_obj.AddMember("size", relation.size, allocator);
    sub_obj.AddMember("offset", relation.offset, allocator);
    arr.PushBack(sub_obj, allocator);
  }
  document.AddMember("relation_list", arr, allocator);

  rapidjson::StringBuffer buffer;

  if (opts.pretty) {
    rapidjson::PrettyWriter<rapidjson::StringBuffer> pretty_writer(buffer);
    document.Accept(pretty_writer);
  } else {
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
  }

  fprintf(dump_file, "%s", buffer.GetString());
  fprintf(dump_file, "\n");
  fflush(dump_file);
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

    dump_file = create_tmp_file(tmp_filename_buf, dir_buf, "backup_download");

    if (dump_file == nullptr) {
      ib::error() << "Invalid Dumpfile passed";
      return 1;
    }
  } else {
    dump_file = stdout;
  }

  /* Read config. */
  if (opts.prefix_key == nullptr || opts.config_name == nullptr) {
    usage();
    return 0;
  }
  std::string prefix_key(opts.prefix_key);
  std::string config_name(opts.config_name);
  std::string scc_conf;
  std::map<std::string, std::string> obs_para;
  if (read_config(config_name, scc_conf, obs_para)) {
    return 1;
  }

  /* Initialize scc if scc is enabled. */
  if (!scc_conf.empty()) {
    if ((scc_init_with_conf_retry(const_cast<char *>(scc_conf.c_str()),
                                  SCC_INIT_MAX_RETRY))) {
      ib::error() << "scc init fail";
      rds_scc_initialized = false;
      return 1;
    } else {
      rds_scc_initialized = true;
    }
  }

  bool error = false;
  if (opts.is_remove) {
    /* Remove objects. */
    error = remove_obs_objects(prefix_key, obs_para);
    if (!error) {
      ib::info() << "remove complete";
    }
  } else if (opts.is_show_relation) {
    /* Show object relation. */
    std::vector<object_relation> object_relation_list;
    error = show_lb_obs_objects_relation(prefix_key, config_name,
                                         &object_relation_list);
    if (error) {
      ib::error() << "show objects relation fail";
    } else {
      dump_objects_relation(object_relation_list, dump_file);
    }
  } else {
    /* Download from obs. */
    std::string trace_log_name;
    if (opts.trace_log_name != nullptr) {
      trace_log_name = opts.trace_log_name;
    }
    error = start_lb_obs_objects_download(prefix_key, config_name,
                                          trace_log_name, opts.worker_num);
    if (error) {
      ib::error() << "download fail";
    } else {
      ib::info() << "download complete";
    }
  }

  /* Finalize scc if scc is initialized. */
  if (rds_scc_initialized) {
    scc_finalize();
    rds_scc_initialized = false;
  }
  scc_close_lib();

  /* Return if operation is failed. */
  if (error) {
    return 1;
  }

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
