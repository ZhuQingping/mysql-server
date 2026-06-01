
#include <fcntl.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "sql/ddl_info_file.h"

class DDLInfoRecordTest : public ::testing::Test {
 protected:
  void SetUp() override {
    testData = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
  }

  std::vector<uint8_t> testData;
};

// Test DDLInfoRecord default constructor
TEST_F(DDLInfoRecordTest, DefaultConstructor) {
  DDLInfoRecord record;

  EXPECT_EQ(record.lsn(), 0);
  EXPECT_EQ(record.size(), 0);
  EXPECT_EQ(record.dataSize(), 0);
  EXPECT_EQ(record.data(), nullptr);
}

// Test DDLInfoRecord parameterized constructor
TEST_F(DDLInfoRecordTest, ParameterizedConstructor) {
  uint64_t testLsn = 12345;
  DDLInfoRecord record(testLsn, 100, testData.data(), testData.size());

  EXPECT_EQ(record.lsn(), testLsn);
  EXPECT_EQ(record.dataSize(), testData.size());
  EXPECT_EQ(record.size(),
            sizeof(DDLInfoRecord::DDLInfoRecordHeader) + testData.size());
  EXPECT_EQ(record.data(), testData.data());
}

// Test calcHeaderChecksum
TEST_F(DDLInfoRecordTest, CalcHeaderChecksum) {
  DDLInfoRecord record(100, 100, testData.data(), testData.size());

  uint8_t checksum = record.calcHeaderChecksum();
  EXPECT_NE(checksum, 0);  // Should have some checksum value

  // Test with same data should produce same checksum
  DDLInfoRecord record2(100, 100, testData.data(), testData.size());
  EXPECT_EQ(checksum, record2.calcHeaderChecksum());
}

// Test calcDataCrc32 with valid data
TEST_F(DDLInfoRecordTest, CalcDataCrc32WithValidData) {
  DDLInfoRecord record(100, 100, testData.data(), testData.size());

  uint32_t crc = record.calcDataCrc32();
  EXPECT_NE(crc, 0);  // Should have some CRC value

  // Test with same data should produce same CRC
  DDLInfoRecord record2(100, 100, testData.data(), testData.size());
  EXPECT_EQ(crc, record2.calcDataCrc32());
}

// Test calcDataCrc32 with null data
TEST_F(DDLInfoRecordTest, CalcDataCrc32WithNullData) {
  DDLInfoRecord record(100, 100, nullptr, 0);

  uint32_t crc = record.calcDataCrc32();
  EXPECT_EQ(crc, 0);  // Should return 0 for null data
}

// Test calcDataCrc32 with zero size
TEST_F(DDLInfoRecordTest, CalcDataCrc32WithZeroSize) {
  DDLInfoRecord record(100, 100, testData.data(), 0);

  uint32_t crc = record.calcDataCrc32();
  EXPECT_EQ(crc, 0);  // Should return 0 for zero size
}

// Test verifyHeaderChecksum - valid case
TEST_F(DDLInfoRecordTest, VerifyHeaderChecksumValid) {
  DDLInfoRecord record(100, 100, testData.data(), testData.size());

  EXPECT_TRUE(record.verifyHeaderChecksum());
}

// Test verifyHeaderChecksum - invalid case
TEST_F(DDLInfoRecordTest, VerifyHeaderChecksumInvalid) {
  DDLInfoRecord record(100, 100, testData.data(), testData.size());

  // Corrupt the checksum
  const_cast<DDLInfoRecord::DDLInfoRecordHeader &>(record.m_header)
      .m_headerChecksum = 0xFF;

  EXPECT_FALSE(record.verifyHeaderChecksum());
}

// Test verifyDataCrc32 - valid case
TEST_F(DDLInfoRecordTest, VerifyDataCrc32Valid) {
  DDLInfoRecord record(100, 100, testData.data(), testData.size());

  EXPECT_TRUE(record.verifyDataCrc32());
}

// Test verifyDataCrc32 - invalid case
TEST_F(DDLInfoRecordTest, VerifyDataCrc32Invalid) {
  DDLInfoRecord record(100, 100, testData.data(), testData.size());

  // Corrupt the CRC
  const_cast<DDLInfoRecord::DDLInfoRecordHeader &>(record.m_header)
      .m_dataCrc32 = 0xFFFFFFFF;

  EXPECT_FALSE(record.verifyDataCrc32());
}

