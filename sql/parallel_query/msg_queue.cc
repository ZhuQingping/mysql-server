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

/**
  @file sql/parallel_query/msg_queue.cc
  Parallel Query V1-MVP: Lock-free ring-buffer message queue implementation.

  Phase 3 scope:
  - Implement MQueue_handle::init/cleanup/send/receive/close/abort.
  - Implement send_bytes/receive_bytes with lock-free ring buffer.
  - Implement send_control_token for FINISH/ERROR/ABORT tokens.
  - No THD dependency, no worker lifecycle, no execution path hookup.
  - No Batch_buffer, no Field_raw_data, no spill-to-disk.

  This implementation is based on the reference MQueue design (WL#006)
  but adapted for MySQL 8.0.46:
  - Uses C++11 std::atomic and std::atomic_thread_fence instead of
    GCC __sync builtins.
  - Uses PQ_mq_event (C++11 mutex/condvar) instead of THD-dependent
    MQ_event with spin-wait.
  - Removes Batch_buffer/IO_CACHE dependencies.
  - Removes Field_raw_data send overload (deferred to Exchange).

  Memory ownership:
  - MQueue::m_buffer is caller-allocated (Exchange::init allocates from
    pq_mem_root or new[]).
  - MQueue_handle::m_buffer is allocated in init(), freed in cleanup().
  - Data returned by receive() points into m_buffer; caller must copy
    before the next receive() call.

  Future integration risks:
  - Phase 4 will replace new[]/delete[] with pq_mem_root allocation.
  - Phase 5 will add Field_raw_data serialization in Exchange, not here.
  - V1-complete will add Batch_buffer spill-to-disk for large result sets.
  - The spin-wait optimization (parallel_msg_queue_spin_count) needs THD
    variables from Phase 0; it will be added in Phase 4.
*/

#include "sql/parallel_query/msg_queue.h"

#include <algorithm>
#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// Utility: power-of-2 rounding
// ---------------------------------------------------------------------------

uint64 pq_lower_exponent(uint64 value) {
  uint64 le = 1;
  while (le <= (value >> 1)) {
    le = le << 1;
  }
  return le;
}

// ---------------------------------------------------------------------------
// MQueue_handle: initialization and cleanup
// ---------------------------------------------------------------------------

bool MQueue_handle::init() {
  if (m_buffer_len == 0) return true;  // Error: zero-length buffer

  m_buffer = new char[m_buffer_len];
  if (m_buffer == nullptr) return true;  // Allocation failed

  return false;  // Success
}

void MQueue_handle::cleanup() {
  delete[] m_buffer;
  m_buffer = nullptr;

  // Note: m_queue, its ring buffer, and events are owned by Exchange,
  // not by MQueue_handle. Exchange::cleanup() destroys them.
}

// ---------------------------------------------------------------------------
// MQueue_handle: send_bytes (lock-free ring write)
// ---------------------------------------------------------------------------

MQ_RESULT MQueue_handle::send_bytes(uint32 nbytes, const void *data,
                                    uint32 *written, bool nowait) {
  assert(m_queue != nullptr);
  assert(m_queue->m_buffer != nullptr);

  uint32 ringsize = m_queue->m_ring_size;
  uint32 sent = 0;

  while (sent < nbytes) {
    uint64 rb = pq_atomic_read_u64(m_queue->m_bytes_read);
    uint64 wb = pq_atomic_read_u64(m_queue->m_bytes_written);
    assert(wb >= rb);

    uint32 used = static_cast<uint32>(wb - rb);
    assert(used <= ringsize);

    // Check detach status
    pq_compiler_barrier();
    if (m_queue->detached == MQ_HAVE_DETACHED) {
      *written = sent;
      return MQ_DETACHED;
    }

    if (m_queue->detached == MQ_TMP_DETACHED) {
      *written = sent;
      return MQ_SUCCESS;
    }

    uint32 available = std::min(ringsize - used, nbytes - sent);

    if (available == 0) {
      // Notify receiver that there is data waiting
      if (m_queue->m_receiver_event) {
        m_queue->m_receiver_event->set_latch();
      }

      if (nowait) {
        *written = sent;
        return MQ_WOULD_BLOCK;
      }

      // Blocking wait for space
      if (m_queue->m_sender_event) {
        m_queue->m_sender_event->wait_latch();
        m_queue->m_sender_event->reset_latch();
      }
    } else {
      uint32 offset = PQ_MQ_MOD(wb, ringsize);
      uint32 sent_once = std::min(available, ringsize - offset);

      // Write data into ring buffer.
      // The memory barrier before memcpy ensures prior writes to the ring
      // buffer are visible (relevant for wrap-around writes where we may
      // overwrite data from a previous message in the same slot).
      pq_memory_barrier();
      memcpy(&m_queue->m_buffer[offset],
             reinterpret_cast<const char *>(data) + sent, sent_once);
      sent += sent_once;

      // Release fence: ensure memcpy data is visible before write position
      // update. The atomic store with release ordering would suffice, but
      // pq_atomic_inc_u64 uses fetch_add with release which is a read-modify-
      // write; the fence makes the memcpy visible to other threads before
      // the position update is visible.
      pq_write_barrier();
      pq_atomic_inc_u64(m_queue->m_bytes_written, sent_once);

      // Notify receiver
      if (m_queue->m_receiver_event) {
        m_queue->m_receiver_event->set_latch();
      }
    }
  }

  assert(sent == nbytes);
  *written = sent;
  return MQ_SUCCESS;
}

