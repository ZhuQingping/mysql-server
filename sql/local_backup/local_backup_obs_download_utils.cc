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

#include <fstream>
#include <set>
#include <unordered_map>

#include "local_backup_obs_download_utils.h"

bool lb_file_path_creator::create_file_with_full_path(
    const std::string &file_path) {
  std::string directory = extract_directory(file_path);
  if (directory.empty() || create_directory(directory)) {
    return true;
  }
  return create_file(file_path);
}

std::string lb_file_path_creator::extract_directory(
    const std::string &file_path) {
  size_t found = file_path.find_last_of(FN_LIBCHAR);
  if (found != std::string::npos) {
    return file_path.substr(0, found);
  }
  return "";
}

bool lb_file_path_creator::create_dir(const std::string &file_path) {
  const uint32_t flag = 0755;
  if (0 != CDE::LBMkdir(file_path.c_str(), flag)) {
    return true;
  }
  return false;
}

bool lb_file_path_creator::create_directory(const std::string &file_path) {
  if (file_path.empty()) {
    return true;
  }
  if (directory_exists(file_path)) {
    return false;
  }
  size_t pos = file_path.find_last_of(FN_LIBCHAR);
  if (pos != std::string::npos) {
    std::string parent_path = file_path.substr(0, pos);
    if (create_directory(parent_path)) {
      return true;
    }
  }
  if (create_dir(file_path) != 0) {
    if (errno != EEXIST) {
      return true;
    }
  }
  return false;
}

bool lb_file_path_creator::directory_exists(const std::string &file_path) {
  struct stat info;
  if (CDE::LBStat(file_path.c_str(), &info) != 0) {
    return false;
  }
  return (info.st_mode & S_IFDIR) != 0;
}

bool lb_file_path_creator::create_file(const std::string &file_path) {
  std::ofstream file(file_path.c_str(), std::ios::out | std::ios::app);
  if (file.is_open()) {
    file.close();
    return false;
  }
  return true;
}

lb_obs_download_trace_logger
    *lb_obs_download_trace_logger::m_trace_logger_instance = nullptr;
std::mutex lb_obs_download_trace_logger::m_trace_logger_instance_mtx;

lb_obs_download_trace_logger *lb_obs_download_trace_logger::get_instance() {
  return m_trace_logger_instance;
}

bool lb_obs_download_trace_logger::is_initialized() {
  return (nullptr != m_trace_logger_instance);
}

bool lb_obs_download_trace_logger::create_instance() {
  std::unique_lock<std::mutex> lock(m_trace_logger_instance_mtx);
  if (nullptr == m_trace_logger_instance) {
    m_trace_logger_instance = new (std::nothrow) lb_obs_download_trace_logger();
  }
  return (nullptr == m_trace_logger_instance);
}

void lb_obs_download_trace_logger::destroy_instance() {
  std::unique_lock<std::mutex> lock(m_trace_logger_instance_mtx);
  if (nullptr != m_trace_logger_instance) {
    delete m_trace_logger_instance;
  }
  m_trace_logger_instance = nullptr;
}

bool lb_obs_download_trace_logger::create_full_path_file(
    std::string &full_name) {
  return lb_file_path_creator::create_file_with_full_path(full_name);
}

bool lb_obs_download_trace_logger::init(const std::string &trace_log_name) {
  std::unique_lock<std::mutex> lock(m_lock);

  if (m_initialized.load()) {
    return false;
  }

  m_trace_log_name = trace_log_name;
  if (create_full_path_file(m_trace_log_name)) {
    return true;
  }

  if (load_from_file()) {
    m_progress_map.clear();
    if (save_to_file()) {
      return true;
    }
  }

  m_initialized.store(true);
  return false;
}

void lb_obs_download_trace_logger::add_progress(
    const std::string &object_name) {
  std::unique_lock<std::mutex> lock(m_lock);

  if (!m_initialized.load()) {
    return;
  }

  lb_obs_download_progress progress;
  progress.object_name = object_name;
  progress.status = lb_obs_download_status::DOWNLOADING;
  progress.downloaded_length = 0;
  progress.object_length = 0;

  m_progress_map[object_name] = progress;
}

