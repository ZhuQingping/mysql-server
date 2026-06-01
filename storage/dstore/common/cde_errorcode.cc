/*
  Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

  Dstore handler error code interfaces.
*/

#include "cde_errorcode.h"

#include <unordered_map>

#include "my_base.h"
#include "mysql/plugin.h"
#include "mysqld_error.h"

#include "errorcode/dstore_backup_restore_error_code.h"
#include "errorcode/dstore_buf_error_code.h"
#include "errorcode/dstore_catalog_error_code.h"
#include "errorcode/dstore_common_error_code.h"
#include "errorcode/dstore_control_error_code.h"
#include "errorcode/dstore_error_struct.h"
#include "errorcode/dstore_framework_error_code.h"
#include "errorcode/dstore_heap_error_code.h"
#include "errorcode/dstore_index_error_code.h"
#include "errorcode/dstore_lock_error_code.h"
#include "errorcode/dstore_logical_replication_error_code.h"
#include "errorcode/dstore_page_error_code.h"
#include "errorcode/dstore_port_error_code.h"
#include "errorcode/dstore_recovery_error_code.h"
#include "errorcode/dstore_rpc_error_code.h"
#include "errorcode/dstore_systable_error_code.h"
#include "errorcode/dstore_tablespace_error_code.h"
#include "errorcode/dstore_transaction_error_code.h"
#include "errorcode/dstore_tuple_error_code.h"
#include "errorcode/dstore_undo_error_code.h"
#include "errorcode/dstore_wal_error_code.h"

#include "cde_def.h"
#include "dml/cde_dml_ctx.h"

