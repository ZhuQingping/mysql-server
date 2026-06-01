/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * ha_cde.h
 *
 *
 * IDENTIFICATION
 * src/cde_cde.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef ha_cde_h
#define ha_cde_h

/* The dstore handler: the interface between MySQL and cde. */

#include <sys/types.h>

#include "common/cde_alloc.h"
#include "common/cde_session.h"
#include "common/cde_srv.h"
#include "common/cde_trxmgr.h"
#include "dict/cde_dict.h"
#include "dict/cde_dict_stat.h"
#include "dict/cde_relation.h"
#include "dict/cde_stats_sampler.h"
#include "framework/dstore_instance_interface.h"
#include "framework/dstore_parallel_interface.h"
#include "ha_cde_cond.h"
#include "my_dbug.h"
#include "page/dstore_itemptr.h"
#include "perfcounters.h"
#include "perfpublisher.h"
#include "sql/handler.h"

class SEL_ROOT;
namespace DSTORE {
class Transaction;
}

namespace CDE {

extern struct handlerton *cde_hton_ptr;

/* Structure defines translation table between mysql index and CDE
index structures */
struct cde_idx_translate_t {
  ulint index_count; /*!< number of valid index entries
                   in the index_mapping array */

  ulint array_size; /*!< array size of index_mapping */

  cde_dict_index_t **index_mapping; /*!< index pointer array directly
                              maps to index in CDE from MySQL
                              array index */
};

/** cde_share is a struct that will be shared among all open handlers.*/
struct CDE_SHARE {
  const char *table_name; /*!< table name */
  uint use_count;
  void *table_name_hash;
  cde_idx_translate_t idx_trans_tbl;
};

/* CDESampler is a struct that used for sampling */
struct CDESampler {
  BlockSamplerData samplerData;
  /* read tuple index in current block */
  int m_currBlockTupleIndex{INT_MAX};

  CDESampler(BlockNumber nblocks, uint32_t samplesize, uint32_t seed) {
    OptstatsBlockSamplerInit(&samplerData, nblocks, samplesize, seed);
  }

  bool CdeSamplerHasMore() { return OptstatsBlockSamplerHasMore(&samplerData); }

  BlockNumber CdeSamplerNext() {
    return OptstatsBlockSamplerNext(&samplerData);
  }
};

int CdeLockTable(lock_mode mode, DSTORE::StorageRelation relation,
                 bool dontWait = false);

/** The class defining a handle to an cde table */
class ha_cde : public handler {
 public:
  ha_cde(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_cde() override;

  row_type get_real_row_type(const HA_CREATE_INFO *create_info) const override;

  const char *table_type() const override;

  enum ha_key_alg get_default_index_algorithm() const override {
    return HA_KEY_ALG_BTREE;
  }

  void static CdeShowIndexTable(std::ostream &os,
                                const Huawei::Common::PublisherFilter *filter);
  void static CdeShowSampleStats(std::ostream &os,
                                 const Huawei::Common::PublisherFilter *filter);
  /** Check if SE supports specific key algorithm. */
  bool is_index_algorithm_supported(enum ha_key_alg key_alg) const override {
    /* This method is never used for FULLTEXT or SPATIAL keys.
    We rely on handler::ha_table_flags() to check if such keys
    are supported. */
    DBUG_ASSERT(key_alg != HA_KEY_ALG_FULLTEXT && key_alg != HA_KEY_ALG_RTREE);
    return (HA_KEY_ALG_BTREE == key_alg);
  }

  int open(const char *name, int, uint open_flags,
           const dd::Table *table_def) override;

  /**
  Clone this handler, used when needing more than one cursor
  to the same table.

  @param[in]      name            Table name.
  @param[in]      mem_root        mem_root to allocate from.

  @return Pointer to clone or NULL if error.
  */
  handler *clone(const char *name, MEM_ROOT *mem_root) override;
  int close(void) override;
  uint max_supported_key_length() const override;
  uint max_supported_key_part_length(
      HA_CREATE_INFO *create_info) const override;
  Table_flags table_flags() const override;
  ulong index_flags(uint idx, uint part, bool all_parts) const override;
  uint max_supported_keys() const override;
  int write_row(uchar *buf) override;
  int update_row(const uchar *old_data, uchar *new_data) override;
  int delete_row(const uchar *buf) override;
  int delete_all_rows() override;
  double scan_time() override;
  double read_time(uint index, uint ranges, ha_rows rows) override;
  longlong get_memory_buffer_size() const override;

  /*note:semi consistent is supported by innodb, dstore has no semi consistent
   */
  // bool was_semi_consistent_read() override;

  // void try_semi_consistent_read(bool yes) override;

  // void unlock_row() override;

  int index_init(uint index, bool sorted) override;
  /** End an index scan.
  @return 0 on success or HA_ERR_* error code */
  int index_end() override;
  int index_read(uchar *buf, const uchar *key, uint key_len,
                 ha_rkey_function find_flag) override;
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_read_last(uchar *buf, const uchar *key, uint key_len) override;
  int index_read_last_map(uchar *buf, const uchar *key,
                          key_part_map keypart_map) override;
  int rnd_init(bool scan) override;
  int rnd_end() override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar *buf, uchar *pos) override;
  int read_range_first(const key_range *start_key, const key_range *end_key,
                       bool eq_range_arg, bool sorted) override;
  int read_range_next() override;

