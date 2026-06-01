/* Copyright (c) 2021, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef pq_handler_h
#define pq_handler_h

/**
   @file common types for all engines which support Parallel Query.

   Some types are final (e.g. PQ_Range), others are "base types" (their names
   end with _Base) and are meant to be used by engines as base classes, adding
   engine-specific members. Some functions use pointers to these base classes
   polymorphically.
*/

#include <limits.h>  // UINT_MAX
#include <string.h>  // memcpy
#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <vector>

#include "my_base.h"                                  // key_part_map
#include "mysql/components/services/bits/psi_bits.h"  // PSI_NOT_INSTRUMENTED
#include "mysql/psi/mysql_mutex.h"                    // mysql_mutex_t
#include "mysql/service_mysql_alloc.h"                // my_malloc

/** Tree depth at which we decide to split blocks further. */
constexpr int PQ_SPLIT_THRESHOLD = 2;

/**
  These constants have the same value as those in InnoDB (DB_SUCCESS) etc, so
  that code can be used in InnoDB and also in Temptable. The equality is
  verified elsewhere with static_assert. It is also verified that InnoDB's
  'dberr_t' type is the same size as 'int'.
*/
#define DB_SUCCESS_AS_IN_INNODB ((int)10)
#define DB_ERROR_AS_IN_INNODB ((int)11)
#define DB_END_OF_INDEX_AS_IN_INNODB ((int)1502)
#define DB_END_OF_RANGE_AS_IN_INNODB ((int)1504)
#define DB_NOT_FOUND_AS_IN_INNODB ((int)1505)

class PQ_Ctx_Base;
class Parallel_leader_Base;
class PQ_Scan_ctx_Base;

struct PQ_ref_key {
  uchar *ptr{nullptr};
  std::size_t len{0};

  PQ_ref_key() {}
  PQ_ref_key(const uchar *ptr1, std::size_t len1) {
    if (len1 > 0) {
      len = len1;
      ptr = static_cast<uchar *>(my_malloc(PSI_NOT_INSTRUMENTED, len1, MYF(0)));
      memcpy(ptr, ptr1, len1);
    } else {
      ptr = nullptr;
      len = 0;
    }
  }

  PQ_ref_key(const PQ_ref_key &ref_key)
      : PQ_ref_key(ref_key.ptr, ref_key.len) {}

  ~PQ_ref_key() {
    if (ptr) {
      my_free(ptr);
      ptr = nullptr;
      len = 0;
    }
  }

  bool operator==(const PQ_ref_key &key1) const {
    if (key1.len == len && len == 0)
      return true;
    else
      return ((key1.len == len) && !memcmp(key1.ptr, ptr, len));
  }
};

class PQ_Slice {
 public:
  PQ_Slice(size_t id, PQ_Scan_ctx_Base *scan_ctx, bool split)
      : m_id(id), m_split(split), m_scan_ctx(scan_ctx) {}

  /** @return the context ID. */
  size_t id() const MY_ATTRIBUTE((warn_unused_result)) { return (m_id); }

  virtual ~PQ_Slice() = default;

#ifndef NDEBUG
  void set_parent_id(size_t id) { m_parent_id = id; }

  int compare_to(const PQ_Slice *const other) {
    if (m_parent_id != other->m_parent_id) {
      return m_parent_id > other->m_parent_id ? 1 : -1;
    } else {
      return m_id == other->m_id ? 0 : m_id > other->m_id ? 1 : -1;
    }
  }
#endif  // NDEBUG

  void set_split(bool split) { m_split = split; }

  bool need_split() const { return m_split; }

  /** Split the context into sub-ranges and add them to the execution queue.
  @return true if error */
  virtual void split() {}

  PQ_Scan_ctx_Base *scan_ctx() { return m_scan_ctx; }

