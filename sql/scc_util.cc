/*****************************************************************************

Copyright (c) 2020, 2025, Huawei and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<sc_cryptoapi.h>) && __has_include(<sc_errcode.h>)
#include <sc_cryptoapi.h>
#include <sc_errcode.h>
#else
#define SEC_SUCCESS 0
#define SEC_FAILURE 1
#define SEC_ERR_NO_INIT 2
#endif
#else
#include <sc_cryptoapi.h>
#include <sc_errcode.h>
#endif

#include <string>
#include "my_securec.h"
#include "sql/log.h"
#include "sql/mysqld.h"

/* Function pointer Types for:
int SCC_Initialize(char *sccCfgFile);
int SCC_Finalize();
int SCC_Decrypt(const char *cipherText, int cipherTextLen, char **plainText,
                int *plainTextLen); */
typedef int (*SCC_Initialize_Type)(char *);
typedef int (*SCC_Finalize_Type)();
typedef int (*SCC_Decrypt_Type)(const char *, int, char **, int *);
SCC_Initialize_Type scc_initialize_func = nullptr;
SCC_Finalize_Type scc_finalize_func = nullptr;
SCC_Decrypt_Type scc_decrypt_func = nullptr;

bool scc_lib_loaded = false;
void *scc_lib_handle = nullptr;

bool scc_load_lib() {
  if (scc_lib_loaded) {
    return false;
  }

  std::string scc_so("/usr/local/seccomponent/lib/libsc-secrypto.so");
  DBUG_EXECUTE_IF("scc_load_lib_not_exist", {
    scc_so = "/usr/local/seccomponent/lib/libsc-secrypto.so.not_exist";
  });

  scc_lib_handle = dlopen(scc_so.c_str(), RTLD_NOW);
  if (scc_lib_handle == nullptr) {
#ifndef IS_DSTORE_BACKUP_TOOL
    sql_print_error("SCC lib dlopen %s error: %s", scc_so.c_str(), dlerror());
#endif
    return true;
  }

  if ((scc_initialize_func = (SCC_Initialize_Type)dlsym(
           scc_lib_handle, "SCC_Initialize")) == nullptr ||
      (scc_finalize_func = (SCC_Finalize_Type)dlsym(
           scc_lib_handle, "SCC_Finalize")) == nullptr ||
      (scc_decrypt_func =
           (SCC_Decrypt_Type)dlsym(scc_lib_handle, "SCC_Decrypt")) == nullptr) {
    dlclose(scc_lib_handle);
    scc_lib_handle = nullptr;
    scc_initialize_func = nullptr;
    scc_finalize_func = nullptr;
    scc_decrypt_func = nullptr;
#ifndef IS_DSTORE_BACKUP_TOOL
    sql_print_error("SCC lib dlsym error: %s", dlerror());
#endif
    return true;
  }

  scc_lib_loaded = true;
  return false;
}

void scc_close_lib() {
  if (scc_lib_loaded) {
    dlclose(scc_lib_handle);
    scc_lib_handle = nullptr;
    scc_initialize_func = nullptr;
    scc_finalize_func = nullptr;
    scc_decrypt_func = nullptr;
  }
}

int scc_init_with_conf(char *conf_file) {
  DBUG_EXECUTE_IF("RDS_SCC_DUMMY_INIT", { return SEC_SUCCESS; });

  int file_length = strlen(conf_file);
  DBUG_EXECUTE_IF("RDS_SCC_FAIL_INIT_FILELEN", { file_length = FN_REFLEN; });
  if (file_length >= FN_REFLEN) {
#ifndef IS_DSTORE_BACKUP_TOOL
    sql_print_error("scc init failed with too large file len=%d", file_length);
#endif
    return SEC_FAILURE;
  }
  return scc_initialize_func(conf_file);
}

int scc_finalize() { return scc_finalize_func(); }