  int index_next(uchar *buf) override;
  int index_next_same(uchar *buf, const uchar *key, uint keylen) override;
  int index_prev(uchar *buf) override;
  int index_first(uchar *buf) override;
  int index_last(uchar *buf) override;

  /*note:ft index is not supported by dstore*/
  /*
  int ft_init() override;

  void ft_end();

  FT_INFO *ft_init_ext(uint flags, uint inx, String *key) override;

  FT_INFO *ft_init_ext_with_hints(uint inx, String *key,
                                  Ft_hints *hints) override;

  int ft_read(uchar *buf) override; */

  int extra(ha_extra_function operation) override;
  int reset() override;
  int external_lock(THD *thd, int lock_type) override;
  int analyze(THD *thd, HA_CHECK_OPT *check_opt) override;
  int optimize(THD *thd, HA_CHECK_OPT *check_opt) override;
  void position(const uchar *record) override;
  int info(uint) override;
  int enable_indexes(uint mode) override;
  int disable_indexes(uint mode) override;

  /** Initialize sampling.
  @param[out] scan_ctx  sample dstore a scan context created by this method that
  has to be used in sample_next
  @param[in]  sampling_percentage sample dstore percentage of records that need
  to be sampled
  @param[in]  sampling_seed       sample dstore random seed that the random
  generator will use
  @param[in]  sampling_method     dstore sampling method to be used; currently
  only SYSTEM sampling is supported
   @param[in]  tablesample         true if the dstore sampling is for
  tablesample
  @return 0 for success, else one of the HA_cde values in case of error. */
  int sample_init(void *&scan_ctx, double sampling_percentage,
                  int sampling_seed, enum_sampling_method sampling_method,
                  const bool tablesample) override;

  /** Get the next record for dstore sampling.
  @param[in]  scan_ctx  dstore scan context of the sampling
  @param[in]  buf       buffer to place the read dstore record
  @return 0 for success, else one of the HA_cde values in case of error. */
  int sample_next(void *scan_ctx, uchar *buf) override;

  /** End sampling.
  @param[in] scan_ctx  Scan context of the dstore sampling
  @return 0 for success, else one of the HA_cde values in case of error. */
  int sample_end(void *scan_ctx) override;

  int start_stmt(THD *thd, thr_lock_type lock_type) override;

  int records(ha_rows *num_rows) override;

  // fix full table scan to index scan in records_from_index by default
  // implement in handler. todo: need to inherit the parallel query capability
  // of the Taurus SQL engine. int records_from_index(ha_rows *num_rows, uint)
  // override {
  //   return ha_cde::records(num_rows);
  // }

  ha_rows records_in_range(uint inx, key_range *min_key,
                           key_range *max_key) override;

  ha_rows estimate_rows_upper_bound() override;

  void update_create_info(HA_CREATE_INFO *create_info) override;

  /** Get storage-engine private data for a data dictionary table. */
  bool get_se_private_data(dd::Table *dd_table, bool reset) override;

