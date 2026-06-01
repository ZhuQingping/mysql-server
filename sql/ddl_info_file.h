/*
   Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/
#ifndef DDL_INFO_FILE_H
#define DDL_INFO_FILE_H

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "mysql/psi/mysql_mutex.h"
#include "securec.h"
#include "sql/current_thd.h"
#include "sql/handler.h"
#include "sql/log.h"
#include "sql/mysqld.h"    // rds_dstore_support_binlog_check
#include "sql/rpl_gtid.h"  // global_gtid_mode, Gtid_mode

enum RecordOpErrCode {
  RECORD_OP_SUCCESS = 0,
  RECORD_OP_NEED_MORE_DATA = -1,
  RECORD_OP_IO_FAILED = -2,
  RECORD_OP_BUFFER_TOO_SMALL = -3,
  RECORD_OP_INVALID_FORMAT = -4,
  RECORD_OP_END_OF_FILE = -5
};

struct DDLInfoRecord {
  struct DDLInfoRecordHeader {
    uint64_t m_lsn;
    /* Total size of the record: sizeof(DDLInfoRecordHeader) + length of m_data
     */
    uint32_t m_size;
    uint64_t m_xid;
    /* Use an 8-bit XOR checksum for simple. */
    uint8_t m_headerChecksum;
    uint32_t m_dataCrc32;
  } __attribute__((packed));
  // m_lsn, m_size and m_xid will covered by m_headerChecksum check.
  static constexpr size_t HEADER_CHKSUM_COVERED_SIZE =
      sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint64_t);

  DDLInfoRecordHeader m_header;
  const uint8_t *m_data;

  DDLInfoRecord() : m_header{0, 0, 0, 0, 0}, m_data(nullptr) {}

  DDLInfoRecord(uint64_t lsn, uint64_t xid, const uint8_t *data,
                uint32_t dataSize)
      : m_header{lsn,
                 static_cast<uint32_t>(sizeof(DDLInfoRecordHeader) + dataSize),
                 xid, 0, 0},
        m_data(data) {
    m_header.m_dataCrc32 = calcDataCrc32();
    m_header.m_headerChecksum = calcHeaderChecksum();
  }

  uint8_t calcHeaderChecksum() const {
    const uint8_t *p = (const uint8_t *)&m_header;
    uint8_t checksum = 0;

    for (size_t i = 0; i < HEADER_CHKSUM_COVERED_SIZE; i++) {
      checksum ^= p[i];
    }
    return checksum;
  }

  uint32_t calcDataCrc32() const {
    if (dataSize() == 0 || m_data == nullptr) {
      return 0;
    }
    return crc32(0, m_data, dataSize());
  }

  bool verifyHeaderChecksum() const {
    return m_header.m_headerChecksum == calcHeaderChecksum();
  }

  bool verifyDataCrc32() const {
    return m_header.m_dataCrc32 == calcDataCrc32();
  }

  RecordOpErrCode verify() const {
    if (!verifyHeaderChecksum() || !verifyDataCrc32()) {
      return RECORD_OP_INVALID_FORMAT;
    }
    return RECORD_OP_SUCCESS;
  }

  uint32_t dataSize() const {
    if (m_header.m_size < sizeof(DDLInfoRecordHeader)) {
      return 0;
    }
    return m_header.m_size - sizeof(DDLInfoRecordHeader);
  }

  uint32_t size() const { return m_header.m_size; }

  uint64_t lsn() const { return m_header.m_lsn; }

  uint64_t xid() const { return m_header.m_xid; }

  const uint8_t *data() const { return m_data; }

  RecordOpErrCode serialize(std::vector<uint8_t> &buf) const;

  RecordOpErrCode deserialize(const uint8_t *buffer, uint32_t bufferSize);

  void debugDump() const;
};

