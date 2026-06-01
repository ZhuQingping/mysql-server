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

#include "sql/local_backup/local_backup_utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <memory>

#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "securec.h"

bool gen_merged_file_obj_meta(std::vector<BakFileItem> &files,
                              lb_object_handler *handler) {
  uint64_t file_num = files.size();
  uint32_t meta_size = sizeof(lb_object_file_group_header);

  for (uint32_t i = 0; i < file_num; i++) {
    meta_size += sizeof(lb_object_file_describe);
    meta_size += files[i].file_name.size();
  }

  char *meta_data = new (std::nothrow) char[meta_size];
  if (meta_data == nullptr) {
    return true;
  }

  ((lb_object_file_group_header *)meta_data)->version = 0;
  ((lb_object_file_group_header *)meta_data)->file_count = file_num;
  char *ptr = meta_data + sizeof(lb_object_file_group_header);
  for (uint32_t i = 0; i < file_num; i++) {
    ((lb_object_file_describe *)ptr)->file_path_length =
        files[i].file_name.size();
    ((lb_object_file_describe *)ptr)->file_content_length = files[i].file_size;
    ptr += sizeof(lb_object_file_describe);
  }
  for (uint32_t i = 0; i < file_num; i++) {
    error_t rc =
        memcpy_s(ptr, files[i].file_name.size(), files[i].file_name.c_str(),
                 files[i].file_name.size());
    // LCOV_EXCL_START
    if (rc != 0) {
      delete[] meta_data;
      return true;
    }
    // LCOV_EXCL_STOP
    ptr += files[i].file_name.size();
  }
  DBUG_ASSERT((ptr - meta_data) == meta_size);

  bool ret = append_data_to_obj(meta_data, meta_size, handler);
  delete[] meta_data;
  return ret;
}

off_t get_file_size_for_backup(std::string &file_name) {
  off_t size = -1;
  int fd = open(file_name.c_str(), O_RDONLY, my_umask);
  if (fd == -1) {
    return size;
  }
  size = lseek(fd, 0, SEEK_END);
  close(fd);
  return size;
}

bool append_data_to_obj(const char *data, uint64_t data_size,
                        lb_object_handler *handler) {
  uint copy_step = handler->m_buffer_len;
  uint8_t *orig_buf = handler->m_buffer;

  bool ret = false;
  uint64_t left_size = data_size;
  while (left_size > 0) {
    uint copy_size = left_size <= copy_step ? left_size : copy_step;

    handler->m_buffer =
        (uint8_t *)(const_cast<char *>(data + (data_size - left_size)));
    handler->m_buffer_offset = copy_size;
    ret = handler->append_object();
    if (ret) {
      break;
    }
    left_size -= copy_size;
  }
  handler->m_buffer = orig_buf;
  return ret;
}

bool append_one_file_to_obj(std::string &file_name, uint64_t file_size,
                            lb_object_handler *handler) {
  /* Skip empty file or directory. */
  if (file_size == 0) {
    return false;
  }

  int fd = open(file_name.c_str(), O_RDONLY, my_umask);
  if (fd == -1) {
    return true;
  }

  uint copy_step = handler->m_buffer_len;
  bool ret = false;
  uint64_t left_size = file_size;
  while (left_size > 0) {
    uint64_t copy_offset = file_size - left_size;
    uint copy_size = left_size <= copy_step ? left_size : copy_step;

    uint64_t buf_left = handler->m_buffer_len - handler->m_buffer_offset;
    if (buf_left == 0) {
      ret = handler->append_object();
      if (ret) {
        break;
      }
      buf_left = handler->m_buffer_len;
    }
    if (copy_size > buf_left) {
      copy_size = buf_left;
    }

    size_t read_size =
        my_pread(fd, handler->m_buffer + handler->m_buffer_offset, copy_size,
                 copy_offset, MYF(0));
    if (read_size == MY_FILE_ERROR) {
      ret = true;
      break;
    }
    handler->m_buffer_offset += read_size;
    left_size -= read_size;
  }

  close(fd);
  return ret;
}