namespace CDE {

bool dstore_rollback_on_timeout = false;

static const char *UNKNOWN_ERROR_MSG = "Unknown Error";

const char *GetDstoreErrmsg() {
  DSTORE::ThreadContextInterface *thrd =
      DSTORE::ThreadContextInterface::GetCurrentThreadContext();
  DBUG_EXECUTE_IF("errcode_getctx_null_injection", thrd = nullptr;);
  if (!thrd) {
    CDE_LOG_WARN("get thread context failed");
    return UNKNOWN_ERROR_MSG;
  }

  const char *msg = thrd->GetErrorMessage();
  if (msg == nullptr || *msg == 0) {
    return UNKNOWN_ERROR_MSG;
  }

  return msg;
}

static std::unordered_map<DSTORE::ErrorCode, HaErrorCode>
    g_dstoreToMysqlErrcodeMap = {
        {DSTORE::ATTRIBUTE_ERROR_UNSUPPORTED_BYVAL_LENGTH, HA_ERR_UNSUPPORTED},
        {DSTORE::BACKUPRESTORE_ERROR_OUT_OF_MEMORY, HA_ERR_OUT_OF_MEM},
        {DSTORE::COMMON_INFO_CONTACT_ENGINEER, HA_ERR_INTERNAL_ERROR},
        {DSTORE::CONTROL_ERROR_MEMORY_NOT_ENOUGH, HA_ERR_OUT_OF_MEM},
        {DSTORE::DECODE_ERROR_UNSUPPORTED_ATTR_TYPE, HA_ERR_UNSUPPORTED},
        {DSTORE::HASH_ERROR_MEMORY_SIZE_OVERFLOW, HA_ERR_INTERNAL_ERROR},
        {DSTORE::HASH_ERROR_OUT_OF_MEMORY, HA_ERR_OUT_OF_MEM},
        {DSTORE::HASH_ERROR_OUT_OF_SHARED_MEMORY, HA_ERR_OUT_OF_MEM},
        {DSTORE::HEAP_ERROR_TUPLE_IS_CHANGED, HA_ERR_RECORD_CHANGED},
        {DSTORE::HEAP_ERROR_TUPLE_IS_DELETED, HA_ERR_RECORD_DELETED},
        {DSTORE::INDEX_ERROR_FAIL_FOR_HUGE_INDEX_TUPLE,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_INDEX_FAIL_FOR_HUGE_INDEX_TUPLE},
        {DSTORE::INDEX_ERROR_INSERT_UNIQUE_CHECK, HA_ERR_FOUND_DUPP_KEY},
        {DSTORE::INDEX_ERROR_MEMORY_ALLOC, HA_ERR_OUT_OF_MEM},
        {DSTORE::LOCK_ERROR_DEADLOCK, HA_ERR_LOCK_DEADLOCK},
        {DSTORE::LOCK_ERROR_NOT_SUPPORTED, HA_ERR_UNSUPPORTED},
        {DSTORE::LOCK_ERROR_OUT_OF_MEMORY, HA_ERR_OUT_OF_MEM},
        {DSTORE::RPC_ERROR_COMMON_OUT_OF_MEMORY, HA_ERR_OUT_OF_MEM},
        {DSTORE::RPC_ERROR_INTERNAL, HA_ERR_INTERNAL_ERROR},
        {DSTORE::RPC_ERROR_OUT_OF_MEMORY, HA_ERR_OUT_OF_MEM},
        {DSTORE::TRANSACTION_ERROR_INVALID_SAVEPOINT, HA_ERR_NO_SAVEPOINT},
        {DSTORE::TRANSACTION_ERROR_OUT_OF_MEMORY, HA_ERR_OUT_OF_MEM},
        {DSTORE::TRANSACTION_ERROR_SAVEPOINT_NOT_FOUND, HA_ERR_NO_SAVEPOINT},
        {DSTORE::TRANSACTION_INFO_SAME_THREAD_DEADLOCK, HA_ERR_LOCK_DEADLOCK},
        {DSTORE::TUPLESORT_ERROR_MISSING_SUPPORT_FUNCTION_FOR_FUNCOID,
         HA_ERR_UNSUPPORTED},
        {DSTORE::TUPLESORT_ERROR_MISSING_SUPPORT_FUNCTION_FOR_TYPE,
         HA_ERR_UNSUPPORTED},
        {DSTORE::TUPLESORT_ERROR_UNEXPECTED_OUT_OF_MEMORY, HA_ERR_OUT_OF_MEM},
        {DSTORE::TUPLE_ERROR_CORRUPTED_LINE_POINTER, HA_ERR_TABLE_CORRUPT},
        {DSTORE::TUPLE_ERROR_CORRUPTED_PAGE_POINTERS, HA_ERR_TABLE_CORRUPT},
        {DSTORE::TUPLE_ERROR_NATTRS_EXCEEDS_LIMIT, HA_ERR_TOO_MANY_FIELDS},
        {DSTORE::TUPLE_ERROR_TOO_MANY_COLUMNS,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_TUPLE_TOO_MANY_COLUMNS},
        {DSTORE::TUPLE_ERROR_UNDEFINED_COLUMN, HA_ERR_INTERNAL_ERROR},
        {DSTORE::UNDO_ERROR_OUT_OF_MEMORY, HA_ERR_OUT_OF_MEM},
        {DSTORE::WAL_ERROR_INIT_ALLOC_OOM, HA_ERR_OUT_OF_MEM},
        {DSTORE::WAL_ERROR_INTERNAL_ERROR, HA_ERR_INTERNAL_ERROR},
        {DSTORE::WAL_ERROR_PANIC_INTERNAL_ERROR, HA_ERR_INTERNAL_ERROR},
        {DSTORE::LOCK_ERROR_WAIT_TIMEOUT, HA_ERR_LOCK_WAIT_TIMEOUT},
        {DSTORE::LOCK_ERROR_WAIT_CANCELED, HA_ERR_QUERY_INTERRUPTED},
        {DSTORE::SQL_WARNING_REQUEST_ARE_CANCELED, HA_ERR_QUERY_INTERRUPTED},
        {DSTORE::SWAT_ERROR_HIT_DAMAGE_PAGE,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_SWAT_HIT_DAMAGE_PAGE},
        {DSTORE::TUPLE_ERROR_TUPLE_TOO_BIG,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_TUPLE_TOO_BIG},
        {DSTORE::BUFFILE_ERROR_FAIL_CREATE_TEMP_FILE,
         HA_ERR_TEMP_FILE_WRITE_FAILURE},
        {DSTORE::TUPLESORT_ERROR_INSUFFICIENT_MEMORY_ALLOWED,
         HA_ERR_OUT_OF_MEM},
        {DSTORE::TUPLESORT_ERROR_COULD_NOT_CREATE_UNIQUE_INDEX,
         HA_ERR_FOUND_DUPP_KEY},
        {DSTORE::TBS_ERROR_TABLESPACE_USE_UP,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_TABLESPACE_FULL},
        {DSTORE::TUPLESORT_ERROR_INVALID_TUPLESORT_STATE,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_TUPLESORT},
        {DSTORE::TUPLESORT_ERROR_TOO_MANY_RUNS_FOR_EXTERNAL_SORT,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_TUPLESORT},
        {DSTORE::TUPLESORT_ERROR_UNEXPECTED_OUT_OF_MEMORY,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_TUPLESORT},
        {DSTORE::TUPLESORT_ERROR_MISSING_SUPPORT_FUNCTION_FOR_TYPE,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_TUPLESORT},
        {DSTORE::TUPLESORT_ERROR_MISSING_SUPPORT_FUNCTION_FOR_FUNCOID,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_TUPLESORT},
        {DSTORE::TUPLESORT_ERROR_UNEXPECTED_END_OF_TAPE,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_TUPLESORT},
        {DSTORE::INDEX_ERROR_EXPRESSION_VALUE_ERR,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_GET_EXPRESSION_VAL},
        {DSTORE::COMMON_ERROR_CREATE_THREAD_FAIL,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_OUT_OF_RESOURCES},
        {DSTORE::COMMON_ERROR_INIT_THREAD_FAIL,
         (int32_t)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_OUT_OF_RESOURCES},
        /* Add more mappings as needed. */
};

HaErrorCode ConvertDstoreErrcodeToMysql(DSTORE::ErrorCode error) {
  HaErrorCode errcode = HA_ERR_GENERIC;
  /* convert from dstore errorcode to handler error code. */
  auto it = g_dstoreToMysqlErrcodeMap.find(error);
  if (it != g_dstoreToMysqlErrcodeMap.end()) {
    errcode = it->second;
  }

  return errcode;
}

HaErrorCode GetAndConvertDstoreErrcodeToMysql() {
  ErrorCode dstoreErr = GetDstoreErrcode();
  DBUG_EXECUTE_IF("errcode_GetDstoreErrcode_injection",
                  dstoreErr = STORAGE_OK;);
  if (dstoreErr == STORAGE_OK) {
    return dstoreErr;
  }

  return ConvertDstoreErrcodeToMysql(dstoreErr);
}

HaErrorCode ConvertErrcodeToMysql(int64_t error, uint32_t flags, THD *thd) {
  if (error == CDE_OK) {
    return CDE_OK;
  }

  (void)(flags);
  (void)(thd);

  HaErrorCode errcode = HA_ERR_GENERIC;

  // Check if the error is within the handler error code range(include both
  // default and self-defined)
  if ((error >= HA_ERR_FIRST && error <= HA_ERR_LAST) ||
      (error > (int64_t)HaDstoreErrE::HA_DSTORE_ERR_FIRST &&
       error < (int64_t)HaDstoreErrE::HA_DSTORE_ERR_LAST)) {
    errcode = (HaErrorCode)error;
  } else {
    errcode =
        ConvertDstoreErrcodeToMysql(static_cast<DSTORE::ErrorCode>(error));
  }

  // Handle specific error cases
  switch (errcode) {
    case HA_ERR_FK_DEPTH_EXCEEDED:
      my_error(ER_FK_DEPTH_EXCEEDED, MYF(0), MAX_CASCADE_DEPTH);
      break;
    case HA_ERR_LOCK_DEADLOCK:
      if (thd != nullptr) {
        thd_mark_transaction_to_rollback(thd, 1);
      }
      break;
    case HA_ERR_LOCK_WAIT_TIMEOUT:
      /* We let MySQL just roll back the latest SQL statement or the whole
      transaction when lock wait timeout. */
      if (thd != nullptr) {
        thd_mark_transaction_to_rollback(thd, (int)dstore_rollback_on_timeout);
      }
      break;
    case HA_ERR_QUERY_INTERRUPTED:
      break;
    case (HaErrorCode)
        HaDstoreErrE::HA_DSTORE_ERR_TRX_UNABLE_TO_ACCESS_CONTINUOUSLY:
      if (thd != nullptr) {
        thd_mark_transaction_to_rollback(thd, 0);
      }
      break;
    case (HaErrorCode)HaDstoreErrE::HA_DSTORE_ERR_SKIP_WAIT:
      my_error(ER_LOCK_NOWAIT, MYF(0));
      break;
    default:
      // Handle other potential cases if needed
      break;
  }

  return errcode;
}
} /* namespace CDE */