 protected:
  /** Context ID. */
  size_t m_id{std::numeric_limits<size_t>::max()};

#ifndef NDEBUG
  /** split from which context id */
  size_t m_parent_id{std::numeric_limits<size_t>::max()};
#endif  // NDEBUG

  /** If true the split the context at the block level. */
  bool m_split{false};

  /** Scanner context. */
  PQ_Scan_ctx_Base *m_scan_ctx{};
};

/// Base class holding an iterator which represents one bound of a range
struct Iter_Base {
  virtual ~Iter_Base() = default;
  /// @returns the iterator, in a form suitable for each child class
  virtual const void *get_border() = 0;
};

/** The first cursor should read up to the second cursor [f, s). */
using Iters = std::pair<std::shared_ptr<Iter_Base>, std::shared_ptr<Iter_Base>>;

class PQ_Range final : public PQ_Slice {
 public:
  Iters m_iters;

  PQ_Range(size_t id, PQ_Scan_ctx_Base *scan_ctx, Iters &iters, bool split)
      : PQ_Slice(id, scan_ctx, split), m_iters(iters) {}

  void split() override;
};

using Ranges = std::vector<std::shared_ptr<PQ_Range>>;

/**
   Specifies the range from where to start the scan and where to end it.
   The type of start/end is a generic pointer.
*/
struct PQ_Borders final {
  /** Default constructor. */
  PQ_Borders() : m_start(), m_end() {}

  /** Constructor.
  @param[in] start            Start key
  @param[in] end              End key. */
  PQ_Borders(const void *start, const void *end) : m_start(start), m_end(end) {}

  /** Start of the scan, can be nullptr for -infinity. */
  const void *m_start{};

  /** End of the scan, can be null for +infinity. */
  const void *m_end{};
};

/** Scan (Scan_ctx) configuration. */

struct PQ_Config_Base {
  /** Constructor.
  @param[in] scan_borders		 Range to scan.
  @param[in] index			 Cluster index to scan. */
  explicit PQ_Config_Base(const PQ_Borders &scan_borders)
      : m_scan_borders(scan_borders) {}

  /** Copy constructor.
  @param[in] config			 Instance to copy from. */
  PQ_Config_Base(const PQ_Config_Base &config) = default;

  virtual ~PQ_Config_Base() = default;
  void clear() {
    if (m_ref_key != nullptr) {
      free(m_ref_key);
      m_ref_key = nullptr;
    }
  }

  /** borders of Range to scan. */
  const PQ_Borders m_scan_borders;

  bool m_pq_reverse_scan{false};

  uchar *m_ref_key{nullptr};

  uint m_ref_key_len{};

  bool m_ref_depend{false};
};

class PQ_Scan_ctx_Base {
 public:
  /** Constructor.
      @param[in]  reader          Parallel reader that owns this context.
      @param[in]  id              ID of this scan context.
      @param[in]  config          Range scan config.
  */
  PQ_Scan_ctx_Base(Parallel_leader_Base *reader, size_t id,
                   PQ_Config_Base &config)
      : m_id(id), m_config(config), m_reader(reader) {}

  /** Destructor. */
  virtual ~PQ_Scan_ctx_Base() = default;

  /** S lock the index. */
  virtual void index_s_lock() = 0;

  /** S unlock the index. */
  virtual void index_s_unlock() = 0;

  /** Partition the data for parallel read.
  @param[in] scan_borders         Range for partitioning.
  @param[in] level                Sub-range required level (0 == root).
  @param[out] ranges              The partition scan ranges. */
  virtual void partition(const PQ_Borders &scan_borders, size_t level,
                         Ranges &ranges) = 0;

  /** Create the execution contexts based on the ranges.
  @param[in]  ranges            Ranges for which to create the contexts.
  @return DB_SUCCESS or error code. */

  Parallel_leader_Base *reader() { return m_reader; }

  size_t depth() const MY_ATTRIBUTE((warn_unused_result)) { return (m_depth); }

