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

#include "sql/sql_show.h"

#include "boot/cde_instance.h"
#include "common/cde_errorcode.h"
#include "common/cde_session.h"

namespace CDE {

TempTrxGuard::TempTrxGuard(const char *location) : m_location(location) {
  CdeConstructDstoreThrd();
  m_thrd = DSTORE::ThreadContextInterface::GetCurrentThreadContext();
  if (TransactionInterface::TrxIsInProgress()) {
    return;
  }

  if (unlikely(TransactionInterface::StartTrxCommand() !=
               DSTORE::DSTORE_SUCC)) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR("Start temporary transaction fail at %s.",
                                    m_location.c_str());
    m_abnormal = true;
    return;
  }

  if (unlikely(TransactionInterface::SetSnapShot() != DSTORE::DSTORE_SUCC)) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR(
        "Set snapshot for temporary transaction fail at %s.",
        m_location.c_str());
    DSTORE::RetStatus ret = TransactionInterface::AbortTrx(false, true);
    if (unlikely(ret != DSTORE::DSTORE_SUCC)) {
      /* Abort temp trx should never fail unless caused by bugs. Even
      it failed, clean-up and status-reset will be finished anyway. So
      just warning.*/
      CDE_LOG_WARN_WITH_DSTORE_ERROR("Abort trx fail at %s.",
                                     m_location.c_str());
    }
    m_abnormal = true;
    return;
  }
  m_trxCreated = true;
}

TempTrxGuard::~TempTrxGuard() {
  if (!m_trxCreated) {
    return;
  }
  CDE_ASSERT(m_abnormal == false);
  DSTORE::RetStatus ret = TransactionInterface::AbortTrx(false, true);
  if (unlikely(ret != DSTORE::DSTORE_SUCC)) {
    /* Abort temp trx should never fail unless caused by bugs. Even
    it failed, clean-up and status-reset will be finished anyway. So
    just warning.*/
    CDE_LOG_WARN_WITH_DSTORE_ERROR("Abort trx fail at %s.", m_location.c_str());
  }
  m_trxCreated = false;
}

void TempTrxGuard::commit(const char *location) {
  if (!m_trxCreated) {
    return;
  }

  CDE_ASSERT(m_abnormal == false);
  DSTORE::RetStatus ret = TransactionInterface::CommitTrxCommand();
  if (unlikely(ret != DSTORE::DSTORE_SUCC)) {
    /* Commit temp trx should never fail unless caused by bugs.
    Callers can't do any thing if commit failed. So just abort.*/
    CDE_LOG_FATAL_WITH_DSTORE_ERROR("Commit trx fail at %s.", location);
  }
  m_trxCreated = false;
}

void cde_trxinfo_t::append_err_msg(const String &msg) { err_msg.append(msg); }

void cde_trxinfo_t::append_err_msg(const char *msg, size_t len) {
  err_msg.append(msg, len);
}

void cde_trxinfo_t::append_err_msg_with_quote(const String &msg) {
  char q[2] = {'`', '\0'};
  if (my_thd != nullptr) {
    q[0] = (char)get_quote_char_for_identifier(my_thd, msg.ptr(), msg.length());
    if (q[0] == static_cast<char>(EOF)) {
      append_err_msg(msg);
      return;
    }
  }
  append_err_msg(q, 1);
  append_err_msg(msg);
  append_err_msg(q, 1);
}

void cde_trxinfo_t::append_err_msg_with_quote(const char *msg, size_t len) {
  char q[2] = {'`', '\0'};
  if (my_thd != nullptr) {
    q[0] = (char)get_quote_char_for_identifier(my_thd, msg, len);
    if (q[0] == static_cast<char>(EOF)) {
      append_err_msg(msg, len);
      return;
    }
  }
  append_err_msg(q, 1);
  append_err_msg(msg, len);
  append_err_msg(q, 1);
}

void cde_trxinfo_t::appendHeapScanHandler(
    DSTORE::HeapScanHandler **heapScanHandler) {
  heapScanHandlers.insert(heapScanHandler);
}

void cde_trxinfo_t::removeHeapScanHandler(
    DSTORE::HeapScanHandler **heapScanHandler) {
  heapScanHandlers.erase(heapScanHandler);
}

void cde_trxinfo_t::endHeapScanBeforeTransactionCommitOrAbort() {
  for (auto heapscan : heapScanHandlers) {
    if (*heapscan) {
      HeapInterface::EndScan(*heapscan);
      HeapInterface::DestroyHeapScanHandler(*heapscan);
      *heapscan = nullptr;
    }
  }
  heapScanHandlers.clear();
}

void CdeResetTrxInfo(cde_trxinfo_t *trxInfo) {
  trxInfo->my_thd = nullptr;
  trxInfo->check_foreigns = true;
  trxInfo->autoinc_row_num = 0;
  trxInfo->scanSnapshot.Init();
}

} /* namespace CDE */
