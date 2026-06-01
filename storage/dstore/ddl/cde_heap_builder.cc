/*
  Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "cde_heap_builder.h"
#include "common/cde_compare_utils.h"
#include "common/cde_perf.h"
#include "common/cde_trxmgr.h"
#include "dict/cde_autoinc.h"
#include "dict/cde_relation.h"
#include "dml/cde_dml_ctx.h"
#include "framework/dstore_thread.h"
#include "framework/dstore_thread_interface.h"
#include "handler/ha_cde_parallel.h"
#include "scope_guard.h"
#include "sql/sql_thd_internal_api.h"
#include "transaction/dstore_transaction.h"
#include "tuple/dstore_memheap_tuple.h"

using namespace CDE;

HeapBatchInserter::HeapBatchInserter(const size_t maxBufferSize,
                                     DSTORE::StorageRelation heapRel)
    : m_maxBufferSize(maxBufferSize), m_heapRel(heapRel) {
  m_insertHander = HeapInterface::CreateHeapInsertHandler();
  CDE_ASSERT(m_insertHander != nullptr);
}

HeapBatchInserter::~HeapBatchInserter() {
  HeapInterface::DestroyHeapInsertHandler(m_insertHander);
  // clean up
  CleanUp();
}

int HeapBatchInserter::AddRow(DSTORE::HeapTuple *tuple) {
  m_tuples.push_back(tuple);
  m_curBufferUsedSize += tuple->GetDiskTupleSize();

  if (m_curBufferUsedSize >= m_maxBufferSize) {
    return InsertRows();
  }
  return CDE_OK;
}

int HeapBatchInserter::InsertRows() {
  if (m_tuples.empty()) {
    return CDE_OK;
  }

  Huawei::Common::LatencyCounter::Timer t(
      cde_perf_counters::getInstance()
          ->_inplace_rebuild_heap_batch_insert_latency);

  AutonomousTrxGuard autonomousTrxGuard(true, __func__);
  if (unlikely(autonomousTrxGuard.IsAbnormal())) {
    /* Error log has been printed. */
    CleanUp();
    return CDE_FAIL;
  }

  uint16_t numTuples = m_tuples.size();
  int ret =
      HeapInterface::BatchInsert(nullptr, m_heapRel, m_tuples.data(), numTuples,
                                 TransactionInterface::GetCurCid());

  CleanUp();

  if (ret != CDE_OK) {
    return GetAndConvertDstoreErrcodeToMysql();
  }

  autonomousTrxGuard.commit(__func__);

  return CDE_OK;
}

void HeapBatchInserter::CleanUp() {
  for (auto &tuple : m_tuples) {
    TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
  }
  m_tuples.clear();
  m_curBufferUsedSize = 0;
}

HeapBuilderContext::HeapBuilderContext(
    TABLE *oldTable, TABLE *newTable, const cde_dict_t *oldDictTable,
    const cde_dict_t *newDictTable, const DSTORE::StorageRelation oldHeapRel,
    const DSTORE::StorageRelation newHeapRel, std::vector<uint32_t> &colMap,
    std::vector<uint32_t> &nonNull, uint32_t addAutoIncPos,
    const size_t maxBufferSize, const size_t maxThreads,
    AutoIncreSequence &sequence)
    : m_oldTable(oldTable),
      m_newTable(newTable),
      m_oldDictTable(oldDictTable),
      m_newDictTable(newDictTable),
      m_oldHeapRel(oldHeapRel),
      m_newHeapRel(newHeapRel),
      m_colMap(colMap),
      m_nonNull(nonNull),
      m_maxBufferSize(maxBufferSize),
      m_maxThreads(maxThreads),
      m_addAutoinc(addAutoIncPos),
      m_sequence(sequence) {
  MutexInit(0, &m_autoincMutex, MY_MUTEX_INIT_FAST);
}

HeapBuilderContext::~HeapBuilderContext() {
  m_newDictTable = nullptr;
  m_oldDictTable = nullptr;
  m_newTable = nullptr;
  m_oldTable = nullptr;
  MutexDestroy(&m_autoincMutex);
}