void lb_obs_download_trace_logger::update_progress(
    const std::string &object_name, uint64_t downloaded_length,
    uint64_t object_length) {
  std::unique_lock<std::mutex> lock(m_lock);

  if (!m_initialized.load()) {
    return;
  }

  auto it = m_progress_map.find(object_name);
  if (it != m_progress_map.end()) {
    if (downloaded_length > 0) {
      it->second.downloaded_length = downloaded_length;
    }
    if (object_length > 0) {
      it->second.object_length = object_length;
    }
    if (downloaded_length == object_length) {
      it->second.status = lb_obs_download_status::COMPLETED;
    }

    save_to_file();
  }
}

rapidjson::Value lb_obs_download_trace_logger::progress_to_json(
    const lb_obs_download_progress &progress,
    rapidjson::Document::AllocatorType &allocator) const {
  rapidjson::Value json_value(rapidjson::kObjectType);

  json_value.AddMember(
      "object_name", rapidjson::Value(progress.object_name.c_str(), allocator),
      allocator);
  json_value.AddMember("status", static_cast<size_t>(progress.status),
                       allocator);
  json_value.AddMember("downloaded_length", progress.downloaded_length,
                       allocator);
  json_value.AddMember("object_length", progress.object_length, allocator);

  return json_value;
}

lb_obs_download_progress lb_obs_download_trace_logger::json_to_progress(
    const rapidjson::Value &json_value) const {
  lb_obs_download_progress progress;

  if (json_value.HasMember("object_name") &&
      json_value["object_name"].IsString()) {
    progress.object_name = json_value["object_name"].GetString();
  }
  if (json_value.HasMember("status") && json_value["status"].IsUint()) {
    progress.status =
        static_cast<lb_obs_download_status>(json_value["status"].GetUint());
  }
  if (json_value.HasMember("downloaded_length") &&
      json_value["downloaded_length"].IsUint64()) {
    progress.downloaded_length = json_value["downloaded_length"].GetUint64();
  }
  if (json_value.HasMember("object_length") &&
      json_value["object_length"].IsUint64()) {
    progress.object_length = json_value["object_length"].GetUint64();
  }

  return progress;
}

bool lb_obs_download_trace_logger::save_to_file() {
  if (m_trace_log_name.empty()) {
    return true;
  }

  std::ofstream ofs(m_trace_log_name, std::ios::binary | std::ios::trunc);
  if (!ofs.is_open()) {
    return true;
  }

  rapidjson::Document document;
  document.SetObject();
  auto &allocator = document.GetAllocator();

  rapidjson::Value arr(rapidjson::kArrayType);
  for (const auto &[object_name, progress] : m_progress_map) {
    arr.PushBack(progress_to_json(progress, allocator), allocator);
  }
  document.AddMember("progress", arr, allocator);

  rapidjson::OStreamWrapper osw(ofs);
  rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(osw);
  document.Accept(writer);

  return false;
}

bool lb_obs_download_trace_logger::load_from_file() {
  if (m_trace_log_name.empty()) {
    return true;
  }

  std::ifstream ifs(m_trace_log_name, std::ios::in | std::ios::binary);
  if (!ifs.is_open()) {
    return true;
  }

  rapidjson::IStreamWrapper isw(ifs);
  rapidjson::Document document;
  document.ParseStream(isw);
  if (document.HasParseError() || !document.IsObject() ||
      !document.HasMember("progress")) {
    return true;
  }

  m_progress_map.clear();

  const rapidjson::Value &arr = document["progress"];
  if (arr.IsArray()) {
    for (rapidjson::SizeType i = 0; i < arr.Size(); i++) {
      lb_obs_download_progress progress = json_to_progress(arr[i]);
      m_progress_map[progress.object_name] = progress;
    }
  }

  return false;
}

bool lb_obs_download_trace_logger::has_progress(
    const std::string &object_name) {
  std::unique_lock<std::mutex> lock(m_lock);
  return (m_progress_map.find(object_name) != m_progress_map.end());
}

lb_obs_download_progress lb_obs_download_trace_logger::get_progress(
    const std::string &object_name) {
  std::unique_lock<std::mutex> lock(m_lock);
  auto it = m_progress_map.find(object_name);
  if (it != m_progress_map.end()) {
    return it->second;
  }
  return lb_obs_download_progress();
}
