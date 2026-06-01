/********************************************************************
 * Copyright (C) Huawei Technologies, 2018
 * Performance counters declarations: contains declaration of peformace
 * measurement counters for debugging and monitoring application
 ********************************************************************/

#ifndef __PERFCOUNTERS_H__
#define __PERFCOUNTERS_H__

#include "common/cde_def.h"
#include "dumpmem.h"
#include "perfpublisherproto.h"
#include "securec.h"
#include "stringutils.h"

#include <folly/Histogram.h>
#include <sched.h>
#include <stdint.h>
#include <sys/sysinfo.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#ifdef CDE_ENABLE_HOT_PERF_COUNTERS
#define CDE_HOT_COUNTER_WRAPPER(perfFunction) perfFunction;
#else
#define CDE_HOT_COUNTER_WRAPPER(perfFunction) \
  do {                                        \
  } while (0)
#endif

namespace Huawei {

namespace Common {

class CounterNode;
class CounterBase;
class CounterCatalog;
class CounterCatalogItem;
struct CounterComparator;

// Counters and nodes are identified at run time by unique handle
// their memory address can serve as such unique handle
typedef void *CounterNodeHandle;
static void *const NullNodeHandle = nullptr;

typedef std::shared_ptr<CounterNode> CounterNodePtr;

// In order for users to quickly specify a counter, they can use numerical ids
typedef uint64_t CounterLibID;
static const CounterLibID InvalidCounterLibId = (CounterLibID)-1;

// map of child  nodes: key - handle, value class shared pointer
typedef std::unordered_map<CounterNodeHandle, CounterNodePtr> CounterNodeMap;
// map of child  nodes: key - counter id, value class shared pointer
typedef std::map<CounterLibID, CounterNodePtr> CounterIdNodeMap;
// set of counters belonging to a node
typedef std::set<CounterBase *, CounterComparator> CounterSet;

// map where the key is id of catalog item and value is item itself
typedef std::unordered_map<CounterLibID, CounterCatalogItem *> CatalogIDMap;

/*
 * Counters are organized in file sytem like structure, with counters being
 * files and counter nodes being directories.
 *
 */
class CounterCatalogItem {
 public:
  CounterCatalogItem(const std::string &name)
      : _id(InvalidCounterLibId), _name(name) {}

  CounterLibID getId() const { return _id; }

  virtual std::string getName() const { return _name; }
  ~CounterCatalogItem() {}

 protected:
  // Id of the counter, all ids are unique, determined at run time
  // and are required for programmatic access
  CounterLibID _id;
  // Name of the counter, names are unique, human readable and allow users to
  // identify counters
  std::string _name;
  friend class CounterCatalog;
};

// This is base class for all counters, it defines basic counter interface
class CounterBase : public CounterCatalogItem {
 public:
  CounterBase(const std::string &name)
      : CounterCatalogItem(name), _disableReset(false) {}
  virtual ~CounterBase() = default;

  // writes human-readable contents of the counter to the provided stream
  virtual void dump(std::ostream & /* os */,
                    CounterCatalogItem * /* parent */) const {}

  // resets the value of the counter, so that values will start to accumulate
  // anew
  void reset() {
    if (!_disableReset) {
      resetInternal();
    }
  }
  // If a counter is used not only for monitoring, it should not be disabled
  // This function, prevents reset of counter by CounterCatalog requests
  void disableReset() { _disableReset = true; }

 private:
  bool _disableReset;
  virtual void resetInternal() {}

  friend std::ostream &operator<<(std::ostream &os, const CounterBase &c);
};

// Custom Comparator for CounterBase*
struct CounterComparator {
  bool operator()(const CounterBase *lhs, const CounterBase *rhs) const {
    return lhs->getId() < rhs->getId();
  }
};

/*
 * Operator to send contents of a counter to a text stream
 */
inline std::ostream &operator<<(std::ostream &os, const CounterBase &c) {
  c.dump(os, nullptr);
  return os;
}

/*
 * Counter that calculates frequency of events.
 */

class EventCounter : public CounterBase {
 public:
  EventCounter(const std::string &name) : CounterBase(name) { resetInternal(); }

  virtual void triggerEvent() { _counter++; }

  float frequency() const {
    using namespace std::chrono;
    milliseconds ms = duration_cast<milliseconds>(sal_clock::now() - _start);
    if (ms.count() == 0) {
      return 0;
    }
    return ((float)_counter.load()) / ms.count() * 1000;
  }

  virtual uint64_t count() const { return _counter.load(); }

  ~EventCounter() {}

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    uint32_t cls = static_cast<uint32_t>(CounterClass::EventCounter);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));

    EventCounterData data;
    data.id = _id;
    if (parent != nullptr) {
      data.parent_id = parent->getId();
    } else {
      data.parent_id = InvalidCounterLibId;
    }
    data.name_length = std::min(_name.size(), sizeof(data.name));
    int ret =
        strncpy_s(data.name, NAME_LENGTH_MAX, _name.c_str(), data.name_length);
    CDE_ASSERT(ret == EOK);
    UNUSED_PARAM(ret);
    data.frequency = frequency();
    data.count = count();
    os.write(reinterpret_cast<char *>(&data), sizeof(data) - sizeof(data.name));
    os.write(data.name, data.name_length);
  }

 protected:
  void resetInternal() override {
    _counter = 0;
    _start = sal_clock::now();
  }

  std::atomic_uint_fast64_t _counter;
  sal_timep_t _start;
};

class StringNameCounter : public CounterBase {
 public:
  StringNameCounter(const std::string &name) : CounterBase(name) {}

  void setValueString(const std::string &value) { _value = value; }
  std::string getValueString() { return _value; }
  ~StringNameCounter() {}

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    uint32_t cls = static_cast<uint32_t>(CounterClass::StringNameCounter);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));

    StringNameCounterData data;
    data.id = _id;
    if (parent != nullptr) {
      data.parent_id = parent->getId();
    } else {
      data.parent_id = InvalidCounterLibId;
    }
    data.name_length = std::min(_name.size(), sizeof(data.name));
    data.value_name_length = std::min(_value.size(), sizeof(data.value_name));
    os.write(reinterpret_cast<char *>(&data),
             sizeof(data) - sizeof(data.name) - sizeof(data.value_name));
    int ret = strncpy_s(data.value_name, NAME_LENGTH_MAX, _value.c_str(),
                        data.value_name_length);
    UNUSED_PARAM(ret);
    os.write(data.value_name, data.value_name_length);
    ret =
        strncpy_s(data.name, NAME_LENGTH_MAX, _name.c_str(), data.name_length);
    UNUSED_PARAM(ret);
    os.write(data.name, data.name_length);
  }

 protected:
  std::string _value;
};
/*
 * Event counter for high frequency events optimized for NUMA
 * Counters are separate per core to avoid cache ping-pong
 * Once in a while per-core counters are summarized in one.
 * Performance overhead is 10x less than for EventCounter
 */
