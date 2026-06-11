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

#ifndef EXCHANGE_PQ_INCLUDED
#define EXCHANGE_PQ_INCLUDED

/**
  @file sql/parallel_query/exchange.h
  Parallel Query V1-MVP: Leader-side record collection from workers.

  Phase 3 scope:
  - Define Exchange base class and Exchange_nosort subclass.
  - Provide minimal init/cleanup/read_mq_record interface.
  - Exchange_nosort uses round-robin polling of worker MQueue_handles.
  - No TABLE/Field/Item dependency in MVP headers (deferred to Phase 5).
  - No execution path hookup.

  Design notes:
  - Exchange creates N MQueue_handles (one per worker) and one shared
    PQ_mq_event for the receiver side.
  - Exchange_nosort::read_mq_record() reads from workers in round-robin,
    returns ROW/FINISH/ERROR via MQMessageType detection.
  - Exchange_sort (ORDER BY Gather Merge) is deferred to V1-complete.

  Memory ownership (MVP):
  - All MQ resources (ring buffers, events, handles) are allocated in
    Exchange::init() using new[]; Phase 4 will switch to pq_mem_root.
  - Exchange::cleanup() destroys all MQ resources.
  - Data returned by read_mq_record() is owned by the MQ handle's local
    buffer; caller must copy into record[0] before the next call.

  Future integration risks:
  - convert_mq_data_to_record() needs TABLE/Field (Phase 5 integration).
  - Exchange_nosort currently uses simplified round-robin; Phase 6 will
    integrate THD kill-check and spin-wait from Phase 0 variables.
  - Exchange_sort needs comparison function and heap merge (V1-complete).
*/

#include "sql/parallel_query/msg_queue.h"

#include <cassert>
#include <cstdint>

struct TABLE;

/**
  Typed MQ message header for PQ row-image protocol.

  The outer MQueue message still uses [len:4B][data:lenB]. The inner payload
  starts with this header so Exchange can distinguish ROW/FINISH/ERROR without
  guessing from data length.
*/
struct PQ_mq_message_header {
  uint32 magic;
  uint16 version;
  uint16 type;
  uint32 payload_len;
  uint32 flags;
};

constexpr uint32 PQ_MQ_MESSAGE_MAGIC = 0x5051524d;  // "PQRM"
constexpr uint16 PQ_MQ_MESSAGE_VERSION = 1;

/**
  Base class for PQ leader-side record collection.

  Exchange manages the MQueue_handle array for all workers and provides
  the interface for reading rows from the MQ into the leader's execution.

  MVP: Exchange::init() creates all MQ infrastructure; Exchange::cleanup()
  destroys it. No TABLE/Field/Item dependency.
*/
class Exchange {
 protected:
  uint32 m_nqueues;               ///< Number of worker MQ queues (= DOP)
  MQueue_handle **m_mq_handles;   ///< Array of MQ handles (one per worker)
  PQ_mq_event *m_receiver_event;  ///< Shared receiver event (leader side)
  uint32 m_ring_size;             ///< Ring buffer size for each MQ

 public:
  Exchange()
      : m_nqueues(0),
        m_mq_handles(nullptr),
        m_receiver_event(nullptr),
        m_ring_size(PQ_MQ_DEFAULT_RING_SIZE) {}

  Exchange(uint32 nqueues, uint32 ring_size)
      : m_nqueues(nqueues),
        m_mq_handles(nullptr),
        m_receiver_event(nullptr),
        m_ring_size(ring_size) {}

  virtual ~Exchange() = default;

  /**
    Initialize MQ infrastructure: allocate ring buffers, events, and handles.

    @retval false  Success
    @retval true   Failure (allocation error)
  */
  virtual bool init();

  /**
    Clean up all MQ resources.

    Destroys ring buffers, events, handles, and the handle array.
  */
  virtual void cleanup();

  /**
    Read one message from the MQ system.

    Returns the message type and data. In Phase 3, this does not
    convert to record[0] format (that is Phase 5's job).

    @param[out] type      Message type (ROW, FINISH, ERROR)
    @param[out] datap     Pointer to message data (may be nullptr for FINISH)
    @param[out] data_len  Length of message data
    @retval true   Message available (check type for content)
    @retval false  All workers finished or error
  */
  virtual bool read_mq_message(MQMessageType &type, void **datap,
                                uint32 &data_len) = 0;

