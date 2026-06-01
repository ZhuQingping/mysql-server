/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 *
 *
 * IDENTIFICATION
 * src/cde_srv.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef __CDE_SRV_H__
#define __CDE_SRV_H__

#include "heap/dstore_heap_interface.h"
#include "heap/dstore_heap_struct.h"
#include "index/dstore_index_interface.h"
#include "index/dstore_index_struct.h"
#include "index/dstore_scankey.h"

#include "dict/cde_dict.h"
#include "dict/cde_relation.h"
namespace CDE {
#define ulint unsigned long

/* Values for row_read_type */
#define ROW_READ_WITH_LOCKS 0
#define ROW_READ_TRY_SEMI_CONSISTENT 1
#define ROW_READ_DID_SEMI_CONSISTENT 2

enum select_mode {
  SELECT_ORDINARY,    /* default behaviour */
  SELECT_SKIP_LOCKED, /* skip the row if row is locked */
  SELECT_NOWAIT       /* return immediately if row is locked */
};

struct dstore_handler_t {
  dstore_handler_t() = default;
  ~dstore_handler_t() {
    if (table_handler != nullptr) {
      DictTableRefGuard dictRefGuard(table_handler);
      dictRefGuard.Release();
      table_handler = nullptr;
    }
    delete rel_info;
    rel_info = nullptr;
  }
  dstore_handler_t(dstore_handler_t &) = delete;
  dstore_handler_t &operator=(dstore_handler_t &) = delete;
  DSTORE::HeapScanHandler *dstore_heap_scan{nullptr};
  session_index_info *current_cde_index{nullptr};
  session_rel_info *rel_info{nullptr};
  SessionAutoincCtx autoincCtx;
  DSTORE::IndexScanHandler *dstore_index_scan{nullptr};
  uint8_t sql_stat_start; /*true, when we start processing of an SQL statement*/
  DSTORE::ItemPointerData dstore_heap_ctid;
  DSTORE::ScanDirection direction;
  key_part_map key_map;
  uint keys_prefix_counts;
  /*!< set to 1 when MySQL calls  handler::extra with the
   * argument HA_EXTRA_KEYREAD;it is enough to read just
   * columns defined in the index (i.e., no read of the
   * clustered index record necessary) */
  unsigned read_just_key : 1;
  /*!< if we are fetching columns through a secondary
   * index and at least one column is not in the
   * secondary index, then this is set to TRUE */
  unsigned need_to_access_clustered : 1;
  bool keep_other_fields_on_keyread; /*!< when using fetch
                        cache with HA_EXTRA_KEYREAD, don't
                        overwrite other fields in mysql row
                        row buffer.*/

  DSTORE::LockMode lock_mode;  // lock table or partition in DSTORE from SQL
                               // engine.

  cde_dict_t *table_handler;

  unsigned clust_index_was_generated : 1; /* whether automatically generated a
                                             clustered index*/
  /** template used to transform rows fast between MySQL and Innobase formats;
  memory for this template is not allocated from 'heap' */
  // mysql_row_templ_t *mysql_template;
  // unsigned n_template : 10;                /*!< number of elements in the
  // template */
  ulint fetch_direction; /*!< ROW_SEL_NEXT or ROW_SEL_PREV */
  bool idx_cond; /*!< True if index condition pushdown is used, false otherwise.
                  */
  ulint row_read_type;
  /*!<
   * ROW_READ_WITH_LOCKS/ROW_READ_TRY_SEMI_CONSISTENT/ROW_READ_TRY_SEMI_CONSISTENT
   */ // dstore ??? semi consistence
  unsigned index_usable : 1;
  ulint hint_need_to_fetch_extra_cols;
  /*!< normally this is set to 0; if this
  is set to ROW_RETRIEVE_PRIMARY_KEY,
  then we should at least retrieve all
  columns in the primary key; if this
  is set to ROW_RETRIEVE_ALL_COLS, then
  we must retrieve all columns in the
  key (if read_just_key == 1), or all
  columns in the table */
  unsigned template_type : 2;   /*!< ROW_MYSQL_WHOLE_ROW,
                                ROW_MYSQL_REC_FIELDS,
                                ROW_MYSQL_DUMMY_TEMPLATE, or
                                ROW_MYSQL_NO_TEMPLATE */
  ulint select_lock_type;       /*!< LOCK_NONE, LOCK_S, or LOCK_X */
  enum select_mode select_mode; /*!< SELECT_ORDINARY, SELECT_SKIP_LOCKED,
                                   SELECT_NOWAIT */

  /** Determines if the query is REPLACE or ON DUPLICATE KEY UPDATE in which
  case duplicate values should be allowed (and further processed) instead of
  causing an error. */
  bool m_allowDuplicates;

  /** Keeping those counters separate per each handler. They can be increased a
  lot during index scan. Thus we initialize them to 0 at index_init. Increase
  them locally for index scan. Then at index_end we increase global perf
  counters by those values. */
  uint64_t m_icpCheckCount = 0;
  uint64_t m_icpMatchCount = 0;

  // Pre-allocated buffers where the handler code can use when converting DStore
  // row format to MySQL row format. These are allocated on MEM_ROOT on handler
  // open(), and deallocated on close(). The buffers have the same length as the
  // number of fields in the table.
  Datum *m_valuesWithVarlen{nullptr};
  Datum *m_valuesWithoutVarlen{nullptr};
  bool *m_nulls{nullptr};
  DSTORE::ScanKey m_keyInfos{nullptr};
  Oid *m_indexColOids{nullptr};
  Oid *m_indexColCollationOids{nullptr};
  DSTORE::ScanKey m_scanAgainKeyInfos{nullptr};

  // The number of elements inside the array "columns_to_decode"
  size_t m_numColumnsToDecode{0};

  // An array of column indexes  that are necessary to decode to fulfill the
  // current query.
  MysqlAndDstoreIndex *m_columnsToDecode{nullptr};

  // The last column's index needed in DeformTuplePart
  uint16_t m_deformEndIndex{0};

  /** In some index read scenarios - like reading a prefixed column from index -
   * we might fallback to reading a whole row from a table. The row in the table
   * will not contain virtual columns. However, the server might still request
   * this virtual fields to be read from the index. In such case, apart from
   * reading a row from a table we also need to read requested virtual fields
   * from the index. At the index_init we check if server requested for any
   * virtual fields to be read - and we set
   * m_readVirutalColsFromIndex. It might be decided right before the
   * read (in index_read) that we are going to fallback to reading from table.
   */
  bool m_readVirutalColsFromIndex{false};

  bool m_needCheckOnlineDdl{false};
  // Decode the row in DStore format to MySQL row format.
  void DecodeDstoreRow(DSTORE::TupleDescData *tupleDesc, DSTORE::Datum *values,
                       DSTORE::HeapTuple *tuple, uchar *buf, const TABLE *table,
                       MEM_ROOT *memRoot,
                       bool allowBlobMemRootClearForReuse = true);
  bool m_needDecodeLobColumns = false;
};
} /* namespace CDE */
#endif  // __CDE_SRV_H__
