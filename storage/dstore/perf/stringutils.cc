/********************************************************************
 * Copyright (C) Huawei Technologies, 2019
 * Performance counters framework implementation:
 *  Helper functions used in performance counters framework
 ********************************************************************/

#include "stringutils.h"
#include <sstream>
namespace Huawei {
namespace Common {

/*
 * Split a string into tokens
 *
 * st        - the string to be splited
 * delimiter - the delimiter to split the string
 */
std::vector<std::string> tokenSplit(const std::string &str, char delimiter) {
  std::vector<std::string> result;
  std::stringstream stream(str);
  std::string token;
  while (getline(stream, token, delimiter)) {
    result.push_back(token);
  }
  return result;
}

}  // namespace Common
}  // namespace Huawei
