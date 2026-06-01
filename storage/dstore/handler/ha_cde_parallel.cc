/*
  Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

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

#include "handler/ha_cde_parallel.h"
#include "debug_sync.h"

#include "boot/cde_instance.h"
#include "common/cde_trxmgr.h"
#include "framework/dstore_instance.h"
#include "framework/dstore_parallel.h"
#include "framework/dstore_thread.h"
#include "framework/dstore_thread_interface.h"
#include "heap/dstore_heap_interface.h"
#include "transaction/dstore_transaction.h"  // DSTORE::Transaction

namespace CDE {

std::atomic<size_t> ParallelReaderInterface::m_activeThreads{0};

size_t ParallelReaderInterface::AcquireThreads(size_t requireThreads) {
  assert(requireThreads > 0 && requireThreads <= 128);

  size_t current = m_activeThreads.load(std::memory_order_relaxed);

  while (true) {
    if (current >= MAX_THREADS) {
      return 0;
    }

    const size_t available = MAX_THREADS - current;

    if (available < requireThreads) {
      return 0;
    }

    const size_t desired = current + requireThreads;

    if (m_activeThreads.compare_exchange_weak(current, desired,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
      return requireThreads;
    }
  }
}

void ParallelReaderInterface::ReleaseThreads(size_t releaseThreads) {
  assert(releaseThreads > 0 && releaseThreads <= 128);

  // 1.fast path: should success in normal cases.
  size_t prev =
      m_activeThreads.fetch_sub(releaseThreads, std::memory_order_release);
  if (likely(prev >= releaseThreads)) {
    return;
  }
  // 2.exception handle
  CDE_LOG_ERROR(
      "Thread quota over-release: attempted=%zu, available=%zu, "
      "forcing to 0",
      releaseThreads, prev);
  size_t expected = prev - releaseThreads;
  while (!m_activeThreads.compare_exchange_weak(
      expected, 0, std::memory_order_release, std::memory_order_relaxed)) {
    /* other threads did it. */
    if (expected <= MAX_THREADS) {
      break;
    }
  }
}

int ParallelHeapScanReaderBase::Init() {
  DBUG_EXECUTE_IF("parallel_reader_init_fail", return CDE_ERROR;);
  // reserver threads
  m_threadNum = AcquireThreads(m_threadNum);
  if (m_threadNum == 0) {
#ifndef NDEBUG
    CDE_LOG_INFO("[Parallel reader] parallel threads exhausted.");
#endif
    return CDE_ERROR;
  }
  // create parallel controller
  m_controller = ParallelInterface::CreateParallelWorkController(m_threadNum);
  if (!m_controller) {
    return CDE_ERROR;
  }

  m_mainTxn = DSTORE::thrd->GetActiveTransaction();

  /* Explicitly clear thread-local error state before proceeding. Certain dstore
  operations propagate error information via thrd->error without resetting it
  upon completion (e.g., TbsDataFile::ProcessFileSizeExceedLimit ->
  LockMgr::Lock -> LocalLock::GrantIfAlreadyHold -> StorageSetErrorCodeOnly).
  This ensures no stale error state persists across operations. */
  DSTORE::StorageClearError();

  return CDE_OK;
}

ParallelHeapScanReaderBase::ParallelHeapScanTaskInfo *
ParallelHeapScanReaderBase::CreateParallelHeapScanTaskInfo() {
  ParallelHeapScanTaskInfo *task =
      new (current_thd->mem_root) ParallelHeapScanTaskInfo();
  if (task == nullptr) {
    return nullptr;
  }
  task->m_thd = current_thd;
  task->m_cb = &m_callback;
  task->m_ctx = m_callbackCtx;
  return task;
}

