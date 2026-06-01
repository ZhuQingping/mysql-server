# build dstore tool: damagepage

set(damagepage_src_list ${DSTORE}/dstore/tools/damagepage/src/tool_damage_page.cpp)
list(APPEND damagepage_src_list ${dstore_src_list})
set(damagepage_compile_options ${PROTECT_OPTIONS} ${WARNING_OPTIONS} ${OPTIMIZE_OPTIONS} ${CHECK_OPTIONS} ${BIN_SECURE_OPTIONS})
set(damagepage_link_options ${BIN_LINK_OPTIONS})
set(damagepage_link_libs ${LIBSECUREC} gsutils ssl crypto ext::lz4)
set(damagepage_macro_options ${MACRO_OPTIONS})
set(damagepage_include_directories
    ${DSTORE}/dstore/interface
    ${DSTORE}/dstore/include
    ${SECURE_INCLUDE_PATH}
    ${DSTORE}/dstore/utils/output/include
)
set(damagepage_link_directories
    ${SECURE_LIB_PATH} ${DSTORE}/dstore/utils/output/lib
    ${DSTORE_3RD_PATH}/openssl/lib
    ${DSTORE_3RD_PATH}/cjson/lib
)

MYSQL_ADD_EXECUTABLE(damagepage
    ${damagepage_src_list}
    INCLUDE_DIRECTORIES ${damagepage_include_directories}
    LINK_LIBRARIES ${damagepage_link_libs}
)
TARGET_LINK_DIRECTORIES(damagepage PRIVATE ${damagepage_link_directories})
