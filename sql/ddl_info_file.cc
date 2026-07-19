#include "ddl_info_file.h"
#include <assert.h>
#include <cinttypes>
#include <errno.h>
#include "binlog.h"

static const std::string DDL_INFO_STORE_FILE_NAME("ddl_info_store_file.dat");
static const std::string GTIDS_IN_WAL_STORE_FILE_NAME(
    "gtids_in_wal_store_file.data");
static const std::string GTIDS_IN_WAL_STORE_FILE_NAME_BAK(
    "gtids_in_wal_store_file.data.bk");
static MEM_ROOT s_xid_mem_root;
Xid_commit_list g_restore_xid_map{Mem_root_allocator<my_xid>{&s_xid_mem_root}};
DDLInfoFileStorage g_ddl_info_store_file(DDL_INFO_STORE_FILE_NAME);
DDLFlushLsnManager g_ddl_flush_lsn_mgr;
GtidInfoFileStorage g_gtids_in_wal_store_file(GTIDS_IN_WAL_STORE_FILE_NAME);
GtidInfoFileStorage g_gtids_in_wal_store_bak_file(
    GTIDS_IN_WAL_STORE_FILE_NAME_BAK);

DDL_INFO_OP_ERROR_CODE restore_xid_from_file_store() {
  assert(!g_ddl_info_store_file.isOpen());
  if (g_ddl_info_store_file.open() != RECORD_OP_SUCCESS) {
    sql_print_error("Failed to open DDL info storage file.");
    return DDL_INFO_FILE_OP_FAILED;
  }
  std::unique_ptr<BasicRecordFileStorage<DDLInfoRecord>::Reader> reader =
      g_ddl_info_store_file.createReaderPtr();
  reader->init();
  DDLInfoRecord record;
  while (reader->has_more_records()) {
    if (reader->get_next(record) != RECORD_OP_SUCCESS) {
      sql_print_error("Failed to get DDL info record.");
      return DDL_INFO_READ_OP_FAILED;
    }
    sql_print_information(
        "DDL crash safe: got XID %llu from existing XID storage file with End "
        "LSN "
        "%llu.",
        record.xid(), record.lsn());
    if (record.xid() != 0) {
      g_restore_xid_map.insert(record.xid());
    }
  }
  return DDL_INFO_OP_SUCCESS;
}

DDL_INFO_OP_ERROR_CODE put_ddl_info(uint64_t end_lsn,
                                    const unsigned char *buffer,
                                    uint32_t buffer_size, uint64_t xid) {
  /* Nothing need to be written. */
  if (xid == 0 && buffer_size == 0) {
    return DDL_INFO_OP_SUCCESS;
  }
  static std::mutex mtx;
  std::lock_guard<std::mutex> lock(mtx);
  if (xid != 0) {
    g_restore_xid_map.insert(xid);
  }
  sql_print_information(
      "DDL crash safe: got XID %llu from WAL during recovery with End LSN "
      "%llu.",
      xid, end_lsn);
  static std::unique_ptr<BasicRecordFileStorage<DDLInfoRecord>::Writer> writer =
      g_ddl_info_store_file.createWriterPtr();
  DDLInfoRecord record(end_lsn, xid, buffer, buffer_size);
  if (writer->append(record, true) != RECORD_OP_SUCCESS) {
    return DDL_INFO_WRITE_OP_FAILED;
  }
  return DDL_INFO_OP_SUCCESS;
}

void rm_ddl_info() {
  g_restore_xid_map.clear();
  g_ddl_info_store_file.clean();
}

RecordOpErrCode DDLInfoRecord::serialize(std::vector<uint8_t> &buf) const {
  memcpy_s(buf.data(), sizeof(DDLInfoRecordHeader), &m_header,
           sizeof(DDLInfoRecordHeader));

  if (dataSize() > 0 && m_data != nullptr) {
    memcpy_s(buf.data() + sizeof(DDLInfoRecordHeader), dataSize(), m_data,
             dataSize());
  }

  return RECORD_OP_SUCCESS;
}