struct GtidInfoRecord {
  struct GtidInfoRecordHeader {
    /* Total size of the record: sizeof(GtidInfoRecordHeader) + length of m_data
     */
    uint32_t m_size;
    uint8_t m_headerChecksum;
    uint32_t m_dataCrc32;
  } __attribute__((packed));
  static constexpr size_t HEADER_CHKSUM_COVERED_SIZE = sizeof(uint32_t);
  GtidInfoRecordHeader m_header;
  const uint8_t *m_data;
  GtidInfoRecord() : m_header{0, 0, 0}, m_data(nullptr) {}
  GtidInfoRecord(const uint8_t *data, uint32_t dataSize)
      : m_header{static_cast<uint32_t>(sizeof(GtidInfoRecordHeader) + dataSize),
                 0, 0},
        m_data(data) {
    m_header.m_dataCrc32 = calcDataCrc32();
    m_header.m_headerChecksum = calcHeaderChecksum();
  }
  uint8_t calcHeaderChecksum() const {
    const uint8_t *p = (const uint8_t *)&m_header;
    uint8_t checksum = 0;

    for (size_t i = 0; i < HEADER_CHKSUM_COVERED_SIZE; i++) {
      checksum ^= p[i];
    }
    return checksum;
  }

  uint32_t calcDataCrc32() const {
    if (dataSize() == 0 || m_data == nullptr) {
      return 0;
    }
    return crc32(0, m_data, dataSize());
  }

  bool verifyHeaderChecksum() const {
    return m_header.m_headerChecksum == calcHeaderChecksum();
  }

  bool verifyDataCrc32() const {
    return m_header.m_dataCrc32 == calcDataCrc32();
  }

  RecordOpErrCode verify() const {
    if (!verifyHeaderChecksum() || !verifyDataCrc32()) {
      return RECORD_OP_INVALID_FORMAT;
    }
    return RECORD_OP_SUCCESS;
  }

  uint32_t dataSize() const {
    if (m_header.m_size < sizeof(GtidInfoRecordHeader)) {
      return 0;
    }
    return m_header.m_size - sizeof(GtidInfoRecordHeader);
  }

  uint32_t size() const { return m_header.m_size; }

  const uint8_t *data() const { return m_data; }

  RecordOpErrCode serialize(std::vector<uint8_t> &buf) const;

  RecordOpErrCode deserialize(const uint8_t *buffer, uint32_t bufferSize);

  void debugDump() const;
};

namespace serialization_traits {
template <typename T>
struct has_serialize {
  template <typename U>
  static auto test(int) -> decltype(
      std::declval<U>().serialize(std::declval<std::vector<uint8_t> &>()),
      std::true_type{});
  template <typename>
  static std::false_type test(...);
  static constexpr bool value = decltype(test<T>(0))::value;
};

template <typename T>
struct has_size {
  template <typename U>
  static auto test(int) -> decltype(std::declval<U>().size(), std::true_type{});
  template <typename>
  static std::false_type test(...);
  static constexpr bool value = decltype(test<T>(0))::value;
};

template <typename T>
struct has_deserialize {
  template <typename U>
  static auto test(int)
      -> decltype(std::declval<U>().deserialize(std::declval<const uint8_t *>(),
                                                std::declval<size_t>()),
                  std::true_type{});
  template <typename>
  static std::false_type test(...);
  static constexpr bool value = decltype(test<T>(0))::value;
};
}  // namespace serialization_traits

/** Note: Operations in BasicRecordFileStorage can only work under single thread
 mode. And for a work loop, we can only do getReaderPtr() when all
 Writer::append() operations are complete. */
template <typename RecordType>
class BasicRecordFileStorage {
  static_assert(serialization_traits::has_serialize<RecordType>::value,
                "RecordType must have serialize() method");
  static_assert(serialization_traits::has_size<RecordType>::value,
                "RecordType must have size() method");
  static_assert(serialization_traits::has_deserialize<RecordType>::value,
                "RecordType must have deserialize() method");

 private:
  static constexpr const char *MAGIC_STRING = "DDLLOG01";
  static constexpr uint32_t CURRENT_VERSION = 1;
  static const uint32_t BATCH_SYNC_THRESHOLD = 1000;

  static constexpr size_t MAGIC_SIZE = 8;
  static constexpr size_t HEADER_TOTAL_SIZE = 512;
  static constexpr size_t FIXED_FIELDS_SIZE =
      sizeof(uint32_t) * 2 + sizeof(uint64_t) * 2 + sizeof(uint32_t);
  static constexpr size_t RESERVED_SIZE =
      HEADER_TOTAL_SIZE - MAGIC_SIZE - FIXED_FIELDS_SIZE;

  struct FileHeader {
    char m_magic[MAGIC_SIZE];
    uint32_t m_version;
    uint32_t m_recordCount;
    uint64_t m_lastRecordPos;
    uint64_t m_validFileSize;
    uint32_t m_crc32;
    char m_reserved[RESERVED_SIZE];
  } __attribute__((packed));
  static_assert(sizeof(FileHeader) == HEADER_TOTAL_SIZE,
                "Header must be exactly 512 bytes");

