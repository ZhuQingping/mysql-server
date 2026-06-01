#ifndef SQL_VFD_VFD_MANAGER_H
#define SQL_VFD_VFD_MANAGER_H

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

#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "my_dir.h"
#include "my_io.h"
#include "my_sys.h"
#include "mysql/components/services/bits/my_io_bits.h"
#include "sql/mysqld.h"
#include "sql/vfd/lru.h"

static constexpr const char *kVfdFilePrefix = "mysql_vfd_";

class VfdFile;

/// VfdManager (VFD == virtual file descriptor) is responsible for handling
/// reads and writes from/to VfdFile. The basic idea is that the VfdManager will
/// keep the number of open files below a certain threshold to avoid hitting any
/// limit on the number of open files. It is implemented by having "VfdFiles",
/// which are files on disk that are opened and closed as needed. If a file must
/// be opened for reading or writing and we cannot do it without exceeding our
/// threshold, the least recently used file will be closed. Thus, this class
/// also contains an LRU that keeps track of the least recently used file.
///
/// The VfdManager is thread-safe, meaning that multiple threads can call any of
/// its functions.
class VfdManager {
 public:
  VfdManager(size_t max_open_files) : m_max_open_files(max_open_files) {}

  /// Read "length" bytes of data from the given file, and put the contents in
  /// "dst". "dst" must be allocated by the caller, and must be at least
  /// "length" bytes long.
  bool Read(VfdFile *file, void *dst, size_t length);

  /// Write "length" bytes from "src" into the file. It will first try to put
  /// the data info the VfdFile in-memory buffer. If there is not enough room,
  /// the file will be openend and its buffered data flushed to disk.
  bool Write(VfdFile *file, const void *src, size_t length);

  /// Close the file and rewind it. This means that the next time you do Read or
  /// Write, it will start from the beginning.
  bool Close(VfdFile *file) noexcept;

  /// Remove any leftover VFD files that might have not been deleted. Usually
  /// called during server startup.
  static void RemoveLeftoverFiles();

  void MoveFile(VfdFile &to, VfdFile &&from);

  void SetNeedsMutex(bool needs_mutex) { m_needs_mutex = needs_mutex; }

 private:
  bool OpenIfNecessary(VfdFile *file, int wanted_filemode);

  const size_t m_max_open_files;
  std::atomic<int> m_next_id{0};

  /// And LRU that keeps track of the least recently used file.
  LRU<int, VfdFile *> m_open_vfd_files;

  std::mutex m_lru_mutex;

  /// Whether LRU operations needs mutex protection. Will be true if we have a
  /// parallel query with at least one HashJoinIterator.
  bool m_needs_mutex{false};
};

#endif
