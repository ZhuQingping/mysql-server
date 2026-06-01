/********************************************************************
 * Copyright (C) Huawei Technologies, 2019
 * Performance counters framework implementation:
 *  Helper functions used in performance counters framework
 ********************************************************************/

#ifndef __PERFUTILS_H__
#define __PERFUTILS_H__


#include <map>
#include <string>
#include <vector>

namespace Huawei {
namespace Common {

/*
 *  tockenSplit - splits a string by delimiter
 *
 * Parameters:
 * str       (IN) - the string to be splited
 * delimiter (IN) - the delimiter
 *
 * Returns:
 * list of string tokens
 *
 */
std::vector<std::string> tokenSplit(const std::string &str, char delimiter);

bool isNumber(const std::string &str);

bool isAlphas(const std::string &str);

void printTop(
    std::vector<std::tuple<std::string, std::string, std::string>> &paramDist);

void printHeader(
    std::vector<std::tuple<std::string, std::string, std::string>> &paramDist);

void printRow(
    std::vector<std::tuple<std::string, std::string, std::string>> &paramDist);

}  // namespace Common
}  // namespace Huawei
#endif /*__PERFUTILS_H__*/
