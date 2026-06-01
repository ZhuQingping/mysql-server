/********************************************************************
 * Copyright (C) Huawei Technologies, 2022
 * Clieny/Server communication framework
 ********************************************************************/
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <netdb.h>
#include <stdio.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "perfpublisher.h"
#include "perfpublisherproto.h"

using namespace Huawei::Common;

// Different types of publishers
enum PublisherType {
  Tcp,
  Unix,
};

// Translation of publisher type CLI argument to enum
std::istream &operator>>(std::istream &in, PublisherType &t) {
  std::string token;
  in >> token;
  if (token == "tcp") {
    t = PublisherType::Tcp;
  } else if (token == "unix") {
    t = PublisherType::Unix;
  } else {
    in.setstate(std::ios_base::failbit);
  }
  return in;
}

// Translation of publisher command CLI argument to enum
std::istream &operator>>(std::istream &in,
                         Huawei::Common::PublisherCommand &t) {
  std::string token;
  in >> token;
  if (token == "reset") {
    t = PublisherCommand::ResetFullTree;
  } else if (token == "dump") {
    t = PublisherCommand::DumpFullTree;
  } else if (token == "dumpmem") {
    t = PublisherCommand::DumpMem;
  } else {
    in.setstate(std::ios_base::failbit);
  }
  return in;
}

/*
 * establishConnection - establishes connection to the server
 *
 * Parameters:
 *  serverName - name or ip of the server
 *
 * Returns:
 *  descriptor for connection
 */
int establishTcpConnection(const char *serverName, uint32_t port) {
  struct sockaddr_in addr;
  struct hostent *server;
  int sockfd;
  int res;

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    throw std::runtime_error("Cannot create socket");
  }
  server = gethostbyname(serverName);
  if (server == nullptr) {
    throw std::runtime_error("Cannot resolve server name");
  }
  int ret = memset_s(&addr, sizeof(addr), 0, sizeof(addr));
  if (ret != EOK) {
    throw std::runtime_error("memset addr failed");
  }
  addr.sin_family = AF_INET;

  ret = memcpy_s(&addr.sin_addr.s_addr, sizeof(addr.sin_addr.s_addr),
                 server->h_addr, server->h_length);
  if (ret != EOK) {
    throw std::runtime_error("copy sun path failed");
  }
  addr.sin_port = htons(port);
  printf("Connecting to %s:%u\n", serverName, port);
  res = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));
  if (res < 0) {
    throw std::runtime_error("Cannot connect to server");
  }
  return sockfd;
}

int establishUnixConnection(uint32_t pid) {
  int sockfd = -1;
  struct sockaddr_un addr;
  std::string name = SLICE_SOCKET_NAME + std::to_string(pid);
  sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockfd < 0) {
    throw std::runtime_error("Cannot create a socket");
  }
  int ret = memset_s(&addr, sizeof(addr), 0, sizeof(addr));
  if (ret != EOK) {
    throw std::runtime_error("memset addr failed");
  }
  addr.sun_family = AF_UNIX;
  ret = strncpy_s(addr.sun_path + 1, sizeof(addr.sun_path) - 1, name.c_str(),
                  sizeof(addr.sun_path) - 2);
  if (ret != EOK) {
    throw std::runtime_error("copy sun path failed");
  }
  int res = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));
  if (res < 0) {
    throw std::runtime_error("Cannot connect to server");
  }

  return sockfd;
}

PublisherFilter parseFilter(const std::string &filter,
                            const std::string &filter_op) {
  PublisherFilter result;
  int ret = memset_s(&result, sizeof(struct PublisherFilter), 0,
                     sizeof(struct PublisherFilter));
  if (ret != EOK) {
    throw std::runtime_error("memset result value failed");
  }
  if (filter == "") {
    return result;
  }

  std::vector<std::string> tokens = tokenSplit(filter, '=');
  if (tokens.size() == 2) {
    std::string field = tokens[0];
    std::string value = tokens[1];
    if (field == "id") {
      result.field = PublisherFilterField::Id;
    } else if (field == "name") {
      result.field = PublisherFilterField::Name;
    } else {
      throw std::runtime_error("Filter field must be 'name' or 'id'");
    }
    if (filter_op == "ne") {
      result.op = PublisherFilterOperation::Unequal;
    } else {
      result.op = PublisherFilterOperation::Equal;
    }
    if (value.size() > sizeof(result.value) - 1) {
      throw std::runtime_error("Filter value is too long");
    }
    ret = strncpy_s(result.value, sizeof(result.value), value.c_str(),
                    sizeof(result.value) - 1);
    if (ret != EOK) {
      throw std::runtime_error("Copy result value failed");
    }
  } else {
    throw std::runtime_error(
        "Invalid format. Filter format must be <field>=<value>");
  }
  return result;
}