# Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.

# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA

INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/plugin/threadpool
  ${CMAKE_SOURCE_DIR}/sql/conn_handler
)


MYSQL_ADD_EXECUTABLE(cpubind-t cpubind-t.cc
  ADD_TEST cpubind
  LINK_LIBRARIES gunit_large server_unittest_library
  COMPILE_DEFINITIONS "${DISABLE_PSI_DEFINITIONS}")

MYSQL_ADD_EXECUTABLE(conn_handler-t conn_handler-t.cc
  ADD_TEST conn_handler
  LINK_LIBRARIES gunit_large server_unittest_library
  COMPILE_DEFINITIONS "${DISABLE_PSI_DEFINITIONS}")

MYSQL_ADD_EXECUTABLE(threadpool_schedule-t threadpool_schedule-t.cc
  ADD_TEST threadpool_schedule
  LINK_LIBRARIES gunit_large server_unittest_library
  COMPILE_DEFINITIONS "${DISABLE_PSI_DEFINITIONS}")
