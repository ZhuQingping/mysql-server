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

/**
  @file sql/parallel_query/pq_aggregate.cc
  Parallel Query V1-complete: Aggregate classification, partial aggregation,
  and combine implementation.

  Phase 8 scope:
  - Implement pq_agg_type_to_string(), pq_classify_agg_item(),
    pq_check_agg_supported(), PQ_partial_agg operations, and
    pq_combine_agg_sets().
  - Only implicit grouping (no GROUP BY) is supported.
  - This infrastructure is inert: not called from any execution path
    because TryCreatePQTableScanIterator still returns nullptr.
  - Phase 6+ will use this for real parallel aggregation.
*/

#include "sql/parallel_query/pq_aggregate.h"

#include <cassert>
#include <cmath>
#include <cstring>

#include "sql/item.h"          // Item, Item::SUM_FUNC_ITEM
#include "sql/item_sum.h"      // Item_sum, Sumfunctype, has_distinct
#include "sql/sql_lex.h"       // Query_block

// ---------------------------------------------------------------------------
// pq_agg_type_to_string
// ---------------------------------------------------------------------------

const char *pq_agg_type_to_string(PQ_agg_type type) {
  switch (type) {
    case PQ_agg_type::COUNT:
      return "COUNT";
    case PQ_agg_type::SUM:
      return "SUM";
    case PQ_agg_type::AVG:
      return "AVG";
    case PQ_agg_type::MIN:
      return "MIN";
    case PQ_agg_type::MAX:
      return "MAX";
    case PQ_agg_type::UNSUPPORTED:
      return "UNSUPPORTED";
    default:
      return "UNKNOWN";
  }
}

// ---------------------------------------------------------------------------
// pq_classify_agg_item
// ---------------------------------------------------------------------------

PQ_agg_type pq_classify_agg_item(const Item *item) {
  if (item == nullptr) return PQ_agg_type::UNSUPPORTED;

  // Only Item_sum subclasses are aggregate functions.
  // In MySQL 8.0, Item::SUM_FUNC_ITEM is the type for all aggregate Items.
  if (item->type() != Item::SUM_FUNC_ITEM) {
    return PQ_agg_type::UNSUPPORTED;
  }

  const Item_sum *sum_item = down_cast<const Item_sum *>(item);

  // Classify by Item_sum::sum_func() (Sumfunctype enum).
  switch (sum_item->sum_func()) {
    case Item_sum::COUNT_FUNC:
      // COUNT(*) and COUNT(expr) are both supported.
      return PQ_agg_type::COUNT;
    case Item_sum::SUM_FUNC:
      // SUM(expr) is supported. DISTINCT variant is not (Phase 9).
      if (sum_item->has_with_distinct()) return PQ_agg_type::UNSUPPORTED;
      return PQ_agg_type::SUM;
    case Item_sum::AVG_FUNC:
      // AVG(expr) is supported. DISTINCT variant is not (Phase 9).
      if (sum_item->has_with_distinct()) return PQ_agg_type::UNSUPPORTED;
      return PQ_agg_type::AVG;
    case Item_sum::MIN_FUNC:
      // MIN(expr) is supported.
      return PQ_agg_type::MIN;
    case Item_sum::MAX_FUNC:
      // MAX(expr) is supported.
      return PQ_agg_type::MAX;
    default:
      // All other aggregate types (GROUP_CONCAT, STD, VAR, BIT_AND, etc.)
      // are not supported in PQ V1.
      return PQ_agg_type::UNSUPPORTED;
  }
}

// ---------------------------------------------------------------------------
// pq_check_agg_supported
// ---------------------------------------------------------------------------

bool pq_check_agg_supported(Query_block *query_block) {
  if (query_block == nullptr) return false;

  // Walk through ALL items in the fields list (both visible and hidden)
  // and check each aggregate. Hidden items can contain aggregates
  // that are not in the SELECT list but are needed for execution
  // (e.g., AVG internally decomposes into SUM + COUNT, or a GROUP BY
  //  item referenced in HAVING but not in the SELECT list).
  //
  // Phase 8: we iterate over the full fields deque to catch hidden
  // aggregates. Any hidden SUM_FUNC_ITEM that is not PQ-compatible
  // causes the whole query block to be rejected.
  for (Item *item : query_block->fields) {
    if (item == nullptr) continue;

    // If this Item is a SUM_FUNC_ITEM (aggregate), check if it's PQ-compatible.
    if (item->type() == Item::SUM_FUNC_ITEM) {
      PQ_agg_type agg_type = pq_classify_agg_item(item);
      if (agg_type == PQ_agg_type::UNSUPPORTED) {
        return false;  // Unsupported aggregate found (visible or hidden)
      }
    }
  }

  return true;  // All aggregates are PQ-compatible
}

// ---------------------------------------------------------------------------
// PQ_partial_agg: accumulate
// ---------------------------------------------------------------------------

