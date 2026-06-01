#ifndef SQL_VFD_VFD_FILE_H
#define SQL_VFD_VFD_FILE_H

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
#include <unordered_map>
#include <utility>
#include <vector>
#include "my_dir.h"
#include "my_io.h"
#include "my_sys.h"
#include "mysql/components/services/bits/my_io_bits.h"
#include "sql/mysqld.h"
#include "sql/vfd/vfd_manager.h"

class VfdManager;

/// VfdFile (VFD == virtual file descriptor) is a file on disk that is opened
/// and closed as needed by a VfdManager. All its functions/operations are
/// visible only to the VfdManager, so all operations (read, write) must go
/// through the VfdManager.
///
/// The VfdFile has a in-memory buffer that is filled before any flushing
/// to/from disk happens. This is to reduce the number of IO operations,
/// especially if we have many small reads/writes. The file is created on disk
/// on the first flush to disk, which happens once the buffer is full or when we
/// call Close() with a dirty buffer. Up until that point, all data lives in
/// memory only.
///
/// The VfdFile is _not_ thread safe. So one VfdFile must be used by exactly one
/// thread. Note that the VfdManager is thread safe, so multiple threads can
/// call its methods without any synchronization.
class VfdFile {
 public:
  VfdFile() = default;

  ~VfdFile();

  /// Not copyable
  VfdFile(const VfdFile &) = delete;
  VfdFile &operator=(VfdFile &) = delete;

  /// Move-ctor not needed
  VfdFile(VfdFile &&) = delete;

 private:
  /// But move-assignement operator is needed
  VfdFile &operator=(VfdFile &&other) {
    if (this != &other) {
      Delete();

      memcpy(m_buffer, other.m_buffer, other.m_buffer_size);
      m_buffer_size = other.m_buffer_size;
      m_file = other.m_file;
      is_read_buffer = other.is_read_buffer;
      m_full_path = std::move(other.m_full_path);
      m_file_offset = other.m_file_offset;
      m_id = other.m_id;

      // Reset other to a somewhat clean state to avoid misuse so that we do not
      // try to double-use the same file etc.
      other.m_buffer_size = 0;
      other.m_file = kUninitialized;
      other.m_full_path.clear();
      other.m_id = -1;
    }
    return *this;
  }

  /// The in-memory buffer. Data is written to this buffer on Write(), and
  /// flushed to disk only when this is full.
  static constexpr size_t kMaxBufferSize{IO_SIZE};
  uchar m_buffer[kMaxBufferSize];

  /// The current read position in the buffer. Note that this is only used when
  /// calling Read().
  uchar *m_buffer_ptr{m_buffer};

  /// The size of the data in "m_buffer".
  size_t m_buffer_size{0};

  /// The actual file on disk.
  File m_file{kUninitialized};

  /// Whether the VfdFile is in read or write mode.
  bool is_read_buffer{false};

  /// The full path on disk to the actual file.
  std::string m_full_path;

  /// The current position in the file on disk. That is, where the next
  /// read/write will happen.
  my_off_t m_file_offset{0};

  static constexpr File kClosed{-1};
  static constexpr File kUninitialized{-2};

  int m_id{-1};

  int Id() const { return m_id; }

  bool IsOpen() const { return m_file > -1; }

  bool IsInitialized() const { return m_file != kUninitialized; }

  bool FlushBufferToFile();

  bool Read(uchar *dst, size_t length);

  bool Write(const uchar *buf, size_t count);

  bool Open(int wanted_filemode);

  bool Close(bool rewind_file);

  bool Delete() noexcept;

  bool HasEverythingInBuffer(size_t bytes_to_read) const {
    return bytes_to_read <= m_buffer_size;
  }

  bool HasRoomInBuffer(size_t bytes_to_write) const {
    return bytes_to_write <= (kMaxBufferSize - m_buffer_size);
  }

  bool HasDataInBuffer() const { return m_buffer_size > 0; }

  /// Expose all private functions to VfdManager.
  friend class VfdManager;
};

#endif