int ParallelHeapScanReaderBase::Run() {
  std::vector<std::thread> workers;
  workers.reserve(m_threadNum - 1);

  std::vector<ParallelHeapScanTaskInfo *> tasks;
  tasks.reserve(m_threadNum);

  for (uint16_t i = 0; i < m_threadNum; ++i) {
    ParallelHeapScanTaskInfo *task = CreateParallelHeapScanTaskInfo();
    DBUG_EXECUTE_IF("parallel_reader_omm_error", task = nullptr;);
    if (!task) {
      ReleaseThreads(m_threadNum);
      CDE_LOG_ERROR("[Parallel reader] task create failed due to OOM.");
      MarkAsFatal();
      return CDE_ERROR;
    }
    tasks.push_back(task);
  }
  DEBUG_SYNC(current_thd, "parallel_reader_chiled_worker_start");

  ParallelWorkerBarrier barrier(m_threadNum - 1);
  uint16_t threadsCreated = 0;
  for (uint16_t i = 0; i < m_threadNum - 1; ++i) {
    try {
      DBUG_EXECUTE_IF(
          "parallel_reader_create_thread_error_01",
          throw std::runtime_error("injected thread creation error"););
      DBUG_EXECUTE_IF("parallel_reader_create_thread_error_02",
                      if (i == 2) throw std::runtime_error(
                          "injected thread creation error 02"););
      workers.emplace_back([this, i, task = tasks[i + 1], &barrier]() {
        WorkerLoop(i + 1, task, barrier);
      });
      threadsCreated++;
    } catch (...) {
      m_error = true;
      CDE_LOG_ERROR(
          "[Parallel reader] Out of resources when creating threads.");
      break;
    }
  }

  /*
    Correct the barrier's expected count to the number of threads that
    were actually launched. This is the critical fix: without this call,
    WaitAllReady() would block forever if any thread failed to launch.
  */
  CDE_ASSERT(threadsCreated == static_cast<uint16_t>(workers.size()));
  barrier.SetWorkerCount(threadsCreated);

  DEBUG_SYNC(current_thd, "parallel_reader_worker_start");

  /* Phase 1: Wait for all launched workers to finish init. */
  barrier.WaitAllReady();
  barrier.BroadcastStart();

  /* Phase 2: Main thread runs its own task. */
  if (!m_error) {
    WorkerLoop(0, tasks[0], barrier);
  }

  /** If the main thread has been killed, propagate the interrupt signal
  to all active worker threads so they can exit gracefully.
  Note: tasks[0] is executed by the main thread itself, so no interrupt
  signal is needed for it. */
  if (current_thd->is_killed() && threadsCreated != 0) {
    for (uint16_t i = 1; i <= threadsCreated; ++i) {
      if (tasks[i]->m_thrd) {
        tasks[i]->m_thrd->SetInterruptPending();
      }
    }
  }

  DEBUG_SYNC(current_thd, "parallel_reader_worker_end");

  /* Phase 3: Wait for all workers to finish, do global cleanup. */
  barrier.WaitAllExiting();
  barrier.BroadcastExit();

  for (auto &worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  ReleaseThreads(m_threadNum);

  if (m_callBackResult != CDE_OK) {
    CDE_LOG_ERROR("[Parallel reader] callback execution failed.");
    return CDE_ERROR;
  }

  if (m_controller->m_mainThread->GetErrorCode() != STORAGE_OK || m_error) {
    CDE_LOG_ERROR(
        "[Parallel reader] heap scan failed during parallel heap scan.");
    return CDE_ERROR;
  }

  if (current_thd->is_killed()) {
    CDE_LOG_ERROR(
        "[Parallel reader] heap scan failed during parallel heap scan because "
        "statement is cancelled.");
    return CDE_ERROR;
  }

  return CDE_OK;
}

void ParallelHeapScanReaderBase::WorkerLoop(uint16_t threadIdx,
                                            ParallelHeapScanTaskInfo *task,
                                            ParallelWorkerBarrier &barrier) {
  bool isMainThread = (threadIdx == 0);
  cde_session_t *session = nullptr;

  if (!isMainThread) {
    DSTORE::thrd = nullptr;
    CdeConstructSession(session);
    DSTORE::thrd->SetNeedCommBuffer(true);
    DSTORE::thrd->RefreshWorkingVersionNum();
    SingleStmtTrxStart(CdeTrxMapIsolationLevel(task->m_thd->tx_isolation));
    task->m_thrd = DSTORE::thrd;
    barrier.NotifyReadyAndWait();
  }
  DSTORE::Transaction *txn = nullptr;
  if (!isMainThread) {
    txn = DSTORE::thrd->GetActiveTransaction();
    /* Copy main thread's transaction info to worker's transaction, we they can
    get a consistent read view with main thread. */
    txn->SetTransactionSnapshotCsn(m_mainTxn->GetSnapshotCsn());
    txn->SetTransactionSnapshotCid(m_mainTxn->GetSnapshotCid());
    txn->SetCurrentXid(m_mainTxn->GetCurrentXid());
  } else {
    txn = m_mainTxn;
  }
  DSTORE::HeapScanHandler *scanHandler =
      HeapInterface::CreateHeapScanHandler(m_heapRel);
  HeapInterface::BeginScan(scanHandler, txn->GetSnapshot());
  HeapInterface::SetParallelController(scanHandler, m_controller, threadIdx);

  DSTORE::HeapTuple *tuple = nullptr;
  int callbackResult = CDE_OK;
  while (likely(((tuple = HeapInterface::SeqScan(scanHandler)) != nullptr &&
                 !task->m_thd->is_killed() && !m_error))) {
    callbackResult = (*task->m_cb)(tuple, threadIdx, task->m_ctx);
    DBUG_EXECUTE_IF("parallel_reader_callback_error",
                    callbackResult = CDE_ERROR;);
    if (callbackResult != CDE_OK) {
      m_callBackResult = callbackResult;
      break;
    }
  }

  DBUG_EXECUTE_IF("parallel_reader_scan_error",
                  { DSTORE::thrd->error->SetErrorCodeOnly(-1); };);
#ifndef NDEBUG
  if (task->m_thd->is_killed()) {
    CDE_LOG_INFO("[Parallel reader] got THD killed signal and exit now.");
  }
#endif

  // propagate error to other threads.
  if ((DSTORE::thrd->error != nullptr &&
       DSTORE::thrd->GetErrorCode() != STORAGE_OK) ||
      callbackResult != CDE_OK) {
    if (callbackResult == CDE_OK) {
      CDE_LOG_ERROR("[Parallel reader] thread %d heap scan failed due to: %s",
                    threadIdx, DSTORE::thrd->error->GetMessage());
    }
    /* Set error flag to signal other worker threads to terminate.
      We use a separate error flag instead of setting the main thread's error
      directly (as done in ParallelBtreeBuildWorker::ScanWorkerMain) to avoid
      conflicts with HeapScanHandler::PrepareValidCrPage, which legitimately
      sets thd->error during FSM page scanning. Direct manipulation of main
      thread error state would corrupt PrepareValidCrPage's error handling. For
      safety, we only set our own flag without touching the main thread's state.
    */
    m_error = true;
  }
  HeapInterface::EndScan(scanHandler);
  HeapInterface::DestroyHeapScanHandler(scanHandler);
  scanHandler = nullptr;

  if (!isMainThread) {
    if (DSTORE::thrd->error != nullptr &&
        DSTORE::thrd->GetErrorCode() != STORAGE_OK) {
      SingleStmtTrxRollback(session);
    } else {
      SingleStmtTrxCommit(session);
    }
  }

  if (!isMainThread) {
    barrier.NotifyExitingAndWait();
    CdeDestorySession(session);
  }
}
} /* namespace CDE */