  /** Add hidden columns and indexes to an InnoDB table definition. */
  int get_extra_columns_and_keys(const HA_CREATE_INFO *,
                                 const List<Create_field> *, const KEY *, uint,
                                 dd::Table *dd_table) override;

  /** Set Engine specific data to dd::Table object for upgrade. */
  // bool upgrade_table(THD *thd, const char *db_name, const char *table_name,
  //                   dd::Table *dd_table) override;

  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
             dd::Table *table_def) override;
  int delete_table(const char *name, const dd::Table *table_def) override;
  int rename_table(const char *from, const char *to,
                   const dd::Table *from_table, dd::Table *to_table) override;
  int ha_check_support_recyclebin(const char *name, bool &support_recyclebin,
                                  const dd::Table *dd_table) override;
  int check(THD *thd, HA_CHECK_OPT *check_opt) override;

  bool get_error_message(int error, String *buf) override;
  bool get_foreign_dup_key(char *, uint, char *, uint) override;
  bool primary_key_is_clustered() const override;
  int cmp_ref(const uchar *ref1, const uchar *ref2) const override;
  Item *idx_cond_push(uint keyno, Item *idx_cond) override;
  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values, ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;
  /** Do cleanup for auto increment calculation. */
  void release_auto_increment() override;
  /*note: dstore doesn't use record lock, it uses td in transaction */
  uint lock_count(void) const override;
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             thr_lock_type lock_type) override;
  void init_table_handle_for_HANDLER() override;

  /**
   This function TRY to estimate the total rows of a range's records
   based on Histogram.

   How we estimate a range using histogram?
   Consider a range where the min_key is [col1=x1, col2=y1] and the
   max_key is [col1=x2, col2=y2]. Since histograms can only be built
   on individual columns, we will split the range for each column into
   sub-range like [col1=x1]~[col1=x2] and [col2=y1]~[col2=y2]. Then we
   will query the histogram for each column's selectivity using the
   sub-ranges above. Last, we multiply the selectivities of multiple
   columns to calculate the total selectivity. Finally, we multiply the
   selectivity by the total number of rows in the table to estimate the
   number of rows in the entire range.
   *
   @param[in]  keynr index number.
   @param[in]  min_key start key value of a range, may be nullptr.
   @param[in]  max_key end key value of a range, may be nulltpr.
   @retval >=0 Number of records estimated in range.
   @retval K_REC_IN_RANGE_HISTOGRAM_ACCESS_FAILED Histogram dosen't exist or
   access failed.
   @retval K_REC_IN_RANGE_OOM Out of memory when allocate new Item_field.
   */
  int64_t records_in_range_by_histogram(uint keynr, key_range *min_key,
                                        key_range *max_key,
                                        ha_rows numberOfRows);

  /* Alter table will be supported in the second phase */
  /** @defgroup ALTER_TABLE_INTERFACE On-line ALTER TABLE interface
  @see handler0alter.cc
  @{ */

  /** Check if CDE supports a particular alter table in-place
  @param altered_table TABLE object for new version of table.
  @param ha_alter_info Structure describing changes to be done by ALTER TABLE
  and holding data used during in-place alter.

  @retval HA_ALTER_INPLACE_NOT_SUPPORTED Not supported
  @retval HA_ALTER_INPLACE_NO_LOCK Supported
  @retval HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE Supported, but requires
  lock during main phase and exclusive lock during prepare phase.
  @retval HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE Supported, prepare phase
  requires exclusive lock (any transactions that have accessed the table must
  commit or roll back first, and no transactions can access the table while
  prepare_inplace_alter_table() is executing)
  */
  enum_alter_inplace_result check_if_supported_inplace_alter(
      TABLE *altered_table, Alter_inplace_info *ha_alter_info) override;

  bool prepare_inplace_alter_table(TABLE *altered_table,
                                   Alter_inplace_info *ha_alter_info,
                                   const dd::Table *old_dd_tab,
                                   dd::Table *new_dd_tab) override;

  bool inplace_alter_table(TABLE *altered_table,
                           Alter_inplace_info *ha_alter_info,
                           const dd::Table *old_dd_tab,
                           dd::Table *new_dd_tab) override;

  bool commit_inplace_alter_table(TABLE *altered_table,
                                  Alter_inplace_info *ha_alter_info,
                                  bool commit, const dd::Table *old_dd_tab,
                                  dd::Table *new_dd_tab) override;

  /** Implementation of inplace_alter_table()
  @tparam               Table           dd::Table or dd::Partition
  @param[in]            altered_table   TABLE object for new version of table.
  @param[in,out]        ha_alter_info   Structure describing changes to be done
                                        by ALTER TABLE and holding data used
                                        during in-place alter.
  the table. Can be adjusted by this call. Changes to the table definition will
  be persisted in the data-dictionary at statement commit time.
  @retval true Failure
  @retval false Success
  */
  template <typename Table>
  bool InplaceAlterTableImpl(TABLE *alteredTable,
                             Alter_inplace_info *haAlterInfo);

  /** Implementation of commit_inplace_alter_table()
  @tparam               Table           dd::Table or dd::Partition
  @param[in]            alteredTable    TABLE object for new version of table.
  @param[in,out]        haAlterInfo     Structure describing changes to be done
                                        by ALTER TABLE and holding data used
                                        during in-place alter.
  @param[in,out]        newDDTab        Table object for the new version of the
                                        table. Can be adjusted by this call.
                                        Changes to the table definition
                                        will be persisted in the data-dictionary
                                        at statement version of it.
  @retval               true Failure
  @retval               false Success */
  template <typename Table>
  bool CommitInplaceAlterTableImpl(TABLE *alteredTable,
                                   Alter_inplace_info *haAlterInfo,
                                   Table *newDDTab);
  template <typename Table>
  bool CommitInplaceAlterRebuildTableImpl(TABLE *alteredTable,
                                          Alter_inplace_info *haAlterInfo,
                                          const Table *oldDDTab,
                                          Table *newDDTab);

  /** Inplace bulid table indexes
  @param[in]            alteredTable      the TABLE object of new table
  @param[in]            haAlterInfo       Structure describing changes to be
  done by ALTER TABLE and holding data used during in-place alter.

  @retval true Failure
  @retval false Success
  */
  bool CdeInplaceBuildIndexes(TABLE *alteredTable,
                              Alter_inplace_info *haAlterInfo);

  bool CdeInplaceRebuildTable(TABLE *alteredTable,
                              Alter_inplace_info *haAlterInfo);

  /** Get the auto-increment value of the table on commit.
  @param[in] ha_alter_info Data used during in-place alter
  @param[in,out] ctx In-place ALTER TABLE context
                return autoinc value in ctx->maxAutoinc
  @param[in] alteredTable MySQL table that is being altered
  @param[in] oldTable MySQL table as it is before the ALTER operation
  */
  void CommitGetAutoinc(Alter_inplace_info *haAlterInfo,
                        const TABLE *alteredTable, const TABLE *oldTable);

  /** @} */

  /* note:parallel scan is not general */
  // using Reader = Parallel_reader_adapter;

  // int parallel_scan_init(void *&scan_ctx, size_t *num_threads,
  //                       bool use_reserved_threads) override;

  /** Start parallel read of InnoDB records. */
  // int parallel_scan(void *scan_ctx, void **thread_ctxs, Reader::Init_fn
  // init_fn,
  //                  Reader::Load_fn load_fn, Reader::End_fn end_fn) override;

  /** End of the parallel scan.
  @param[in]      scan_ctx      A scan context created by parallel_scan_init. */
  // void parallel_scan_end(void *scan_ctx) override;

  virtual void print_error(int error, myf errflag) override;

  void set_old_name(const char *oldName) override;

  int post_rename_table_statistics(const char *from, const char *to,
                                   const dd::Table *fromTable,
                                   const dd::Table *toTable) override;

  bool check_if_incompatible_data(HA_CREATE_INFO *info,
                                  uint table_changes) override;

  CdeCondHandler &get_cond_handler() { return m_cond_handler; }

  const dstore_handler_t *get_dstore_handler() { return m_dstore; }
  MEM_ROOT &get_mem_root() { return m_mem_root; }

  /** When offset pushdown is activated, it is used to communicate the offset
  value to the handler */
  void set_pushed_offset(ha_rows pushed_offset) override {
    m_pushed_offset = pushed_offset;
  }
  /** If non-zero, offset pushdown is activated */
  ha_rows get_pushed_offset() const override { return m_pushed_offset; }

  /// This isn't declared in class 'handler' as it's an implementation detail
  /// which is internal to dstore.
  /** Increments the scanned_offset counter that accounts for the number of rows
  that have been skipped during offset pushdown */
  void increment_scanned_offset() { m_scanned_offset++; }
  /** Retrieves how many rows have been skipped so far due to offset pushdown */
  ha_rows get_scanned_offset() const { return m_scanned_offset; }

  /** If offset_pushdown determined that the offset should be pushed down to
  dstore, we check whether we have skipped enough rows to reach the
  required offset.
  @param[in]	ha  contains information about offset pushdown
  @return true if offset pushdown is active and not all rows have been skipped,
  false otherwise */
  bool skip_row_for_offset_pushdown() {
    return (get_pushed_offset() > get_scanned_offset());
  }

  /**
  Perform aggregation by group for none index table
  @return true continue grouping in storage
          false return the grouped row to upper layer */
  bool aggregate_rows_by_group() {
    if (m_pushed_aggregate != nullptr) {
      return aggregate_in_engine(m_pushed_aggregate);
    }
    return false;
  }

  /**
  Perform aggregation by group for indexed table
  @return true continue grouping in storage
          false return the grouped row to uppper layer */
  bool aggregate_rows_by_group_for_index() {
    if (m_pushed_aggregate != nullptr) {
      // out of range
      if (compare_key(end_range) > 0) {
        return false;
      }
      return aggregate_in_engine(m_pushed_aggregate);
    }
    return false;
  }

  /**
  Check could skip decoding for unqualified count
  @return true can skip decoding
          false can not skip decoding */
  bool aggregate_unqualified_count() {
    if (m_pushed_aggregate != nullptr) {
      return unqualified_count_in_engine(m_pushed_aggregate);
    }
    return false;
  }

 private:
  /*note:MRR is implemented by MySQL handler actually */
  /** @name Multi Range Read interface
  @{ */

  /**
  Initialize multi range read @see DsMrr_impl::dsmrr_init

  @param[in] seq_funcs       Range sequence to be traversed
  @param[in] seq_init_param  First parameter for seq->init()
  @param[in] n_ranges        Number of ranges in the sequence
  @param[in] mode            Flags, see the description section for the details
  @param[in,out] buf         memory buffer to be used

  @return  int  0 for OK, others for error code
  */
  int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                            uint n_ranges, uint mode,
                            HANDLER_BUFFER *buf) override;

  /**
  Process next multi range read @see DsMrr_impl::dsmrr_next
  @param[in,out]  buf  Undefined if HA_MRR_NO_ASSOCIATION flag is in
  effect.Otherwise, the opaque value associated with the range that contains the
  returned record.

  @return  int  0 for OK, others for error code
  */
  int multi_range_read_next(char **range_info) override;

  /**
  Initialize multi range read and get information. @see
  ha_myisam::multi_range_read_info_const @see DsMrr_impl::dsmrr_info_const

  @param[in]      keyno           Index number
  @param[in]      seq             Range sequence to be traversed
  @param[in]      seq_init_param  First parameter for seq->init()
  @param[in]      n_ranges_arg    Number of ranges in the sequence, or 0 if the
                                  caller can't efficiently determine it
  @param[in,out]  bufsz           IN:  Size of the buffer available for use;
                                  OUT: Size of the buffer that is expected to be
                                  actually used, or 0 if buffer is not needed.
  @param[in,out]  flags           A combination of HA_MRR_* flags
  @param[out]     force_default_mrr Force default MRR implementation
  @param[out]     cost            Estimated cost of MRR access

  @return HA_POS_ERROR for Error or the engine is unable to perform the
  requested scan. others for OK.
  */
  ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                      void *seq_init_param, uint n_ranges,
                                      uint *bufsz, uint *flags,
                                      bool *force_default_mrr,
                                      Cost_estimate *cost) override;

  /**
  Initialize multi range read and get information. @see DsMrr_impl::dsmrr_info

  @param[in]      keyno           Index number
  @param[in]      n_ranges        Estimated number of ranges (i.e. intervals) in
  the range sequence.
  @param[in]      n_rows          Estimated total number of records contained
  within all of the ranges
  @param[in,out]  bufsz           IN:  Size of the buffer available for use
                                  OUT: Size of the buffer that will be actually
  used, or 0 if buffer is not needed.
  @param[in,out]  flags           A combination of HA_MRR_* flags
  @param[out]     cost            Estimated cost of MRR access

  @return 0 for OK, other for error or can't perform the requested scan
  */
  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                uint *bufsz, uint *flags,
                                Cost_estimate *cost) override;

  /**
  Calculate execute rows for range.
  TODO this function need to be removed due to poor performance.

  @param[in]  keynr   index number
  @param[in]  min_key start key value of the range, may also be nullptr
  @param[in]  max_key range end key val, may also be nullptr

  @return rows of range.
  */
  ha_rows calculate_exact_rows_for_range(uint keynr, key_range *min_key,
                                         key_range *max_key);

  /**
    Condition pushdown

    Push a condition to ndbcluster storage engine for evaluation
    during table and index scans. The conditions will be cleared
    by calling handler::extra(HA_EXTRA_RESET) or handler::reset().

    The current implementation supports arbitrary AND/OR nested conditions
    with comparisons between columns and constants (including constant
    expressions and function calls) and the following comparison operators:
    =, !=, >, >=, <, <=, "is null", and "is not null".

    If the condition consist of multiple AND/OR'ed 'boolean terms',
    parts of it may be pushed, and other parts will be returned as a
    'remainder condition', which the server has to evaluate.

    handler::pushed_cond will be assigned the (part of) the condition
    which we accepted to be pushed down.

    Note that this handler call has been partly deprecated by
    handlerton::push_to_engine(), which does both join- and
    condition pushdown for the entire query AccessPath.
    The only remaining intended usage for ::cond_push() is simple
    update and delete queries, where the join part is not relevant.

    @param cond          Condition to be pushed down.

    @return Return the 'remainder' condition, consisting of the AND'ed
            sum of boolean terms which could not be pushed. A nullptr
            is returned if entire condition was supported.
    */
  const Item *cond_push(const Item *cond) override;

  std::string explain_extra() const override;

  /**
  Get the handlerton of the storage engine if the SE is capable of
  pushing down some of the AccessPath functionality.
  (Join, Filter conditions, ... possiby more)

  Call the handlerton::push_to_engine() method for performing the
  actual pushdown of (parts of) the AccessPath functionality

  @return   handlerton* of the SE if it may be capable of
            off loading part of the query by calling
            handlerton::push_to_engine()
            Else, 'nullptr' is returned.
  */
  const handlerton *hton_supporting_engine_pushdown() override { return ht; }

 private:
  virtual int info_impl(uint flag, bool is_analyze);

 protected:
  /**
  check current statement's snapshot valid, flush while snapshot csn invalid or
  forceUpdata flag set true
  @param[in]  select_lock_type lock type
  @param[in]  trx  cuurent transaction
  @param[in]  forceUpdate  don't check current csn, force flush cached snapshot

  @return
  */
  void updateStatementSnapShot(ulint select_lock_type, cde_trxinfo_t *trx,
                               bool forceUpdate = false);

  /**
    Return max limits for a single set of multi-valued keys

    @param[out]  num_keys      number of keys to store
    @param[out]  keys_length   total length of keys, bytes
  */
  void mv_key_capacity(uint *num_keys, size_t *keys_length) const override;

  session_index_info *cde_index_lookup(int keynr);

  uint32_t cde_index_lookup(const char *index_rel_name);

  void index_read_cleanup();

  /**
  check if top transaction memory context has reset.

  @return     true while memory context is reset, otherwise false
  */
  bool CdeIsTransactionMemContextReset();

  void column_bitmaps_signal() override;

  // Figure out which columns we need to decode in order fulfill the query and
  // record those in m_dstore::columns_to_decode.
  void set_columns_to_decode();

  /** Can reuse the columns decoding. Mainly used for partition.
  @retval       true can reuse the decoding from earlier partition */
  virtual bool CanReuseColumnsDecoding() const { return false; }

  /** Compares two 'refs'. A 'ref' is the (internal) primary key value of the
  row. If there is no explicitly declared non-null unique key or a primary key,
  then CDE internally uses the row id as the primary key.
  @return < 0 if ref1 < ref2, 0 if equal, else > 0 */
  static int cmpRef(const uchar *ref1, const uchar *ref2);

  /** The multi range read session object */
  DsMrr_impl m_ds_mrr;

  /** Save CPU time with prebuilt/cached data structures */
  // row_prebuilt_t *m_prebuilt;

  /* need to add new struct CDE_SHARE for dstore */
  /** information for MySQL table locking */
  // CDE_SHARE *m_share;

  Table_flags m_int_table_flags;
  ulint m_stored_select_lock_type;
  bool m_mysql_has_locked;
  dstore_handler_t *m_dstore{nullptr};

  /* used for handler specific relation&index info */
  Memory::MemHeap m_mem_root;
  /* Memory area for BLOB data */
  MEM_ROOT m_blob_mem_root;

  // used for sampling info
  std::unique_ptr<CDESampler> m_samplerCtx{nullptr};
  CdeCondHandler m_cond_handler;

  /** value of the offset used for pushing down the offset to dstore. */
  ha_rows m_pushed_offset{0};
  /**
     Number of rows that have been skipped in dstore,
     scanned_offset is incremented up to pushed_offset when offset_pushdown
     has determined the offset should be pushed
  */
  ha_rows m_scanned_offset{0};

  // helper variable to skip calling update_auto_increment
  // in ha_cde::write_row, as it was already called by partition_handler
  // however, even though we skip calling this function -
  // we still need to update autoinc per partTable -
  // as this value will be later used when initializing partition table's
  // autinc after re-openning. For instance after truncate_partition_low.
  bool m_skipUpdateAutoIncrement{false};

  // helper variable to return from ha_cde::records_in_range the exact number
  // of rows in range, including 0 - without increasing number of rows from 0 to
  // 1.
  // This is needed by parition handler, where 0 rows for partition is OK value.
  // The partition handler will increase from 0 to 1, to make sure
  // records_in_range deosn't return 0 - which seems needed by MySQL server
  // (for more info check comment in ha_cde::records_in_range).
  bool m_returnExactNrowsInRange{false};

  bool m_dontUseHistogram{false};

  /** Normally blob mem root can be cleared before row is read. However,
   * in case of partitioned table we may need to keep the rows in memory
   * between reads. This is needed for ordered scan with use of
   * records priority queue.
   */
  bool m_allowBlobMemRootClearForReuse{true};
};

