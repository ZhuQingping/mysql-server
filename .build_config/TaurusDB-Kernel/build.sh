#! /bin/bash

# ----------------------------------------------------------------------------------------
# Input Args:
#       UNION_PACKAGE_SWITCH: this build product is kernel pkg or fuxi pkg, default is kernel pkg
#       VERSION: fuxi union pkg version
#       EXIST_PKGx: kernel pkg name which need build in fuxi union pkg
# ----------------------------------------------------------------------------------------

source /etc/profile >/dev/null
export XBUILD_DIR=$(cd `dirname $0`;pwd)

WORK_DIR=$(cd `dirname $0`/../..; pwd)
SERVER_NAME=$(echo ${XBUILD_DIR} | awk -F '/' '{print $NF}')
[[ -z "${SERVER_NAME}" ]] && die "Server name can not be null!"

PKG_DIR="${WORK_DIR}"

export WORKSPACE="${WORK_DIR}"

PLATFORM=`uname -r`
echo "$PLATFORM" |grep -q "aarch"
if [ $? -eq 0 ]; then
    echo "MYSQL_VERSION_PLATFORM=arm" >> ${WORKSPACE}/MYSQL_VERSION
fi

is_union=`echo "${UNION_PACKAGE_SWITCH}" | tr 'A-Z' 'a-z'`
[[ -z "${is_union}" ]] && is_union='false'

# common function
function die()
{
   [[ -n "$*" ]] && echo "[ERROR] $*";
   exit 1
}

# build kernel package
# you can overwrite this function to adaptor your kernel build
function build_kernel_package()
{
    cd ${WORK_DIR}

    # Export TEST is just for the compatibility of ci gate
    export TEST=
    if [[ "${BUILD}" = "debug" ]]; then
        sh +x ${WORK_DIR}/BUILD/build_debug.sh  --workdir=${WORK_DIR}/hwsql-builder --result=${WORK_DIR}/artis --install=${WORK_DIR}/debug/binary --jobs=${PROJECT_THREADS}  --package --enable-install
        #touch v_fake_debug_package.tar.gz
        touch fake_debug_package_test.tar.gz
        sh +x ${XBUILD_DIR}/cicd/script/obs_build.sh
    elif [[ "${BUILD}" = "asan" ]]; then
        sh +x ${WORK_DIR}/BUILD/build_debug.sh  --workdir=${WORK_DIR}/hwsql-builder --result=${WORK_DIR}/artis --install=${WORK_DIR}/debug/binary --jobs=${PROJECT_THREADS}  --package --enable-install --asan
        #touch v_fake_debug_package.tar.gz
        touch fake_debug_package_test.tar.gz
        sh +x ${XBUILD_DIR}/cicd/script/obs_build.sh
    elif [[ "${BUILD}" = "cmc_pack" ]]; then
        mkdir -p ${WORK_DIR}/hwsql-builder/build
        cp ${WORKSPACE}/package/${DSTORE_BRANCH}/mysql-*.tar.gz ${WORK_DIR}/hwsql-builder/build/
        rm -rf ${WORKSPACE}/package
        ls -l ${WORK_DIR}/hwsql-builder/build
        #build management package
        sh +x ${XBUILD_DIR}/cicd/script/obs_build.sh
    else
        sh +x ${WORK_DIR}/BUILD/build_release.sh  --workdir=${WORK_DIR}/hwsql-builder --result=${WORK_DIR}/artis --install=${WORK_DIR}/release/binary --jobs=${PROJECT_THREADS}  --package --exclude-test --enable-install
        #build management package
        sh +x ${XBUILD_DIR}/cicd/script/obs_build.sh
    fi
    rm -rf ${WORK_DIR}/hwsql-builder/build/_CPack_Packages
    echo 'Build kernel package success'
}

# build fuxi auto deploy union package
function build_fuxi_package()
{
    cd ${WORK_DIR}
    rm -f /tmp/autodeploy_*
    cp -af ${WORK_DIR}/../autodeploy/autodeploy_*.tar.gz .
    rm -rf $WORK_DIR/autodeploy
    rm -rf $WORK_DIR/deploy
    tar xf autodeploy_*.tar.gz
    [[ ! -d deploy ]] && echo "Download autodeploy package failed." && exit 1

    local version="${VERSION}.$(date "+%Y%m%d")"
    [[ -z "${version}" ]] && die "Version Can not be Null"

    sh ${WORK_DIR}/deploy/buildMicroPkgFx.sh ${version} ${SERVER_NAME}
    [[ $? -ne 0 ]] && die "Run buildMicroPkgFx.sh failed"

    local APP_VERSION=${APP_VERSION:-$version}
    local CID_BUILD_NUMBER=${CID_BUILD_NUMBER:-000}

    local dir_list=(
      '../taurusdb_arm'
      '../taurusdb_x86'
      )

    for dir_name in ${dir_list[@]};
    do
      if [[ -d "${dir_name}" ]]; then
        ls -l ${dir_name}/v*.tar.gz 
        [[ $? -ne 0 ]] && die "can not find mysql package in ${dir_name}"
        cp ${dir_name}/v*.tar.gz ${PKG_DIR}
      else 
        echo "${dir_name} package is not build"
      fi
    done

    ls -l ${PKG_DIR}
    export CID_WORKSPACE=${WORKSPACE}
    msg=`xbuild package --config ${XBUILD_DIR}/xbuild.yml -w ${WORK_DIR} -o ${WORK_DIR} -v ${APP_VERSION}.${CID_BUILD_NUMBER} 3>&2`

    [[ $? -ne 0 ]] && die "xbuild failed, msg is ${msg}"

    rm -rf ${WORK_DIR}/.build_config/DBS-*
    rm -rf ${WORK_DIR}/.build_config/tmprpm

    echo 'Build fuxi package success!'
}


if [[ "${is_union}" = "true" ]]; then
    build_fuxi_package
else
    build_kernel_package
fi

echo "Run $0 success."
