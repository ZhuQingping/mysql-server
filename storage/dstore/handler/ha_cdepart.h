/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2026 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * ha_cdepart.h
 *
 */

/* The DStore Partition handler: the interface between MySQL and DStore. */

#ifndef HA_CDEPART_H
#define HA_CDEPART_H

#include <stddef.h>
#include <sys/types.h>

#include "dd/types/partition.h"
#include "dict/cde_dict.h"
#include "ha_cde.h"
#include "my_compiler.h"
#include "partitioning/partition_handler.h"

/* Forward declarations */
class Altered_partitions;
class partition_info;

/** HA_DUPLICATE_POS and HA_READ_BEFORE_WRITE_REMOVAL is not
set from ha_innobase, but cannot yet be supported in ha_innopart.
Full text and geometry is not yet supported. */
const handler::Table_flags HA_CDE_DISABLED_TABLE_FLAGS =
    (HA_CAN_FULLTEXT | HA_CAN_FULLTEXT_EXT | HA_CAN_GEOMETRY |
     HA_DUPLICATE_POS | HA_READ_BEFORE_WRITE_REMOVAL);

namespace CDE {

/** DStore partition specific Handler_share. */
class HaCdepartShare : public Partition_share {
 private:
  /** Array of all included table definitions (one per partition). */
  cde_dict_t **m_tableParts;

  /** Total number of partitions. */
  uint m_totParts;

  /** Reference count. */
  uint m_refCount;

  /** Pointer back to owning TABLE_SHARE. */
  TABLE_SHARE *m_tableShare;

  Memory::MemHeap m_mem_root;

 public:
  explicit HaCdepartShare(TABLE_SHARE *table_share);

  ~HaCdepartShare() override;

  /** Set DStore table for given partition.
  @param[in]    part_id Partition number.
  @param[in]    table   Table. */
  inline void setTablePart(uint part_id, cde_dict_t *table) {
    CDE_ASSERT_DEBUG(nullptr != m_tableParts);
    CDE_ASSERT_DEBUG(part_id < m_totParts);
    m_tableParts[part_id] = table;
  }

  /** Return dstore table for given partition.
  @param[in]    part_id Partition number.
  @return       DStore table. */
  inline cde_dict_t *getTablePart(uint partId) const {
    CDE_ASSERT_DEBUG(m_tableParts != nullptr);
    CDE_ASSERT_DEBUG(partId < m_totParts);
    return (m_tableParts[partId]);
  }

  /** Return whether share has opened DStore tables for partitions. */
  bool hasTableParts() const { return (m_tableParts != nullptr); }

  /** Increment share and DStore tables reference counters. */
  void incrementRefCounts();
  void decrementRefCounts();

  /** Open DStore tables for partitions and return them as array.
  @param[in,out]        thd             Thread context
  @param[in]    table           MySQL table definition
  @param[in]    dd_table        Global DD table object
  @param[in]    part_info       Partition info (partition names to use)
  @param[in]    table_name      Table name (db/table_name)
  @return       Array on InnoDB tables on success else nullptr. */
  static cde_dict_t **openTableParts(THD *thd, TABLE *table,
                                     const dd::Table *dd_table,
                                     partition_info *part_info,
                                     const char *table_name,
                                     dstore_handler_t **&dstoreHandlerParts,
                                     Memory::MemHeap &memRoot,
                                     TABLE_SHARE *share);

  /**
  Initialize the share with table and indexes per partition.
  @param[in]    part_info       Partition info (partition names to use).
  @param[in]    table_parts     Array of InnoDB tables for partitions.
  */
  void setTablePartsAndIndexes(partition_info *part_info,
                               cde_dict_t **table_parts);

  /** Close the table partitions.
  If all instances are closed, also release the resources.*/
  void closeTableParts();

  TABLE_SHARE *getTableShare() const { return (m_tableShare); }

  /** Get the number of partitions
  @return number of partitions */
  uint getNumParts() const {
    CDE_ASSERT_DEBUG(m_totParts != 0);
    return (m_totParts);
  }

  /** Return DStore tables for partitions. */
  cde_dict_t **getTableParts() { return m_tableParts; }

