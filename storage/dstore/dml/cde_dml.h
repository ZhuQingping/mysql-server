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

#ifndef __CDE_DML_H__
#define __CDE_DML_H__

#include "sql/table.h"

#include "common/cde_trxmgr.h"
#include "dml/cde_dml_ctx.h"
#include "handler/ha_cde.h"
#include "page/dstore_itemptr.h"
#include "systable/dstore_relation.h"

namespace CDE {
// todo: Decouple from mysql head file. Data format converting should be removed
// from this api
class cde_dml_api {
 public:
  static int insert_heap(DSTORE::StorageRelation heap_rel,
                         DSTORE::Datum *values, bool *is_nulls,
                         DSTORE::ItemPointerData &ctid);

  static int insert_index(session_index_info *index,
                          DSTORE::Datum *index_values, bool *index_is_nulls,
                          DSTORE::ItemPointer heap_ctid);

  static int update_heap(DSTORE::StorageRelation heap_rel,
                         DSTORE::Datum *values, bool *is_nulls,
                         DSTORE::ItemPointerData &old_ctid,
                         DSTORE::ItemPointerData &new_ctid);

  static int delete_index(session_index_info *index,
                          DSTORE::Datum *index_values, bool *index_is_nulls,
                          DSTORE::ItemPointer heap_ctid);

  static int delete_heap(DSTORE::StorageRelation heap_rel,
                         DSTORE::ItemPointerData &ctid);

  /**
  Reads one record from covering index.

  @param[in,out]  isolationLevel the current transaction isolation level
  @param[in,out]  buf  the buffer to store record to return to MySQL
  @param[in]  table  the TABLE struct containing metadata
  @param[in]  ha  the storage engine handler
  @param[in]  dstoreHandler  dstore handler containing context and state
  @param[in]  snapShotData  lock tuple's snapshot

  @return int Returns CDE_OK on success, or an error code if an error occurs
  (e.g., HA_ERR_END_OF_FILE, HA_ERR_LOCK_TABLE_FULL).
  */
  static int ReadIndexOnly(cde_isolation_level_t isolationLevel, uchar *buf,
                           const TABLE *table, ha_cde *ha,
                           dstore_handler_t *dstoreHandler,
                           DSTORE::SnapshotData *snapShotData);

  /**
  Reads one record from table. Index scan to get ctid first and then scan
  heap to get full record based on ctid. Check (index condition pushdown)ICP
  match or not before go back to heap table if ICP used.

  @param[in,out]  buf  the buffer to store record to return to MySQL
  @param[in]  table  the TABLE struct containing metadata
  @param[in]  ha  the storage engine handler
  @param[in]  dstore_handler  dstore handler containing context and state
  @param[in]  blobMemRoot  MEM_ROOT to handle memory allocations
  @param[in]  allowBlobMemRootClearForReuse allow clear of blobMemRoot
                                            before usage
  @param[in]  snapShotData  lock tuple's snapshot

  @return int Returns 0 on success, or an error code if an error occurs (e.g.,
  HA_ERR_END_OF_FILE, HA_ERR_LOCK_TABLE_FULL).
  */
  static int ReadFullRow(cde_isolation_level_t isolationLevel, uchar *buf,
                         const TABLE *table, ha_cde *ha,
                         dstore_handler_t *dstore_handler,
                         MEM_ROOT *blobMemRoot,
                         bool allowBlobMemRootClearForReuse,
                         DSTORE::SnapshotData *snapShotData);
};

int CdeDmlWriteRow(const TABLE *table, const unsigned char *mysql_ptr,
                   dml_ins_ctx *ins_ctx);

int CdeDmlUpdateRow(const TABLE *table, const unsigned char *mysql_ptr_old,
                    const unsigned char *mysql_ptr_new, dml_upd_ctx *upd_ctx);

bool CdeCheckIndexNeedUpdate(uint32_t *attr_cols, uint32_t index_col_num,
                             bool *is_changed);

void CdeSetBtreeCtxForInsAndDel(
    session_index_info *index, DSTORE::Datum *index_values,
    bool *index_is_nulls, DSTORE::ItemPointer heap_ctid,
    DSTORE::BtreeInsertAndDeleteCommonData &btree_context);

} /* namespace CDE */
#endif  // __CDE_DML_H__
