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

#include <mutex>
#include <thread>

#include "catalog/dstore_fake_type.h"
#include "common/cde_compare_utils.h"
#include "common/cde_def.h"
#include "common/cde_errorcode.h"
#include "common/cde_prototypes.h"
#include "common/cde_trxmgr.h"
#include "dml/cde_heap.h"
#include "tuple/dstore_tuple_interface.h"

namespace CDE {
uint32_t g_autoincLockMode = AUTOINC_LOCK_INTERLEAVED;

/**
Get autoinc value from datum which stored float in little-endian format.

@param[in]  outerPtr  Pointer to memory which stored the float value.

@return The autoinc value read.
*/
static uint64_t AutoincFromFloatDatum(DSTORE::Datum datum) {
  float val = DatumToFloat(datum);
  if (val < 0) {
    return 0;
  }
  return (uint64_t)val;
}

/**
Get autoinc value from datum which stored double in little-endian format.

@param[in]  outerPtr  Pointer to memory which stored the double value.

@return The autoinc value read.
*/
static uint64_t AutoincFromDoubleDatum(DSTORE::Datum datum) {
  double val = DatumToDouble(datum);
  if (val < 0) {
    return 0;
  }
  if (unlikely(val >=
               static_cast<double>(std::numeric_limits<int64_t>::max()))) {
    return std::numeric_limits<int64_t>::max();
  }
  return (uint64_t)val;
}

/**
Parse integer value from DSTORE::Datum. Data-type must be integer or
floating point, otherwise it will assert.

@param[in]  typeOid  Type-Oid of datum
@param[in]  datum    A datum contains either a value of a pass-by-value
                     type or a pointer to a value of a pass-by-reference
                     type. In this function, it must be pass-by-value type.
@param[in]  isUnsigned  Boolean value indicating whether the type is unsigned.
@return The integer value parsed from the datum.
*/
static uint64_t ParseIntFromDatum(DSTORE::Oid typeOid, DSTORE::Datum datum,
                                  bool isUnsigned) {
  uint64_t autoinc = 0;
  switch (typeOid) {
    case INT1OID:
      if (!isUnsigned && DatumToInt<int8_t>(datum) < 0) {
        autoinc = 0;
        break;
      }
      autoinc = DatumToInt<uint8_t>(datum);
      break;
    case INT2OID:
      if (!isUnsigned && DatumToInt<int16_t>(datum) < 0) {
        autoinc = 0;
        break;
      }
      autoinc = DatumToInt<uint16_t>(datum);
      break;
    case INT4OID:
      if (!isUnsigned && DatumToInt<int32_t>(datum) < 0) {
        autoinc = 0;
        break;
      }
      autoinc = DatumToInt<uint32_t>(datum);
      break;
    case INT8OID:
      if (!isUnsigned && DatumToInt<int64_t>(datum) < 0) {
        autoinc = 0;
        break;
      }
      autoinc = DatumToInt<uint64_t>(datum);
      break;
    case INT3OID:
      if (!isUnsigned && DatumToInt24<int32_t, DSTORE::Datum *>(&datum) < 0) {
        autoinc = 0;
        break;
      }
      autoinc = DatumToInt24<uint32_t, DSTORE::Datum *>(&datum);
      break;
    case FLOAT4OID:
      autoinc = AutoincFromFloatDatum(datum);
      break;
    case FLOAT8OID:
      autoinc = AutoincFromDoubleDatum(datum);
      break;
    default:
      CDE_LOG_ERROR("Parse autoinc failed, type-oid is unsupported:%u",
                    typeOid);
      CDE_ASSERT(0);
  }

  return autoinc;
}

uint64_t ParseIntFromField(const Field *autoincField) {
  DSTORE::Oid baseOid = DSTORE::DSTORE_INVALID_OID;
  DSTORE::Oid expandOid = DSTORE::DSTORE_INVALID_OID;
  bool isUnsigned = false;
  DSTORE::Datum dstorePtr = DSTORE::INVALID_DATUM;
  CdeVarlena tmpPlaceHolder;
  /* MySQL restricts the data type of autoincrement columns to integer or
  floating point. Therefore, the following two functions must return success. */
  (void)CdeDatatypeMysqlToDstore(autoincField, &baseOid, &expandOid, true,
                                 &isUnsigned);
  (void)StoreMysqlFieldToDstoreFormat(autoincField->field_ptr(), baseOid,
                                      autoincField, true, dstorePtr,
                                      tmpPlaceHolder);
  /* We can't use table->found_next_number_field->val_int() to get the
  updated value of autoinc because the field may not be set in readset.*/
  return ParseIntFromDatum(baseOid, dstorePtr, isUnsigned);
}

/**
Load the max value of autoinc from index which contains the autoinc column
when table is opening. During this period of time, there MUST be NO update
on the table.

@param[in]  table         Table struct from SQL-layer.
@param[in]  autoincIndex  Index which contains the autoinc column.
@param[out] value         The max value of autoinc loaded from index.

@return false if success otherwise true
*/
bool LoadMaxAutoincFromIndex(const TABLE *table,
                             DSTORE::StorageRelation indexRelation,
                             const uint32_t indexColNum, uint64_t *value) {
  TempTrxGuard tempTrxGuard(__func__);
  if (tempTrxGuard.IsAbnormal()) {
    CDE_LOG_ERROR("Start temporary trx failed at autoinc initialization.");
    return true;
  }

  uint32_t autoincKeyNo = table->s->next_number_index;
  /** Determine whether the auto-inc column is an acs or desc
  index. MySQL guaranteed the auto-increment column must be the
  first key_part if in an multi-columns index. */
  DSTORE::ScanDirection direction =
      DSTORE::ScanDirection::BACKWARD_SCAN_DIRECTION;
  if (table->key_info[autoincKeyNo].key_part[0].key_part_flag &
      HA_REVERSE_SORT) {
    direction = DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION;
  }

  DSTORE::IndexScanHandler *indexScanHandler =
      IndexInterface::ScanBegin(indexRelation, indexRelation->index, 0, 0);
  if (indexScanHandler == nullptr) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR(
        "Begin scan fail at autoinc initialization.");
    return true;
  }