 protected:
  /** @return the scan context ID. */
  size_t id() const MY_ATTRIBUTE((warn_unused_result)) { return (m_id); }

  /** Set the error state.
  @param[in] err                Error state to set to. */
  void set_error_state(int err) {  // dberr_t->int
    m_err.store(err, std::memory_order_relaxed);
  }

  /** @return true if in error state. */
  bool is_error_set() const MY_ATTRIBUTE((warn_unused_result)) {
    return (m_err.load(std::memory_order_relaxed) != DB_SUCCESS_AS_IN_INNODB);
  }

  /** @return the maximum number of threads configured. */
  size_t max_threads() const;

  /** @return true if at least one thread owns the S latch on the index. */
  virtual bool index_s_own() const = 0;

  /** virtual ctor of proper child type (PQ_Ctx) */
  virtual std::shared_ptr<PQ_Ctx_Base> make_ctx(
      std::shared_ptr<PQ_Range> &range) = 0;

 public:
  /** Create an execution context for a range and add it to
  the Parallel_reader's run queue.
  @param[in] range              Range for which to create the context.
  @param[in] split              true if the sub-tree should be split further.
  @return DB_SUCCESS or error code. */
  std::shared_ptr<PQ_Ctx_Base> create_context(std::shared_ptr<PQ_Range> &range);

  uchar *get_ref_key(uint *key_len) {
    *key_len = m_config.m_ref_key_len;
    return m_config.m_ref_key;
  }

 protected:
  /** Context ID. */
  size_t m_id{std::numeric_limits<size_t>::max()};

  // Parallel scan configuration. References what's stored in the child class.
  PQ_Config_Base &m_config;

  /** Depth of the Btree. */
  size_t m_depth{};

  /** The parallel reader. */
  Parallel_leader_Base *m_reader{};

  /** Error during parallel read. */
  std::atomic<int> m_err{DB_SUCCESS_AS_IN_INNODB};

  friend class PQ_Ctx_Base;
  friend class PQ_Range;

  PQ_Scan_ctx_Base(PQ_Scan_ctx_Base &&) = delete;
  PQ_Scan_ctx_Base(const PQ_Scan_ctx_Base &) = delete;
  PQ_Scan_ctx_Base &operator=(PQ_Scan_ctx_Base &&) = delete;
  PQ_Scan_ctx_Base &operator=(const PQ_Scan_ctx_Base &) = delete;
};

template <typename T>
class PQ_queue {
 public:
  virtual std::shared_ptr<T> dequeue() = 0;
  virtual void enqueue(std::shared_ptr<T>) = 0;
  virtual void clear() = 0;
  virtual ~PQ_queue() = default;
};

template <typename T>
class PQ_dual_queue final : public PQ_queue<T> {
 private:
  using Elements = std::list<std::shared_ptr<T>>;

  Elements m_array[2];
  bool m_reverse{false};
  /// Indicates which of m_array is the current queue (other is the standby)
  uint m_current_idx{0};

 public:
  explicit PQ_dual_queue(bool reverse)
      : m_array{Elements(), Elements()}, m_reverse(reverse), m_current_idx(0) {}

  /// Pops head of current queue
  std::shared_ptr<T> dequeue() override {
    std::shared_ptr<T> element = nullptr;
    if (!current()->empty()) {
      element = current()->front();
      current()->pop_front();
    }
    return element;
  }

  /// Pushes to tail of standy queue
  void enqueue(std::shared_ptr<T> element) override {
    standby()->push_back(element);
  }

  void clear() override {
    m_array[0].clear();
    m_array[1].clear();
  }
  /// Moves everything from current to standby's tail, and makes the standby
  /// be the current.
  void rotate();

 private:
  inline Elements *standby() { return &m_array[rotate_index()]; }
  inline uint8_t rotate_index() const { return (m_current_idx + 1) % 2; }
  inline Elements *current() { return &m_array[m_current_idx]; }
};

