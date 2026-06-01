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

#ifndef __CDE_DML_CTX_H__
#define __CDE_DML_CTX_H__

// clang-format off
#include "heap/dstore_heap_interface.h"
#include "index/dstore_index_interface.h"

#include "common/cde_alloc.h"
#include "common/cde_session.h"
#include "common/cde_typecache.h"
#include "dict/cde_dict.h"
#include "dict/cde_relation.h"
#include "dml/cde_btree.h"
#include "common/cde_relation_cache.h"
// clang-format on
namespace CDE {
#define MAX_CASCADE_DEPTH (15)

struct dml_tuple_t {
  DSTORE::Datum *values{nullptr};
  CdeVarlena *varlenas{nullptr};
  bool *is_nulls{nullptr};
  bool *is_blob{nullptr};
  DSTORE::ItemPointerData ctid{DSTORE::INVALID_ITEM_POINTER};
};

struct dml_index_tuple_t {
  Datum *index_values;
  bool *index_is_nulls;
  CdeVarlena *index_varlenas;
  DSTORE::ScanKeyData *key_infos;
  Oid *col_oids;
  Oid *col_collation_oids;
};

struct dstore_scan_ctx {
  DSTORE::IndexTuple *m_cur_ituple;
  DSTORE::HeapScanHandler *m_heap_scan;
};

class dml_upd_task;

enum dml_ctx_type_e {
  CTX_TYPE_NONE = 0,
  CTX_TYPE_READ,
  CTX_TYPE_INSERT,
  CTX_TYPE_UPDATE,
  CTX_TYPE_DELETE,
};

enum dml_task_state_e {
  TASK_RUNNING = 0,
  TASK_PROCEDURE_WAIT,
  TASK_COMPLETED,
  TASK_COMMAND_WAIT,
  TASK_LOCK_WAIT,
  TASK_SUSPENDED
};

static_assert((sizeof(dml_tuple_t) % CDE_MEM_ALIGNMENT) == 0);
static_assert((sizeof(DSTORE::Datum) % CDE_MEM_ALIGNMENT) == 0);
static_assert((sizeof(CdeVarlena) % CDE_MEM_ALIGNMENT) == 0);
static_assert((sizeof(dml_index_tuple_t) % CDE_MEM_ALIGNMENT) == 0);
static_assert((sizeof(DSTORE::ScanKeyData) % CDE_MEM_ALIGNMENT) == 0);

class dml_ctx {
 public:
  dml_ctx(cde_trxinfo_t *trxinfo, cde_dict_t *table, session_rel_info *rel,
          uint32_t fields_num, uint32_t vfields_num, THD *current_thd)
      : m_trxinfo(trxinfo),
        m_table(table),
        m_rel(rel),
        m_fields_num(fields_num),
        m_vfields_num(vfields_num),
        m_current_thd(current_thd) {}

  virtual ~dml_ctx() {
    if (m_full_tuple && m_full_tuple != m_tuple) {
      for (uint32_t i = 0; i < m_fields_num; i++) {
        if (m_full_tuple->is_blob[i] &&
            m_full_tuple->varlenas[i].data != nullptr) {
          CdeFree(const_cast<unsigned char *>(m_full_tuple->varlenas[i].data));
          m_full_tuple->varlenas[i].data = nullptr;
        }
      }
    }
    if (m_tuple) {
      for (uint32_t i = 0; i < m_fields_num - m_vfields_num; i++) {
        if (m_tuple->is_blob[i] && m_tuple->varlenas[i].data != nullptr) {
          CdeFree(const_cast<unsigned char *>(m_tuple->varlenas[i].data));
          m_tuple->varlenas[i].data = nullptr;
        }
      }
      CdeFree(m_tuple);
    }
    if (nullptr != m_current_thd) {
      RelationCache *cache =
          relationCacheManager.getCache(m_current_thd->thread_id());
      std::lock_guard<RelationCache> lock(*cache);
      for (auto relation : m_relationsInUse)
        cache->releaseRelation(relation.second);
      m_current_thd = nullptr;
    }
  }

  dml_ctx(dml_ctx &) = delete;
  dml_ctx &operator=(dml_ctx &) = delete;
  cde_trxinfo_t *get_trxinfo() { return m_trxinfo; }
  cde_dict_t *get_table() { return m_table; }
  session_rel_info *get_rel() { return m_rel; }
  session_index_info *get_index() { return m_index; }
  uint32_t get_fields_num() { return m_fields_num; }

