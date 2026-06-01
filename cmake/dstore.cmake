# first check DSTORE_THIRD/DSTORE_UTILS/DSTORE repo exist
# pre generate file and adjust compile flags that dstore needed
# compile with DSTORE code together and generate libcde.a

IF(NOT EXISTS ${DSTORE}/dstore)
    message(FATAL_ERROR "dstore not exist, please download first from 
    https://dstore.git")
ENDIF()

include(${DSTORE}/dstore/options.cmake)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DSTORE_USE_ASSERT_CHECKING ON)
endif()

if(ASAN)
    list(APPEND MACRO_OPTIONS -DMEMCHECK)
endif()

if(WITH_TSAN)
    add_compile_definitions(__SANTIZE_THREAD__)
    add_compile_definitions(ENABLE_THREAD_CHECK)
    add_compile_definitions(_REENTRANT)
endif()

configure_file (
  "${DSTORE}/dstore/cmake/config.h.in"
  "${DSTORE}/dstore/include/config.h"
)
execute_process(COMMAND perl generrorid -s ${DSTORE}/dstore/src
                WORKING_DIRECTORY ${DSTORE}/dstore/src)

execute_process(COMMAND perl gentraceid -s ${DSTORE}/dstore/src/common/instrument/trace -t ${DSTORE}/dstore/include/common/instrument/trace
                WORKING_DIRECTORY ${DSTORE}/dstore/src/common/instrument/trace)

execute_process(
    COMMAND perl  generate-wait_state_types.pl  wait_state_names.txt 
    WORKING_DIRECTORY ${DSTORE}/dstore/src/common/instrument/wait_event
)

execute_process(
    COMMAND cp ${DSTORE}/dstore/src/common/instrument/wait_event/wait_state.h 
    ${DSTORE}/dstore/interface/framework/dstore_wait_state.h
)

execute_process(
    COMMAND perl  generate-io_event_types.pl  io_event_names.txt 
    WORKING_DIRECTORY ${DSTORE}/dstore/src/common/instrument/wait_event
)

execute_process(
    COMMAND cp ${DSTORE}/dstore/src/common/instrument/wait_event/io_event.h 
    ${DSTORE}/dstore/interface/framework/dstore_io_event.h
)

STRING_APPEND(CMAKE_CXX_FLAGS " -Wno-error=register")
STRING_APPEND(CMAKE_CXX_FLAGS " -Wno-error=implicit-fallthrough")
STRING_APPEND(CMAKE_CXX_FLAGS " -Wno-error=cast-qual")
STRING_APPEND(CMAKE_CXX_FLAGS " -Wno-error=suggest-attribute=format")

IF(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  STRING_APPEND(CMAKE_CXX_FLAGS " -msse4.2 -mcx16")
ELSEIF(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -march=armv8-a+crc+crypto")
  IF (CMAKE_BUILD_TYPE AND (CMAKE_BUILD_TYPE STREQUAL "release"))
    STRING_APPEND(CMAKE_CXX_FLAGS " -march=armv8-a+crc+lse")
  ELSE()
    STRING_APPEND(CMAKE_CXX_FLAGS " -march=armv8-a+crc")
  ENDIF()
ENDIF()

MACRO(SUBSRCLIST result curdir)
  FILE(GLOB_RECURSE children RELATIVE ${curdir} ${curdir}/*)
  SET(dirlist "")
  FOREACH(child ${children})
      LIST(APPEND srclist ${curdir}/${child})
  ENDFOREACH()
  SET(${result} ${srclist})
ENDMACRO()

SUBSRCLIST(dstore_src_list ${DSTORE}/dstore/src)
list(FILTER dstore_src_list EXCLUDE REGEX ".*/flashback/.*")

SET(DSTORE_3RD_PATH ${DSTORE}/local_libs)

list(APPEND CDE_ALL_SOURCES ${dstore_src_list})

SET(CDE_BUILD_ROOT ${CMAKE_BINARY_DIR}/storage/dstore)
SET(CDE_BIN_DIR ${CDE_BUILD_ROOT}/bin)
SET(CDE_CFG_DIR ${CDE_BUILD_ROOT}/config)
SET(CDE_LIB_DIR ${CDE_BUILD_ROOT}/lib)

INCLUDE_DIRECTORIES(
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/sql
        ${BOOST_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/storage/dstore
        ${CMAKE_SOURCE_DIR}/storage/dstore/perf/include
        SYSTEM ${DSTORE}/dstore/utils/output/include
        SYSTEM ${DSTORE}/dstore/include
        SYSTEM ${DSTORE}/dstore/interface
)

add_library(counter STATIC ${PERFCOUNTER_SOURCE})

add_library(dstore_tmp STATIC ${dstore_src_list})

SET(CDE_LINK_LIBS ${LIBSECUREC} gsutils counter ssl crypto extra::rapidjson)

MYSQL_ADD_PLUGIN(cde ${CDE_ALL_SOURCES}
  STORAGE_ENGINE
  DEFAULT
  LINK_LIBRARIES ${CDE_LINK_LIBS}
)

TARGET_LINK_DIRECTORIES(cde PUBLIC ${DSTORE}/dstore/utils/output/lib ${DSTORE_3RD_PATH}/openssl/lib)

FILE(GLOB CDE_DEPEND_LIBS
  "${DSTORE}/dstore/utils/output/lib/*"
)

INSTALL(FILES ${DSTORE_3RD_PATH}/openssl/lib/libcrypto.so DESTINATION ${INSTALL_LIBDIR}/dstore)
INSTALL(FILES ${DSTORE_3RD_PATH}/openssl/lib/libssl.so DESTINATION ${INSTALL_LIBDIR}/dstore)
INSTALL(FILES ${CDE_DEPEND_LIBS} DESTINATION ${INSTALL_LIBDIR}/dstore)
