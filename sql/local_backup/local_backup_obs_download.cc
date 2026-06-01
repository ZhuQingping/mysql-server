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

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <thread>
#include <unordered_map>

#include "local_backup_obs_download.h"
#include "my_dbug.h"

/* Size interval of local backup obs download progress update. Value is 200MB.
 */
uint64_t lb_obs_download_progress_size_interval = 209715200;

class lb_obs_download_worker {
 public:
  lb_obs_download_worker()
      : m_handler(nullptr),
        m_action(nullptr),
        m_name_group(nullptr),
        m_lock(),
        m_condition(),
        m_base_path(),
        m_no_delimiter(0),
        m_trace_logger(nullptr),
        m_object_relation_list(nullptr) {}
  ~lb_obs_download_worker() {
    destroy();
    m_handler = nullptr;
    m_trace_logger = nullptr;
    m_object_relation_list = nullptr;
  }
  bool init(lb_object_handler *handler,
            lb_obs_download_trace_logger *trace_logger,
            std::string &file_base_path, uint32_t no_delimiter,
            std::vector<object_relation> *object_relation_list);
  void destroy();
  void push_object_name(object_name_group *group);
  bool is_busy() {
    return ((nullptr != get_current_name_group()) ? true : false);
  }
  bool is_leave() { return m_leave; }

  lb_object_handler *m_handler;

 private:
  bool create_thread();
  void thread_main();
  object_name_group *get_current_name_group();
  void clear_current_name_group();
  void wait() {
    std::unique_lock<std::mutex> lock(m_lock);
    while (nullptr == m_name_group && !m_shutdown.load()) {
      const uint64_t timeoutMs = 1000; /* 1S */
      (void)m_condition.wait_for(lock, std::chrono::milliseconds(timeoutMs));
    }
  }
  bool process_object_group(bool &no_action);
  bool process_as_multi_file();
  bool process_as_single_file();
  bool find_subpath_begin_pos(uint32_t &pos);
  bool create_full_path_file(std::string &full_name);
  std::string extract_directory(std::string &full_name);
  bool check_object_downloaded_as_multi_file(
      std::vector<file_read_handle> file_read_handler_list,
      uint64_t &start_offset, uint32_t &file_no, uint64_t &file_write_offset,
      uint64_t &last_saved_length);
  void update_trace_log_as_multi_file(int fd, uint64_t file_write_offset,
                                      uint64_t file_length,
                                      uint64_t downloaded_length,
                                      uint64_t object_length,
                                      uint64_t &last_saved_length);
  bool dump_one_object_to_file(int fd, uint64_t &start_offset);
  bool check_object_downloaded_as_single_file(uint64_t &start_offset,
                                              uint64_t &current_obj_offset);
  void update_trace_log_as_single_file(int fd, uint64_t current_obj_offset,
                                       uint64_t object_length,
                                       uint64_t &last_saved_length);
  void shutdown();

  std::thread *m_action;
  object_name_group *m_name_group;
  std::mutex m_lock;
  std::condition_variable m_condition;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_shutdown{false};
  std::atomic<bool> m_leave{false};

  /* the target base path in block storage */
  std::string m_base_path;
  /* after which one delimiter of object name, the sub string is needed
     to be assembled into a complete file path with m_base_path */
  uint32_t m_no_delimiter;
  lb_obs_download_trace_logger *m_trace_logger;
  std::vector<object_relation> *m_object_relation_list;
};

object_name_group *lb_obs_download_worker::get_current_name_group() {
  std::unique_lock<std::mutex> lock(m_lock);
  object_name_group *group = m_name_group;
  return group;
}

void lb_obs_download_worker::clear_current_name_group() {
  std::unique_lock<std::mutex> lock(m_lock);
  delete m_name_group;
  m_name_group = nullptr;
}

bool lb_obs_download_worker::find_subpath_begin_pos(uint32_t &pos) {
  uint32_t total = m_no_delimiter;
  size_t last_pos = 0;
  size_t current_pos = m_name_group->m_prefix_name.find(FN_LIBCHAR);
  if (current_pos == std::string::npos) {
    return true;
  }

  total--;
  while (total > 0) {
    last_pos = current_pos;
    current_pos = m_name_group->m_prefix_name.find(FN_LIBCHAR, last_pos + 1);
    if (current_pos == std::string::npos) {
      return true;
    }
    total--;
  }
  pos = current_pos;
  return false;
}

bool lb_obs_download_worker::create_full_path_file(std::string &full_name) {
  return lb_file_path_creator::create_file_with_full_path(full_name);
}

std::string lb_obs_download_worker::extract_directory(std::string &full_name) {
  return lb_file_path_creator::extract_directory(full_name);
}