  /** Initialize single dstore handlers.
  @param[in]    partTable         DStore partition table
  @param[in]    dstore            DStore handler to initialize
  @param[in]    forDDLInfoReplay  Whether it's init for ddl info replay
  @param[in]    table             MySQL table definition
  @param[in]    memRoot           mem root from which allocate memory
  @param[in]    fistPartDstore    first partition's dstore handler.
                                  Some of the member fields may be reused
                                  in the subsequent partitions.
  @return       0  or error */
  static int64_t initDStoreHandlerPart(cde_dict_t *partTable,
                                       dstore_handler_t *dstore,
                                       bool forDDLInfoReplay, TABLE *table,
                                       Memory::MemHeap &memRoot,
                                       dstore_handler_t *fistPartDstore);

 private:
  /** Disable default constructor. */
  HaCdepartShare() = default;

  /** Open one partition
  @param[in]    thd             Thread THD
  @param[in]    table           MySQL table definition
  @param[in]    dd_part         dd::Partition
  @param[in]    part_name       Table name of this partition
  @param[out]   part_dict_table DStore table for partition
  @param[out]   dstoreHandler   dstore handler to be created and
                                initialized
  @param[in/out]memRoot         memory root to allocate memory
                                from
  @param[in]    tableShare      table's share
  @param[in]    fistPartDstore  first partition's dstore handler.
                                Some of the member fields may be reused
                                in the subsequent partitions.
  @retval       false   On success
  @retval       true    On failure */
  static bool openOneTablePart(
      THD *thd, TABLE *table, const dd::Partition *dd_part,
      const char *part_name, cde_dict_t **part_dict_table,
      dstore_handler_t **dstoreHandler, Memory::MemHeap &memRoot,
      TABLE_SHARE *tableShare, dstore_handler_t *firstPartDstore);
};

/** Get explicit specified tablespace for one (sub)partition, checking
from lowest level
@param[in]      tablespace      table-level tablespace if specified
@param[in]      part            Partition to check
@param[in]      sub_part        Sub-partition to check, if no, just NULL
@return Tablespace name, if nullptr or [0] = '\0' then nothing specified */
const char *partitionGetTablespace(const char *tablespace,
                                   const partition_element *part,
                                   const partition_element *sub_part);

/** The class defining a partitioning aware handle to an InnoDB table.
Based on ha_innobase and extended with
- Partition_helper for re-using common partitioning functionality
- Partition_handler for providing partitioning specific api calls.
Generic partitioning functions are implemented in Partition_helper.
Lower level storage functions are implemented in ha_innobase.
Partition_handler is inherited for implementing the handler level interface
for partitioning specific functions, like truncate_partition.
InnoDB specific functions related to partitioning is implemented here. */
class haCdepart : public ha_cde,
                  public Partition_helper,
                  public Partition_handler {
 public:
  haCdepart(handlerton *hton, TABLE_SHARE *table_arg);

  ~haCdepart() override = default;

  /** Clone this handler, used when needing more than one cursor
  to the same table.
  @param[in]    name            Table name.
  @param[in]    mem_root        mem_root to allocate from.
  @retval       Pointer to clone or NULL if error. */
  handler *clone(const char *name, MEM_ROOT *mem_root) override;

  /** Currently DStore does not support inplace alter */
  enum_alter_inplace_result check_if_supported_inplace_alter(
      TABLE *, Alter_inplace_info *) override {
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }

  /** Optimize table.
  @param[in]    thd             Connection thread handle.
  @param[in]    check_opt       Currently ignored.
  @return       0 for success else error code. */
  int optimize(THD *thd, HA_CHECK_OPT *check_opt) override;

  int extra(enum ha_extra_function operation) override;

  int post_rename_table_statistics(const char *from, const char *to,
                                   const dd::Table *fromTable,
                                   const dd::Table *toTable) override;

  void print_error(int error, myf errflag) override;

  bool is_ignorable_error(int error) override;

  int start_stmt(THD *thd, thr_lock_type lock_type) override;

  ha_rows records_in_range(uint inx, key_range *min_key,
                           key_range *max_key) override;

  ha_rows estimate_rows_upper_bound() override;

  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
             dd::Table *table_def) override;

