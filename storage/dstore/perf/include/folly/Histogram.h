/*
 * Copyright 2016 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Modified by Alex Depoutovitch, Huawei 2018(c)
 * This is a modified verion of folly's Histogram class that is extracted from
 * full folly library so not to include all other classes and 3rd party
 * dependencies.
 * In order to make it compatible with C++ 11, old version from 2016 has been
 * take.
 * Version tag is: v2016.12.05.00
 * https://github.com/facebook/folly/blob/v2016.12.05.00/folly
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define UBSAN_DISABLE(x)
#include "folly/Stats.h"

namespace folly {

namespace detail {

/*
 * A helper class to manage a set of histogram buckets.
 */
template <typename T, typename BucketT>
class HistogramBuckets {
 public:
  typedef T ValueType;
  typedef BucketT BucketType;

  /*
   * Create a set of histogram buckets.
   *
   * One bucket will be created for each bucketSize interval of values within
   * the specified range.  Additionally, one bucket will be created to track
   * all values that fall below the specified minimum, and one bucket will be
   * created for all values above the specified maximum.
   *
   * If (max - min) is not a multiple of bucketSize, the last bucket will cover
   * a smaller range of values than the other buckets.
   *
   * (max - min) must be larger than or equal to bucketSize.
   */
  HistogramBuckets(ValueType bucketSize, ValueType min, ValueType max,
                   const BucketType &defaultBucket);
  ~HistogramBuckets() {}

  /* Returns the bucket size of each bucket in the histogram. */
  ValueType getBucketSize() const { return bucketSize_; }

  /* Returns the min value at which bucketing begins. */
  ValueType getMin() const { return min_; }

  /* Returns the max value at which bucketing ends. */
  ValueType getMax() const { return max_; }

  /*
   * Returns the number of buckets.
   *
   * This includes the total number of buckets for the [min, max) range,
   * plus 2 extra buckets, one for handling values less than min, and one for
   * values greater than max.
   */
  size_t getNumBuckets() const { return buckets_.size(); }

  /* Returns the bucket index into which the given value would fall. */
  size_t getBucketIdx(ValueType value) const;

  /* Returns the bucket for the specified value */
  BucketType &getByValue(ValueType value) {
    return buckets_[getBucketIdx(value)];
  }

  /* Returns the bucket for the specified value */
  const BucketType &getByValue(ValueType value) const {
    return buckets_[getBucketIdx(value)];
  }

  /*
   * Returns the bucket at the specified index.
   *
   * Note that index 0 is the bucket for all values less than the specified
   * minimum.  Index 1 is the first bucket in the specified bucket range.
   */
  BucketType &getByIndex(size_t idx) { return buckets_[idx]; }

  /* Returns the bucket at the specified index. */
  const BucketType &getByIndex(size_t idx) const { return buckets_[idx]; }

  /*
   * Returns the minimum threshold for the bucket at the given index.
   *
   * The bucket at the specified index will store values in the range
   * [bucketMin, bucketMin + bucketSize), or [bucketMin, max), if the overall
   * max is smaller than bucketMin + bucketSize.
   */
  ValueType getBucketMin(size_t idx) const {
    if (idx == 0) {
      return std::numeric_limits<ValueType>::min();
    }
    if (idx == buckets_.size() - 1) {
      return max_;
    }

    return min_ + ((idx - 1) * bucketSize_);
  }

  /*
   * Returns the maximum threshold for the bucket at the given index.
   *
   * The bucket at the specified index will store values in the range
   * [bucketMin, bucketMin + bucketSize), or [bucketMin, max), if the overall
   * max is smaller than bucketMin + bucketSize.
   */
  ValueType getBucketMax(size_t idx) const {
    if (idx == buckets_.size() - 1) {
      return std::numeric_limits<ValueType>::max();
    }

    return min_ + (idx * bucketSize_);
  }

  /**
   * Computes the total number of values stored across all buckets.
   *
   * Runs in O(numBuckets)
   *
   * @param countFn A function that takes a const BucketType&, and returns the
   *                number of values in that bucket
   * @return Returns the total number of values stored across all buckets
   */
  template <typename CountFn>
  uint64_t computeTotalCount(CountFn countFromBucket) const;

  /**
   * Determine which bucket the specified percentile falls into.
   *
   * Looks for the bucket that contains the Nth percentile data point.
   *
   * @param pct     The desired percentile to find, as a value from 0.0 to 1.0.
   * @param countFn A function that takes a const BucketType&, and returns the
   *                number of values in that bucket.
   * @param lowPct  The lowest percentile stored in the selected bucket will be
   *                returned via this parameter.
   * @param highPct The highest percentile stored in the selected bucket will
   *                be returned via this parameter.
   *
   * @return Returns the index of the bucket that contains the Nth percentile
   *         data point.
   */
  template <typename CountFn>
  size_t getPercentileBucketIdx(double pct, CountFn countFromBucket,
                                double *lowPct = nullptr,
                                double *highPct = nullptr) const;

