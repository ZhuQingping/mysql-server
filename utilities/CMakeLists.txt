# Copyright (c) 2006, 2024, Oracle and/or its affiliates.
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

ADD_WSHADOW_WARNING()
DISABLE_MISSING_PROFILE_WARNING()
MSVC_CPPCHECK_DISABLE()

IF(NOT CMAKE_CROSSCOMPILING)
  MYSQL_ADD_EXECUTABLE(comp_err
    comp_err.cc
    COMPONENT Test
    LINK_LIBRARIES mysys
    )
ENDIF()

MYSQL_ADD_EXECUTABLE(comp_client_err
  comp_client_err.cc
  ${CMAKE_SOURCE_DIR}/libmysql/errmsg.cc
  LINK_LIBRARIES mysys
  SKIP_INSTALL
  )

ADD_CUSTOM_COMMAND(OUTPUT ${PROJECT_BINARY_DIR}/include/mysqlclient_ername.h
  COMMAND comp_client_err
  --in_file=${PROJECT_SOURCE_DIR}/include/errmsg.h
  --out_file=${PROJECT_BINARY_DIR}/include/mysqlclient_ername.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/comp_client_err.cc)

MY_ADD_CUSTOM_TARGET(GenClientError ALL
  DEPENDS ${PROJECT_BINARY_DIR}/include/mysqlclient_ername.h)

FILE(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/${INSTALL_MYSQLSHAREDIR})

# Create localized error message files, <language>/errmsg.sys
#
# errmsg-utf8.txt was split into messages_to_clients.txt and
# messages_to_error_log.txt, respectively.
#
# As the names suggest, this covers messages sent by the server to
# the error-log, and those sent by the server to clients.
# For messages built into the clients (or rather, libmysql / C-API)
# itself, see the MYSQL_ADD_EXECUTABLE(comp_client_err ...) section.
#
# Please see errmsg_readme.txt in the same directory for more information.
#
ADD_CUSTOM_COMMAND(OUTPUT
  ${PROJECT_BINARY_DIR}/include/mysqld_error.h
  ${PROJECT_BINARY_DIR}/include/mysqld_ername.h
  ${PROJECT_BINARY_DIR}/include/mysqld_errmsg.h
  ${PROJECT_BINARY_DIR}/${INSTALL_MYSQLSHAREDIR}/english/errmsg.sys
  COMMAND comp_err
  --charset=${PROJECT_SOURCE_DIR}/share/charsets
  --out-dir=${PROJECT_BINARY_DIR}/${INSTALL_MYSQLSHAREDIR}/
  --header_file=${PROJECT_BINARY_DIR}/include/mysqld_error.h
  --name_file=${PROJECT_BINARY_DIR}/include/mysqld_ername.h
  --errmsg_file=${PROJECT_BINARY_DIR}/include/mysqld_errmsg.h
  --in_file_errlog=${PROJECT_SOURCE_DIR}/share/messages_to_error_log.txt
  --in_file_toclient=${PROJECT_SOURCE_DIR}/share/messages_to_clients.txt
  DEPENDS
  ${PROJECT_SOURCE_DIR}/share/messages_to_error_log.txt
  ${PROJECT_SOURCE_DIR}/share/messages_to_clients.txt
  ${CMAKE_CURRENT_SOURCE_DIR}/comp_err.cc)


MY_ADD_CUSTOM_TARGET(GenError ALL
  DEPENDS
  ${PROJECT_BINARY_DIR}/include/mysqld_error.h
  ${PROJECT_BINARY_DIR}/include/mysqld_ername.h
  ${PROJECT_BINARY_DIR}/include/mysqld_errmsg.h
  ${PROJECT_BINARY_DIR}/${INSTALL_MYSQLSHAREDIR}/english/errmsg.sys
  ${PROJECT_SOURCE_DIR}/share/messages_to_error_log.txt
  ${PROJECT_SOURCE_DIR}/share/messages_to_clients.txt)

