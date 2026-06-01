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

#include "my_config.h"

#include <gtest/gtest.h>
#include <string>

#include "sql/parallel_query/bloom_filter.h"

namespace bloom_filter_unittest {

TEST(BloomFilter, TestBloomFilter) {
  std::string value1("foo");
  std::string value2("bar");

  BloomFilter bloom_filter(/*bits=*/128, /*num_hashes=*/2);
  bloom_filter.AddValue(value1.data(), value1.size());
  ASSERT_TRUE(bloom_filter.PossiblyExists(value1.data(), value1.size()));
  ASSERT_FALSE(bloom_filter.PossiblyExists(value2.data(), value2.size()));

  bloom_filter.AddValue(value2.data(), value2.size());
  ASSERT_TRUE(bloom_filter.PossiblyExists(value1.data(), value1.size()));
  ASSERT_TRUE(bloom_filter.PossiblyExists(value2.data(), value2.size()));
}

TEST(BloomFilter, TestGetNumBitsInFilter) {
  // Test with some expected results from this web page:
  // https://hur.st/bloomfilter/
  ASSERT_EQ(3165, GetNumBitsInFilter(200, 0.0005));
  ASSERT_EQ(1438, GetNumBitsInFilter(100, 0.001));
  ASSERT_EQ(959, GetNumBitsInFilter(100, 0.01));
}

TEST(BloomFilter, GetNumHashFunctions) {
  // Test with some expected results from this web page:
  // https://hur.st/bloomfilter/
  ASSERT_EQ(11, GetNumHashFunctions(200, 0.0005));
  ASSERT_EQ(10, GetNumHashFunctions(100, 0.001));
  ASSERT_EQ(7, GetNumHashFunctions(100, 0.01));
}

TEST(BloomFilter, GetFalsePositivesProbability) {
  // Test with some expected results from this web page:
  // https://hur.st/bloomfilter/
  double res = GetFalsePositiveProbability(1000, 400, 10);
  ASSERT_TRUE(0.83122525 > res);
  ASSERT_TRUE(res > 0.83122524);

  res = GetFalsePositiveProbability(1000, 40, 10);
  ASSERT_TRUE(0.00001517 > res);
  ASSERT_TRUE(res > 0.00001516);

  ASSERT_EQ(1.0, GetFalsePositiveProbability(1, 4000, 10));
}

TEST(BloomFilter, TestReInitBloomFilter) {
  // See that counter are reset when we re-init the Bloom filter.
  std::string value1("foo");
  std::string value2("bar");
  BloomFilter bloom_filter(/*bits=*/128, /*num_hashes=*/2);
  bloom_filter.AddValue(value1.data(), value1.size());

  ASSERT_EQ(0, bloom_filter.GetNumAcceptedValues());
  ASSERT_EQ(0, bloom_filter.GetNumRejectedValues());

  ASSERT_TRUE(bloom_filter.PossiblyExists(value1.data(), value1.size()));
  ASSERT_FALSE(bloom_filter.PossiblyExists(value2.data(), value2.size()));
  ASSERT_EQ(1, bloom_filter.GetNumAcceptedValues());
  ASSERT_EQ(1, bloom_filter.GetNumRejectedValues());

  bloom_filter.Init(/*bits=*/0, /*num_hash_functions=*/0);
  ASSERT_EQ(0, bloom_filter.GetNumAcceptedValues());
  ASSERT_EQ(0, bloom_filter.GetNumRejectedValues());
}

}  // namespace bloom_filter_unittest
