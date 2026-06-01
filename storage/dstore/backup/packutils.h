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
 * packutils.h
 *
 *
 *
 * IDENTIFICATION
 *        storage/dstore/backup/packutils.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef PACK_UTILS_H
#define PACK_UTILS_H

#include <cstdint>
#include <vector>

#include <zlib.h>
#include "common/cde_def.h"

#ifndef byte
#define byte unsigned char
#endif

#define PACKHEADER_BYTE_SIZE 10
struct __attribute__((packed)) PackHeaderCrc {
 private:
  uint32_t _crc;
  uint16_t _version;
  uint32_t _size;
  uint32_t _reserved;

 public:
  PackHeaderCrc() : _crc(0), _version(0), _size(0), _reserved(0) {}
  explicit PackHeaderCrc(uint16_t version, uint32_t size)
      : _crc(0), _version(version), _size(size), _reserved(0) {}

  inline void setChecksum(uint32_t crc) { _crc = crc; }
  inline uint32_t getChecksum() { return _crc; }
  inline void setVersion(uint16_t version) { _version = version; }
  inline uint16_t getVersion() { return _version; }

  inline void setSize(uint32_t size) { _size = size; }
  inline uint32_t getSize() { return _size; }

  inline void setReserved(uint32_t reserved) { _reserved = reserved; }
  inline uint32_t getReserved() { return _reserved; }
};

const uint32_t PACKHEADERCRC_BYTE_SIZE = sizeof(PackHeaderCrc);

class PackBuffer {
 private:
  byte *_packbuf;
  uint32_t _size;
  byte *_curpos;
  byte *_prevpos;
  PackBuffer()
      : _packbuf(nullptr), _size(0), _curpos(nullptr), _prevpos(nullptr) {}

 public:
  PackBuffer(byte *buf, uint32_t size)
      : _packbuf(buf), _size(size), _curpos(buf), _prevpos(buf) {}

  ~PackBuffer() {
    _packbuf = nullptr;
    _curpos = nullptr;
    _prevpos = nullptr;
  }

  inline uint32_t get_size() const { return _size; }

  inline void write_bytes_2(uint16_t n) {
    _curpos[0] = (byte)(n >> 8);
    _curpos[1] = (byte)(n);
    _curpos += 2;
  }

  inline uint16_t read_bytes_2() {
    uint16_t ret = ((uint16_t)(_curpos[0]) << 8) | (uint16_t)(_curpos[1]);
    _curpos += 2;
    return ret;
  }

  inline void write_bytes_4(uint32_t n) {
    _curpos[0] = static_cast<byte>(n >> 24);
    _curpos[1] = static_cast<byte>(n >> 16);
    _curpos[2] = static_cast<byte>(n >> 8);
    _curpos[3] = static_cast<byte>(n);
    _curpos += 4;
  }

  inline uint32_t read_bytes_4() {
    uint32_t ret = ((_curpos[0] << 24) | (_curpos[1] << 16) |
                    (_curpos[2] << 8) | _curpos[3]);
    _curpos += 4;
    return ret;
  }

  inline void write_bytes_8(uint64_t n) {
    uint32_t high = (uint32_t)(n >> 32);
    uint32_t low = (uint32_t)n;
    _curpos[0] = static_cast<byte>(high >> 24);
    _curpos[1] = static_cast<byte>(high >> 16);
    _curpos[2] = static_cast<byte>(high >> 8);
    _curpos[3] = static_cast<byte>(high);
    _curpos += 4;

    _curpos[0] = static_cast<byte>(low >> 24);
    _curpos[1] = static_cast<byte>(low >> 16);
    _curpos[2] = static_cast<byte>(low >> 8);
    _curpos[3] = static_cast<byte>(low);
    _curpos += 4;
  }

  inline uint64_t read_bytes_8() {
    uint64_t u64;
    uint32_t high = ((_curpos[0] << 24) | (_curpos[1] << 16) |
                     (_curpos[2] << 8) | _curpos[3]);
    _curpos += 4;
    uint32_t low = ((_curpos[0] << 24) | (_curpos[1] << 16) |
                    (_curpos[2] << 8) | _curpos[3]);
    _curpos += 4;
    u64 = high;
    u64 <<= 32;
    u64 |= low;
    return (u64);
  }

  inline bool write_string(const char *str, uint32_t len) {
    assert(str != nullptr);
    int ret = memcpy_s(_curpos, len, str, len);
    if (ret != EOK) {
      CDE_LOG_ERROR("memcpy_s failed in write_string");
      return true;
    }
    _curpos += len;
    return false;
  }

  inline bool read_string(char *str, uint32_t len) {
    assert(str != nullptr);
    int ret = memcpy_s(str, len, _curpos, len);
    if (ret != EOK) {
      CDE_LOG_ERROR("memcpy_s failed in read_string");
      return true;
    }
    _curpos += len;
    return false;
  }

  inline void write_header(PackHeaderCrc &ph) {
    write_bytes_4(ph.getChecksum());
    write_bytes_2(ph.getVersion());
    write_bytes_4(ph.getSize());
    write_bytes_4(ph.getReserved());
  }

  inline bool read_header(PackHeaderCrc *ph, bool verification = false) {
    // read checksum
    ph->setChecksum(read_bytes_4());
    byte *start = _curpos;
    ph->setVersion(read_bytes_2());
    ph->setSize(read_bytes_4());
    ph->setReserved(read_bytes_4());

    if (verification) {
      // make sure the bound is OK
      if (check_bound(ph->getSize())) {
        return true;
      }

      // back to after checksum
      uint32_t calcCrc = crc32(0, start, PACKHEADER_BYTE_SIZE + ph->getSize());
      if (calcCrc != ph->getChecksum()) {
        std::ostringstream o;
        o << "checksum doesn't match"
          << ". storage checksum= " << ph->getChecksum()
          << ", calculate checksum= " << calcCrc;
        CDE_LOG_ERROR("%s", o.str().c_str());
        return true;
      }
    }
    return false;
  }

  inline bool complete_record() {
    // the concept is similar as database commit concept
    // Once it is called.
    // Calculate the checksum between _prevpos + 4 and _curpos
    // and write checksum at _prevpos
    // after set checksum, set _prevpos as _curpos for next calculation
    if (_prevpos + 4 >= _curpos) {
      std::ostringstream o;
      o << "Pack buffer internal pointer is wrong"
        << ". _curpos - _prevpos = " << _curpos - _prevpos
        << ", it should be greater than 4";
      CDE_LOG_ERROR("%s", o.str().c_str());
      return true;
    }
    uint32_t calcCrc = crc32(0, _prevpos + 4, _curpos - _prevpos - 4);
    _prevpos[0] = static_cast<byte>(calcCrc >> 24);
    _prevpos[1] = static_cast<byte>(calcCrc >> 16);
    _prevpos[2] = static_cast<byte>(calcCrc >> 8);
    _prevpos[3] = static_cast<byte>(calcCrc);
    _prevpos = _curpos;
    return false;
  }

 private:
  inline bool check_bound(const uint32_t mv) {
    if (_packbuf == nullptr || _curpos == nullptr ||
        _curpos - _packbuf + mv > _size) {
      std::ostringstream o;
      o << "Exceeds pack buffer boundary. bufferSize=" << _size
        << ", currentPos=" << (_curpos - _packbuf) << ", movePos=" << mv;
      CDE_LOG_ERROR("%s", o.str().c_str());
      return true;
    }
    return false;
  }
};

#endif