  dml_ctx_type_e get_ctx_type() { return m_ctx_type; }

  THD *get_current_thd() { return m_current_thd; }

  void set_index(session_index_info *index) { m_index = index; }

  inline size_t calc_full_tuple_mem_size() const {
    return sizeof(dml_tuple_t) +
           m_fields_num * (sizeof(DSTORE::Datum) + sizeof(CdeVarlena) +
                           sizeof(bool) + sizeof(bool));
  }

  inline size_t calc_tuple_mem_size() const {
    size_t numberOfFields = m_fields_num - m_vfields_num;
    return sizeof(dml_tuple_t) +
           numberOfFields * (sizeof(DSTORE::Datum) + sizeof(CdeVarlena) +
                             sizeof(bool) + sizeof(bool));
  }

  inline size_t calc_index_tuple_mem_size() const {
    return sizeof(dml_index_tuple_t) +
           m_fields_num * (sizeof(DSTORE::Datum) + sizeof(bool) +
                           sizeof(CdeVarlena) + sizeof(DSTORE::ScanKeyData) +
                           sizeof(DSTORE::Oid) + sizeof(DSTORE::Oid)) +
           CDE_MEM_ALIGNMENT;
  }

  char *init_full_tuple(dml_tuple_t *&tuple, char *buf);
  char *init_tuple(dml_tuple_t *&tuple, char *buf);
  char *init_index_tuple(dml_index_tuple_t *&index_tuple, char *buf);
  virtual int init() = 0;

  dml_tuple_t *tuple() { return m_tuple; }
  dml_tuple_t *full_tuple() { return m_full_tuple; }
  dml_index_tuple_t *index_tuple() { return m_index_tuple; }

  /** Retrieve relation for a given table. In order:
    - try to get relation from this context's cache, if not then:
    - try to get relation from global relations cache, if not then
    - copy reltion from global dict sys and use it, also add it to
      the global cache for later reuse - by this or other thread

    @param[in]  checkTable  table for which retrieve relation

    @return retrieved relation
  */
  session_rel_info *getRelation(cde_dict_t *checkTable);
  bool NeedCheckOnlineDdl() { return m_needCheckOnlineDdl; }
  void SetNeedCheckOnlineDdl(bool val) { m_needCheckOnlineDdl = val; }

 protected:
  /** get relation from local context's cache
    @param[in]  tableId table id for which retrieve the relation
    @return relation from local cache or nullptr if
            relation is absent in local cache
  */
  inline session_rel_info *getPinnedRelation(uint64_t tableId) {
    return m_relationsInUse.count(tableId) != 0 ? m_relationsInUse[tableId]
                                                : nullptr;
  }
  /** add relation to local context's cache
    @param[in] relInfo  relation

    @return relation from local cache or nullptr if
            relation is absent in local cache
  */
  inline void pinRelation(session_rel_info *relInfo) {
    CDE_ASSERT_DEBUG(m_relationsInUse.count(relInfo->m_tableId) == 0);
    m_relationsInUse[relInfo->m_tableId] = relInfo;
  }
  dml_ctx_type_e m_ctx_type{CTX_TYPE_NONE};
  dml_tuple_t *m_tuple{nullptr};
  dml_tuple_t *m_full_tuple{nullptr};
  dml_index_tuple_t *m_index_tuple{nullptr};
  cde_trxinfo_t *m_trxinfo{nullptr};
  cde_dict_t *m_table{nullptr};
  session_rel_info *m_rel;
  session_index_info *m_index{nullptr};
  uint32_t m_fields_num{0};
  uint32_t m_vfields_num{0};
  THD *m_current_thd{nullptr};
  std::unordered_map<uint64_t, session_rel_info *> m_relationsInUse;
  bool m_needCheckOnlineDdl{false};
};

class dml_ins_ctx : public dml_ctx {
 public:
  dml_ins_ctx(cde_trxinfo_t *trxinfo, cde_dict_t *table, session_rel_info *rel,
              uint32_t fields_num, uint32_t vfields_num, THD *current_thd)
      : dml_ctx(trxinfo, table, rel, fields_num, vfields_num, current_thd) {
    m_ctx_type = CTX_TYPE_INSERT;
  }
  ~dml_ins_ctx() { m_foreign = nullptr; }