template <typename T>
class PQ_single_queue : public PQ_queue<T> {
  using Elements = std::list<std::shared_ptr<T>>;

 public:
  Elements m_elements;

  bool m_reverse{false};

  /** Mutex protecting m_elementss. */
  mutable std::mutex m_mutex;

 public:
  explicit PQ_single_queue(bool reverse) : m_elements(), m_reverse(reverse) {}

  std::shared_ptr<T> dequeue() override;
  virtual void enqueue(std::shared_ptr<T> element) override {
    m_elements.push_back(element);
  }
  void clear() override { m_elements.clear(); }

  /// Inserts a whole vector
  void push(std::vector<std::shared_ptr<T>> &element, bool no_reverse);
};

class PQ_ranges_queue final : public PQ_single_queue<PQ_Range> {
 public:
  explicit PQ_ranges_queue(bool reverse) : PQ_single_queue<PQ_Range>(reverse) {}

  void mark_split(size_t n_slices, size_t n_threads, size_t &split_cur);
};

template <typename T, typename K>
class PQ_slices_mngr final {
 public:
  K m_slices_queue;

  /** state of worker currently doing parallel reads. */
  std::atomic<bool> work_done{false};

  // number of scan range contexts
  size_t m_n_slices{0};
  // number of worker threads
  size_t m_n_threads{0};
  // the slice id of spliting range context
  size_t m_spliting_id{UINT_MAX};
  // the depth of index btree
  uint m_btree_depth{0};

  /** Context ID. Monotonically increasing ID. */
  std::atomic_size_t m_slice_id{};

  std::mutex m_mutex;
  std::condition_variable m_cond;

 public:
  PQ_slices_mngr(size_t max_thread, bool reverse_scan)
      : m_slices_queue(reverse_scan), m_n_threads(max_thread) {}

  virtual ~PQ_slices_mngr() = default;

  void prepare_split_slice(std::vector<std::shared_ptr<T>> &slices,
                           bool reverse_scan) {
    m_mutex.lock();
    push(slices, false);
    if (!reverse_scan) {
      m_spliting_id++;
    } else {
      m_spliting_id--;
    }
    m_mutex.unlock();
    m_cond.notify_all();
  }

  bool is_split_done() { return m_spliting_id >= m_n_slices; }

  bool is_worker_done() { return work_done.load(std::memory_order_relaxed); }

  virtual std::shared_ptr<T> fetch_one() {
    std::shared_ptr<T> task = nullptr;

    std::unique_lock<std::mutex> lk(m_mutex);
    task = m_slices_queue.dequeue();
    while (task == nullptr && !is_split_done()) {
      m_cond.wait(lk);
      task = m_slices_queue.dequeue();
    }
    lk.unlock();
    return task;
  }

  virtual void push(std::vector<std::shared_ptr<T>> &slices, bool first_split) {
    m_slices_queue.push(slices, first_split);
  }

  void wakeup_workers() {
    set_worker_done();
    m_cond.notify_all();
  }

  void set_worker_done() { work_done.store(true, std::memory_order_relaxed); }

  void mark_split() {
    if ((m_btree_depth < PQ_SPLIT_THRESHOLD) && (m_n_slices < m_n_threads)) {
      return;
    }

    m_slices_queue.mark_split(m_n_slices, m_n_threads, m_spliting_id);
  }

  void reset() { m_n_slices = 0; }
};

/// A bundle of ref-access description
struct PQ_Ref_info final {
  bool pq_ref_depend{false};  ///< true if referred value is not constant
  const uchar *pq_ref_key_ptr{nullptr};  ///< key value to look up
  uint pq_ref_key_len{0};                ///< length of key value
};