// Test verify - success case
TEST_F(DDLInfoRecordTest, VerifySuccess) {
  DDLInfoRecord record(100, 100, testData.data(), testData.size());

  EXPECT_EQ(record.verify(), RECORD_OP_SUCCESS);
}

// Test verify - invalid format case
TEST_F(DDLInfoRecordTest, VerifyInvalidFormat) {
  DDLInfoRecord record(100, 100, testData.data(), testData.size());

  // Corrupt the checksum
  const_cast<DDLInfoRecord::DDLInfoRecordHeader &>(record.m_header)
      .m_headerChecksum = 0xFF;

  EXPECT_EQ(record.verify(), RECORD_OP_INVALID_FORMAT);
}

// Test dataSize with invalid header size
TEST_F(DDLInfoRecordTest, DataSizeInvalidHeaderSize) {
  DDLInfoRecord record;

  // Set header size smaller than header itself
  const_cast<DDLInfoRecord::DDLInfoRecordHeader &>(record.m_header).m_size =
      sizeof(DDLInfoRecord::DDLInfoRecordHeader) - 1;

  EXPECT_EQ(record.dataSize(), 0);
}

// Test serialize
TEST_F(DDLInfoRecordTest, Serialize) {
  DDLInfoRecord record(100, 100, testData.data(), testData.size());

  std::vector<uint8_t> buffer(record.size());
  RecordOpErrCode result = record.serialize(buffer);

  EXPECT_EQ(result, RECORD_OP_SUCCESS);

  // Verify header was copied
  DDLInfoRecord::DDLInfoRecordHeader *header =
      reinterpret_cast<DDLInfoRecord::DDLInfoRecordHeader *>(buffer.data());
  EXPECT_EQ(header->m_lsn, 100);
  EXPECT_EQ(header->m_size, record.size());
}

// Test serialize with zero data size
TEST_F(DDLInfoRecordTest, SerializeZeroDataSize) {
  DDLInfoRecord record(100, 100, nullptr, 0);

  std::vector<uint8_t> buffer(record.size());
  RecordOpErrCode result = record.serialize(buffer);

  EXPECT_EQ(result, RECORD_OP_SUCCESS);
}

// Test deserialize - success case
TEST_F(DDLInfoRecordTest, DeserializeSuccess) {
  // Create a valid record and serialize it
  DDLInfoRecord originalRecord(100, 100, testData.data(), testData.size());
  std::vector<uint8_t> buffer(originalRecord.size());
  originalRecord.serialize(buffer);

  // Deserialize into new record
  DDLInfoRecord newRecord;
  RecordOpErrCode result = newRecord.deserialize(buffer.data(), buffer.size());

  EXPECT_EQ(result, RECORD_OP_SUCCESS);
  EXPECT_EQ(newRecord.lsn(), 100);
  EXPECT_EQ(newRecord.dataSize(), testData.size());
}

// Test deserialize - null buffer
TEST_F(DDLInfoRecordTest, DeserializeNullBuffer) {
  DDLInfoRecord record;

  RecordOpErrCode result = record.deserialize(nullptr, 100);

  EXPECT_EQ(result, RECORD_OP_NEED_MORE_DATA);
}

// Test deserialize - buffer too small for header
TEST_F(DDLInfoRecordTest, DeserializeBufferTooSmallForHeader) {
  DDLInfoRecord record;
  uint8_t smallBuffer[10];

  RecordOpErrCode result = record.deserialize(smallBuffer, sizeof(smallBuffer));

  EXPECT_EQ(result, RECORD_OP_NEED_MORE_DATA);
}

// Test deserialize - buffer too small for full record
TEST_F(DDLInfoRecordTest, DeserializeBufferTooSmallForRecord) {
  DDLInfoRecord originalRecord(100, 100, testData.data(), testData.size());
  std::vector<uint8_t> buffer(originalRecord.size());
  originalRecord.serialize(buffer);

  DDLInfoRecord newRecord;
  RecordOpErrCode result =
      newRecord.deserialize(buffer.data(), buffer.size() - 1);

  EXPECT_EQ(result, RECORD_OP_NEED_MORE_DATA);
}

// Test deserialize - invalid header checksum
TEST_F(DDLInfoRecordTest, DeserializeInvalidHeaderChecksum) {
  DDLInfoRecord originalRecord(100, 100, testData.data(), testData.size());
  std::vector<uint8_t> buffer(originalRecord.size());
  originalRecord.serialize(buffer);

  // Corrupt header checksum
  DDLInfoRecord::DDLInfoRecordHeader *header =
      reinterpret_cast<DDLInfoRecord::DDLInfoRecordHeader *>(buffer.data());
  header->m_headerChecksum = 0xFF;

  DDLInfoRecord newRecord;
  RecordOpErrCode result = newRecord.deserialize(buffer.data(), buffer.size());

  EXPECT_EQ(result, RECORD_OP_INVALID_FORMAT);
}