  /**
   * Estimate the value at the specified percentile.
   *
   * @param pct     The desired percentile to find, as a value from 0.0 to 1.0.
   * @param countFn A function that takes a const BucketType&, and returns the
   *                number of values in that bucket.
   * @param avgFn   A function that takes a const BucketType&, and returns the
   *                average of all the values in that bucket.
   *
   * @return Returns an estimate for N, where N is the number where exactly pct
   *         percentage of the data points in the histogram are less than N.
   */
  template <typename CountFn, typename AvgFn>
  ValueType getPercentileEstimate(double pct, CountFn countFromBucket,
                                  AvgFn avgFromBucket) const;

  /*
   * Iterator access to the buckets.
   *
   * Note that the first bucket is for all values less than min, and the last
   * bucket is for all values greater than max.  The buckets tracking values in
   * the [min, max) actually start at the second bucket.
   */
  typename std::vector<BucketType>::const_iterator begin() const {
    return buckets_.begin();
  }
  typename std::vector<BucketType>::iterator begin() {
    return buckets_.begin();
  }
  typename std::vector<BucketType>::const_iterator end() const {
    return buckets_.end();
  }
  typename std::vector<BucketType>::iterator end() { return buckets_.end(); }

 private:
  ValueType bucketSize_;
  ValueType min_;
  ValueType max_;
  std::vector<BucketType> buckets_;
};

}  // namespace detail

/*
 * A basic histogram class.
 *
 * Groups data points into equally-sized buckets, and stores the overall sum of
 * the data points in each bucket, as well as the number of data points in the
 * bucket.
 *
 * The caller must specify the minimum and maximum data points to expect ahead
 * of time, as well as the bucket width.
 */
template <typename T>
class Histogram {
 public:
  typedef T ValueType;
  typedef detail::Bucket<T> Bucket;

  Histogram(ValueType bucketSize, ValueType min, ValueType max)
      : buckets_(bucketSize, min, max, Bucket()) {}
  ~Histogram() {}
  /* Add a data point to the histogram */
  void addValue(ValueType value) UBSAN_DISABLE("signed-integer-overflow")
      UBSAN_DISABLE("unsigned-integer-overflow") {
    Bucket &bucket = buckets_.getByValue(value);
    // NOTE: Overflow is handled elsewhere and tests check this
    // behavior (see HistogramTest.cpp TestOverflow* tests).
    // TODO: It would be nice to handle overflow here and redesign this class.
    bucket.sum += value;
    bucket.count += 1;
  }

  /* Add multiple same data points to the histogram */
  void addRepeatedValue(ValueType value, uint64_t nSamples)
      UBSAN_DISABLE("signed-integer-overflow")
          UBSAN_DISABLE("unsigned-integer-overflow") {
    Bucket &bucket = buckets_.getByValue(value);
    // NOTE: Overflow is handled elsewhere and tests check this
    // behavior (see HistogramTest.cpp TestOverflow* tests).
    // TODO: It would be nice to handle overflow here and redesign this class.
    bucket.sum += value * nSamples;
    bucket.count += nSamples;
  }

  /*
   * Remove a data point to the histogram
   *
   * Note that this method does not actually verify that this exact data point
   * had previously been added to the histogram; it merely subtracts the
   * requested value from the appropriate bucket's sum.
   */
  void removeValue(ValueType value) UBSAN_DISABLE("signed-integer-overflow")
      UBSAN_DISABLE("unsigned-integer-overflow") {
    Bucket &bucket = buckets_.getByValue(value);
    // NOTE: Overflow is handled elsewhere and tests check this
    // behavior (see HistogramTest.cpp TestOverflow* tests).
    // TODO: It would be nice to handle overflow here and redesign this class.
    if (bucket.count > 0) {
      bucket.sum -= value;
      bucket.count -= 1;
    } else {
      bucket.sum = ValueType();
      bucket.count = 0;
    }
  }

  /* Remove multiple same data points from the histogram */
  void removeRepeatedValue(ValueType value, uint64_t nSamples) {
    Bucket &bucket = buckets_.getByValue(value);
    // TODO: It would be nice to handle overflow here.
    if (bucket.count >= nSamples) {
      bucket.sum -= value * nSamples;
      bucket.count -= nSamples;
    } else {
      bucket.sum = ValueType();
      bucket.count = 0;
    }
  }