void PQ_partial_agg::accumulate(double val, bool is_null) {
  switch (m_type) {
    case PQ_agg_type::COUNT:
      // COUNT(*) always increments. COUNT(expr) increments only if !is_null.
      // Phase 8 treats all COUNT the same way: increment if !is_null.
      // COUNT(*) is represented as COUNT with is_null=false for each row.
      if (!is_null) {
        m_count++;
        m_val_null = false;
      }
      break;

    case PQ_agg_type::SUM:
      if (!is_null) {
        m_val += val;
        m_count++;  // Track count for NULL-result determination
        m_val_null = false;
      }
      break;

    case PQ_agg_type::AVG:
      if (!is_null) {
        m_val += val;   // Partial sum
        m_count++;      // Partial count
        m_val_null = false;
      }
      break;

    case PQ_agg_type::MIN:
      if (!is_null) {
        if (m_val_null || val < m_val) {
          m_val = val;
        }
        m_val_null = false;
      }
      break;

    case PQ_agg_type::MAX:
      if (!is_null) {
        if (m_val_null || val > m_val) {
          m_val = val;
        }
        m_val_null = false;
      }
      break;

    default:
      // Unsupported: do nothing
      break;
  }
}

// ---------------------------------------------------------------------------
// PQ_partial_agg: combine
// ---------------------------------------------------------------------------

void PQ_partial_agg::combine(const PQ_partial_agg &other) {
  // If both are null, result is null.
  // If one is null, result is the non-null one.
  // If both are non-null, combine according to aggregate type.

  if (other.m_val_null && m_val_null) {
    // Both null: result stays null
    return;
  }

  if (m_val_null && !other.m_val_null) {
    // This is null, other is not: take other's values
    m_val = other.m_val;
    m_count = other.m_count;
    m_val_null = false;
    return;
  }

  if (!m_val_null && other.m_val_null) {
    // This is non-null, other is null: keep this
    return;
  }

  // Both non-null: combine
  switch (m_type) {
    case PQ_agg_type::COUNT:
      m_count += other.m_count;
      break;

    case PQ_agg_type::SUM:
      m_val += other.m_val;
      m_count += other.m_count;
      break;

    case PQ_agg_type::AVG:
      // AVG combine: total_sum / total_count
      m_val += other.m_val;     // Sum of partial sums
      m_count += other.m_count; // Sum of partial counts
      break;

    case PQ_agg_type::MIN:
      if (other.m_val < m_val) {
        m_val = other.m_val;
      }
      break;

    case PQ_agg_type::MAX:
      if (other.m_val > m_val) {
        m_val = other.m_val;
      }
      break;

    default:
      // Unsupported: do nothing
      break;
  }
}

// ---------------------------------------------------------------------------
// pq_combine_agg_sets
// ---------------------------------------------------------------------------

bool pq_combine_agg_sets(const PQ_agg_set *worker_sets, uint32_t n_workers,
                         PQ_agg_set *result) {
  if (worker_sets == nullptr || result == nullptr || n_workers == 0) {
    return true;  // Error: invalid input
  }

  size_t n_agg = result->size();
  if (n_agg == 0) return true;  // Error: no aggregates

  // Verify all worker sets have the same number of aggregates
  for (uint32_t w = 0; w < n_workers; w++) {
    if (worker_sets[w].size() != n_agg) {
      return true;  // Error: mismatched sizes
    }
  }

  // Verify slot type consistency: each aggregate slot must have the
  // same PQ_agg_type across all worker sets. This prevents accidental
  // misalignment (e.g., worker 0 slot 0 = COUNT but worker 1 slot 0 = SUM),
  // which would produce wrong combine results.
  for (size_t a = 0; a < n_agg; a++) {
    PQ_agg_type expected_type = worker_sets[0].m_partials[a].m_type;
    for (uint32_t w = 1; w < n_workers; w++) {
      if (worker_sets[w].m_partials[a].m_type != expected_type) {
        return true;  // Error: slot type mismatch across workers
      }
    }
  }

  // Initialize result from first worker, then combine remaining workers
  result->m_partials = worker_sets[0].m_partials;

  for (uint32 w = 1; w < n_workers; w++) {
    for (size_t a = 0; a < n_agg; a++) {
      result->m_partials[a].combine(worker_sets[w].m_partials[a]);
    }
  }

  // For AVG, compute final value: total_sum / total_count
  for (size_t a = 0; a < n_agg; a++) {
    if (result->m_partials[a].m_type == PQ_agg_type::AVG &&
        !result->m_partials[a].m_val_null) {
      if (result->m_partials[a].m_count > 0) {
        result->m_partials[a].m_val =
            result->m_partials[a].m_val / result->m_partials[a].m_count;
      } else {
        result->m_partials[a].m_val_null = true;
      }
    }
  }

  return false;  // Success
}