/**
  Check if the object was downloaded already as multi file.

  @param[in]          file_read_handler_list      File read handler list.
  @param[in,out]      start_offset                Object start offset.
  @param[in,out]      file_no                     File number in
  file_read_handler_list.
  @param[out]         file_write_offset           File write offset.
  @param[out]         last_saved_length           Last time saved length.

  @return true if current object was completely downloaded already, false
  otherwise.
*/
bool lb_obs_download_worker::check_object_downloaded_as_multi_file(
    std::vector<file_read_handle> file_read_handler_list,
    uint64_t &start_offset, uint32_t &file_no, uint64_t &file_write_offset,
    uint64_t &last_saved_length) {
  /* Skip the check if this is not the beginning of the object. */
  if (file_no != 0 && start_offset != 0) {
    return false;
  }

  /* Return directly if the object was not downloaded before. */
  std::string object_name = m_handler->get_name();
  if (!m_trace_logger->has_progress(object_name)) {
    m_trace_logger->add_progress(object_name);
    return false;
  }

  /*
    Return if the start offset is bigger than the downloaded length recorded in
    trace log.
  */
  lb_obs_download_progress progress = m_trace_logger->get_progress(object_name);
  if (start_offset >= progress.downloaded_length) {
    return false;
  }

  /* Update offset/file_no... if the object was downloaded already. */
  while (start_offset < progress.downloaded_length) {
    file_read_handle *handle_ptr = &(file_read_handler_list[file_no]);
    start_offset +=
        (handle_ptr->describe.file_content_length - file_write_offset);
    if (start_offset <= progress.downloaded_length) {
      file_no++;
      file_write_offset = 0;
    } else {
      file_write_offset = (handle_ptr->describe.file_content_length -
                           (start_offset - progress.downloaded_length));
      last_saved_length = file_write_offset;
    }
  }
  start_offset = progress.downloaded_length;
  if (progress.status == lb_obs_download_status::COMPLETED) {
    return true;
  }
  return false;
}

/**
  Fsync the file and update the progress into the trace log as multi file.

  @param[in]          fd                     File descriptor
  @param[in]          file_write_offset      File write offset.
  @param[in]          file_length            File length.
  @param[in]          downloaded_length      Downloaded length.
  @param[in]          object_length          Object length.
  @param[in,out]      last_saved_length      Last time saved length.
*/
void lb_obs_download_worker::update_trace_log_as_multi_file(
    int fd, uint64_t file_write_offset, uint64_t file_length,
    uint64_t downloaded_length, uint64_t object_length,
    uint64_t &last_saved_length) {
  if (file_write_offset - last_saved_length >
          lb_obs_download_progress_size_interval ||
      file_write_offset == file_length) {
    CDE::LBFsync(fd);
    m_trace_logger->update_progress(m_handler->get_name(), downloaded_length,
                                    object_length);
    last_saved_length = file_write_offset;
  }
}

