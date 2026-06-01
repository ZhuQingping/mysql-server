#ifndef SQL_BLOOM_FILTER_H
#define SQL_BLOOM_FILTER_H

#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "extra/lz4/my_xxhash.h"
#include "my_bit.h"
#include "my_dbug.h"
#include "template_utils.h"

/// A set of random numbers that will be used as seeds for xxHash. They are
/// randomly picked, and have no special meaning.
constexpr uint64_t seeds[] = {19192266151884785ULL,    13671732931115033569ULL,
                              10600667739060003299ULL, 12771927295658813266ULL,
                              9519508907396087707ULL,  7425798094623675169ULL,
                              11419800887149245868ULL, 18283726473600735741ULL,
                              9889624649968333056ULL,  15275833277757137806ULL};

/// A set of random numbers that will be used as hash values for values with
/// zero length. Sending zero length value through xxHash triggers an UBSAN
/// warning. And we need as many hash values for the zero length value as the
/// maximum number of hash functions.
constexpr uint64_t zero_length_hash_values[] = {
    13462899692396258773ULL, 1118671130706480821ULL, 9154012000028016338ULL,
    4873981583365986953ULL,  4219621156507958561ULL, 9299928727625074882ULL,
    151099988541150776ULL,   9940980079427538991ULL, 5051336849912461522ULL,
    15804237834106254883ULL};

static constexpr size_t MAX_NUM_HASH_FUNCTIONS = array_elements(seeds);
static_assert(array_elements(zero_length_hash_values) == MAX_NUM_HASH_FUNCTIONS,
              "The number of hash values for zero lengtth values must be equal "
              "to the maximum number of hash functions!");

/// An implementation of a probabilistic data structure. It is used to test
/// whether an element is a member of a set, and it will return either
/// "definitely not in set" or "possibly in set". The rate of false positives
/// can be controlled by adjusting the number of bits the Bloom filter occupies
/// and the number of hash functions it uses.
///
/// Note that a Bloom filter is an append only data structure; once an element
/// is added to the Bloom filter it cannot be removed. You would have to clear
/// the entire Bloom filter and re-insert the elements to be kept in order to
/// remove elements.
///
/// Since Bloom filters are cheap to construct and use, typical usage of a Bloom
/// filter is as a pre-filtering stage before executing a more expensive
/// operation.
///
/// Example usage:
///
/// // Create a Bloom filter with 1024 available bits and 4 hash functions.
/// BloomFilter bloom_filter(1024, 4);
///
/// // Add your values to the Bloom filter.
/// bloom_filter.AddValue(10);
/// ...
/// bloom_filter.AddValue(100);
///
/// // Test whether the value 42 exists in the Bloom filter. If the function
/// // returns "false", we are 100% certain that the value does not exist.
/// // If the function returns "true", the value _may_ exist (not guaranteed).
/// bool value_exists = bloom_filter.PossiblyExists(42);
class BloomFilter {
 public:
  BloomFilter() {}

  BloomFilter(size_t num_bits, size_t num_hash_functions) {
    Init(num_bits, num_hash_functions);
  }

  void Init(size_t num_bits, size_t num_hash_functions) {
    assert(m_num_hash_functions < array_elements(seeds));

    m_num_rejected_values = 0;
    m_num_accepted_values = 0;
    m_num_hash_functions = num_hash_functions;
    m_bits.clear();

    // Ensure that num_bits always is a power of two. This allows us to avoid
    // mod when calculating which bit to test/set.
    num_bits = my_round_up_to_next_power(num_bits);
    m_bits.resize(num_bits, false);
  }

  void AddValue(const void *data, size_t length) {
    if (length == 0) {
      // Do not send zero length values through xxHash as it triggers an UBSAN
      // warning.
      for (size_t i = 0; i < m_num_hash_functions; ++i) {
        m_bits[GetBitIndex(zero_length_hash_values[i])] = true;
      }
    } else {
      for (size_t i = 0; i < m_num_hash_functions; ++i) {
        const uint64_t hash = MY_XXH64(data, length, seeds[i]);
        m_bits[GetBitIndex(hash)] = true;
      }
    }
  }

  bool PossiblyExists(const void *data, size_t length) const {
    if (length == 0) {
      // Do not send zero length values through xxHash as it triggers an UBSAN
      // warning.
      for (size_t i = 0; i < m_num_hash_functions; ++i) {
        if (!m_bits[GetBitIndex(zero_length_hash_values[i])] ||
            DBUG_EVALUATE_IF("force_bloom_filter_miss", true, false)) {
          ++m_num_rejected_values;
          return false;
        }
      }
    } else {
      for (size_t i = 0; i < m_num_hash_functions; ++i) {
        const uint64_t hash = MY_XXH64(data, length, seeds[i]);
        if (!m_bits[GetBitIndex(hash)] ||
            DBUG_EVALUATE_IF("force_bloom_filter_miss", true, false)) {
          ++m_num_rejected_values;
          return false;
        }
      }
    }

    ++m_num_accepted_values;
    return true;
  }

  /// @returns the number of values rejected by the Bloom filter since last call
  ///          to "Init()".
  size_t GetNumRejectedValues() const { return m_num_rejected_values; }

  /// @returns the number of values accepted by the Bloom filter since last call
  ///          to "Init()".
  size_t GetNumAcceptedValues() const { return m_num_accepted_values; }

 private:
  inline size_t GetBitIndex(uint64_t hash) const {
    // Since we know that the number of bits will be a power of two, do a
    // bitwise AND instead of (hash % m_bits.size()).
    assert((m_bits.size() & (m_bits.size() - 1)) == 0);
    return hash & (m_bits.size() - 1);
  }

  std::vector<bool> m_bits;
  size_t m_num_hash_functions{0};

  /// The number of values rejected by the Bloom filter since last call to
  /// Init(). Declared as mutable to avoid removing "const" from
  /// "PossiblyExists()".
  mutable size_t m_num_rejected_values{0};

  /// The number of values accepted by the Bloom filter since last call to
  /// Init(). Declared as mutable to avoid removing "const" from
  /// "PossiblyExists()".
  mutable size_t m_num_accepted_values{0};
};

// All of the formulas below are taken from this website:
// https://hur.st/bloomfilter
inline size_t GetNumBitsInFilter(size_t expected_num_items,
                                 double false_positive_probability) {
  return std::ceil((expected_num_items * std::log(false_positive_probability)) /
                   std::log(1 / std::pow(2, std::log(2))));
}

inline size_t GetNumHashFunctions(size_t expected_num_items,
                                  double false_positive_probability) {
  if (expected_num_items == 0) {
    // It is not possible to derive the number of hash functions with no items,
    // so just return 1 to avoid division by zero further down.
    return 1;
  }

  return std::round(
      (GetNumBitsInFilter(expected_num_items, false_positive_probability) /
       static_cast<double>(expected_num_items)) *
      std::log(2));
}

inline double GetFalsePositiveProbability(size_t num_bits_in_filter,
                                          size_t expected_num_items,
                                          size_t num_hash_functions) {
  if (expected_num_items == 0) {
    return 0;
  }

  return std::pow(1 - std::exp(-static_cast<int64_t>(num_hash_functions) /
                               (num_bits_in_filter /
                                static_cast<double>(expected_num_items))),
                  num_hash_functions);
}

#endif
