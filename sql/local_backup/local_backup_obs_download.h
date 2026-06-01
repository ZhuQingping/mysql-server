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

#ifndef LOCAL_BACKUP_OBS_DOWNLOAD_H
#define LOCAL_BACKUP_OBS_DOWNLOAD_H

#include <string>
#include "local_backup_obs_download_utils.h"

/**
  Start the download of objects from OBS with the specified prefix key.

  @param[in]      prefix_key          The prefix key to filter objects in OBS.
  @param[in]      config_name         The name of the configuration file.
  @param[in]      trace_log_name      The name of the trace log file to record
  download progress.
  @param[in]      worker_num          The number of worker threads to use for
  downloading.

  @return false if the download was successfully, true otherwise.
*/
bool start_lb_obs_objects_download(std::string &prefix_key,
                                   std::string &config_name,
                                   std::string &trace_log_name,
                                   uint32_t worker_num);

/**
  Show the relationship between file and object in OBS with the specified prefix
  key.

  @param[in]       prefix_key         The prefix key to filter objects in OBS.
  @param[in]       config_name        The name of the configuration file.
  @param[out]      relation_list      A pointer to a vector that will store the
  object relations.

  @return false if the object relations were retrieved successfully, true
  otherwise.
*/
bool show_lb_obs_objects_relation(std::string &prefix_key,
                                  std::string &config_name,
                                  std::vector<object_relation> *relation_list);

#endif