MYSQL_ADD_EXECUTABLE(range_check_err
  range_check_err.cc
  DEPENDENCIES GenError
  LINK_LIBRARIES mysys
  SKIP_INSTALL
  )

# Set InnoDB mutex type
ADD_DEFINITIONS(-DMUTEX_EVENT)

# The copied shared libraries must be available at build time.
IF(TARGET copy_openssl_dlls)
  ADD_DEPENDENCIES(GenError copy_openssl_dlls)
  ADD_DEPENDENCIES(comp_client_err copy_openssl_dlls)
  ADD_DEPENDENCIES(comp_err copy_openssl_dlls)
ENDIF()

MYSQL_ADD_EXECUTABLE(static_thread_local_test
  static_thread_local_test.cc
  SKIP_INSTALL
  EXCLUDE_FROM_ALL
  )

MYSQL_ADD_EXECUTABLE(my_print_defaults
  my_print_defaults.cc
  COMPONENT Server
  LINK_LIBRARIES mysys
  )
MYSQL_ADD_EXECUTABLE(perror
  perror.cc
  COMPONENT Server
  DEPENDENCIES GenError
  LINK_LIBRARIES mysys
  )
IF(WITH_LZ4 STREQUAL "bundled")
  MYSQL_ADD_EXECUTABLE(lz4_decompress
    lz4_decompress.cc
    COMPONENT Server
    LINK_LIBRARIES ext::lz4 mysys
    )
ENDIF()

MYSQL_ADD_EXECUTABLE(zlib_decompress
  zlib_decompress.cc
  COMPONENT Server
  LINK_LIBRARIES ext::zlib mysys
  )

# All targets below belong to COMPONENT Server and depend on InnoDB.
IF(WITHOUT_SERVER)
  RETURN()
ENDIF()

ADD_DEFINITIONS(-DHAVE_LZ4=1)

# Add path to the InnoDB headers and DD
INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/storage/innobase
  ${CMAKE_SOURCE_DIR}/storage/innobase/include
  ${CMAKE_SOURCE_DIR}/sql
  )

# We use the InnoDB code directly in case the code changes.
MYSQL_ADD_EXECUTABLE(innochecksum
  innochecksum.cc
  COMPONENT Server
  DEPENDENCIES GenError
  LINK_LIBRARIES mysys innodb_zipdecompress ext::lz4 extra::rapidjson
  )
TARGET_COMPILE_DEFINITIONS(innochecksum PRIVATE UNIV_NO_ERR_MSGS UNIV_LIBRARY)
IF(MY_COMPILER_IS_GNU_OR_CLANG)
  ADD_COMPILE_FLAGS(innochecksum.cc
    COMPILE_FLAGS "-Wno-unused-parameter -Wno-cast-qual")
ENDIF()

# This compiler says -Werror=stringop-overflow= for:
# memcmp(&space_id, buf.begin() + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 4)
IF(MY_COMPILER_IS_GNU_8)
  ADD_COMPILE_FLAGS(innochecksum.cc COMPILE_FLAGS "-Wno-stringop-overflow")
  MY_TARGET_LINK_OPTIONS(innochecksum "-Wno-stringop-overflow")
ENDIF()

MYSQL_ADD_EXECUTABLE(ibd2sdi
  ibd2sdi.cc
  COMPONENT Server
  DEPENDENCIES GenError
  LINK_LIBRARIES mysys innodb_zipdecompress ext::lz4 extra::rapidjson
  )
TARGET_COMPILE_DEFINITIONS(ibd2sdi PRIVATE UNIV_NO_ERR_MSGS UNIV_LIBRARY DISABLE_PSI_MEMORY)
IF(MY_COMPILER_IS_GNU_OR_CLANG)
  ADD_COMPILE_FLAGS(ibd2sdi.cc
    COMPILE_FLAGS "-Wno-unused-parameter -Wno-cast-qual")
ENDIF()