bool lb_obs_download_worker::process_as_multi_file() {
  uint64_t real_length = 0;
  uint64_t object_length = 0;
  uint64_t start_offset = 0;
  uint64_t end_offset = 0;
  uint64_t current_end_offset = 0;
  uint64_t current_read_length = 0;
  std::set<uint32_t> &id_set = m_name_group->m_suffix->id_list;

  /* Eliminate the obs backup path and put together
   * with target database base path
   */
  uint32_t pos = 0;
  if (find_subpath_begin_pos(pos)) {
    return true;
  }

  /* lb_object_file_group_header and all lb_object_file_describe must in first
   * object and the length is smaller than m_handler->m_buffer_len
   */
  auto iter = id_set.begin();

  if (m_name_group->m_suffix->m_type ==
      OBS_OBJ_TYPE_MERGED_FILES_WITH_SEP_META) {
    /* Handle meta object first for OBS_OBJ_TYPE_MERGED_FILES_WITH_SEP_META */
    m_handler->make_object_name(m_name_group->m_prefix_name,
                                OBS_OBJ_TYPE_MERGED_FILES_META, 0, 0);
  } else {
    m_handler->make_object_name(m_name_group->m_prefix_name,
                                m_name_group->m_suffix->m_type,
                                m_name_group->m_suffix->m_version, *iter);
  }
  if (m_handler->get_object_length(object_length)) {
    return true;
  }

  /* 1. read lb_object_file_group_header from first object */
  current_read_length = sizeof(lb_object_file_group_header);
  if (m_handler->get_object_stream(start_offset, current_read_length,
                                   real_length)) {
    return true;
  }
  if (current_read_length != real_length) {
    return true;
  }
  uint32_t file_count =
      ((lb_object_file_group_header *)(m_handler->m_buffer))->file_count;
  DBUG_EXECUTE_IF("bd_worker_mock_obs", file_count = 1;);

  /* 2. read all lb_object_file_describe based on the file count of
   * lb_object_file_group_header and push back read handle including length of
   * file name and length of file content
   */
  start_offset += sizeof(lb_object_file_group_header);
  end_offset = start_offset + file_count * sizeof(lb_object_file_describe);
  uint64_t all_file_name_length = 0;
  std::vector<file_read_handle> file_read_handler_list;
  while (start_offset < end_offset) {
    current_read_length =
        std::min(end_offset - start_offset, m_handler->m_buffer_len);
    current_end_offset = start_offset + current_read_length -
                         current_read_length % sizeof(lb_object_file_describe);
    if (m_handler->get_object_stream(start_offset, current_read_length,
                                     real_length)) {
      return true;
    }
    if (current_read_length != real_length) {
      return true;
    }
    uint8_t *buffer = m_handler->m_buffer;
    while (start_offset < current_end_offset) {
      lb_object_file_describe *describe = (lb_object_file_describe *)buffer;
      DBUG_EXECUTE_IF("bd_worker_mock_obs", {
        describe->file_path_length =
            10 * id_set.size() + m_name_group->m_suffix->m_type;
        describe->file_content_length =
            (m_name_group->m_suffix->m_type ==
             OBS_OBJ_TYPE_MERGED_FILES_WITH_SEP_META)
                ? (object_length * id_set.size())
                : (object_length * id_set.size() - end_offset -
                   describe->file_path_length);
      });
      file_read_handle handle;
      handle.describe = *describe;
      handle.file_name =
          extract_directory(m_name_group->m_prefix_name) + FN_LIBCHAR;
      handle.fd = -1;
      file_read_handler_list.push_back(handle);
      all_file_name_length += describe->file_path_length;
      buffer += sizeof(lb_object_file_describe);
      start_offset += sizeof(lb_object_file_describe);
    }
  }
  if (start_offset != end_offset) {
    return true;
  }

  /* 3. read all file name and supplement these into file read handler list */
  uint32_t file_no = 0;
  end_offset = start_offset + all_file_name_length;
  while (start_offset < end_offset) {
    current_read_length =
        std::min(end_offset - start_offset, m_handler->m_buffer_len);
    current_end_offset = start_offset + current_read_length;
    if (m_handler->get_object_stream(start_offset, current_read_length,
                                     real_length)) {
      return true;
    }
    if (current_read_length != real_length) {
      return true;
    }
    uint8_t *buffer = m_handler->m_buffer;
    while (start_offset < current_end_offset) {
      file_read_handle *handle_ptr = &(file_read_handler_list[file_no]);
      if (start_offset + handle_ptr->describe.file_path_length >
          current_end_offset) {
        break;
      }
      handle_ptr->file_name.append((char *)buffer,
                                   handle_ptr->describe.file_path_length);
      buffer += handle_ptr->describe.file_path_length;
      start_offset += handle_ptr->describe.file_path_length;
      file_no++;
    }
  }
  if (start_offset != end_offset) {
    return true;
  }

  /* 4. Read file content from objects and dump to multi files.
   * The first object maybe including some file content
   */
  file_no = 0;

  if (m_name_group->m_suffix->m_type ==
      OBS_OBJ_TYPE_MERGED_FILES_WITH_SEP_META) {
    /* Start to handle data object for
    OBS_OBJ_TYPE_MERGED_FILES_WITH_SEP_META */
    m_handler->make_object_name(m_name_group->m_prefix_name,
                                m_name_group->m_suffix->m_type,
                                m_name_group->m_suffix->m_version, *iter);
    start_offset = 0;
    if (m_handler->get_object_length(object_length)) {
      return true;
    }
  }

  /* In show object relation function. */
  std::string object_name = m_handler->get_name();
  if (m_object_relation_list != nullptr) {
    while (iter != id_set.end()) {
      file_read_handle *handle_ptr = &(file_read_handler_list[file_no]);
      std::string sub_path = handle_ptr->file_name.substr(pos + 1);
      object_relation relation;
      relation.file_name = sub_path;
      relation.size = handle_ptr->describe.file_content_length;
      relation.offset = start_offset;
      start_offset += handle_ptr->describe.file_content_length;

      while (start_offset >= object_length) {
        iter++;
        if (iter == id_set.end()) {
          break;
        }
        start_offset -= object_length;
        m_handler->make_object_name(m_name_group->m_prefix_name,
                                    m_name_group->m_suffix->m_type,
                                    m_name_group->m_suffix->m_version, *iter);
        object_name += (start_offset > 0) ? (", " + m_handler->get_name()) : "";
        if (m_handler->get_object_length(object_length)) {
          return true;
        }
      }

      relation.object_name = object_name;
      m_object_relation_list->push_back(relation);
      object_name = m_handler->get_name();
      file_no++;
    }

    return false;
  }

  uint64_t file_write_offset = 0;
  uint64_t last_saved_length = 0;
  while (iter != id_set.end()) {
    /* last object have read finish, need advance next object to read content
     * and dump to files */
    if (start_offset == object_length) {
      iter++;
      if (iter == id_set.end()) {
        break;
      }
      start_offset = 0;
      m_handler->make_object_name(m_name_group->m_prefix_name,
                                  m_name_group->m_suffix->m_type,
                                  m_name_group->m_suffix->m_version, *iter);
      if (m_handler->get_object_length(object_length)) {
        return true;
      }
    }

    /* Check if the object was downloaded already. */
    if (check_object_downloaded_as_multi_file(
            file_read_handler_list, start_offset, file_no, file_write_offset,
            last_saved_length)) {
      continue;
    }

    /* read next region of remain content from current object */
    current_read_length =
        std::min((object_length - start_offset), m_handler->m_buffer_len);
    if (m_handler->get_object_stream(start_offset, current_read_length,
                                     real_length)) {
      return true;
    }
    if (current_read_length != real_length) {
      return true;
    }
    /* use up buffer of last read reading and dump to files */
    m_handler->m_buffer_offset = 0;
    uint8_t *buffer = m_handler->m_buffer;
    while (
        m_handler->m_buffer_offset < current_read_length ||
        (m_handler->m_buffer_offset == current_read_length &&
         file_no < file_read_handler_list.size() &&
         file_read_handler_list[file_no].describe.file_content_length == 0)) {
      file_read_handle *handle_ptr = &(file_read_handler_list[file_no]);
      std::string sub_path = handle_ptr->file_name.substr(pos);
      std::string full_file_path = m_base_path + sub_path;
      if (-1 == handle_ptr->fd) {
        if (create_full_path_file(full_file_path)) {
          return true;
        }
        handle_ptr->fd = CDE::LBOpen(full_file_path.c_str(), O_RDWR);
        if (handle_ptr->fd < 0) {
          return true;
        }
      }

      uint64_t current_file_write_length = std::min(
          (current_read_length - m_handler->m_buffer_offset),
          (handle_ptr->describe.file_content_length - file_write_offset));
      if (current_file_write_length > 0) {
        if (0 > CDE::LBPwrite(handle_ptr->fd, buffer, current_file_write_length,
                              file_write_offset)) {
          return true;
        }
        file_write_offset += current_file_write_length;
        m_handler->m_buffer_offset += current_file_write_length;
        buffer += current_file_write_length;

        /* Fsync the file and update the progress into the trace log. */
        uint64_t downloaded_length = start_offset + m_handler->m_buffer_offset;
        update_trace_log_as_multi_file(handle_ptr->fd, file_write_offset,
                                       handle_ptr->describe.file_content_length,
                                       downloaded_length, object_length,
                                       last_saved_length);
      }
      /* current file have been written finish and need advance next file */
      if (file_write_offset == handle_ptr->describe.file_content_length) {
        CDE::LBClose(handle_ptr->fd);
        /* Remove empty directory hidden file. */
        std::string empty_dir_suffix("/.empty_dir");
        if (full_file_path.size() > empty_dir_suffix.size() &&
            full_file_path.compare(
                full_file_path.size() - empty_dir_suffix.size(),
                empty_dir_suffix.length(), empty_dir_suffix) == 0) {
          remove(full_file_path.c_str());
        }
        file_no++;
        file_write_offset = 0;
        last_saved_length = 0;
      }
    }
    start_offset += real_length;
  }

  return false;
}

