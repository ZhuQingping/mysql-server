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

MYSQL_ADD_EXECUTABLE(dstore_ddl_info-t
  dstore_ddl_info-t.cc
  ${CMAKE_SOURCE_DIR}/sql/sql_prepare.cc
  ENABLE_EXPORTS
  ADD_TEST dstore_ddl_info-t
  LINK_LIBRARIES gunit_large server_unittest_library
)

MYSQL_ADD_EXECUTABLE(ddl_info_file-t
  ddl_info_file-t.cc
  ${CMAKE_SOURCE_DIR}/sql/ddl_info_file.cc
  ${CMAKE_SOURCE_DIR}/sql/log.cc
  ENABLE_EXPORTS
  ADD_TEST ddl_info_file-t
  LINK_LIBRARIES gunit_small ${LIBSECUREC} server_unittest_library
)