HeapBuilder::HeapBuilder(
    TABLE *oldTable, TABLE *newTable, const cde_dict_t *oldDictTable,
    const cde_dict_t *newDictTable, const DSTORE::StorageRelation oldHeapRel,
    DSTORE::StorageRelation newHeapRel, std::vector<uint32_t> &colMap,
    std::vector<uint32_t> &nonNull, const AddedFieldInfos &addedFieldsInfos,
    uint32_t addAutoIncPos, const size_t maxBufferSize, const size_t maxThreads,
    AutoIncreSequence &sequence, bool isBatchInsertMode)
    : m_context(oldTable, newTable, oldDictTable, newDictTable, oldHeapRel,
                newHeapRel, colMap, nonNull, addAutoIncPos, maxBufferSize,
                maxThreads, sequence),
      m_isBatchInsertMode(isBatchInsertMode) {
  m_oldRows.resize(m_context.m_maxThreads);
  uint32_t numColumnsOld = m_context.m_oldHeapRel->attr->natts;
  for (auto &row : m_oldRows) {
    row.m_valuesWithoutVarlen =
        m_memHeap.ArrayAlloc<DSTORE::Datum>(numColumnsOld);
    row.m_nulls = m_memHeap.ArrayAlloc<bool>(numColumnsOld);
  }

  m_newRows.resize(m_context.m_maxThreads);
  uint32_t numColumnsNew = m_context.m_newHeapRel->attr->natts;
  for (auto &row : m_newRows) {
    row.m_valuesWithoutVarlen =
        m_memHeap.ArrayAlloc<DSTORE::Datum>(numColumnsNew);
    row.m_nulls = m_memHeap.ArrayAlloc<bool>(numColumnsNew);
  }

  for (uint32_t i = 0; i < m_context.m_maxThreads; ++i) {
    auto &newRow = m_newRows[i];
    for (auto &fieldInfo : addedFieldsInfos) {
      if (fieldInfo.m_isDefaultNull) {
        newRow.m_nulls[fieldInfo.m_Pos] = true;
        continue;
      }

      newRow.m_valuesWithoutVarlen[fieldInfo.m_Pos] =
          fieldInfo.m_defaultValueWithoutrVarlen;
    }
  }

  /*When multiple threads concurrently insert data into a new heap, it is
   * necessary to copy the StorageRelation object to avoid sharing the same
   * object.*/
  m_heapRels.resize(m_context.m_maxThreads);
  for (uint16_t i = 0; i < m_context.m_maxThreads; i++) {
    CDE_ASSERT(cde_relation::Clone(m_context.m_newHeapRel, &m_memHeap,
                                   m_context.m_newDictTable->fillfactor,
                                   m_heapRels[i]) == CDE_SUCC);
  }

  if (m_isBatchInsertMode) {
    for (uint16_t i = 0; i < m_context.m_maxThreads; i++) {
      m_batchInserters.push_back(
          Memory::New<HeapBatchInserter>(maxBufferSize, m_heapRels[i]));
    }
  }

  m_errorStates.resize(m_context.m_maxThreads);
  std::fill(m_errorStates.begin(), m_errorStates.end(), CDE_OK);
}

HeapBuilder::~HeapBuilder() {
  for (uint16_t i = 0; i < m_context.m_maxThreads; i++) {
    m_heapRels[i]->Destroy();
  }

  if (m_isBatchInsertMode) {
    for (uint16_t i = 0; i < m_context.m_maxThreads; i++) {
      Memory::Delete(m_batchInserters[i]);
    }
  }
}

int HeapBuilder::BuildHeap() {
  Huawei::Common::LatencyCounter::Timer t(
      cde_perf_counters::getInstance()->_inplace_rebuild_heap_latency);
  auto buildRow = [this](DSTORE::HeapTuple *tuple, uint16_t threadIdx,
                         void *ctx) -> int {
    return this->BuildHeapRowCallback(tuple, threadIdx, ctx);
  };

  ParallelHeapScanReader paraHeapScaner(
      m_context.m_oldHeapRel, m_context.m_maxThreads, std::move(buildRow),
      static_cast<void *>(&m_context));

  if (paraHeapScaner.Init() != CDE_OK) {
    return CDE_ERROR;
  }

  if (paraHeapScaner.Run() != CDE_OK) {
    for (auto errstat : m_errorStates) {
      if (errstat != CDE_OK) {
        return errstat;
      }
    }
    return CDE_FAIL;
  }

  /* For batch insert, we need to insert the last batch if success. */
  if (m_isBatchInsertMode) {
    for (auto inserter : m_batchInserters) {
      int err = inserter->InsertRows();
      if (err != CDE_OK) {
        return err;
      }
    }
  }
  return CDE_OK;
}

