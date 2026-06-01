/*
   Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

/* See http://code.google.com/p/googletest/wiki/Primer */
#include <gtest/gtest.h>
#include <cstring>

#include "my_dbug.h"
#include "my_inttypes.h"
#include "mysql/service_mysql_alloc.h"

extern int parse_and_check_cpubind_info(const char *proposed_value);
extern int get_cpuid_by_group_index(int index);
extern int parse_cpu_bind_info(const char *cpubind_info);
extern char *threadpool_cpubind_info;

void my_assert_handler(const char *, const char *, int) {}
TEST(cpubind, cpubindtest) {
  int ret = parse_and_check_cpubind_info("cpubind:0-1");
  EXPECT_EQ(ret, 0);
  ret = parse_and_check_cpubind_info("CpUbind : 0-1");
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(get_cpuid_by_group_index(0), 0);
  EXPECT_EQ(get_cpuid_by_group_index(1), 1);
  EXPECT_EQ(get_cpuid_by_group_index(2), 0);
  EXPECT_EQ(get_cpuid_by_group_index(3), 1);
  EXPECT_EQ(get_cpuid_by_group_index(4), 0);
  ret = parse_and_check_cpubind_info("CpUbind : 2- 3");
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(get_cpuid_by_group_index(0), 2);
  EXPECT_EQ(get_cpuid_by_group_index(1), 3);
  EXPECT_EQ(get_cpuid_by_group_index(2), 2);
  EXPECT_EQ(get_cpuid_by_group_index(3), 3);
  EXPECT_EQ(get_cpuid_by_group_index(4), 2);

  ret = parse_and_check_cpubind_info("CpUbind : 0-1, 3- 2");
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(get_cpuid_by_group_index(0), 0);
  EXPECT_EQ(get_cpuid_by_group_index(1), 1);
  EXPECT_EQ(get_cpuid_by_group_index(2), 2);
  EXPECT_EQ(get_cpuid_by_group_index(3), 3);
  EXPECT_EQ(get_cpuid_by_group_index(4), 0);

  // start > end
  ret = parse_and_check_cpubind_info("CpUbind : 3- 2");
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(get_cpuid_by_group_index(0), 2);
  EXPECT_EQ(get_cpuid_by_group_index(1), 3);
  EXPECT_EQ(get_cpuid_by_group_index(2), 2);
  EXPECT_EQ(get_cpuid_by_group_index(3), 3);
  EXPECT_EQ(get_cpuid_by_group_index(4), 2);

  // start == end
  ret = parse_and_check_cpubind_info("CpUbind : 1-1");
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(get_cpuid_by_group_index(0), 1);
  EXPECT_EQ(get_cpuid_by_group_index(1), 1);
  EXPECT_EQ(get_cpuid_by_group_index(2), 1);
  EXPECT_EQ(get_cpuid_by_group_index(3), 1);

  // invalid param test
  ret = parse_and_check_cpubind_info("CpU bind : 3- 2");
  EXPECT_TRUE(ret != 0);
  ret = parse_and_check_cpubind_info("cpubind : -1-5");
  EXPECT_TRUE(ret != 0);
  ret = parse_and_check_cpubind_info("cpubind : 1-1000");
  EXPECT_TRUE(ret != 0);

  // just for coverage.
  ret = parse_cpu_bind_info("cpubind : 1-1000");
  EXPECT_EQ(ret, 0);

  threadpool_cpubind_info = nullptr;
  ret = parse_cpu_bind_info("cpubind: 1");
  EXPECT_EQ(ret, 0);
  EXPECT_STREQ("nobind", threadpool_cpubind_info);

  threadpool_cpubind_info =
      my_strdup(PSI_NOT_INSTRUMENTED, "CpU bind : 3- 2000", MYF(0));
  ret = parse_cpu_bind_info(threadpool_cpubind_info);
  EXPECT_EQ(ret, 0);
  EXPECT_STREQ("nobind", threadpool_cpubind_info);

  // Inject out of memory
  DBUG_SET("+d,simulate_out_of_memory");
  ret = parse_cpu_bind_info("cpubind: 1");
  DBUG_SET("-d,simulate_out_of_memory");
  EXPECT_NE(ret, 0);
}
