/********************************************************************
 * Copyright (C) Huawei Technologies, 2018
 * Performance counters framework: server to publish perfromance measurments
 *
 *  Contains code that sends perfromance counters over the
 *  network to clients
 ********************************************************************/

#include "perfpublisher.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <string>
#include "memstats.h"

namespace Huawei {
namespace Common {

using namespace std;

// max write size when dump full tree, 1GB
static constexpr uint64_t DUMP_WRITE_SIZE = 1 * 1024 * 1024 * 1024;

const char *CounterTcpPublisher::DEFAULT_PUBLISH_ADDR = "127.0.0.1";

CounterTcpPublisher::CounterTcpPublisher(uint32_t port) {
  _port = port;
  _addr = DEFAULT_PUBLISH_ADDR;
}

CounterTcpPublisher::CounterTcpPublisher(const char *addr, uint32_t port) {
  _port = port;
  _addr = addr;
}

CounterSocketPublisher::~CounterSocketPublisher() noexcept { stop(); }

/*
 * createListenSocket - creates a server socket on which worker thread will
 * listen.
 */

void CounterTcpPublisher::createListenSocket() {
  struct sockaddr_in addr;
  int t = 1;

  if ((_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    CDE_THROW(errno, "Cannot create socket");
  }

  if (setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(int)) < 0) {
    CDE_THROW(errno, "Cannot set socket option");
  }

  memset_s(&addr, sizeof(addr), 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(_addr);
  addr.sin_port = htons(_port);

  if (bind(_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    close(_fd);
    _fd = -1;
    CDE_THROW(errno, "Cannot bind socket");
  }
}

CounterUnixPublisher::CounterUnixPublisher(const std::string &socketName) {
  std::stringstream fmt;
  int pid = getpid();

  fmt << socketName << pid;
  _socketName = fmt.str();
}

CounterSocketPublisher::CounterSocketPublisher() {
  _stopFlag = false;
  _fd = -1;
}

/*
 * createListenSocket - creates a server socket on which worker thread will
 * listen.
 */

void CounterUnixPublisher::createListenSocket() {
  struct sockaddr_un addr;

  if ((_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    CDE_THROW(errno, "Cannot create socket");
  }

  memset_s(&addr, sizeof(addr), 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  int ret = strncpy_s(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                      _socketName.c_str(), sizeof(addr.sun_path) - 2);
  if (ret != EOK) {
    CDE_THROW(ret, "Copy sun path failed");
  }

  ret = unlink(_socketName.c_str());
  if (ret != 0) {
  }

  if (bind(_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    close(_fd);
    _fd = -1;
    CDE_THROW(errno, "Cannot bind socket");
  }
}

/*
 * stop  - stops thread that serves clients
 */

void CounterSocketPublisher::stop() {
  _stopFlag = true;
  if (_fd >= 0) {
    shutdown(_fd, SHUT_RDWR);
    close(_fd);
  }
  _fd = -1;
  if (_workerThread.joinable()) {
    try {
      _workerThread.join();
    } catch (std::exception &e) {
    }
  }
}

void CounterSocketPublisher::start() {
  createListenSocket();
  _workerThread = thread(&CounterSocketPublisher::run, this);
}

/*
 * processDumpFullTree - dumps all counters into the socket
 *
 * Parameters:
 * fd - socket
 * filter - special counter item list
 */
void CounterSocketPublisher::processDumpFullTree(int fd,
                                                 PublisherFilter *filter) {
  CounterCatalog *cc = CounterCatalog::Get();
  stringstream ss;
  cc->dump(ss, filter);
  string output = ss.str();
  int result;
  uint64_t written = 0;
  uint64_t length = output.length();

  do {
    uint64_t size = 0;
    // avoid write failure of excessive data size
    if (length - written >= DUMP_WRITE_SIZE) {
      size = DUMP_WRITE_SIZE;
    } else {
      size = length - written;
    }
    result = write(fd, output.c_str() + written, size);
    if (result > 0) {
      written += result;
    }
  } while (result > 0 && length > written);
}

/*
 * processDumpRootNode - dump specify root node into the socket
 *
 * Parameters:
 * fd - socket to which result is written
 * filter - special counter item list
 */
void CounterSocketPublisher::processDumpRootNode(int fd,
                                                 PublisherFilter *filter) {
  CounterCatalog *cc = CounterCatalog::Get();
  stringstream ss;
  cc->dumpRootNode(ss, filter);
  string output = ss.str();
  int result, written = 0;
  int length = output.length();

  do {
    result = write(fd, output.c_str() + written, length - written);
    if (result > 0) {
      written += result;
    }
  } while (result > 0 && length > written);
}

/*
 * processResetFullTree - resets all counters
 *
 * Parameters:
 * fd - socket to which result is written
 * filter - special counter item list
 */
void CounterSocketPublisher::processResetFullTree(int fd,
                                                  PublisherFilter *filter) {
  ResetResponse res;
  res.result = PublisherResponseCode::Ok;
  try {
    CounterCatalog *cc = CounterCatalog::Get();
    cc->reset(filter);
  } catch (std::exception &) {
    res.result = PublisherResponseCode::Error;
  }
  auto ret = write(fd, &res, sizeof(res));
  if (ret < 0) {
  }
}

/*
 * processListCounterIds - list all counter ids
 *
 * Parameters:
 * fd - socket to which result is written
 * filter - special counter item list
 */
void CounterSocketPublisher::processListCounterIds(int fd,
                                                   PublisherFilter *filter) {
  CounterCatalog *cc = CounterCatalog::Get();
  stringstream ss;
  cc->listCounterIds(ss, filter);
  string output = ss.str();
  int result, written = 0;
  int length = output.length();

  do {
    result = write(fd, output.c_str() + written, length - written);
    if (result > 0) {
      written += result;
    }
  } while (result > 0 && length > written);
}

/*
 * processMemStats - stats the memory usage info from libtcmalloc or libjemalloc
 *
 * Parameters:
 * fd - socket to which result is written
 */
void CounterSocketPublisher::processMemStats(int fd) {
  constexpr int buffer_length = 20480;
  std::vector<char> buffer(buffer_length, 0);
  MallocExtension_GetStats(buffer.data(), buffer.size());

  int result, written = 0;
  int length = strlen(buffer.data());
  do {
    result = write(fd, buffer.data() + written, length - written);
    if (result > 0) {
      written += result;
    }
  } while (result > 0 && length > written);
}

void CounterSocketPublisher::processJeMemStats(int fd) {
  auto writeCallBack = [](void *opaque, const char *str) {
    int fd = *(int *)opaque;
    int result, written = 0;
    int length = strlen(str);
    do {
      result = write(fd, str + written, length - written);
      if (result > 0) {
        written += result;
      }
    } while (result > 0 && length > written);
  };

  CdeMallocStatsPrint(writeCallBack, (void *)&fd, NULL);
}

/*
 * processDumpMem - dumps all values from memory
 *
 * Parameters:
 * fd - socket to which result is written
 */
void CounterSocketPublisher::processDumpMem(__attribute__((unused)) int fd,
                                            __attribute__((unused))
                                            string namekey = "") {
#ifdef ENABLE_ODD_MEMDUMP
  ResetResponse res;
  res.result = PublisherResponseCode::Ok;
  try {
    CounterCatalog *cc = CounterCatalog::Get();
    cc->dumpmemall();
  } catch (std::exception &) {
    res.result = PublisherResponseCode::Error;
    write(fd, &res, sizeof(res));
    LOG4CXX_ERROR(Log, "Error Dumping Memory ");
  }
  write(fd, &res, sizeof(res));
#endif
}

/*
 * processCustomFunction - executes custom command handler installed by modules
 *                         other than CountersRegistry
 *
 * Parameters:
 * c - command to execute that identifies custom handler
 * fd -  socket to which result is written
 * filter - additional parameters passed to the custom handler
 */

void CounterSocketPublisher::processCustomFunction(PublisherCommand c, int fd,
                                                   PublisherFilter *filter) {
  std::unique_lock<std::mutex> l(_ccMutex);
  auto i = _customCommands.find((uint32_t)c);
  if (i == _customCommands.end()) {
    return;
  }
  std::stringstream ss;
  i->second(ss, filter);
  string output = ss.str();
  int result, written = 0;
  int length = output.length();

  do {
    result = write(fd, output.c_str() + written, length - written);
    if (result > 0) {
      written += result;
    }
  } while (result > 0 && length > written);
}

/*
 * addCustomCommand - adds a custom command handler to the list of commands
 *                    that can be processed by components other than
 *                    CounterRegistry
 *
 * Parameters:
 * c - command id
 * cc - command handler
 */

void CounterSocketPublisher::addCustomCommand(PublisherCommand c,
                                              const CustomCommand &cc) {
  std::unique_lock<std::mutex> l(_ccMutex);
  _customCommands[(uint32_t)c] = cc;
}

/*
 * removeCustomCommand - removes a custom command handler installed by
 *                       addCustomCommand
 *
 * Parameters:
 * c - command id
 */

void CounterSocketPublisher::removeCustomCommand(PublisherCommand c) {
  std::unique_lock<std::mutex> l(_ccMutex);
  auto i = _customCommands.find((uint32_t)c);
  if (i == _customCommands.end()) {
    return;
  }
  _customCommands.erase(i);
}

/*
 * processConnection - processes client request.
 *
 * This implementation writes contents of all known counters to socket.
 * fd -  socket to which result is written
 */
void CounterSocketPublisher::processConnection(int fd) {
  int result;

  PublisherRequest req;
  result = read(fd, &req, sizeof(req));
  if (result != sizeof(req)) {
    return;
  }
  PublisherFilter *filter = nullptr;
  if (req.filter.field == PublisherFilterField::Id ||
      req.filter.field == PublisherFilterField::Name) {
    filter = &req.filter;
  }
  switch (req.cmd) {
    case PublisherCommand::DumpFullTree:
      processDumpFullTree(fd, filter);
      break;
    case PublisherCommand::DumpRootNode:
      processDumpRootNode(fd, filter);
      break;
    case PublisherCommand::ResetFullTree:
      processResetFullTree(fd, filter);
      break;
    case PublisherCommand::ListCounterIds:
      processListCounterIds(fd, filter);
      processDumpMem(fd);
      break;
    case PublisherCommand::DumpMem:
      processDumpMem(fd);
      break;
    case PublisherCommand::MemStats:
      processMemStats(fd);
      break;
    case PublisherCommand::JeMemStats:
      processJeMemStats(fd);
      break;
    default:
      processCustomFunction(req.cmd, fd, filter);
      break;
  }
}

/*
 * run   - root function of the worker thread that serves requests for counters.
 */

void CounterSocketPublisher::run() {
  try {
    if (listen(_fd, BACKLOGSIZE) == -1) {
      CDE_THROW(errno, "Cannot listen on a socket");
    }

    while (!_stopFlag) {
      int cl;
      if ((cl = accept(_fd, NULL, NULL)) == -1) {
        if (_stopFlag) {
          break;
        }
        CDE_THROW(errno, "Cannot accept incoming connection");
      }

      try {
        processConnection(cl);
      } catch (std::exception &e) {
      }
      close(cl);
    }
  } catch (std::exception &e) {
  }
}

}  // namespace Common
}  // namespace Huawei