void DrawGtidInfo(const unsigned char *buffer, uint32_t &start_position);

void DrawGtidExecutedInfo(const unsigned char *buffer,
                          uint32_t &start_position);

/**
Get current PDB id. This is a common interface to release the used pdb id.

@return current pdb id
*/
DSTORE::PdbId GetCurrentPdbId();

bool CdeReportErrorOnDamagePage();
bool CdeEnableDamagePageManager();
bool CdeEnableIndexSample();
bool CdeExpandKeyLenByTD();

/**
Start trx if it is not started. It's called in below scenes:
ddl:create/rename_table/delete_table
dml:external_lock
lock table:start_stmt

@param[in]  thd  thread context.
@param[in]  rw  true if it's a read-write trx, false is read-only(default).

@return void
*/
void CdeTrxStartIfNotStarted(THD *thd, bool rw = false);

/**
Copy table flags from MySQL's TABLE_SHARE into a dstore table object.

@param[in]  table        dstore table.
@param[in]  table_share  MySQL table share.

@return void
*/
void CdeSetTableFlagsFromTableShare(cde_dict_t *table,
                                    const TABLE_SHARE *table_share);

/** @return the number of DDL threads to use (global/session). */
[[nodiscard]] size_t ThdDdlThreads(THD *thd) noexcept;

/** @return wether use batch insert to rebuild heap. */
bool ThdEnableBatchInsertForRebuild(THD *thd) noexcept;

/** @return the number of block size for tmp buffer. */
[[nodiscard]] uint32 ThdTmpBufferSize(THD *thd) noexcept;

ulong thd_parallel_read_threads(THD *thd);
} /* namespace CDE */
#endif /* ha_cde_h */