  /** Drop a table.
  @param[in]    name            table name
  @param[in,out]        dd_table        data dictionary table
  @return error number
  @retval 0 on success */
  int delete_table(const char *name, const dd::Table *dd_table) override;

  /** Rename a table.
  @param[in]    from            table name before rename
  @param[in]    to              table name after rename
  @param[in]    from_table      data dictionary table before rename
  @param[in,out]        to_table        data dictionary table after rename
  @return       error number
  @retval       0 on success */
  int rename_table(const char *from, const char *to,
                   const dd::Table *from_table, dd::Table *to_table) override;

  int check(THD *thd, HA_CHECK_OPT *check_opt) override;

  /** Repair a partitioned table.
  Only repairs records in wrong partitions (moves them to the correct
  partition or deletes them if not in any partition).
  @param[in]    thd             MySQL THD object/thread handle.
  @param[in]    repair_opt      Repair options.
  @return       0 or error code. */
  int repair(THD *thd, HA_CHECK_OPT *repair_opt) override;

  /** Get the current auto_increment value.
  @param[in]    offset                  Table auto-inc offset.
  @param[in]    increment               Table auto-inc increment.
  @param[in]    nb_desired_values       Number of required values.
  @param[out]   first_value             The auto increment value.
  @param[out]   nb_reserved_values      Number of reserved values. */
  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values, ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;

  /* Get partition row type
  @param[in] partition_table partition table
  @param[in] part_id Id of partition for which row type to be retrieved
  @return Partition row type. */
  enum row_type get_partition_row_type(const dd::Table *partition_table,
                                       uint part_id) override;

  int cmp_ref(const uchar *ref1, const uchar *ref2) const override;

  int read_range_first(const key_range *start_key, const key_range *end_key,
                       bool eq_range_arg, bool sorted) override {
    return (Partition_helper::ph_read_range_first(start_key, end_key,
                                                  eq_range_arg, sorted));
  }

  void position(const uchar *record) override {
    Partition_helper::ph_position(record);
  }

  bool check_if_incompatible_data(HA_CREATE_INFO *info [[maybe_unused]],
                                  uint table_changes
                                  [[maybe_unused]]) override {
    CDE_ASSERT_DEBUG(false);
    return COMPATIBLE_DATA_NO;
  }

  int delete_all_rows() override { return (handler::delete_all_rows()); }

  int disable_indexes(uint mode [[maybe_unused]]) override {
    return (HA_ERR_WRONG_COMMAND);
  }

  int enable_indexes(uint mode [[maybe_unused]]) override {
    return (HA_ERR_WRONG_COMMAND);
  }

  bool get_foreign_dup_key(char *child_table_name [[maybe_unused]],
                           uint child_table_name_len [[maybe_unused]],
                           char *child_key_name [[maybe_unused]],
                           uint child_key_name_len [[maybe_unused]]) override {
    CDE_ASSERT_DEBUG(false);
    return false;
  }

  int read_range_next() override {
    return (Partition_helper::ph_read_range_next());
  }

  uint32_t calculate_key_hash_value(Field **field_array) override {
    return (Partition_helper::ph_calculate_key_hash_value(field_array));
  }

  Table_flags table_flags() const override {
    return (ha_cde::table_flags() | HA_CAN_REPAIR);
  }

  void release_auto_increment() override {
    Partition_helper::ph_release_auto_increment();
  }

  /** See Partition_handler. */
  void get_dynamic_partition_info(ha_statistics *stat_info,
                                  ha_checksum *check_sum,
                                  uint part_id) override {
    Partition_helper::get_dynamic_partition_info_low(stat_info, check_sum,
                                                     part_id);
  }

  uint alter_flags(uint flags [[maybe_unused]]) const override {
    return (HA_PARTITION_FUNCTION_SUPPORTED | HA_INPLACE_CHANGE_PARTITION);
  }

  Partition_handler *get_partition_handler() override {
    return (static_cast<Partition_handler *>(this));
  }

  void set_part_info(partition_info *part_info, bool early) override {
    Partition_helper::set_part_info_low(part_info, early);
  }