class NumaEventCounter : public EventCounter {
 public:
  /*
   * Constructs the counter
   *
   * Parameters:
   *  si - sum interval, how large local per-core counter should be
   *       to be added to the total counter. The bigger the value, the
   *       smaller the overhead, and worse accuracy
   *  name - name of the counter
   */
  NumaEventCounter(int si, const std::string &name) : EventCounter(name) {
    _coreNumber = get_nprocs();
    _counters = new std::atomic_uint_fast64_t[_coreNumber * Allignment];
    for (int i = 0; i < _coreNumber; i++) {
      _counters[i * Allignment] = 0;
    }
    _sumInterval = si;
    _summarizing = false;
  }

  void triggerEvent() override {
    int id = sched_getcpu();
    if (id < 0) {
      id = 0;
    }
    if (_counters[id * Allignment]++ > _sumInterval) {
      summarize();
    }
  }

  int getErrorMargin() const { return _coreNumber; }

  // returns the approximate value of count, which is equal of the last
  // summarized value plus whatever the local value use
  // this is best we can do without causing CPU cache miss
  uint64_t count() const override {
    int id = sched_getcpu();
    if (id < 0) {
      id = 0;
    }
    return _counter.load() + _counters[id * Allignment];
  }

  ~NumaEventCounter() { delete[] _counters; }

  /*
   * Adds and resets all per-core counters to the total one
   */
  void summarize() {
    bool expected = false;
    if (!_summarizing.compare_exchange_strong(expected, true)) {
      // already in progress
      return;
    }
    for (int i = 0; i < _coreNumber; i++) {
      _counter += _counters[i * Allignment].exchange(0);
    }
    _summarizing = false;
  }

 protected:
  void resetInternal() override {
    for (int i = 0; i < _coreNumber; i++) {
      _counters[i * Allignment] = 0;
    }
    EventCounter::resetInternal();
  }

 private:
  const int Allignment = 8;  // how many 64bit words are in one cache
                             // line, used to move different counters
                             // to different cache lines
  int _coreNumber;           // number of cores in the system
  std::atomic_uint_fast64_t *_counters;  // local per-core counters
  unsigned _sumInterval;                 // how often we add local counters to
                                         // the global one
  std::atomic<bool> _summarizing;        // flag that summarizing is running
};

/* Simple value counter with only min/max/last/count (ie, no sum & avg).
 * Has lower overhead than NumericCounter and for different tracking
 * purpose (eg, suitable for tracking persisted-LSN, recycle-LSN, etc).
 */
template <class T>
class SimpleNumericCounter : public CounterBase {
 public:
  SimpleNumericCounter(const std::string &name) : CounterBase(name) {
    _last = 0;
    resetInternal();
  }
  ~SimpleNumericCounter() {}

  void setValue(T value) {
    // Atomically increment the count and get the previous value
    uint64_t c = _count.fetch_add(1);

    T min = _min.load();
    T max = _max.load();
    if (min > value || c == 0) {
      _min.store(value);
    }
    if (max < value || c == 0) {
      _max.store(value);
    }
    _last.store(value);
  }

  uint64_t count() const { return _count.load(std::memory_order_relaxed); }

  T min() const { return _min.load(std::memory_order_relaxed); }

  T max() const { return _max.load(std::memory_order_relaxed); }

  T last() const { return _last.load(std::memory_order_relaxed); }

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    NumericCounterData<T> data;
    data.name_length = std::min(_name.size(), sizeof(data.name));
    int ret =
        strncpy_s(data.name, NAME_LENGTH_MAX, _name.c_str(), data.name_length);
    CDE_ASSERT(ret == EOK);
    UNUSED_PARAM(ret);
    data.id = _id;
    if (parent != nullptr) {
      data.parent_id = parent->getId();
    } else {
      data.parent_id = InvalidCounterLibId;
    }
    data.count = count();
    data.min = min();
    data.max = max();
    data.last = last();
    os.write(reinterpret_cast<char *>(&data), sizeof(data) - sizeof(data.name));
    os.write(data.name, data.name_length);
  }

 protected:
  void resetInternal() override {
    _min = 0;
    _max = 0;
    _count = 0;
  }

 private:
  std::atomic<T> _min, _max, _last;
  std::atomic_uint_fast64_t _count;
};

/*
 * Frequently used SimpleNumericCounter template instantiation.
 */
class SimpleNumeric64Counter : public SimpleNumericCounter<int64_t> {
 public:
  SimpleNumeric64Counter(const std::string &name)
      : SimpleNumericCounter<int64_t>(name) {}
  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    uint32_t cls = static_cast<int64_t>(CounterClass::SimpleNumeric64Counter);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));
    SimpleNumericCounter<int64_t>::dump(os, parent);
  }
  ~SimpleNumeric64Counter() {}
};

/*
 * Frequently used SimpleNumericCounter template instantiation.
 */
class SimpleNumericU64Counter : public SimpleNumericCounter<uint64_t> {
 public:
  SimpleNumericU64Counter(const std::string &name)
      : SimpleNumericCounter<uint64_t>(name) {}
  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    uint32_t cls = static_cast<uint64_t>(CounterClass::SimpleNumericU64Counter);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));
    SimpleNumericCounter<uint64_t>::dump(os, parent);
  }
  ~SimpleNumericU64Counter() {}
};

/*
 * Counter that calculates average, minimum and maximum of some value.
 * Because we do not want to have any locking, min and max members can on a
 * very rare occasion miss some values. Average value is always correct.
 */

template <class T>
class NumericCounter : public CounterBase {
 public:
  /*
   * Fast flag decreases overhead of adding new measurement by 20%.
   * The disadvantage of "noMinMax" flag is that  min/max values and
   * incremental counter modifications are unavailable.
   */
  NumericCounter(const std::string &name, bool noMinMax = false)
      : CounterBase(name), _noMinMax(noMinMax), _onlyCnt(false) {
    _last = 0;
    resetInternal();
  }

  /*
   * When a NumericCounter just be used as a simple counter,
   * need to set _onlyCnt to true to avoid displaying invalid information.
   */
  void setOnlyCount(bool value) { _onlyCnt = value; }
  /*
   * When adding new measurement to the counter, call this function.
   * E.g., when a counter represents allocated memory size and a value is
   * total size of allocated memory calculated at the time of function call.
   */
  virtual void addValue(T value) { addValueInt(value, true); }

  /*
   * When adding a _startCount to the counter, call this function.
   */
  void addStartCount() { _startCount.fetch_add(1, std::memory_order_relaxed); }

