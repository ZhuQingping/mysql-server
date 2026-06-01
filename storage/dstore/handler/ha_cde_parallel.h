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

#ifndef HA_CDE_PARALLEL_H
#define HA_CDE_PARALLEL_H

#include <atomic>
#include <cstddef>
#include <functional>

#include "dml/cde_heap.h"
#include "sql/handler.h"

namespace DSTORE {
class ParallelWorkController;
class Transaction;
}  // namespace DSTORE

namespace CDE {

static constexpr size_t CacheLineSize() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
  return 64;
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__) || \
    defined(_M_ARM)
  return 128;
#else
  return 64;
#endif
}
constexpr size_t CDE_CACHE_LINE_SIZE = CacheLineSize();

template <typename T, size_t CacheLineSize = CDE_CACHE_LINE_SIZE>
class CacheLinePadded : public T {
 private:
  static constexpr size_t m_baseSize = sizeof(T);
  static constexpr size_t m_alignedSize =
      ((m_baseSize + CacheLineSize - 1) / CacheLineSize) * CacheLineSize;
  static constexpr size_t m_paddingSize = m_alignedSize - m_baseSize;

  char m_padding[m_paddingSize > 0 ? m_paddingSize : 1];

 public:
  template <typename... Args>
  CacheLinePadded(Args &&... args) : T(std::forward<Args>(args)...) {}
};

/**
  Lifecycle barrier for parallel query worker threads.

  Provides two symmetric synchronization phases between the main
  thread and a group of worker threads:

    Phase 1 - Start: Each worker signals that it has completed
              initialization and then blocks. The main thread waits
              until all workers are ready, then broadcasts a start
              signal to release them simultaneously.

    Phase 2 - Exit:  Each worker signals that it has finished its
              task and then blocks. The main thread waits until all
              workers are ready to exit, then broadcasts an exit
              signal to release them for cleanup and termination.

  Typical usage:

  ParallelWorkerBarrier barrier(max_workers);
  for (int i = 0; i < max_workers; ++i) {
    try {
      threads.emplace_back([&barrier, ...]() {
        THD *thd = create_thd();
        barrier.NotifyReadyAndWait();    // Ready, then block.
        do_scan_work();
        barrier.NotifyExitingAndWait();  // Done, then block.
        destroy_thd(thd);
      });
    } catch (...) { break; }
  }

  // Correct the expected count to match actual threads launched.
  barrier.SetWorkerCount(threads.size());

  barrier.WaitAllReady();     // Block until all workers initialized.
  barrier.BroadcastStart();   // Release all workers simultaneously.

  do_main_thread_work();

  barrier.WaitAllExiting();   // Block until all workers done.
  barrier.BroadcastExit();    // Release all workers to clean up.

  for (auto &t : threads) t.join();
*/
class ParallelWorkerBarrier {
 public:
  /**
    Constructor.

    @param worker_count  Expected number of worker threads. May be
                         corrected later via SetWorkerCount() if thread
                         creation fails partway through.
  */
  explicit ParallelWorkerBarrier(uint16_t worker_count)
      : m_total(worker_count),
        m_readyCount(0),
        m_startNotified(false),
        m_exitingCount(0),
        m_exitNotified(false) {}

  /** Disallow copy and assignment. */
  ParallelWorkerBarrier(const ParallelWorkerBarrier &) = delete;
  ParallelWorkerBarrier &operator=(const ParallelWorkerBarrier &) = delete;

  // -----------------------------------------------------------------------
  // Setup API (must be called before WaitAllReady)
  // -----------------------------------------------------------------------