RecordOpErrCode DDLInfoRecord::deserialize(const uint8_t *buffer,
                                           uint32_t bufferSize) {
  if (buffer == nullptr || bufferSize < sizeof(DDLInfoRecordHeader)) {
    return RECORD_OP_NEED_MORE_DATA;
  }

  memcpy_s(&m_header, sizeof(DDLInfoRecordHeader), buffer,
           sizeof(DDLInfoRecordHeader));

  if (bufferSize < m_header.m_size) {
    return RECORD_OP_NEED_MORE_DATA;
  }

  if (!verifyHeaderChecksum()) {
    return RECORD_OP_INVALID_FORMAT;
  }

  if (dataSize() > 0) {
    m_data = buffer + sizeof(DDLInfoRecordHeader);

    if (!verifyDataCrc32()) {
      return RECORD_OP_INVALID_FORMAT;
    }
  } else {
    m_data = nullptr;
  }

  return RECORD_OP_SUCCESS;
}

void DDLInfoRecord::debugDump() const {
  printf("DDLInfoRecord (Header: %lu bytes):\n", sizeof(DDLInfoRecordHeader));
  printf("  LSN: %" PRIu64 "\n", m_header.m_lsn);
  printf("  size: %u\n", m_header.m_size);
  printf("  Header Checksum: 0x%02x\n", m_header.m_headerChecksum);
  printf("  Data CRC32: 0x%08x\n", m_header.m_dataCrc32);
  printf("  Data size: %u\n", dataSize());
  printf("  Header Checksum Valid: %s\n",
         verifyHeaderChecksum() ? "Yes" : "No");
  printf("  Data CRC32 Valid: %s\n", verifyDataCrc32() ? "Yes" : "No");

  if (dataSize() > 0 && m_data != nullptr) {
    printf("  Data Preview: ");
    uint32_t previewSize = std::min(dataSize(), 32u);
    for (uint32_t i = 0; i < previewSize; i++) {
      if (m_data[i] >= 32 && m_data[i] < 127) {
        printf("%c", m_data[i]);
      } else {
        printf(".");
      }
    }
    if (dataSize() > 32) {
      printf("...");
    }
    printf("\n");
  }
}

RecordOpErrCode GtidInfoRecord::serialize(std::vector<uint8_t> &buf) const {
  memcpy_s(buf.data(), sizeof(GtidInfoRecordHeader), &m_header,
           sizeof(GtidInfoRecordHeader));

  if (dataSize() > 0 && m_data != nullptr) {
    memcpy_s(buf.data() + sizeof(GtidInfoRecordHeader), dataSize(), m_data,
             dataSize());
  }
  return RECORD_OP_SUCCESS;
}

RecordOpErrCode GtidInfoRecord::deserialize(const uint8_t *buffer,
                                            uint32_t bufferSize) {
  if (buffer == nullptr || bufferSize < sizeof(GtidInfoRecordHeader)) {
    return RECORD_OP_NEED_MORE_DATA;
  }

  memcpy_s(&m_header, sizeof(GtidInfoRecordHeader), buffer,
           sizeof(GtidInfoRecordHeader));

  if (bufferSize < m_header.m_size) {
    return RECORD_OP_NEED_MORE_DATA;
  }

  if (!verifyHeaderChecksum()) {
    return RECORD_OP_INVALID_FORMAT;
  }

  if (dataSize() > 0) {
    m_data = buffer + sizeof(GtidInfoRecordHeader);
    if (!verifyDataCrc32()) {
      return RECORD_OP_INVALID_FORMAT;
    }
  } else {
    m_data = nullptr;
  }
  return RECORD_OP_SUCCESS;
}