/**
  Check if the object was downloaded already as single file.

  @param[out]      start_offset            File write start offset.
  @param[out]      current_obj_offset      Current object offset.

  @return true if current object was completely downloaded already, false
  otherwise.
*/
bool lb_obs_download_worker::check_object_downloaded_as_single_file(
    uint64_t &start_offset, uint64_t &current_obj_offset) {
  /* Return directly if the object was not downloaded before. */
  std::string object_name = m_handler->get_name();
  if (!m_trace_logger->has_progress(object_name)) {
    m_trace_logger->add_progress(object_name);
    return false;
  }

  /* Update offset if the object was downloaded already. */
  lb_obs_download_progress progress = m_trace_logger->get_progress(object_name);
  start_offset += progress.downloaded_length;
  if (progress.status == lb_obs_download_status::COMPLETED) {
    return true;
  }
  current_obj_offset = progress.downloaded_length;
  return false;
}

/**
  Fsync the file and update the progress into the trace log as single file.

  @param[in]          fd                      File descriptor
  @param[in]          current_obj_offset      Current object offset.
  @param[in]          object_length           Object length.
  @param[in,out]      last_saved_length       Last time saved length.
*/
void lb_obs_download_worker::update_trace_log_as_single_file(
    int fd, uint64_t current_obj_offset, uint64_t object_length,
    uint64_t &last_saved_length) {
  if (current_obj_offset - last_saved_length >
          lb_obs_download_progress_size_interval ||
      current_obj_offset == object_length) {
    CDE::LBFsync(fd);
    m_trace_logger->update_progress(m_handler->get_name(), current_obj_offset,
                                    object_length);
    last_saved_length = current_obj_offset;
  }
}

