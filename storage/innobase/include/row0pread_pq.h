#ifndef row0par_read_pq_h
#define row0par_read_pq_h

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "univ.i"

// Forward declarations
struct trx_t;
struct mtr_t;
struct btr_pcur_t;
struct buf_block_t;
struct dict_table_t;

#include "btr0cur.h"
#include "db0err.h"
#include "fil0fil.h"
#include "os0event.h"
#include "page0size.h"
#include "read0types.h"
#include "rem0types.h"
#include "sql/parallel_query/pq_handler.h"

class Parallel_leader final : public Parallel_leader_Base {
 public:
  using super = Parallel_leader_Base;
  /** Constructor.
  @param[in]  max_threads       Maximum number of threads to use. */
  explicit Parallel_leader(size_t max_threads, bool reverse_scan, trx_t *trx)
      : Parallel_leader_Base(max_threads, reverse_scan), m_trx(trx) {}

  std::shared_ptr<PQ_Scan_ctx_Base> make_scan_ctx(
      void *trx, const PQ_Config_Base &config) override;

  /** leader's m_trx for consistent read in worker thread */
  trx_t *m_trx{nullptr};
};

/** Boundary of the range to scan. */
struct Iter : public Iter_Base {
  /** Destructor. */
  ~Iter() override;

  const void *get_border() override;

  /** Heap used to allocate m_rec, m_tuple and m_pcur. */
  mem_heap_t *m_heap{};

  /** m_rec column offsets. */
  const ulint *m_offsets{};

  /** Start scanning from this key. Raw data of the row. */
  const rec_t *m_rec{};

  /** Tuple representation inside m_rec, for two Iter instances in a range
  m_tuple will be [first->m_tuple, second->m_tuple). */
  const dtuple_t *m_tuple{};

  /** Number of externally stored columns. */
  ulint m_n_ext{ULINT_UNDEFINED};

  /** Persistent cursor.*/
  btr_pcur_t *m_pcur{};

  /** Persistent cursor.*/
  btr_pcur_t *m_pcur_template{};
};

/** Scan (Scan_ctx) configuration. */
struct PQ_Config : public PQ_Config_Base {
  using super = PQ_Config_Base;
  /** Constructor.
  @param[in] scan_borders		 Range to scan.
  @param[in] index			 Cluster index to scan. */
  PQ_Config(const PQ_Borders &scan_borders, dict_index_t *index)
      : super(scan_borders),
        is_intrinsic(index->table->is_intrinsic()),
        m_index(index),
        m_is_compact(dict_table_is_comp(index->table)),
        m_page_size(dict_tf_to_fsp_flags(index->table->flags)) {}

  const bool is_intrinsic;  ///< if our table is InnoDB-intrinsic

  uint m_range_errno{0};

  /** (Cluster) Index in table to scan. */
  dict_index_t *m_index;

  /** Row format of table. */
  const bool m_is_compact;

  /** Tablespace page size. */
  const page_size_t m_page_size;

  btr_pcur_t *m_pcur{nullptr};
};

class PQ_Scan_ctx : public PQ_Scan_ctx_Base {
 public:
  /** Constructor.
  @param[in]  reader          Parallel reader that owns this context.
  @param[in]  id              ID of this scan context.
  @param[in]  trx             Transaction covering the scan.
  @param[in]  config          Range scan config. */
  PQ_Scan_ctx(Parallel_leader *reader, size_t id, trx_t *trx,
              const PQ_Config &config)
      : PQ_Scan_ctx_Base(reader, id, m_config_copy),
        m_trx(trx),
        m_config_copy(config) {}

  /** Destructor. */
  ~PQ_Scan_ctx() override { m_config_copy.clear(); }

  /** S lock the index. */
  void index_s_lock() override;

  /** S unlock the index. */
  void index_s_unlock() override;

  /** Partition the B+Tree for parallel read.
  @param[in] scan_borders         Range for partitioning.
  @param[in] level                Sub-range required level (0 == root).
  @param[out] ranges              The partition scan ranges. */
  void partition(const PQ_Borders &scan_borders, size_t level,
                 Ranges &ranges) override;

 private:
  /** mtr_t savepoint. */
  using Savepoint = std::pair<ulint, buf_block_t *>;

  /** For releasing the S latches after processing the blocks. */
  using Savepoints = std::vector<Savepoint, ut::allocator<Savepoint>>;

  /** Fetch a block from the buffer pool and acquire an S latch on it.
  @param[in]      page_id       Page ID.
  @param[in,out]  mtr           Mini transaction covering the fetch.
  @param[in]      line          Line from where called.
  @return the block fetched from the buffer pool. */
  buf_block_t *block_get_s_latched(const page_id_t &page_id, mtr_t *mtr,
                                   int line) const
      MY_ATTRIBUTE((warn_unused_result));

  /** Find the page number of the node that contains the search key. If the
  key is null then we assume -infinity.
  @param[in]  block             Page to look in.
  @param[in] key                Key of the first record in the range.
  @return the left child page number. */
  page_no_t search(const buf_block_t *block, const dtuple_t *key) const
      MY_ATTRIBUTE((warn_unused_result));