void persist_gtids_in_wal_to_file(const Gtid_set *gtid_set, bool need_lock) {
  DBUG_EXECUTE_IF("crash_before_rename_gtids_in_wal_file", DBUG_SUICIDE(););
  if (g_gtids_in_wal_store_file.is_exist() &&
      g_gtids_in_wal_store_file.has_record() &&
      ::rename(GTIDS_IN_WAL_STORE_FILE_NAME.c_str(),
               GTIDS_IN_WAL_STORE_FILE_NAME_BAK.c_str()) != 0) {
    sql_print_error(
        "Rename file gtids_in_wal_store_file.data to "
        "gtids_in_wal_store_file.data.bak failed.");
    return;
  }

  DBUG_EXECUTE_IF("crash_before_open_gtids_in_wal_file", DBUG_SUICIDE(););
  if (g_gtids_in_wal_store_file.open() != RECORD_OP_SUCCESS) {
    sql_print_error("Failed to open gtids in wal storage file.");
    return;
  }

  DBUG_EXECUTE_IF("crash_before_persist_gtids_in_wal_to_file", DBUG_SUICIDE(););
  static std::unique_ptr<BasicRecordFileStorage<GtidInfoRecord>::Writer>
      writer = g_gtids_in_wal_store_file.createWriterPtr();
  if (need_lock) {
    global_sid_lock->wrlock();
  }
  global_sid_lock->assert_some_lock();
  char *gtid_str = nullptr;
  uint32_t gtid_len = static_cast<uint32_t>(gtid_set->to_string(&gtid_str));
  if (need_lock) {
    global_sid_lock->unlock();
  }

  GtidInfoRecord record(reinterpret_cast<const uint8_t *>(gtid_str), gtid_len);
  // This function will call the file synchronization operation.
  if (writer->append(record, true) != RECORD_OP_SUCCESS) {
    sql_print_error("Write gtids in wal failed.");
  }

  sql_print_information("Write_gtid %s to wal file", gtid_str);
  my_free(gtid_str);
  DBUG_EXECUTE_IF("crash_before_close_gtids_in_wal_file", DBUG_SUICIDE(););
  g_gtids_in_wal_store_file.close();
}

Gtid_set *read_gtids_in_wal_from_file() {
  Gtid_set *read_gtid = nullptr;
  bool error_out = true;
  if (g_gtids_in_wal_store_file.is_exist() &&
      g_gtids_in_wal_store_file.has_record()) {
    read_gtid = read_gtids_from_file(&g_gtids_in_wal_store_file, error_out);
  }
  if (error_out && g_gtids_in_wal_store_bak_file.is_exist() &&
      g_gtids_in_wal_store_bak_file.has_record()) {
    read_gtid = read_gtids_from_file(&g_gtids_in_wal_store_bak_file, error_out);
  }
  return read_gtid;
}

Gtid_set *read_gtids_from_file(GtidInfoFileStorage *file, bool &error_out) {
  Sid_map *sid_map = nullptr;
  GtidInfoRecord record;
  if (file->open() != RECORD_OP_SUCCESS) {
    sql_print_error("Failed to open gtids in wal storage %s file.",
                    file->get_file_name().c_str());
    error_out = true;
    return nullptr;
  }
  std::unique_ptr<BasicRecordFileStorage<GtidInfoRecord>::Reader> file_reader =
      file->createReaderPtr();
  file_reader->init();
  if (!file_reader->has_more_records()) {
    sql_print_warning("File '%s' has no record.",
                      file->get_file_name().c_str());
    file->close();
    error_out = true;
    return nullptr;
  }
  if (file_reader->get_next(record) != RECORD_OP_SUCCESS) {
    sql_print_error("Failed to get gtids in wal record from %s file.",
                    file->get_file_name().c_str());
    file->close();
    error_out = true;
    return nullptr;
  }
  file->close();
  if (record.m_data == nullptr) {
    error_out = false;
    return nullptr;
  }
  global_sid_lock->wrlock();
  sid_map = new Sid_map(nullptr /*no rwlock*/);
  enum_return_status status = RETURN_STATUS_REPORTED_ERROR;
  Gtid_set *gtids_in_wal_from_file =
      new Gtid_set(sid_map, (const char *)(record.m_data), &status);
  if (rds_binlog_dstore_trace) {
    char *gtid_in_wal_str = nullptr;
    gtids_in_wal_from_file->to_string(&gtid_in_wal_str);
    my_dstore_support_binlog_check_trace("Read gtids in wal from file:%s",
                                         gtid_in_wal_str);
    my_free(gtid_in_wal_str);
    gtid_in_wal_str = nullptr;
  }
  global_sid_lock->unlock();
  if (status == RETURN_STATUS_OK) {
    error_out = false;
    return gtids_in_wal_from_file;
  } else {
    sql_print_error("Gtids_in_wal read from file error.");
    delete gtids_in_wal_from_file;
    delete sid_map;
    error_out = true;
    return nullptr;
  }
}