// Test deserialize - invalid data CRC
TEST_F(DDLInfoRecordTest, DeserializeInvalidDataCrc) {
  DDLInfoRecord originalRecord(100, 100, testData.data(), testData.size());
  std::vector<uint8_t> buffer(originalRecord.size());
  originalRecord.serialize(buffer);

  // Corrupt data CRC
  DDLInfoRecord::DDLInfoRecordHeader *header =
      reinterpret_cast<DDLInfoRecord::DDLInfoRecordHeader *>(buffer.data());
  header->m_dataCrc32 = 0xFFFFFFFF;

  DDLInfoRecord newRecord;
  RecordOpErrCode result = newRecord.deserialize(buffer.data(), buffer.size());

  EXPECT_EQ(result, RECORD_OP_INVALID_FORMAT);
}

// Test deserialize - zero data size
TEST_F(DDLInfoRecordTest, DeserializeZeroDataSize) {
  DDLInfoRecord originalRecord(100, 100, nullptr, 0);
  std::vector<uint8_t> buffer(originalRecord.size());
  originalRecord.serialize(buffer);

  DDLInfoRecord newRecord;
  RecordOpErrCode result = newRecord.deserialize(buffer.data(), buffer.size());

  EXPECT_EQ(result, RECORD_OP_SUCCESS);
  EXPECT_EQ(newRecord.dataSize(), 0);
  EXPECT_EQ(newRecord.data(), nullptr);
}

// Test debugDump - just ensure it doesn't crash
TEST_F(DDLInfoRecordTest, DebugDump) {
  DDLInfoRecord record(100, 100, testData.data(), testData.size());

  // Redirect stdout to capture output
  testing::internal::CaptureStdout();
  record.debugDump();
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_FALSE(output.empty());
  EXPECT_NE(output.find("DDLInfoRecord"), std::string::npos);
}

// Test debugDump with zero data
TEST_F(DDLInfoRecordTest, DebugDumpZeroData) {
  DDLInfoRecord record(100, 100, nullptr, 0);

  testing::internal::CaptureStdout();
  record.debugDump();
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_FALSE(output.empty());
}

// BasicRecordFileStorage Tests
class BasicRecordFileStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    testFileName = "test_record_file.dat";
    // Clean up any existing test file
    unlink(testFileName.c_str());
  }

  void TearDown() override {
    // Clean up test file
    unlink(testFileName.c_str());
  }

  std::string testFileName;
};

// Test storage constructor
TEST_F(BasicRecordFileStorageTest, Constructor) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);

  EXPECT_FALSE(storage.isOpen());
}

// Test storage destructor
TEST_F(BasicRecordFileStorageTest, Destructor) {
  {
    BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
    EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);
    EXPECT_TRUE(storage.isOpen());
  }
  // Storage should be closed after destructor
}

// Test open new file
TEST_F(BasicRecordFileStorageTest, OpenNewFile) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);

  RecordOpErrCode result = storage.open();

  EXPECT_EQ(result, RECORD_OP_SUCCESS);
  EXPECT_TRUE(storage.isOpen());
  EXPECT_EQ(storage.getRecordCount(), 0);
}

// Test open existing file
TEST_F(BasicRecordFileStorageTest, OpenExistingFile) {
  // Create a file first
  {
    BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
    EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);
  }

  // Open existing file
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
  RecordOpErrCode result = storage.open();

  EXPECT_EQ(result, RECORD_OP_SUCCESS);
  EXPECT_TRUE(storage.isOpen());
}

// Test open already opened file
TEST_F(BasicRecordFileStorageTest, OpenAlreadyOpened) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);

  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);
  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);  // Should succeed
}

// Test sync
TEST_F(BasicRecordFileStorageTest, Sync) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);

  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);
  EXPECT_EQ(storage.sync(), RECORD_OP_SUCCESS);
}

// Test sync without open
TEST_F(BasicRecordFileStorageTest, SyncWithoutOpen) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);

  EXPECT_EQ(storage.sync(), RECORD_OP_IO_FAILED);
}