bool lb_obs_download_worker::dump_one_object_to_file(int fd,
                                                     uint64_t &start_offset) {
  uint64_t real_length = 0;
  uint64_t object_length = 0;
  uint64_t current_obj_offset = 0;

  if (m_handler->get_object_length(object_length)) {
    return true;
  }

  /* In show object relation function. */
  if (m_object_relation_list != nullptr) {
    start_offset += object_length;
    return false;
  }

  /* Check if the object was downloaded already. */
  if (check_object_downloaded_as_single_file(start_offset,
                                             current_obj_offset)) {
    return false;
  }

  uint64_t last_saved_length = current_obj_offset;
  while (current_obj_offset < object_length) {
    uint64_t current_read_length =
        std::min(m_handler->m_buffer_len, object_length - current_obj_offset);
    if (m_handler->get_object_stream(current_obj_offset, current_read_length,
                                     real_length)) {
      return true;
    }
    if (current_read_length != real_length) {
      return true;
    }
    if (real_length > 0) {
      if (0 >
          CDE::LBPwrite(fd, m_handler->m_buffer, real_length, start_offset)) {
        return true;
      }
      start_offset += real_length;
      current_obj_offset += real_length;

      /* Fsync the file and update the progress into the trace log. */
      update_trace_log_as_single_file(fd, current_obj_offset, object_length,
                                      last_saved_length);
    }
  }
  return false;
}

bool lb_obs_download_worker::process_as_single_file() {
  /* Eliminate the obs backup path and put together
   * with target database base path
   */
  uint32_t pos = 0;
  if (find_subpath_begin_pos(pos)) {
    return true;
  }
  std::string sub_path = m_name_group->m_prefix_name.substr(pos);
  std::string full_file_path = m_base_path + sub_path;

  int fd = -1;
  if (m_object_relation_list == nullptr) {
    if (create_full_path_file(full_file_path)) {
      return true;
    }
    fd = CDE::LBOpen(full_file_path.c_str(), O_RDWR);
    if (fd < 0) {
      return true;
    }
  }

  std::set<uint32_t> &id_set = m_name_group->m_suffix->id_list;
  auto iter = id_set.begin();
  uint64_t write_offset = 0;
  std::string object_name;
  while (iter != id_set.end()) {
    m_handler->clear();
    m_handler->make_object_name(m_name_group->m_prefix_name,
                                m_name_group->m_suffix->m_type,
                                m_name_group->m_suffix->m_version, *iter);
    object_name += (object_name.empty() ? "" : ", ") + m_handler->get_name();
    if (dump_one_object_to_file(fd, write_offset)) {
      if (fd != -1) {
        CDE::LBClose(fd);
      }
      return true;
    }
    iter++;
  }
  if (fd != -1) {
    CDE::LBClose(fd);
  }

  /* In show object relation function. */
  if (m_object_relation_list != nullptr) {
    object_relation relation;
    relation.file_name = sub_path.substr(1);
    relation.object_name = object_name;
    relation.size = write_offset;
    relation.offset = 0;
    m_object_relation_list->push_back(relation);
  }

  return false;
}

bool lb_obs_download_worker::process_object_group(bool &no_action) {
  bool is_error = false;
  no_action = false;
  object_name_group *name_group = get_current_name_group();
  if (nullptr == name_group) {
    no_action = true;
    return is_error;
  }
  if (OBS_OBJ_TYPE_ONE_FILE == name_group->m_suffix->m_type) {
    is_error = process_as_single_file();
  } else if (OBS_OBJ_TYPE_MERGED_FILES_META != name_group->m_suffix->m_type) {
    /* Skip seperated meta object. Will be handled in process_as_multi_file() */
    is_error = process_as_multi_file();
  }
  return is_error;
}