/** Parallel reader execution context. */
class PQ_Ctx_Base : public PQ_Slice {
 public:
  /** Constructor.
  @param[in]    id              Thread ID.
  @param[in]    scan_ctx        Scan context.
  @param[in]    range           Range that the thread has to read. */
  PQ_Ctx_Base(size_t id, PQ_Scan_ctx_Base *scan_ctx,
              std::shared_ptr<PQ_Range> &range)
      : PQ_Slice(id, scan_ctx, false), m_range(range) {}

  /** Destructor. */
  virtual ~PQ_Ctx_Base() = default;

  /**
     Reads next record inside the range.
     @param[out] buf  record will be stored here
     @param      handler_specific  pointer to anything suited for the engine
     @returns error code
   */
  virtual int read_record(uchar *buf, void *handler_specific)
      MY_ATTRIBUTE((warn_unused_result)) = 0;

  bool check_ref_key(const PQ_Ref_info &ref_info);

  virtual void reset_for_next_read_round(bool reverse) = 0;

 protected:
  /** Range to read in this context. */
  std::shared_ptr<PQ_Range> m_range{};

 public:
  /** Current executing thread ID. */
  size_t m_thread_id{std::numeric_limits<size_t>::max()};

  bool start_read{true};  // start to read range's records

  Parallel_leader_Base *reader;
};

template <class T>
class Map_cell {
 public:
  struct KV {
    const PQ_ref_key *key{nullptr};
    T value{nullptr};
    KV *next{nullptr};

    ~KV() {
      if (key) {
        delete key;
        key = nullptr;
      }
    }
  };
  mysql_mutex_t m_mutex;
  KV *head;

  Map_cell() : head(nullptr) {
    mysql_mutex_init(PSI_NOT_INSTRUMENTED, &m_mutex, MY_MUTEX_INIT_FAST);
  }

  bool find(const PQ_ref_key &ref_key, T *pvalue) {
    mysql_mutex_lock(&m_mutex);
    KV *e = head;
    while (e != nullptr) {
      if (*(e->key) == ref_key) break;
      e = e->next;
    }
    mysql_mutex_unlock(&m_mutex);
    if (e) {
      if (pvalue) *pvalue = e->value;
      return true;
    } else
      return false;
  }

  virtual T get(const PQ_ref_key &ref_key) = 0;

  bool insert(const PQ_ref_key *ref_key, T value) {
    bool ret = true;
    mysql_mutex_lock(&m_mutex);
    KV *e = head;
    if (e == nullptr) {
      PQ_ref_key *ref_key1 = new PQ_ref_key(*ref_key);
      KV *kv = new KV{ref_key1, value};
      head = kv;
    } else {
      while (e != nullptr) {
        if (*(e->key) == *ref_key) {
          ret = false;
          break;
        }
        e = e->next;
      }
      PQ_ref_key *ref_key1 = new PQ_ref_key(*ref_key);
      KV *kv = new KV{ref_key1, value};
      kv->next = head;
      head = kv;
    }
    mysql_mutex_unlock(&m_mutex);
    return ret;
  }

  virtual ~Map_cell() {
    KV *kv = head;
    KV *next = nullptr;
    while (kv) {
      next = kv->next;
      delete kv;
      kv = next;
    }
    head = nullptr;
    mysql_mutex_destroy(&m_mutex);
  }
};

using Slices_mngr = PQ_slices_mngr<PQ_Range, PQ_ranges_queue>;

template <class T, class K>
class PQ_map {
 public:
  static const int MAX_CELLS = 512;

  K m_elements[MAX_CELLS];

  std::size_t hashcode(const PQ_ref_key &key) const {
    if (key.len == 0) return 0;
    std::size_t h = 0;
    for (ulong i = 0; i < key.len; i++) {
      h = 31 * h + (key.ptr[i] & 0xff);
    }
    return h % MAX_CELLS;
  }

  T operator[](const PQ_ref_key &key) {
    size_t no = hashcode(key);
    return (m_elements[no].get(key));
  }

  bool find(const PQ_ref_key &key) {
    size_t no = hashcode(key);
    return (m_elements[no].find(key, nullptr));
  }

