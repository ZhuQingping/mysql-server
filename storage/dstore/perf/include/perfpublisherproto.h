/********************************************************************
 * Copyright (C) Huawei Technologies, 2018
 * Performance counters framework: protocol between perfclient and publisher
 *
 *  Contains declaration of structures that client and publisher exchange
 *  between each other. This is independent of specific network transport
 ********************************************************************/

#ifndef __PERFPUBLISHERPROTO_H_
#define __PERFPUBLISHERPROTO_H_

#include "perfcounters.h"

#define NAME_LENGTH_MAX 128

namespace Huawei {
namespace Common {

/*
 * Command that client sends to publisheri
 */
// clang-format off
enum class PublisherCommand : uint32_t {
  DumpFullTree = 0,    // Request all counters of all nodes in text fromat
  ResetFullTree,       // reset all counters of all nodes
  ListCounterIds,      // Request all counter IDs of all nodes in text fromat
  DumpMem,             // Dump a in memory value or data structure
  MemStats,            // Stats the memory use by the process
  JeMemStats,          // Stats the Jemalloc use by the process
  DumpSliceSpaceUsage, // dump slice space usage
  DumpRootNode,        // Request counters of specify root node in specify format
  DumpAlarm,           // dump all alarm norms
  UpdateConfig,        // update slicestore configure parameter
  DumpConfig,          // dump slicestore configure parameter
  ShowIndexTable,      // dump index table
  ShowSampleTable,      // dump index table
  InvalidCommand       // should be the last one in the enum
};
// clang-format on

enum class PublisherResponseCode : uint32_t {
  Ok = 0,
  Error,
};

enum class PublisherFilterField : uint32_t { None = 0, Id, Name };

enum class PublisherFilterOperation : uint32_t { None = 0, Equal, Unequal };

struct PublisherFilter {
  PublisherFilterField field;
  PublisherFilterOperation op;
  char value[255];
};

/*
 * All types of counter
 */
enum class CounterClass : uint32_t {
  EventCounter = 0,
  Numeric64Counter,
  Numeric32Counter,
  SimpleNumeric64Counter,
  SimpleNumericU64Counter,
  LatencyCounter,
  ConcurLatencyCounter,
  Histogram64Counter,
  Histogram64LogCounter,
  CounterNode,
  RplLatencyCounter,
  RplEventCounter,
  PbsLatencyCounter,
  StringNameCounter,
  NewNumeric64Counter
};

/*
 * EventCounter payload
 */
struct EventCounterData {
  uint64_t id;
  uint64_t parent_id;
  uint16_t name_length;
  float frequency;
  uint64_t count;
  // NOTE(hb): the protocol requires name to be the last field
  char name[NAME_LENGTH_MAX];
};

/*
 * StringNameCounter payload
 */
struct StringNameCounterData {
  uint64_t id;
  uint64_t parent_id;
  uint16_t value_name_length;
  uint16_t name_length;
  char value_name[NAME_LENGTH_MAX];
  // NOTE(hb): the protocol requires name to be the last field
  char name[NAME_LENGTH_MAX];
};

/*
 * RplEventCounter payload
 */
struct RplEventCounterData {
  uint64_t id;
  uint64_t parent_id;
  uint16_t name_length;
  uint64_t count;
  uint64_t start_lsn;
  uint64_t end_lsn;
  float time;
  float frequency;
  // NOTE(hb): the protocol requires name to be the last field
  char name[NAME_LENGTH_MAX];
};

/*
 * NumericCounter payload
 */
template <typename T>
struct NumericCounterData {
  uint64_t id;
  uint64_t parent_id;
  uint16_t name_length;
  bool noMinMax;
  bool onlyCnt;
  T average;
  T min;
  T max;
  T sum;
  T last;
  uint64_t startCount;
  uint64_t count;
  // NOTE(hb): the protocol requires name to be the last field
  char name[NAME_LENGTH_MAX];
};

/*
 * HistogramCounter header
 */
struct HistogramCounterHeader {
  uint64_t id;
  uint64_t parent_id;
  uint16_t name_length;
  uint16_t numBuckets;
  // NOTE(hb): the protocol requires name to be the last field
  char name[NAME_LENGTH_MAX];
};

/*
 * HistogramCounter bucket data
 */
template <typename T>
struct HistogramCounterData {
  T bucketMin;
  T bucketMax;
  uint64_t bucketCount;
};

/*
 * CounterNode payload
 */
struct CounterNodeData {
  uint64_t id;
  uint64_t parent_id;
  uint16_t name_length;
  // NOTE(hb): the protocol requires name to be the last field
  char name[NAME_LENGTH_MAX];
};

/*
 * Request that client sends to the publisher
 * Command specific arguments may follow
 */
struct PublisherRequest {
  PublisherCommand cmd;
  PublisherFilter filter;
  uint32_t size;
};

struct ResetResponse {
  PublisherResponseCode result;
};

}  // namespace Common
}  // namespace Huawei

#endif
