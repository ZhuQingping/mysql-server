#!/bin/bash
set -e

ARCH=$(uname -m)
build_type=${1:-"release"}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mysql_root_path=$(dirname $(dirname $(dirname `readlink -f "$0"`)))
dstore_root_path=${mysql_root_path}/../dstore
obs_root_path="${dstore_root_path}/huaweicloud-sdk-c-obs"

# Check obs code exists.
if [[ ! -d "${obs_root_path}" ]]; then
  echo "OBS source code not found at: ${obs_root_path}"
  echo "Please make sure huaweicloud-sdk-c-obs repository exists in parent directory."
  exit 1
fi

build_type_lower=$(echo "${build_type}" | tr '[:upper:]' '[:lower:]')
if [[ "${build_type_lower}" == "debug" ]]; then
  CMAKE_BUILD_TYPE="Debug"
  LIB_SUFFIX="_d"
else
  CMAKE_BUILD_TYPE="Release" 
  LIB_SUFFIX=""
fi

echo "========================================"
echo "OBS Build Configuration"
echo "========================================"
echo "Build type: ${CMAKE_BUILD_TYPE}"
echo "OBS source: ${obs_root_path}"
echo "MySQL root: ${mysql_root_path}"
echo "========================================"

OBS_OUTPUT_DIR="${obs_root_path}/output"
echo "[OBS] Checking output directory: ${OBS_OUTPUT_DIR}"

# Create the directory only if it does not exist
if [[ ! -d "${OBS_OUTPUT_DIR}/include" ]]; then
  echo "[OBS] Creating include directory: ${OBS_OUTPUT_DIR}/include"
  mkdir -p "${OBS_OUTPUT_DIR}/include"
else
  echo "[OBS] Include directory already exists: ${OBS_OUTPUT_DIR}/include"
fi

if [[ ! -d "${OBS_OUTPUT_DIR}/lib" ]]; then
  echo "[OBS] Creating lib directory: ${OBS_OUTPUT_DIR}/lib"
  mkdir -p "${OBS_OUTPUT_DIR}/lib"
else
  echo "[OBS] Lib directory already exists: ${OBS_OUTPUT_DIR}/lib"
fi

# Verify that the directory was created successfully
if [[ ! -d "${OBS_OUTPUT_DIR}/include" || ! -d "${OBS_OUTPUT_DIR}/lib" ]]; then
  echo "[OBS] Failed to create output directories!!!"
  exit 1
fi

# build obs library
build_obs_library() {
  local obs_path="$1"
  local build_type="$2"
  
  echo "[OBS] Building OBS library in: ${obs_path}/build_for_mysql"
  
  (
    export openssl_version=openssl-1.1.1w
    export curl_version=curl-8.11.1
    export pcre_version=pcre-8.45
    export iconv_version=iconv-1.15
    export libxml2_version=libxml2-2.9.9
    export SPDLOG_VERSION=spdlog-1.12.0

    if [[ "$ARCH" == "aarch64" ]] || [[ "$ARCH" == "arm64" ]]; then
      export openssl_version=openssl-1.1.1k
      export curl_version=curl-7.78.0
    else
      export cjson_version=cjson-1.7.18
    fi

    cd "${obs_path}"

    # Check if CMakeLists.txt exists
    if [[ ! -f "CMakeLists.txt" ]]; then
      echo "[OBS] CMakeLists.txt not found in ${obs_path}"
      exit 1
    fi

    # Modify to static library (check whether modification is required first)
    if grep -q "add_library(eSDKOBS SHARED" CMakeLists.txt; then
      echo "[OBS] Converting to static library..."
      sed -i 's/add_library(eSDKOBS SHARED/add_library(eSDKOBS STATIC/g' CMakeLists.txt
      
      # Verification modification successful
      if ! grep -q "add_library(eSDKOBS STATIC" CMakeLists.txt; then
        echo "[OBS] Failed to convert library to static"
        exit 1
      fi
    else
      echo "[OBS] Library is already static"
    fi

    # Clean and create the build directory
    echo -e "\033[34m[OBS] Preparing build_for_mysql directory...\033[0m"
    rm -rf build_for_mysql
    mkdir -p build_for_mysql && cd build_for_mysql || exit 1

    echo -e "\033[34m[OBS] Configuring CMake...\033[0m"
    cmake .. -DCMAKE_BUILD_TYPE="${build_type}" -DSPDLOG_VERSION="${SPDLOG_VERSION}" -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    
    # Get the number of CPU cores
    local cpu_cores=$(nproc 2>/dev/null || echo 4)
    echo "[OBS] Building obs with ${cpu_cores} cores..."
    make -j${cpu_cores}

    # Verify build results
    if [[ ! -f "lib/libeSDKOBS.a" ]]; then
      echo "[OBS] OBS library build failed - libeSDKOBS.a not found"
      exit 1
    fi
    
    echo "[OBS] Build completed successfully."
  )
}

