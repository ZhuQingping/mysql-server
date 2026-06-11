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
  @file sql/parallel_query/exchange.cc
  Parallel Query V1-MVP: Exchange_nosort round-robin collection implementation.

  Phase 3 scope:
  - Exchange::init() creates MQueue infrastructure for all workers.
  - Exchange_nosort::read_mq_message() reads from workers in round-robin.
  - Detect ROW, FINISH, and ERROR tokens from MQ messages.
  - No TABLE/Field dependency, no convert_mq_data_to_record, no execution hookup.

  This implementation is based on the reference Exchange_nosort but
  simplified for Phase 3 MVP:
  - Uses simplified MQMessageType token detection instead of checking
    msg_len == 1 for ERROR (as the reference does).
  - No TABLE/Field/Item, no stable output (m_stab_output/m_ref_length).
  - No mem_root_deque<Item*> for recv_items (deferred to Phase 5).
  - All allocations use new[]/delete[]; pq_mem_root switch is Phase 4.

  Memory ownership:
  - Exchange::init() owns all MQ resources.
  - Exchange::cleanup() frees all MQ resources.
  - Data pointers from read_mq_message() point into MQ handle local buffers;
    caller must copy before next call.
*/

#include "sql/parallel_query/exchange.h"

#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// Exchange: init / cleanup
// ---------------------------------------------------------------------------

bool Exchange::init() {
  assert(m_nqueues > 0);
  assert(m_ring_size > 0);

  // Round ring_size to power of 2 for PQ_MQ_MOD optimization
  uint64 actual_ring_size = pq_lower_exponent(m_ring_size);
  if (actual_ring_size < PQ_MQ_DEFAULT_RING_SIZE) {
    actual_ring_size = PQ_MQ_DEFAULT_RING_SIZE;
  }
  m_ring_size = static_cast<uint32>(actual_ring_size);

  // Allocate shared receiver event (leader side)
  m_receiver_event = new PQ_mq_event();
  if (m_receiver_event == nullptr) goto err;

  // Allocate MQ handle array. Initialize all entries to nullptr for safe
  // cleanup on partial allocation failure.
  m_mq_handles = new MQueue_handle *[m_nqueues];
  if (m_mq_handles == nullptr) goto err;
  for (uint32 i = 0; i < m_nqueues; i++) {
    m_mq_handles[i] = nullptr;
  }

  // Create MQueue + ring buffer + sender event per worker
  for (uint32 i = 0; i < m_nqueues; i++) {
    char *ring_buffer = new char[m_ring_size];
    if (ring_buffer == nullptr) goto err;

    PQ_mq_event *sender_event = new PQ_mq_event();
    if (sender_event == nullptr) {
      delete[] ring_buffer;
      goto err;
    }

    MQueue *mqueue = new MQueue(sender_event, m_receiver_event, ring_buffer,
                                m_ring_size);
    if (mqueue == nullptr) {
      delete[] ring_buffer;
      delete sender_event;
      goto err;
    }

    m_mq_handles[i] = new MQueue_handle(mqueue, PQ_MQ_DEFAULT_BUFFER_SIZE);
    if (m_mq_handles[i] == nullptr) {
      // MQueue does not own ring_buffer or sender_event; free them manually
      delete[] ring_buffer;
      delete sender_event;
      delete mqueue;
      goto err;
    }

    if (m_mq_handles[i]->init()) {
      goto err;
    }
  }

  return false;  // Success

err:
  // Cleanup partially allocated resources
  cleanup();
  return true;  // Failure
}

