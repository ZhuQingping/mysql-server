/********************************************************************
 * Copyright (C) Huawei Technologies, 2018
 * Performance counters: client application
 * Client application to retrieve performance counters
 ********************************************************************/

#include "perfcounters.h"
#include "perfpublisher.h"
#include "publishercommon.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <netdb.h>
#include <stdio.h>
#include <boost/program_options.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace Huawei::Common;
namespace po = boost::program_options;
namespace pt = boost::property_tree;

/*
 * Prints usage of the application
 */
void usage() {
  printf(
      "Usage:\n"
      "perfclient <command> <server name>  <port>\n"
      "Command is: \n"
      "\t reset - resets counters\n"
      "\t dump  - dumps all counters\n");
}

class EventCounterClient {
 public:
  EventCounterClient(const EventCounterData &data) : _data(data) {}

  void dump(int depth) {
    std::string indent(depth, '\t');
    std::cout << indent;
    std::cout << _data.name << " id=" << _data.id
              << " : freq=" << _data.frequency << "/sec. cnt=" << _data.count
              << std::endl;
  }
  ~EventCounterClient() {}

 protected:
  EventCounterData _data;
};

class RplEventCounterClient {
 public:
  RplEventCounterClient(const RplEventCounterData &data) : _data(data) {}

  void dump(int depth) {
    using namespace std::chrono;
    using namespace std;
    string indent(depth, '\t');
    float gap = (_data.end_lsn - _data.start_lsn) / (1024.0f * 1024.0f);
    cout << indent << _data.name << ": time=" << setiosflags(ios::fixed)
         << setprecision(3) << _data.time << "(sec)"
         << " freq=" << _data.frequency << "/sec. cnt=" << _data.count
         << " lsn_gap=" << setiosflags(ios::fixed) << setprecision(4) << gap
         << "MB"
         << " start_lsn=" << _data.start_lsn << " end_lsn=" << _data.end_lsn
         << endl;
  }
  ~RplEventCounterClient() {}

 protected:
  RplEventCounterData _data;
};

class StringNameCounterClient {
 public:
  StringNameCounterClient(const StringNameCounterData &data) : _data(data) {}

  void dump(int depth) {
    using namespace std::chrono;
    using namespace std;
    string indent(depth, '\t');
    cout << indent << _data.name << " id=" << _data.id
         << " : value=" << _data.value_name << endl;
  }
  ~StringNameCounterClient() {}

 protected:
  StringNameCounterData _data;
};

template <typename T>
class NumericCounterClient {
 public:
  NumericCounterClient(const NumericCounterData<T> &data) : _data(data) {}

  void dump(int depth) {
    std::string indent(depth, '\t');
    std::cout << indent;
    if (!_data.noMinMax && !_data.onlyCnt) {
      std::cout << _data.name << " id=" << _data.id
                << " : avg/min/max/last=" << _data.average << '/' << _data.min
                << '/' << _data.max << '/' << _data.last
                << " cnt=" << _data.count << std::endl;
    } else if (!_data.onlyCnt) {
      std::cout << _data.name << " id=" << _data.id
                << " : avg=" << _data.average << " cnt=" << _data.count
                << std::endl;
    } else {
      std::cout << _data.name << " id=" << _data.id << " cnt=" << _data.count
                << std::endl;
    }
  }
  ~NumericCounterClient() {}

 protected:
  NumericCounterData<T> _data;
};

template <typename T>
class SimpleNumericCounterClient {
 public:
  SimpleNumericCounterClient(const NumericCounterData<T> &data) : _data(data) {}

  void dump(int depth) {
    std::string indent(depth, '\t');
    std::cout << indent;
    std::cout << _data.name << " id=" << _data.id
              << " : min/max/last=" << _data.min << '/' << _data.max << '/'
              << _data.last << " cnt=" << _data.count << std::endl;
  }
  ~SimpleNumericCounterClient() {}

 protected:
  NumericCounterData<T> _data;
};

template <typename T>
class NewNumericCounterClient {
 public:
  NewNumericCounterClient(const NumericCounterData<T> &data) : _data(data) {}

