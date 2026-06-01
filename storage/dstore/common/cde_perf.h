/********************************************************************
 * Copyright (C) Huawei Technologies, 2018
 * Performance counters framework: server to publish perfromance measurments
 *
 *  Contains declaration of code that sends perfromance counters over the
 *  network to clients
 ********************************************************************/
#ifndef __CDE_PERF__
#define __CDE_PERF__

#include <memory>
#include "perfcounters.h"
// clang-format off
/**
 * index_read_latency: the latency of the ha_cde::index_read
 * cde_dstore_scan_next: the latency of the dstore_interface::scan_next
 * cde_dstore_onlyscannext: the latency of the dstore_interface::onlyscannext
 * cde_write_row_latency: the latency of the ha_cde::write_row
 * cde_create_table_latency: the latency of the  ha_cde::create
 */
namespace CDE {
#define CDE_COUNTERS_DEF(_def) \
_def(Latency, index_read_latency, index_read_latency)    \
_def(Latency, cde_dstore_scan_next, cde_dstore_scan_next)    \
_def(Latency, cde_dstore_scan_rescan, cde_dstore_scan_rescan)    \
_def(Latency, cde_dstore_onlyscannext, cde_dstore_onlyscannext)    \
_def(Latency, cde_write_row_latency, cde_write_row_latency)    \
_def(Latency, cde_index_lookup, cde_index_lookup)    \
_def(Latency, cde_create_table_latency,cde_create_table_latency)    \
_def(Latency, records_in_range_latency, records_in_range_latency) \
_def(Latency, cde_info_latency, cde_info_latency) \
_def(Latency, cde_analyze_latency, cde_analyze_latency) \
_def(Numeric64, info_flag_time, info_flag_time) \
_def(Numeric64, info_flag_variable, info_flag_variable) \
_def(Numeric64, info_flag_const, info_flag_const) \
_def(Latency, cde_dstore_sample_latency, cde_dstore_sample_latency) \
_def(Numeric64, info_flag_auto, info_flag_auto) \
_def(Latency, inplace_build_indexes_latency, inplace_build_indexes_latency) \
_def(Numeric64, alive_dstore_sessions, alive_dstore_sessions) \
_def(SimpleNumericU64, dict_size, dict_size) \
_def(Numeric64, cde_icp_check, cde_icp_check) \
_def(Numeric64, cde_icp_match, cde_icp_match) \
_def(Numeric64, handler_mem_allocted, handler_mem_allocted) \
_def(Numeric64, session_rel_info_mem_allocted, session_rel_info_mem_allocted) \
_def(Numeric64, dstore_trx_thread_id_pushdown_hit, dstore_trx_thread_id_pushdown_hit) \
_def(Numeric64, dstore_locks_thread_id_pushdown_hit, dstore_locks_thread_id_pushdown_hit) \
_def(Numeric64, dstore_memory_thread_id_pushdown_hit, dstore_memory_thread_id_pushdown_hit) \
_def(Numeric64, dstore_undo_zone_id_pushdown_hit, dstore_undo_zone_id_pushdown_hit) \
_def(Numeric64, dstore_segment_pushdown_hit, dstore_segment_pushdown_hit) \
_def(Numeric64, dstore_indexes_pushdown_hit, dstore_indexes_pushdown_hit) \
_def(Latency, inplace_rebuild_heap_latency, inplace_rebuild_heap_latency) \
_def(Latency, inplace_rebuild_heap_single_insert_latency, inplace_rebuild_heap_single_insert_latency) \
_def(Latency, inplace_rebuild_heap_batch_insert_latency, inplace_rebuild_heap_batch_insert_latency)

/** Those counters are increasing only. No need to store max/min/latest
 * values for those. min is always 0 and max == latest == counter */
#define CDE_COUNTERS_COUNT_ONLY(_def) \
_def(cde_icp_check) \
_def(cde_icp_match)
// clang-format on

class cde_perf_counters {
  /* Pointer to singleton instance. */
  static std::unique_ptr<cde_perf_counters> _singleInstance;

  /* Private constructor to prevent external instantiantion */
  cde_perf_counters();

 public:
  /* InnoDB perf counters. */
  CDE_COUNTERS_DEF(COUNTER_LIST_DECL);
  Huawei::Common::CounterNodeRegistration _ccReg;

  /* Create global singleton instance. */
  static cde_perf_counters *createInstance();

  /* Get singleton instance pointer. */
  static cde_perf_counters *getInstance() { return _singleInstance.get(); }

  /* Terminate global singleton instance. */
  static void terminateInstance();
};
} /* namespace CDE */
#endif  // _CDE_PERF_