  /**
    Correct the expected worker count after thread creation.

    Must be called after the thread-launch loop and before
    WaitAllReady(), to handle the case where thread creation fails
    partway through and fewer threads were launched than originally
    intended.

    This method acquires the internal mutexes, so it is safe to call
    even if some workers have already called NotifyReadyAndWait() or
    NotifyExitingAndWait(). If all already-launched workers happen to
    be ready by the time this call completes, the waiting condition
    variables are signaled immediately.

    @param actualCount  The actual number of successfully launched
                        worker threads.
  */
  void SetWorkerCount(uint16_t actualCount) {
    {
      std::unique_lock<std::mutex> lock(m_startMutex);
      m_total = actualCount;
      /* Wake the main thread if all launched workers are already ready. */
      if (m_readyCount == m_total) {
        m_readyCv.notify_one();
      }
    }
    {
      std::unique_lock<std::mutex> lock(m_exitMutex);
      /* Wake the main thread if all launched workers are already exiting. */
      if (m_exitingCount == m_total) {
        m_exitingCv.notify_one();
      }
    }
  }

  // -----------------------------------------------------------------------
  // Worker thread API
  // -----------------------------------------------------------------------

  /**
    Signal that this worker has completed initialization, then block
    until the main thread broadcasts the start signal.

    Called by each worker after setup (e.g. thrd creation). The worker
    is suspended here until BroadcastStart() is called. When the last
    worker calls this, the main thread is unblocked from
    WaitAllReady().
  */
  void NotifyReadyAndWait() {
    std::unique_lock<std::mutex> lock(m_startMutex);
    if (++m_readyCount == m_total) {
      m_readyCv.notify_one();
    }
    m_startCv.wait(lock, [this] { return m_startNotified; });
  }

  /**
    Signal that this worker has finished its task, then block until
    the main thread broadcasts the exit signal.

    Called by each worker after it has completed its work. The worker
    is suspended here until BroadcastExit() is called. When the last
    worker calls this, the main thread is unblocked from
    WaitAllExiting(). Cleanup (e.g. thrd destruction) should be
    performed after this call returns.
  */
  void NotifyExitingAndWait() {
    std::unique_lock<std::mutex> lock(m_exitMutex);
    if (++m_exitingCount == m_total) {
      m_exitingCv.notify_one();
    }
    m_exitCv.wait(lock, [this] { return m_exitNotified; });
  }

  // -----------------------------------------------------------------------
  // Main thread API
  // -----------------------------------------------------------------------

  /**
    Block until all worker threads have completed initialization.

    Called by the main thread after SetWorkerCount(). Returns once
    every worker has called NotifyReadyAndWait(). After this returns,
    it is safe to access resources created by workers (e.g. their thrds).
  */
  void WaitAllReady() {
    std::unique_lock<std::mutex> lock(m_startMutex);
    m_readyCv.wait(lock, [this] { return m_readyCount == m_total; });
  }

  /**
    Release all worker threads that are blocked in NotifyReadyAndWait().

    Called by the main thread after WaitAllReady() returns and any
    additional setup is complete. All workers resume simultaneously,
    ensuring a coordinated start.
  */
  void BroadcastStart() {
    std::unique_lock<std::mutex> lock(m_startMutex);
    m_startNotified = true;
    m_startCv.notify_all();
  }

  /**
    Block until all worker threads have finished their tasks.

    Called by the main thread after its own work is done. Returns once
    every worker has called NotifyExitingAndWait(). After this returns,
    it is safe to perform global cleanup before releasing the workers.
  */
  void WaitAllExiting() {
    std::unique_lock<std::mutex> lock(m_exitMutex);
    m_exitingCv.wait(lock, [this] { return m_exitingCount == m_total; });
  }

  /**
    Release all worker threads that are blocked in NotifyExitingAndWait().

    Called by the main thread after WaitAllExiting() returns and any
    global cleanup is complete. All workers resume simultaneously to
    perform their own cleanup (e.g. thrd destruction) and then exit.
  */
  void BroadcastExit() {
    std::unique_lock<std::mutex> lock(m_exitMutex);
    m_exitNotified = true;
    m_exitCv.notify_all();
  }

 private:
  /**
    Expected number of worker threads. Initialized in the constructor
    and may be corrected by SetWorkerCount() after thread creation.
  */
  uint16_t m_total;

  // -----------------------------------------------------------------------
  // Start barrier state
  // -----------------------------------------------------------------------