  FileHeader m_fileHeader;
  std::string m_fileName;
  int m_fd;
  uint32_t m_pendingRecords;

  void InitFileHeader() {
    memcpy_s(m_fileHeader.m_magic, 8, MAGIC_STRING, 8);
    m_fileHeader.m_version = CURRENT_VERSION;
    m_fileHeader.m_recordCount = 0;
    m_fileHeader.m_lastRecordPos = 0;
    m_fileHeader.m_validFileSize = sizeof(FileHeader);
    m_fileHeader.m_crc32 = 0;
    memset_s(m_fileHeader.m_reserved, sizeof(m_fileHeader.m_reserved), 0,
             sizeof(m_fileHeader.m_reserved));
  }

  uint32_t calcHeaderCrc(const FileHeader &header) {
    FileHeader tempHeader = header;
    tempHeader.m_crc32 = 0;
    return crc32(0, reinterpret_cast<const Bytef *>(&tempHeader),
                 sizeof(tempHeader));
  }

  RecordOpErrCode createNewFile() {
    InitFileHeader();
    m_fileHeader.m_crc32 = calcHeaderCrc(m_fileHeader);

    int fd = ::open(m_fileName.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    const char *errMsg = strerror(errno);
    if (fd < 0) {
      sql_print_error("Failed to open file %s, error: %s", m_fileName.c_str(),
                      errMsg);
      return RECORD_OP_IO_FAILED;
    }

    ssize_t written = ::write(fd, reinterpret_cast<const char *>(&m_fileHeader),
                              sizeof(m_fileHeader));
    if (written != sizeof(m_fileHeader)) {
      ::close(fd);
      return RECORD_OP_IO_FAILED;
    }

    if (fsync(fd) != 0) {
      ::close(fd);
      return RECORD_OP_IO_FAILED;
    }

    ::close(fd);
    return RECORD_OP_SUCCESS;
  }

  RecordOpErrCode loadFileHeader() {
    if (lseek(m_fd, 0, SEEK_SET) != 0) {
      return RECORD_OP_IO_FAILED;
    }

    ssize_t bytesRead = ::read(m_fd, &m_fileHeader, sizeof(FileHeader));
    if (bytesRead != sizeof(FileHeader)) {
      return RECORD_OP_IO_FAILED;
    }

    if (memcmp(m_fileHeader.m_magic, MAGIC_STRING, 8) != 0) {
      return RECORD_OP_INVALID_FORMAT;
    }

    if (m_fileHeader.m_version > CURRENT_VERSION) {
      return RECORD_OP_INVALID_FORMAT;
    }

    uint32_t expectedCrc = calcHeaderCrc(m_fileHeader);
    if (m_fileHeader.m_crc32 != expectedCrc) {
      return RECORD_OP_INVALID_FORMAT;
    }

    return RECORD_OP_SUCCESS;
  }

  RecordOpErrCode openNewFile() {
    RecordOpErrCode result = createNewFile();
    if (result != RECORD_OP_SUCCESS) {
      return result;
    }

    m_fd = ::open(m_fileName.c_str(), O_RDWR);
    if (m_fd < 0) {
      sql_print_error("Failed to open file %s, error: %s", m_fileName.c_str(),
                      strerror(errno));
      return RECORD_OP_IO_FAILED;
    }

    result = loadFileHeader();
    if (result != RECORD_OP_SUCCESS) {
      ::close(m_fd);
      m_fd = -1;
    }
    return result;
  }

  RecordOpErrCode updateFileHeader() {
    m_fileHeader.m_crc32 = calcHeaderCrc(m_fileHeader);

    if (lseek(m_fd, 0, SEEK_SET) != 0) {
      return RECORD_OP_IO_FAILED;
    }

    ssize_t written = ::write(m_fd, &m_fileHeader, sizeof(FileHeader));
    if (written != sizeof(FileHeader) || fsync(m_fd) != 0) {
      return RECORD_OP_IO_FAILED;
    }

    return RECORD_OP_SUCCESS;
  }

  RecordOpErrCode append_record(std::vector<uint8_t> &record, bool sync) {
    if (lseek(m_fd, m_fileHeader.m_validFileSize, SEEK_SET) !=
        static_cast<off_t>(m_fileHeader.m_validFileSize)) {
      return RECORD_OP_IO_FAILED;
    }

    ssize_t written = ::write(m_fd, record.data(), record.size());
    if (written != static_cast<ssize_t>(record.size())) {
      return RECORD_OP_IO_FAILED;
    }

    m_fileHeader.m_recordCount++;
    m_fileHeader.m_lastRecordPos = m_fileHeader.m_validFileSize;
    m_fileHeader.m_validFileSize += record.size();
    m_pendingRecords++;

    if (m_pendingRecords >= BATCH_SYNC_THRESHOLD || sync) {
      return this->sync();
    }

    return RECORD_OP_SUCCESS;
  }

 public:
  explicit BasicRecordFileStorage(const std::string &fileName)
      : m_fileName(fileName), m_fd(-1), m_pendingRecords(0) {}

  ~BasicRecordFileStorage() {
    if (m_fd >= 0) {
      close();
    }
  }
  std::string get_file_name() { return m_fileName; }

  RecordOpErrCode open() {
    if (m_fd >= 0) {
      return RECORD_OP_SUCCESS;
    }

    /* unlike the file if it's empty. */
    struct stat fileStat;
    if (stat(m_fileName.c_str(), &fileStat) == 0 && fileStat.st_size == 0) {
      if (clean() != RECORD_OP_SUCCESS) {
        return RECORD_OP_IO_FAILED;
      }
    }

    m_fd = ::open(m_fileName.c_str(), O_RDWR);
    if (m_fd >= 0) {
      RecordOpErrCode result = loadFileHeader();
      if (result != RECORD_OP_SUCCESS) {
        sql_print_error("Failed to open file %s, error: %s", m_fileName.c_str(),
                        strerror(errno));
        ::close(m_fd);
        m_fd = -1;
        std::time_t t = std::time(nullptr);
        std::string backFileName = m_fileName + "_" + std::to_string(t);
        if (std::rename(m_fileName.c_str(), backFileName.c_str()) == 0) {
          sql_print_error("Backup file %s to %s", m_fileName.c_str(),
                          backFileName.c_str());
          return openNewFile();
        } else {
          sql_print_error("Failed to rename file %s -> %s (errno %d)",
                          m_fileName.c_str(), backFileName.c_str(), errno);
        }
      }
      return result;
    } else {
      return openNewFile();
    }
  }

  RecordOpErrCode sync() {
    if (m_fd < 0) {
      return RECORD_OP_IO_FAILED;
    }

    if (fsync(m_fd) != 0) {
      return RECORD_OP_IO_FAILED;
    }

    RecordOpErrCode result = updateFileHeader();
    if (result != RECORD_OP_SUCCESS) {
      return result;
    }
    m_pendingRecords = 0;
    return RECORD_OP_SUCCESS;
  }

  RecordOpErrCode close() {
    if (m_fd >= 0) {
      RecordOpErrCode result = sync();
      ::close(m_fd);
      m_fd = -1;
      return result;
    }
    return RECORD_OP_SUCCESS;
  }

  RecordOpErrCode clean() {
    close();
    if (unlink(m_fileName.c_str()) != 0) {
      if (errno != ENOENT) {
        sql_print_error("Failed to unlink file %s, error: %s",
                        m_fileName.c_str(), strerror(errno));
        return RECORD_OP_IO_FAILED;
      }
    }
    return RECORD_OP_SUCCESS;
  }

  bool is_exist() {
    bool res = true;
    if (::access(m_fileName.c_str(), F_OK) != 0) {
      res = false;
      sql_print_information("File '%s' is not existent", m_fileName.c_str());
    }
    return res;
  }

  bool has_record() {
    bool res = true;
    struct stat fileStat;
    if (stat(m_fileName.c_str(), &fileStat) == 0 &&
        fileStat.st_size <= static_cast<off_t>(HEADER_TOTAL_SIZE)) {
      sql_print_warning("File '%s' has no record.", m_fileName.c_str());
      res = false;
    }
    return res;
  }

  class Reader {
   private:
    BasicRecordFileStorage &m_storage;
    uint64_t m_filePos;
    uint32_t m_currentRecordIndex;
    uint32_t m_totalRecords;
    std::vector<uint8_t> m_buffer;
    size_t m_bufferSize;
    size_t m_dataStart;
    size_t m_dataEnd;

    RecordOpErrCode fillBuffer() {
      size_t availableData = m_dataEnd - m_dataStart;

      if (m_dataStart > 0 && availableData > 0) {
        memmove_s(m_buffer.data(), availableData, m_buffer.data() + m_dataStart,
                  availableData);
        m_dataEnd = availableData;
        m_dataStart = 0;
      }

      size_t spaceAvailable = m_bufferSize - m_dataEnd;
      if (spaceAvailable == 0) {
        return RECORD_OP_BUFFER_TOO_SMALL;
      }

      uint64_t remainingFileSize =
          m_storage.m_fileHeader.m_validFileSize - m_filePos;
      size_t readSize =
          std::min(spaceAvailable, static_cast<size_t>(remainingFileSize));

      if (readSize == 0) {
        return RECORD_OP_END_OF_FILE;
      }

      if (lseek(m_storage.m_fd, m_filePos, SEEK_SET) !=
          static_cast<off_t>(m_filePos)) {
        return RECORD_OP_IO_FAILED;
      }

      ssize_t bytesRead =
          ::read(m_storage.m_fd, m_buffer.data() + m_dataEnd, readSize);
      if (bytesRead <= 0) {
        sql_print_error("Failed to read record from file, error: %s",
                        strerror(errno));
        return RECORD_OP_IO_FAILED;
      }

      m_dataEnd += bytesRead;
      m_filePos += bytesRead;

      return RECORD_OP_SUCCESS;
    }

    RecordOpErrCode ensureDataAvailable(size_t minSize) {
      size_t availableData = m_dataEnd - m_dataStart;

      if (availableData >= minSize) {
        return RECORD_OP_SUCCESS;
      }

      return fillBuffer();
    }

   public:
    explicit Reader(BasicRecordFileStorage &storage,
                    size_t bufferSize = 64 * 1024)
        : m_storage(storage),
          m_filePos(sizeof(FileHeader)),
          m_currentRecordIndex(0),
          m_totalRecords(0),
          m_buffer(bufferSize),
          m_bufferSize(bufferSize),
          m_dataStart(0),
          m_dataEnd(0) {}

    RecordOpErrCode init() {
      if (m_storage.m_fd < 0) {
        return RECORD_OP_IO_FAILED;
      }

      m_totalRecords = m_storage.m_fileHeader.m_recordCount;
      m_filePos = sizeof(FileHeader);
      m_currentRecordIndex = 0;
      m_dataStart = 0;
      m_dataEnd = 0;

      return RECORD_OP_SUCCESS;
    }

    RecordOpErrCode get_next(RecordType &record) {
      if (m_currentRecordIndex >= m_totalRecords) {
        return RECORD_OP_END_OF_FILE;
      }

      RecordOpErrCode result = ensureDataAvailable(sizeof(record));
      if (result != RECORD_OP_SUCCESS && result != RECORD_OP_END_OF_FILE) {
        return result;
      }

      while (true) {
        size_t availableData = m_dataEnd - m_dataStart;

        result =
            record.deserialize(m_buffer.data() + m_dataStart, availableData);

        if (result == RECORD_OP_SUCCESS) {
          m_dataStart += record.size();
          m_currentRecordIndex++;
          return RECORD_OP_SUCCESS;
        } else if (result == RECORD_OP_NEED_MORE_DATA) {
          result = fillBuffer();
          if (result != RECORD_OP_SUCCESS) {
            return result;
          }
        } else {
          return result;
        }
      }
    }

    bool has_more_records() const {
      return m_currentRecordIndex < m_totalRecords;
    }

    uint32_t get_total_records() const { return m_totalRecords; }

    uint32_t get_current_index() const { return m_currentRecordIndex; }
  };

  class Writer {
   private:
    BasicRecordFileStorage &m_storage;

   public:
    explicit Writer(BasicRecordFileStorage &storage) : m_storage(storage) {}

    RecordOpErrCode append(const RecordType &record, bool sync = false) {
      std::vector<uint8_t> serializedData;
      serializedData.resize(record.size());
      record.serialize(serializedData);
      auto result = m_storage.append_record(serializedData, sync);
      if (result != RECORD_OP_SUCCESS) {
        sql_print_error("Failed to append record to file, error: %s",
                        strerror(errno));
      }
      return result;
    }
  };

  std::unique_ptr<Reader> createReaderPtr(size_t bufferSize = 1024 * 1024) {
    sync();
    return m_fd < 0 ? nullptr : std::make_unique<Reader>(*this, bufferSize);
  }

  std::unique_ptr<Writer> createWriterPtr() {
    return m_fd < 0 ? nullptr : std::make_unique<Writer>(*this);
  }

  uint32_t getRecordCount() const { return m_fileHeader.m_recordCount; }

  uint64_t getFileSize() const { return m_fileHeader.m_validFileSize; }

  bool isOpen() const { return m_fd >= 0; }
};

using DDLInfoFileStorage = BasicRecordFileStorage<DDLInfoRecord>;
extern Xid_commit_list g_restore_xid_map;
extern DDLInfoFileStorage g_ddl_info_store_file;

/* Error code for DDL info operation. */
enum DDL_INFO_OP_ERROR_CODE {
  DDL_INFO_OP_SUCCESS = 0,
  DDL_INFO_FILE_OP_FAILED = -1,
  DDL_INFO_READ_OP_FAILED = -2,
  DDL_INFO_WRITE_OP_FAILED = -3
};

/**
 @brief Try to read XIDs from existed ddl_info_store_file.dat

 @retval DDL_INFO_OP_SUCCESS restore success.
 @retval DDL_INFO_FILE_OP_FAILED cannot open ddl info file.
 @retval DDL_INFO_READ_OP_FAILED failed to read ddl info.
 */
DDL_INFO_OP_ERROR_CODE restore_xid_from_file_store();

/**
 @brief Write a DDL info (from WAL scanning) to local file.

 This function will firstly try to open existed DDL info file and
 restore it's XIDs. After that it will add the DDL info to the file.

 @param end_lsn End LSN of the WAL segment containing the DDL info.
 @param buffer DDL SQL statement buffer.
 @param buffer_size Length of the DDL SQL buffer; 0 if no DDL SQL info.
 @param xid XID of the DDL commit; 0 if no XID info.

 @retval DDL_INFO_OP_SUCCESS save DDL info success.
 @retval DDL_INFO_FILE_OP_FAILED cannot open ddl info file.
 @retval DDL_INFO_WRITE_OP_FAILED failed to save ddl info.
 */
DDL_INFO_OP_ERROR_CODE put_ddl_info(uint64_t end_lsn,
                                    const unsigned char *buffer,
                                    uint32_t buffer_size, uint64_t xid);
void rm_ddl_info();

using GtidInfoFileStorage = BasicRecordFileStorage<GtidInfoRecord>;
extern GtidInfoFileStorage g_gtid_info_store_file;

/**
 * Persists the GTID set from WAL (Write-Ahead Log) into file.
 *
 * This function writes the current GTID set to a file for durability. It first
 * checks whether the file can be opened successfully. If the GTID set is empty,
 * it logs an informational message and returns early without writing.
 *
 * @param gtid_set Pointer to the GTID set to be persisted.
 * @param need_lock Whether to acquire the global SID lock during string
 * conversion
 */
void persist_gtids_in_wal_to_file(const Gtid_set *gtid_set,
                                  bool need_lock = true);

/**
 * Reads GTID set from  files.
 *
 * This function attempts to load the GTID set from the store file.
 * If the primary file is unavailable or empty, it falls back to the backup
 * file.
 * @return A pointer to a Gtid_set containing the loaded GTIDs, or nullptr if
 * both files are non-existent or empty.
 */
Gtid_set *read_gtids_in_wal_from_file();

/**
 * Reads GTID set from a persistent file.
 *
 * This function opens the specified GtidInfoFileStorage file, reads the first
 * GtidInfoRecord containing serialized GTID information, and reconstructs a
 * Gtid_set object using the provided SID map. The file is closed immediately
 * after reading the single record. If successful, the constructed Gtid_set is
 * returned; otherwise, appropriate error messages are logged and nullptr is
 * returned.
 *
 * @param file Pointer to the GtidInfoFileStorage instance representing the file
 *             from which GTIDs are to be loaded.
 * @param[out] error_out Boolean indicating if the operation was successful
 (false) or encountered issues (true)
 * @return A dynamically allocated Gtid_set object containing the GTIDs read
 * from the file on success; nullptr on failure (e.g., file I/O error, parsing
 * error).
 *
 */

Gtid_set *read_gtids_from_file(GtidInfoFileStorage *file, bool &error_out);

/**
 @brief DDL transaction flushed LSN recycling point manager

 This class maintains a mapping of active DDL transactions to their
 corresponding LSNs and tracks the minimum recyclable LSN position for safe log
 cleanup.
 */
class DDLFlushLsnManager {
 public:
  DDLFlushLsnManager() {}
  ~DDLFlushLsnManager() {}

