build_type=$1
source ./common_conf.sh
#source ${dstore_root_path}/dstore/buildenv
export LOCAL_LIB_PATH=${dstore_root_path}/local_libs
cd ${dstore_root_path}/dstore/utils
if [[ "${build_type}" != "debug" ]]; then
  build_type="release"
fi
bash build.sh -m ${build_type}