  void initialize_partitioning(partition_info *part_info, bool early) {
    Partition_helper::set_part_info_low(part_info, early);
  }

  handler *get_handler() override { return (static_cast<handler *>(this)); }

 private:
  /** Pointer to Ha_innopart_share on the TABLE_SHARE. */
  HaCdepartShare *m_partShare{nullptr};

  dstore_handler_t **m_dstoreHandlers{nullptr};

  inline dstore_handler_t *getDStorePart(uint partId) const {
    CDE_ASSERT_DEBUG(m_dstoreHandlers != nullptr);
    CDE_ASSERT_DEBUG(partId < m_partShare->getNumParts());
    CDE_ASSERT_DEBUG(partId < m_tot_parts);
    CDE_ASSERT_DEBUG(m_partShare->getNumParts() == m_tot_parts);
    return (m_dstoreHandlers[partId]);
  }

  /** New partitions during ADD/REORG/... PARTITION. */
  Altered_partitions *m_new_partitions;

  /** Reset state of file to after 'open'. This function is called
  after every statement for all tables used by that statement. */
  int reset() override;

  /** Initialize dstore handlers for DStore parititon tables
  @param[in]    thd               Thread context
  @param[in]    partTable         DStore partition tables
  @param[in]    memRoot           mem root from which allocate memory
  @return       0  or error */
  int64_t initDStoreHandlerParts(THD *thd, cde_dict_t **tableParts,
                                 Memory::MemHeap &memRoot);

  /** Change active partition.
  Copies needed info into m_prebuilt from the partition specific memory.
  @param[in]    part_id Partition to set as active. */
  void setPartition(uint part_id);

  /** Update active partition.
  Copies needed info from m_prebuilt into the partition specific memory.
  @param[in]    part_id Partition to set as active. */
  void updatePartition(uint part_id);

  /** TRUNCATE a DStore partitioned table.
  @param[in]            name            table name
  @param[in]            form            table definition
  @param[in,out]        table_def       dd::Table describing table to be
  truncated. Can be adjusted by SE, the changes will be saved into
  the data-dictionary at statement commit time.
  @return       error number
  @retval 0 on success */
  int CdeTruncatePartitions(const char *name, TABLE *form,
                            dd::Table *table_def);

  /** Set the autoinc column max value.
  This should only be called once from ha_innobase::open().
  Therefore there's no need for a covering lock.
  @param[in]    no_lock If locking should be skipped. Not used!
  @return       0 for success or error code. */
  int initialize_auto_increment(bool no_lock) override;

  /** write row to new partition.
  @param[in]    new_part        New partition to write to.
  @return 0 for success else error code. */
  int write_row_in_new_part(uint new_part) override {
    CDE_ASSERT_DEBUG(false);
    (void)new_part;
    return 0;
  }

  /** Write a row in specific partition.
  Stores a row in a DStore database, to the table specified in this
  handle.
  @param[in]    part_id Partition to write to.
  @param[in]    record  A row in MySQL format.
  @return error code. */
  int write_row_in_part(uint part_id, uchar *record) override;

  /** Update a row in partition.
  Updates a row given as a parameter to a new value.
  @param[in]    part_id Partition to update row in.
  @param[in]    old_row Old row in MySQL format.
  @param[in]    new_row New row in MySQL format.
  @return       0 or error number. */
  int update_row_in_part(uint part_id, const uchar *old_row,
                         uchar *new_row) override;

  /** Deletes a row in partition.
  @param[in]    part_id Partition to delete from.
  @param[in]    record  Row to delete in MySQL format.
  @return       0 or error number. */
  int delete_row_in_part(uint part_id, const uchar *record) override;

  /** Return first record in index from a partition.
  @param[in]    part    Partition to read from.
  @param[out]   record  First record in index in the partition.
  @return error number or 0. */
  int index_first_in_part(uint part, uchar *record) override;

  /** Return last record in index from a partition.
  @param[in]    part    Partition to read from.
  @param[out]   record  Last record in index in the partition.
  @return error number or 0. */
  int index_last_in_part(uint part, uchar *record) override;

