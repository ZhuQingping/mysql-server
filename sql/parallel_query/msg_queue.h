/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in license.xml
   elsewhere in this distribution.  You may use this software under
   the terms of the GNU General Public License, version 2.0,
   or the terms of any other license that is available in this distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef MSG_QUEUE_PQ_INCLUDED
#define MSG_QUEUE_PQ_INCLUDED

/**
  @file sql/parallel_query/msg_queue.h
  Parallel Query V1-MVP: Lock-free ring-buffer message queue skeleton.

  Phase 3 scope:
  - Define MQMessageType enum (ROW, FINISH, ERROR, ABORT).
  - Define MQueue (ring buffer) and MQueue_handle (send/receive handle).
  - Provide minimal compilable send/receive/close/abort interfaces.
  - No Batch_buffer, no Field_raw_data, no THD dependency in core types.
  - No connection to execution path, iterator, or handler.

  Design notes:
  - Each worker has a dedicated MQueue (one-producer-per-queue).
  - Leader reads from all workers' MQueue handles via Exchange_nosort.
  - The ring buffer uses power-of-2 size for fast modulo (MOD macro).
  - Atomic read/write positions with memory barriers for lock-free operation.
  - send() blocks when MQ is full; receive() is non-blocking by default.
  - MQ_DETACHED signals that the producer has disconnected.

  Memory ownership (MVP):
  - MQueue::m_buffer is allocated from pq_mem_root (or caller-provided).
  - MQueue_handle::m_buffer is a local read buffer, also from pq_mem_root.
  - The data pointer returned by receive() points into m_buffer; caller must
    copy before next receive().
  - No deep copy of row data in MQueue itself; that is Exchange's job.

  Future integration risks:
  - Batch_buffer_manager (spill to disk) is deferred to V1-complete.
  - Field_raw_data serialization (NULL/compress var-len) is deferred.
  - THD-based spin count and condition variable wait depend on Phase 0 variables.
  - pq_mem_root allocation will replace new/delete in init paths (Phase 4).
*/

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "my_dbug.h"
#include "my_inttypes.h"

/**
  Message type sent through the MQ.

  MVP row transfer uses a simple byte buffer: [len:4B][data:lenB].
  FINISH: worker signals normal completion.
  ERROR:  worker signals a fatal error (payload = error code).
  ABORT:  leader signals workers to stop; worker reads this as MQ_DETACHED.
*/
enum class MQMessageType {
  ROW,     ///< Row data (byte buffer, format deferred to Exchange)
  FINISH,  ///< Worker finished normally
  ERROR,   ///< Worker encountered an error
  ABORT,   ///< Leader aborting; close producer side
  PARTIAL_GROUP  ///< GROUP BY partial aggregate payload
};

/**
  Result of MQ send/receive operations.
*/
enum MQ_RESULT {
  MQ_SUCCESS,      ///< Sent or received a message
  MQ_WOULD_BLOCK,  ///< Not completed; retry later
  MQ_DETACHED      ///< Producer has detached / queue closed
};

/**
  Detached status of the MQ.
*/
enum MQ_DETACHED_STATUS {
  MQ_NOT_DETACHED = 0,  ///< Queue is active
  MQ_HAVE_DETACHED,     ///< Producer has permanently detached
  MQ_TMP_DETACHED       ///< Temporarily detached (MVP: not used, placeholder)
};

/**
  Quick modulo for power-of-2 ring sizes.
  Replaces expensive % operation with & when size is power of 2.
*/
#define PQ_MQ_MOD(x, y) ((x) & ((y) - 1))

/**
  Memory barrier primitives for lock-free ring buffer.

  These use C++11 atomic fences for portability, matching the reference
  implementation's intent but avoiding GCC-specific __sync primitives.
*/
inline void pq_write_barrier() {
  std::atomic_thread_fence(std::memory_order_release);
}
inline void pq_read_barrier() {
  std::atomic_thread_fence(std::memory_order_acquire);
}
inline void pq_compiler_barrier() {
  std::atomic_signal_fence(std::memory_order_seq_cst);
}
inline void pq_memory_barrier() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

/**
  Atomic helpers for 64-bit ring buffer positions.

  Uses C++11 std::atomic instead of GCC __sync builtins for portability.
  MVP: no THD dependency, no DBUG instrumentation beyond assert.
*/
inline uint64 pq_atomic_read_u64(std::atomic<uint64> &var) {
  return var.load(std::memory_order_acquire);
}

inline void pq_atomic_write_u64(std::atomic<uint64> &var, uint64 val) {
  var.store(val, std::memory_order_release);
}

inline void pq_atomic_inc_u64(std::atomic<uint64> &var, uint64 inc) {
  var.fetch_add(inc, std::memory_order_release);
}