  /*
   * When you want to add a  measurement that incrementally differs from the
   * previous, call this funtion.
   *
   * Returns old value of a counter (before increment
   *
   * Comments:
   * E.g., when a counter represents allocated memory size and a value is
   * a size of a newly allocated block.
   *
   * This replaces necessity for user to keep the current value of a value
   * being measured in order to pass a new observation to addValue function.
   * In such counters, sum is usually meaningless.
   * Also, because of counters that may use increment/decrement functions,
   * _lastValue cannot be changed during reset() command.
   */
  T increment(T value = 1) {
    T old = _last.fetch_add(value);
    addValueInt(old + value, false);
    return old;
  }

  /*
   * When you want to add a measurement that is incrementally smaller than the
   * previous, call this funtion.
   *
   * Returns old value of a counter (before decrement)
   * Also see comments for increment()
   */
  T decrement(T value = 1) {
    T old = _last.fetch_sub(value);
    addValueInt(old - value, false);
    return old;
  }

  T average() const {
    uint64_t c = _count.load();
    if (c == 0) {
      return 0;
    }
    return _sum.load() / c;
  }

  uint64_t count() const { return _count.load(); }
  uint64_t startCount() const { return _startCount.load(); }

  T min() const { return _min.load(); }

  T max() const { return _max.load(); }

  T sum() const { return _sum.load(); }

  T last() const { return _last.load(); }

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    NumericCounterData<T> data;
    data.name_length = std::min(_name.size(), sizeof(data.name));
    int ret =
        strncpy_s(data.name, NAME_LENGTH_MAX, _name.c_str(), data.name_length);
    CDE_ASSERT(ret == EOK);
    UNUSED_PARAM(ret);
    data.id = _id;
    if (parent != nullptr) {
      data.parent_id = parent->getId();
    } else {
      data.parent_id = InvalidCounterLibId;
    }
    data.noMinMax = _noMinMax;
    data.onlyCnt = _onlyCnt;
    data.average = average();
    data.sum = sum();
    data.startCount = startCount();
    data.count = count();
    data.min = min();
    data.max = max();
    data.last = last();
    os.write(reinterpret_cast<char *>(&data), sizeof(data) - sizeof(data.name));
    os.write(data.name, data.name_length);
  }
  ~NumericCounter() {}

 protected:
  void resetInternal() override {
    _min = 0;
    _max = 0;
    _count = 0;
    _startCount = 0;
    _sum = 0;
  }

 private:
  void addValueInt(T value, bool storeLast) {
    // Atomically increment the count and get the previous value
    uint64_t c = _count.fetch_add(1);
    if (_onlyCnt) {
      return;
    }
    _sum += value;
    if (!_noMinMax) {
      T min = _min.load();
      T max = _max.load();
      if (min > value || c == 0) {
        _min.store(value);
      }
      if (max < value || c == 0) {
        _max.store(value);
      }
      if (storeLast) {
        _last.store(value);
      }
    }
  }

  std::atomic<T> _sum, _min, _max, _last;
  std::atomic_uint_fast64_t _count, _startCount;

 protected:
  bool _noMinMax, _onlyCnt;
};

/*
 * uint32_t NumericalCounter template instantiation.
 */
class Numeric32Counter : public NumericCounter<uint32_t> {
 public:
  Numeric32Counter(const std::string &name, bool noMinMax = false)
      : NumericCounter<uint32_t>(name, noMinMax) {}

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    uint32_t cls = static_cast<uint32_t>(CounterClass::Numeric32Counter);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));
    NumericCounter<uint32_t>::dump(os, parent);
  }
  ~Numeric32Counter() {}
};

/*
 * Frequently used NumericalCounter template instantiation.
 */
class Numeric64Counter : public NumericCounter<int64_t> {
 public:
  Numeric64Counter(const std::string &name, bool noMinMax = false)
      : NumericCounter<int64_t>(name, noMinMax) {}
  ~Numeric64Counter() {}

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    uint32_t cls = static_cast<uint32_t>(CounterClass::Numeric64Counter);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));
    NumericCounter<int64_t>::dump(os, parent);
  }
};

/*
 * Exponential Moving Average (EMA) Counter
 *   S_1 = Y_1
 *   S_t = alpha*Y_t + (1-alpha)*S_(t-1) if t > 1
 *   The coefficient alpha represents the degree of weighting decrease,
 *   a constant smoothing factor between 0 and 1.
 *   A higher alpha discounts older observations faster.
 *   Yt is the value at a time period t.
 *   St is the value of the EMA at any time period t.
 *
 *   In this implementation,
 *   each value is an accumulated value per time interval (e.g. 1 sec)
 *
 *   e.g. it tracks EMA of average latency per second
 *
 */
template <class T>
class EMA_counter : public NumericCounter<T> {
 public:
  /*
   * EMA_counter
   * Parameters:
   *     name - counter name
   *     alpha - co-efficiency, in range of (0,1]
   *     interval - in ms, time interval to triger a EMA computation
   *                default is 1000ms, i.e. 1 sec
   */
  EMA_counter(const std::string &name, float alpha = 1, int interval = 1000)
      : NumericCounter<T>(name, true)  // don't need minMax in counter
  {
    _interval = interval;
    _isStarted = false;
    _isComputingEMA = false;
    // check alpha range (0,1]
    if (alpha <= 0 || alpha > 1.0) {
      alpha = 1;  // use default value
    }
    _alpha = alpha;
    _EMA_avg = 0;
    _EMA_count = 0;
  }
  /*
   * checkTimeForCompute: compute EMA if time interval is reached
   *
   * Parameters:
   *   curTime - current time
   * Returns:
   */
  void checkTimeForCompute(const sal_timep_t &curTime) {
    using namespace std::chrono;
    // check should we finish current interval and compute
    milliseconds ms = duration_cast<milliseconds>(curTime - _intervalStartTime);
    if (ms.count() >= _interval) {
      // need gurantee only one thread can compute
      // other threads just skip this (may lead to small inaccuracy)
      bool expected = false;
      if (_isComputingEMA.compare_exchange_strong(expected, true)) {
        // current interval finished, compute EMA
        // double check the time difference, others may change start time
        ms = duration_cast<milliseconds>(curTime - _intervalStartTime);
        if (ms.count() >= _interval) {
          // compute EMA over avg
          // round may bring some anormaly values in corner case
          _EMA_avg = static_cast<int64_t>(
              _alpha * NumericCounter<T>::average() + (1 - _alpha) * _EMA_avg);
          // compute EMA over count
          _EMA_count = static_cast<uint64_t>(
              _alpha * NumericCounter<T>::count() + (1 - _alpha) * _EMA_count);
          // start a new interval by reset counter
          _intervalStartTime = curTime;
          NumericCounter<T>::resetInternal();
        }
        _isComputingEMA = false;
      }
    }
  }

