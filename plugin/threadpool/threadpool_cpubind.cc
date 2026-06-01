/* Copyright (C) 2012 Monty Program Ab
   Copyright (c) 2019, 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
   USA */

#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mysql/components/services/log_builtins.h"
#include "mysqld_error.h"
#include "threadpool.h"

struct Cpu_range_t {
  int start;
  int end;
};

static std::vector<int> g_available_cpus;

/** We bind all the threads in a group to a specified cpu,
So get the cpu id from the group index

@param[in] index  index of the group in the group array.

@return  cpu id
*/
int get_cpuid_by_group_index(int index) {
  if (g_available_cpus.empty()) {
    return -1;
  }
  return g_available_cpus[index % g_available_cpus.size()];
}

/** get cpu list from cpubind info

@param[in] cpubind the user configed cpulist in string

@return  vector of cpu ids
*/
static std::vector<Cpu_range_t> parse_cpu_bind(const std::string &cpubind) {
  std::vector<Cpu_range_t> ranges;
  // Define a regex pattern to match the entire string and extract ranges
  std::regex pattern(
      R"(^cpubind\s*:\s*((\d+\s*-\s*\d+)(\s*,\s*\d+\s*-\s*\d+)*)$)");
  std::smatch match;

  if (std::regex_match(cpubind, match, pattern)) {
    // Extract the content after the colon
    std::string rangesStr = match[1].str();
    // Split the ranges by commas
    std::regex rangePattern(R"(\s*(\d+)\s*-\s*(\d+)\s*)");
    auto words_begin =
        std::sregex_iterator(rangesStr.begin(), rangesStr.end(), rangePattern);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
      std::smatch match = *i;
      if (match.size() == 3) {  // Match should have two groups: start and end
        try {
          int start = std::stoi(match.str(1));
          int end = std::stoi(match.str(2));
          if (end < start) std::swap(start, end);
          if (start < 0 ||
              end >= static_cast<int>(sysconf(_SC_NPROCESSORS_CONF)))
            throw std::invalid_argument("Invalid cpuid range:" + cpubind);
          ranges.push_back({start, end});
        } catch (const std::invalid_argument &e) {
          throw std::invalid_argument(
              "Invalid number format in cpubind string.");
        } catch (const std::out_of_range &e) {
          throw std::out_of_range("Number out of range in cpubind string.");
        }
      }
    }
  } else {
    throw std::invalid_argument(
        "Input string does not match the required cpubind format.");
  }

  return ranges;
}

/** check whether the str has prefix

@param[in] str the string will be checked
@param[in] prefix the match patten

@return  true if patten match, false otherwise
*/
static bool has_prefix(const std::string &str, const std::string &prefix) {
  if (prefix.size() > str.size()) {
    return false;
  }
  auto mismatchPair = std::mismatch(prefix.begin(), prefix.end(), str.begin());
  return mismatchPair.first == prefix.end();
}

/** Get the bind mode from user parameters

@param[in] bind_value the user configed cpu bind info

@return  one of the Tp_cpu_bind_mode_t
*/
static Tp_cpu_bind_mode_t get_bind_mode(std::string bind_value) {
  if (bind_value == TP_NO_BIND) {
    return Tp_cpu_bind_mode_t::TP_NOBIND_MODE;
  } else if (has_prefix(bind_value, TP_CPU_BIND_PREFIX)) {
    return Tp_cpu_bind_mode_t::TP_CPUBIND_MODE;
  }
  return Tp_cpu_bind_mode_t::TP_BIND_INVALID;
}

/** parse user passed cpubind parameters, and check
whether it is valid

@param[in] proposed_value user passed parameters

@return  0 if success, non-zero otherwise
*/
int parse_and_check_cpubind_info(const char *proposed_value) {
  if (proposed_value == nullptr) return 1;
  if (strlen(proposed_value) < strlen(TP_NO_BIND)) return 1;

  const char *start = proposed_value;
  const char *end = proposed_value + strlen(proposed_value) - 1;
  while (std::isspace(*start)) ++start;
  while (std::isspace(*end)) --end;

  std::string bind_value = std::string(start, end + 1);
  std::transform(bind_value.begin(), bind_value.end(), bind_value.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  Tp_cpu_bind_mode_t bind_mode = get_bind_mode(bind_value);

  std::vector<Cpu_range_t> ranges;
  cpu_set_t cpuset;
  bool set_valid = false;
  switch (bind_mode) {
    case Tp_cpu_bind_mode_t::TP_NOBIND_MODE:
      g_available_cpus.clear();
      break;
    case Tp_cpu_bind_mode_t::TP_CPUBIND_MODE:
      try {
        ranges = parse_cpu_bind(bind_value);
      } catch (...) {
        // return 1 here, set variable will be failed
        return 1;
      }

      /** get available cpu info, the process may be limited
          by cgroup or taskset
      */
      CPU_ZERO(&cpuset);
      set_valid = (sched_getaffinity(getpid(), sizeof(cpuset), &cpuset) == 0);

      g_available_cpus.clear();
      for (const auto &range : ranges) {
        for (int i = range.start; i <= range.end; i++) {
          if (set_valid && CPU_ISSET(i, &cpuset)) {
            g_available_cpus.push_back(i);
          }
        }
      }
      break;
    default:
      return 1;
  }
  return 0;
}

/** Bind current thread to cpu

@param[in] thread_group current thread group
*/
void set_current_thread_affinity(thread_group_t *thread_group) {
  int cpu_id = get_cpuid_by_group_index(thread_group->index);
  if (cpu_id >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    int ret =
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (ret != 0) {
      LogErr(WARNING_LEVEL, ER_UNKNOWN_ERROR,
             "pthread_setaffinity_np failed, error code:", ret);
    }
  }
}