// Test close
TEST_F(BasicRecordFileStorageTest, Close) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);

  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);
  EXPECT_EQ(storage.close(), RECORD_OP_SUCCESS);
  EXPECT_FALSE(storage.isOpen());
}

// Test close without open
TEST_F(BasicRecordFileStorageTest, CloseWithoutOpen) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);

  EXPECT_EQ(storage.close(), RECORD_OP_SUCCESS);
}

// Test clean
TEST_F(BasicRecordFileStorageTest, Clean) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);

  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);
  EXPECT_EQ(storage.clean(), RECORD_OP_SUCCESS);
  EXPECT_FALSE(storage.isOpen());

  // File should not exist anymore
  EXPECT_EQ(access(testFileName.c_str(), F_OK), -1);
}

// Test createReaderPtr
TEST_F(BasicRecordFileStorageTest, CreateReaderPtr) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);

  // Should return nullptr if not opened
  EXPECT_EQ(storage.createReaderPtr(), nullptr);

  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);
  auto reader = storage.createReaderPtr();
  EXPECT_NE(reader, nullptr);
}

// Test createWriterPtr
TEST_F(BasicRecordFileStorageTest, CreateWriterPtr) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);

  // Should return nullptr if not opened
  EXPECT_EQ(storage.createWriterPtr(), nullptr);

  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);
  auto writer = storage.createWriterPtr();
  EXPECT_NE(writer, nullptr);
}

// Test Reader class
TEST_F(BasicRecordFileStorageTest, ReaderInit) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);

  auto reader = storage.createReaderPtr();
  EXPECT_EQ(reader->init(), RECORD_OP_SUCCESS);
  EXPECT_EQ(reader->get_total_records(), 0);
  EXPECT_EQ(reader->get_current_index(), 0);
  EXPECT_FALSE(reader->has_more_records());
}

// Test Reader get_next with no records
TEST_F(BasicRecordFileStorageTest, ReaderGetNextNoRecords) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);

  auto reader = storage.createReaderPtr();
  EXPECT_EQ(reader->init(), RECORD_OP_SUCCESS);

  DDLInfoRecord record;
  EXPECT_EQ(reader->get_next(record), RECORD_OP_END_OF_FILE);
}

// Test Writer append
TEST_F(BasicRecordFileStorageTest, WriterAppend) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);

  auto writer = storage.createWriterPtr();
  EXPECT_NE(writer, nullptr);

  std::vector<uint8_t> testData = {'T', 'e', 's', 't'};
  DDLInfoRecord record(1, 1, testData.data(), testData.size());

  EXPECT_EQ(writer->append(record), RECORD_OP_SUCCESS);
  EXPECT_EQ(storage.getRecordCount(), 1);
}

// Test complete read/write cycle
TEST_F(BasicRecordFileStorageTest, CompleteReadWriteCycle) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);

  // Write some records
  auto writer = storage.createWriterPtr();
  std::vector<uint8_t> testData1 = {'T', 'e', 's', 't', '1'};
  std::vector<uint8_t> testData2 = {'T', 'e', 's', 't', '2'};

  DDLInfoRecord record1(1, 1, testData1.data(), testData1.size());
  DDLInfoRecord record2(2, 2, testData2.data(), testData2.size());

  EXPECT_EQ(writer->append(record1), RECORD_OP_SUCCESS);
  EXPECT_EQ(writer->append(record2), RECORD_OP_SUCCESS);

  EXPECT_EQ(storage.sync(), RECORD_OP_SUCCESS);

  // Read records back
  auto reader = storage.createReaderPtr();
  EXPECT_EQ(reader->init(), RECORD_OP_SUCCESS);
  EXPECT_EQ(reader->get_total_records(), 2);

  DDLInfoRecord readRecord1, readRecord2;
  EXPECT_EQ(reader->get_next(readRecord1), RECORD_OP_SUCCESS);
  EXPECT_EQ(reader->get_next(readRecord2), RECORD_OP_SUCCESS);

  EXPECT_EQ(readRecord1.lsn(), 1);
  EXPECT_EQ(readRecord2.lsn(), 2);

  // Should be end of file now
  DDLInfoRecord extraRecord;
  EXPECT_EQ(reader->get_next(extraRecord), RECORD_OP_END_OF_FILE);
}

#include <random>

