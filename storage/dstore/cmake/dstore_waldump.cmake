# Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is designed to work with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have either included with
# the program or referenced in the documentation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# build dstore tool: waldump
 
FILE(GLOB_RECURSE waldump_src_list
    "${DSTORE}/dstore/tools/waldump/src/*.cpp"
)
list(APPEND waldump_src_list ${dstore_src_list})
set(waldump_compile_options ${PROTECT_OPTIONS} ${WARNING_OPTIONS} ${OPTIMIZE_OPTIONS} ${CHECK_OPTIONS} ${BIN_SECURE_OPTIONS})
set(waldump_link_options ${BIN_LINK_OPTIONS})
set(waldump_link_libs ${LIBSECUREC} gsutils ssl crypto communication ext::lz4)
set(waldump_macro_options ${MACRO_OPTIONS})
set(waldump_include_directories
    ${DSTORE}/dstore/interface
    ${DSTORE}/dstore/include
    ${SECURE_INCLUDE_PATH}
    ${DSTORE}/dstore/utils/output/include
)
set(waldump_link_directories
    ${SECURE_LIB_PATH} ${DSTORE}/dstore/utils/output/lib
    ${DSTORE_3RD_PATH}/kernel/dependency/openssl/comm/lib
)
 
MYSQL_ADD_EXECUTABLE(waldump
    ${waldump_src_list}
    INCLUDE_DIRECTORIES ${waldump_include_directories}
    LINK_LIBRARIES ${waldump_link_libs}
)
TARGET_LINK_DIRECTORIES(waldump PRIVATE ${waldump_link_directories})