  /**
    Get the MQ handle for a specific worker.

    @param worker_id  Worker index (0 .. m_nqueues-1)
    @return           MQueue_handle pointer
  */
  MQueue_handle *get_mq_handle(uint32 worker_id) const {
    assert(m_mq_handles != nullptr);
    assert(worker_id < m_nqueues);
    return m_mq_handles[worker_id];
  }

  /** Number of worker queues. */
  uint32 nqueues() const { return m_nqueues; }

  /** Exchange type discriminator. */
  enum ExchangeType {
    EXCHANGE_NOSORT = 0,
    EXCHANGE_SORT,
  };

  virtual ExchangeType get_exchange_type() const = 0;
};

/**
  Exchange_nosort: round-robin collection from worker MQs.

  Leader reads from each worker's MQ in round-robin order. When a worker
  sends FINISH, that queue is marked as read-done and skipped. When all
  workers have sent FINISH, the exchange returns false (no more records).

  MVP: no THD kill-check, no spin-wait, no stable output (ref_length).
  These are deferred to Phase 5+ integration.
*/
class Exchange_nosort : public Exchange {
 public:
  enum class Materialize_status {
    ROW,
    EOF_REACHED,
    WOULD_BLOCK,
    ERROR
  };

 private:
  uint32 m_active_readers;  ///< Workers still producing data
  uint32 m_next_queue;     ///< Next queue to read from (round-robin index)
  bool m_all_done;         ///< All workers have finished

  /**
    Read one message from a specific worker's MQ.

    @param worker_id  Which worker to read from
    @param[out] type  Message type
    @param[out] datap Data pointer
    @param[out] len   Data length
    @param[out] done  Worker has finished (FINISH/DETACHED)
    @return           true if data available, false otherwise
  */
  bool get_next_from_worker(uint32 worker_id, MQMessageType &type,
                            void **datap, uint32 &len, bool &done);

  /**
    Read one message from the next worker in round-robin order.

    @param[out] type  Message type
    @param[out] datap Data pointer
    @param[out] len   Data length
    @return           true if data available, false if all workers done
  */
  bool read_next_round_robin(MQMessageType &type, void **datap, uint32 &len);

 public:
  Exchange_nosort()
      : Exchange(),
        m_active_readers(0),
        m_next_queue(0),
        m_all_done(false) {}

  Exchange_nosort(uint32 nqueues, uint32 ring_size)
      : Exchange(nqueues, ring_size),
        m_active_readers(nqueues),
        m_next_queue(0),
        m_all_done(false) {}

  ~Exchange_nosort() override = default;

  bool init() override;
  void cleanup() override;

  /**
    Read one message from MQ in round-robin manner.

    Checks for ROW, FINISH, and ERROR tokens. When a worker sends FINISH,
    decrements m_active_readers. When all workers finish, returns false.

    ERROR tokens from a worker cause immediate return with type=ERROR.

    @param[out] type      Message type
    @param[out] datap     Data pointer (nullptr for FINISH)
    @param[out] data_len  Data length (0 for FINISH)
    @retval true   Message available
    @retval false  All workers finished (or error path needs abort)
  */
  bool read_mq_message(MQMessageType &type, void **datap,
                        uint32 &data_len) override;

  /**
    Read and materialize one typed row-image message.

    This helper is the future `PQTableScanIterator::Read()` building block. It
    consumes messages until it either materializes one ROW payload into
    table->record[0], observes EOF, or sees an error/no-data condition. It does
    not wait on worker events; production `Read()` must add kill-check and wait
    policy around it before real row stream activation.

    @param table       TABLE whose record[0] receives the row image
    @param[out] eof    True when all worker queues finished
    @param[out] row    True when a row was materialized

    @retval false  Row materialized, EOF observed, or no data currently ready
    @retval true   ERROR token or malformed row image
  */
  bool materialize_next_record_image(TABLE *table, bool *eof, bool *row);