  /*
   * overloading addValue of base class
   * after insert new value, check whether we need
   * move the SMA to next slot
   */
  void addValue(T value) override {
    sal_timep_t curTime = sal_clock::now();

    if (!_isStarted) {
      // init intervalStartTime using first value's time
      _intervalStartTime = curTime;
      _EMA_avg = value;  // use first value as init value
      _isStarted = true;
    } else {
      checkTimeForCompute(curTime);
    }

    // call base class to add this value to counter
    NumericCounter<T>::addValue(value);
  }

  /*
   * getEMA - get EMA over the avg values of past interval
   *
   */
  int64_t getEMA() {
    if (_isStarted) {
      // check time to see whether we need compute EMA
      sal_timep_t curTime = sal_clock::now();
      checkTimeForCompute(curTime);
    }
    return _EMA_avg;
  }
  /*
   * getEMACount - get EMA over the counts of past interval
   *
   */
  uint64_t getEMACount() {
    if (_isStarted) {
      // check time to see whether we need compute EMA
      sal_timep_t curTime = sal_clock::now();
      checkTimeForCompute(curTime);
    }
    return _EMA_count;
  }

  ~EMA_counter() {}

  // Note: only use as internal counter now, will add dump funciton later

 protected:
  //  accumulate stats in a time interval, unit: milli sec
  //  when the interval time is reached, compute EMA
  //  Note: it is possilbe that infrequent requests lead to
  //        real interval is larger than this defined interval
  int _interval;
  // start time of current interval
  sal_timep_t _intervalStartTime;
  // is intervalStartTime initialized or not
  // first request sets it as true
  std::atomic<bool> _isStarted;
  // is computing EMA ongoing
  // make sure only one thread can do the EMA computation
  std::atomic<bool> _isComputingEMA;
  // EMA over average of values in the interval
  std::atomic<int64_t> _EMA_avg;
  // EMA over counts of values in the interval
  std::atomic<uint64_t> _EMA_count;
  // co-efficient alpha
  float _alpha;
};

class EMANumeric64Counter : public EMA_counter<int64_t> {
 public:
  EMANumeric64Counter(const std::string &name, float alpha = 0.5,
                      int interval = 1000)
      : EMA_counter<int64_t>(name, alpha, interval) {}
  ~EMANumeric64Counter() {}
};

/*
 * EMA Numerical counter that is tuned to calculate time latencies.
 */
class EMALatencyCounter : public EMANumeric64Counter {
 private:
  // shortcut for time
  typedef sal_timep_t Time;

 public:
  /*
   * Base class for EMA timers.
   * Timer produces one measurment for latency counter.
   * This is an abstract class.
   * Concrete implemenation should define method to get current timestamp in
   * nanoseconds.
   * TODO: change Timer class to be template type and remove duplicate code
   */
  class EMATimerBase {
   public:
    EMATimerBase(EMALatencyCounter &c) : _c(c) {
      _started = false;
      _paused = false;
      _accumulated = 0;
    }

    virtual ~EMATimerBase() = default;

   private:
    /* Remembers timer start timestamp */
    virtual void setStartTime() = 0;
    /* Returns difference in ns between start timestamp and current timestamp*/
    virtual uint64_t getDifference() = 0;

   public:
    void start() {
      CDE_ASSERT(_started == false);
      CDE_ASSERT(_paused == false);
      setStartTime();
      _started = true;
      _paused = false;
      _accumulated = 0;
    }

    void end() {
      try {
        if (_started) {
          if (!_paused) {
            _accumulated += getDifference();
          }
          _c.addValue(_accumulated);
          _paused = false;
          _started = false;
        }
      } catch (std::exception &e) {
        std::cerr << "EMATimerBase end failed" << e.what() << std::endl;
      } catch (...) {
        std::cerr << "EMATimerBase end failed with unknown reason" << std::endl;
      }
    }

    void pause() {
      CDE_ASSERT(_started);
      CDE_ASSERT(!_paused);
      _accumulated += getDifference();
      _paused = true;
    }

    void resume() {
      CDE_ASSERT(_paused);
      CDE_ASSERT(_started);
      setStartTime();
      _paused = false;
    }

    void startOrResume() { _paused ? resume() : start(); }

   protected:
    EMALatencyCounter &_c;
    bool _started;
    bool _paused;
    uint64_t _accumulated;
  };

  /*
   * Timer based on standard library time functions.
   * Additional latency introduced by timer
   * creation/counter update/counter destruction. is about 300-500ns.
   * Additional latency introduced by timer pause/resume is about 30-50ns.
   * The advantage of this timer is consistency of measurments.
   * This timer should be used in most of the cases.
   */
  class EMATimer : public EMATimerBase {
   public:
    EMATimer(EMALatencyCounter &c, bool startNow = true) : EMATimerBase(c) {
      if (startNow) {
        start();
      }
    }
    ~EMATimer() noexcept {
      if (_started) {
        end();
      }
    }

   private:
    virtual void setStartTime() override {
      using namespace std::chrono;
      _startTime = sal_clock::now();
    }

    virtual uint64_t getDifference() override {
      using namespace std::chrono;
      nanoseconds ns =
          duration_cast<nanoseconds>(sal_clock::now() - _startTime);
      CDE_ASSERT(ns.count() >= 0);
      return ns.count();
    }
    Time _startTime;
  };

 public:
  EMALatencyCounter(const std::string &name, float alpha = 0.5,
                    int interval = 1000)
      : EMANumeric64Counter(name, alpha, interval) {}
  ~EMALatencyCounter() {}

 private:
  // To hide inherited functions. They do not have meaning for latency
  void increment(int64_t) const {}
  void decrement(int64_t) const {}
};

/**
 *
 * derive class for "sum" function
 */
class NewNumeric64Counter : public NumericCounter<int64_t> {
 public:
  NewNumeric64Counter(const std::string &name, bool noMinMax = false)
      : NumericCounter<int64_t>(name, noMinMax) {}
  ~NewNumeric64Counter() {}

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    uint32_t cls = static_cast<uint32_t>(CounterClass::NewNumeric64Counter);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));
    NumericCounter<int64_t>::dump(os, parent);
  }
};

/*
 * Numerical counter that is tuned to calculate time latencies.
 */
class LatencyCounter : public Numeric64Counter {
 private:
  // shortcut for time
  typedef sal_timep_t Time;

 public:
  /*
   * Base class for timers.
   * Timer produces one measurment for latency counter.
   * This is an abstract class.
   * Concrete implemenation should define method to get current timestamp in
   * nanoseconds.
   */
  class TimerBase {
   public:
    TimerBase(LatencyCounter &c) : _c(c) {
      _started = false;
      _paused = false;
      _accumulated = 0;
    }