  void dump(int depth) {
    std::string indent(depth, '\t');
    std::cout << indent;
    if (!_data.noMinMax && !_data.onlyCnt) {
      std::cout << _data.name << " id=" << _data.id
                << " : avg/min/max/sum/last=" << _data.average << '/'
                << _data.min << '/' << _data.max << '/' << _data.sum << '/'
                << _data.last << " cnt=" << _data.count << std::endl;
    } else if (!_data.onlyCnt) {
      std::cout << _data.name << " id=" << _data.id
                << " : avg=" << _data.average << " cnt=" << _data.count
                << std::endl;
    } else {
      std::cout << _data.name << " id=" << _data.id << " cnt=" << _data.count
                << std::endl;
    }
  }
  ~NewNumericCounterClient() {}

 protected:
  NumericCounterData<T> _data;
};

class LatencyCounterClient {
 public:
  LatencyCounterClient(const NumericCounterData<int64_t> &data) : _data(data) {}

  void dump(int depth) {
    std::string indent(depth, '\t');
    std::cout << indent;
    uint64_t a = _data.average;
    int factor = 1;
    std::string units = "ns";

    if (a > 10000000ULL) {
      factor = 1000000ULL;
      units = "ms";
    } else if (a > 10000ULL) {
      factor = 1000ULL;
      units = "us";
    }

    if (!_data.noMinMax) {
      std::cout << _data.name << " id=" << _data.id
                << " : avg/min/max/sum=" << _data.average / factor << '/'
                << _data.min / factor << '/' << _data.max / factor << '/'
                << _data.sum / factor << units << " start=" << _data.startCount
                << " cnt=" << _data.count << std::endl;
    } else {
      std::cout << _data.name << " id=" << _data.id
                << " : avg/sum=" << _data.average / factor << '/'
                << _data.sum / factor << units << " start=" << _data.startCount
                << " cnt=" << _data.count << std::endl;
    }
  }
  ~LatencyCounterClient() {}

 protected:
  NumericCounterData<int64_t> _data;
};

class ConcurLatencyCounterClient {
 public:
  ConcurLatencyCounterClient(const NumericCounterData<int64_t> &data)
      : _data(data) {}

  void dump(int depth) {
    std::string indent(depth, '\t');
    std::cout << indent;
    uint64_t a = _data.average;
    int factor = 1;
    std::string units = "ns";

    if (a > 10000000ULL) {
      factor = 1000000ULL;
      units = "ms";
    } else if (a > 10000ULL) {
      factor = 1000ULL;
      units = "us";
    }

    if (!_data.noMinMax) {
      std::cout << _data.name << " id=" << _data.id
                << " : avg/min/max/sum=" << _data.average / factor << '/'
                << _data.min / factor << '/' << _data.max / factor << '/'
                << _data.sum / factor << units << " cnt=" << _data.count
                << std::endl;
    } else {
      std::cout << _data.name << " id=" << _data.id
                << " : avg/sum=" << _data.average / factor << '/'
                << _data.sum / factor << units << " cnt=" << _data.count
                << std::endl;
    }
  }
  ~ConcurLatencyCounterClient() {}

 protected:
  NumericCounterData<int64_t> _data;
};

class RplLatencyCounterClient {
 public:
  RplLatencyCounterClient(const NumericCounterData<int64_t> &data)
      : _data(data) {}

  void dump(int depth) {
    std::string indent(depth, '\t');
    std::cout << indent;

    float factor = 1000000.0f;
    float _average_ = _data.average / factor;
    float _sum_ = _data.sum / factor;
    std::string units = "ms";

    if (!_data.noMinMax) {
      float _min_ = _data.min / factor;
      float _max_ = _data.max / factor;
      std::cout << _data.name << ": avg/min/max/sum=" << _average_ << '/'
                << _min_ << '/' << _max_ << '/' << _sum_ << units
                << " start=" << _data.startCount << " cnt=" << _data.count
                << std::endl;
    } else {
      std::cout << _data.name << ": avg/sum=" << _average_ << '/' << _sum_
                << units << " start=" << _data.startCount
                << " cnt=" << _data.count << std::endl;
    }
  }
  ~RplLatencyCounterClient() {}

 protected:
  NumericCounterData<int64_t> _data;
};

class PbsLatencyCounterClient {
 public:
  PbsLatencyCounterClient(const NumericCounterData<int64_t> &data)
      : _data(data) {}

