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

#ifndef CDE_HEAP_BUILDER_H
#define CDE_HEAP_BUILDER_H

#include <stdint.h>
#include <limits>
#include "common/cde_alloc.h"
#include "common/cde_def.h"
#include "common/cde_error.h"
#include "common/cde_errorcode.h"
#include "common/cde_mutex.h"
#include "common/cde_typecache.h"
#include "dict/cde_dict.h"
#include "heap/dstore_heap_interface.h"
#include "sql/sql_class.h"
#include "sql/table.h"

namespace CDE {
/** logical tuple context. */
struct Row {
  DSTORE::HeapTuple *m_tuple{};
  DSTORE::HeapTuple *m_tupleWithLob{};
  DSTORE::Datum *m_valuesWithoutVarlen{};
  bool *m_nulls{};
  DSTORE::ItemPointerData ctid{};
};

/** new table added column field info, exclude virtual column */
struct AddedFieldInfo {
  bool m_isDefaultNull = false;
  DSTORE::Datum m_defaultValueWithoutrVarlen;
  CdeVarlena m_defaultValueWithVarlen;
  uint32_t m_Pos;
  Oid m_typeOid = 0;
};

using AddedFieldInfos = Memory::Vector<AddedFieldInfo>;

using Rows = Memory::Vector<Row>;

/* Batch insert tuples to heap. */
class HeapBatchInserter {
 public:
  HeapBatchInserter(const size_t maxBufferSize,
                    DSTORE::StorageRelation heapRel);
  ~HeapBatchInserter();

  /** Add a tuple to the buffer. If buffer is full,
  insert these tuples to the new heap.
    @param[in,out] tuple            tuple to add.
  */
  int AddRow(DSTORE::HeapTuple *tuple);

  /** Insert a batch of tuples. */
  int InsertRows();

  void CleanUp();

 private:
  DSTORE::HeapInsertHandler *m_insertHander;
  Memory::Vector<DSTORE::HeapTuple *> m_tuples;
  const size_t m_maxBufferSize{};
  size_t m_curBufferUsedSize{};
  DSTORE::StorageRelation m_heapRel{};
};

/** Generate the next autoinc based on a snapshot of the session
auto_increment_increment and auto_increment_offset variables.
Assignment operator would be used during the inplace_alter_table()
phase only **/
class AutoIncreSequence {
 public:
  /** Constructor.
  @param[in,out]  thd           The session
  @param[in] startValue        The lower bound
  @param[in] maxValue          The upper bound (inclusive) */
  AutoIncreSequence(THD *thd, uint64_t startValue, uint64_t maxValue) noexcept;

  /** Destructor. */
  ~AutoIncreSequence() = default;

  /** Postfix increment
  @return the value to insert */
  uint64_t operator++(int) noexcept;

  /** Check if the autoinc "sequence" is exhausted.
  @return true if the sequence is exhausted */
  bool eof() const noexcept { return m_eof; }

  /** Assignment operator to copy the sequence values
  @param[in] rhs                Sequence to copy from */
  AutoIncreSequence &operator=(const AutoIncreSequence &rhs) noexcept {
    if (this == &rhs) {
      return *this;
    }
    CDE_ASSERT_DEBUG(rhs.m_nextValue > 0);
    CDE_ASSERT_DEBUG(rhs.m_maxValue == m_maxValue);
    m_nextValue = rhs.m_nextValue;
    m_increment = rhs.m_increment;
    m_offset = rhs.m_offset;
    m_eof = rhs.m_eof;
    return *this;
  }

  /** @return the next value in the sequence */
  uint64_t Last() const noexcept {
    CDE_ASSERT_DEBUG(m_nextValue > 0);
    return m_nextValue;
  }

  /** Maximum column value if adding an AUTOINC column else 0. Once
  we reach the end of the sequence it will be set to ~0. */
  const uint64_t m_maxValue{};

  /** Value of auto_increment_increment */
  uint64_t m_increment{};

  /** Value of auto_increment_offset */
  uint64_t m_offset{};

  /** Next value in the sequence */
  uint64_t m_nextValue{};

  /** true if no more values left in the sequence */
  bool m_eof{};
};

/** Build heap context/configuration. */
class HeapBuilderContext {
 public:
  static constexpr uint32_t INVALID_COL_NUM =
      std::numeric_limits<uint32_t>::max();

 public:
  HeapBuilderContext(TABLE *oldTable, TABLE *newTable,
                     const cde_dict_t *oldDictTable,
                     const cde_dict_t *newDictTable,
                     const DSTORE::StorageRelation oldHeapRel,
                     const DSTORE::StorageRelation newHeapRel,
                     std::vector<uint32_t> &colMap,
                     std::vector<uint32_t> &nonNull, uint32_t addAutoIncPos,
                     const size_t maxBufferSize, const size_t maxThreads,
                     AutoIncreSequence &sequence);

  ~HeapBuilderContext();

  /** MySQL table for old table. */
  TABLE *m_oldTable{};

  /** MySQL table for new table. */
  TABLE *m_newTable{};

  /** Source dict table, read rows from this table. */
  const cde_dict_t *m_oldDictTable{};