// ---------------------------------------------------------------------------
// MQueue_handle: receive_bytes (lock-free ring read)
// ---------------------------------------------------------------------------

MQ_RESULT MQueue_handle::receive_bytes(uint32 bytes_needed, uint32 *nbytesp,
                                       void *datap, bool nowait) {
  assert(m_queue != nullptr);
  assert(m_queue->m_buffer != nullptr);

  *nbytesp = 0;
  uint32 ringsize = m_queue->m_ring_size;

  for (;;) {
    // Effective read position includes consume_pending (batch optimization)
    uint64 rb = pq_atomic_read_u64(m_queue->m_bytes_read) + m_consume_pending;
    uint64 wb = pq_atomic_read_u64(m_queue->m_bytes_written);
    assert(wb >= rb);

    uint32 used = static_cast<uint32>(wb - rb);
    assert(used <= ringsize);
    uint32 offset = PQ_MQ_MOD(rb, ringsize);

    if (used >= bytes_needed) {
      // Enough data available: read directly
      if (offset + bytes_needed <= ringsize) {
        // Contiguous read
        memcpy(datap, &m_queue->m_buffer[offset], bytes_needed);
      } else {
        // Wrap-around read: two memcpy calls
        uint32 part_1 = ringsize - offset;
        uint32 part_2 = bytes_needed - part_1;
        memcpy(datap, &m_queue->m_buffer[offset], part_1);
        memcpy(static_cast<char *>(datap) + part_1, &m_queue->m_buffer[0],
               part_2);
      }

      *nbytesp = bytes_needed;
      pq_memory_barrier();

      // Notify sender that space has been freed
      if (m_queue->m_sender_event) {
        m_queue->m_sender_event->set_latch();
      }
      return MQ_SUCCESS;
    }

    if (used > 0) {
      // Partial data available: read what we can
      uint32 can_read = std::min(used, ringsize - offset);
      memcpy(datap, &m_queue->m_buffer[offset], can_read);
      *nbytesp = can_read;

      pq_memory_barrier();

      if (m_queue->m_sender_event) {
        m_queue->m_sender_event->set_latch();
      }
      return MQ_SUCCESS;
    }

    // No data available

    // Check detach: if producer detached and no data left, we are done
    if (m_queue->detached == MQ_HAVE_DETACHED) {
      pq_read_barrier();
      // Recheck write position: producer may have written just before detach
      uint64 wb2 = pq_atomic_read_u64(m_queue->m_bytes_written);
      uint64 rb2 = pq_atomic_read_u64(m_queue->m_bytes_read) + m_consume_pending;
      if (wb2 > rb2) continue;  // Data appeared after detach, read it first
      return MQ_DETACHED;
    }

    // Flush consume_pending to keep sender informed about available space
    flush_consume_pending();

    // Non-blocking: return immediately
    if (nowait) return MQ_WOULD_BLOCK;

    // Blocking: wait for sender to produce data
    if (m_queue->m_receiver_event) {
      m_queue->m_receiver_event->wait_latch();
      m_queue->m_receiver_event->reset_latch();
    }
  }
}

// ---------------------------------------------------------------------------
// MQueue_handle: send (message-level)
// ---------------------------------------------------------------------------