  /**
    Read and materialize one typed row-image message with precise stream status.

    This is the V2-8J wait-policy boundary. It remains non-blocking, but lets
    future `Read()` code distinguish a temporary no-message condition from EOF.

    @param table        TABLE whose record[0] receives the row image
    @param[out] status  ROW, EOF_REACHED, WOULD_BLOCK, or ERROR

    @retval false  Status is valid
    @retval true   Invalid input or malformed row image
  */
  bool materialize_next_record_image_status(TABLE *table,
                                            Materialize_status *status);

  /**
    Wait briefly for producer activity.

    This is the bounded wait primitive for `PQTableScanIterator::Read()`: the
    caller must perform THD kill checks before and after waiting.

    @param timeout_us  Maximum wait time in microseconds; 0 means unbounded
  */
  void wait_for_message(uint64 timeout_us);

  /**
    Enqueue one fixed-size record image as a typed ROW message.

    This helper is for controlled producer paths. It does not send FINISH, so
    callers can enqueue multiple ROW messages before closing the stream.

    @retval false  ROW enqueued
    @retval true   Invalid input or MQ send failure
  */
  bool enqueue_record_image(uint32 worker_id, TABLE *source_table);

  /**
    Enqueue one fixed-size record image for a smoke producer.

    This is a controlled V2-8J helper: it copies the current
    source_table->record[0] into a typed ROW message for worker_id, followed by
    a typed FINISH token. It is not a general worker producer API.

    @retval false  ROW and FINISH enqueued
    @retval true   Invalid input or MQ send failure
  */
  bool enqueue_record_image_smoke(uint32 worker_id, TABLE *source_table);

  /**
    Enqueue a typed FINISH token for a smoke producer.

    @retval false  FINISH enqueued
    @retval true   Invalid worker id or MQ send failure
  */
  bool enqueue_finish_smoke(uint32 worker_id);

  /**
    Enqueue a typed ERROR token for a smoke producer.

    @retval false  ERROR enqueued
    @retval true   Invalid worker id or MQ send failure
  */
  bool enqueue_error_smoke(uint32 worker_id);

  /**
    Run a controlled synthetic row stream through this exchange.

    The helper pre-fills each worker queue with one ROW payload followed by a
    FINISH token, then consumes the messages through read_mq_message(). It is
    only a V2-6 transport smoke: it does not materialize rows into TABLE
    records and does not imply real PQ execution.

    @param[out] rows_read      Number of synthetic ROW payloads read
    @param[out] finishes_read  Number of FINISH tokens observed

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_synthetic_row_stream_smoke(uint32 *rows_read,
                                      uint32 *finishes_read);

  /**
    Run a controlled synthetic row-image materialization smoke.

    The helper pre-fills each worker queue with one typed ROW message carrying
    a fixed-size MySQL record image, then a typed FINISH message. The leader
    consumes the messages and copies ROW payloads into table->record[0].

    This is only a V2-8B protocol smoke. It does not start real workers, does
    not read InnoDB rows, and must not be counted as real PQ execution.

    @param table              Leader TABLE whose record[0] receives payloads
    @param[out] rows_read     Number of typed ROW payloads materialized
    @param[out] finishes_read Number of FINISH tokens observed

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_synthetic_row_image_smoke(TABLE *table, uint32 *rows_read,
                                     uint32 *finishes_read);

  /**
    Run a controlled synthetic partial GROUP BY message smoke.

    The helper sends one typed PARTIAL_GROUP payload and one FINISH per worker,
    then verifies the leader can decode the PARTIAL_GROUP messages through the
    normal Exchange_nosort round-robin path. It does not merge aggregates and
    does not connect to SQL execution.

    @param[out] groups_read   Number of PARTIAL_GROUP payloads decoded
    @param[out] finishes_read Number of FINISH tokens observed

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_synthetic_partial_group_smoke(uint32 *groups_read,
                                         uint32 *finishes_read);

  ExchangeType get_exchange_type() const override { return EXCHANGE_NOSORT; }
};

#endif  // EXCHANGE_PQ_INCLUDED