int scc_decrypt(const char *cipher, int cipher_len, char **plain_text,
                int *plain_text_len) {
  DBUG_EXECUTE_IF("RDS_SCC_DUMMY_DECRYPT", {
    // 64 is HEX_PLAINTEXT_LEN, serves TDE feature.
    char dummy_plaintext_hex[64 + 1] =
        "1234567890abcdef1234567890abcdef1234567890abcdef1234567890ABCDEF";
    dummy_plaintext_hex[64] = '\0';
    char *dummy_ret = static_cast<char *>(malloc(64 + 1));
    memcpy_s(dummy_ret, 64 + 1, dummy_plaintext_hex, 64 + 1);
    *plain_text = dummy_ret;
    *plain_text_len = 64;
    return SEC_SUCCESS;
  });
  DBUG_EXECUTE_IF("RDS_SCC_DECRYPT_FAIL1", { rds_scc_initialized = 0; });
  DBUG_EXECUTE_IF("RDS_SCC_DECRYPT_FAIL2", { cipher_len = 0; });
  DBUG_EXECUTE_IF("RDS_SCC_DECRYPT_FAIL3", {
    // sttrlen = 65 > 64, hexplaintext
    char bad_plaintext[64 + 1 + 1] =
        "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef1";
    bad_plaintext[64 + 1] = '\0';
    char *bad_ret = static_cast<char *>(malloc(64 + 1 + 1));
    memcpy_s(bad_ret, 64 + 1, bad_plaintext, 64 + 1 + 1);
    *plain_text = bad_ret;
    *plain_text_len = 64 + 1;
    return SEC_SUCCESS;
  });

  if (!rds_scc_initialized) {
#ifndef IS_DSTORE_BACKUP_TOOL
    sql_print_error(
        "Secrypto component does not load. "
        "Please set rds_scc_conf with the scc.conf.");
#endif
    return SEC_ERR_NO_INIT;
  }
  if (cipher == NULL || cipher_len == 0) {
#ifndef IS_DSTORE_BACKUP_TOOL
    sql_print_error("%s scc_decrypt invalid input,%s", debug_info_prefix,
                    (cipher ? "cipher is nullptr" : "cipherlen is zero"));
#endif
    return SEC_FAILURE;
  }

  return scc_decrypt_func(cipher, cipher_len, plain_text, plain_text_len);
}

int scc_init_with_conf_retry(char *conf_file, int retry_cnt) {
  if (scc_load_lib()) {
#ifndef IS_DSTORE_BACKUP_TOOL
    sql_print_error("Load Secrypto component failed");
#endif
    return SEC_FAILURE;
  }

  int ret = SEC_FAILURE;
  int cur_retry = 0;
  do {
    ret = scc_init_with_conf(conf_file);
    cur_retry++;
    if (ret != SEC_SUCCESS) {
#ifndef IS_DSTORE_BACKUP_TOOL
      sql_print_error("call Secrypto component decrypt failed,ret=%d,retry:%d",
                      ret, cur_retry);
#endif
      // last fail no sleep
      sleep(cur_retry >= retry_cnt ? 0 : 1);
    }
  } while (ret != SEC_SUCCESS && cur_retry < retry_cnt);
  return ret;
}

int scc_decrypt_retry(const char *cipher, int cipher_len, char **plain_text,
                      int *plain_text_len, int retry_cnt) {
  int ret = SEC_FAILURE;
  int cur_retry = 0;
  do {
    ret = scc_decrypt(cipher, cipher_len, plain_text, plain_text_len);
    cur_retry++;
    if (ret != SEC_SUCCESS) {
#ifndef IS_DSTORE_BACKUP_TOOL
      sql_print_error("call Secrypto component decrypt fail,ret=%d,retry:%d",
                      ret, cur_retry);
#endif
      sleep(cur_retry >= retry_cnt ? 0 : 1);
    }
  } while (ret != SEC_SUCCESS && cur_retry < retry_cnt);
  return ret;
}
