/********************************************************************
 * Copyright (C) Huawei Technologies, 2018
 * Performance counters: client application
 * Client application to retrieve performance counters
 ********************************************************************/

#include "perfclient.h"

int main(int argc, char **argv) {
  uint32_t port;
  PublisherType type;
  std::string cmd;
  std::string filter;
  std::string filter_op;
  int pid;
  std::string serverName;

  // Command line options
  // clang-format off
  po::options_description config("Configuration");
  config.add_options()
  (
    "type,t", po::value<PublisherType>(&type)->default_value(PublisherType::Tcp),
    "Publisher type can be unix or tcp"
  )
  (
    "server,s", po::value<std::string>(&serverName)->default_value(""),
    "server name for TCP publisher"
  )
  (
    "port,p", po::value<uint32_t>(&port)->default_value(CounterTcpPublisher::PORT),
    "TCP port for TCP listener"
  )
  (
    "command,c", po::value<std::string>(&cmd)->default_value("dump"),
    "Command to send to publisher: dump, reset, ids, dumpmem, memstats, jememstats, dumpspace, dumprootnode, dumpalarm"
  )
  (
    "filter,f", po::value<std::string>(&filter)->default_value(""),
    "Filter applied to the response. Format: <id|name>=<value1,value2,...>"
  )
  (
    "filter-mode", po::value<std::string>(&filter_op)->default_value(""),
    "Filter operator for filter. now supoort eq and ne"
  )
  (
    "pid", po::value<int>(&pid)->default_value(0),
    "Process id for the Unix domain socket listener"
  )
  (
    "help,h", "Give this help list"
  );
  // clang-format on
  po::variables_map vm;
  po::positional_options_description p;
  try {
    p.add("server", 1);
    po::store(
        po::command_line_parser(argc, argv).options(config).positional(p).run(),
        vm);
    po::notify(vm);
    if (vm.count("help")) {
      std::cout << config << "\n";
      return 1;
    }
  } catch (std::exception &e) {
    std::cerr << "Invalid command line arguments: " << e.what() << "\n";
    return 1;
  }

  int res = 0;
  char buffer[4096];
  int fd = -1;
  // The default port
  try {
    if (type == PublisherType::Tcp) {
      if (serverName.length() == 0) {
        std::cout << config << "\n";
        throw std::runtime_error("Invalid arguments");
      }
    } else if (type == PublisherType::Unix) {
      if (pid <= 0) {
        std::cout << config << "\n";
        throw std::runtime_error("Invalid arguments");
      }
    }

    PublisherRequest req;
    if (cmd == "dump") {
      req.cmd = PublisherCommand::DumpFullTree;
    } else if (cmd == "reset") {
      req.cmd = PublisherCommand::ResetFullTree;
    } else if (cmd == "ids") {
      req.cmd = PublisherCommand::ListCounterIds;
    } else if (cmd == "dumpmem") {
      req.cmd = PublisherCommand::DumpMem;
    } else if (cmd == "memstats") {
      req.cmd = PublisherCommand::MemStats;
    } else if (cmd == "jememstats") {
      req.cmd = PublisherCommand::JeMemStats;
    } else if (cmd == "dumpspace") {
      req.cmd = PublisherCommand::DumpSliceSpaceUsage;
    } else if (cmd == "dumprootnode") {
      req.cmd = PublisherCommand::DumpRootNode;
    } else if (cmd == "dumpalarm") {
      req.cmd = PublisherCommand::DumpAlarm;
    } else if (cmd == "indextable") {
      req.cmd = PublisherCommand::ShowIndexTable;
    } else if (cmd == "sampletable") {
      req.cmd = PublisherCommand::ShowSampleTable;
    } else {
      std::cout << config << "\n";
      throw std::runtime_error("Invalid command");
    }

    req.filter = parseFilter(filter, filter_op);
    req.size = sizeof(req);

    if (type == PublisherType::Tcp) {
      fd = establishTcpConnection(serverName.c_str(), port);
    } else {
      fd = establishUnixConnection(pid);
    }
    res = write(fd, &req, sizeof(req));
    if (res < 0) {
      throw std::runtime_error("Cannot send command");
    }

    if (req.cmd == PublisherCommand::DumpFullTree) {
      res = dumpFullTree(fd);
      if (res < 0) {
        throw std::runtime_error("Cannot receive data from server");
      }
      printf("\n");
    } else if (req.cmd == PublisherCommand::DumpRootNode) {
      res = dumpRootNode(fd);
      if (res < 0) {
        throw std::runtime_error("Cannot receive data from server");
      }
      printf("\n");
    } else if (req.cmd == PublisherCommand::ResetFullTree) {
      ResetResponse resp;
      res = read(fd, &resp, sizeof(resp));
      if (res != sizeof(resp)) {
        throw std::runtime_error("Cannot receive data from server");
      }
      printf("Reset command sent. Result is %d\n",
             static_cast<uint32_t>(resp.result));
    } else if (req.cmd == PublisherCommand::ListCounterIds) {
      while ((res = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[res] = 0;
        printf("%s", buffer);
      }
      if (res < 0) {
        throw std::runtime_error("Cannot receive data from server");
      }
      printf("\n");
    } else if (req.cmd == PublisherCommand::DumpMem) {
      ResetResponse resp;
      res = read(fd, &resp, sizeof(resp));
      if (res != sizeof(resp)) {
        throw std::runtime_error("Cannot receive data from server");
      }
      printf("DUMP MEM command sent. Result is %d\n",
             static_cast<uint32_t>(resp.result));
      printf(
          "If result is okay.. Check file server side @/tmp/mem.{data,meta}\n");
    } else if (req.cmd == PublisherCommand::DumpSliceSpaceUsage) {
      while ((res = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[res] = 0;
        printf("%s", buffer);
      }
      if (res < 0) {
        throw std::runtime_error("Cannot receive data from server");
      }
      printf("\n");
    } else if (req.cmd == PublisherCommand::MemStats) {
      while ((res = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[res] = 0;
        printf("%s", buffer);
      }
      if (res < 0) {
        throw std::runtime_error("Cannot receive data from server");
      }
      printf("\n");
    } else if (req.cmd == PublisherCommand::JeMemStats) {
      while ((res = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[res] = 0;
        printf("%s", buffer);
      }
      if (res < 0) {
        throw std::runtime_error("Cannot receive data from server");
      }
      printf("\n");
    } else if(req.cmd == PublisherCommand::ShowIndexTable || req.cmd == PublisherCommand::ShowSampleTable) {
      while ((res = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[res] = 0;
        printf("%s", buffer);
      }
      if (res < 0) {
        throw std::runtime_error("Cannot receive data from server");
      }
      printf("\n");


    } else {
      while ((res = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[res] = 0;
        std::cout << buffer;
      }

      if (res < 0) {
        throw std::runtime_error("Cannot receive data from server");
      }

      printf("\n");
    }
  } catch (std::exception &e) {
    printf("Error - %s\n", e.what());
    res = 1;
  }

  close(fd);
  return res;
}