    virtual ~TimerBase() = default;

   private:
    /* Remembers timer start timestamp */
    virtual void setStartTime() = 0;
    /* Returns difference in ns between start timestamp and current timestamp*/
    virtual uint64_t getDifference() = 0;

   public:
    void start() {
      CDE_ASSERT(_started == false);
      CDE_ASSERT(_paused == false);
      setStartTime();
      _started = true;
      _paused = false;
      _accumulated = 0;
      _c.addStartCount();
    }

    void end() {
      try {
        if (_started) {
          if (!_paused) {
            _accumulated += getDifference();
          }
          _c.addValue(_accumulated);
          _paused = false;
          _started = false;
        }
      } catch (std::exception &e) {
        std::cerr << "Timer end failed" << e.what() << std::endl;
      } catch (...) {
        std::cerr << "Timer end failed with unknown reason" << std::endl;
      }
    }

    void pause() {
      CDE_ASSERT(_started);
      CDE_ASSERT(!_paused);
      _accumulated += getDifference();
      _paused = true;
    }

    void resume() {
      CDE_ASSERT(_paused);
      CDE_ASSERT(_started);
      setStartTime();
      _paused = false;
    }

    void startOrResume() { _paused ? resume() : start(); }

   protected:
    LatencyCounter &_c;
    bool _started;
    bool _paused;
    uint64_t _accumulated;
  };

  /*
   * Timer based on standard library time functions.
   * Additional latency introduced by timer
   * creation/counter update/counter destruction. is about 300-500ns.
   * Additional latency introduced by timer pause/resume is about 30-50ns.
   * The advantage of this timer is consistency of measurments.
   * This timer should be used in most of the cases.
   */
  class Timer : public TimerBase {
   public:
    Timer(LatencyCounter &c, bool startNow = true) : TimerBase(c) {
      if (startNow) {
        start();
      }
    }
    ~Timer() noexcept {
      if (_started) {
        end();
      }
    }

   private:
    virtual void setStartTime() override {
      using namespace std::chrono;
      _startTime = sal_clock::now();
    }

    virtual uint64_t getDifference() override {
      using namespace std::chrono;
      nanoseconds ns =
          duration_cast<nanoseconds>(sal_clock::now() - _startTime);
      CDE_ASSERT(ns.count() >= 0);
      return ns.count();
    }
    Time _startTime;
  };

  // TODO(alx): Add timer based on rdtsc for short intervals (less than 500ns)
  // class FastTimer : public TimerBase {

 public:
  LatencyCounter(const std::string &name, bool fast = false)
      : Numeric64Counter(name, fast) {}
  ~LatencyCounter() {}
  /*
   * Prints this counter to a stream in text format.
   *
   * ss     - string stream to which counters will be print
   */
  void dump(std::stringstream &ss) const {
    uint64_t a = average();
    int factor = 1;
    std::string units = "ns";

    if (a > 10000000ULL) {
      factor = 1000000ULL;
      units = "ms";
    } else if (a > 10000ULL) {
      factor = 1000ULL;
      units = "us";
    }

    if (!_noMinMax) {
      ss << _name << " id=" << _id
         << " : avg/min/max/sum=" << average() / factor << '/' << min() / factor
         << '/' << max() / factor << '/' << sum() / factor << units
         << " startCnt=" << startCount() << " cnt=" << count() << std::endl;
    } else {
      ss << _name << " id=" << _id << " : avg/sum=" << average() / factor << '/'
         << sum() / factor << units << " startCnt=" << startCount()
         << " cnt=" << count() << std::endl;
    }
  }

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    uint32_t cls = static_cast<uint32_t>(CounterClass::LatencyCounter);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));
    NumericCounter<int64_t>::dump(os, parent);
  }

 private:
  // To hide inherited functions. They do not have meaning for latency
  void increment(int64_t) const {}
  void decrement(int64_t) const {}
};

/*
 * Numerical counter that is tuned to calculate time latencies.
 * Difference between this and LatencyCounter is that this allows
 * concurrent threads to try to start the timer in a consistent way.
 * If multiple threads can call start() & end() in out-of-order,
 * the onus is on the caller to make sure start() is called
 * before end(), otherwise the accumulated time is undefined.
 * For simplicity ConcurLatencyCounter doesn't support pause/resume.
 */
class ConcurLatencyCounter : public Numeric64Counter {
 private:
  // shortcut for time
  typedef sal_timep_t Time;

 public:
  /*
   * Base class for timers.
   * Timer produces one measurment for latency counter.
   * This is an abstract class.
   * Concrete implemenation should define method to get current timestamp in
   * nanoseconds.
   */
  class TimerBase {
   public:
    TimerBase(ConcurLatencyCounter &c) : _c(c) {
      _started = false;
      _accumulated = 0;
    }

    virtual ~TimerBase() = default;

   private:
    /* Remembers timer start timestamp */
    virtual void setStartTime() = 0;
    /* Returns difference in ns between start timestamp and current timestamp*/
    virtual uint64_t getDifference() = 0;

   public:
    void start() {
      bool expected = false;
      if (_started.compare_exchange_strong(expected, true)) {
        setStartTime();
        _accumulated = 0;
      }
    }

    void end() {
      try {
        bool expected = true;
        if (_started.compare_exchange_strong(expected, false)) {
          _accumulated += getDifference();
          _c.addValue(_accumulated);
        }
      } catch (std::exception &e) {
        std::cerr << "Timer end failed" << e.what() << std::endl;
      } catch (...) {
        std::cerr << "Timer end failed with unknown reason" << std::endl;
      }
    }

   protected:
    ConcurLatencyCounter &_c;
    std::atomic<bool> _started;
    std::atomic<uint64_t> _accumulated;
  };

  /*
   * Timer based on standard library time functions.
   * Additional latency introduced by timer
   * creation/counter update/counter destruction. is about 300-500ns.
   * Additional latency introduced by timer pause/resume is about 30-50ns.
   * The advantage of this timer is consistency of measurments.
   * This timer should be used in most of the cases.
   */
  class Timer : public TimerBase {
   public:
    Timer(ConcurLatencyCounter &c, bool startNow = true) : TimerBase(c) {
      if (startNow) {
        start();
      }
    }
    ~Timer() noexcept {
      if (_started) {
        end();
      }
    }

   private:
    virtual void setStartTime() override {
      using namespace std::chrono;
      _startTime = sal_clock::now();
    }

    virtual uint64_t getDifference() override {
      using namespace std::chrono;
      nanoseconds ns =
          duration_cast<nanoseconds>(sal_clock::now() - _startTime);
      CDE_ASSERT(ns.count() >= 0);
      return ns.count();
    }
    Time _startTime;
  };