  /// Disable copy constructor and assignment operator
  DDLFlushLsnManager(const DDLFlushLsnManager &) = delete;
  DDLFlushLsnManager &operator=(const DDLFlushLsnManager &) = delete;

  /// Disable move constructor and assignment operator
  DDLFlushLsnManager(DDLFlushLsnManager &&) = delete;
  DDLFlushLsnManager &operator=(DDLFlushLsnManager &&) = delete;

  /**
   @brief Called before DDL transaction commit to DStore engine.

   Registers the DDL transaction and updates recyclable point if necessary.
   If this is the first active DDL, sets the recyclable LSN to the latest
   WAL stream flush LSN directly.

   @param hton handlerton used to mark the LSN info, only DStore supported
   currently.
   @param xid DDL transaction XID.
   */
  void markDdlCommitStart(handlerton *hton, uint64_t xid) {
    std::lock_guard<std::mutex> guard(m_mutex);

    // By design, hton->get_current_flushed_plsn cannot fail.
    uint64_t flushPlsn = hton->get_current_flushed_plsn();
    bool wasEmpty = m_activeDdlMap.empty();
    // Add to map
    m_activeDdlMap[xid] = flushPlsn;
    m_lsnXidSet.insert({flushPlsn, xid});
    /* The second condition of the && ensures that a recovery point can only
    be set here if it hasn't already been set in MYSQL_BIN_LOG::order_commit. */
    if (wasEmpty &&
        (!opt_bin_log ||
         (opt_bin_log &&
          !(rds_dstore_support_binlog_check && !rds_write_binlog_into_redo &&
            global_gtid_mode.get() == Gtid_mode::ON)))) {
      sql_print_information(
          "DDL crash safe: Transaction with XID %llu start commit and set "
          "DStore flush plsn to %llu",
          xid, flushPlsn);
      hton->set_recovery_plsn_for_taurus(flushPlsn);
    }
  }