  /** Protects m_total (start side), m_readyCount and m_startNotified. */
  std::mutex m_startMutex;

  /** Signaled when m_readyCount reaches m_total. */
  std::condition_variable m_readyCv;

  /** Signaled by the main thread to release all waiting workers. */
  std::condition_variable m_startCv;

  /** Number of workers that have completed initialization. */
  uint16_t m_readyCount;

  /** Set to true when the main thread has called BroadcastStart(). */
  bool m_startNotified;

  // -----------------------------------------------------------------------
  // Exit barrier state
  // -----------------------------------------------------------------------

  /** Protects m_total (exit side), m_exitingCount and m_exitNotified. */
  std::mutex m_exitMutex;

  /** Signaled when m_exitingCount reaches m_total. */
  std::condition_variable m_exitingCv;

  /** Signaled by the main thread to release all waiting workers. */
  std::condition_variable m_exitCv;

  /** Number of workers that have finished their tasks. */
  uint16_t m_exitingCount;

  /** Set to true when the main thread has called BroadcastExit(). */
  bool m_exitNotified;
};

class ParallelReaderInterface {
 public:
  using Callback = std::function<int(DSTORE::HeapTuple *,
                                     const uint16_t threadIdx, void *ctx)>;
  static constexpr size_t MAX_THREADS = 256;

  virtual ~ParallelReaderInterface() = default;
  virtual int Init() = 0;
  virtual int Run() = 0;

 protected:
  static size_t AcquireThreads(size_t requireThreads);
  static void ReleaseThreads(size_t releaseThreads);

 private:
  static std::atomic<size_t> m_activeThreads;
};

class alignas(CDE_CACHE_LINE_SIZE) ParallelHeapScanReaderBase
    : public ParallelReaderInterface {
 public:
  ParallelHeapScanReaderBase(StorageRelation heapRel, uint16_t threadNum,
                             Callback &&callback, void *callbackCtx)
      : m_heapRel(heapRel),
        m_callback(callback),
        m_callbackCtx(callbackCtx),
        m_threadNum(threadNum) {}
  ~ParallelHeapScanReaderBase() {
    ParallelInterface::DestroyParallelWorkController(m_controller);
  }
  struct ParallelHeapScanTaskInfo {
   public:
    const THD *m_thd;
    Callback *m_cb;
    void *m_ctx;
    /* ThreadContext for this worker thread, used by the main thread to call
    SetInterruptPending() on all workers upon receiving a kill signal.
    Stored here rather than in ParallelHeapScanReaderBase to avoid bloating
    that struct beyond a single cache line. */
    ThreadContext *m_thrd;
  };
  int Init() override;

  ParallelHeapScanTaskInfo *CreateParallelHeapScanTaskInfo();

  int Run() override;

  bool HasFatalError() const { return m_fatalError; }

  void MarkAsFatal() { m_fatalError = true; }

 private:
  void WorkerLoop(uint16_t threadIdx, ParallelHeapScanTaskInfo *task,
                  ParallelWorkerBarrier &barrier);

  DSTORE::Transaction *m_mainTxn{nullptr};
  DSTORE::ParallelWorkController *m_controller{nullptr};

  StorageRelation m_heapRel;
  Callback m_callback;
  void *m_callbackCtx;
  int m_callBackResult{CDE_OK};
  uint16_t m_threadNum;
  bool m_error{false};
  // Fatal error occurred (such as OOM). Do not retry handler::records.
  bool m_fatalError{false};
};

using ParallelHeapScanReader = CacheLinePadded<ParallelHeapScanReaderBase>;

template <typename T, size_t CacheLineSize = CDE_CACHE_LINE_SIZE>
inline constexpr bool verifyCacheLinePadding =
    sizeof(CacheLinePadded<T, CacheLineSize>) % CacheLineSize == 0;
static_assert(verifyCacheLinePadding<ParallelHeapScanReaderBase>);
} /* namespace CDE */
#endif /* HA_CDE_PARALLEL_H */