  void dump(int depth) {
    std::string indent(depth, '\t');
    std::cout << indent;

    float factor = 1000000.0f;
    float _average_ = _data.average / factor;
    float _sum_ = _data.sum / factor;
    std::string units = "ms";

    if (!_data.noMinMax) {
      float _min_ = _data.min / factor;
      float _max_ = _data.max / factor;
      std::cout << _data.name << ": avg/min/max/sum=" << _average_ << '/'
                << _min_ << '/' << _max_ << '/' << _sum_ << units
                << " cnt=" << _data.count << std::endl;
    } else {
      std::cout << _data.name << ": avg/sum=" << _average_ << '/' << _sum_
                << units << " cnt=" << _data.count << std::endl;
    }
  }
  ~PbsLatencyCounterClient() {}

 protected:
  NumericCounterData<int64_t> _data;
};

template <typename T>
class HistogramCounterClient {
 public:
  HistogramCounterClient(const HistogramCounterHeader &header,
                         HistogramCounterData<T> buckets[])
      : _header(header), _buckets(buckets) {}

  void dump(int depth) {
    std::string indent(depth, '\t');
    std::cout << indent;
    unsigned int numBuckets = _header.numBuckets;
    std::cout << _header.name << " id=" << _header.id << " :" << std::endl;
    for (unsigned int n = 0; n < numBuckets; ++n) {
      if (n != 0) {
        std::cout << "\t[" << _buckets[n].bucketMin;
      } else {
        std::cout << "\tless then ";
      }

      if (n != 0 && n != numBuckets - 1) {
        std::cout << " - ";
      }
      if (n != numBuckets - 1) {
        std::cout << _buckets[n].bucketMax << ")";
      } else {
        std::cout << " or more";
      }
      std::cout << ": " << _buckets[n].bucketCount << std::endl;
    }
  }
  ~HistogramCounterClient() {}

 protected:
  HistogramCounterHeader _header;
  HistogramCounterData<T> *_buckets;
};

class CounterNodeClient {
 public:
  CounterNodeClient(const CounterNodeData &data) : _data(data) {}

  void dump(int depth) {
    std::string indent(depth, '\t');
    std::cout << indent << _data.name << " id=" << _data.id << std::endl;
  }

  uint64_t getId() const { return _data.id; }

  uint64_t getParentId() const { return _data.parent_id; }
  ~CounterNodeClient() {}

 protected:
  CounterNodeData _data;
};

int computeDepth(uint64_t nodeId,
                 std::unordered_map<uint64_t, CounterNodeClient *> &idNodeMap) {
  int depth = 0;
  while (nodeId != InvalidCounterLibId &&
         idNodeMap.find(nodeId) != idNodeMap.end()) {
    depth++;
    CounterNodeClient *n = idNodeMap[nodeId];
    nodeId = n->getParentId();
  }

  return depth;
};

ssize_t readFromServer(int fd, void *buf, size_t length) {
  uint32_t byteRead = 0;
  int res;
  while ((res = read(fd, buf, length - byteRead)) > 0) {
    byteRead = byteRead + res;
    if (byteRead >= length) {
      break;
    }
    buf = (char *)buf + res;
  }
  if (res < 0) {
    throw std::runtime_error("Cannot receive data from server");
  }
  return byteRead;
}

void readName(int fd, char *name, uint16_t length, int max_length) {
  if (length > max_length) {
    throw std::runtime_error("the length of counter name is too large");
  }
  readFromServer(fd, name, length);
  if (length < max_length) {
    name[length] = '\0';
  } else {
    name[max_length - 1] = '\0';
  }
};

template <CounterClass cc>
struct CounterClass2Type {
  constexpr static CounterClass value = cc;
};

template <class CounterData>
struct CounterDataReader {
  static void readData(int fd, CounterData &data) {
    uint32_t data_size_without_name = sizeof(data) - sizeof(data.name);
    if (data_size_without_name > 0) {
      readFromServer(fd, &data, data_size_without_name);
    }
    readName(fd, data.name, data.name_length, sizeof(data.name));
  }
};

template <CounterClass cc, class CounterData>
struct PropertyParser {};

template <>
struct PropertyParser<CounterClass::EventCounter, EventCounterData> {
  static void parse(const EventCounterData &data, pt::ptree &property) {
    property.put("id", data.id);
    property.put("name", data.name);
    property.put("freq", data.frequency);
    property.put("freq_unit", "sec");
    property.put("cnt", data.count);
  }
};

template <>
struct PropertyParser<CounterClass::StringNameCounter, StringNameCounterData> {
  static void parse(const StringNameCounterData &data, pt::ptree &property) {
    property.put("id", data.id);
    property.put("name", data.name);
    property.put("value", data.value_name);
  }
};