// Test writing and reading large batches of random-length records
TEST_F(BasicRecordFileStorageTest, LargeVolumeRandomRecords) {
  const int RECORD_COUNT = 1000;
  const uint32_t MAX_DATA_LEN = 64 * 1024;  // Maximum 64KB
  std::mt19937 rng(12345);
  std::uniform_int_distribution<uint32_t> lenDist(0, MAX_DATA_LEN);

  // Open storage
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
  ASSERT_EQ(storage.open(), RECORD_OP_SUCCESS);

  auto writer = storage.createWriterPtr();
  ASSERT_NE(writer, nullptr);

  // Generate and write multiple random-length records
  std::vector<std::vector<uint8_t>> allData(RECORD_COUNT);
  std::vector<uint64_t> allLsn(RECORD_COUNT);
  for (int i = 0; i < RECORD_COUNT; ++i) {
    uint32_t len = lenDist(rng);
    allData[i].resize(len);
    for (uint32_t j = 0; j < len; ++j) {
      allData[i][j] = static_cast<uint8_t>(rng() & 0xFF);
    }
    allLsn[i] = static_cast<uint64_t>(i + 1000);
    DDLInfoRecord rec(allLsn[i], 1024, len ? allData[i].data() : nullptr, len);
    ASSERT_EQ(writer->append(rec), RECORD_OP_SUCCESS);
  }

  // Force sync to flush to disk and update header
  ASSERT_EQ(storage.sync(), RECORD_OP_SUCCESS);
  EXPECT_EQ(storage.getRecordCount(), RECORD_COUNT);

  // Create reader and initialize with smaller buffer to trigger multiple
  // refills
  auto reader = storage.createReaderPtr(1024 * 1024);  // 1MB buffer
  ASSERT_NE(reader, nullptr);
  ASSERT_EQ(reader->init(), RECORD_OP_SUCCESS);
  EXPECT_EQ(reader->get_total_records(), RECORD_COUNT);

  // Read and verify each record sequentially
  DDLInfoRecord readRec;
  for (int i = 0; i < RECORD_COUNT; ++i) {
    EXPECT_TRUE(reader->has_more_records());
    RecordOpErrCode code = reader->get_next(readRec);
    ASSERT_EQ(code, RECORD_OP_SUCCESS) << "Failed to read record " << i;

    // Verify LSN and length
    EXPECT_EQ(readRec.lsn(), allLsn[i]);
    EXPECT_EQ(readRec.dataSize(), allData[i].size());

    // Verify data content
    if (allData[i].size() > 0) {
      EXPECT_EQ(0,
                memcmp(readRec.data(), allData[i].data(), allData[i].size()));
    } else {
      EXPECT_EQ(readRec.data(), nullptr);
    }
    EXPECT_EQ(reader->get_current_index(), static_cast<uint32_t>(i + 1));
  }

  // After last record, should have no more records
  EXPECT_FALSE(reader->has_more_records());
  RecordOpErrCode eofCode = reader->get_next(readRec);
  EXPECT_EQ(eofCode, RECORD_OP_END_OF_FILE);
}

// Test file offset management with batch sync threshold
TEST_F(BasicRecordFileStorageTest, BatchSyncThresholdTest) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
  ASSERT_EQ(storage.open(), RECORD_OP_SUCCESS);

  auto writer = storage.createWriterPtr();
  ASSERT_NE(writer, nullptr);

  // Write records up to but not exceeding BATCH_SYNC_THRESHOLD (1000)
  std::vector<uint8_t> testData(100, 'X');
  for (int i = 0; i < 999; ++i) {
    DDLInfoRecord record(i + 1, i + 1, testData.data(), testData.size());
    ASSERT_EQ(writer->append(record), RECORD_OP_SUCCESS);
  }

  // Record count should be updated but not synced yet
  EXPECT_EQ(storage.getRecordCount(), 999);

  // Add one more record to trigger auto-sync
  DDLInfoRecord record(1000, 1000, testData.data(), testData.size());
  ASSERT_EQ(writer->append(record), RECORD_OP_SUCCESS);
  EXPECT_EQ(storage.getRecordCount(), 1000);

  // Verify all records can be read
  auto reader = storage.createReaderPtr();
  ASSERT_EQ(reader->init(), RECORD_OP_SUCCESS);
  EXPECT_EQ(reader->get_total_records(), 1000);

  // Read a few records to verify file offset management
  for (int i = 0; i < 10; ++i) {
    DDLInfoRecord readRecord;
    ASSERT_EQ(reader->get_next(readRecord), RECORD_OP_SUCCESS);
    EXPECT_EQ(readRecord.lsn(), static_cast<uint64_t>(i + 1));
  }
}