 public:
  ConcurLatencyCounter(const std::string &name, bool fast = false)
      : Numeric64Counter(name, fast) {}
  ~ConcurLatencyCounter() {}
  /*
   * Prints this counter to a stream in text format.
   *
   * ss     - string stream to which counters will be print
   */
  void dump(std::stringstream &ss) const {
    uint64_t a = average();
    int factor = 1;
    std::string units = "ns";

    if (a > 10000000ULL) {
      factor = 1000000ULL;
      units = "ms";
    } else if (a > 10000ULL) {
      factor = 1000ULL;
      units = "us";
    }

    if (!_noMinMax) {
      ss << _name << " id=" << _id
         << " : avg/min/max/sum=" << average() / factor << '/' << min() / factor
         << '/' << max() / factor << '/' << sum() / factor << units
         << " cnt=" << count() << std::endl;
    } else {
      ss << _name << " id=" << _id << " : avg/sum=" << average() / factor << '/'
         << sum() / factor << units << " cnt=" << count() << std::endl;
    }
  }

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    uint32_t cls = static_cast<uint32_t>(CounterClass::ConcurLatencyCounter);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));
    NumericCounter<int64_t>::dump(os, parent);
  }

 private:
  // To hide inherited functions. They do not have meaning for latency
  void increment(int64_t) const {}
  void decrement(int64_t) const {}
};

/*
 * Counter that produces histogram. It results in printing value distribution
 * over the give range split into equal size buckets
 */
template <class T>
class HistogramCounter : public CounterBase {
 public:
  /*
   * Function HistogramCounter - construction of Histogram counter
   *
   * Parameters:
   *  name (IN) - name of the counter
   *  bucketSize - size of a single bucket (all buckets have the same size)
   *  min - top value of the first bucket (bottom value is min(typeof(T))
   *  max - bottom value of the last bucket (top value is max(typeof(T))
   */
  HistogramCounter(const std::string &name, T bucketSize, T min, T max)
      : CounterBase(name), _histo(bucketSize, min, max) {
    resetInternal();
  }
  ~HistogramCounter() {}
  /* Add a data point to the histogram */
  virtual void addValue(T value) { _histo.addValue(value); }

  /*
   * Prints this counter to a stream in binary format.
   *
   * ss     - output stream to which counters will be print
   */
  virtual void dump(std::stringstream &ss) const {
    unsigned int numBuckets = getNumBuckets();
    ss << _name << " id=" << _id << " :" << std::endl;
    for (unsigned int n = 0; n < numBuckets; ++n) {
      if (n != 0) {
        ss << "\t[" << getBucketMinByIdx(n);
      } else {
        ss << "\tless then ";
      }

      if (n != 0 && n != numBuckets - 1) {
        ss << " - ";
      }
      if (n != numBuckets - 1) {
        ss << getBucketMaxByIdx(n) << ")";
      } else {
        ss << " or more";
      }
      ss << ": " << getBucketCountByIdx(n) << std::endl;
    }
  }

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    HistogramCounterHeader header;
    header.id = _id;
    header.name_length = std::min(_name.size(), sizeof(header.name));
    int ret = strncpy_s(header.name, sizeof(header.name), _name.c_str(),
                        header.name_length);
    CDE_ASSERT(ret == EOK);
    UNUSED_PARAM(ret);
    if (parent != nullptr) {
      header.parent_id = parent->getId();
    } else {
      header.parent_id = InvalidCounterLibId;
    }
    header.numBuckets = getNumBuckets();
    os.write(reinterpret_cast<char *>(&header),
             sizeof(header) - sizeof(header.name));
    os.write(header.name, header.name_length);

    for (unsigned int n = 0; n < header.numBuckets; ++n) {
      HistogramCounterData<T> data;
      data.bucketMin = getBucketMinByIdx(n);
      data.bucketMax = getBucketMaxByIdx(n);
      data.bucketCount = getBucketCountByIdx(n);
      os.write(reinterpret_cast<char *>(&data), sizeof(data));
    }
  }

  virtual T bucketSize() const { return _histo.getBucketSize(); }

  virtual T min() const { return _histo.getMin(); }

  virtual T max() const { return _histo.getMax(); }

  void toTabSeparatedValues(std::ostream &os,
                            bool skipEmptyBuckets = true) const {
    _histo.toTSV(os, skipEmptyBuckets);
  }

  /*
   * merges the "other" histogram with the current histogram
   * The current histogram adds values from "other"
   */
  void merge(const HistogramCounter<T> &other) { _histo.merge(other._histo); }

  virtual uint16_t getNumBuckets() const { return _histo.getNumBuckets(); }
  virtual uint64_t getBucketCountByIdx(uint16_t idx) const {
    return _histo.getBucketByIndex(idx).count;
  }
  virtual T getBucketMinByIdx(uint16_t idx) const {
    return _histo.getBucketMin(idx);
  }
  virtual T getBucketMaxByIdx(uint16_t idx) const {
    return _histo.getBucketMax(idx);
  }
  virtual void clear() { _histo.clear(); }

 protected:
  virtual void resetInternal() override { this->clear(); }

  folly::Histogram<T> _histo;
};

/*
 * Frequently used HistogramCounter template instantiation for int64_t.
 */
class Histogram64Counter : public HistogramCounter<int64_t> {
 public:
  using HistogramCounter<int64_t>::dump;
  Histogram64Counter(const std::string &name, int64_t bucketSize, int64_t min,
                     int64_t max)
      : HistogramCounter<int64_t>(name, bucketSize, min, max) {}

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    uint32_t cls = static_cast<uint32_t>(CounterClass::Histogram64Counter);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));
    HistogramCounter<int64_t>::dump(os, parent);
  }
  ~Histogram64Counter() {}
};

/*
 * Logarithmic histogam counter for integer values
 * Each bucket size is twice as large as previous
 */
class Histogram64LogCounter : public Histogram64Counter {
 public:
  Histogram64LogCounter(const std::string &name, int64_t bucketSize,
                        int64_t min, int64_t max)
      : Histogram64Counter(name, bucketSize, transform(min), transform(max)) {}

  void addValue(int64_t value) override {
    Histogram64Counter::addValue(transform(value));
  }
  ~Histogram64LogCounter() {}

  int64_t min() const override {
    return transformBack(Histogram64Counter::min());
  }

  int64_t max() const override {
    return transformBack(Histogram64Counter::max());
  }

  int64_t getBucketMinByIdx(uint16_t idx) const override {
    return transformBack(Histogram64Counter::getBucketMinByIdx(idx));
  }

  int64_t getBucketMaxByIdx(uint16_t idx) const override {
    return transformBack(Histogram64Counter::getBucketMaxByIdx(idx));
  }

