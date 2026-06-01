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

#include "local_backup_obs_handler.h"
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include "my_dbug.h"
#include "my_thread.h"
#include "securec.h"

#include "sql/local_backup/full_local_backup.h"
#include "sql/local_backup/local_backup_file_utils.h"

void lb_object_handler::make_object_out_name(std::string &out_name,
                                             std::string &file_name,
                                             uint32_t type, uint32_t version,
                                             uint32_t id) {
  out_name.clear();
  out_name = file_name;
  out_name += "+";
  out_name += std::to_string(type);
  out_name += "@";
  out_name += std::to_string(version);
  out_name += "@";
  out_name += std::to_string(id);
}
/* object name: [file_name]+[type]@[version]@[id]*/
void lb_object_handler::make_object_name(std::string &file_name, uint32_t type,
                                         uint32_t version, uint32_t id) {
  lb_object_handler::make_object_out_name(m_object_name, file_name, type,
                                          version, id);
}
void lb_object_handler::reset() {
  clear();
  m_lb_inf = nullptr;
  if (nullptr != m_buffer) {
    delete[] m_buffer;
  }
  m_buffer = nullptr;
  m_buffer_len = 0;
}
void lb_object_handler::clear(bool clear_name) {
  m_buffer_offset = 0;
  if (clear_name) {
    m_object_name.clear();
  }
  m_appendable_object_count = 0;
  m_append_count = 0;
  m_get_count = 0;
  m_append_position.clear();
}
std::string lb_object_handler::get_name() { return m_object_name; }
bool lb_object_handler::get_prefix_name(std::string &input_object_name,
                                        std::string &prefix_name) {
  size_t last_plus_pos = input_object_name.rfind('+');
  if (last_plus_pos != std::string::npos) {
    prefix_name = input_object_name.substr(0, last_plus_pos);
  } else {
    return true;
  }
  return false;
}
bool lb_object_handler::get_number(const std::string &input_object_name,
                                   uint32_t *type, uint32_t *version,
                                   uint32_t *id) {
  size_t last_at_plus = input_object_name.rfind('+');
  if ((last_at_plus == std::string::npos) || (last_at_plus == 0)) {
    return true;
  }
  size_t last_at_pos = input_object_name.rfind('@');
  if ((last_at_pos == std::string::npos) || (last_at_pos == 0)) {
    return true;
  }
  size_t second_last_at_pos = input_object_name.rfind('@', last_at_pos - 1);
  if (second_last_at_pos == std::string::npos) {
    return true;
  }
  if ((last_at_plus >= second_last_at_pos) ||
      (second_last_at_pos >= last_at_pos)) {
    return true;
  }

  if (type != nullptr) {
    *type = 10;
    std::string type_substr = input_object_name.substr(
        last_at_plus + 1, second_last_at_pos - last_at_plus - 1);
    if (1 == type_substr.length()) {
      if (type_substr[0] >= '0' &&
          type_substr[0] <= '0' + OBS_OBJ_TYPE_MERGED_FILES_META) {
        *type = std::stoul(type_substr);
      }
    }
    if (*type > OBS_OBJ_TYPE_MERGED_FILES_META) {
      return true;
    }
  }
  if (version != nullptr) {
    *version = 10;
    std::string version_substr = input_object_name.substr(
        second_last_at_pos + 1, last_at_pos - second_last_at_pos - 1);
    if (1 == version_substr.length()) {
      if (version_substr[0] >= '0' && version_substr[0] <= '9') {
        *version = std::stoul(version_substr);
      }
    }
    if (*version > 9) {
      return true;
    }
  }
  if (id != nullptr) {
    std::string id_substr = input_object_name.substr(last_at_pos + 1);
    for (uint32_t loop = 0; loop < id_substr.length(); loop++) {
      if ((id_substr[loop] < '0') || (id_substr[loop] > '9')) {
        return true;
      }
    }
    *id = std::stoul(id_substr);
  }

  return false;
}
bool lb_object_handler::upgrade_name_from_version() {
  std::string prefix_name;
  if (get_prefix_name(m_object_name, prefix_name)) {
    return true;
  }
  uint32_t type = 0;
  uint32_t version = 0;
  uint32_t id = 0;
  if (get_number(m_object_name, &type, &version, &id)) {
    return true;
  }

  const uint32_t max_object_version = 9;
  const uint32_t min_object_version = 0;
  if (max_object_version == version) {
    version = min_object_version;
  } else {
    version++;
  }
  make_object_name(prefix_name, type, version, id);
  return false;
}
bool lb_object_handler::upgrade_name_from_id() {
  std::string prefix_name;
  if (get_prefix_name(m_object_name, prefix_name)) {
    return true;
  }
  uint32_t type = 0;
  uint32_t version = 0;
  uint32_t id = 0;
  if (get_number(m_object_name, &type, &version, &id)) {
    return true;
  }
  id++;
  make_object_name(prefix_name, type, version, id);
  return false;
}

