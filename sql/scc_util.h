/*****************************************************************************

Copyright (c) 2025, Huawei and/or its affiliates. All Rights Reserved.

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

#ifndef MYSQL_SCC_UTIL
#define MYSQL_SCC_UTIL

#define SCC_API_NO_RETRY 1
#ifdef UNIV_DEBUG
// debug no sleep, no retry
#define SCC_INIT_MAX_RETRY SCC_API_NO_RETRY
#define SCC_DECRYPT_MAX_RETRY SCC_API_NO_RETRY
#else
#define SCC_INIT_MAX_RETRY 3
#define SCC_DECRYPT_MAX_RETRY 3
#endif /* UNIV_DEBUG */

/*
  Initialize seccrypto for encrypt/decrypt secret data.

  Return 0 on success.
*/
int scc_init_with_conf(char *conf_file);

/*
  Finalize scc.
  Called by plugin uninstall.

  Return 0 on success.
*/
int scc_finalize();

/*
  Use seccrypto to decrypt.
  Initialized seccrypto befor calling this api.
  Caller should free memory if succeed.

  Return 0 on success.
*/
int scc_decrypt(const char *cipher, int cipher_len, char **plain_text,
                int *plain_text_len);

/*
  retry wrapper of scc_init_with_conf
  args except for retry_cnt are same
  retry_cnt = 1 means no more retry after first fail
  Return 0(SEC_SUCCESS) on success.
*/
int scc_init_with_conf_retry(char *conf_file, int retry_cnt);

/*
  retry wrapper of scc_decrypt
  args except for retry_cnt are same
  retry_cnt = 1 means no more retry after first fail

  Return 0(SEC_SUCCESS) on success.
*/
int scc_decrypt_retry(const char *cipher, int cipher_len, char **plain_text,
                      int *plain_text_len, int retry_cnt);

/*
  Close SCC library.
*/
void scc_close_lib();

#endif  // MYSQL_SCC_UTIL