template <>
struct PropertyParser<CounterClass::RplEventCounter, RplEventCounterData> {
  static void parse(const RplEventCounterData &data, pt::ptree &property) {
    float gap = (data.end_lsn - data.start_lsn) / (1024.0f * 1024.0f);
    property.put("id", data.id);
    property.put("name", data.name);
    property.put("time", data.time);
    property.put("time_unit", "sec");
    property.put("freq", data.frequency);
    property.put("freq_unit", "sec");
    property.put("cnt", data.count);
    property.put("lsn_gap", gap);
    property.put("lsn_gap_unit", "MB");
    property.put("start_lsn", data.start_lsn);
    property.put("end_lsn", data.end_lsn);
  }
};

template <>
struct PropertyParser<CounterClass::Numeric64Counter,
                      NumericCounterData<int64_t>> {
  static void parse(const NumericCounterData<int64_t> &data,
                    pt::ptree &property) {
    property.put("id", data.id);
    property.put("name", data.name);
    property.put("avg", data.average);
    property.put("cnt", data.count);
    if (!data.noMinMax) {
      property.put("min", data.min);
      property.put("max", data.max);
      property.put("last", data.last);
    }
  }
};

template <>
struct PropertyParser<CounterClass::Numeric32Counter,
                      NumericCounterData<uint32_t>> {
  static void parse(const NumericCounterData<uint32_t> &data,
                    pt::ptree &property) {
    property.put("id", data.id);
    property.put("name", data.name);
    property.put("avg", data.average);
    property.put("cnt", data.count);
    if (!data.noMinMax) {
      property.put("min", data.min);
      property.put("max", data.max);
      property.put("last", data.last);
    }
  }
};

template <>
struct PropertyParser<CounterClass::SimpleNumeric64Counter,
                      NumericCounterData<int64_t>> {
  static void parse(const NumericCounterData<int64_t> &data,
                    pt::ptree &property) {
    property.put("id", data.id);
    property.put("name", data.name);
    property.put("min", data.min);
    property.put("max", data.max);
    property.put("last", data.last);
    property.put("count", data.count);
  }
};

template <>
struct PropertyParser<CounterClass::SimpleNumericU64Counter,
                      NumericCounterData<uint64_t>> {
  static void parse(const NumericCounterData<uint64_t> &data,
                    pt::ptree &property) {
    property.put("id", data.id);
    property.put("name", data.name);
    property.put("min", data.min);
    property.put("max", data.max);
    property.put("last", data.last);
    property.put("count", data.count);
  }
};

template <>
struct PropertyParser<CounterClass::NewNumeric64Counter,
                      NumericCounterData<int64_t>> {
  static void parse(const NumericCounterData<int64_t> &data,
                    pt::ptree &property) {
    property.put("id", data.id);
    property.put("name", data.name);
    property.put("avg", data.average);
    property.put("sum", data.sum);
    property.put("cnt", data.count);
    if (!data.noMinMax) {
      property.put("min", data.min);
      property.put("max", data.max);
      property.put("last", data.last);
    }
  }
};

template <>
struct PropertyParser<CounterClass::LatencyCounter,
                      NumericCounterData<int64_t>> {
  static void parse(const NumericCounterData<int64_t> &data,
                    pt::ptree &property) {
    uint64_t standard = data.average;
    int factor = 1;
    std::string units = "ns";
    if (standard > 10000000ULL) {
      factor = 1000000ULL;
      units = "ms";
    } else if (standard > 10000ULL) {
      factor = 1000ULL;
      units = "us";
    }

    property.put("id", data.id);
    property.put("name", data.name);
    property.put("unit", units);
    property.put("avg", data.average / factor);
    property.put("sum", data.sum / factor);
    property.put("cnt", data.count);

    if (!data.noMinMax) {
      property.put("min", data.min / factor);
      property.put("max", data.max / factor);
    }
  }
};

template <>
struct PropertyParser<CounterClass::RplLatencyCounter,
                      NumericCounterData<int64_t>> {
  static void parse(const NumericCounterData<int64_t> &data,
                    pt::ptree &property) {
    float factor = 1000000.0f;
    std::string units = "ms";

    property.put("id", data.id);
    property.put("name", data.name);
    property.put("unit", units);
    property.put("avg", data.average / factor);
    property.put("sum", data.sum / factor);
    property.put("cnt", data.count);

    if (!data.noMinMax) {
      property.put("min", data.min / factor);
      property.put("max", data.max / factor);
    }
  }
};

