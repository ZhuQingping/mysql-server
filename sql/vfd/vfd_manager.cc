/* Copyright (c) 2023, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/vfd/vfd_manager.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <mutex>
#include <thread>
#include "scope_guard.h"
#include "sql/current_thd.h"
#include "sql/sql_class.h"
#include "sql/vfd/vfd_file.h"

bool VfdManager::Read(VfdFile *file, void *dst, size_t length) {
  // Can we read everything from the internal file buffer? If not, open the
  // underlying file first.
  if (!file->HasEverythingInBuffer(length)) {
    auto lock = m_needs_mutex ? std::unique_lock<std::mutex>(m_lru_mutex)
                              : std::unique_lock<std::mutex>();

    // OpenIfNecessary will bump the file to the front of the LRU.
    if (OpenIfNecessary(file, O_RDONLY)) {
      return true;
    }

    // Assert that OpenIfNecessary() actually did move the file to the front of
    // the LRU.
    assert(m_open_vfd_files.begin()->first == file->Id());

    return file->Read(pointer_cast<uchar *>(dst), length);
  } else {
    return file->Read(pointer_cast<uchar *>(dst), length);
  }
}

bool VfdManager::Write(VfdFile *file, const void *src, size_t length) {
  // Does everything fit in the buffer? If not, open the underlying file for
  // writing first.
  if (!file->HasRoomInBuffer(length)) {
    auto lock = m_needs_mutex ? std::unique_lock<std::mutex>(m_lru_mutex)
                              : std::unique_lock<std::mutex>();

    // OpenIfNecessary will bump the file to the front of the LRU.
    if (OpenIfNecessary(file, O_WRONLY)) {
      return true;
    }

    // Assert that OpenIfNecessary() actually did move the file to the front of
    // the LRU.
    assert(m_open_vfd_files.begin()->first == file->Id());

    return file->Write(pointer_cast<const uchar *>(src), length);
  } else {
    return file->Write(pointer_cast<const uchar *>(src), length);
  }
}

bool VfdManager::Close(VfdFile *file) noexcept {
  auto lock = m_needs_mutex ? std::unique_lock<std::mutex>(m_lru_mutex)
                            : std::unique_lock<std::mutex>();

  if (file->HasDataInBuffer() && !file->is_read_buffer) {
    if (OpenIfNecessary(file, O_WRONLY)) {
      return true;
    }
  }
  if (file->Close(/*rewind_file=*/true)) {
    return true;
  }

  if (m_open_vfd_files.Contains(file->Id()) &&
      m_open_vfd_files.Delete(file->Id())) {
    return true;
  }
  return false;
}

void VfdManager::RemoveLeftoverFiles() {
  for (uint i = 0; i <= mysql_tmpdir_list.max; ++i) {
    const char *tmpdir = mysql_tmpdir_list.list[i];
    MY_DIR *dir = my_dir(tmpdir, MYF(0));
    if (dir == nullptr) {
      continue;
    }

    for (uint j = 0; j < dir->number_off_files; ++j) {
      fileinfo file = dir->dir_entry[j];
      if (strncmp(file.name, kVfdFilePrefix, strlen(kVfdFilePrefix)) == 0) {
        std::string full_path =
            std::string(tmpdir) + std::string("/") + std::string(file.name);
        my_delete(full_path.c_str(), MYF(0));
      }
    }
    my_dirend(dir);
  }
}

bool VfdManager::OpenIfNecessary(VfdFile *file, int wanted_filemode) {
  if (!file->IsInitialized()) {
    // We do not care about m_next_id wrapping around:
    // - The VfdManager is attached to the THD. So the VfdManager only lives for
    //   one connection.
    // - m_next_id is an integer, which means that one connection would have to
    //   create more than 2 billion files for it to wrap around, which seems
    //   highly unlikely (you would have to create 68 temporary files per second
    //   for a year).
    file->m_id = m_next_id++;
  }

  if (file->IsOpen()) {
    assert((wanted_filemode == O_RDONLY) == file->is_read_buffer);
    assert(m_open_vfd_files.Contains(file->Id()));

    // The file is already open, so bump it to the front of the LRU.
    m_open_vfd_files.Get(file->Id());
    return false;
  }

  // File is not open. See if we have room for more open files without
  // exceeding our budget.
  assert(!m_open_vfd_files.Contains(file->Id()));

  // An "if" should probably suffice, but let's play safe.
  assert(m_open_vfd_files.size() <= m_max_open_files);
  while (m_open_vfd_files.size() >= m_max_open_files) {
    auto file_to_close = m_open_vfd_files.LeastRecentlyUsed().second;
    if (file_to_close->Close(/*rewind_file=*/false) ||
        m_open_vfd_files.Delete(file_to_close->Id())) {
      return true;
    }
  }

  if (file->Open(wanted_filemode)) {
    return true;
  }
  m_open_vfd_files.Add(file->Id(), file);

  return false;
}

void VfdManager::MoveFile(VfdFile &to, VfdFile &&from) {
  Close(&to);
  Close(&from);
  to = std::move(from);
}
