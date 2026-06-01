/* Copyright (c) 2025, Huawei and/or its affiliates.

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

#ifndef LOCAL_BACKUP_OBS_DOWNLOAD_UTILS_H
#define LOCAL_BACKUP_OBS_DOWNLOAD_UTILS_H

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "local_backup_file_utils.h"
#include "local_backup_obs_handler.h"
#include "my_io.h"

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/filewritestream.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>

class lb_file_path_creator {
 public:
  static bool create_file_with_full_path(const std::string &file_path);
  static std::string extract_directory(const std::string &file_path);

 private:
  static bool create_dir(const std::string &file_path);
  static bool create_directory(const std::string &file_path);
  static bool directory_exists(const std::string &file_path);
  static bool create_file(const std::string &file_path);
};

struct object_suffix {
  object_suffix()
      : m_type(0), m_version(0), id_list(), m_last_id(0), m_last_offset(0) {}
  ~object_suffix() { id_list.clear(); }
  uint32_t m_type;
  uint32_t m_version;
  std::set<uint32_t> id_list;

  /* The id of last download object which used for resumption of work after a
   * break */
  uint32_t m_last_id;
  /* The offset for the last download object which used for resumption of work
   * after a break */
  uint64_t m_last_offset;
};

struct object_name_group {
  object_name_group(const std::string &prefix_name, object_suffix *suffix)
      : m_prefix_name(prefix_name), m_suffix(suffix) {}
  ~object_name_group() {
    m_prefix_name.clear();
    m_suffix = nullptr;
  }
  std::string m_prefix_name;
  object_suffix *m_suffix;
};

struct file_read_handle {
  lb_object_file_describe describe;
  std::string file_name;
  int fd;
};

/** File object relation. */
struct object_relation {
  std::string file_name;
  std::string object_name;
  uint64_t size;
  uint64_t offset;
};

/** Download status in trace log. */
enum class lb_obs_download_status : size_t { DOWNLOADING = 0, COMPLETED };

/** Download progress in trace log. */
struct lb_obs_download_progress {
  std::string object_name;
  lb_obs_download_status status;
  uint64_t downloaded_length;
  uint64_t object_length;
};

/** Download trace logger. */
class lb_obs_download_trace_logger {
 public:
  /**
    Return the single instance of the trace logger.

    @return a pointer to the lb_obs_download_trace_logger instance.
  */
  static lb_obs_download_trace_logger *get_instance();

  /**
    Check if the trace logger instance has been initialized.

    @return true if the logger is initialized, false otherwise.
  */
  static bool is_initialized();

  /**
    Create the trace logger instance.

    @return false if the instance was created successfully, true otherwise.
  */
  static bool create_instance();

  /**
    Destroy the trace logger instance.
  */
  static void destroy_instance();

  /**
    Initialize the trace logger with the trace log name.

    @param[in]      trace_log_name      The name of the trace log file.

    @return false if initialization was successful, true otherwise.
  */
  bool init(const std::string &trace_log_name);

  /**
    Add a new object to the progress map.

    @param[in]      object_name      The name of the object to add.
  */
  void add_progress(const std::string &object_name);

  /**
    Update the progress of a specific object.

    @param[in]      object_name            The name of the object to update.
    @param[in]      downloaded_length      The length of the object that has
    been downloaded.
    @param[in]      object_length          The total length of the object.
  */
  void update_progress(const std::string &object_name,
                       uint64_t downloaded_length, uint64_t object_length);

  /**
    Check if progress information exists for a specific object.

    @param[in]      object_name      The name of the object to check.

    @return true if progress information exists, false otherwise.
  */
  bool has_progress(const std::string &object_name);

  /**
    Get the progress information for a specific object.

    @param[in]      object_name      The name of the object to get progress for.

    @return the progress information for the object.
  */
  lb_obs_download_progress get_progress(const std::string &object_name);

 private:
  /** Private constructor. */
  lb_obs_download_trace_logger() {}
  /** Private destructor */
  ~lb_obs_download_trace_logger() {}

  /**
    Create the full path for the trace log file.

    @param[in]      full_name      The full path of the trace log file.

    @return false if the file was created successfully, true otherwise.
  */
  bool create_full_path_file(std::string &full_name);

  /**
    Convert progress information to JSON format.

    @param[in]      progress       The progress information to convert.
    @param[in]      allocator      The JSON document allocator.

    @return The JSON value representing the progress information.
  */
  rapidjson::Value progress_to_json(
      const lb_obs_download_progress &progress,
      rapidjson::Document::AllocatorType &allocator) const;

  /**
    Convert JSON format to progress information.

    @param[in]      json_value      The JSON value to convert.

    @return The progress information.
  */
  lb_obs_download_progress json_to_progress(
      const rapidjson::Value &json_value) const;

  /**
    Save the progress map to the trace log file.

    @return false if the save was successful, true otherwise.
  */
  bool save_to_file();

  /**
    Load the progress map from the trace log file.

    @return false if the load was successful, true otherwise.
  */
  bool load_from_file();

  /* Static pointer to the single instance of the trace logger. */
  static lb_obs_download_trace_logger *m_trace_logger_instance;
  /* Mutex to ensure thread-safe access to the instance. */
  static std::mutex m_trace_logger_instance_mtx;

  /* Trace log name which record the last download progress. */
  std::string m_trace_log_name;
  /* Map object name to progress info. */
  std::unordered_map<std::string, lb_obs_download_progress> m_progress_map;
  /* Mutex to ensure thread-safe access to the progress map. */
  std::mutex m_lock;
  /* Atomic boolean to indicate if the trace logger is initialized. */
  std::atomic<bool> m_initialized{false};
};

#endif