  bool insert(const PQ_ref_key *key, T value) {
    size_t no = hashcode(*key);
    return m_elements[no].insert(key, value);
  }
};

class Slice_map_cell : public Map_cell<Slices_mngr *> {
 public:
  Slices_mngr *get(const PQ_ref_key &ref_key) override {
    mysql_mutex_lock(&m_mutex);
    KV *e = head;
    while (e != nullptr) {
      if (*(e->key) == ref_key) break;
      e = e->next;
    }
    mysql_mutex_unlock(&m_mutex);
    if (e)
      return e->value;
    else
      return nullptr;
  }
};

class Key_map_cell : public Map_cell<bool> {
 public:
  bool get(const PQ_ref_key &ref_key) override {
    mysql_mutex_lock(&m_mutex);
    KV *e = head;
    while (e != nullptr) {
      if (*(e->key) == ref_key) break;
      e = e->next;
    }
    mysql_mutex_unlock(&m_mutex);
    if (e)
      return e->value;
    else
      return false;
  }
};

using PQ_slices_map = PQ_map<Slices_mngr *, Slice_map_cell>;

class PQ_ref_map : public PQ_map<bool, Key_map_cell> {
  mysql_mutex_t m_map_mutex;

 public:
  PQ_ref_map() {
    mysql_mutex_init(PSI_NOT_INSTRUMENTED, &m_map_mutex, MY_MUTEX_INIT_FAST);
  }
  ~PQ_ref_map() { mysql_mutex_destroy(&m_map_mutex); }

  void enter() { mysql_mutex_lock(&m_map_mutex); }
  void exit() { mysql_mutex_unlock(&m_map_mutex); }
};

class Parallel_worker final {
 private:
  /// queue used to keep local PQ_ctxs
  PQ_dual_queue<PQ_Ctx_Base> m_local_ctxs;

  PQ_slices_map &pq_slices_map;
  PQ_ref_map &pq_key_map;
  /** used to record ref key status */

 public:
  // Store the ref_key to determine whether it has changed during
  // ha_innobase::pq_worker_scan_next processing.
  std::shared_ptr<PQ_ref_key> p_ref_key{};

 public:
  Parallel_worker(bool reverse, PQ_slices_map &queue_map, PQ_ref_map &key_map)
      : m_local_ctxs(reverse), pq_slices_map(queue_map), pq_key_map(key_map) {}

  /** Destructor. */
  ~Parallel_worker() { m_local_ctxs.clear(); }

  /**
     Picks a context for execution.
     @param ref_info description of ref access key
     @param[out] ctx found context (nullptr if not found)
     @returns true if found context
   */
  bool dispatch_ctx(const PQ_Ref_info &ref_info,
                    std::shared_ptr<PQ_Ctx_Base> *ctx);
  std::shared_ptr<PQ_Ctx_Base> get_ctx(const PQ_Ref_info &ref_info);

  void clear_ctx() { m_local_ctxs.clear(); }
};

class Parallel_leader_Base {
 public:
  /** Maximum value for innodb-parallel-read-threads. */
  constexpr static size_t MAX_THREADS{256};

  /** Constructor.
  @param[in]  max_threads       Maximum number of threads to use. */
  explicit Parallel_leader_Base(size_t max_threads, bool reverse_scan);

  /** Destructor. */
  virtual ~Parallel_leader_Base();

  bool build_ranges(void *trx, const PQ_Config_Base &config);

  /** @return the configured max threads size. */
  size_t max_threads() const MY_ATTRIBUTE((warn_unused_result)) {
    return m_max_threads;
  }

  uint key{0};

  // leader only splits table into ranges
  // use a map to store slices. there could be many slices queues, each
  // correspond different ref key. for table/index and "ref const", there are
  // only one slice queue, which key is NULL. for "ref field" there could be
  // many slices queues, each correspond different depend ref key.
  PQ_slices_map pq_slices_map;