void Exchange::cleanup() {
  if (m_mq_handles != nullptr) {
    for (uint32 i = 0; i < m_nqueues; i++) {
      if (m_mq_handles[i] != nullptr) {
        MQueue *mq = m_mq_handles[i]->get_mqueue();
        if (mq != nullptr) {
          // Free MQ resources (ring buffer and sender event)
          if (mq->m_buffer != nullptr) {
            delete[] mq->m_buffer;
            mq->m_buffer = nullptr;
          }
          if (mq->m_sender_event != nullptr) {
            delete mq->m_sender_event;
            mq->m_sender_event = nullptr;
          }
          delete mq;
        }
        m_mq_handles[i]->cleanup();
        delete m_mq_handles[i];
        m_mq_handles[i] = nullptr;
      }
    }
    delete[] m_mq_handles;
    m_mq_handles = nullptr;
  }

  if (m_receiver_event != nullptr) {
    delete m_receiver_event;
    m_receiver_event = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Exchange_nosort: init / cleanup
// ---------------------------------------------------------------------------

bool Exchange_nosort::init() {
  if (Exchange::init()) return true;

  m_active_readers = m_nqueues;
  m_next_queue = 0;
  m_all_done = false;

  return false;  // Success
}

void Exchange_nosort::cleanup() {
  Exchange::cleanup();
  m_active_readers = 0;
  m_next_queue = 0;
  m_all_done = false;
}

// ---------------------------------------------------------------------------
// Exchange_nosort: get_next_from_worker
// ---------------------------------------------------------------------------

bool Exchange_nosort::get_next_from_worker(uint32 worker_id,
                                           MQMessageType &type,
                                           void **datap, uint32 &len,
                                           bool &done) {
  done = false;
  MQueue_handle *handle = get_mq_handle(worker_id);

  if (handle->has_readdone()) {
    done = false;
    type = MQMessageType::FINISH;
    *datap = nullptr;
    len = 0;
    return false;
  }

  void *raw_data = nullptr;
  uint32 raw_len = 0;
  MQ_RESULT result = handle->receive(&raw_data, &raw_len);

  if (result == MQ_DETACHED) {
    done = true;
    handle->set_readdone();
    type = MQMessageType::FINISH;
    *datap = nullptr;
    len = 0;
    return false;
  }

  if (result == MQ_WOULD_BLOCK) {
    type = MQMessageType::ROW;  // Placeholder: no data yet
    *datap = nullptr;
    len = 0;
    return false;
  }

  // MQ_SUCCESS: we have data. Determine if it's a control token or a row.
  // MVP convention: control tokens are sent as send_control_token() which
  // produces a 1-byte payload [len=1:4B][type_byte:1B]. Rows are longer.
  assert(raw_data != nullptr);

  if (raw_len == 1) {
    // Control token: decode type from the byte
    uint8 type_byte = *reinterpret_cast<uint8 *>(raw_data);
    type = static_cast<MQMessageType>(type_byte);
    *datap = nullptr;
    len = 0;

    if (type == MQMessageType::FINISH) {
      done = true;
      handle->set_readdone();
      return false;
    }

    if (type == MQMessageType::ERROR) {
      return true;  // Error needs to be propagated to leader
    }

    // ABORT from leader side: shouldn't be received by leader, but handle it
    return false;
  }

  // Regular row data
  type = MQMessageType::ROW;
  *datap = raw_data;
  len = raw_len;
  return true;
}

// ---------------------------------------------------------------------------
// Exchange_nosort: read_next_round_robin
// ---------------------------------------------------------------------------

bool Exchange_nosort::read_next_round_robin(MQMessageType &type,
                                            void **datap, uint32 &len) {
  uint32 nvisited = 0;

  while (!m_all_done && nvisited < m_nqueues) {
    bool done = false;
    bool got_data =
        get_next_from_worker(m_next_queue, type, datap, len, done);

    if (done) {
      // This worker finished
      m_active_readers--;
      if (m_active_readers == 0) {
        m_all_done = true;
        return false;
      }
      // Move to next worker
      m_next_queue++;
      if (m_next_queue >= m_nqueues) {
        m_next_queue = 0;
      }
      continue;
    }

    if (got_data) {
      return true;  // Row or ERROR available
    }

    // No data from this worker (MQ_WOULD_BLOCK), try next
    m_next_queue++;
    if (m_next_queue >= m_nqueues) {
      m_next_queue = 0;
    }
    nvisited++;
  }

  // Round complete: no data available from any active worker.
  // In production, leader would wait on receiver_event; MVP returns false.
  return false;
}

// ---------------------------------------------------------------------------
// Exchange_nosort: read_mq_message
// ---------------------------------------------------------------------------

bool Exchange_nosort::read_mq_message(MQMessageType &type, void **datap,
                                       uint32 &data_len) {
  type = MQMessageType::ROW;
  *datap = nullptr;
  data_len = 0;

  if (m_all_done) {
    return false;
  }

  bool result = read_next_round_robin(type, datap, data_len);

  // If no data found and not all done, try blocking wait on receiver event.
  // MVP: we don't block because there are no real workers yet.
  // Phase 6 integration: add receiver_event wait with THD kill-check.
  if (!result && !m_all_done) {
    // For now, just return false. When real workers exist, we will
    // wait on m_receiver_event and retry.
    return false;
  }

  return result;
}

bool Exchange_nosort::run_synthetic_row_stream_smoke(uint32 *rows_read,
                                                     uint32 *finishes_read) {
  if (rows_read != nullptr) *rows_read = 0;
  if (finishes_read != nullptr) *finishes_read = 0;
  if (m_mq_handles == nullptr || m_nqueues == 0) return true;

  for (uint32 i = 0; i < m_nqueues; ++i) {
    const uint32 payload[2] = {0x50514558U, i};
    MQueue_handle *handle = get_mq_handle(i);
    if (handle->send(payload, sizeof(payload)) != MQ_SUCCESS) return true;
    if (handle->send_control_token(MQMessageType::FINISH) != MQ_SUCCESS) {
      return true;
    }
  }

  uint32 local_rows = 0;
  while (!m_all_done) {
    MQMessageType type;
    void *datap = nullptr;
    uint32 data_len = 0;
    bool got_message = read_mq_message(type, &datap, data_len);

    if (got_message) {
      if (type != MQMessageType::ROW || datap == nullptr ||
          data_len != sizeof(uint32) * 2) {
        return true;
      }
      const uint32 *payload = static_cast<const uint32 *>(datap);
      if (payload[0] != 0x50514558U) return true;
      ++local_rows;
      continue;
    }

    if (!m_all_done) {
      return true;
    }
  }

  if (local_rows != m_nqueues) return true;
  if (rows_read != nullptr) *rows_read = local_rows;
  if (finishes_read != nullptr) *finishes_read = m_nqueues;
  return false;
}
