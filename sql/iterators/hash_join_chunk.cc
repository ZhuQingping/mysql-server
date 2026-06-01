/* Copyright (c) 2019, 2024, Oracle and/or its affiliates.

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

#include "sql/iterators/hash_join_chunk.h"

#include <stddef.h>
#include <new>
#include <utility>

#include "my_inttypes.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/iterators/hash_join_buffer.h"
#include "sql/mysqld.h"
#include "sql/sql_base.h"
#include "sql/sql_const.h"
#include "sql/vfd/vfd_file.h"
#include "sql/vfd/vfd_manager.h"
#include "sql_string.h"
#include "template_utils.h"

using pack_rows::TableCollection;

HashJoinChunk::HashJoinChunk(HashJoinChunk &&other)
    : m_num_rows(other.m_num_rows),
      m_uses_match_flags(other.m_uses_match_flags),
      m_vfd_manager(other.m_vfd_manager) {
  m_vfd_manager->MoveFile(m_vfd, std::move(other.m_vfd));
}

HashJoinChunk &HashJoinChunk::operator=(HashJoinChunk &&other) {
  if (this != &other) {
    m_vfd_manager = other.m_vfd_manager;
    m_num_rows = other.m_num_rows;
    m_uses_match_flags = other.m_uses_match_flags;
    m_vfd_manager->MoveFile(m_vfd, std::move(other.m_vfd));
  }
  return *this;
}

void HashJoinChunk::Init(bool uses_match_flags) {
  m_num_rows = 0;
  m_uses_match_flags = uses_match_flags;
}

bool HashJoinChunk::Rewind() {
  if (m_vfd_manager->Close(&m_vfd)) {
    my_error(ER_TEMP_FILE_WRITE_FAILURE, MYF(0));
    return true;
  }

  return false;
}

bool HashJoinChunk::WriteRowToChunk(String *buffer, bool matched,
                                    const pack_rows::TableCollection &tables) {
  if (pack_rows::StoreFromTableBuffers(tables, buffer)) {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR),
             ComputeRowSizeUpperBound(tables));
    return true;
  }

  if (m_uses_match_flags) {
    if (m_vfd_manager->Write(&m_vfd, &matched, sizeof(matched))) {
      my_error(ER_TEMP_FILE_WRITE_FAILURE, MYF(0));
      return true;
    }
  }

  // Write out the length of the data.
  size_t data_length = buffer->length();
  if (m_vfd_manager->Write(&m_vfd, &data_length, sizeof(data_length))) {
    my_error(ER_TEMP_FILE_WRITE_FAILURE, MYF(0));
    return true;
  }

  // ... and then write the actual data.
  if (m_vfd_manager->Write(&m_vfd, buffer->ptr(), data_length)) {
    my_error(ER_TEMP_FILE_WRITE_FAILURE, MYF(0));
    return true;
  }
  m_num_rows++;
  return false;
}

bool HashJoinChunk::LoadRowFromChunk(String *buffer, bool *matched,
                                     const pack_rows::TableCollection &tables) {
  if (m_uses_match_flags) {
    if (m_vfd_manager->Read(&m_vfd, matched, sizeof(*matched))) {
      my_error(ER_TEMP_FILE_WRITE_FAILURE, MYF(0));
      return true;
    }
  }

  // Read the length of the row.
  size_t row_length;
  if (m_vfd_manager->Read(&m_vfd, &row_length, sizeof(row_length))) {
    my_error(ER_TEMP_FILE_WRITE_FAILURE, MYF(0));
    return true;
  }

  // Read the actual data of the row.
  if (buffer->reserve(row_length)) {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), row_length);
    return true;
  }

  buffer->length(row_length);
  if (m_vfd_manager->Read(&m_vfd, buffer->ptr(), row_length)) {
    my_error(ER_TEMP_FILE_WRITE_FAILURE, MYF(0));
    return true;
  }

  hash_join_buffer::LoadBufferRowIntoTableBuffers(
      tables, {buffer->ptr(), buffer->length()});
  return false;
}