  // Indicate whether ref key store in slices map or not.
  PQ_ref_map pq_key_map;

  /** Set the error state.
  @param[in] err				 Error state to set to. */
  void set_error_state(int err) { m_err.store(err, std::memory_order_relaxed); }

  /** @return true if in error state. */
  bool is_error_set() const MY_ATTRIBUTE((warn_unused_result)) {
    return (m_err.load(std::memory_order_relaxed) != DB_SUCCESS_AS_IN_INNODB);
  }

  bool is_reverse() const { return m_reverse; }

  // Disable copying.
  Parallel_leader_Base(const Parallel_leader_Base &) = delete;
  Parallel_leader_Base(const Parallel_leader_Base &&) = delete;
  Parallel_leader_Base &operator=(Parallel_leader_Base &&) = delete;
  Parallel_leader_Base &operator=(const Parallel_leader_Base &) = delete;

 protected:
  /**
     Creates a PQ_Scan_ctx_Base. Has to be defined in the concrete child class.
     @param handler_specific Pointer to anything suitable for the storage
     engine.
     @param config  Configuration
     @returns the created object.
   */
  virtual std::shared_ptr<PQ_Scan_ctx_Base> make_scan_ctx(
      void *handler_specific, const PQ_Config_Base &config) = 0;

  using Scan_ctxs = std::list<std::shared_ptr<PQ_Scan_ctx_Base>>;

  /**
     Scan contexts.

     @note This is only used for storing objects, and never fetching them. Still
     it is necessary, as these objects are shared_ptr, so storing them extends
     their lifetime.
  */
  Scan_ctxs m_scan_ctxs{};

  /** Counter for allocating scan context IDs. */
  size_t m_scan_ctx_id{};

  size_t m_max_threads{};

  bool m_reverse{false};

  std::atomic<int> m_err{DB_SUCCESS_AS_IN_INNODB};
};

template <typename T>
void PQ_single_queue<T>::push(std::vector<std::shared_ptr<T>> &ctxs,
                              bool no_reverse) {
  std::unique_lock<std::mutex> lk(m_mutex);
  if (!no_reverse && m_reverse) {
    m_elements.insert(m_elements.begin(), ctxs.begin(), ctxs.end());
  } else {
    m_elements.insert(m_elements.end(), ctxs.begin(), ctxs.end());
  }
}

template <typename T>
std::shared_ptr<T> PQ_single_queue<T>::dequeue() {
  std::shared_ptr<T> ctx = nullptr;
  std::unique_lock<std::mutex> lk(m_mutex);
  if (!m_elements.empty()) {
    if (!m_reverse) {
      ctx = m_elements.front();
      m_elements.pop_front();
    } else {
      ctx = m_elements.back();
      m_elements.pop_back();
    }
  }
  return ctx;
}

template <typename T>
void PQ_dual_queue<T>::rotate() {
  {
    // filled standby;
    auto element = dequeue();
    while (element != nullptr) {
      enqueue(element);
      element = dequeue();
    }
  }
  // reset pcur
  for (auto &element : *standby()) {
    element->reset_for_next_read_round(m_reverse);
  }
  // rotate array
  m_current_idx = rotate_index();
}

/* PQ temptable table type */
enum class PQ_temp_table_type {
  TEMP_MEM,    /* Temptable */
  TEMP_INNODB, /* InnoDB */
  TEMP_HEAP,   /* MEMORY */
  TEMP_UNKNOWN
};

struct PQ_shared_info {
  PQ_temp_table_type m_type{PQ_temp_table_type::TEMP_UNKNOWN};
  void *m_share{nullptr};
  void *m_table{nullptr};
};

struct Key_ref {
  int keyno{0};
  const uchar *key{nullptr};
  key_part_map keypart_map{0};
  bool reverse{false};
};
#endif /* !pq_handler_h */