// LCOV_EXCL_START
bool lb_object_handler::get_type_from_name(std::string &input_object_name,
                                           uint32_t *type) {
  if (nullptr == type) {
    return true;
  }
  if (get_number(input_object_name, type, nullptr, nullptr)) {
    return true;
  }
  return false;
}
// LCOV_EXCL_STOP

bool lb_object_handler::list_object_list(
    std::string &prefix_name, std::vector<std::string> &object_list) {
  bool is_truncate = false;
  char next_marker[NAMELEN];
  memset_s(next_marker, NAMELEN, 0x0, NAMELEN);
  lb_inf::vObjects data;
  do {
    if (m_lb_inf->lb_list_all_object(is_truncate, next_marker, &data,
                                     prefix_name.c_str())) {
      return true;
    }
    for (uint32_t loop = 0; loop < data.size(); loop++) {
      object_list.push_back(data[loop].key);
    }
  } while (is_truncate);
  return false;
}
bool lb_object_handler::get_full_object() {
  std::string prefix_name;
  std::vector<std::string> list_name;
  const uint32_t min_object_version = 0;
  get_prefix_name(m_object_name, prefix_name);
  if (list_object_list(prefix_name, list_name)) {
    return true;
  }
  if (list_name.size() > 2) {
    return true;
  }
  std::string *expect_name = &(list_name[0]);
  if (expect_name == nullptr) {
    return true;
  }
  if (list_name.size() > 1) {
    uint32_t version1 = 0;
    uint32_t version2 = 0;
    std::string *name2 = &(list_name[1]);
    get_number(*expect_name, nullptr, &version1, nullptr);
    get_number(*name2, nullptr, &version2, nullptr);
    if ((version1 > version2) || (min_object_version == version1)) {
      if (remove_object(expect_name)) {
        return true;
      }
      m_object_name = *name2;
    } else {
      if (remove_object(name2)) {
        return true;
      }
      m_object_name = *expect_name;
    }
  } else {
    m_object_name = *expect_name;
  }

  int ret = m_lb_inf->lb_get_object(m_object_name.c_str(), (char *)m_buffer, 0,
                                    0, m_buffer_offset);
  if (ret) {
    clear();
    return true;
  }
  return false;
}
bool lb_object_handler::put_object() {
  if (m_buffer_offset == 0) {
    return false;
  }

  int ret = m_lb_inf->lb_put_object(m_object_name.c_str(), (char *)m_buffer,
                                    m_buffer_offset);
  if (ret) {
    clear();
    return true;
  }
  m_buffer_offset = 0;
  return false;
}
bool lb_object_handler::append_object() {
  if (m_buffer_offset == 0) {
    return false;
  }

  const uint64_t max_append_count = 8000;
  int ret = m_lb_inf->lb_append_object(m_object_name.c_str(), (char *)m_buffer,
                                       m_buffer_offset, m_append_position);
  if (ret) {
    clear();
    return true;
  }

#ifndef NDEBUG
  bool my_thread_inited = my_thread_is_inited();
  if (!my_thread_inited) {
    my_thread_init();  // For using DBUG_ to test
  }
  DBUG_EXECUTE_IF("mock_append_object_max_append", {
    if (m_buffer_offset < m_buffer_len) {
      m_appendable_object_count = max_append_count;
    }
  });
  if (!my_thread_inited) {
    my_thread_end();
  }
#endif

  m_buffer_offset = 0;
  m_appendable_object_count++;
  m_append_count++;
  /*
    Sleep after several append operations, to achieve the effect of speed
    limit.
  */
  if (rds_local_backup_sleep_interval > 0) {
    if (m_append_count % rds_local_backup_sleep_interval == 0) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(rds_local_backup_sleep_time_ms));
    }
  }
  if (m_appendable_object_count >= max_append_count) {
    if (upgrade_name_from_id()) {
      return true;
    }
    m_append_position.clear();
    m_appendable_object_count = 0;
#ifndef IS_DSTORE_BACKUP_TOOL
    increase_current_lb_object_count();
#endif
  }
  return false;
}
bool lb_object_handler::remove_object(std::string *name) {
  std::vector<std::string> object_names;
  if (nullptr == name) {
    object_names.push_back(m_object_name);
  } else {
    object_names.push_back(*name);
  }
  int ret = m_lb_inf->lb_batch_delete_objects(object_names);
  if (ret) {
    clear();
    return true;
  }
  return false;
}
bool lb_object_handler::get_object_length(uint64_t &length [[maybe_unused]]) {
#ifdef SUPPORT_OBS
  int ret = m_lb_inf->lb_get_object_meta(m_object_name.c_str(), length);
  if (0 != ret && OBS_STATUS_HttpErrorNotFound != ret) {
    return true;
  }
#endif
  return false;
}
bool lb_object_handler::get_object_stream(uint64_t start_offset,
                                          uint64_t remain_length,
                                          uint64_t &real_length) {
  clear(false);
  int ret =
      m_lb_inf->lb_get_object(m_object_name.c_str(), (char *)m_buffer,
                              start_offset, remain_length, m_buffer_offset);
  if (ret) {
    return true;
  }
  real_length = m_buffer_offset;
  m_get_count++;
  /*
    Sleep after several get operations, to achieve the effect of speed
    limit.
  */
  if (rds_local_backup_sleep_interval > 0) {
    if (m_get_count % rds_local_backup_sleep_interval == 0) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(rds_local_backup_sleep_time_ms));
    }
  }
  return false;
}

