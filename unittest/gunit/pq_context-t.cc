/* Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "my_config.h"

#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include "sql/parallel_query/pq_context.h"

namespace pq_context_unittest {

TEST(PQThdContext, ErrorSignalIsVisibleAndResetsAtStatementBoundary) {
  PQ_thd_context context;
  std::atomic<bool> start{false};

  std::thread worker([&context, &start] {
    while (!start.load(std::memory_order_relaxed)) {
    }
    context.set_error();
  });

  start.store(true, std::memory_order_relaxed);
  while (!context.has_error()) {
  }
  worker.join();

  context.clear_error_after_workers_join();
  EXPECT_FALSE(context.has_error());
}

}  // namespace pq_context_unittest

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