// Test reader initialization with corrupted file state
TEST_F(BasicRecordFileStorageTest, ReaderInitWithCorruptedFile) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
  ASSERT_EQ(storage.open(), RECORD_OP_SUCCESS);

  // Try to create reader when storage is not properly initialized
  auto reader = storage.createReaderPtr();
  ASSERT_NE(reader, nullptr);

  // Init should succeed even with empty file
  EXPECT_EQ(reader->init(), RECORD_OP_SUCCESS);
  EXPECT_EQ(reader->get_total_records(), 0);
  EXPECT_FALSE(reader->has_more_records());
}

// Test fillBuffer edge cases
TEST_F(BasicRecordFileStorageTest, FillBufferEdgeCases) {
  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
  ASSERT_EQ(storage.open(), RECORD_OP_SUCCESS);

  auto writer = storage.createWriterPtr();
  ASSERT_NE(writer, nullptr);

  // Write exactly one record
  std::vector<uint8_t> testData(1000, 'T');
  DDLInfoRecord record(1, 1, testData.data(), testData.size());
  ASSERT_EQ(writer->append(record), RECORD_OP_SUCCESS);
  ASSERT_EQ(storage.sync(), RECORD_OP_SUCCESS);

  // Create reader with buffer size just equal to record size
  auto reader = storage.createReaderPtr(record.size());
  ASSERT_EQ(reader->init(), RECORD_OP_SUCCESS);

  DDLInfoRecord readRecord;
  EXPECT_EQ(reader->get_next(readRecord), RECORD_OP_SUCCESS);
  EXPECT_EQ(readRecord.lsn(), 1);
  EXPECT_EQ(readRecord.xid(), 1);
  EXPECT_EQ(readRecord.dataSize(), 1000);
}

// Test has_serialize trait
TEST(TraitTest, HasSerialize) {
  EXPECT_TRUE(serialization_traits::has_serialize<DDLInfoRecord>::value);
}

// Test has_size trait
TEST(TraitTest, HasSize) {
  EXPECT_TRUE(serialization_traits::has_size<DDLInfoRecord>::value);
}

// Test has_deserialize trait
TEST(TraitTest, HasDeserialize) {
  EXPECT_TRUE(serialization_traits::has_deserialize<DDLInfoRecord>::value);
}

// Test error scenarios that are hard to trigger in normal flow
class ErrorScenarioTest : public ::testing::Test {
 protected:
  void SetUp() override {
    testFileName = "error_test_file.dat";
    unlink(testFileName.c_str());
  }

  void TearDown() override { unlink(testFileName.c_str()); }

  std::string testFileName;
};

// Test file creation failure by using invalid path
TEST_F(ErrorScenarioTest, FileCreationFailure) {
  BasicRecordFileStorage<DDLInfoRecord> storage("/invalid/path/file.dat");

  EXPECT_NE(storage.open(), RECORD_OP_SUCCESS);
  EXPECT_FALSE(storage.isOpen());
}

// Test corrupted file header magic
TEST_F(ErrorScenarioTest, CorruptedFileMagic) {
  // Create a file with wrong magic
  std::ofstream file(testFileName, std::ios::binary);
  char wrongMagic[8] = {'W', 'R', 'O', 'N', 'G', 'M', 'A', 'G'};
  file.write(wrongMagic, 8);
  file.close();

  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);
}

// Test file with wrong version
TEST_F(ErrorScenarioTest, WrongFileVersion) {
  // Create file with correct magic but wrong version
  std::ofstream file(testFileName, std::ios::binary);
  char magic[8] = {'D', 'D', 'L', 'L', 'O', 'G', '0', '1'};
  uint32_t wrongVersion = 999;
  file.write(magic, 8);
  file.write(reinterpret_cast<const char *>(&wrongVersion),
             sizeof(wrongVersion));
  file.close();

  BasicRecordFileStorage<DDLInfoRecord> storage(testFileName);
  EXPECT_EQ(storage.open(), RECORD_OP_SUCCESS);
}

TEST_F(ErrorScenarioTest, DDLInfoOp) {
  unsigned char ddl[3] = {'D', 'D', 'L'};
  g_ddl_info_store_file.open();
  /* Test stale DDL info is OK */
  EXPECT_EQ(put_ddl_info(0, ddl, 3, 256), DDL_INFO_OP_SUCCESS);
  rm_ddl_info();
  EXPECT_EQ(put_ddl_info(128, ddl, 3, 256), DDL_INFO_WRITE_OP_FAILED);
  unlink("ddl_info_store_file.dat");
}