  /** Return previous record in index from a partition.
  @param[in]    part    Partition to read from.
  @param[out]   record  Last record in index in the partition.
  @return error number or 0. */
  int index_prev_in_part(uint part, uchar *record) override;

  /** Return next record in index from a partition.
  @param[in]    part    Partition to read from.
  @param[out]   record  Last record in index in the partition.
  @return error number or 0. */
  int index_next_in_part(uint part, uchar *record) override;

  /** Return next same record in index from a partition.
  This routine is used to read the next record, but only if the key is
  the same as supplied in the call.
  @param[in]    part    Partition to read from.
  @param[out]   record  Last record in index in the partition.
  @param[in]    key     Key to match.
  @param[in]    length  Length of key.
  @return error number or 0. */
  int index_next_same_in_part(uint part, uchar *record, const uchar *key,
                              uint length) override;

  /** Start index scan and return first record from a partition.
  This routine starts an index scan using a start key. The calling
  function will check the end key on its own.
  @param[in]    part    Partition to read from.
  @param[out]   record  First matching record in index in the partition.
  @param[in]    key     Key to match.
  @param[in]    keypart_map     Which part of the key to use.
  @param[in]    find_flag       Key condition/direction to use.
  @return error number or 0. */
  int index_read_map_in_part(uint part, uchar *record, const uchar *key,
                             key_part_map keypart_map,
                             enum ha_rkey_function find_flag) override;

  /** Return last matching record in index from a partition.
  @param[in]    part    Partition to read from.
  @param[out]   record  Last matching record in index in the partition.
  @param[in]    key     Key to match.
  @param[in]    keypart_map     Which part of the key to use.
  @return error number or 0. */
  int index_read_last_map_in_part(uint part, uchar *record, const uchar *key,
                                  key_part_map keypart_map) override;

  /** Start index scan and return first record from a partition.
  This routine starts an index scan using a start and end key.
  @param[in]    part            Partition to read from.
  @param[in,out]        record          First matching record in index in the
  partition, if NULL use table->record[0] as return buffer.
  @param[in]    start_key       Start key to match.
  @param[in]    end_key         End key to match.
  @param[in]    sorted          Return rows in sorted order.
  @return       error number or 0. */
  int read_range_first_in_part(uint part, uchar *record,
                               const key_range *start_key,
                               const key_range *end_key, bool sorted) override;

  /** Return next record in index range scan from a partition.
  @param[in]    part    Partition to read from.
  @param[in,out]        record  First matching record in index in the partition,
  if NULL use table->record[0] as return buffer.
  @return       error number or 0. */
  int read_range_next_in_part(uint part, uchar *record) override;

  /** Start index scan and return first record from a partition.
  This routine starts an index scan using a start key. The calling
  function will check the end key on its own.
  @param[in]    part    Partition to read from.
  @param[out]   record  First matching record in index in the partition.
  @param[in]    index   Index to read from.
  @param[in]    key     Key to match.
  @param[in]    keypart_map     Which part of the key to use.
  @param[in]    find_flag       Key condition/direction to use.
  @return error number or 0. */
  int index_read_idx_map_in_part(uint part, uchar *record, uint index,
                                 const uchar *key, key_part_map keypart_map,
                                 enum ha_rkey_function find_flag) override;

  /** Setup the ordered record buffer and the priority queue.
 @param[in]    used_parts      Number of used partitions in query.
 @return false for success, else true. */
  int init_record_priority_queue_for_parts(uint used_parts) override;

  /** Destroy the ordered record buffer and the priority queue. */
  void destroy_record_priority_queue_for_parts() override;

  /** Initialize sampling.
  @param[out] scan_ctx  A scan context created by this method that has to be
  used in sample_next
  @param[in]  sampling_percentage percentage of records that need to be
  sampled
  @param[in]  sampling_seed       random seed that the random generator will
  use
  @param[in]  sampling_method     sampling method to be used; currently only
  SYSTEM sampling is supported
  @param[in]  tablesample         true if the sampling is for tablesample
  @return 0 for success, else one of the HA_xxx values in case of error. */
  int sample_init(void *&scan_ctx, double sampling_percentage,
                  int sampling_seed, enum_sampling_method sampling_method,
                  const bool tablesample) override;