void lb_obs_download_worker::thread_main() {
  m_running.store(true);
  while (true) {
    /* Only leave when m_shutdown is set and m_name_group is nullptr. */
    {
      std::unique_lock<std::mutex> lock(m_lock);
      if (m_shutdown.load() && m_name_group == nullptr) {
        break;
      }
    }

    bool no_action = false;
    if (process_object_group(no_action)) {
      break;
    }
    clear_current_name_group();
    if (no_action) {
      wait();
    }
  }
  m_leave.store(true);
}

bool lb_obs_download_worker::create_thread() {
  m_action = new (std::nothrow)
      std::thread(&lb_obs_download_worker::thread_main, this);
  if (nullptr == m_action) {
    return true;
  }
  while (!m_running) {
    std::this_thread::sleep_for(std::chrono::microseconds(1000));
  }
  return false;
}

bool lb_obs_download_worker::init(
    lb_object_handler *handler, lb_obs_download_trace_logger *trace_logger,
    std::string &file_base_path, uint32_t no_delimiter,
    std::vector<object_relation> *object_relation_list) {
  bool mock_obs = 0;
  DBUG_EXECUTE_IF("bd_worker_mock_obs", mock_obs = 1;);
  if (!mock_obs && create_thread()) {
    return true;
  }
  m_handler = handler;
  m_trace_logger = trace_logger;
  m_base_path = file_base_path;
  m_no_delimiter = no_delimiter;
  m_object_relation_list = object_relation_list;
#ifndef NDEBUG
  if (mock_obs) {
    bool no_action = false;
    std::string prefix_name =
        "taurus/dstore/bak/test/mtr/full_backup/object-name";
    object_suffix suffix;
    suffix.id_list.insert(0);
    object_name_group *name_group =
        new (std::nothrow) object_name_group(prefix_name, &suffix);
    push_object_name(name_group);
    process_object_group(no_action);
    suffix.m_type = 1;
    process_object_group(no_action);
    suffix.id_list.insert(1);
    process_object_group(no_action);
    suffix.m_type = 2;
    process_object_group(no_action);
    clear_current_name_group();
  }
#endif
  return false;
}

void lb_obs_download_worker::shutdown() {
  std::unique_lock<std::mutex> lock(m_lock);
  m_shutdown.store(true);
  m_condition.notify_all();
}

void lb_obs_download_worker::destroy() {
  if (nullptr != m_action) {
    shutdown();
    m_action->join();
    delete m_action;
    m_action = nullptr;
  }
  if (m_name_group != nullptr) {
    delete m_name_group;
    m_name_group = nullptr;
  }
}

void lb_obs_download_worker::push_object_name(object_name_group *group) {
  std::unique_lock<std::mutex> lock(m_lock);
  m_name_group = group;
  m_condition.notify_all();
}

class lb_obs_download_mgr {
 public:
  lb_obs_download_mgr(
      std::string &prefix_key, std::string &config_name,
      std::string &trace_log_name,
      std::vector<object_relation> *object_relation_list = nullptr)
      : m_handler_mgr(nullptr),
        m_prefix_key(prefix_key),
        m_config_name(config_name),
        m_trace_log_name(trace_log_name),
        m_obs_mode(true),
        m_buffer_len(16 * 1024 * 1024),
        m_obs_cfg_map(),
        m_base_path(),
        m_no_delimiter(0),
        m_object_name_map(),
        m_worker_pool(),
        m_trace_logger(nullptr),
        m_object_relation_list(object_relation_list) {}
  ~lb_obs_download_mgr() {
    destroy_handler_mgr();
    m_trace_logger = nullptr;
    m_object_relation_list = nullptr;
  }
  bool init(uint32_t worker_num);
  bool start();
  void destroy();

  lb_object_handler_mgr *m_handler_mgr;

 private:
  bool read_config();
  bool get_all_name_list();
  bool dispatch_action();
  lb_obs_download_worker *select_worker();
  void wait_timeout() {
    const uint64_t timeout_ms = 100; /* 100ms */
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
  }
  void clear_obs_cfg_and_param() {
    m_prefix_key.clear();
    m_config_name.clear();
    m_trace_log_name.clear();
    m_obs_cfg_map.clear();
  }
  void clear_object_name_map() {
    auto iter = m_object_name_map.begin();
    while (iter != m_object_name_map.end()) {
      object_suffix *suffix = iter->second;
      delete suffix;
      suffix = nullptr;
      iter++;
    }
    m_object_name_map.clear();
  }
  void clear_worker_pool() {
    for (uint32_t loop = 0; loop < m_worker_pool.size(); loop++) {
      lb_obs_download_worker *worker = m_worker_pool[loop];
      worker->destroy();
      m_handler_mgr->come_back_handler(worker->m_handler);
      delete worker;
      worker = nullptr;
    }
    m_worker_pool.clear();
  }
  void destroy_handler_mgr() {
    if (nullptr != m_handler_mgr) {
      if (m_handler_mgr->m_lb_obs_interface != nullptr) {
        m_handler_mgr->m_lb_obs_interface->deinit();
        delete m_handler_mgr->m_lb_obs_interface;
        m_handler_mgr->m_lb_obs_interface = nullptr;
      }
      delete m_handler_mgr;
      m_handler_mgr = nullptr;
    }
  }
  void destroy_trace_logger() {
    lb_obs_download_trace_logger::destroy_instance();
  }

