/*
  Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef __CDE_ERRORCODE_H__
#define __CDE_ERRORCODE_H__

#include "cde_def.h"
#include "cde_error.h"
#include "errorcode/dstore_common_error_code.h"
#include "framework/dstore_thread_interface.h"
namespace CDE {

extern bool dstore_rollback_on_timeout;

/** type for ha error code definded in my_base.h */
using HaErrorCode = uint32_t;

/**
 Get the dstore error code.

 This function returns an error code of type ErrorCode which represents
 the error status of a dstore operation.
 If the thread context is not available or dstore error is STORAGE_OK, it
 returns CDE_ERROR.

 @return ErrorCode The error code of dstore.
*/
inline DSTORE::ErrorCode GetDstoreErrcode() {
  DSTORE::ThreadContextInterface *thrd =
      DSTORE::ThreadContextInterface::GetCurrentThreadContext();
  DBUG_EXECUTE_IF("errcode_getctx_null_injection", thrd = nullptr;);
  if (!thrd) {
    CDE_LOG_WARN("get thread context failed");
    return CDE_ERROR;
  }

  auto errCode = thrd->GetErrorCode();
  if (errCode == STORAGE_OK) {
    return CDE_ERROR;
  }

  return errCode;
}

/**
 Retrieves the dstore error message.

 @return The error message string of lastest dstore error.
*/
const char *GetDstoreErrmsg();

/**
 Converts a dstore error code to the corresponding MySQL handler error
 code.

 Only used in ut

 @param[in]  error  The dstore error code to convert.

 @return The corresponding MySQL handler error code.
*/
HaErrorCode ConvertDstoreErrcodeToMysql(DSTORE::ErrorCode error);

/**
 Get the current dstore error code and Converts to a MySQL handler error code.

 This function retrieves the current dstore error code and maps it to the
 corresponding MySQL handler error code.

 @return The corresponding MySQL handler error code.
*/
HaErrorCode GetAndConvertDstoreErrcodeToMysql();

/**
 Converts a given error code into the corresponding MySQL handler error code.
 This function should only be used at the end of handler virtual interfaces.

 @param[in]  error  The generic error code to convert, including handler and
 dstore error code.
 @param[in]  flags  Table flags or 0, will be used to format error info for
 ER_TOO_BIG_ROWSIZE/ER_INDEX_COLUMN_TOO_LONG.
 @param[in]  thd    MySQL thread context or NULL, will tell to MySQL about a
 possible transaction rollback caused by a lock wait timeout or a deadlock, or
 kill related session while be interrupted.

 @return The corresponding MySQL handler error code.
*/
HaErrorCode ConvertErrcodeToMysql(int64_t error, uint32_t flags, THD *thd);
} /* namespace CDE */
#endif  // __CDE_ERRORCODE_H__
