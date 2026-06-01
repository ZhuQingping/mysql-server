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

#include "sql/vfd/vfd_file.h"
#include <fcntl.h>
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysql/psi/mysql_file.h"
#include "sql/mysqld.h"
#include "sql/vfd/vfd_manager.h"

VfdFile::~VfdFile() {
  // We intentionally ignore the return value from Delete(), as there is not
  // much we can do about any errors reported from it.
  (void)Delete();
}

bool VfdFile::Delete() noexcept {
  if (IsOpen()) {
    if (mysql_file_close(m_file, MYF(0)) != 0) {
      return true;
    }
  }

  if (!m_full_path.empty()) {
    if (mysql_file_delete(key_file_hash_join, m_full_path.c_str(), MYF(0)) !=
        0) {
      return true;
    }
  }

  return false;
}

bool VfdFile::Read(uchar *dst, size_t length) {
  size_t dst_idx = 0;
  while (length > 0) {
    if (m_buffer_size == 0) {
      // The in-memory buffer is empty, so fill it up with data from the file on
      // disk. The VfdManager has opened the file for us if this is necessary.
      assert(is_read_buffer);
      size_t num_bytes_read =
          mysql_file_read(m_file, m_buffer, kMaxBufferSize, MYF(0));
      if (num_bytes_read == MY_FILE_ERROR) {
        return true;
      }
      m_buffer_size = num_bytes_read;
      m_buffer_ptr = m_buffer;
    }

    // Copy data from the in-memory buffer to "dst". May be called multiple
    // times per function call if "length" is larger than the in-memory buffer.
    size_t bytes_to_copy = std::min(length, m_buffer_size);
    memcpy(dst + dst_idx, m_buffer_ptr, bytes_to_copy);
    m_buffer_ptr += bytes_to_copy;
    dst_idx += bytes_to_copy;
    m_buffer_size -= bytes_to_copy;
    assert(length >= bytes_to_copy);
    length -= bytes_to_copy;
  }

  return false;
}

bool VfdFile::FlushBufferToFile() {
  if (m_buffer_size == 0) {
    return false;
  }

  assert(!is_read_buffer);
  size_t num_bytes_written =
      mysql_file_write(m_file, m_buffer, m_buffer_size, MYF(0));
  if (num_bytes_written == MY_FILE_ERROR ||
      num_bytes_written != m_buffer_size) {
    return true;
  }

  m_buffer_size = 0;
  return false;
}

bool VfdFile::Write(const uchar *buf, size_t count) {
  while (count > 0) {
    if (m_buffer_size == kMaxBufferSize) {
      // If the in-memory buffer is full, flush its contents to disk. The file
      // is already opened by the VfdManager if this is necessary.
      if (FlushBufferToFile()) {
        return true;
      }
    }

    size_t bytes_to_copy = std::min(kMaxBufferSize - m_buffer_size, count);
    memcpy(m_buffer + m_buffer_size, buf, bytes_to_copy);
    buf += bytes_to_copy;
    count -= bytes_to_copy;
    m_buffer_size += bytes_to_copy;
  }

  return false;
}

bool VfdFile::Open(int wanted_filemode) {
  assert(wanted_filemode == O_RDONLY || wanted_filemode == O_WRONLY);

  if (!IsInitialized()) {
    // The file has not been initialized, meaning that this is the first call to
    // Open(). We must thus create the actual file on disk.
    assert(m_full_path.empty());
    char filename[FN_REFLEN];

    // Note that both "mode" and "myf" is currently unused on UNIX platforms.
    m_file =
        mysql_file_create_temp(key_file_hash_join, filename, mysql_tmpdir,
                               kVfdFilePrefix, /*mode=*/0, KEEP_FILE, MYF(0));
    if (m_file == -1) {
      return true;
    }

    m_full_path.assign(filename);
    is_read_buffer = wanted_filemode == O_RDONLY;

    // It does not make sense to request for reading from a newly created file.
    // So requesting O_RDONLY here would most likely be a mistake.
    assert(wanted_filemode == O_WRONLY);
  }

  if (IsOpen()) {
    return false;
  }

  is_read_buffer = wanted_filemode == O_RDONLY;
  m_file = mysql_file_open(key_file_hash_join, m_full_path.c_str(),
                           wanted_filemode, /*myf=*/0);
  if (m_file == -1) {
    return true;
  }

  my_off_t res = mysql_file_seek(m_file, m_file_offset, SEEK_SET, MYF(0));
  assert(res != MY_FILEPOS_ERROR);
  return res == MY_FILEPOS_ERROR;
}

bool VfdFile::Close(bool rewind_file) {
  if (rewind_file) {
    if (!is_read_buffer) {
      FlushBufferToFile();
    }
    m_file_offset = 0;
  }

  if (!IsOpen()) {
    return false;
  }

  if (!rewind_file) {
    // Save the current position in the file, so we know where to start
    // reading/writing on the next call to Open().
    m_file_offset = mysql_file_tell(m_file, MYF(0));
    if (m_file_offset == MY_FILEPOS_ERROR) {
      return true;
    }
  }

  if (mysql_file_close(m_file, /*myf=*/0) == -1) {
    return true;
  }

  m_file = kClosed;
  return false;
}