int HeapBuilder::BuildHeapWithBatchInsert(DSTORE::HeapTuple *tuple,
                                          uint16_t threadIdx) {
  auto cleanupGuard = create_scope_guard([&]() {
    if (m_newRows[threadIdx].m_tupleWithLob != nullptr) {
      TupleInterface::DestroyTuple(m_newRows[threadIdx].m_tupleWithLob,
                                   CdeFreeMemForDtuple);
      m_newRows[threadIdx].m_tupleWithLob = nullptr;
    }
  });

  /* Step 1. Deform old heap tuple. */
  int ret = DeformOldTuple(tuple, m_oldRows[threadIdx], threadIdx);
  if (ret != CDE_OK) {
    return ret;
  }

  /* Step 2. Check constraints. */
  if (!CheckNullConstraints(m_oldRows[threadIdx].m_nulls, threadIdx)) {
    return CDE_ERROR;
  }

  /* Step 3. Handle autoincrement column. */
  ret = HandleAutoinc(m_newRows[threadIdx], threadIdx);
  if (ret != CDE_OK) {
    return CDE_ERROR;
  }

  /* Step 4. Form new heap tuple. */
  ret = FormNewTuple(m_oldRows[threadIdx], m_newRows[threadIdx], threadIdx);
  if (ret != CDE_OK) {
    return ret;
  }

  /* Step 5. Insert new heap tuple. */
  ret = m_batchInserters[threadIdx]->AddRow(m_newRows[threadIdx].m_tuple);
  if (ret != CDE_OK) {
    m_errorStates[threadIdx] = ret;
  }

  return ret;
}

int HeapBuilder::BuildHeapWithSingleInsert(DSTORE::HeapTuple *tuple,
                                           uint16_t threadIdx) {
  auto cleanupGuard = create_scope_guard([&]() {
    if (m_newRows[threadIdx].m_tupleWithLob != nullptr) {
      TupleInterface::DestroyTuple(m_newRows[threadIdx].m_tupleWithLob,
                                   CdeFreeMemForDtuple);
      m_newRows[threadIdx].m_tupleWithLob = nullptr;
    }

    if (m_newRows[threadIdx].m_tuple != nullptr) {
      TupleInterface::DestroyTuple(m_newRows[threadIdx].m_tuple,
                                   CdeFreeMemForDtuple);
      m_newRows[threadIdx].m_tuple = nullptr;
    }
  });

  /* Step 1. Deform old heap tuple. */
  int ret = DeformOldTuple(tuple, m_oldRows[threadIdx], threadIdx);
  if (ret != CDE_OK) {
    return ret;
  }

  /* Step 2. Check constraints. */
  if (!CheckNullConstraints(m_oldRows[threadIdx].m_nulls, threadIdx)) {
    return CDE_ERROR;
  }

  /* Step 3. Handle autoincrement column. */
  ret = HandleAutoinc(m_newRows[threadIdx], threadIdx);
  if (ret != CDE_OK) {
    return CDE_ERROR;
  }

  /* Step 4. Form new heap tuple. */
  ret = FormNewTuple(m_oldRows[threadIdx], m_newRows[threadIdx], threadIdx);
  if (ret != CDE_OK) {
    return ret;
  }

  /* Step 5. Insert new heap tuple. */
  ret = InsertSingleTuple(threadIdx);

  return ret;
}

