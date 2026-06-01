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

IF(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    SET(DSTORE_PLATFORM "euleros2.5_x86_64")
    MESSAGE(STATUS "Building on x86_64 architecture")
ELSEIF(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    SET(DSTORE_PLATFORM "euleros2.9_aarch64")
    MESSAGE(STATUS "Building on ARM architecture")
ELSE()
    MESSAGE(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
ENDIF()

MESSAGE(STATUS "Using EulerOS directory: ${DSTORE_PLATFORM}")

STRING_APPEND(CMAKE_CXX_FLAGS " -Wno-error=suggest-override -Wno-error=extra-semi -Wno-error=suggest-attribute=format")

SET(CDE_SOURCES
  common/ha_cde_debug.cc
  common/cde_alloc.cc
  common/cde_def.cc
  common/cde_errorcode.cc
  common/cde_typecache.cc
  common/cde_rpl_wal.cc
  common/cde_compare_utils.cc
  common/cde_session.cc
  common/cde_srv.cc
  common/cde_diagnose.cc
  common/cde_trxmgr.cc
  common/cde_relation_cache.cc
  boot/cde_instance.cc
  dml/cde_dml.cc
  dml/cde_dml_ctx.cc
  dml/cde_heap.cc
  dml/cde_btree.cc
  dict/cde_autoinc.cc
  dict/cde_dict.cc
  dict/cde_dict_bg_stat.cc
  dict/cde_dict_bg_evict.cc
  dict/cde_dict_stat.cc
  dict/cde_relation.cc
  dict/cde_stats_sampler.cc
  ddl/cde_ddl.cc
  ddl/cde_ddl_log.cc
  ddl/cde_online_ddl.cc
  ddl/cde_tuple.cc
  ddl/cde_dd_table.cc
  ddl/cde_mysql_ops.cc
  ddl/dd_helper.cc
  ddl/ha_cde_alter.cc
  ddl/cde_tablespace.cc
  ddl/cde_heap_builder.cc
  backup/full_backup_meta_json.cc
  backup/local_backup.cc
  backup/local_backup_file_mgr.cc
  backup/local_backup_binlog_file_mgr.cc
  backup/local_backup_obs_object.cc
  rpl_wal/cde_pdb_replica.cc
  )

SET(CDE_HA_SOURCE handler/ha_cde.cc handler/ha_cde_cond.cc handler/ha_cde_parallel.cc handler/cde_information_schema.cc handler/ha_cdepart.cc)
SET(CDE_ALL_SOURCES ${CDE_SOURCES} ${CDE_HA_SOURCE})

option(ENABLE_HOT_PERF_COUNTERS "Enable perf counters in hot functions (default: OFF)" OFF)
if(ENABLE_HOT_PERF_COUNTERS)
    add_compile_definitions(CDE_ENABLE_HOT_PERF_COUNTERS)
endif()

set(PERFCOUNTER_SOURCE
    perf/perfcounters.cc
    perf/perfpublisher.cc
    perf/memstats.cc
    perf/stringutils.cc
    )

STRING(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPER)
SET(CDE_SRC_ROOT ${CMAKE_SOURCE_DIR}/storage/dstore)
ADD_DEFINITIONS(-DMYSQL_SERVER)
ADD_DEFINITIONS(-DLOG_SUBSYSTEM_TAG="DSTORE")
option(BUILD_PAGEDUMP_WALDUMP "Build Pagedump and Waldump" OFF)
IF(DSTORE)
  include(dstore)
  SET(UTILS_LIB_PATH ${DSTORE}/dstore/utils/output/lib)
  IF(BUILD_PAGEDUMP_WALDUMP)
    include(cmake/dstore_pagedump.cmake)
    include(cmake/dstore_waldump.cmake)
  ENDIF()
  IF(CMAKE_BUILD_TYPE_UPPER STREQUAL "DEBUG")
    include(dstore_damagepage)
  ENDIF()
ELSE()
  IF(NOT(CMAKE_BUILD_TYPE_UPPER STREQUAL "DEBUG"))
    SET(DSTORE_LIB_PATH ${CMAKE_SOURCE_DIR}/storage/dstore/dstore/${DSTORE_PLATFORM}/lib/release)
    SET(UTILS_LIB_PATH ${CMAKE_SOURCE_DIR}/storage/dstore/dstore/${DSTORE_PLATFORM}/utils/lib/release)
  ELSE()
    SET(DSTORE_LIB_PATH ${CMAKE_SOURCE_DIR}/storage/dstore/dstore/${DSTORE_PLATFORM}/lib/debug)
    SET(UTILS_LIB_PATH ${CMAKE_SOURCE_DIR}/storage/dstore/dstore/${DSTORE_PLATFORM}/utils/lib/debug)
  ENDIF()
  SET(DSTORE_BIN_PATH ${CMAKE_SOURCE_DIR}/storage/dstore/dstore/${DSTORE_PLATFORM}/bin)
  SET(CDE_3RD_LIB_PATH
    ${UTILS_LIB_PATH}
  )
  SET(CDE_3RD_INC_PATH ${CMAKE_SOURCE_DIR}/storage/dstore/dstore/3rdparty/include)

  # gcc lib has asan lib
  #set(ASAN_HOME ${CDE_3RD_PATH}/asan)
  #set(ASAN_LIB_PATH ${ASAN_HOME}/lib)
  #MESSAGE("-- ASAN_HOME: ${ASAN_HOME}")

  SET(CDE_BUILD_ROOT ${CMAKE_BINARY_DIR}/storage/dstore)
  SET(CDE_CFG_DIR ${CDE_BUILD_ROOT}/config)

  SET(CDE_DSTORE_INC
    ${CDE_SRC_ROOT}/dstore/interface
    ${CDE_SRC_ROOT}/dstore/include
    ${CDE_SRC_ROOT}/dstore/3rdparty/include
  )

  SET(CDE_MYSQL_INC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/sql
    ${CMAKE_SOURCE_DIR}
  )

  SET(CDE_INC
    ${CMAKE_SOURCE_DIR}/storage/dstore
    ${CMAKE_SOURCE_DIR}/storage/dstore/perf/include
    ${CDE_DSTORE_INC}
    ${CDE_MYSQL_INC}
    ${CDE_3RD_INC_PATH}
  )

  INCLUDE_DIRECTORIES(
    ${CDE_INC}
    ${BOOST_INCLUDE_DIR}
  )

  add_library(counter STATIC  ${PERFCOUNTER_SOURCE})


  ADD_LIBRARY(cde_lib STATIC ${CDE_SOURCES})
  ADD_DEPENDENCIES(cde_lib GenError)

  FILE(GLOB CDE_3RD_LIBS
    "${DSTORE_LIB_PATH}/*"
    "${UTILS_LIB_PATH}/*"
  )

  FILE(GLOB CDE_CFG_FILES
    "${CDE_CFG_DIR}/*"
  )

  TARGET_LINK_DIRECTORIES(cde_lib PUBLIC ${DSTORE_LIB_PATH} ${CDE_3RD_LIB_PATH})

  SET(CDE_LINK_LIBS dstore.a ${LIBSECUREC} gsutils counter communication extra::rapidjson ext::zlib)

  IF(ASAN)
      SET(CDE_LINK_LIBS ${CDE_LINK_LIBS} asan)
      SET(ASAM_FLAGS "-static-libasan -fsanitize=address -fsanitize-recover=address -fno-omit-frame-pointer -fsanitize-address-use-after-scope")
      SET(CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS}  ${ASAM_FLAGS}")
      SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  ${ASAM_FLAGS}")
  ENDIF()

  TARGET_LINK_LIBRARIES(cde_lib ${CDE_LINK_LIBS})

  MYSQL_ADD_PLUGIN(cde ${CDE_HA_SOURCE}
    STORAGE_ENGINE
    DEFAULT
    LINK_LIBRARIES cde_lib
  )

  IF(CMAKE_BUILD_TYPE_UPPER STREQUAL "DEBUG")
    FILE(COPY "${CMAKE_SOURCE_DIR}/storage/dstore/dstore/${DSTORE_PLATFORM}/bin/debug/damagepage"
      DESTINATION ${CMAKE_BINARY_DIR}/runtime_output_directory FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
    )
    INSTALL(FILES "${CMAKE_SOURCE_DIR}/storage/dstore/dstore/${DSTORE_PLATFORM}/bin/debug/damagepage"
      DESTINATION ${INSTALL_BINDIR} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
    )
  ENDIF()
ENDIF()

# directory that dstore needed
SET(CDE_INSTALL_DIR_LIST
  ${CDE_CFG_DIR}
)

foreach(DIR ${CDE_INSTALL_DIR_LIST})
    message(STATUS "Creating directory: ${DIR}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DIR}
    )
endforeach()

IF(WITH_UNIT_TESTS)
  ADD_SUBDIRECTORY(ut)
ENDIF()
# install dstore and 3rd so
INSTALL(FILES ${CDE_3RD_LIBS} DESTINATION ${INSTALL_LIBDIR}/dstore)

SET(CDE_CFG_FILES
${CDE_SRC_ROOT}/config/tenant_isoland_start.json
)

# Copy dstore tenant_isoland_start config files from source code directory during cmake stage, and update the copied file, keep the code files unchanged.
FILE(COPY ${CDE_CFG_FILES} DESTINATION ${CDE_CFG_DIR})
# use libvfslinuxadapter.so in source code directory
EXECUTE_PROCESS(COMMAND sed -i "s#\"clientLibPath\":\\s*\"[^\"]*\"#\"clientLibPath\": \"${UTILS_LIB_PATH}/libvfslinuxadapter.so\"#" ${CDE_CFG_DIR}/tenant_isoland_start.json)


# Copy dstore tenant_isoland_start config files from build path to install path
INSTALL(FILES ${CDE_CFG_FILES} DESTINATION ${INSTALL_LIBDIR}/dstore)
# update the copied file in install path, and all the files are located in the installation directory
SET(DSTORE_INSTALL_DIR  ${CMAKE_INSTALL_PREFIX}/${INSTALL_LIBDIR}/dstore)
INSTALL(CODE
"execute_process(
    COMMAND sed -i
        \"s#\\\"clientLibPath\\\":\\\\s*\\\"[^\\\"]*\\\"#\\\"clientLibPath\\\": \\\"${DSTORE_INSTALL_DIR}/libvfslinuxadapter.so\\\"#\"
        \"${DSTORE_INSTALL_DIR}/tenant_isoland_start.json\"
)"
)