  /** Get the next record for sampling.
  @param[in]  scan_ctx  Scan context of the sampling
  @param[in]  buf       buffer to place the read record
  @return 0 for success, else one of the HA_xxx values in case of error. */
  int sample_next(void *scan_ctx, uchar *buf) override;

  /** End sampling.
  @param[in] scan_ctx  Scan context of the sampling
  @return 0 for success, else one of the HA_xxx values in case of error. */
  int sample_end(void *scan_ctx) override;

  /** Initialize random read/scan of a specific partition.
  @param[in]    part_id         Partition to initialize.
  @param[in]    scan            True for scan else random access.
  @return error number or 0. */
  int rnd_init_in_part(uint part_id, bool scan) override;

  /** Get next row during scan of a specific partition.
  Also used to read the FIRST row in a table scan.
  @param[in]    part_id Partition to read from.
  @param[out]   buf     Next row.
  @return error number or 0. */
  int rnd_next_in_part(uint part_id, uchar *buf) override;

  /** End random read/scan of a specific partition.
  @param[in]    part_id         Partition to end random read/scan.
  @param[in]    scan            True for scan else random access.
  @return error number or 0. */
  int rnd_end_in_part(uint part_id, bool scan) override;

  /** Return position for cursor in last used partition.
  Stores a reference to the current row to 'ref' field of the handle. Note
  that in the case where we have generated the clustered index for the
  table, the function parameter is illogical: we MUST ASSUME that 'record'
  is the current 'position' of the handle, because if row ref is actually
  the row id internally generated in DStore, then 'record' does not contain
  it. We just guess that the row id must be for the record where the handle
  was positioned the last time.
  @param[out]   ref_arg Pointer to buffer where to write the position.
  @param[in]    record  Record to position for. */
  void position_in_last_part(uchar *ref_arg, const uchar *record) override;

  /** Read row using position using given record to find.
  This works as position()+rnd_pos() functions, but does some
  extra work,calculating m_last_part - the partition to where
  the 'record' should go.
  Only useful when position is based on primary key
  (HA_PRIMARY_KEY_REQUIRED_FOR_POSITION).
  @param[in]    record  Current record in MySQL Row Format.
  @return       0 for success else error code. */
  /* This function should never be used as in DStore
  we dont have HA_PRIMARY_KEY_REQUIRED_FOR_POSITION */
  int rnd_pos_by_record(uchar *record) override {
    CDE_ASSERT_DEBUG(false);
    (void)record;
    return 0;
  }

  /** Compare key and rowid.
  Helper function for sorting records in the priority queue.
  a/b points to table->record[0] rows which must have the
  key fields set. The bytes before a and b store the rowid.
  This is used for comparing/sorting rows first according to
  KEY and if same KEY, by rowid (ref).

  @param[in]    key_info        Null terminated array of index
  information.
  @param[in]    a               Pointer to record+ref in first record.
  @param[in]    b               Pointer to record+ref in second record.
  @return Return value is SIGN(first_rec - second_rec)
  @retval       0       Keys are equal.
  @retval       -1      second_rec is greater than first_rec.
  @retval       +1      first_rec is greater than second_rec. */
  static int keyAndRowidCmp(KEY **key_info, uchar *a, uchar *b);

  /** Copy a cached MySQL row.
  If requested, also avoids overwriting non-read columns.
  @param[out]   buf             Row in MySQL format.
  @param[in]    cached_row      Which row to copy. */
  void copy_cached_row(uchar *buf, const uchar *cached_row) override;

  /** Open an InnoDB table.
  @param[in]    name            table name
  @param[in]    mode            access mode
  @param[in]    test_if_locked  test if the file to be opened is locked
  @param[in]    table_def       dd::Table describing table to be opened
  @retval 1 if error
  @retval 0 if success */
  int open(const char *name, int mode, uint test_if_locked,
           const dd::Table *table_def) override;

  int close() override;

  double scan_time() override;

  int index_init(uint index, bool sorted) override;

  int index_end() override;

  int rnd_init(bool scan) override {
    return (Partition_helper::ph_rnd_init(scan));
  }

