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

#ifndef SQL_LOCAL_BACKUP_UTILS_INCLUDED
#define SQL_LOCAL_BACKUP_UTILS_INCLUDED

#include <string>
#include <vector>

#include "sql/local_backup/local_backup_obs_handler.h"

struct BakFileItem {
  std::string file_name_orig;
  std::string file_name;
  uint64_t file_size;

  BakFileItem(const std::string &name_orig, const std::string &name,
              uint64_t size)
      : file_name_orig(name_orig), file_name(name), file_size(size) {}
};

struct BakFileGroup {
  std::vector<BakFileItem> files;
  uint64_t total_size;

  void clear() {
    files.clear();
    total_size = 0;
  }
};

/**
  Generates meta data for merged backup file object.

  @param[in]  files    Vector of information of backup files to be merged
  @param[in]  handler  Handler for OBS

  @return return false on success
*/
bool gen_merged_file_obj_meta(std::vector<BakFileItem> &files,
                              lb_object_handler *handler);

off_t get_file_size_for_backup(std::string &file_name);

bool append_data_to_obj(const char *data, uint64_t data_size,
                        lb_object_handler *handler);

bool append_one_file_to_obj(std::string &file_name, uint64_t file_size,
                            lb_object_handler *handler);
#endif  // SQL_LOCAL_BACKUP_UTILS_INCLUDED