/**
  Event for blocking send/receive.

  MVP: simplified from reference implementation. Uses C++11 mutex/condvar.
  Full THD-aware spin-wait is deferred to Phase 4 (worker lifecycle).
  Phase 3 provides the interface but does not activate blocking waits
  from execution paths.
*/
class PQ_mq_event {
 public:
  std::atomic<bool> latch{false};
  std::mutex m_mutex;
  std::condition_variable m_cond;

  PQ_mq_event() = default;
  ~PQ_mq_event() = default;

  /** Signal that data is available or space is freed. */
  void set_latch() {
    latch.store(true, std::memory_order_release);
    m_cond.notify_one();
  }

  /**
    Wait for partner to set latch, with optional timeout.

    MVP: simple blocking wait. THD kill-check and spin-wait are deferred
    to Phase 4 integration. timeout_us = 0 means no timeout (blocking).
  */
  void wait_latch(uint64 timeout_us = 0) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (timeout_us > 0) {
      m_cond.wait_for(lock, std::chrono::microseconds(timeout_us),
                      [this] { return latch.load(std::memory_order_acquire); });
    } else {
      m_cond.wait(lock,
                  [this] { return latch.load(std::memory_order_acquire); });
    }
  }

  /** Reset latch for next round. */
  void reset_latch() { latch.store(false, std::memory_order_release); }
};

/**
  Lock-free ring-buffer message queue.

  One MQueue per worker. The ring buffer is a contiguous byte array
  with atomic read/write positions. Messages are [len:4B][data:lenB].

  MVP: no Batch_buffer spill, no THD-aware event, no partial result
  compression. The ring buffer must be power-of-2 sized for PQ_MQ_MOD.
*/
class MQueue {
 public:
  PQ_mq_event *m_sender_event;    ///< Producer (worker) synchronization
  PQ_mq_event *m_receiver_event;  ///< Consumer (leader) synchronization
  std::atomic<uint64> m_bytes_written{0};  ///< Write position (atomic)
  std::atomic<uint64> m_bytes_read{0};     ///< Read position (atomic)
  char *m_buffer;                  ///< Ring buffer array
  uint32 m_ring_size;              ///< Ring buffer size (power of 2)
  MQ_DETACHED_STATUS detached;     ///< Queue detach status

  MQueue()
      : m_sender_event(nullptr),
        m_receiver_event(nullptr),
        m_buffer(nullptr),
        m_ring_size(0),
        detached(MQ_NOT_DETACHED) {}

  MQueue(PQ_mq_event *sender_event, PQ_mq_event *receiver_event,
         char *ring, uint32 ring_size)
      : m_sender_event(sender_event),
        m_receiver_event(receiver_event),
        m_buffer(ring),
        m_ring_size(ring_size),
        detached(MQ_NOT_DETACHED) {}

  ~MQueue() = default;
};

/**
  Handle for sending/receiving messages from an MQueue.

  Each MQueue_handle wraps one MQueue. Workers use send(); leader uses
  receive(). The handle maintains local state for partial reads.

  MVP: send/receive operate on raw byte buffers (void*, uint32 len).
  Field_raw_data serialization is deferred to Exchange integration (Phase 5+).
  Batch_buffer spill-to-disk is deferred to V1-complete.
*/
class MQueue_handle {
 private:
  MQueue *m_queue;              ///< Underlying ring queue
  char *m_buffer;               ///< Local buffer for partial reads
  uint32 m_buffer_len;          ///< Local buffer length
  uint32 m_consume_pending;     ///< Batched read-position update
  uint32 m_partial_bytes;       ///< Partial bytes read in current message
  uint32 m_expected_bytes;      ///< Expected message body length
  bool m_length_word_complete;  ///< Length word fully read?
  bool m_read_done;             ///< All data from this queue consumed?

 public:
  MQueue_handle()
      : m_queue(nullptr),
        m_buffer(nullptr),
        m_buffer_len(0),
        m_consume_pending(0),
        m_partial_bytes(0),
        m_expected_bytes(0),
        m_length_word_complete(false),
        m_read_done(false) {}

  MQueue_handle(MQueue *queue, uint32 buffer_len)
      : m_queue(queue),
        m_buffer(nullptr),
        m_buffer_len(buffer_len),
        m_consume_pending(0),
        m_partial_bytes(0),
        m_expected_bytes(0),
        m_length_word_complete(false),
        m_read_done(false) {}

  ~MQueue_handle() = default;

  /**
    Initialize the handle: allocate local buffer.

    MVP: uses simple new[] for local buffer. Phase 4 will switch to
    pq_mem_root allocation.

    @retval false  Success
    @retval true   Failure (buffer_len == 0 or allocation failed)
  */
  bool init();