template <>
struct PropertyParser<CounterClass::CounterNode, CounterNodeData> {
  static void parse(const CounterNodeData &data, pt::ptree &property) {
    property.put("id", data.id);
    property.put("name", data.name);
  }
};

struct NodeIndex {
  std::string name;
  uint64_t id;
  uint64_t parentId;
};

typedef std::unordered_map<uint64_t, NodeIndex> NodeMap;

struct PropertyTreePolicy {
  PropertyTreePolicy() : _currParentId(InvalidCounterLibId) {}

  template <class CounterType, class CounterData>
  void addChild(CounterData &data, CounterType) {
    pt::ptree node;

    PropertyParser<CounterType::value, CounterData>::parse(data, node);

    if (CounterType::value == CounterClass::CounterNode) {
      _currParentId = data.id;
    } else {
      data.parent_id = _currParentId;
    }

    _nodeMap[data.id].id = data.id;
    _nodeMap[data.id].parentId = data.parent_id;
    _nodeMap[data.id].name = data.name;

    getParent(data.parent_id).put_child(data.name, node);
  }

  void dump(std::ostream &os) { pt::write_json(os, _root); }

 protected:
  ~PropertyTreePolicy() {}

  pt::ptree &getParent(uint64_t parentId) {
    std::string path = "";
    uint64_t currId = parentId;

    while (currId != InvalidCounterLibId &&
           _nodeMap.find(currId) != _nodeMap.end()) {
      if (path.size() != 0) {
        path = "." + path;
      }

      path = _nodeMap[currId].name + path;
      currId = _nodeMap[currId].parentId;
    }

    if (path.size() == 0) {
      return _root;
    } else {
      return _root.get_child(path);
    }
  }

 private:
  pt::ptree _root;
  uint64_t _currParentId;
  NodeMap _nodeMap;
};

template <class TreePolicy>
struct CounterDataProcessor : public TreePolicy {
  int loadFromStream(int fd) {
    int res;
    char buffer[sizeof(CounterClass)];
    while ((res = readFromServer(fd, buffer, sizeof(buffer))) > 0) {
      uint32_t *i = reinterpret_cast<uint32_t *>(buffer);
      CounterClass cls = static_cast<CounterClass>(*i);
      switch (cls) {
        case CounterClass::EventCounter: {
          EventCounterData data;
          CounterDataReader<EventCounterData>::readData(fd, data);
          TreePolicy::addChild(data,
                               CounterClass2Type<CounterClass::EventCounter>());
        } break;
        case CounterClass::RplEventCounter: {
          RplEventCounterData data;
          CounterDataReader<RplEventCounterData>::readData(fd, data);
          TreePolicy::addChild(
              data, CounterClass2Type<CounterClass::RplEventCounter>());
        } break;
        case CounterClass::Numeric64Counter: {
          NumericCounterData<int64_t> data;
          CounterDataReader<NumericCounterData<int64_t>>::readData(fd, data);
          TreePolicy::addChild(
              data, CounterClass2Type<CounterClass::Numeric64Counter>());
        } break;
        case CounterClass::Numeric32Counter: {
          NumericCounterData<uint32_t> data;
          CounterDataReader<NumericCounterData<uint32_t>>::readData(fd, data);
          TreePolicy::addChild(
              data, CounterClass2Type<CounterClass::Numeric32Counter>());
        } break;
        case CounterClass::SimpleNumeric64Counter: {
          NumericCounterData<int64_t> data;
          CounterDataReader<NumericCounterData<int64_t>>::readData(fd, data);
          TreePolicy::addChild(
              data, CounterClass2Type<CounterClass::SimpleNumeric64Counter>());
        } break;
        case CounterClass::SimpleNumericU64Counter: {
          NumericCounterData<uint64_t> data;
          CounterDataReader<NumericCounterData<uint64_t>>::readData(fd, data);
          TreePolicy::addChild(
              data, CounterClass2Type<CounterClass::SimpleNumericU64Counter>());
        } break;
        case CounterClass::LatencyCounter: {
          NumericCounterData<int64_t> data;
          CounterDataReader<NumericCounterData<int64_t>>::readData(fd, data);
          TreePolicy::addChild(
              data, CounterClass2Type<CounterClass::LatencyCounter>());
        } break;
        case CounterClass::StringNameCounter: {
          StringNameCounterData data;
          uint32_t data_size_without_name =
              sizeof(data) - sizeof(data.value_name) - sizeof(data.name);
          if (data_size_without_name > 0) {
            readFromServer(fd, &data, data_size_without_name);
          }
          readName(fd, data.value_name, data.value_name_length,
                   sizeof(data.value_name));
          readName(fd, data.name, data.name_length, sizeof(data.name));
        } break;
        case CounterClass::RplLatencyCounter: {
          NumericCounterData<int64_t> data;
          CounterDataReader<NumericCounterData<int64_t>>::readData(fd, data);
          TreePolicy::addChild(
              data, CounterClass2Type<CounterClass::RplLatencyCounter>());
        } break;
        case CounterClass::Histogram64Counter:
        case CounterClass::Histogram64LogCounter: {
          (void)buffer;
        } break;
        case CounterClass::CounterNode: {
          CounterNodeData data;
          CounterDataReader<CounterNodeData>::readData(fd, data);
          TreePolicy::addChild(data,
                               CounterClass2Type<CounterClass::CounterNode>());
        } break;
        case CounterClass::NewNumeric64Counter: {
          NumericCounterData<int64_t> data;
          CounterDataReader<NumericCounterData<int64_t>>::readData(fd, data);
          TreePolicy::addChild(
              data, CounterClass2Type<CounterClass::NewNumeric64Counter>());
        } break;
        default:
          throw std::runtime_error("Cannot process unknown counter class");
      }
    }

    return res;
  }
};