  DSTORE::SnapshotData snapshot;
  CDESetSnapshotByCurrent(snapshot);
  IndexInterface::IndexScanSetSnapshot(indexScanHandler, &snapshot);
  IndexInterface::ScanSetWantItup(indexScanHandler, true);

  DSTORE::TupleDesc tupleDesc;
  bool recheck = false;
  DSTORE::IndexTuple *itup = IndexInterface::OnlyScanNext(
      indexScanHandler, direction, &tupleDesc, &recheck);
  if (itup != nullptr) {
    std::vector<DSTORE::Datum> values(indexColNum);
    std::vector<char> isNulls(indexColNum);
    TupleInterface::DeformIndexTuple(itup, tupleDesc, values.data(),
                                     (bool *)isNulls.data());
    /* Autoinc column must be the first column of index. */
    DSTORE::Oid typeOid = indexRelation->attr->attrs[0]->atttypid;
    bool isUnsigned = indexRelation->attr->attrs[0]->extAttrs &
                      DSTORE::ExtAttrFlags::UNSIGNED;
    *value = ParseIntFromDatum(typeOid, values[0], isUnsigned);
  } else {
    /* An empty table. */
    *value = 0;
  }

  (void)IndexInterface::ScanEnd(indexScanHandler);
#ifndef NDEBUG
  CDE_LOG_INFO("load table:%s autoinc %lu from index.",
               table->s->table_name.str, *value);
#endif
  tempTrxGuard.commit(__func__);
  return false;
}

bool InitAutoinc(DictAutoinc *autoincDict, const TABLE *table,
                 uint64_t ddAutoIncVal, DSTORE::StorageRelation indexRelation,
                 const uint32_t indexColNum) {
  const Field *autoincField = table->found_next_number_field;
  CDE_ASSERT_DEBUG(!IsVirtualGeneratedField(autoincField));
  autoincDict->MutexEnter();
  if (autoincDict->GetInitialized()) {
    autoincDict->MutexExit();
    return false;
  }

  uint64_t autoincValue = 0;
  if (unlikely(LoadMaxAutoincFromIndex(table, indexRelation, indexColNum,
                                       &autoincValue))) {
    autoincDict->MutexExit();
    return true;
  }
  CDE_LOG_DEBUG("get autoinc value[%lu] from index", autoincValue);

  /* At the this stage we do not know the increment
  nor the offset, so use a default increment of 1. */
  if (ddAutoIncVal > autoincValue)
    autoincDict->SetAutoinc(ddAutoIncVal);
  else
    autoincDict->SetAutoinc(CalcNextAutoinc(autoincValue, 1, 1, 0,
                                            autoincField->get_max_int_value()));
  autoincDict->SetInitialized(true);
  autoincDict->MutexExit();
  return false;
}