  static inline int32_t log2(const uint64_t x) {
    if (x == 0) {
      return 0;
    }

    return (sizeof(x) * 8 - 1 - __builtin_clzl(x));
  }

  int64_t transform(int64_t v) const { return log2(v); }

  int64_t transformBack(int64_t v) const { return 1 << (uint64_t)v; }

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os,
                    CounterCatalogItem *parent) const override {
    uint32_t cls = static_cast<uint32_t>(CounterClass::Histogram64LogCounter);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));
    HistogramCounter<int64_t>::dump(os, parent);
  }
  /*
   * Prints this counter to a stream in text format.
   *
   * ss     - output stream to which counters will be print
   */
  virtual void dump(std::stringstream &ss) const override {
    HistogramCounter<int64_t>::dump(ss);
  }
};

/*
 * This class represents a node in a tree hierarchy of objects that contains
 * performance counters.
 */

class CounterNode : public CounterCatalogItem {
 public:
  CounterNode(const std::string &name)
      : CounterCatalogItem(name),
        _parent(NullNodeHandle),
        _self(NullNodeHandle) {}

  /*
   * Prints this counter to a stream in binary format.
   *
   * os     - output stream to which counters will be print
   * parent - the parent node
   */
  virtual void dump(std::ostream &os, CounterNode *parent) const {
    uint32_t cls = static_cast<uint32_t>(CounterClass::CounterNode);
    os.write(reinterpret_cast<char *>(&cls), sizeof(cls));

    CounterNodeData data;
    data.id = _id;
    data.name_length = std::min(_name.size(), sizeof(data.name));
    int ret =
        strncpy_s(data.name, NAME_LENGTH_MAX, _name.c_str(), data.name_length);
    CDE_ASSERT(ret == EOK);
    UNUSED_PARAM(ret);
    if (parent != nullptr) {
      data.parent_id = parent->getId();
    } else {
      data.parent_id = InvalidCounterLibId;
    }
    os.write(reinterpret_cast<char *>(&data), sizeof(data) - sizeof(data.name));
    os.write(data.name, data.name_length);
  }

  CounterNodeHandle getHandle() const { return _self; }
  ~CounterNode() {}

 public:
  CounterNodeHandle _parent;
  CounterIdNodeMap _children;
  CounterNodeHandle _self;
  CounterSet _counters;

  friend class CounterCatalog;
};

/*
 * This class is a singleton repository of all counters in the process.
 * It is responsible to maintain structure of counters and retrieving
 * counter information.
 */

class CounterCatalog {
 public:
  static CounterCatalog *Get();

  void registerNode(const CounterNodeHandle h, const std::string &name);
  void unregisterNode(const CounterNodeHandle h);

  void registerChild(const CounterNodeHandle child,
                     const CounterNodeHandle parent);

  void registerCounter(const CounterNodeHandle h, CounterBase *counter);
  void unregisterCounter(const CounterNodeHandle h, CounterBase *counter);

#ifdef ENABLE_ODD_MEMDUMP
  // TODO(krm)
  //  This may not be a good place to place [un]registerDumpMem
  //  1. Move to .cc?
  //  2. Extend CounterCatalog?
  void registerDumpMem(
      const DumpMemHandle h,          // pointer to the class
      const std::string &namekey,     // Unique ID (class name or unique string)
      DumpMemCB cb,                   // Callback function
      const std::string &nametype) {  // Class name as string
    std::lock_guard<std::mutex> lock(_mutex);
    _dumpMemHandleMap[namekey] = h;
    _dumpMemCBMap[namekey] = cb;
    _dumpMemTypeMap[namekey] = nametype;
  }
  void unregisterDumpMem(std::string namekey) {
    std::lock_guard<std::mutex> lock(_mutex);
    _dumpMemHandleMap.erase(namekey);
    _dumpMemCBMap.erase(namekey);
    _dumpMemTypeMap.erase(namekey);
  }

  char *dumpmem(std::string namekey, DumpMemoArchive &oar) {
    // Get cb (callback function) and dmhit (Class pointer)
    DumpMemCB cb;
    DumpMemHandle dmh;
    DumpMemCBMap::const_iterator cbit = _dumpMemCBMap.find(namekey);
    DumpMemHandleMap::const_iterator dmhit = _dumpMemHandleMap.find(namekey);

    //  TODO:(krm)
    //  Must always find a cb and dmhit -> assert this never happens
    if (cbit == _dumpMemCBMap.end() || dmhit == _dumpMemHandleMap.end()) {
      /*
      int *zerosize = new int;
      *zerosize = 0;
      return (char *)zerosize;
      */
      CDE_ASSERT(false);
      return nullptr;
    }
    cb = cbit->second;
    dmh = dmhit->second;

    // cb is the static class member responsible for encode..
    // Given an instance pointer + ouput archive (boost:oarchive)
    // it will serialize the members into it.
    cb(dmh, oar);
    return nullptr;
  }

  char *dumpmemall() {
    // Create two new files: mofs and ofs
    //   mofs: holds the metadata
    //   ofs: holds the memory dump data
    std::ofstream mofs(DUMP_MET_FNAME, std::ios::trunc);
    // Mark the file, by starting it with a special character
    // to avoid confusion
    mofs.put(dumpMemFileTypeCode::metafile);

    std::ofstream ofs(DUMP_MEM_FNAME, std::ios::trunc);
    // Mark the file, by starting it with a different character
    ofs.put(dumpMemFileTypeCode::datafile);

    // Create boost out archive by passing ofs -> boost::oarchive
    DumpMemoArchive oar(ofs);

    // To dump all, we iterate any of the Maps.
    for (auto const &it : _dumpMemTypeMap) {
      dumpmem(it.first, oar);
      // Print metadata to file in the following format:
      // |ClassName| |size(UniqID)| |UniqID| |Last offset of dump|
      mofs << " " << it.second << " " << it.first.size() << " " << it.first
           << " " << ofs.tellp();
    }

    ofs.close();
    mofs.close();

    return nullptr;
  }
#endif

  void dump(std::ostream &os, PublisherFilter *filter) const;

  void dumpRootNode(std::ostream &os, PublisherFilter *filter) const;

  void reset(PublisherFilter *filter);

  void listCounterIds(std::ostream &os, PublisherFilter *filter) const;
  ~CounterCatalog() {}

