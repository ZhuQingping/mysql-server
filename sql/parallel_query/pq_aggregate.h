/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in license.xml
   elsewhere in this distribution.  You may use this software under the
   terms of the GNU General Public License, version 2.0,
   or the terms of any other license of the available in this distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef PQ_AGGREGATE_INCLUDED
#define PQ_AGGREGATE_INCLUDED

/**
  @file sql/parallel_query/pq_aggregate.h
  Parallel Query V1-complete: Aggregate type classification, partial
  aggregation, and combine logic.

  Phase 8 scope:
  - Define PQ_agg_type enum for supported aggregate functions.
  - Define PQ_partial_agg struct for per-worker partial aggregate values.
  - Define pq_agg_combine() for merging partial aggregates into final result.
  - Define pq_check_agg_supported() for checking if a query block's
    implicit grouping aggregates are PQ-compatible.
  - Only implicit grouping (aggregates without GROUP BY) is supported.
    Explicit GROUP BY remains serial fallback (Phase 9).

  Design rationale:
  - PQ V1 supports 5 basic aggregate types: COUNT, SUM, AVG, MIN, MAX.
  - Each worker computes partial aggregates over its scan range.
  - Workers send partial aggregates to leader via MQ as a special message.
  - Leader combines partial aggregates to produce final result.
  - COUNT(*) is the simplest: each worker counts rows, leader sums counts.
  - SUM: each worker sums column values, leader sums partial sums.
  - AVG: each worker computes (partial_sum, partial_count), leader computes
    total_sum / total_count.
  - MIN/MAX: each worker computes partial min/max, leader picks global min/max.

  Inert guarantee:
  - pq_check_agg_supported() is a pure decision function; it does not
    modify the execution plan or start any workers.
  - PQ_partial_agg is only allocated if PQ execution actually happens
    (which requires TryCreatePQTableScanIterator to return non-null,
    which it does not in Phase 8 because real workers are not yet created).
  - The aggregation infrastructure is ready for Phase 6+ when InnoDB
    parallel scan is implemented.
*/

#include <cstdint>
#include <vector>

class Item;
class Query_block;

/**
  PQ-supported aggregate function types.

  These are the aggregate functions that PQ V1 can handle in parallel.
  Each type defines how partial aggregation and combine work:
  - COUNT: partial=count, combine=sum
  - SUM: partial=sum, combine=sum
  - AVG: partial=(sum, count), combine=sum/count
  - MIN: partial=min, combine=min
  - MAX: partial=max, combine=max
*/
enum class PQ_agg_type {
  COUNT,     ///< COUNT(*) or COUNT(expr)
  SUM,       ///< SUM(expr)
  AVG,       ///< AVG(expr)
  MIN,       ///< MIN(expr)
  MAX,       ///< MAX(expr)
  UNSUPPORTED,  ///< Aggregate type not supported by PQ V1

  PQ_AGG_TYPE_COUNT  ///< Sentinel for iteration
};

/**
  Convert PQ_agg_type to human-readable string.

  @param type  The aggregate type
  @return      Static string constant (never nullptr)
*/
const char *pq_agg_type_to_string(PQ_agg_type type);

/**
  Classify an Item as a PQ_agg_type.

  Checks whether the Item is a supported aggregate function (SUM_ITEM,
  COUNT_ITEM, AVG_ITEM, MIN_ITEM, MAX_ITEM) and returns the corresponding
  PQ_agg_type. Returns UNSUPPORTED for any other Item type.

  @param item  The Item to classify

  @return  PQ_agg_type corresponding to the Item's aggregate function,
           or UNSUPPORTED if not a supported aggregate.
*/
PQ_agg_type pq_classify_agg_item(const Item *item);

/**
  Check whether a query block's implicit grouping aggregates are
  PQ-compatible.

  Only implicit grouping (aggregates without explicit GROUP BY) is checked.
  The function verifies that every aggregate Item in the SELECT list is
  one of the 5 supported types (COUNT, SUM, AVG, MIN, MAX).

  @param query_block  The query block to check

  @retval true   All aggregates are PQ-compatible
  @retval false  At least one aggregate is unsupported
*/
bool pq_check_agg_supported(Query_block *query_block);

/**
  Per-worker partial aggregate value for a single aggregate function.

  PQ_partial_agg holds the partial result of one aggregate function
  computed by one worker over its scan range. The leader combines
  all workers' partial aggregates to produce the final result.

  For COUNT/SUM/MIN/MAX: only m_val is used.
  For AVG: both m_val and m_count are used (m_val = partial sum,
           m_count = partial count, final = total_val / total_count).
*/
struct PQ_partial_agg {
  PQ_agg_type m_type{PQ_agg_type::UNSUPPORTED};  ///< Aggregate type
  double m_val{0.0};      ///< Partial aggregate value (sum/min/max)
  uint64_t m_count{0};       ///< Partial count (for COUNT and AVG)
  bool m_val_null{true};   ///< True if no rows contributed (result is NULL)

  /** Reset to default (null) state. */
  void reset() {
    m_val = 0.0;
    m_count = 0;
    m_val_null = true;
  }

  /**
    Accumulate one row's contribution into this partial aggregate.

    @param val     The value from this row (0 for COUNT(*))
    @param is_null True if the value is NULL (skip for most aggregates)
  */
  void accumulate(double val, bool is_null);

  /**
    Combine another partial aggregate into this one.

    After combining, this partial_agg represents the merged result
    of both partials. Used by leader to combine all worker results.

    @param other  The partial aggregate to merge into this one
  */
  void combine(const PQ_partial_agg &other);
};

/**
  Per-worker set of partial aggregates for all aggregate Items in a query.

  Each worker computes one PQ_partial_agg per aggregate Item. The leader
  collects all PQ_agg_set from all workers and combines them to produce
  the final query result.

  Phase 8: this struct is defined for infrastructure purposes. It is
  NOT allocated or used in any execution path because TryCreatePQTableScanIterator
  still returns nullptr (no real workers). Phase 6+ will allocate this
  in the worker's execution context and send it via MQ.
*/
struct PQ_agg_set {
  std::vector<PQ_partial_agg> m_partials;  ///< One partial per aggregate Item

  /** Initialize with given number of aggregate slots. */
  explicit PQ_agg_set(size_t n_agg = 0) : m_partials(n_agg) {}

  /** Reset all partial aggregates to null state. */
  void reset() {
    for (auto &p : m_partials) p.reset();
  }

  /** Number of aggregate slots. */
  size_t size() const { return m_partials.size(); }
};

/**
  Combine multiple PQ_agg_set (from all workers) into a final result set.

  This is the leader-side combine operation. For each aggregate slot,
  it merges all workers' partial values using the appropriate combine
  rule for the aggregate type.

  @param worker_sets  Array of PQ_agg_set, one per worker
  @param n_workers    Number of workers
  @param[out] result  Combined result set (must have same size as worker_sets[0])

  @retval false  Success
  @retval true   Error (e.g., mismatched sizes)
*/
bool pq_combine_agg_sets(const PQ_agg_set *worker_sets, uint32_t n_workers,
                         PQ_agg_set *result);

#endif  // PQ_AGGREGATE_INCLUDED
