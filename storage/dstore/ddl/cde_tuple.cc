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

#include "cde_tuple.h"
#include "common/cde_error.h"
#include "common/cde_typecache.h"
#include "dd_helper.h"
#include "dml/cde_heap.h"

#include "catalog/dstore_fake_attribute.h"
#include "common/cde_compare_utils.h"
#include "dd_table_share.h"
#include "sql/dd/types/table.h"

using DSTORE::ATTRIBUTE_FIXED_PART_SIZE;
using DSTORE::DstoreTupInitDefVal;
using DSTORE::Form_pg_attribute;
using DSTORE::FormData_pg_attribute;
using DSTORE::SysAttributeTupDef;
using DSTORE::TupleDesc;
using DSTORE::TupleDescData;
namespace CDE {

size_t cde_tuple_desc::get_tuple_desc_size(uint32_t col_nums) {
  return MAXALIGN((uint32_t)(sizeof(TupleDescData) +
                             col_nums * sizeof(SysAttributeTupDef *)));
}

size_t cde_tuple_desc::get_fixed_attr_size() {
  return static_cast<uint32_t>(MAXALIGN(ATTRIBUTE_FIXED_PART_SIZE));
}

size_t cde_tuple_desc::get_attr_size(uint32_t col_nums) {
  return col_nums * get_fixed_attr_size();
}

size_t cde_tuple_desc::GetDefValEntrySize() {
  return MAXALIGN(sizeof(DstoreTupInitDefVal));
}

size_t cde_tuple_desc::GetInitDefValSize(uint32_t colNum) {
  return colNum * GetDefValEntrySize();
}

TupleDesc cde_tuple_desc::GenerateTupleDescTemplate(uint32_t colNums,
                                                    bool needInitDefVals) {
  size_t descSize = get_tuple_desc_size(colNums);
  size_t attrsSize = get_attr_size(colNums);
  size_t totalSize = descSize + attrsSize;
  if (needInitDefVals) {
    size_t initDefValSize = GetInitDefValSize(colNums);
    size_t datumArraySize = sizeof(Datum) * colNums;
    totalSize += initDefValSize + datumArraySize;
  }
  char *offset = static_cast<char *>(CdeZalloc(sizeof(char) * totalSize));
  if (offset == nullptr) {
    CDE_LOG_ERROR("CdeZalloc fail! offset is nullptr");
    return nullptr;
  }
  auto desc = (TupleDesc)offset;

  desc->natts = colNums;
  desc->tdisredistable = false;
  desc->attrs = (Form_pg_attribute *)(offset + sizeof(TupleDescData));
  desc->initdefvals = nullptr;
  desc->tdtypeid = RECORDOID;
  desc->tdtypmod = -1;
  desc->tdhasoid = false;
  desc->tdrefcount = -1;
  desc->tdhasuids = false;

  offset += descSize;
  for (int i = 0; i < desc->natts; i++) {
    desc->attrs[i] = (Form_pg_attribute)offset;
    offset += MAXALIGN(ATTRIBUTE_FIXED_PART_SIZE);
  }

  desc->initdefvals = nullptr;

  if (needInitDefVals) {
    desc->initdefvals = reinterpret_cast<DstoreTupInitDefVal *>(offset);
    /* |defval[0] | defval[1] | ..defval[n] | [datum0] | [datum1].. */
    offset += colNums * GetDefValEntrySize();
    for (int i = 0; i < desc->natts; i++) {
      desc->initdefvals[i].datum = (Datum *)(offset);
      offset += sizeof(Datum);
    }
  }

  return desc;
}

#define isVirtualFld(field) ((field)->gcol_info && !(field)->stored_in_db)

TupleDesc cde_tuple_desc::build_heap_tuple_desc(Oid rel_oid,
                                                const TABLE *m_form,
                                                bool skipVirtual) {
  uint32_t numberOfColumns = m_form->s->fields;
  if (skipVirtual) {
    uint32_t numberOfVColumns = 0;
    for (uint32_t i = 0; i < m_form->s->fields; ++i)
      if (isVirtualFld(m_form->field[i])) ++numberOfVColumns;
    numberOfColumns -= numberOfVColumns;
  }

  TupleDesc heap_desc = GenerateTupleDescTemplate(numberOfColumns);

  for (uint32_t i = 0, colNum = 0; i < m_form->s->fields; i++) {
    if (isVirtualFld(m_form->field[i]) && skipVirtual) continue;

    heap_desc->attrs[colNum]->attrelid = rel_oid;
    Field *mysql_field = m_form->field[i];
    DSTORE::Oid base_oid, expand_oid;
    bool isUnsigned = false;
    int ret = CdeDatatypeMysqlToDstore(mysql_field, &base_oid, &expand_oid,
                                       true, &isUnsigned);
    if (ret != CDE_OK) {
      CDE_LOG_ERROR("CdeDatatypeMysqlToDstore fail! ret = %d", ret);
      return nullptr;
    }
    heap_desc->attrs[colNum]->atttypid = expand_oid;
    // not use dstore col name, heap_desc->attrs[i]->attname.data
    const DSTORE::TypeCache *type_cache = CdeGetTypeCache(base_oid);
    heap_desc->attrs[colNum]->attlen =
        CdeGetTypeRealLen(base_oid, type_cache, mysql_field->pack_length());
    heap_desc->attrs[colNum]->attbyval = type_cache->attbyval;
    heap_desc->attrs[colNum]->attalign = type_cache->attalign;
    heap_desc->attrs[colNum]->attcacheoff = -1;
    heap_desc->attrs[colNum]->attstattarget = 0;
    heap_desc->attrs[colNum]->attnum = i;  // from 0
    heap_desc->attrs[colNum]->attndims = 0;
    heap_desc->attrs[colNum]->attcacheoff = -1;
    heap_desc->attrs[colNum]->attstorage = 'p';
    heap_desc->attrs[colNum]->attnotnull =
        !mysql_field->is_nullable();              // nullable
    heap_desc->attrs[colNum]->atthasdef = false;  // defalut
    heap_desc->attrs[colNum]->attisdropped = false;
    heap_desc->attrs[colNum]->attislocal = false;
    heap_desc->attrs[colNum]->attcmprmode = 0;
    heap_desc->attrs[colNum]->attinhcount = 0;
    heap_desc->attrs[colNum]->attcollation = mysql_field->charset()->number;
    heap_desc->attrs[colNum]->extAttrs =
        (isUnsigned ? DSTORE::ExtAttrFlags::UNSIGNED
                    : DSTORE::ExtAttrFlags::NONE) |
        DSTORE::ExtAttrFlags::EXACTCMP;
#ifdef CATALOG_VARLEN
    heap_desc->attrs[colNum]->attkvtype = 0;
    heap_desc->attrs[colNum]->attidentity = 0;
#endif
    ++colNum;
  }
  return heap_desc;
}

/**
Build tupledesc default value from dictcol default value, simple type like
int/uint just store the value into datum, complext type like varchar, store
the address which point to dictcol's real content memory.

@param[in]      dictColDefault   Dict col default value which load from dd
@param[in]      attr             Attrbuite of this column in dstore
@param[in,out]  tupleDefault     Tuple column's default value
*/
void BuildTupleDefaultVals(DictColDefaultVal *dictColDefault,
                           FormData_pg_attribute *attr,
                           DstoreTupInitDefVal *tupleDefault) {
  if (attr->attbyval) {
    Oid base_type_oid = CdeConvertDatatypeOid(attr->atttypid);
    bool isUnsigned = attr->extAttrs & DSTORE::ExtAttrFlags::UNSIGNED;
    switch (base_type_oid) {
      case INT1OID:
        if (isUnsigned) {
          *(tupleDefault->datum) = *(const uint8_t *)dictColDefault->m_value;
        } else {
          *(tupleDefault->datum) = *(const int8_t *)dictColDefault->m_value;
        }
        break;
      case INT2OID:
        if (isUnsigned) {
          *(tupleDefault->datum) = *(const uint16_t *)dictColDefault->m_value;
        } else {
          *(tupleDefault->datum) = *(const int16_t *)dictColDefault->m_value;
        }
        break;
      case INT3OID:
        (void)memcpy_s((unsigned char *)(tupleDefault->datum),
                       dictColDefault->m_len, dictColDefault->m_value,
                       dictColDefault->m_len);
        break;
      case INT4OID:
        if (isUnsigned) {
          *(tupleDefault->datum) = *(const uint32_t *)dictColDefault->m_value;
        } else {
          *(tupleDefault->datum) = *(const int32_t *)dictColDefault->m_value;
        }
        break;
      case FLOAT4OID:
        *(tupleDefault->datum) = *(const int32_t *)dictColDefault->m_value;
        break;
      case FLOAT8OID:
        *(tupleDefault->datum) = *(const int64_t *)dictColDefault->m_value;
        break;
      case INT8OID:
        if (isUnsigned) {
          *(tupleDefault->datum) = *(const uint64_t *)dictColDefault->m_value;
        } else {
          *(tupleDefault->datum) = *(const int64_t *)dictColDefault->m_value;
        }
        break;
      case DATEOID:
        ConvertMysqlDateBinaryToDstore(dictColDefault->m_value,
                                       dictColDefault->m_len,
                                       *(tupleDefault->datum));
        break;
      case TIMESTAMPOID:
      case TIMESTAMPTZOID:
        ConvertMysqlTimestampBinaryToDstore(dictColDefault->m_value,
                                            dictColDefault->m_len,
                                            *(tupleDefault->datum));
        break;
      default:
        CDE_ASSERT(0);
        break;
    }
  } else {
    /* Complex type like varchar, datatime just store the pointer to
    real content. */
    tupleDefault->datum = (Datum *)dictColDefault->m_value;
  }
}

static bool BuildColumnCommonAttribute(const dd::Column *ddCol,
                                       FormData_pg_attribute *attr) {
  DSTORE::Oid baseOid, expandOid;
  bool isUnsigned = false;
  int ret = DDTypeToDstore(ddCol, &baseOid, &expandOid, true, &isUnsigned);
  if (ret != CDE_OK) {
    return false;
  }
  attr->atttypid = expandOid;
  const DSTORE::TypeCache *typeCache = CdeGetTypeCache(baseOid);
  attr->attlen =
      CdeGetTypeRealLen(baseOid, typeCache, DDGetColumnPackLength(ddCol));
  attr->attbyval = typeCache->attbyval;
  attr->attalign = typeCache->attalign;
  attr->attcacheoff = -1;
  attr->attstattarget = 0;
  attr->attndims = 0;
  attr->attcacheoff = -1;
  attr->attstorage = 'p';
  attr->attnotnull = !ddCol->is_nullable();  // nullable
  attr->atthasdef = false;                   // defalut
  attr->attisdropped = false;
  attr->attislocal = false;
  attr->attcmprmode = 0;
  attr->attinhcount = 0;
  attr->attcollation = GetColumnCharsetNo(ddCol);
  attr->extAttrs = (isUnsigned ? DSTORE::ExtAttrFlags::UNSIGNED
                               : DSTORE::ExtAttrFlags::NONE) |
                   DSTORE::ExtAttrFlags::EXACTCMP;
#ifdef CATALOG_VARLEN
  attr->attkvtype = 0;
  attr->attidentity = 0;
#endif
  return true;
}

/**
Build tupledesc from dd table, since TABLE from server does not include
columns which has been dropped instantly, but we need those dropped
column's info for dml right now.

@param[in]      relOid   Oid of heap relation to build
@param[in]      ddTable  DD table used to build tupledesc
@param[in]      dictCols Dict col array used to fill tuple init def vals

@return TupleDesc
*/
TupleDesc cde_tuple_desc::BuildHeapTupleDescFromDD(DSTORE::Oid relOid,
                                                   const dd::Table *ddTable,
                                                   DictCol *dictCols,
                                                   bool skipVirtual) {
  uint32_t colNums = ddTable->columns().size();

  uint32_t numberOfVColumns = 0;
  for (const dd::Column *ddCol : ddTable->columns())
    if (ddCol->is_virtual()) ++numberOfVColumns;

  if (skipVirtual) {
    colNums -= numberOfVColumns;
  }

  TupleDesc heapDesc = GenerateTupleDescTemplate(colNums, true);
  int i = 0, j = 0, k = 0;
  for (const dd::Column *ddCol : ddTable->columns()) {
    if (skipVirtual && ddCol->is_virtual()) {
      ++j;
      continue;
    }

    if (ddCol->is_virtual()) {
      i = colNums - numberOfVColumns + k;
      ++k;
    } else {
      i = dictCols[j].m_phyPos;
    }

    heapDesc->attrs[i]->attrelid = relOid;

    if (!BuildColumnCommonAttribute(ddCol, heapDesc->attrs[i])) {
      CDE_LOG_ERROR("DDTypeToDstore failed for table: %s, datatype: %d!",
                    ddTable->name().c_str(), (int)ddCol->type());
      CdeFree(heapDesc);
      return nullptr;
    }
    DSTORE::Oid baseOid, expandOid;
    bool isUnsigned = false;
    int ret = DDTypeToDstore(ddCol, &baseOid, &expandOid, true, &isUnsigned);
    if (ret != CDE_OK) {
      CDE_LOG_ERROR(
          "DDTypeToDstore failed for table: %s, datatype: %d! ret: %d",
          ddTable->name().c_str(), (int)ddCol->type(), ret);
      CdeFree(heapDesc);
    }

    heapDesc->attrs[i]->attnum = i;  // from 0
    heapDesc->initdefvals[i].isNull = DDColDefaultIsNull(ddCol);
    if (dictCols && dictCols[j].m_versionAdded > 0 &&
        !heapDesc->initdefvals[i].isNull) {
      BuildTupleDefaultVals(&dictCols[j].m_instantDefaultVal,
                            heapDesc->attrs[i], &heapDesc->initdefvals[i]);
    }

    j++;
  }

  return heapDesc;
}

TupleDesc cde_tuple_desc::build_index_tuple_desc(
    Oid index_oid, TupleDesc heap_tuple_desc, const KEY *key,
    uint32_t *index_cols, uint32_t *attrCols, DictCol *dictCols) {
  int num_key_attrs = (int)key->user_defined_key_parts;
  TupleDesc index_desc = GenerateTupleDescTemplate(num_key_attrs, false);
  for (int i = 0; i < num_key_attrs; i++) {
    KEY_PART_INFO *key_part = key->key_part + i;
    CDE_ASSERT(key_part != nullptr && key_part->field != nullptr);
    uint col_no = key_part->field->field_index();
    index_cols[i] = col_no;
    attrCols[i] = dictCols[col_no].m_phyPos;
    errno_t rc = memcpy_s(index_desc->attrs[i], sizeof(SysAttributeTupDef),
                          heap_tuple_desc->attrs[attrCols[i]],
                          sizeof(SysAttributeTupDef));
    if (rc != 0) {
      return nullptr;
    }
    index_desc->attrs[i]->attrelid = index_oid;
    index_desc->attrs[i]->attnum = i + 1;  // from 1
  }
  return index_desc;
}

TupleDesc cde_tuple_desc::build_index_tuple_desc(
    Oid index_oid, TupleDesc heap_tuple_desc, const cde_field_def *field_def,
    uint32_t n_fields, uint32_t *index_cols, uint32_t *attrCols,
    DictCol *dictCols) {
  errno_t rc = 0;
  TupleDesc index_desc = GenerateTupleDescTemplate(n_fields, false);
  for (uint32_t i = 0; i < n_fields; i++) {
    index_cols[i] = field_def[i].col_no;
    if (field_def[i].vCol) {
      attrCols[i] = heap_tuple_desc->natts + field_def[i].virtualPos;
      if (!BuildColumnCommonAttribute(field_def[i].ddCol,
                                      index_desc->attrs[i])) {
        return nullptr;
      }
    } else {
      attrCols[i] = dictCols[field_def[i].dict_col_no].m_phyPos;
      rc = memcpy_s(index_desc->attrs[i], sizeof(SysAttributeTupDef),
                    heap_tuple_desc->attrs[attrCols[i]],
                    sizeof(SysAttributeTupDef));
    }

    if (rc != 0) {
      return nullptr;
    }
    index_desc->attrs[i]->attrelid = index_oid;
    index_desc->attrs[i]->attnum = i + 1;  // from 1
  }
  return index_desc;
}

TupleDesc cde_tuple_desc::clone(TupleDesc src_desc, MEM_ROOT *mem_root) {
  /*
   * Allocate enough memory for the tuple descriptor, including the
   * attribute rows, and set up the attribute row pointers.
   *
   * Note: we assume that sizeof(struct tupleDesc) is a multiple of the
   * struct pointer alignment requirement, and hence we don't need to insert
   * alignment padding between the struct and the array of attribute row
   * pointers.
   *
   * Note: Only the fixed part of pg_attribute rows is included in tuple
   * descriptors, so we only need ATTRIBUTE_FIXED_PART_SIZE space per attr.
   * That might need alignment padding, however.
   *
   * Note: For initdefvals, here we didn't alloc memory for each column, just
   * point to the same address as src_desc, since we thought that initdefvals
   * will not change during dml,all handler will be released and reopen if
   * column defautlt value is changed.
   */
  int natts = src_desc->natts;
  size_t desc_size = cde_tuple_desc::get_tuple_desc_size(natts);
  size_t attrs_size = cde_tuple_desc::get_attr_size(natts);
  size_t alloc_len = desc_size + attrs_size;
  char *stg = static_cast<char *>(mem_root->Alloc(alloc_len));
  if (unlikely(stg == nullptr)) {
    return nullptr;
  }
  TupleDescData *dst_desc =
      static_cast<TupleDescData *>(static_cast<void *>(stg));
  *dst_desc = *src_desc;

  if (natts > 0) {
    dst_desc->attrs = static_cast<Form_pg_attribute *>(
        static_cast<void *>(stg + sizeof(TupleDescData)));
    stg += desc_size;
    size_t fixed_attr_size = cde_tuple_desc::get_fixed_attr_size();
    for (int i = 0; i < natts; i++) {
      dst_desc->attrs[i] =
          static_cast<Form_pg_attribute>(static_cast<void *>(stg));
      (void)memcpy_s(dst_desc->attrs[i], fixed_attr_size, src_desc->attrs[i],
                     fixed_attr_size);
      stg += fixed_attr_size;
    }
  } else {
    dst_desc->attrs = nullptr;
  }

  /* todo : Whether need alloc sepearte memory for each session specific desc,
  just set the same addr as src right now, no matter src is null or not. */
  dst_desc->initdefvals = src_desc->initdefvals;

  /*
   * Also, assume the destination is not to be ref-counted.  (Copying the
   * source's refcount would be wrong in any case.)
   */
  dst_desc->tdrefcount = -1;

  return dst_desc;
}
} /* namespace CDE */