IF(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    SET(DSTORE_PLATFORM "euleros2.5_x86_64")
    MESSAGE(STATUS "Building on x86_64 architecture")
ELSEIF(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    SET(DSTORE_PLATFORM "euleros2.9_aarch64")
    MESSAGE(STATUS "Building on ARM architecture")
ELSE()
    MESSAGE(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
ENDIF()

SET(CDE_SRC_ROOT ${CMAKE_SOURCE_DIR}/storage/dstore)
IF(DSTORE)
  SET(CDE_INC
    ${CDE_SRC_ROOT}
    ${DSTORE}/dstore/utils/output/include
    ${DSTORE}/dstore/include
    ${DSTORE}/dstore/interface
  )
ELSE()
  SET(CDE_INC
    ${CDE_SRC_ROOT}
    ${CDE_SRC_ROOT}/dstore/include
    ${CDE_SRC_ROOT}/dstore/interface
    ${CDE_SRC_ROOT}/dstore/3rdparty/include
  )
ENDIF()

STRING(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPER)

IF(NOT(CMAKE_BUILD_TYPE_UPPER STREQUAL "DEBUG"))
  SET(DSTORE_LIB_PATH ${CDE_SRC_ROOT}/dstore/${DSTORE_PLATFORM}/lib/release)
  SET(DSTORE_UTIL_LIB_PATH ${CDE_SRC_ROOT}/dstore/${DSTORE_PLATFORM}/utils/lib/release)
ELSE()
  SET(DSTORE_LIB_PATH ${CDE_SRC_ROOT}/dstore/${DSTORE_PLATFORM}/lib/debug)
  SET(DSTORE_UTIL_LIB_PATH ${CDE_SRC_ROOT}/dstore/${DSTORE_PLATFORM}/utils/lib/debug)
ENDIF()

IF(DSTORE)
  MYSQL_ADD_EXECUTABLE(backup_monitor
    backup_monitor.cc
    ${CDE_SRC_ROOT}/backup/full_backup_meta_json.cc
    ${CDE_SRC_ROOT}/backup/local_backup.cc
    ${CDE_SRC_ROOT}/backup/local_backup_file_mgr.cc
    ${CDE_SRC_ROOT}/backup/local_backup_binlog_file_mgr.cc
    ${CMAKE_SOURCE_DIR}/sql/local_backup/local_backup_file_utils.cc
    COMPONENT Server
    DEPENDENCIES GenError cde
    INCLUDE_DIRECTORIES ${CDE_INC}
    LINK_LIBRARIES mysys innodb_zipdecompress ext::lz4 extra::rapidjson
    )
  
  TARGET_LINK_DIRECTORIES(backup_monitor PRIVATE ${DSTORE}/local_libs/cjson/lib)

  TARGET_LINK_LIBRARIES(backup_monitor
    ${CMAKE_BINARY_DIR}/storage/dstore/libcde.a
    ${LIBSECUREC}
    ${DSTORE}/dstore/utils/output/lib/libgsutils.so
  )
ELSE()
  MYSQL_ADD_EXECUTABLE(backup_monitor
    backup_monitor.cc
    ${CDE_SRC_ROOT}/backup/full_backup_meta_json.cc
    ${CDE_SRC_ROOT}/backup/local_backup.cc
    ${CDE_SRC_ROOT}/backup/local_backup_file_mgr.cc
    ${CDE_SRC_ROOT}/backup/local_backup_binlog_file_mgr.cc
    ${CMAKE_SOURCE_DIR}/sql/local_backup/local_backup_file_utils.cc
    COMPONENT Server
    DEPENDENCIES GenError
    INCLUDE_DIRECTORIES ${CDE_INC}
    LINK_LIBRARIES mysys innodb_zipdecompress ext::lz4 extra::rapidjson
    )
  TARGET_LINK_LIBRARIES(backup_monitor
    ${DSTORE_LIB_PATH}/libdstore.so
    ${LIBSECUREC}
    ${DSTORE_UTIL_LIB_PATH}/libgsutils.so
  )
ENDIF()

# backup_monitor depends on dstore
ADD_INSTALL_RPATH_FOR_DSTORE(backup_monitor)

TARGET_COMPILE_DEFINITIONS(backup_monitor PRIVATE UNIV_NO_ERR_MSGS UNIV_LIBRARY DISABLE_PSI_MEMORY IS_DSTORE_BACKUP_TOOL)
IF(MY_COMPILER_IS_GNU_OR_CLANG)
  ADD_COMPILE_FLAGS(backup_monitor.cc
    COMPILE_FLAGS "-Wno-unused-parameter -Wno-cast-qual -Wno-suggest-override")
ENDIF()

MYSQL_ADD_EXECUTABLE(backup_download
  backup_download.cc
  ${CMAKE_SOURCE_DIR}/sql/local_backup/local_backup_file_utils.cc
  ${CMAKE_SOURCE_DIR}/sql/local_backup/local_backup_obs_download_utils.cc
  ${CMAKE_SOURCE_DIR}/sql/local_backup/local_backup_obs_download.cc
  ${CMAKE_SOURCE_DIR}/sql/local_backup/local_backup_obs_handler.cc
  ${CMAKE_SOURCE_DIR}/sql/local_backup/local_backup_obs_utils.cc
  ${CMAKE_SOURCE_DIR}/sql/scc_util.cc
  COMPONENT Server
  DEPENDENCIES GenError
  LINK_LIBRARIES mysys innodb_zipdecompress ext::lz4 extra::rapidjson
  )
IF(SUPPORT_OBS)
  STRING(TOUPPER "${CMAKE_SYSTEM_PROCESSOR}" PROCESSOR_UPPER)
  IF(PROCESSOR_UPPER STREQUAL "ARM" OR PROCESSOR_UPPER STREQUAL "AARCH64")
    SET(OBS_DEP_TYPE "arm")
    SET(OBS_LOG_DEP_TYPE "aarch64")
  ELSE()
    SET(OBS_DEP_TYPE "linux")
    SET(OBS_LOG_DEP_TYPE "linux_64")
  ENDIF()
  SET(OBS_DEP_LINK_DIRECTORIES
    ${OBS_PATH}/build/script/Provider/build/${OBS_DEP_TYPE}/cjson-1.7.18/lib
    ${OBS_PATH}/build/script/Provider/build/${OBS_DEP_TYPE}/iconv-1.15/lib
    ${OBS_PATH}/build/script/Provider/build/${OBS_DEP_TYPE}/spdlog-1.12.0/lib
    ${OBS_PATH}/build/script/Provider/build/${OBS_DEP_TYPE}/libxml2-2.9.9/lib
    ${OBS_PATH}/build/script/Provider/build/${OBS_DEP_TYPE}/pcre-8.45/lib
    ${OBS_PATH}/platform/eSDK_LogAPI_V2.1.10/C/${OBS_LOG_DEP_TYPE}
  )
  TARGET_LINK_DIRECTORIES(backup_download PUBLIC ${OBS_DEP_LINK_DIRECTORIES})
  TARGET_LINK_LIBRARIES(backup_download libobs curl_interface pcre cjson iconv spdlog xml2 eSDKLogAPI)
  TARGET_COMPILE_DEFINITIONS(backup_download PRIVATE SUPPORT_OBS)
ENDIF()
TARGET_INCLUDE_DIRECTORIES(backup_download PRIVATE "/usr/local/seccomponent/include")
TARGET_LINK_LIBRARIES(backup_download
  ${LIBSECUREC}
)
TARGET_COMPILE_DEFINITIONS(backup_download PRIVATE UNIV_NO_ERR_MSGS UNIV_LIBRARY DISABLE_PSI_MEMORY IS_DSTORE_BACKUP_TOOL DISABLE_ERROR_LOGGING)
IF(MY_COMPILER_IS_GNU_OR_CLANG)
  ADD_COMPILE_FLAGS(backup_download.cc
    COMPILE_FLAGS "-Wno-unused-parameter -Wno-cast-qual")
ENDIF()