  int rnd_end() override { return (Partition_helper::ph_rnd_end()); }

  int external_lock(THD *thd, int lock_type) override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             thr_lock_type lock_type) override;

  int write_row(uchar *record) override {
    return Partition_helper::ph_write_row(record);
  }

  int update_row(const uchar *old_record, uchar *new_record) override {
    return (Partition_helper::ph_update_row(old_record, new_record));
  }

  int delete_row(const uchar *record) override {
    return (Partition_helper::ph_delete_row(record));
  }

  /** Delete all rows in the requested partitions.
  Done by deleting the partitions and recreate them again.
  @param[in,out]        dd_table        dd::Table object for partitioned table
  which partitions need to be truncated. Can be adjusted by this call.
  Changes to the table definition will be persisted in the data-dictionary
  at statement commit time.
  @return       0 or error number. */
  int truncate_partition_low(dd::Table *dd_table) override;

  /** Exchange partition.
  Low-level primitive which implementation is provided here.
  @param[in]    part_id                 The id of the partition to be exchanged
  @param[in]    part_table              partitioned table to be exchanged
  @param[in]    swap_table              table to be exchanged
  @return error number
  @retval 0     on success */
  int exchange_partition_low(uint part_id, dd::Table *partTable,
                             dd::Table *swapTable) override;

  THD *get_thd() const override { return ha_thd(); }

  TABLE *get_table() const override { return table; }

  bool get_eq_range() const override { return eq_range; }

  void set_eq_range(bool eq_range_arg) override { eq_range = eq_range_arg; }

  void set_range_key_part(KEY_PART_INFO *key_part) override {
    range_key_part = key_part;
  }

  /** Copy used fields from cached row.
  Copy cache record field by field, don't touch fields that
  are not covered by current key.
  @param[out]     buf            Where to copy the MySQL row.
  @param[in]      cachedRec      What to copy (in MySQL row format). */
  void CopyCachedFieldsForMysql(uchar *buf, const uchar *cachedRec);

 protected:
  int rnd_next(uchar *record) override {
    return (Partition_helper::ph_rnd_next(record));
  }

  int rnd_pos(uchar *record, uchar *pos) override;

  int records(ha_rows *num_rows) override;

  int records_from_index(ha_rows *num_rows, uint) override {
    /* Force use of cluster index until we implement sec index parallel scan. */
    return haCdepart::records(num_rows);
  }

  int index_next(uchar *record) override {
    return (Partition_helper::ph_index_next(record));
  }

  int index_next_same(uchar *record, const uchar *, uint keylen) override {
    return (Partition_helper::ph_index_next_same(record, keylen));
  }

  int index_prev(uchar *record) override {
    return (Partition_helper::ph_index_prev(record));
  }

  int index_first(uchar *record) override {
    return (Partition_helper::ph_index_first(record));
  }

  int index_last(uchar *record) override {
    return (Partition_helper::ph_index_last(record));
  }

  int index_read_last_map(uchar *record, const uchar *key,
                          key_part_map keypart_map) override {
    return (Partition_helper::ph_index_read_last_map(record, key, keypart_map));
  }

  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override {
    return (
        Partition_helper::ph_index_read_map(buf, key, keypart_map, find_flag));
  }

  int index_read_idx_map(uchar *buf, uint index, const uchar *key,
                         key_part_map keypart_map,
                         enum ha_rkey_function find_flag) override {
    return (Partition_helper::ph_index_read_idx_map(buf, index, key,
                                                    keypart_map, find_flag));
  }

  /** Updates and return statistics.
  Returns statistics information of the table to the MySQL interpreter,
  in various fields of the handle object.
  // @param[in]    flag            Flags for what to update and return.
  @param[in]    is_analyze      True if called from "::analyze()".
  // @return       HA_ERR_* error code or 0. */
  int info_impl(uint flag, bool is_analyze) override;

  bool CanReuseColumnsDecoding() const override {
    return m_reuseColumnsDecoding;
  }

  bool m_reuseColumnsDecoding{false};

  ha_rows m_rowsInAllPartitions{0};
};

} /* namespace CDE */

#endif /* ha_cdepart.h */