MQ_RESULT MQueue_handle::send(const void *data, uint32 len, bool nowait) {
  MQ_RESULT res;
  uint32 written;

  // (1) Send length word
  uint32 nbytes = len;
  res = send_bytes(PQ_MQ_WORD_LENGTH, reinterpret_cast<const char *>(&nbytes),
                   &written, nowait);
  if (res != MQ_SUCCESS) {
    return res;
  }
  assert(written == PQ_MQ_WORD_LENGTH);

  // (2) Send message body
  res = send_bytes(nbytes, data, &written, nowait);
  if (res != MQ_SUCCESS) {
    return res;
  }
  assert(written == nbytes);

  return MQ_SUCCESS;
}

// ---------------------------------------------------------------------------
// MQueue_handle: send_control_token
// ---------------------------------------------------------------------------

MQ_RESULT MQueue_handle::send_control_token(MQMessageType type) {
  // Encode type as a 1-byte payload with standard length word.
  // Format: [len=1:4B][type_byte:1B]
  uint8 type_byte = static_cast<uint8>(type);
  return send(&type_byte, 1);
}

// ---------------------------------------------------------------------------
// MQueue_handle: receive (message-level)
// ---------------------------------------------------------------------------

MQ_RESULT MQueue_handle::receive(void **datap, uint32 *nbytesp, bool nowait) {
  MQ_RESULT res;
  uint32 rb;

  // Flush consume_pending if it exceeds 1/4 of ring size (batch optimization)
  if (m_consume_pending > m_queue->m_ring_size / 4) {
    flush_consume_pending();
  }

  // (1) Read length word (4 bytes)
  while (!m_length_word_complete) {
    assert(m_partial_bytes < PQ_MQ_WORD_LENGTH);
    res = receive_bytes(PQ_MQ_WORD_LENGTH - m_partial_bytes, &rb,
                        &m_buffer[m_partial_bytes], nowait);
    if (res != MQ_SUCCESS) return res;

    m_partial_bytes += rb;
    m_consume_pending += rb;

    if (m_partial_bytes >= PQ_MQ_WORD_LENGTH) {
      assert(m_partial_bytes == PQ_MQ_WORD_LENGTH);
      m_expected_bytes = *reinterpret_cast<uint32 *>(m_buffer);
      m_length_word_complete = true;
      m_partial_bytes = 0;
    }
  }

  uint32 nbytes = m_expected_bytes;

  // Reallocate local buffer if needed
  if (m_buffer_len < nbytes) {
    while (m_buffer_len < nbytes) {
      m_buffer_len *= 2;
    }
    delete[] m_buffer;
    m_buffer = new char[m_buffer_len];
    if (m_buffer == nullptr) {
      *nbytesp = 0;
      *datap = nullptr;
      return MQ_DETACHED;
    }
  }

  // (2) Read message body
  for (;;) {
    assert(m_partial_bytes <= nbytes);
    uint32 still_needed = nbytes - m_partial_bytes;

    res = receive_bytes(still_needed, &rb, &m_buffer[m_partial_bytes], nowait);
    if (res != MQ_SUCCESS) return res;

    m_partial_bytes += rb;
    m_consume_pending += rb;

    if (m_partial_bytes >= nbytes) break;
  }

  // Reset for next message
  m_length_word_complete = false;
  m_partial_bytes = 0;

  *nbytesp = nbytes;
  *datap = m_buffer;
  return MQ_SUCCESS;
}

// ---------------------------------------------------------------------------
// MQueue_handle: close_producer / abort_consumer
// ---------------------------------------------------------------------------

void MQueue_handle::close_producer() {
  if (!m_queue) return;

  m_queue->detached = MQ_HAVE_DETACHED;

  // Wake up receiver so it can detect detach
  if (m_queue->m_receiver_event) {
    m_queue->m_receiver_event->set_latch();
  }
}

void MQueue_handle::abort_consumer() {
  if (!m_queue) return;

  m_queue->detached = MQ_HAVE_DETACHED;

  // Wake up sender so it can detect detach
  if (m_queue->m_sender_event) {
    m_queue->m_sender_event->set_latch();
  }
}

// ---------------------------------------------------------------------------
// MQueue_handle: flush_consume_pending
// ---------------------------------------------------------------------------

void MQueue_handle::flush_consume_pending() {
  if (m_consume_pending > 0) {
    uint32 offset = m_consume_pending;
    m_consume_pending = 0;

    pq_memory_barrier();
    pq_atomic_inc_u64(m_queue->m_bytes_read, offset);
  }
}