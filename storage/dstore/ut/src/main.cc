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

#include "my_sys.h"
#include "utils/ut_cde_test.h"

CHARSET_INFO *system_charset_info = nullptr;

GTEST_API_ int main(int argc, char **argv) {
  printf("Running main() from %s\n", __FILE__);

  MY_INIT("dstore-test");

  CDE::CDETEST::InitOnce();
  testing::InitGoogleTest(&argc, argv);
  int exit_code = RUN_ALL_TESTS();
  CDE::CDETEST::Destroy();

  return exit_code;
}