void LockAutoinc(DictAutoinc *autoincDict, TrxTableSet &tableSet, int command,
                 cde_dict_t *dictTable) {
  switch (g_autoincLockMode) {
    case AUTOINC_LOCK_INTERLEAVED:
      autoincDict->MutexEnter();
      break;
    case AUTOINC_LOCK_CONSECUTIVE:
      /* For simple (single/multi) row INSERTs, we fallback to
      the AUTOINC_LOCK_TRADITIONAL only if another transaction
      has already acquired the AUTOINC lock on behalf of a LOAD
      FILE or INSERT ... SELECT etc. type of statement. */
      if (command == SQLCOM_INSERT || command == SQLCOM_REPLACE) {
        autoincDict->MutexEnter();
        /* Check that another transaction isn't already holding
        the AUTOINC lock on the table. */
        if (autoincDict->TableAutoincLockedByOthers()) {
          /* Release the mutex to avoid deadlocks. */
          autoincDict->MutexExit();
        } else {
          break;
        }
      }
      /* Fall through to AUTOINC_LOCK_TRADITIONAL mode. */
      [[fallthrough]];
    case AUTOINC_LOCK_TRADITIONAL:
      autoincDict->TableAutoincLock();
      tableSet.insert(dictTable);
      break;
    default:
      /* Should never happen. */
      CDE_ASSERT(0);
  }
}

void UpdateTableAutoinc(SessionAutoincCtx *autoincCtx, DictAutoinc *autoincDict,
                        uint64_t autoinc, uint64_t colMaxValue) {
  /* There are 2 situations that autoinc bigger than colMaxValue at here:
  1) The value of unsigned 'autoinc' is casted from a signed value of field
  witch represent the value set explicitly by the user, the 'autoinc' will
  bigger than colMaxValue, we need filter out these negative value.
  2) The value of unsigned 'autoinc' is casted from a float-point field. For
  float-point type, the colMaxValue is defined by Field_float::get_max_int_value
  or Field_double::get_max_int_value, which may not match with the actul values
  casted from field. */
  if (autoinc <= colMaxValue) {
    CDE_ASSERT(autoincCtx->m_autoincIncrement > 0);
    autoinc = CalcNextAutoinc(autoinc, 1, autoincCtx->m_autoincIncrement,
                              autoincCtx->m_autoincOffset, colMaxValue);
    autoincDict->MutexEnter();
    autoincDict->SetAutoincIfGreater(autoinc);
    autoincDict->MutexExit();
  }
}

uint64_t CalcNextAutoinc(uint64_t current, uint64_t desireCnt, uint64_t step,
                         uint64_t offset, uint64_t colMaxValue) {
  CDE_ASSERT(desireCnt > 0);
  CDE_ASSERT(step > 0);
  CDE_ASSERT(colMaxValue > 0);

  uint64_t nextValue = 0;
  uint64_t block = desireCnt * step;

  /* According to MySQL documentation, if the offset is greater than
  the step then the offset is ignored. */
  if (offset > block) {
    offset = 0;
  }

  /* Check for overflow. Current can be > colMaxValue if the value is
  in reality a negative value.The visual studio compilers converts
  large double values automatically into unsigned long long datatype
  maximum value */

  if (block >= colMaxValue || offset > colMaxValue || current >= colMaxValue ||
      colMaxValue - offset <= offset) {
    nextValue = colMaxValue;
  } else {
    CDE_ASSERT(colMaxValue > current);
    uint64_t free = colMaxValue - current;

    if (free < offset || free - offset <= block) {
      nextValue = colMaxValue;
    } else {
      nextValue = 0;
    }
  }

  if (nextValue == 0) {
    uint64_t next;
    if (current > offset) {
      next = (current - offset) / step;
    } else {
      next = (offset - current) / step;
    }

    CDE_ASSERT(colMaxValue > next);
    nextValue = next * step;
    /* Check for multiplication overflow. */
    CDE_ASSERT(nextValue >= next);
    CDE_ASSERT(colMaxValue > nextValue);

    /* Check for overflow */
    if (colMaxValue - nextValue >= block) {
      nextValue += block;

      if (colMaxValue - nextValue >= offset) {
        nextValue += offset;
      } else {
        nextValue = colMaxValue;
      }
    } else {
      nextValue = colMaxValue;
    }
  }

  CDE_ASSERT(nextValue != 0);
  CDE_ASSERT(nextValue <= colMaxValue);
  return nextValue;
}
} /* namespace CDE */
