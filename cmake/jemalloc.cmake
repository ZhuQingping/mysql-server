# old cmake does not have ExternalProject file
IF(CMAKE_VERSION VERSION_LESS 3.11.2)
  MACRO (CHECK_JEMALLOC)
  ENDMACRO()
  RETURN()
ENDIF()

INCLUDE(ExternalProject)

MACRO (USE_BUNDLED_JEMALLOC)
  SET(SOURCE_DIR "${CMAKE_SOURCE_DIR}/extra/jemalloc")
  SET(BINARY_DIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/extra/jemalloc/build")
  SET(LIBJEMALLOC "libjemalloc")
  SET(JEMALLOC_CFLAGS "-fPIC -mtune=generic -O3 -funroll-loops -g -fno-reorder-blocks-and-partition")
  IF(${CMAKE_C_COMPILER_VERSION} VERSION_GREATER 10.0.0.0)
    SET(JEMALLOC_CFLAGS "${JEMALLOC_CFLAGS} -Wno-missing-attributes -Wno-switch-outside-range")
  ENDIF()
  SET(JEMALLOC_CONFIGURE_OPTS
    CC=${CMAKE_C_COMPILER}
    CFLAGS=${JEMALLOC_CFLAGS}
    --with-malloc-conf=background_thread:true
    --with-private-namespace=jemalloc_internal_
  )

  IF(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    LIST(APPEND JEMALLOC_CONFIGURE_OPTS --with-lg-page=16)
  ENDIF()

  IF (CMAKE_BUILD_TYPE MATCHES "Debug" AND NOT APPLE) # see the comment in CMakeLists.txt
    LIST(APPEND JEMALLOC_CONFIGURE_OPTS --enable-debug)
  ENDIF()

  IF (WITH_JEMALLOC_JEPROF)
    LIST(APPEND JEMALLOC_CONFIGURE_OPTS --enable-prof)
  ENDIF()

  IF(CMAKE_GENERATOR MATCHES "Makefiles")
    SET(MAKE_COMMAND ${CMAKE_MAKE_PROGRAM})
  ELSE() # Xcode/Ninja generators
    SET(MAKE_COMMAND make)
  ENDIF()

  IF(NOT EXISTS ${SOURCE_DIR}/configure)
    EXECUTE_PROCESS(
      COMMAND ./autogen.sh
      WORKING_DIRECTORY "${SOURCE_DIR}"
    )
  ENDIF()

  ExternalProject_Add(jemalloc
    PREFIX extra/jemalloc
    SOURCE_DIR ${SOURCE_DIR}
    BINARY_DIR ${BINARY_DIR}
    STAMP_DIR  ${BINARY_DIR}
    CONFIGURE_COMMAND "${SOURCE_DIR}/configure" ${JEMALLOC_CONFIGURE_OPTS}
    BUILD_COMMAND  ${MAKE_COMMAND} "build_lib_static"
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${BINARY_DIR}/lib/libjemalloc_pic.a
  )
  ADD_LIBRARY(libjemalloc STATIC IMPORTED)
  SET_TARGET_PROPERTIES(libjemalloc PROPERTIES IMPORTED_LOCATION "${BINARY_DIR}/lib/libjemalloc_pic.a")
  ADD_DEPENDENCIES(libjemalloc jemalloc)
  STRING(APPEND CMAKE_C_FLAGS " -fno-builtin-malloc -fno-builtin-calloc")
  STRING(APPEND CMAKE_C_FLAGS " -fno-builtin-realloc -fno-builtin-free")
  STRING(APPEND CMAKE_CXX_FLAGS " -fno-builtin-malloc -fno-builtin-calloc")
  STRING(APPEND CMAKE_CXX_FLAGS " -fno-builtin-realloc -fno-builtin-free")
ENDMACRO()

IF(CMAKE_SYSTEM_NAME MATCHES "Linux" OR APPLE)
 # Linux and OSX are the only systems where bundled jemalloc can be built without problems,
 # as they both have GNU make and jemalloc actually compiles.
 # Also, BSDs use jemalloc as malloc already
 SET(WITH_JEMALLOC_DEFAULT "bundled")
ELSE()
 SET(WITH_JEMALLOC_DEFAULT "no")
ENDIF()

SET(WITH_JEMALLOC ${WITH_JEMALLOC_DEFAULT} CACHE STRING
    "Which jemalloc to use. Possible values are 'no', 'bundled', 'system', 'yes' (system if possible, otherwise bundled)")

IF(WITH_ASAN AND WITH_JEMALLOC)
  MESSAGE(FATAL_ERROR "Address sanitizer conflicts with jemalloc")
ENDIF()

MACRO (CHECK_JEMALLOC)
  IF(WITH_JEMALLOC STREQUAL "system" OR WITH_JEMALLOC STREQUAL "yes")
    CHECK_LIBRARY_EXISTS(jemalloc malloc_stats_print "" HAVE_JEMALLOC)
    IF (HAVE_JEMALLOC)
      SET(LIBJEMALLOC jemalloc)
      SET(MALLOC_LIBRARY "system jemalloc")
    ELSEIF (WITH_JEMALLOC STREQUAL "system")
      MESSAGE(FATAL_ERROR "system jemalloc is not found")
    ELSEIF (WITH_JEMALLOC STREQUAL "yes")
      SET(trybundled 1)
    ENDIF()
  ENDIF()
  IF(WITH_JEMALLOC STREQUAL "bundled" OR trybundled)
    USE_BUNDLED_JEMALLOC()
    SET(MALLOC_LIBRARY "bundled jemalloc")
  ELSE()
    # See if WITH_JEMALLOC is of the form </path/to/custom/installation>
    FILE(GLOB WITH_JEMALLOC_HEADER ${WITH_JEMALLOC}/include/jemalloc/jemalloc.h)
    IF (WITH_JEMALLOC_HEADER)
      SET(WITH_JEMALLOC_PATH ${WITH_JEMALLOC} CACHE PATH "path to custom jemalloc installation")
      SET(WITH_JEMALLOC_PATH ${WITH_JEMALLOC})
      SET(LIBJEMALLOC "libjemalloc")
      ADD_LIBRARY(libjemalloc STATIC IMPORTED)
      SET_TARGET_PROPERTIES(libjemalloc PROPERTIES IMPORTED_LOCATION "${WITH_JEMALLOC}/lib/libjemalloc_pic.a")
      SET(MALLOC_LIBRARY "system jemalloc (${WITH_JEMALLOC})")
    ENDIF()
  ENDIF()
ENDMACRO()