  virtual int init() override;

  void set_foreign(cde_dict_foreign_t *foreign) { m_foreign = foreign; }
  cde_dict_foreign_t *get_foreign() { return m_foreign; }

  bool check_has_null(uint32_t col_nums);

  bool need_check_foreign() { return m_trxinfo->check_foreigns; }

  void set_err_index(cde_dict_index_t *err_index) {
    get_trxinfo()->err_index = err_index;
  }

  int fill_index_tuple(uint32_t fk_col_nums, dml_ins_ctx *parent_ctx);

  int create_heap_scan();

  int fetch_tuple(DSTORE::ItemPointer ctid, DSTORE::HeapTuple *&tuple,
                  DSTORE::HeapTuple *&realTuple,
                  DSTORE::HeapTuple *&tupleWithLob);

  void destroy_heap_scan();

  int create_index_scan(session_index_info *index, uint32_t col_num);

  int index_scan_next(DSTORE::ItemPointer &p_ctid,
                      DSTORE::ScanDirection direction);

  void destroy_index_scan();

 protected:
  cde_dict_foreign_t *m_foreign{nullptr};
  DSTORE::HeapScanHandler *m_heap_scan{nullptr};
  DSTORE::IndexScanHandler *m_index_scan{nullptr};
};

class dml_upd_ctx : public dml_ins_ctx {
 public:
  dml_upd_ctx(cde_trxinfo_t *trxinfo, cde_dict_t *table, session_rel_info *rel,
              uint32_t fields_num, uint32_t vfields_num, bool is_delete,
              THD *current_thd, MEM_ROOT *blobMemRoot, TABLE *mysqlTable)
      : dml_ins_ctx(trxinfo, table, rel, fields_num, vfields_num,
                    current_thd),  // for not it's 0
        m_is_delete(is_delete),
        m_blobMemRoot(blobMemRoot),
        m_mysqlTable(mysqlTable) {
    m_ctx_type = is_delete ? CTX_TYPE_DELETE : CTX_TYPE_UPDATE;
  }

  ~dml_upd_ctx();

  int init() final;

  void set_parent(dml_upd_ctx *parent) { m_parent = parent; }
  dml_upd_ctx *get_parent() { return m_parent; }

  void set_cascade(dml_upd_ctx *cascade) { m_cascade = cascade; }
  dml_upd_ctx *get_cascade() { return m_cascade; }

  bool is_delete() { return m_is_delete; }

  dml_tuple_t *tuple_old() { return m_tuple_old; }

  dml_tuple_t *full_tuple_old() { return m_full_tuple_old; }

  bool *get_is_changed_vec() { return m_is_changed; }

  void bind_task(dml_upd_task *task) { m_task = task; }
  dml_upd_task *get_task() { return m_task; }

  MEM_ROOT *get_blob_mem_root() { return m_blobMemRoot; }

  int fill_tuple(DSTORE::HeapTuple *heap_tuple, uint32_t fk_col_nums,
                 uint8_t fk_rule, dml_upd_ctx *parent_ctx);

  void set_old_ctid(DSTORE::ItemPointer p_old_ctid) {
    m_tuple_old->ctid = *p_old_ctid;
  }

  bool check_old_has_null(uint32_t col_nums);

  void set_varlena_datum(const Oid &atttypeId, uint32_t fieldIdx,
                         uint32_t valueIdx, Datum *values,
                         CdeVarlena *varlenas);

  /** Fill virtual columns for index update/delete. This means calculating
   * virtual columns inside full_tuple_old (always - update/delete) and
   * full_tuple (only update).
   * @param[in] indexCols   positions of index columns among other columns in
   * the table - for which we should calculate value (virtual columns)
   * @param[in] indexColNum number of columns for which calculate values
   * @param[in] mysqlTable  pointer to MySQL table. Can be null.
   *                        If null - tmp TABLE* object will be
   *                        openned.
   * @return error or CDE_OK
   */
  int FillVirtualColumns(uint32_t *indexCols, uint32_t indexColNum,
                         TABLE *mysqlTable);

  /** Calculate change vector that includes virtual columns. */
  void CalculateChangeVecForVirtualColumns();

