# WL#006: PQ Message Queue

## Summary

Implement a high-throughput, lock-free message queue for transferring partial
query results from worker threads to the leader thread in parallel query
execution.

## Motivation

The message queue is the critical data path between workers and leader. It
must handle high message throughput with minimal synchronization overhead.
A naive locking approach would become a bottleneck under heavy parallel load.

## Specification

### Architecture

Each worker has a dedicated message queue (MQ). The leader reads from all
workers' MQs. This one-producer-per-queue design eliminates inter-worker
contention.

```
Worker1 -> MQ1 \
Worker2 -> MQ2  -> Leader (consumer of all MQs)
  ...
WorkerN -> MQN /
```

### Message Format

```
message = (len: 4 bytes, data: len bytes)
```

### Data Structures

**MQueue** - ring array storage:

```c++
struct MQueue {
  uint64 m_bytes_read;          // read position (atomically updated)
  uint64 m_bytes_write;         // write position (atomically updated)
  char  *m_ring_array;          // ring buffer
  size_t m_ring_size;           // buffer size
  PQ_mq_event *m_sender_event;   // producer synchronization
  PQ_mq_event *m_receiver_event; // consumer synchronization
};
```

**MQueue_handle** - operation handle:

```c++
struct MQueue_handle {
  MQueue  *m_queue;
  size_t   m_consume_pending;     // accumulated consumed bytes (batch update)
  char    *m_buffer;              // local buffer for partial reads
  size_t   m_buffer_len;
  size_t   m_partial_bytes;       // partial read state
  size_t   m_expected_bytes;      // expected message length
  bool     m_length_word_complete;

  send(void *msg, size_t len);
  receive(void *msg, size_t *len);
};
```

### Synchronization Model

**Worker (producer) - blocking send:**
- Worker can only send to one leader
- If MQ is full, worker blocks until space is available
- Ensures worker doesn't outpace the leader

**Leader (consumer) - non-blocking receive:**
- Leader reads from multiple workers' MQs
- If a particular MQ is empty, return immediately (try next worker)
- No point waiting for a specific worker when others may have data

### Send Procedure

```
send(msg, len):
  send_bytes(len, MQ)    // send length word
  send_bytes(msg, MQ)    // send data

send_bytes(data, len, MQ):
  while true:
    rb = load(m_bytes_read) % m_ring_size
    wb = load(m_bytes_write) % m_ring_size
    available = m_ring_size - (wb - rb)
    if available >= len:
      write data into m_ring_array[wb .. wb + len]
      store(m_bytes_write, wb + len)  // atomic update
      return
```

### Receive Procedure

Two-phase read: first read the length word, then read the data. Non-blocking:
returns whatever is available, tracks partial state in `m_buffer`.

```
receive(MQ):
  len = receive_bytes(MQ)     // read 4-byte length word
  msg = receive_bytes(MQ)     // read message body

receive_bytes(MQ):
  if !m_length_word_complete:
    // try to read length word, may be partial
    // return if incomplete, resume on next call
  else:
    // read message body, may be partial
    // return if incomplete, resume on next call
```

### Optimizations

#### Batch Updating

Read position `m_bytes_read` updates are batched to reduce atomic operations.

Each receive accumulates consumed bytes in `m_consume_pending`. When the
threshold is reached, the read position is updated atomically.

**Lazy update strategy** to prevent over-calculation of available space:

```c++
offset = m_consume_pending;
m_consume_pending = 0;
write_barrier();
store(m_bytes_read, m_bytes_read + offset);
```

This ensures the sender sees a read position that is <= the actual position,
so it never writes into unconsumed space.

#### Quick Modulo

Ring buffer position requires `% m_ring_size`. When `m_ring_size` is a power
of 2, this reduces to a single `&` operation:

```
a % b = a & (b - 1)   // when b is power of 2
```

Pre-store `m_ring_size - 1` to replace `%` with `&`, avoiding expensive
division and multiplication instructions.

### Partial Result Data Transfer

When sending `record[0]` through the message queue, optimizations are applied:

1. **NULL fields:** Only send the NULL flag, skip field data
2. **Variable-length fields:** Only send effective length + data, not the
   full fixed-length allocation

**Message format on MQ:**

```
[total_len: 4B][null_len: 2B][null_flag: null_len bytes][field_data...]
```

Each field uses 2 bits in `null_flag`:
- `00`: NOT_CONST, NOT_NULL (send field data)
- `01`: NOT_CONST, NULL (skip field data)
- `10`: CONST, NOT_NULL (skip, use cached value)
- `11`: CONST, NULL (skip)

**Field_raw_data** structure for efficient transfer:

```c++
struct Field_raw_data {
  uchar *m_ptr;        // raw data pointer
  uint32 m_len;        // data length to send
  uchar  m_var_len;    // varstring length bytes (0 for fixed-length)
  bool   m_need_send;  // false if NULL or CONST
};
```

Leader reassembles received data into `record[0]` using
`convert_mq_data_to_record()`.

## References

- `MQueue`, `MQueue_handle` - message queue implementation
- `Field_raw_data` - field transfer descriptor
- `convert_mq_data_to_record` - reassembly on leader