int HeapBuilder::InsertSingleTuple(uint16_t threadIdx) {
  Huawei::Common::LatencyCounter::Timer t(
      cde_perf_counters::getInstance()
          ->_inplace_rebuild_heap_single_insert_latency);
  AutonomousTrxGuard autonomousTrxGuard(true, __func__);
  if (unlikely(autonomousTrxGuard.IsAbnormal())) {
    /* Error log has been printed. */
    return CDE_FAIL;
  }

  int ret = HeapInterface::Insert(
      m_heapRels[threadIdx], m_newRows[threadIdx].m_tuple,
      m_newRows[threadIdx].ctid, TransactionInterface::GetCurCid());

  if (ret != CDE_OK) {
    auto err = GetAndConvertDstoreErrcodeToMysql();
    CDE_ASSERT(err != CDE_OK);
    m_errorStates[threadIdx] = err;
    return CDE_ERROR;
  }

  autonomousTrxGuard.commit(__func__);
  return CDE_OK;
}

int HeapBuilder::BuildHeapRowCallback(DSTORE::HeapTuple *tuple,
                                      uint16_t threadIdx,
                                      [[maybe_unused]] void *ctx) {
  if (tuple == nullptr) {
    return CDE_ERROR;
  }

  if (m_isBatchInsertMode) {
    return BuildHeapWithBatchInsert(tuple, threadIdx);
  }

  return BuildHeapWithSingleInsert(tuple, threadIdx);
}

int HeapBuilder::FormNewTuple(Row &oldRow, Row &newRow, uint16_t threadIdx) {
  uint32_t numColumnsOld = m_context.m_oldHeapRel->attr->natts;
  newRow.m_tupleWithLob = oldRow.m_tupleWithLob;
  oldRow.m_tupleWithLob = nullptr;
  for (uint32_t oldColIndex = 0; oldColIndex < numColumnsOld; oldColIndex++) {
    uint32_t newColIndex = m_context.m_colMap[oldColIndex];
    if (newColIndex == HeapBuilderContext::INVALID_COL_NUM) {
      continue;
    }

    newRow.m_valuesWithoutVarlen[newColIndex] =
        oldRow.m_valuesWithoutVarlen[oldColIndex];
    newRow.m_nulls[newColIndex] = oldRow.m_nulls[oldColIndex];
  }

  newRow.m_tuple = TupleInterface::FormHeapTuple(
      m_context.m_newHeapRel->attr, newRow.m_valuesWithoutVarlen,
      newRow.m_nulls, AllocMemZeroForDtuple);
  if (newRow.m_tuple == nullptr) {
    auto err = GetAndConvertDstoreErrcodeToMysql();
    CDE_ASSERT(err != CDE_OK);
    m_errorStates[threadIdx] = err;
    return CDE_ERROR;
  }
  return CDE_OK;
}

int HeapBuilder::DeformOldTuple(DSTORE::HeapTuple *tuple, Row &row,
                                uint16_t threadIdx) {
  DSTORE::TupleDesc tupleDesc = m_context.m_oldHeapRel->attr;
  row.m_tuple = tuple;
  DSTORE::HeapTuple *relTuple = row.m_tuple;
  if (tupleDesc->tdhaslob) {
    row.m_tupleWithLob = HeapInterface::FetchTupleWithLob(
        m_context.m_oldHeapRel, tuple,
        DSTORE::thrd->GetActiveTransaction()->GetSnapshot(),
        AllocMemZeroForDtuple);
    if (row.m_tupleWithLob == nullptr) {
      CDE_LOG_ERROR("Fetch blob tuple failed for table:%s",
                    m_context.m_oldDictTable->name.c_str());
      auto err = GetAndConvertDstoreErrcodeToMysql();
      CDE_ASSERT(err != CDE_OK);
      m_errorStates[threadIdx] = err;
      return CDE_ERROR;
    }
    relTuple = row.m_tupleWithLob;
  }
  relTuple->DeformTuple(tupleDesc, row.m_valuesWithoutVarlen, row.m_nulls);

  if (row.m_tuple == row.m_tupleWithLob) {
    // If the LOB column size is less than 2KB,
    // the tuple is not rebuilt, and there is no need to release
    // the m_tupleWithLob memory during the cleanup phase.
    row.m_tupleWithLob = nullptr;
  }

  return CDE_OK;
}

bool HeapBuilder::CheckNullConstraints(bool *const isNulls,
                                       uint16_t threadIdx) {
  for (const auto colIdx : m_context.m_nonNull) {
    if (isNulls[colIdx]) {
      m_errorStates[threadIdx] = static_cast<HaErrorCode>(
          HaDstoreErrE::HA_DSTORE_ERR_INPLACE_INVALID_USE_OF_NULL);
      return false;
    }
  }

  return true;
}