int dumpRootNode(int fd) {
  int res = 0;
  CounterDataProcessor<PropertyTreePolicy> processor;
  res = processor.loadFromStream(fd);
  processor.dump(std::cout);

  return res;
}

int dumpFullTree(int fd) {
  int res;
  std::unordered_map<uint64_t, CounterNodeClient *> idNodeMap;
  char buffer[sizeof(CounterClass)];
  while ((res = readFromServer(fd, buffer, sizeof(buffer))) > 0) {
    uint32_t *i = reinterpret_cast<uint32_t *>(buffer);
    CounterClass cls = static_cast<CounterClass>(*i);
    switch (cls) {
      case CounterClass::EventCounter: {
        EventCounterData data;
        uint32_t data_size_without_name = sizeof(data) - sizeof(data.name);
        readFromServer(fd, &data, data_size_without_name);
        readName(fd, data.name, data.name_length, sizeof(data.name));
        EventCounterClient counter = EventCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::RplEventCounter: {
        RplEventCounterData data;
        uint32_t data_size_without_name = sizeof(data) - sizeof(data.name);
        readFromServer(fd, &data, data_size_without_name);
        readName(fd, data.name, data.name_length, sizeof(data.name));
        RplEventCounterClient counter = RplEventCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::Numeric64Counter: {
        NumericCounterData<int64_t> data;
        uint32_t data_size_without_name = sizeof(data) - sizeof(data.name);
        readFromServer(fd, &data, data_size_without_name);
        readName(fd, data.name, data.name_length, sizeof(data.name));
        NumericCounterClient<int64_t> counter =
            NumericCounterClient<int64_t>(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::Numeric32Counter: {
        NumericCounterData<uint32_t> data;
        uint32_t data_size_without_name = sizeof(data) - sizeof(data.name);
        readFromServer(fd, &data, data_size_without_name);
        readName(fd, data.name, data.name_length, sizeof(data.name));
        NumericCounterClient<uint32_t> counter =
            NumericCounterClient<uint32_t>(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::SimpleNumeric64Counter: {
        NumericCounterData<int64_t> data;
        uint32_t data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          readFromServer(fd, &data, data_size_without_name);
        }
        readName(fd, data.name, data.name_length, sizeof(data.name));
        SimpleNumericCounterClient<int64_t> counter =
            SimpleNumericCounterClient<int64_t>(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::SimpleNumericU64Counter: {
        NumericCounterData<uint64_t> data;
        uint32_t data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          readFromServer(fd, &data, data_size_without_name);
        }
        readName(fd, data.name, data.name_length, sizeof(data.name));
        SimpleNumericCounterClient<uint64_t> counter =
            SimpleNumericCounterClient<uint64_t>(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::LatencyCounter: {
        NumericCounterData<int64_t> data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          readFromServer(fd, &data, data_size_without_name);
        }
        readName(fd, data.name, data.name_length, sizeof(data.name));
        LatencyCounterClient counter = LatencyCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::ConcurLatencyCounter: {
        NumericCounterData<int64_t> data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          readFromServer(fd, &data, data_size_without_name);
        }
        readName(fd, data.name, data.name_length, sizeof(data.name));
        ConcurLatencyCounterClient counter = ConcurLatencyCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::RplLatencyCounter: {
        NumericCounterData<int64_t> data;
        uint32_t data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          readFromServer(fd, &data, data_size_without_name);
        }
        readName(fd, data.name, data.name_length, sizeof(data.name));
        RplLatencyCounterClient counter = RplLatencyCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::PbsLatencyCounter: {
        NumericCounterData<int64_t> data;
        uint32_t data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          readFromServer(fd, &data, data_size_without_name);
        }
        readName(fd, data.name, data.name_length, sizeof(data.name));
        PbsLatencyCounterClient counter = PbsLatencyCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::Histogram64Counter:
      case CounterClass::Histogram64LogCounter: {
        HistogramCounterHeader header;
        int header_size_without_name = sizeof(header) - sizeof(header.name);
        readFromServer(fd, &header, header_size_without_name);
        readName(fd, header.name, header.name_length, sizeof(header.name));
        HistogramCounterData<int64_t> buckets[header.numBuckets];
        for (uint16_t n = 0; n < header.numBuckets; ++n) {
          readFromServer(fd, &buckets[n], sizeof(buckets[n]));
        }
        HistogramCounterClient<int64_t> counter =
            HistogramCounterClient<int64_t>(header, buckets);
        int depth = computeDepth(header.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::CounterNode: {
        CounterNodeData data;
        uint32_t data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          readFromServer(fd, &data, data_size_without_name);
        }
        readName(fd, data.name, data.name_length, sizeof(data.name));
        CounterNodeClient *node = new CounterNodeClient(data);
        idNodeMap[node->getId()] = node;
        int depth = computeDepth(node->getParentId(), idNodeMap);
        node->dump(depth);
      } break;
      case CounterClass::StringNameCounter: {
        StringNameCounterData data;
        uint32_t data_size_without_name =
            sizeof(data) - sizeof(data.value_name) - sizeof(data.name);
        if (data_size_without_name > 0) {
          readFromServer(fd, &data, data_size_without_name);
        }
        readName(fd, data.value_name, data.value_name_length,
                 sizeof(data.value_name));
        readName(fd, data.name, data.name_length, sizeof(data.name));
        StringNameCounterClient counter = StringNameCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::NewNumeric64Counter: {
        NumericCounterData<int64_t> data;
        uint32_t data_size_without_name = sizeof(data) - sizeof(data.name);
        readFromServer(fd, &data, data_size_without_name);
        readName(fd, data.name, data.name_length, sizeof(data.name));
        NewNumericCounterClient<int64_t> counter =
            NewNumericCounterClient<int64_t>(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      default:
        throw std::runtime_error("Cannot process unknown counter class");
    }
  }  // end while

  return res;
}

void readName2Stringstream(std::stringstream &ss, char *name, uint16_t length,
                           int max_length) {
  if (length > max_length) {
    throw std::runtime_error("the length of counter name is too large");
  }
  ss.read(name, length);
  if (length < max_length) {
    name[length] = '\0';
  } else {
    name[max_length - 1] = '\0';
  }
};

// used in unit test for benckmark
void dumpFullTree(std::stringstream &ss) {
  std::unordered_map<uint64_t, CounterNodeClient *> idNodeMap;
  char buffer[sizeof(CounterClass)];
  while (ss.read(buffer, sizeof(buffer))) {
    uint32_t *i = reinterpret_cast<uint32_t *>(buffer);
    CounterClass cls = static_cast<CounterClass>(*i);
    switch (cls) {
      case CounterClass::EventCounter: {
        EventCounterData data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        ss.read(reinterpret_cast<char *>(&data), data_size_without_name);
        readName2Stringstream(ss, data.name, data.name_length,
                              sizeof(data.name));
        EventCounterClient counter = EventCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::RplEventCounter: {
        RplEventCounterData data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        ss.read(reinterpret_cast<char *>(&data), data_size_without_name);
        readName2Stringstream(ss, data.name, data.name_length,
                              sizeof(data.name));
        RplEventCounterClient counter = RplEventCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::Numeric64Counter: {
        NumericCounterData<int64_t> data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        ss.read(reinterpret_cast<char *>(&data), data_size_without_name);
        readName2Stringstream(ss, data.name, data.name_length,
                              sizeof(data.name));
        NumericCounterClient<int64_t> counter =
            NumericCounterClient<int64_t>(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::Numeric32Counter: {
        NumericCounterData<uint32_t> data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        ss.read(reinterpret_cast<char *>(&data), data_size_without_name);
        readName2Stringstream(ss, data.name, data.name_length,
                              sizeof(data.name));
        NumericCounterClient<uint32_t> counter =
            NumericCounterClient<uint32_t>(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::SimpleNumeric64Counter: {
        NumericCounterData<int64_t> data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          ss.read(reinterpret_cast<char *>(&data), data_size_without_name);
        }
        readName2Stringstream(ss, data.name, data.name_length,
                              sizeof(data.name));
        SimpleNumericCounterClient<int64_t> counter =
            SimpleNumericCounterClient<int64_t>(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::SimpleNumericU64Counter: {
        NumericCounterData<uint64_t> data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          ss.read(reinterpret_cast<char *>(&data), data_size_without_name);
        }
        readName2Stringstream(ss, data.name, data.name_length,
                              sizeof(data.name));
        SimpleNumericCounterClient<uint64_t> counter =
            SimpleNumericCounterClient<uint64_t>(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::LatencyCounter: {
        NumericCounterData<int64_t> data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          ss.read(reinterpret_cast<char *>(&data), data_size_without_name);
        }
        readName2Stringstream(ss, data.name, data.name_length,
                              sizeof(data.name));
        LatencyCounterClient counter = LatencyCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::RplLatencyCounter: {
        NumericCounterData<int64_t> data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          ss.read(reinterpret_cast<char *>(&data), data_size_without_name);
        }
        readName2Stringstream(ss, data.name, data.name_length,
                              sizeof(data.name));
        RplLatencyCounterClient counter = RplLatencyCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::Histogram64Counter:
      case CounterClass::Histogram64LogCounter: {
        HistogramCounterHeader header;
        int header_size_without_name = sizeof(header) - sizeof(header.name);
        ss.read(reinterpret_cast<char *>(&header), header_size_without_name);
        readName2Stringstream(ss, header.name, header.name_length,
                              sizeof(header.name));
        HistogramCounterData<int64_t> buckets[header.numBuckets];
        for (unsigned int n = 0; n < header.numBuckets; ++n) {
          ss.read(reinterpret_cast<char *>(&buckets[n]), sizeof(buckets[n]));
        }
        HistogramCounterClient<int64_t> counter =
            HistogramCounterClient<int64_t>(header, buckets);
        int depth = computeDepth(header.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::CounterNode: {
        CounterNodeData data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        if (data_size_without_name > 0) {
          ss.read(reinterpret_cast<char *>(&data), data_size_without_name);
        }
        readName2Stringstream(ss, data.name, data.name_length,
                              sizeof(data.name));
        CounterNodeClient *node = new CounterNodeClient(data);
        idNodeMap[node->getId()] = node;
        int depth = computeDepth(node->getParentId(), idNodeMap);
        node->dump(depth);
      } break;
      case CounterClass::StringNameCounter: {
        StringNameCounterData data;
        uint32_t data_size_without_name =
            sizeof(data) - sizeof(data.value_name) - sizeof(data.name);
        if (data_size_without_name > 0) {
          ss.read(reinterpret_cast<char *>(&data), data_size_without_name);
        }
        readName2Stringstream(ss, data.value_name, data.value_name_length,
                              sizeof(data.value_name));
        readName2Stringstream(ss, data.name, data.name_length,
                              sizeof(data.name));
        StringNameCounterClient counter = StringNameCounterClient(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      } break;
      case CounterClass::NewNumeric64Counter: {
        NumericCounterData<int64_t> data;
        int data_size_without_name = sizeof(data) - sizeof(data.name);
        ss.read(reinterpret_cast<char *>(&data), data_size_without_name);
        readName2Stringstream(ss, data.name, data.name_length,
                              sizeof(data.name));
        NewNumericCounterClient<int64_t> counter =
            NewNumericCounterClient<int64_t>(data);
        int depth = computeDepth(data.parent_id, idNodeMap) + 1;
        counter.dump(depth);
      }
      default:
        throw std::runtime_error("Cannot process unknown counter class");
    }
  }  // end while

  return;
}