static lb_object_handler_mgr *g_lb_object_handler_mgr = nullptr;

bool lb_object_handler_mgr::create_instance(
    bool real_obs_mode, uint32_t normal_buffer_len,
    std::map<std::string, std::string> &obs_para) {
  if (nullptr == g_lb_object_handler_mgr) {
    g_lb_object_handler_mgr = new (std::nothrow)
        lb_object_handler_mgr(real_obs_mode, normal_buffer_len);
    if (nullptr == g_lb_object_handler_mgr) {
      return true;
    }
    return g_lb_object_handler_mgr->init(obs_para);
  }
  return false;
}
void lb_object_handler_mgr::destroy_instance() {
  if (nullptr != g_lb_object_handler_mgr) {
    if (g_lb_object_handler_mgr->m_lb_obs_interface != nullptr) {
      g_lb_object_handler_mgr->m_lb_obs_interface->deinit();
      delete g_lb_object_handler_mgr->m_lb_obs_interface;
      g_lb_object_handler_mgr->m_lb_obs_interface = nullptr;
    }
    delete g_lb_object_handler_mgr;
  }
  g_lb_object_handler_mgr = nullptr;
}
bool lb_object_handler_mgr::init_obs_sdk(
    std::map<std::string, std::string> &obs_para) {
  int ret = m_lb_obs_interface->init(obs_para);
  if (ret) {
    return true;
  }
  return false;
}

bool lb_object_handler_mgr::init(std::map<std::string, std::string> &obs_para) {
#ifdef SUPPORT_OBS
  if (m_real_obs_mode) {
    m_lb_obs_interface = new (std::nothrow) lb_inf_obs_impl();
  } else {
    m_lb_obs_interface = new (std::nothrow) lb_inf_file_impl();
  }
#else
  // LCOV_EXCL_START
  m_lb_obs_interface = new (std::nothrow) lb_inf_file_impl();
  // LCOV_EXCL_STOP
#endif
  if (nullptr == m_lb_obs_interface) {
    return true;
  }

  if (init_obs_sdk(obs_para)) {
    delete m_lb_obs_interface;
    m_lb_obs_interface = nullptr;
    return true;
  }
  return false;
}
void lb_object_handler_mgr::free_all_handler() {
  while (!m_free_pool.empty()) {
    lb_object_handler *handler = m_free_pool.front();
    delete handler;
    handler = nullptr;
    m_free_pool.pop();
  }
}
void lb_object_handler_mgr::free() { free_all_handler(); }
lb_object_handler *lb_object_handler_mgr::fetch_handler() {
  lb_object_handler *handler = nullptr;
  std::unique_lock<std::mutex> lock(m_lock);
  if (!m_free_pool.empty()) {
    handler = m_free_pool.front();
    m_free_pool.pop();
    handler->clear();
  } else {
    handler = new (std::nothrow) lb_object_handler();
    if (nullptr == handler) {
      return handler;
    }
    handler->m_buffer = new (std::nothrow) uint8_t[m_normal_buffer_len];
    if (nullptr == handler->m_buffer) {
      delete handler;
      handler = nullptr;
      return handler;
    }
    handler->m_lb_inf = m_lb_obs_interface;
    handler->m_buffer_len = m_normal_buffer_len;
  }
  return handler;
}
void lb_object_handler_mgr::come_back_handler(lb_object_handler *handler) {
  handler->clear();
  std::unique_lock<std::mutex> lock(m_lock);
  m_free_pool.push(handler);
}
lb_object_handler *fetch_lb_object_handler() {
  return g_lb_object_handler_mgr->fetch_handler();
}
/* mysqld startup and create */
bool create_lb_object_handler_instance(
    bool real_obs_mode, uint32_t normal_buffer_len,
    std::map<std::string, std::string> &obs_para) {
  return lb_object_handler_mgr::create_instance(real_obs_mode,
                                                normal_buffer_len, obs_para);
}
/* mysqld shutdown and destroy */
void destroy_lb_object_handler_instance() {
  lb_object_handler_mgr::destroy_instance();
}
void come_back_lb_object_handler(lb_object_handler *handler) {
  g_lb_object_handler_mgr->come_back_handler(handler);
}

// LCOV_EXCL_START
/* if stop local backup including full or archive, then clear */
void clear_all_lb_object_handler() { g_lb_object_handler_mgr->free(); }
// LCOV_EXCL_STOP
