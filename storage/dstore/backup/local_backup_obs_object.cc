/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v2. You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 * KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the
 * Mulan PSL v2 for more details.
 * ---------------------------------------------------------------------------------------
 *
 * local_backup_mgr.cc
 *
 *
 *
 * IDENTIFICATION
 *        storage/dstore/backup/local_backup_obs_object.cc
 *
 * ---------------------------------------------------------------------------------------
 */
#include "common/cde_def.h"
#include "local_backup_file_mgr.h"

namespace CDE {

bool LocalBackupObsObject::OpenFile(bool create [[maybe_unused]],
                                    bool readOnly) {
  if (nullptr == m_handler) {
    if (GetLbObsObjectHandle(m_fileName)) {
      return true;
    }
  }
  m_readOnly = readOnly;
  return false;
}

bool LocalBackupObsObject::CloseFile() {
  if (!m_readOnly) {
    Flush();
  }
  if (!(m_writeFlag & NO_NORMAL_FREE_HANDLE)) {
    FreeLbObsObjectHandle();
  }
  return false;
}

bool LocalBackupObsObject::TruncateFile(uint64_t size) {
  if (nullptr == m_handler) {
    return true;
  }
  if (m_writeFlag & SINGLE_OBJECT_MODLE) {
    if ((0 == m_handler->m_buffer_offset) ||
        (m_handler->m_buffer_offset < size)) {
      m_handler->m_buffer_offset = 0;
      if (m_handler->get_full_object()) {
        FreeLbObsObjectHandle();
        return true;
      }
    }

    m_handler->m_buffer_offset = size;
    std::string oldName = m_handler->get_name();
    if (m_handler->upgrade_name_from_version()) {
      return true;
    }
    if (m_handler->put_object()) {
      FreeLbObsObjectHandle();
      return true;
    }
    if (m_handler->remove_object(&oldName)) {
      FreeLbObsObjectHandle();
      return true;
    }
    return false;
  }

  return true;
}

bool LocalBackupObsObject::GetFileSize(uint64_t &size) {
  if (nullptr == m_handler) {
    return true;
  }
  if (m_handler->get_object_length(size)) {
    FreeLbObsObjectHandle();
    return true;
  }
  return false;
}

bool LocalBackupObsObject::WriteSimple(void *buffer, uint64_t dataLength,
                                       uint64_t offset) {
  if (nullptr == m_handler) {
    return true;
  }

  uint8_t *obsBuf = m_handler->m_buffer;
  uint64_t maxBufLen = m_handler->m_buffer_len;
  error_t rc = 0;
  if (m_writeFlag & SINGLE_OBJECT_MODLE) {
    if ((m_writeFlag & SINGLE_OBJECT_EXISTENCE) &&
        (0 == m_handler->m_buffer_offset)) {
      if (m_handler->get_full_object()) {
        FreeLbObsObjectHandle();
        return true;
      }
    }

    if (LB_INVALID_OFFSET == offset) {
      offset = m_handler->m_buffer_offset;
    }

    uint64_t writeOffset = offset + dataLength;
    /* The size of lb meta file can not over the buffer size */
    if (writeOffset > m_handler->m_buffer_len) {
      return true;
    }
    rc = memcpy_s((obsBuf + offset), (maxBufLen - offset),
                  static_cast<uint8_t *>(buffer), dataLength);
    // LCOV_EXCL_START
    if (rc != 0) {
      return true;
    }
    // LCOV_EXCL_STOP
    if (writeOffset > m_handler->m_buffer_offset) {
      m_handler->m_buffer_offset = writeOffset;
    }
    std::string oldName;
    if (m_writeFlag & SINGLE_OBJECT_EXISTENCE) {
      oldName = m_handler->get_name();
      if (m_handler->upgrade_name_from_version()) {
        return true;
      }
    }
    if (m_handler->put_object()) {
      FreeLbObsObjectHandle();
      return true;
    }
    if (m_writeFlag & SINGLE_OBJECT_EXISTENCE) {
      if (m_handler->remove_object(&oldName)) {
        return true;
      }
    } else {
      m_writeFlag |= SINGLE_OBJECT_EXISTENCE;
    }
  } else {
    uint64_t writeLen = dataLength;
    uint8_t *inputBuf = static_cast<uint8_t *>(buffer);
    while (writeLen > 0) {
      uint64_t reserverLen = maxBufLen - m_handler->m_buffer_offset;
      if (reserverLen != 0) {
        uint64_t currentWriteLen =
            (writeLen >= reserverLen) ? reserverLen : writeLen;
        rc = memcpy_s(obsBuf + m_handler->m_buffer_offset, currentWriteLen,
                      inputBuf, currentWriteLen);
        // LCOV_EXCL_START
        if (rc != 0) {
          return true;
        }
        // LCOV_EXCL_STOP
        inputBuf += currentWriteLen;
        writeLen -= currentWriteLen;
        m_handler->m_buffer_offset += currentWriteLen;
      }
      if (m_handler->m_buffer_offset == m_handler->m_buffer_len) {
        if (m_handler->append_object()) {
          FreeLbObsObjectHandle();
          return true;
        }
        if (!(m_writeFlag & MULTI_OBJECT_EXISTENCE)) {
          m_writeFlag |= MULTI_OBJECT_EXISTENCE;
        }
      }
    }
  }

  return false;
}

bool LocalBackupObsObject::Flush() {
  bool isError = true;
  if (nullptr == m_handler) {
    return true;
  }
  if (m_writeFlag & SINGLE_OBJECT_MODLE) {
    isError = m_handler->put_object();
  } else {
    isError = m_handler->append_object();
  }
  if (!(m_writeFlag & NO_NORMAL_FREE_HANDLE)) {
    FreeLbObsObjectHandle();
  }
  return isError;
}

bool LocalBackupObsObject::FileExists() {
  /* object creating with put object not like file, so not open file with creat
   */
  return true;
}

bool LocalBackupObsObject::ReadSimple(void *buffer, uint64_t length,
                                      uint64_t offset) {
  if (nullptr == m_handler) {
    return true;
  }

  if (!(m_writeFlag & SINGLE_OBJECT_MODLE)) {
    return true;
  }

  if (m_handler->m_buffer_offset < (offset + length)) {
    m_handler->m_buffer_offset = 0;
    if (m_handler->get_full_object()) {
      FreeLbObsObjectHandle();
      return true;
    }
  }
  error_t rc =
      memcpy_s((uint8_t *)buffer, length, m_handler->m_buffer + offset, length);
  // LCOV_EXCL_START
  if (rc != 0) {
    return true;
  }
  // LCOV_EXCL_STOP
  return false;
}

bool LocalBackupObsObject::GetLbObsObjectHandle(std::string &name) {
  if (nullptr != m_handler) {
    m_handler->clear();
  } else {
    m_handler = fetch_lb_object_handler();
    if (nullptr == m_handler) {
      return true;
    }
  }

  uint32_t version = 0;
  if (m_writeFlag & SINGLE_OBJECT_MODLE) {
    /* Get version for single object. */
    std::vector<std::string> object_list;
    std::string prefix(name);
    prefix.append("+0@");
    if (m_handler->list_object_list(prefix, object_list)) {
      return true;
    }
    if (object_list.size() > 1) {
      return true;
    }
    if (object_list.size() == 1) {
      if (m_handler->get_number(object_list[0], nullptr, &version, nullptr)) {
        return true;
      }
      AddWriteFlag(SINGLE_OBJECT_EXISTENCE);
    }
  }

  uint32_t type = OBS_OBJ_TYPE_ONE_FILE;
  if (m_writeFlag & WITH_SEPERATED_META) {
    type = OBS_OBJ_TYPE_MERGED_FILES_WITH_SEP_META;
  }
  m_handler->make_object_name(name, type, version, 0);

  return false;
}

void LocalBackupObsObject::FreeLbObsObjectHandle() {
  if (nullptr == m_handler) {
    return;
  }
  m_handler->clear();
  come_back_lb_object_handler(m_handler);
  m_handler = nullptr;
}

}  // namespace CDE