  bool CheckNeedCalculateVirtualColumn(uint32_t *indexCols,
                                       uint32_t indexColNum);

  /** Get the pointer to MySQL table object
   * @return pointer to MySQL table object
   */
  TABLE *GetMySQLTable();

 private:
  /** Computes value for virutal column by calling server callback function.
   * @param[in] mysql_table  table for which virtual column is to be calculated
   * @param[in] vCol         virutal column to be calculated
   * @param[in] tuple        tuple where we should put the calculated value
   */
  int DStoreGetComputedValue(TABLE *mysql_table, DictVirtualCol *vCol,
                             dml_tuple_t *tuple);

  /** Fill full tuple based on partial tuple. Full tuple includes virtual
   * columns, partial tuple is a regular tuple that does not include virtual
   * columns. Virtual columns are materialized only in full tuple - which is
   * used for index insert/updates/deletes.
   * @param[in]  fromTuple  partial tuple
   * @param[out] toTuple    fill tuple
   */
  void FillFullTuple(dml_tuple_t *fromTuple, dml_tuple_t *toTuple);

  dml_tuple_t *m_tuple_old{nullptr};
  dml_tuple_t *m_full_tuple_old{nullptr};
  bool *m_is_changed{nullptr};
  bool m_is_delete{false};
  dml_upd_ctx *m_cascade{nullptr};
  dml_upd_ctx *m_parent{nullptr};
  dml_upd_task *m_task{nullptr};
  /** Whether a value was already calcuated for virtual column.
   * FillVirtualColumns may be called multiple time for different
   * sets of virtual columns to be calculated. In case the value
   * for virtual column was already calculated, skip the calculation.
   */
  std::vector<bool> calculatedVColumns;

  /** When we get a value for virtual column in DStoreGetComputeValue,
   * we first need to allocate memory for mysql record, that we send
   * to server layer. Server calculates virtual column and send us
   * this memory back. We now have to translate this memory to DStore.
   * For varlena types and data types we dont copy mysql value
   * to dstore value, we just point into mysql record. That is why
   * we need to keep the memory allocated for this mysql record
   * as long as those values are in use. We clear this memory in
   * dml_upd_ctx destructor.
   */
  std::vector<unsigned char *> virtualDatum;

  /** When blob type in virtual column and this virtual column as a index,
   *  we re-use DstoreIndexDataToMysql in DStoreGetComputeValue to get a value
   *  from virtual column, so we need get value from index data parse get blob
   *  value, need blob mem root. Now we set first table's handler object
   * m_blob_mem_root to dml_upd_task and pass it from parent to child. We don't
   * need clear this memory in dml_upd_ctx destructor.
   */
  MEM_ROOT *m_blobMemRoot;

  /** Pointer to TABLE* object. Used when calculating virtual columns
   * on slave server.
   */
  TABLE *m_mysqlTable{nullptr};
};

class dml_upd_task {
 public:
  dml_upd_task(dml_upd_ctx *ctx)
      : m_cur_ctx(ctx), m_prev_ctx(ctx), m_cascade_depth(0) {}
  ~dml_upd_task() {}

  dml_upd_ctx *get_cur_ctx() { return m_cur_ctx; }
  dml_upd_ctx *get_prev_ctx() { return m_prev_ctx; }

  void set_cur_ctx(dml_upd_ctx *ctx) { m_cur_ctx = ctx; }
  void set_prev_ctx(dml_upd_ctx *ctx) { m_prev_ctx = ctx; }

  void reset_ctx(dml_upd_ctx *ctx) {
    m_cur_ctx = ctx;
    m_prev_ctx = ctx;
  }

  uint8_t get_cascade_depth() { return m_cascade_depth; }
  void inc_cascade_depth() { m_cascade_depth++; }
  void reset_cascade_depth() { m_cascade_depth = 0; }
  bool check_cascade_depth() { return m_cascade_depth > MAX_CASCADE_DEPTH; }

 private:
  void *m_child{nullptr};
  dml_task_state_e m_state{TASK_RUNNING};
  bool m_is_active{false};
  dml_upd_ctx *m_cur_ctx{nullptr};
  dml_upd_ctx *m_prev_ctx{nullptr};
  uint8_t m_cascade_depth{0};
  uint8_t m_lock_state{0};
};
} /* namespace CDE */
#endif  // __CDE_DML_CTX_H__