  /** Traverse from given sub-tree page number to start of the scan range
  from the given page number.
  @param[in]  page_no           Page number of sub-tree.
  @param[in,out]  mtr           Mini-transaction.
  @param[in] key                Key of the first record in the range.
  @param[in,out] savepoints     Blocks S latched and accessed.
  @return the leaf node page cursor. */
  page_cur_t start_range(page_no_t page_no, mtr_t *mtr, const dtuple_t *key,
                         Savepoints &savepoints) const
      MY_ATTRIBUTE((warn_unused_result));

  /** Create and add the range to the scan ranges.
  @param[in,out]  ranges        Ranges to scan.
  @param[in,out]  leaf_page_cursor Leaf page cursor on which to create the
                                persistent cursor.
  @param[in,out]  mtr           Mini-transaction */
  void create_range(Ranges &ranges, page_cur_t &leaf_page_cursor, mtr_t *mtr);

  /** Find the subtrees to scan in a block.
  @param[in]      scan_borders    Partition based on this scan range.
  @param[in]      page_no       Page to partition at if at required level.
  @param[in]      depth         Sub-range current level.
  @param[in]      level         Sub-range starting level (0 == root).
  @param[in,out]  ranges        Ranges to scan.
  @param[in,out]  mtr           Mini-transaction */
  void create_ranges(const PQ_Borders &scan_borders, page_no_t page_no,
                     size_t depth, const size_t level, Ranges &ranges,
                     mtr_t *mtr);

  /** Build a dtuple_t from rec_t.
  @param[in]      rec           Build the dtuple from this record.
  @param[in,out]  iter          Build in this iterator. */
  void copy_row(const rec_t *rec, Iter *iter) const;

  /** Create the persistent cursor that will be used to traverse the
  partition and position on the the start row.
  @param[in]      page_cursor   Current page cursor
  @param[in]      mtr           Mini-transaction covering the read.
  @return Start iterator. */
  std::shared_ptr<Iter> create_persistent_cursor(const page_cur_t &page_cursor,
                                                 mtr_t *mtr) const
      MY_ATTRIBUTE((warn_unused_result));

  dberr_t find_visible_record(byte *buf, const rec_t *&rec,
                              const rec_t *&clust_rec, ulint *&offsets,
                              ulint *&clust_offsets, mem_heap_t *&heap,
                              mtr_t *mtr, bool &mtr_has_extra_clust_latch,
                              row_prebuilt_t *prebuilt = nullptr)
      MY_ATTRIBUTE((warn_unused_result));

  std::shared_ptr<PQ_Ctx_Base> make_ctx(
      std::shared_ptr<PQ_Range> &range) override;

  bool index_s_own() const override {
    return (m_s_locks.load(std::memory_order_acquire) > 0);
  }

  /** Covering transaction. */
  const trx_t *m_trx{};

  PQ_Config m_config_copy;

  /** Number of threads that have S locked the index. */
  std::atomic_size_t m_s_locks{};

  friend class PQ_Ctx;
  friend class PQ_Ctx_intrinsic;

  friend class PQ_Range;
};

/** Parallel reader execution context. */
class PQ_Ctx : public PQ_Ctx_Base {
 public:
  using super = PQ_Ctx_Base;
  using super::super;

  /** Destructor. */
  ~PQ_Ctx() override;

  int read_record(uchar *buf, void *handler_specific) override
      MY_ATTRIBUTE((warn_unused_result));

  /** @return the covering transaction. */
  const trx_t *trx() const MY_ATTRIBUTE((warn_unused_result)) {
    return down_cast<PQ_Scan_ctx *>(m_scan_ctx)->m_trx;
  }

  /** @return the index being scanned. */
  const dict_index_t *index() const MY_ATTRIBUTE((warn_unused_result)) {
    return down_cast<PQ_Scan_ctx *>(m_scan_ctx)->m_config_copy.m_index;
  }

  void reset_for_next_read_round(bool reverse) override;

 public:
  /** Current row cursor */
  btr_pcur_t *m_pcur{};

  mem_heap_t *m_blob_heap{};  // heap for containing mysql records
  mem_heap_t *m_heap{};       // heap for containing innnodb rec
  ulint offsets_[REC_OFFS_NORMAL_SIZE];
  ulint clust_offsets_[REC_OFFS_NORMAL_SIZE];

 protected:
  /**
    @returns a pointer to the start boundary, as iterator.
    @note that it is a naked pointer obtained from a shared pointer, use it to
    read members, don't store it.
    @param reverse    true if the scan is done in reverse order.
  */
  auto get_iter(bool reverse) {
    return down_cast<Iter *>(
        (reverse ? m_range->m_iters.second : m_range->m_iters.first).get());
  }
};

class PQ_Ctx_intrinsic : public PQ_Ctx {
 public:
  using super = PQ_Ctx;
  using super::super;

  int read_record(uchar *buf, void *handler_specific) override
      MY_ATTRIBUTE((warn_unused_result));

  void reset_for_next_read_round(bool reverse) override;

 public:
  /** Compare the passed in record to see if it is out of the expected range
  @param[in]	rec	physical record to compare
  @param[in]	offsets	the offsets array of the record
  @return true if out of range, otherwise false */
  bool out_of_range(const rec_t *rec, const ulint *offsets);
};

#endif /* !row0par_read__pq_h */