  /* Remove all data points from the histogram */
  void clear() {
    for (size_t i = 0; i < buckets_.getNumBuckets(); i++) {
      buckets_.getByIndex(i).clear();
    }
  }

  /* Subtract another histogram data from the histogram */
  void subtract(const Histogram &hist) {
    // the two histogram bucket definitions must match to support
    // subtract.
    if (getBucketSize() != hist.getBucketSize() || getMin() != hist.getMin() ||
        getMax() != hist.getMax() || getNumBuckets() != hist.getNumBuckets()) {
      throw std::invalid_argument("Cannot subtract input histogram.");
    }

    for (size_t i = 0; i < buckets_.getNumBuckets(); i++) {
      buckets_.getByIndex(i) -= hist.buckets_.getByIndex(i);
    }
  }

  /* Merge two histogram data together */
  void merge(const Histogram &hist) {
    // the two histogram bucket definitions must match to support
    // a merge.
    if (getBucketSize() != hist.getBucketSize() || getMin() != hist.getMin() ||
        getMax() != hist.getMax() || getNumBuckets() != hist.getNumBuckets()) {
      throw std::invalid_argument("Cannot merge from input histogram.");
    }

    for (size_t i = 0; i < buckets_.getNumBuckets(); i++) {
      buckets_.getByIndex(i) += hist.buckets_.getByIndex(i);
    }
  }

  /* Copy bucket values from another histogram */
  void copy(const Histogram &hist) {
    // the two histogram bucket definition must match
    if (getBucketSize() != hist.getBucketSize() || getMin() != hist.getMin() ||
        getMax() != hist.getMax() || getNumBuckets() != hist.getNumBuckets()) {
      throw std::invalid_argument("Cannot copy from input histogram.");
    }

    for (size_t i = 0; i < buckets_.getNumBuckets(); i++) {
      buckets_.getByIndex(i) = hist.buckets_.getByIndex(i);
    }
  }

  /* Returns the bucket size of each bucket in the histogram. */
  ValueType getBucketSize() const { return buckets_.getBucketSize(); }
  /* Returns the min value at which bucketing begins. */
  ValueType getMin() const { return buckets_.getMin(); }
  /* Returns the max value at which bucketing ends. */
  ValueType getMax() const { return buckets_.getMax(); }
  /* Returns the number of buckets */
  size_t getNumBuckets() const { return buckets_.getNumBuckets(); }

  /* Returns the specified bucket (for reading only!) */
  const Bucket &getBucketByIndex(size_t idx) const {
    return buckets_.getByIndex(idx);
  }

  /*
   * Returns the minimum threshold for the bucket at the given index.
   *
   * The bucket at the specified index will store values in the range
   * [bucketMin, bucketMin + bucketSize), or [bucketMin, max), if the overall
   * max is smaller than bucketMin + bucketSize.
   */
  ValueType getBucketMin(size_t idx) const {
    return buckets_.getBucketMin(idx);
  }

  /*
   * Returns the maximum threshold for the bucket at the given index.
   *
   * The bucket at the specified index will store values in the range
   * [bucketMin, bucketMin + bucketSize), or [bucketMin, max), if the overall
   * max is smaller than bucketMin + bucketSize.
   */
  ValueType getBucketMax(size_t idx) const {
    return buckets_.getBucketMax(idx);
  }

  /**
   * Computes the total number of values stored across all buckets.
   *
   * Runs in O(numBuckets)
   */
  uint64_t computeTotalCount() const {
    CountFromBucket countFn;
    return buckets_.computeTotalCount(countFn);
  }

  /*
   * Get the bucket that the specified percentile falls into
   *
   * The lowest and highest percentile data points in returned bucket will be
   * returned in the lowPct and highPct arguments, if they are non-NULL.
   */
  size_t getPercentileBucketIdx(double pct, double *lowPct = nullptr,
                                double *highPct = nullptr) const {
    // We unfortunately can't use lambdas here yet;
    // Some users of this code are still built with gcc-4.4.
    CountFromBucket countFn;
    return buckets_.getPercentileBucketIdx(pct, countFn, lowPct, highPct);
  }

  /**
   * Estimate the value at the specified percentile.
   *
   * @param pct     The desired percentile to find, as a value from 0.0 to 1.0.
   *
   * @return Returns an estimate for N, where N is the number where exactly pct
   *         percentage of the data points in the histogram are less than N.
   */
  ValueType getPercentileEstimate(double pct) const {
    CountFromBucket countFn;
    AvgFromBucket avgFn;
    return buckets_.getPercentileEstimate(pct, countFn, avgFn);
  }