 private:
  CounterCatalog();
  CounterNodePtr getNode(const CounterNodeHandle h, bool create);
  CounterCatalogItem *getItem(CounterLibID cid) const;
  CounterCatalogItem *getItem(const std::string &name) const;
  void unregisterChildNodes(const CounterNodePtr &node);
  void dump(CounterNode *node, std::ostream &os, CounterNode *parent,
            PublisherFilter *filter) const;
  void reset(const CounterNodePtr &node);
  void listCounterIds(CounterNode *node, std::ostream &os,
                      const std::string &prefix, PublisherFilter *filter) const;
  void addToIdMap(CounterCatalogItem *ci, CatalogIDMap &);
  void removeFromIdMap(CounterCatalogItem *ci, CatalogIDMap &);
  std::vector<CounterCatalogItem *> processFilter(
      PublisherFilter *filter) const;

  /* all nodes, key - handle, value - node */
  CounterNodeMap _allNodes;
  /* root nodes, key - unique Id, value - node */
  CounterIdNodeMap _rootNodes;
  /* mutex that protects _allNodes and _rootNodes */
  mutable std::mutex _mutex;
  /*
   * id that will be assigned to the next registered node or counter.
   * id is unique as long as process runs, but will be reused after restart
   */
  std::atomic<CounterLibID> _nextID;
  /* all nodes, key - unique Id, value node */
  CatalogIDMap _idNodeMap;
  /* all counters, key - unique Id, value counter */
  CatalogIDMap _idCounterMap;

#ifdef ENABLE_ODD_MEMDUMP
  /* all dump memory call back functions, key - unique Id, value - CB function
   */
  DumpMemCBMap _dumpMemCBMap;
  DumpMemHandleMap _dumpMemHandleMap;
  DumpMemTypeMap _dumpMemTypeMap;
#endif
};

/*
 * This wrapper class is used for automatic registering and deregistering
 * counter nodes.
 * Add an instance of this class as a member variable to a class you want
 * to register with the CounterCatalog.
 */
class CounterNodeRegistration {
  /*
   * Default copy of CounterNodeRegistration will not work, since each
   * owner has to make calls to CounterCatalog to register itself.
   * Therefore, default copy/move constructor/assignment are disabled.
   */
  CounterNodeRegistration(const CounterNodeRegistration &) = delete;
  CounterNodeRegistration(CounterNodeRegistration &&) = delete;
  CounterNodeRegistration &operator=(CounterNodeRegistration &&) = delete;
  CounterNodeRegistration &operator=(const CounterNodeRegistration &) = delete;

 public:
  CounterNodeRegistration(const CounterNodeHandle h, const std::string name)
      : CounterNodeRegistration() {
    init(h, name);
  }

  CounterNodeRegistration() {
    _cc = CounterCatalog::Get();
    _h = NullNodeHandle;
  }
  void init(const CounterNodeHandle h, const std::string name) {
    _cc->registerNode(h, name);
    _h = h;
  }

  ~CounterNodeRegistration() noexcept { unregisterNode(); }

  void registerCounter(CounterBase *counter) {
    _cc->registerCounter(_h, counter);
  }

  void unregisterCounter(CounterBase *counter) {
    _cc->unregisterCounter(_h, counter);
  }

#ifdef ENABLE_ODD_MEMDUMP
  void registerDumpMem(const DumpMemHandle h, const std::string &namekey,
                       DumpMemCB cb, const std::string &nametype) {
    _cc->registerDumpMem(h, namekey, cb, nametype);
  }

  void unregisterDumpMem(std::string namekey) {
    _cc->unregisterDumpMem(namekey);
  }
#endif

  void unregisterNode() {
    try {
      if (_h != NullNodeHandle) {
        _cc->unregisterNode(_h);
        _h = NullNodeHandle;
      }
    } catch (std::exception &e) {
      std::cerr << "~CounterNodeRegistration failed" << e.what() << std::endl;
    } catch (...) {
      std::cerr << "~CounterNodeRegistration failed with unknown reason"
                << std::endl;
    }
  }

  void registerChild(const CounterNodeHandle child) {
    _cc->registerChild(child, _h);
  }

  void registerChildOf(const CounterNodeHandle parent) {
    _cc->registerChild(_h, parent);
  }

 private:
  CounterNodeHandle _h;
  CounterCatalog *_cc;
};

/*
 * The macros below allow you to create, register and unregister counters in
 * convenient way. They are not mandatory to use.
 * Usage:
 * 1.
 * In the class that has counters define the following macro:
 * #define SLICE_COUNTERS_DEF(_def)                     \
 *    _def(Latency, writeLatency, WriteLatency)         \
 *    _def(Event, writeFrequency, WriteFrequency)       \
 *    _def(Numeric64, bytesWritten, BytesWritten)
 *
 * The first colum is type of counter (latency, event, numeric, etc....)
 * The second one is variable name that this counter will correspond to.
 *    This name is to be used when updating counter
 * The third column contains the human-readable name of the counter
 *
 * 2. In the class body put:
 *      SLICE_COUNTERS_DEF(COUNTER_LIST_DECL)
 *      CounterNodeRegistration _ccReg;
 * 3. In the class constructor initialization list put
 *      SLICE_COUNTERS_DEF(COUNTER_LIST_INIT)
 * 4. In the class constructor body put
 *      SLICE_COUNTERS_DEF(COUNTER_LIST_REG)
 *
 * For example:
 *
 * // Slice.h
 * #include "perfcounters.h"
 * class Slice {
 *      #define SLICE_COUNTERS_DEF(_def)                     \
 *          _def(Latency, writeLatency, WriteLatency)         \
 *          _def(Event, writeFrequency, WriteFrequency)       \
 *          _def(Numeric64, bytesWritten, BytesWritten)
 *  private:
 *      SLICE_COUNTERS_DEF(COUNTER_LIST_DECL);
 *      CounterNodeRegistration _ccReg;
 *  public:
 *      Slice() :
 *          SLICE_COUNTERS_DEF(COUNTER_LIST_INIT), _ccReg(this, getSliceName())
 *      {
 *          SLICE_COUNTERS_DEF(COUNTER_LIST_REG);
 *      }
 *
 * The code above declares 3 pefromance counters for each slice, in the code
 * they will be called: _writeLatencyCounter, _writeFrequencyCounter, and
 * _bytesWrittenCounter
 * Each Slice instance will be represented by a separate counter registry node
 * All counters and nodes will be automatically registered/unregistered as
 * slices are being created and destroyed.
 *
 */
#define COUNTER_LIST_DECL(type, var, name) Huawei::Common::type##Counter _##var;

#define COUNTER_LIST_INIT(type, var, name) _##var(#name),

#define COUNTER_LIST_REG(type, var, name) _ccReg.registerCounter(&_##var);

#define COUNTER_LIST_DEREG(type, var, name) _ccReg.unregisterCounter(&_##var);

#define COUNTER_LIST_SET_ONLY_COUNT(var) _##var.setOnlyCount(true);

};  // namespace Common

};  // namespace Huawei

#endif  //__PERFCOUNTERS_H__