  /**
   @brief Called after DDL transaction's InnoDB transaction completes

   Unregisters the DDL transaction and updates recyclable point if the removed
   transaction was determining the current recyclable LSN.

   @param hton handlerton used to mark the LSN info, only DStore supported
   currently.
   @param xid DDL transaction XID.
   */
  void markDdlCommitFinish(handlerton *hton, uint64_t xid) {
    std::lock_guard<std::mutex> guard(m_mutex);
    bool isMinLsn = false;
    auto it = m_activeDdlMap.find(xid);
    // For simplicity, we tolerate attempts to remove a non-existent XID
    // which could happen during instance initialization.
    if (it != m_activeDdlMap.end()) {
      if (it->second == m_lsnXidSet.begin()->first) {
        isMinLsn = true;
      }
      m_lsnXidSet.erase({it->second, xid});
      m_activeDdlMap.erase(it);
    }

    /* This conditon ensures that a recovery point can only be set here if it
    hasn't already been set in MYSQL_BIN_LOG::order_commit. */
    if (isMinLsn &&
        (!opt_bin_log ||
         (opt_bin_log &&
          !(rds_dstore_support_binlog_check && !rds_write_binlog_into_redo &&
            global_gtid_mode.get() == Gtid_mode::ON)))) {
      uint64_t flushPlsn = m_lsnXidSet.empty()
                               ? std::numeric_limits<uint64_t>::max()
                               : m_lsnXidSet.begin()->first;
      sql_print_information(
          "DDL crash safe: Transaction with XID %llu finish commit and set "
          "DStore flush plsn to %llu",
          xid, flushPlsn);
      hton->set_recovery_plsn_for_taurus(flushPlsn);
    }
  }

 private:
  /// Mutex protecting all member variables
  std::mutex m_mutex;

  /// Map of active DDL transactions: transaction XID -> LSN
  std::unordered_map<uint64_t, uint64_t> m_activeDdlMap;

  /// Set of (LSN, XID) pairs: automatically sorted by LSN, then XID
  /// Guarantees uniqueness and natural ordering
  std::set<std::pair<uint64_t, uint64_t>> m_lsnXidSet;
};

extern DDLFlushLsnManager g_ddl_flush_lsn_mgr;
#endif  // DDL_INFO_FILE_H
