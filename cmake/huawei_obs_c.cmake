# old cmake does not have ExternalProject file
IF(CMAKE_VERSION VERSION_LESS "2.6.0")
  MACRO (CHECK_LIB_OBS_C)
  ENDMACRO()
  RETURN()
ENDIF()

# Define the compiled library path
if(NOT DEFINED OBS_PATH OR "${OBS_PATH}" STREQUAL "")
    set(OBS_PATH "${CMAKE_SOURCE_DIR}/../dstore/huaweicloud-sdk-c-obs")
    message(WARNING "[OBS] OBS library path is null, set default path: ${OBS_PATH}")
else()
    message(STATUS "[OBS] OBS library path is already set to: ${OBS_PATH}")
endif()

SET(PREBUILT_OBS_PATH "${OBS_PATH}/output" CACHE PATH "Path to prebuilt OBS library")
message(STATUS "[OBS] OBS output path is: ${PREBUILT_OBS_PATH}")

MACRO (USE_BUNDLED_LIB_OBS_C)
  SET(LIBOBS "libobs")

  # Set header file path
  SET(OBS_C_INCLUDE_DIR "${PREBUILT_OBS_PATH}/include")
  
  # Select the library file path
  SET(OBS_LIB_NAME "libeSDKOBS.a")
  MESSAGE(STATUS "[OBS] Release build selected")
  
  SET(OBS_C_LIBRARY_PATH "${PREBUILT_OBS_PATH}/lib/${OBS_LIB_NAME}")

  MESSAGE(STATUS "[OBS] Using prebuilt library")
  MESSAGE(STATUS "[OBS] Include dir: ${OBS_C_INCLUDE_DIR}")
  MESSAGE(STATUS "[OBS] Library path: ${OBS_C_LIBRARY_PATH}")
  MESSAGE(STATUS "[OBS] Build type: ${CMAKE_BUILD_TYPE}")

  # Check if the header file exists
  IF(NOT EXISTS "${OBS_C_INCLUDE_DIR}/eSDKOBS.h")
    MESSAGE(FATAL_ERROR "[OBS] Header file not found at ${OBS_C_INCLUDE_DIR}/eSDKOBS.h")
    MESSAGE(STATUS "[OBS] Please check if the header file exists")
  ENDIF()

  # Check if the library file exists
  IF(NOT EXISTS "${OBS_C_LIBRARY_PATH}")
    MESSAGE(FATAL_ERROR "[OBS] Library file not found at ${OBS_C_LIBRARY_PATH}")
    MESSAGE(STATUS "[OBS] Available library directories:")
    EXECUTE_PROCESS(COMMAND ls -la "${PREBUILT_OBS_PATH}/lib/"
                    ERROR_QUIET OUTPUT_VARIABLE LIB_DIRS)
    MESSAGE(STATUS "[OBS] ${LIB_DIRS}")
    
    # Show available library files
    IF(EXISTS "${PREBUILT_OBS_PATH}/lib/")
      EXECUTE_PROCESS(COMMAND ls -la "${PREBUILT_OBS_PATH}/lib/"
                      ERROR_QUIET OUTPUT_VARIABLE DEBUG_LIBS)
      MESSAGE(STATUS "[OBS] Debug library files: ${DEBUG_LIBS}")
    ENDIF()
    
    IF(EXISTS "${PREBUILT_OBS_PATH}/lib/")
      EXECUTE_PROCESS(COMMAND ls -la "${PREBUILT_OBS_PATH}/lib/"
                      ERROR_QUIET OUTPUT_VARIABLE RELEASE_LIBS)
      MESSAGE(STATUS "[OBS] Release library files: ${RELEASE_LIBS}")
    ENDIF()
  ENDIF()

  # Import compiled libraries
  ADD_LIBRARY(libobs STATIC IMPORTED GLOBAL)
  SET_TARGET_PROPERTIES(libobs PROPERTIES 
    IMPORTED_LOCATION "${OBS_C_LIBRARY_PATH}"
  )

  MESSAGE(STATUS "[OBS] Prebuilt OBS configuration completed!")
ENDMACRO()

MACRO (CHECK_LIB_OBS_C)
  USE_BUNDLED_LIB_OBS_C()
  SET(OBS_C_LIBRARY "prebuilt Huawei OBS C (${CMAKE_BUILD_TYPE})")
ENDMACRO()