  /*
   * Get a human-readable string describing the histogram contents
   */
  std::string debugString() const;

  /*
   * Write the histogram contents in tab-separated values (TSV) format.
   * Format is "min max count sum".
   */
  void toTSV(std::ostream &out, bool skipEmptyBuckets = true) const;

  struct CountFromBucket {
    uint64_t operator()(const Bucket &bucket) const { return bucket.count; }
  };
  struct AvgFromBucket {
    ValueType operator()(const Bucket &bucket) const {
      if (bucket.count == 0) {
        return ValueType(0);
      }
      // Cast bucket.count to a signed integer type.  This ensures that we
      // perform division properly here: If bucket.sum is a signed integer
      // type but we divide by an unsigned number, unsigned division will be
      // performed and bucket.sum will be converted to unsigned first.
      // If bucket.sum is unsigned, the code will still do unsigned division
      // correctly.
      //
      // The only downside is if bucket.count is large enough to be negative
      // when treated as signed.  That should be extremely unlikely, though.
      return bucket.sum / static_cast<int64_t>(bucket.count);
    }
  };

 private:
  detail::HistogramBuckets<ValueType, Bucket> buckets_;
};

namespace detail {

template <typename T, typename BucketT>
HistogramBuckets<T, BucketT>::HistogramBuckets(ValueType bucketSize,
                                               ValueType min, ValueType max,
                                               const BucketType &defaultBucket)
    : bucketSize_(bucketSize), min_(min), max_(max) {
  // Deliberately make this a signed type, because we're about
  // to compare it against max-min, which is nominally signed, too.
  int64_t numBuckets = int64_t((max - min) / bucketSize);
  // Round up if the bucket size does not fit evenly
  if (numBuckets * bucketSize < max - min) {
    ++numBuckets;
  }
  // Add 2 for the extra 'below min' and 'above max' buckets
  numBuckets += 2;
  buckets_.assign(size_t(numBuckets), defaultBucket);
}

template <typename T, typename BucketType>
size_t HistogramBuckets<T, BucketType>::getBucketIdx(ValueType value) const {
  if (value < min_) {
    return 0;
  } else if (value >= max_) {
    return buckets_.size() - 1;
  } else {
    // the 1 is the below_min bucket
    return size_t(((value - min_) / bucketSize_) + 1);
  }
}

template <typename T, typename BucketType>
template <typename CountFn>
uint64_t HistogramBuckets<T, BucketType>::computeTotalCount(
    CountFn countFromBucket) const {
  uint64_t count = 0;
  for (size_t n = 0; n < buckets_.size(); ++n) {
    count += countFromBucket(const_cast<const BucketType &>(buckets_[n]));
  }
  return count;
}

template <typename T, typename BucketType>
template <typename CountFn>
size_t HistogramBuckets<T, BucketType>::getPercentileBucketIdx(
    double pct, CountFn countFromBucket, double *lowPct,
    double *highPct) const {
  auto numBuckets = buckets_.size();

  // Compute the counts in each bucket
  std::vector<uint64_t> counts(numBuckets);
  uint64_t totalCount = 0;
  for (size_t n = 0; n < numBuckets; ++n) {
    uint64_t bucketCount =
        countFromBucket(const_cast<const BucketType &>(buckets_[n]));
    counts[n] = bucketCount;
    totalCount += bucketCount;
  }

  // If there are no elements, just return the lowest bucket.
  // Note that we return bucket 1, which is the first bucket in the
  // histogram range; bucket 0 is for all values below min_.
  if (totalCount == 0) {
    // Set lowPct and highPct both to 0.
    // getPercentileEstimate() will recognize this to mean that the histogram
    // is empty.
    if (lowPct) {
      *lowPct = 0.0;
    }
    if (highPct) {
      *highPct = 0.0;
    }
    return 1;
  }

  // Loop through all the buckets, keeping track of each bucket's
  // percentile range: [0,10], [10,17], [17,45], etc.  When we find a range
  // that includes our desired percentile, we return that bucket index.
  double prevPct = 0.0;
  double curPct = 0.0;
  uint64_t curCount = 0;
  size_t idx;
  for (idx = 0; idx < numBuckets; ++idx) {
    if (counts[idx] == 0) {
      // skip empty buckets
      continue;
    }

    prevPct = curPct;
    curCount += counts[idx];
    curPct = static_cast<double>(curCount) / totalCount;
    if (pct <= curPct) {
      // This is the desired bucket
      break;
    }
  }

  if (lowPct) {
    *lowPct = prevPct;
  }
  if (highPct) {
    *highPct = curPct;
  }
  return idx;
}

template <typename T, typename BucketType>
template <typename CountFn, typename AvgFn>
T HistogramBuckets<T, BucketType>::getPercentileEstimate(
    double pct, CountFn countFromBucket, AvgFn avgFromBucket) const {
  // Find the bucket where this percentile falls
  double lowPct;
  double highPct;
  size_t bucketIdx =
      getPercentileBucketIdx(pct, countFromBucket, &lowPct, &highPct);
  if (lowPct == 0.0 && highPct == 0.0) {
    // Invalid range -- the buckets must all be empty
    // Return the default value for ValueType.
    return ValueType();
  }
  if (lowPct == highPct) {
    // Unlikely to have exact equality,
    // but just return the bucket average in this case.
    // We handle this here to avoid division by 0 below.
    return avgFromBucket(buckets_[bucketIdx]);
  }

  // Compute information about this bucket
  ValueType avg = avgFromBucket(buckets_[bucketIdx]);
  ValueType low;
  ValueType high;
  if (bucketIdx == 0) {
    if (avg > min_) {
      // This normally shouldn't happen.  This bucket is only supposed to track
      // values less than min_.  Most likely this means that integer overflow
      // occurred, and the code in avgFromBucket() returned a huge value
      // instead of a small one.  Just return the minimum possible value for
      // now.
      //
      // (Note that if the counter keeps being decremented, eventually it will
      // wrap and become small enough that we won't detect this any more, and
      // we will return bogus information.)
      return getBucketMin(bucketIdx);
    }
    // For the below-min bucket, just assume the lowest value ever seen is
    // twice as far away from min_ as avg.
    high = min_;
    low = high - (2 * (high - avg));
    // Adjust low in case it wrapped
    if (low > avg) {
      low = std::numeric_limits<ValueType>::min();
    }
  } else if (bucketIdx == buckets_.size() - 1) {
    if (avg < max_) {
      // Most likely this means integer overflow occurred.  See the comments
      // above in the minimum case.
      return getBucketMax(bucketIdx);
    }
    // Similarly for the above-max bucket, assume the highest value ever seen
    // is twice as far away from max_ as avg.
    low = max_;
    high = low + (2 * (avg - low));
    // Adjust high in case it wrapped
    if (high < avg) {
      high = std::numeric_limits<ValueType>::max();
    }
  } else {
    low = getBucketMin(bucketIdx);
    high = getBucketMax(bucketIdx);
    if (avg < low || avg > high) {
      // Most likely this means an integer overflow occurred.
      // See the comments above.  Return the midpoint between low and high
      // as a best guess, since avg is meaningless.
      return (low + high) / 2;
    }
  }

  // Since we know the average value in this bucket, we can do slightly better
  // than just assuming the data points in this bucket are uniformly
  // distributed between low and high.
  //
  // Assume that the median value in this bucket is the same as the average
  // value.
  double medianPct = (lowPct + highPct) / 2.0;
  if (pct < medianPct) {
    // Assume that the data points lower than the median of this bucket
    // are uniformly distributed between low and avg
    double pctThroughSection = (pct - lowPct) / (medianPct - lowPct);
    return T(low + ((avg - low) * pctThroughSection));
  } else {
    // Assume that the data points greater than the median of this bucket
    // are uniformly distributed between avg and high
    double pctThroughSection = (pct - medianPct) / (highPct - medianPct);
    return T(avg + ((high - avg) * pctThroughSection));
  }
}

}  // namespace detail

template <typename T>
std::string Histogram<T>::debugString() const {
  std::stringstream ss;
  ss << "num buckets: " << buckets_.getNumBuckets()
     << ", bucketSize: " << buckets_.getBucketSize()
     << ", min: " << buckets_.getMin() << ", max: " << buckets_.getMax()
     << "\n";

  for (size_t i = 0; i < buckets_.getNumBuckets(); ++i) {
    ss << "  " << buckets_.getBucketMin(i) << ": "
       << buckets_.getByIndex(i).count << "\n"
       << i;
  }

  return ss.str();
}

template <typename T>
void Histogram<T>::toTSV(std::ostream &out, bool skipEmptyBuckets) const {
  for (size_t i = 0; i < buckets_.getNumBuckets(); ++i) {
    // Do not output empty buckets in order to reduce data file size.
    if (skipEmptyBuckets && getBucketByIndex(i).count == 0) {
      continue;
    }
    const auto &bucket = getBucketByIndex(i);
    out << getBucketMin(i) << '\t' << getBucketMax(i) << '\t' << bucket.count
        << '\t' << bucket.sum << '\n';
  }
}

}  // namespace folly
