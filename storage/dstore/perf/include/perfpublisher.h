/********************************************************************
 * Copyright (C) Huawei Technologies, 2018
 * Performance counters framework: server to publish perfromance measurments
 *
 *  Contains declaration of code that sends perfromance counters over the
 *  network to clients
 ********************************************************************/
#ifndef __PERF_PUBLISHER__
#define __PERF_PUBLISHER__

#include "perfcounters.h"

#include <string.h>
#include <functional>
#include <mutex>
#include <ostream>
#include <thread>

namespace Huawei {
namespace Common {

#define SLICE_SOCKET_NAME "SliceServerUnixSocket"

/*
 * This class publishes counter information through UNIX domain sockets.
 * Name of the socket is apexcounterlib followed by process id.
 * As of now, in response to a connection, all counters are printed to the
 * sockets.
 * TODO: support retrieval of certain counters. Reset and so on.
 */

class CounterSocketPublisher {
 public:
  // Signature of the custom command handler
  using CustomCommand =
      std::function<void(std::ostream &os, const PublisherFilter *filter)>;
  CounterSocketPublisher();
  virtual ~CounterSocketPublisher() noexcept;

  virtual void stop();
  virtual void start();
  void addCustomCommand(PublisherCommand c, const CustomCommand &cc);
  void removeCustomCommand(PublisherCommand c);

 protected:
  virtual void createListenSocket() = 0;
  virtual void run();
  virtual void processConnection(int fd);

  void processResetFullTree(int fd, PublisherFilter *filter);
  void processDumpFullTree(int fd, PublisherFilter *filter);
  void processDumpRootNode(int fd, PublisherFilter *filter);
  void processListCounterIds(int fd, PublisherFilter *filter);
  void processDumpMem(int fd, std::string namekey);
  void processMemStats(int fd);
  void processJeMemStats(int fd);
  void processCustomFunction(PublisherCommand c, int fd,
                             PublisherFilter *filter);

  // server socket descriptor
  int _fd;
  // thread that serves requests
  std::thread _workerThread;
  // This flag is raised to indicate that request serving thread must stop
  bool _stopFlag;
  // backlog size of the server socket
  static const int BACKLOGSIZE = 5;

  // Extensible comands mechanism
  // map of custom commands for publisher
  // key is id of the command
  // value is a command handler
  std::unordered_map<uint32_t, CustomCommand> _customCommands;
  // mutex protecting _customCommands
  std::mutex _ccMutex;
};

/*
 * This class publishes counter information through UNIX domain sockets.
 * Name of the socket is apexcounterlib followed by process id.
 * As of now, in response to a connection, all counters are printed to the
 * sockets.
 * TODO: support retrieval of certain counters. Reset and so on.
 */

class CounterTcpPublisher : public CounterSocketPublisher {
 public:
  CounterTcpPublisher(uint32_t port);
  CounterTcpPublisher(const char *addr, uint32_t port);
  CounterTcpPublisher() : CounterTcpPublisher(DEFAULT_PUBLISH_ADDR, PORT) {}

  static const int PORT = 20201;
  static const char *DEFAULT_PUBLISH_ADDR;
  ~CounterTcpPublisher() {}

 private:
  void createListenSocket() override;

  // TCP port to listen to
  uint32_t _port;
  const char *_addr;
  // backlog size of the server socket
  static const int BACKLOGSIZE = 5;
};

class CounterUnixPublisher : public CounterSocketPublisher {
 public:
  CounterUnixPublisher(const std::string &socketName);
  ~CounterUnixPublisher() {}

 protected:
  void createListenSocket() override;
  // name of the server socket to connect to
  std::string _socketName;
};

}  // namespace Common
}  // namespace Huawei

#endif  // __PERF_PUBLISHER__