int HeapBuilder::HandleAutoinc(Row &row, uint16_t threadIdx) {
  if (m_context.m_addAutoinc == HeapBuilderContext::INVALID_COL_NUM) {
    return CDE_OK;
  }

  if (row.m_nulls[m_context.m_addAutoinc]) {
    return CDE_OK;
  }

  if (m_context.m_sequence.eof()) {
    m_errorStates[threadIdx] =
        static_cast<HaErrorCode>(HA_ERR_AUTOINC_READ_FAILED);
    return CDE_ERROR;
  }

  // Generate autoincrement value
  uint64_t value = 0;
  {
    CdeMutexGuard lock(&m_context.m_autoincMutex, CDE_LOCATION_HERE);
    value = m_context.m_sequence++;
  }

  // Store auto increment value to dstore Datum
  DSTORE::Oid typeOid =
      m_context.m_newHeapRel->attr->attrs[m_context.m_addAutoinc]->atttypid;
  DSTORE::Oid baseTypeOid = CdeConvertDatatypeOid(typeOid);
  DSTORE::Datum *data = &row.m_valuesWithoutVarlen[m_context.m_addAutoinc];
  errno_t err = 0;
  // TODO, wether need to handler unsigned
  switch (baseTypeOid) {
    case INT1OID: {
      err = memcpy_s(data, sizeof(uint8_t), &value, sizeof(uint8_t));
      break;
    }

    case INT2OID: {
      err = memcpy_s(data, sizeof(uint16_t), &value, sizeof(uint16_t));
      break;
    }

    case INT4OID: {
      err = memcpy_s(data, sizeof(uint32_t), &value, sizeof(uint32_t));
      break;
    }

    case FLOAT4OID: {
      /* For columns of type double and float,
      we need to convert them to IEEE floating-point format. */
      float valConverted = static_cast<float>(value);
      err = memcpy_s(data, sizeof(float), &valConverted, sizeof(float));
      break;
    }

    case INT3OID: {
      /* For columns of type MEDIUMINT, we need to convert them to little-endian
       * storage. */
      uint32_t valConverted = CdeReadFrom3LittleEndian(&value);
      err = memcpy_s(data, sizeof(uint64_t), &valConverted, MYSQL_STORE_BYTE3);
      break;
    }

    case FLOAT8OID: {
      /* For columns of type double and float,
      we need to convert them to IEEE floating-point format. */
      double valConverted = static_cast<double>(value);
      err = memcpy_s(data, sizeof(uint64_t), &valConverted, sizeof(uint64_t));
      break;
    }

    case INT8OID: {
      err = memcpy_s(data, sizeof(uint64_t), &value, sizeof(uint64_t));
      break;
    }

    default:
      CDE_ASSERT(false);
      break;
  }

  CDE_ASSERT(err == CDE_OK);
  return CDE_OK;
}

AutoIncreSequence::AutoIncreSequence(THD *thd, uint64_t startValue,
                                     uint64_t maxValue) noexcept
    : m_maxValue(maxValue), m_nextValue(startValue) {
  if (thd != nullptr && m_maxValue > 0) {
    thd_get_autoinc(thd, reinterpret_cast<uint64_t *>(&m_offset),
                    reinterpret_cast<uint64_t *>(&m_increment));

    if (m_increment > 1 || m_offset > 1) {
      /* If there is an offset or increment specified
      then we need to work out the exact next value. */

      m_nextValue =
          CalcNextAutoinc(startValue, 1, m_increment, m_offset, m_maxValue);

    } else if (startValue == 0) {
      /* The next value can never be 0. */
      m_nextValue = 1;
    }
  } else {
    m_eof = true;
  }
}

uint64_t AutoIncreSequence::operator++(int) noexcept {
  const auto current = m_nextValue;

  CDE_ASSERT_DEBUG(!m_eof);
  CDE_ASSERT_DEBUG(m_maxValue > 0);

  m_nextValue = CalcNextAutoinc(current, 1, m_increment, m_offset, m_maxValue);

  if (m_nextValue == m_maxValue && current == m_nextValue) {
    m_eof = true;
  }

  return current;
}