# copy obs files
copy_obs_files() {
  local obs_path="$1"
  local output_dir="$2"
  local lib_suffix="$3"

  echo "[OBS] Copying OBS files..."

  local lib_source="${obs_path}/build_for_mysql/lib/libeSDKOBS.a"
  local header_source="${obs_path}/source/eSDK_OBS_API/eSDK_OBS_API_C++/inc/eSDKOBS.h"

  # Check if the source file exists
  if [[ ! -f "${lib_source}" ]]; then
    echo "[OBS] Library file not found: ${lib_source}"
    return 1
  fi

  if [[ ! -f "${header_source}" ]]; then
    echo "[OBS] Header file not found: ${header_source}"
    return 1
  fi

  # Copy the library file
  local lib_dest="${output_dir}/lib/libeSDKOBS${lib_suffix}.a"

  # Get file size
  source_size=$(stat -c%s "${lib_source}" 2>/dev/null || echo 0)
  dest_size=$(stat -c%s "${lib_dest}" 2>/dev/null || echo 0)

  echo "[OBS] Source file size: ${source_size} bytes"
  echo "[OBS] Target file size: ${dest_size} bytes"

  # If the target file exists and is the same size, skip copying.
  if [[ -f "${lib_dest}" ]] && [[ "${source_size}" -eq "${dest_size}" ]] && [[ "${source_size}" -gt 0 ]]; then
    echo -e "\033[36m[OBS] Library file size matches, skipping copy: $(basename "${lib_dest}")\033[0m"
  else
    echo -e "\033[36m[OBS] Copying library: ${lib_source} -> ${lib_dest}\033[0m"
    cp -f "${lib_source}" "${lib_dest}" || { echo "Library copy failed"; return 1; }
    echo "[OBS] Library file copied successfully"
  fi

  # Copy header file
  local header_dest="${output_dir}/include/eSDKOBS.h"

  if [[ -f "${header_dest}" ]] && cmp -s "${header_source}" "${header_dest}"; then
    echo "[OBS] Header file unchanged: eSDKOBS.h"
  else
    echo "[OBS] Copying header: ${header_source} -> ${header_dest}"
    cp -f "${header_source}" "${header_dest}" || { echo "Header copy failed"; return 1; }
    echo "[OBS] Header file copied successfully"
  fi
  
  # Verify copy results
  if [[ -f "${lib_dest}" && -f "${output_dir}/include/eSDKOBS.h" ]]; then
    echo "[OBS] Files copied successfully:"
    echo "  Library: $(basename "${lib_dest}")"
    echo "  Header: eSDKOBS.h"
    return 0
  else
    echo "[OBS] File copy failed"
    return 1
  fi
}

# Displays a list of files with full time
show_files_with_time() {
  local dir="$1"
  local description="$2"
  
  echo "${description}:"
  if [ -d "$dir" ] && [ "$(ls -A "$dir")" ]; then
    for file in "$dir"/*; do
      if [ -f "$file" ]; then
        filename=$(basename "$file")
        mod_time=$(stat -c "%y" "$file" | cut -d'.' -f1)
        echo -e "\033[36m  ${filename} (path: $file, modification time: ${mod_time})\033[0m"
      fi
    done
  else
    echo "No file!!!"
  fi
}

main() {
  # delete two lines containing "set_sigaction_for_sigpipe" from the general.c file.
  # Otherwise OBS SDK will change processing of SIGPIPE and make mysqld exit unexpectedly.
  # After OBS SDK deinitialized, it will unregister handler for SIGPIPE and make process exit
  # when receives SIGPIPE.
  # Since handler for SIGPIPE only print log messages, we remove register and unregister of it.
  local general_c_file="${obs_root_path}/source/eSDK_OBS_API/eSDK_OBS_API_C++/src/general.c"
  if [[ -f "${general_c_file}" ]]; then
    echo "[OBS] Removing lines containing 'set_sigaction_for_sigpipe' from ${general_c_file}"
    # Will remove call of set_sigaction_for_sigpipe and unset_sigaction_for_sigpipe both.
    sed -i '/set_sigaction_for_sigpipe/d' "${general_c_file}"
  else
    echo "[OBS] File not found: ${general_c_file}"
    exit 1
  fi

  # build obs library
  if ! build_obs_library "${obs_root_path}" "${CMAKE_BUILD_TYPE}"; then
    echo "[OBS] OBS build failed"
    exit 1
  fi
  
  # copy obs files
  if ! copy_obs_files "${obs_root_path}" "${OBS_OUTPUT_DIR}" "${LIB_SUFFIX}"; then
    echo "[OBS] File copy failed"
    exit 1
  fi
  
  echo "========================================"
  echo " OBS Build Completed Successfully!"
  echo "========================================"
  echo "Output directory: ${OBS_OUTPUT_DIR}"
  # Print the modification time of the output files.
  show_files_with_time "${OBS_OUTPUT_DIR}/lib" "Library files"
  show_files_with_time "${OBS_OUTPUT_DIR}/include" "Header files"
  echo "========================================"
}

main