  /* command parameters begin */
  std::string m_prefix_key;
  /* record the obs ak, sk, name, url and so on */
  std::string m_config_name;
  /* record last download process and support resumption of work after a break
   */
  std::string m_trace_log_name;
  /* command parameters end */

  /* read from config file begin */
  bool m_obs_mode;
  uint32_t m_buffer_len;
  std::map<std::string, std::string> m_obs_cfg_map;
  /* the target base path in block storage */
  std::string m_base_path;
  /* after which one delimiter of object name, the sub string is needed
     to be assembled into a complete file path with m_base_path */
  uint32_t m_no_delimiter;
  /* read from config file end */

  /* map file name to object suffix */
  std::unordered_map<std::string, object_suffix *> m_object_name_map;
  std::vector<lb_obs_download_worker *> m_worker_pool;
  lb_obs_download_trace_logger *m_trace_logger;
  std::vector<object_relation> *m_object_relation_list;
};

bool lb_obs_download_mgr::read_config() {
  std::ifstream ifs(m_config_name, std::ios::in | std::ios::binary);
  if (!ifs.is_open()) {
    return true;
  }
  rapidjson::IStreamWrapper isw(ifs);
  rapidjson::Document doc;
  doc.ParseStream(isw);
  if (doc.HasParseError() || !doc.HasMember("obs_mode") ||
      !doc.HasMember("buffer_len") || !doc.HasMember("obs_url") ||
      !doc.HasMember("ak") || !doc.HasMember("sk") ||
      !doc.HasMember("bucket_name") || !doc.HasMember("base_path") ||
      !doc["obs_mode"].IsBool() || !doc["buffer_len"].IsUint() ||
      !doc["obs_url"].IsString() || !doc["ak"].IsString() ||
      !doc["sk"].IsString() || !doc["bucket_name"].IsString() ||
      !doc["base_path"].IsString()) {
    return true;
  }

  m_obs_mode = doc["obs_mode"].GetBool();
  m_buffer_len = doc["buffer_len"].GetUint();
  std::string obs_url = doc["obs_url"].GetString();
  std::string ak = doc["ak"].GetString();
  std::string sk = doc["sk"].GetString();
  std::string bucket_name = doc["bucket_name"].GetString();
  std::string uri_style = "0";
  if (doc.HasMember("uri_style") && doc["uri_style"].IsUint()) {
    uri_style = doc["uri_style"].GetUint() == 1 ? "1" : "0";
  }
  std::string scc_decrypt = "false";
  if (doc.HasMember("scc_decrypt") && doc["scc_decrypt"].IsBool()) {
    scc_decrypt = doc["scc_decrypt"].GetBool() ? "true" : "false";
  }
  m_obs_cfg_map = {{"obs_url", obs_url},
                   {"AK", ak},
                   {"SK", sk},
                   {"bucket_name", bucket_name},
                   {"uri_style", uri_style},
                   {"scc_decrypt", scc_decrypt}};
  m_base_path = doc["base_path"].GetString();
  m_no_delimiter = static_cast<uint32_t>(
      std::count(m_prefix_key.begin(), m_prefix_key.end(), FN_LIBCHAR));
  if (m_trace_log_name.empty()) {
    m_trace_log_name = m_base_path + "/download_trace.json";
  }
  return false;
}

bool lb_obs_download_mgr::get_all_name_list() {
  lb_object_handler *handler = m_handler_mgr->fetch_handler();
  if (nullptr == handler) {
    return true;
  }
  std::vector<std::string> object_list;
  if (handler->list_object_list(m_prefix_key, object_list)) {
    m_handler_mgr->come_back_handler(handler);
    return true;
  }

  bool is_error = false;
  for (uint32_t loop = 0; loop < object_list.size(); loop++) {
    std::string &object_name = object_list[loop];
    std::string prefix_name;
    uint32_t type = 0;
    uint32_t version = 0;
    uint32_t id = 0;
    if (lb_object_handler::get_prefix_name(object_name, prefix_name)) {
      is_error = true;
      break;
    }
    if (lb_object_handler::get_number(object_name, &type, &version, &id)) {
      is_error = true;
      break;
    }
    auto iter = m_object_name_map.find(prefix_name);
    if (iter == m_object_name_map.end()) {
      object_suffix *suffix = new (std::nothrow) object_suffix();
      if (nullptr == suffix) {
        is_error = true;
        break;
      }
      suffix->m_type = type;
      suffix->m_version = version;
      suffix->id_list.insert(id);
      m_object_name_map[prefix_name] = suffix;
    } else {
      object_suffix *suffix = iter->second;
      suffix->id_list.insert(id);
    }
  }

  m_handler_mgr->come_back_handler(handler);
  return is_error;
}

