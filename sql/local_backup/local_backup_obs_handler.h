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

#ifndef LOCAL_BACKUP_OBS_HANDLER_H
#define LOCAL_BACKUP_OBS_HANDLER_H

#include <cctype>
#include <iostream>
#include <queue>
#include <string>

#include "local_backup_obs_utils.h"

struct __attribute__((packed)) lb_object_file_group_header {
  uint32_t version;
  uint64_t file_count;
};

struct __attribute__((packed)) lb_object_file_describe {
  uint32_t file_path_length;
  uint64_t file_content_length;
};

struct lb_object_handler {
  lb_object_handler() : m_buffer(nullptr) { reset(); }
  ~lb_object_handler() {
    destroy();
    reset();
  }

  /* object name: [file_name]+[type]@[version]@[id] */
  void make_object_name(std::string &file_name, uint32_t type, uint32_t version,
                        uint32_t id);
  static void make_object_out_name(std::string &out_name,
                                   std::string &file_name, uint32_t type,
                                   uint32_t version, uint32_t id);
  void reset();
  void clear(bool clear_name = true);
  std::string get_name();
  static bool get_number(const std::string &object_name, uint32_t *type,
                         uint32_t *version, uint32_t *id);
  static bool get_prefix_name(std::string &object_name,
                              std::string &prefix_name);
  bool upgrade_name_from_version();
  bool upgrade_name_from_id();
  bool get_type_from_name(std::string &input_object_name, uint32_t *type);
  bool list_object_list(std::string &prefix_name,
                        std::vector<std::string> &object_list);
  bool get_full_object();
  bool get_object_stream(uint64_t start_offset, uint64_t remain_length,
                         uint64_t &real_length);
  bool put_object();
  bool append_object();
  bool remove_object(std::string *name = nullptr);
  bool get_object_length(uint64_t &length);
  void destroy() {
    if (nullptr != m_buffer) {
      delete[] m_buffer;
      m_buffer = nullptr;
    }
    m_lb_inf = nullptr;
  }

  /* every handler share the same lb_inf object within the lb_object_handler_mgr
   * instance */
  lb_inf *m_lb_inf;
  uint8_t *m_buffer;
  std::string m_object_name;
  uint64_t m_buffer_len;
  /* current buffer filling length */
  uint64_t m_buffer_offset;
  uint32_t m_appendable_object_count;
  std::string m_append_position;
  uint64_t m_append_count;
  uint64_t m_get_count;
};

bool create_lb_object_handler_instance(
    bool real_obs_mode, uint32_t normal_buffer_len,
    std::map<std::string, std::string> &obs_para);
void destroy_lb_object_handler_instance();
lb_object_handler *fetch_lb_object_handler();
void come_back_lb_object_handler(lb_object_handler *handler);
void clear_all_lb_object_handler();

class lb_object_handler_mgr {
 public:
  static bool create_instance(bool real_obs_mode, uint32_t normal_buffer_len,
                              std::map<std::string, std::string> &obs_para);
  static void destroy_instance();

  lb_object_handler_mgr(bool real_obs_mode, uint32_t normal_buffer_len)
      : m_lb_obs_interface(nullptr),
        m_real_obs_mode(real_obs_mode),
        m_normal_buffer_len(normal_buffer_len),
        m_lock(),
        m_free_pool() {}

  ~lb_object_handler_mgr() { free(); }

  bool init(std::map<std::string, std::string> &obs_para);
  lb_object_handler *fetch_handler();
  void come_back_handler(lb_object_handler *handler);
  void free();

  lb_inf *m_lb_obs_interface;

 private:
  bool init_obs_sdk(std::map<std::string, std::string> &obs_para);

  void free_all_handler();

  bool m_real_obs_mode;
  uint32_t m_normal_buffer_len;
  std::mutex m_lock;
  std::queue<lb_object_handler *> m_free_pool;

  // std::map<std::string, std::string> m_obs_para;
};

#endif