  /** Dict Table where new heap are created */
  const cde_dict_t *m_newDictTable{};

  /** dstore heap relation from old table */
  const DSTORE::StorageRelation m_oldHeapRel{};

  /** dstore heap relation from new table */
  const DSTORE::StorageRelation m_newHeapRel{};

  /** Mapping of old column numbers to new ones, or nullptr if none
  were added. */
  std::vector<uint32_t> m_colMap;

  /** Non null columns. */
  std::vector<uint32_t> m_nonNull;

  /** Maximum number of bytes to use. */
  const size_t m_maxBufferSize{};

  /** Maximum number of threads to use. */
  const size_t m_maxThreads{};

  /** For parallel access to the autoincrement generator. */
  mysql_mutex_t m_autoincMutex;

  /** Number of added AUTO_INCREMENT columns, or MAX if
  none added. */
  uint32_t m_addAutoinc{INVALID_COL_NUM};

  /** Autoinc sequence. */
  AutoIncreSequence &m_sequence;
};

class HeapBuilder {
 public:
  HeapBuilder(TABLE *oldTable, TABLE *newTable, const cde_dict_t *oldDictTable,
              const cde_dict_t *newDictTable,
              const DSTORE::StorageRelation oldHeapRel,
              DSTORE::StorageRelation newHeapRel, std::vector<uint32_t> &colMap,
              std::vector<uint32_t> &nonNull,
              const AddedFieldInfos &addedFieldsInfos, uint32_t addAutoIncPos,
              const size_t maxBufferSize, const size_t maxThreads,
              AutoIncreSequence &sequence, bool isBatchInsertMode);
  ~HeapBuilder();

  /** Build new heap by reading old heap tuples, creating new
  heap tuples, inserting new tuples into new heap with batch insert or single
  insert.
  @return CDE_OK or error code. */
  int BuildHeap();

  /** Callback function for processing each tuple read from old heap
@param[in] tuple              tuple read from old heap.
@param[in] threadIdx          ID of current thread.
@return CDE_OK or error code. */
  int BuildHeapRowCallback(DSTORE::HeapTuple *tuple, uint16_t threadIdx,
                           [[maybe_unused]] void *ctx);

  /** Form the new tuple according to the tuple read from old heap
  @param[in] oldRow              the context of old tuple
  @param[in/out] newRow          the context of form new tuple
  @param[in] threadIdx          ID of current thread.
  @return CDE_OK or error code. */
  int FormNewTuple(Row &oldRow, Row &newRow, uint16_t threadIdx);

  /** Deform the old tuple
  @param[in] tuple               the old tuple to be deform
  @param[in/out] row            the context of tuple to be deform
  @param[in] threadIdx          ID of current thread.
  @return CDE_OK or error code. */
  int DeformOldTuple(DSTORE::HeapTuple *tuple, Row &row, uint16_t threadIdx);

  /** Check if the nonnull columns satisfy the constraint.
  @param[in] isNulls                null bits of the tuple to be check.
  @param[in] threadIdx          ID of current thread.
  @return true on success. */
  bool CheckNullConstraints(bool *const isNulls, uint16_t threadIdx);

  /** Handle auto increment.
  @param[in] row                Row with autoinc column.
  @param[in] threadIdx          ID of current thread.
  @return CDE_OK or error code. */
  int HandleAutoinc(Row &row, uint16_t threadIdx);

 private:
  /** Read a tuple from old heap, construct new tuple and insert it to new heap.
  That is, after processing one tuple from the old heap, it is immediately
  inserted into the new heap.
  @param[in] tuple              tuple read from old heap.
  @param[in] threadIdx          ID of current thread.
  @return CDE_OK or error code. */
  int BuildHeapWithSingleInsert(DSTORE::HeapTuple *tuple, uint16_t threadIdx);

  /** Read tuples from old heap, construct new tuple and add it to cache, when
  the cache is full, insert these tuples into new heap. That is, inserting after
  accumulating a batch of tuples.
  @param[in] tuple              tuple read from old heap.
  @param[in] threadIdx          ID of current thread.
  @return CDE_OK or error code. */
  int BuildHeapWithBatchInsert(DSTORE::HeapTuple *tuple, uint16_t threadIdx);

  /** Start an autonomous transaction and insert a tuple.
  @param[in] threadIdx          ID of current thread.
  @return CDE_OK or error code. */
  int InsertSingleTuple(uint16_t threadIdx);

  HeapBuilderContext m_context;

  /** the tuple context read from old heap of per thread */
  Memory::Vector<Row> m_oldRows;

  /** the tuple context to be insert into new heap of per thread */
  Memory::Vector<Row> m_newRows;

  /** the batch inserter of per thread */
  Memory::Vector<HeapBatchInserter *> m_batchInserters;

  /** error of per thread. */
  Memory::Vector<HaErrorCode> m_errorStates;

  /** StorageRelation of per thread. */
  Memory::Vector<DSTORE::StorageRelation> m_heapRels;

  /** the memeory heap. */
  Memory::MemHeap m_memHeap;

  /** Wether use batch insert when insert tuple to the new heap */
  bool m_isBatchInsertMode{false};
};
}  // namespace CDE
#endif