  /** Clean up allocated resources. */
  void cleanup();

  // --- Send interface (producer / worker side) ---

  /**
    Send a raw byte message to the MQ.

    Format: [len:4B][data:lenB]. Blocking by default.

    @param data   Pointer to message data
    @param len    Length of message data
    @param nowait If true, return MQ_WOULD_BLOCK when MQ is full
    @return       MQ_SUCCESS, MQ_WOULD_BLOCK, or MQ_DETACHED
  */
  MQ_RESULT send(const void *data, uint32 len, bool nowait = false);

  /**
    Send a control token (FINISH/ERROR/ABORT) to the MQ.

    Encodes MQMessageType as a single-byte payload with the length word.

    @param type  The control token type
    @return      MQ_SUCCESS or MQ_DETACHED
  */
  MQ_RESULT send_control_token(MQMessageType type);

  // --- Receive interface (consumer / leader side) ---

  /**
    Receive a message from the MQ.

    Two-phase: first read 4B length word, then read message body.
    Non-blocking by default: returns MQ_WOULD_BLOCK if no data available.

    @param[out] datap   Pointer to received data (points into m_buffer)
    @param[out] nbytesp Length of received data
    @param nowait       If true, return MQ_WOULD_BLOCK when MQ is empty
    @return             MQ_SUCCESS, MQ_WOULD_BLOCK, or MQ_DETACHED
  */
  MQ_RESULT receive(void **datap, uint32 *nbytesp, bool nowait = true);

  // --- Status interface ---

  /** Check if all data has been consumed from this queue. */
  bool has_readdone() const { return m_read_done; }

  /** Mark this queue as fully consumed. */
  void set_readdone() { m_read_done = true; }

  /** Access the underlying MQueue. */
  MQueue *get_mqueue() const { return m_queue; }

  /** Access the receiver event for leader wait. */
  PQ_mq_event *get_receiver() const {
    return m_queue ? m_queue->m_receiver_event : nullptr;
  }

  /** Access the sender event for producer wait. */
  PQ_mq_event *get_sender() const {
    return m_queue ? m_queue->m_sender_event : nullptr;
  }

  /** Set detach status on the underlying queue. */
  void set_detached_status(MQ_DETACHED_STATUS status) {
    if (m_queue) m_queue->detached = status;
  }

  /** Check if the queue is permanently detached. */
  bool is_detached() const {
    assert(m_queue);
    return m_queue->detached == MQ_HAVE_DETACHED;
  }

  /**
    Close the producer side of this MQ.

    Sets detached status, notifies receiver that no more data will arrive.
    Used by worker on normal finish or error exit.
  */
  void close_producer();

  /**
    Abort the MQ from the consumer (leader) side.

    Sets detached status, notifies sender that consumer is gone.
    Used by leader when aborting query (kill, error, OOM).
  */
  void abort_consumer();

 private:
  /** Flush batched consume-pending to atomic read position. */
  void flush_consume_pending();

  /**
    Send raw bytes into the ring buffer.

    @param nbytes  Number of bytes to send
    @param data    Pointer to data
    @param written[out] Bytes actually written
    @param nowait  If true, return MQ_WOULD_BLOCK when no space
    @return        MQ_RESULT
  */
  MQ_RESULT send_bytes(uint32 nbytes, const void *data, uint32 *written,
                       bool nowait = false);

  /**
    Receive raw bytes from the ring buffer.

    @param bytes_needed  Number of bytes needed
    @param nbytesp[out]  Bytes actually received
    @param datap[out]    Pointer to received data
    @param nowait        If true, return MQ_WOULD_BLOCK when no data
    @return              MQ_RESULT
  */
  MQ_RESULT receive_bytes(uint32 bytes_needed, uint32 *nbytesp, void *datap,
                          bool nowait = true);
};

/**
  Compute the largest power of 2 not exceeding value.

  Used to round MQ ring size to a power of 2 for PQ_MQ_MOD optimization.

  @param value  Input value
  @return       Largest power of 2 <= value
*/
uint64 pq_lower_exponent(uint64 value);

/** Length of the length word preceding each MQ message. */
constexpr uint32 PQ_MQ_WORD_LENGTH = sizeof(uint32);

/** Default local buffer size for MQueue_handle. */
constexpr uint32 PQ_MQ_DEFAULT_BUFFER_SIZE = 1024;

/** Default ring buffer size for MQueue (must be power of 2). */
constexpr uint32 PQ_MQ_DEFAULT_RING_SIZE = 16384;

#endif  // MSG_QUEUE_PQ_INCLUDED