bool lb_obs_download_mgr::init(uint32_t worker_num) {
  if (read_config()) {
    return true;
  }
  m_handler_mgr =
      new (std::nothrow) lb_object_handler_mgr(m_obs_mode, m_buffer_len);
  if (nullptr == m_handler_mgr) {
    return true;
  }
  if (m_handler_mgr->init(m_obs_cfg_map)) {
    delete m_handler_mgr;
    m_handler_mgr = nullptr;
    return true;
  }
  if (!lb_obs_download_trace_logger::is_initialized()) {
    lb_obs_download_trace_logger::create_instance();
  }
  m_trace_logger = lb_obs_download_trace_logger::get_instance();
  if (m_trace_logger->init(m_trace_log_name)) {
    return true;
  }

  uint32_t loop = 0;
  for (loop = 0; loop < worker_num; loop++) {
    lb_obs_download_worker *worker =
        new (std::nothrow) lb_obs_download_worker();
    if (nullptr == worker) {
      break;
    }
    lb_object_handler *handler = m_handler_mgr->fetch_handler();
    if (nullptr == handler) {
      delete worker;
      worker = nullptr;
      break;
    }
    if (worker->init(handler, m_trace_logger, m_base_path, m_no_delimiter,
                     m_object_relation_list)) {
      m_handler_mgr->come_back_handler(handler);
      delete worker;
      handler = nullptr;
      worker = nullptr;
      break;
    }
    m_worker_pool.push_back(worker);
  }
  if (loop < worker_num) {
    return true;
  }
  return false;
}

lb_obs_download_worker *lb_obs_download_mgr::select_worker() {
  lb_obs_download_worker *need_worker = nullptr;
  while (!need_worker) {
    for (uint32_t loop = 0; loop < m_worker_pool.size(); loop++) {
      lb_obs_download_worker *worker = m_worker_pool[loop];
      if (worker->is_leave()) {
        return nullptr;
      }
      if (!worker->is_busy()) {
        need_worker = worker;
        break;
      }
    }
    if (nullptr == need_worker) {
      wait_timeout();
    }
  }
  return need_worker;
}

bool lb_obs_download_mgr::dispatch_action() {
  auto iter = m_object_name_map.begin();
  while (iter != m_object_name_map.end()) {
    const std::string &prefix_name = iter->first;
    object_suffix *suffix = iter->second;
    lb_obs_download_worker *worker = select_worker();
    if (worker == nullptr) {
      return true;
    }
    if (DBUG_EVALUATE_IF("bd_worker_mock_obs", false, true)) {
      object_name_group *name_group =
          new (std::nothrow) object_name_group(prefix_name, suffix);
      if (nullptr == name_group) {
        return true;
      }
      worker->push_object_name(name_group);
    }
    iter++;
  }
  return false;
}

bool lb_obs_download_mgr::start() {
  if (get_all_name_list()) {
    return true;
  }
  if (dispatch_action()) {
    return true;
  }
  return false;
}

void lb_obs_download_mgr::destroy() {
  clear_worker_pool();
  clear_obs_cfg_and_param();
  clear_object_name_map();
  destroy_handler_mgr();
  destroy_trace_logger();
}

bool start_lb_obs_objects_download(std::string &prefix_key,
                                   std::string &config_name,
                                   std::string &trace_log_name,
                                   uint32_t worker_num) {
  bool is_error = false;
  lb_obs_download_mgr mgr(prefix_key, config_name, trace_log_name);
  if (mgr.init(worker_num)) {
    is_error = true;
  }
  if (!is_error && mgr.start()) {
    is_error = true;
  }
  mgr.destroy();
  return is_error;
}

bool show_lb_obs_objects_relation(std::string &prefix_key,
                                  std::string &config_name,
                                  std::vector<object_relation> *relation_list) {
  bool is_error = false;
  std::string trace_log_name;
  lb_obs_download_mgr mgr(prefix_key, config_name, trace_log_name,
                          relation_list);
  if (mgr.init(1)) {
    is_error = true;
  }
  if (!is_error && mgr.start()) {
    is_error = true;
  }
  mgr.destroy();
  return is_error;
}
