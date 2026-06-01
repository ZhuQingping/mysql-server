/*
  Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef __UT_CDE_TEST_H__
#define __UT_CDE_TEST_H__

/*
 * there is a Max(x, y) in types/data_types.h
 * and it is conflict with gtest-internal.h::Max() function
 * so I undef Max there
 */
#undef Max
#include "gtest/gtest.h"

namespace CDE {
class CDETEST : public ::testing::Test {
 public:
  CDETEST() {}
  virtual ~CDETEST() {}

  static void InitOnce();

  static void Destroy();

  static void Bootstrap();

  static void Start();

  static void Stop();

  static void StartSession();

  static void StopSession();

  static void SetUpTestCase();

  static void TearDownTestCase();

  static void PrintTestCaseName();

  void SetUp() override;

  void TearDown() override;
};

// Singleton
class ut_cde_cfg {
 public:
  static void init_file_path();
  static void deinit_file_path();
  static inline const char *get_start_cfg_path() { return m_start_cfg_fp.c_str(); }
  static inline const char *get_root_path() { return m_root_fp.c_str(); }
  static inline const char *get_data_path() { return m_data_fp.c_str(); }
  static inline const char *get_log_path() { return m_log_fp.c_str(); }
  static inline const char *get_dstore_path() { return m_dstore_fp.c_str(); }
  static inline void reset_root_path(std::string path);

 private:
  static std::string m_root_fp;
  static std::string m_start_cfg_fp;
  static std::string m_data_fp;
  static std::string m_log_fp;
  static std::string m_md_fp;
  static std::string m_wal_fp;
  static std::string m_dstore_fp;
};

} /* namespace CDE */
#endif  // __UT_CDE_TEST_H__
