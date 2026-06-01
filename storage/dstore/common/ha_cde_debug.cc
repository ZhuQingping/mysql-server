/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * ha_cde_debug.cc
 *
 *
 * IDENTIFICATION
 * src/ha_cde_debug.cc
 *
 * -------------------------------------------------------------------------
 */
// clang-format off
#include "perfcounters.h"
#include "common/cde_alloc.h"
#include "common/cde_perf.h"
#include "common/cde_trxmgr.h"

#include "perfpublisher.h"
#include "perfpublisherproto.h"

#include "dict/cde_dict.h"
#include "boot/cde_instance.h"

#include "heap/dstore_heap_interface.h"
#include "systable/dstore_relation.h"
#include "index/dstore_index_interface.h"
#include "tuple/dstore_tuple_interface.h"
// clang-format on

#include <iomanip>
namespace CDE {
std::shared_ptr<Huawei::Common::CounterUnixPublisher> gPublisherPtr = nullptr;

static bool is_initilized_for_debug = false;
std::unique_ptr<cde_perf_counters> cde_perf_counters::_singleInstance(nullptr);

cde_perf_counters::cde_perf_counters()
    // NOLINTNEXTLINE
    : CDE_COUNTERS_DEF(COUNTER_LIST_INIT) _ccReg(this, "Cde") {
  CDE_COUNTERS_DEF(COUNTER_LIST_REG);
  CDE_COUNTERS_COUNT_ONLY(COUNTER_LIST_SET_ONLY_COUNT);
}

cde_perf_counters *cde_perf_counters::createInstance() {
  _singleInstance = std::unique_ptr<cde_perf_counters>(new cde_perf_counters());
  return _singleInstance.get();
}

void cde_perf_counters::terminateInstance() {
  // NOLINTNEXTLINE
  auto inst = _singleInstance.release();
  // NOLINTNEXTLINE
  delete inst;
}

void CdeInitializeDstoreInstance() {
  if (!is_initilized_for_debug) {
    auto g_instance = CdeGetDstoreInstance();
    auto dstore_thrd_ctx = DSTORE::ThreadContextInterface::Create();
    dstore_thrd_ctx->InitializeBasic();
    dstore_thrd_ctx->SetXactPdbId(DSTORE::g_defaultPdbId);
    dstore_thrd_ctx->InitStorageContext(DSTORE::g_defaultPdbId);
    dstore_thrd_ctx->InitTransactionRuntime(DSTORE::g_defaultPdbId, nullptr,
                                            nullptr);
    g_instance->AddVisibleThread(dstore_thrd_ctx, DSTORE::g_defaultPdbId);
    is_initilized_for_debug = true;
  }
}

/*
 * show the tables state: totaltuples, deadTuples,liveTuples,PageCounts
 */
void CdeShowSampleStats(std::ostream &os,
                        const Huawei::Common::PublisherFilter *filter) {
  std::string filter_value(filter->value);
  std::vector<std::string> values =
      Huawei::Common::tokenSplit(filter_value, ':');
  if (values.size() != 1) {
    os << "error: the command format is not right! "
          "should be: perfclient  -t unix --pid `pidof mysqld` -c sampletable "
          "-f name=./test/sbtest1 "
       << std::endl;
    return;
  }
  std::string table_name = values[0];
  cde_dict_t *cde_table = DictSysGetTable(table_name.c_str());
  DictTableRefGuard dictRefGuard(cde_table);
  DSTORE::HeapScanHandler *heap_table_handler =
      HeapInterface::CreateHeapScanHandler(cde_table->dstore_relation);
  CdeInitializeDstoreInstance();
  DSTORE::SnapshotData dstore_snapshot;
  CDESetSnapshotByTrans(dstore_snapshot);
  HeapInterface::BeginScan(heap_table_handler, &dstore_snapshot);
  DSTORE::HeapSampleScanContext context;
  int totalLiveTups = 0, totalDeadTups = 0, totalTups = 0;
  DSTORE::StorageRelationData *table_rel = cde_table->get_dstore_relation();
  uint64 page_cnt =
      StorageTableInterface::GetTableBlockCount(table_rel->tableSmgr);
  DSTORE::RetStatus status = DSTORE::RetStatus::DSTORE_SUCC;

  for (uint64 i = 0; i < page_cnt; ++i) {
    context.SetSampleBlockNum(i);
    Huawei::Common::LatencyCounter::Timer t(
        cde_perf_counters::getInstance()->_cde_dstore_sample_latency);
    status = HeapInterface::SampleScan(heap_table_handler, &context);
    t.end();
    totalLiveTups += context.numLiveTuples;
    totalDeadTups += context.numDeadTuples;
    totalTups += context.numTuples;

    for (int tuple_num = 0; tuple_num < context.numTuples &&
                            tuple_num < DSTORE::MAX_ITEM_OFFSET_NUMBER;
         tuple_num++) {
      TupleInterface::DestroyTuple(context.tuples[tuple_num],
                                   CdeFreeMemForDtuple);
    }
  }

  os << "show sampleScan stats: \n "
     << " totalLiveTups= " << totalLiveTups
     << "\n  totalDeadTups = " << totalDeadTups
     << "\n  totalTups = " << totalTups << "\n"
     << "\n pageCounts=" << page_cnt
     << "\n "
        "status="
     << status << "\n";
  HeapInterface::EndScan(heap_table_handler);
  HeapInterface::DestroyHeapScanHandler(heap_table_handler);
}

/*
 * show the index tables all records
 */
void CdeShowIndexTable(std::ostream &os,
                       const Huawei::Common::PublisherFilter *filter) {
  std::string filter_value(filter->value);
  std::vector<std::string> values =
      Huawei::Common::tokenSplit(filter_value, ':');
  if (values.size() != 2) {
    os << "error: the command format is not right! "
          "should be: perfclient  -t unix --pid `pidof mysqld` -c indextable "
          "-f name=./test/sbtest1:PRIMARY "
       << std::endl;
    return;
  }
  std::string table_name = values[0];
  std::string key_name = values[1];
  cde_dict_t *cde_table = DictSysGetTable(table_name.c_str());
  DictTableRefGuard dictRefGuard(cde_table);
  if (cde_table == nullptr) {
    os << "error:" << table_name
       << " table is not exist,maybe not in cache,you should open it in mysql "
          "client first "
       << std::endl;
    return;
  }
  cde_dict_index_t *current_index = nullptr;
  const std::vector<cde_dict_index_t *> *idx_dict_vec =
      cde_table->get_index_dict_vec();
  std::string index_all = " ";
  for (cde_dict_index_t *idx_dict : *idx_dict_vec) {
    if (!idx_dict->IsCommitted()) {
      continue;
    }

    auto iter_key_name = idx_dict->name;
    index_all += iter_key_name;
    index_all += " ";
    if (iter_key_name == key_name) {
      current_index = idx_dict;
      break;
    }
  }

  if (current_index == nullptr) {
    os << "error:" << key_name
       << " index is not exist, the index list is: " << index_all << std::endl;
    return;
  }

  CdeInitializeDstoreInstance();

  DSTORE::IndexScanHandler *index_scan_handler =
      IndexInterface::ScanBegin(current_index->rel, current_index->rel->index,
                                current_index->index_col_num, 0);
  DSTORE::SnapshotData dstore_snapshot;
  CDESetSnapshotByTrans(dstore_snapshot);
  IndexInterface::IndexScanSetSnapshot(index_scan_handler, &dstore_snapshot);
  IndexInterface::ScanSetWantItup(index_scan_handler, true);
  DSTORE::IndexTuple *itup;
  bool recheck = false;
  os << "show the index table: " << table_name << ": " << key_name << "\n";
  std::vector<Datum> value_s(
      static_cast<unsigned long>(current_index->rel->attr->natts));
  std::vector<char> isNulls(
      static_cast<unsigned long>(current_index->rel->attr->natts));

  os << "-----------------------------" << std::endl;
  const int colWidth = 6;
  for (int j = 0; j < current_index->rel->attr->natts; j++) {
    os << "| " << std::left << std::setw(colWidth - 1)
       << cde_table->col_names[current_index->index_cols[j]];
  }
  os << "|"
     << "\n";

  while (true) {
    itup = IndexInterface::OnlyScanNext(
        index_scan_handler, DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION,
        &current_index->rel->attr, &recheck);

    if (!itup) break;
    TupleInterface::DeformIndexTuple(itup, current_index->rel->attr,
                                     value_s.data(), (bool *)isNulls.data());
    int i = 0;
    for (const Datum &value : value_s) {
      if ((bool)isNulls[i])
        os << "| " << std::right << std::setw(colWidth - 1) << "NULL";
      else
        os << "| " << std::right << std::setw(colWidth - 1) << (int)value;
      i++;
    }
    os << "|"
       << "\n";
  }

  os << "-----------------------------" << std::endl;
  IndexInterface::ScanEnd(index_scan_handler);
}

void CdeDebugInit() {
  gPublisherPtr =
      std::make_shared<Huawei::Common::CounterUnixPublisher>(SLICE_SOCKET_NAME);
  gPublisherPtr->start();
  gPublisherPtr->addCustomCommand(
      Huawei::Common::PublisherCommand::ShowIndexTable, CdeShowIndexTable);
  gPublisherPtr->addCustomCommand(
      Huawei::Common::PublisherCommand::ShowSampleTable, CdeShowSampleStats);

  cde_perf_counters::createInstance();
}

void CdeDebugExit() {
  gPublisherPtr->stop();
  cde_perf_counters::terminateInstance();
}
} /* namespace CDE */
