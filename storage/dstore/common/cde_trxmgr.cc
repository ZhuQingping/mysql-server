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

#include "cde_trxmgr.h"
#include "cde_errorcode.h"
#include "cde_session.h"

namespace CDE {

cde_isolation_level_t CdeTrxMapIsolationLevel(enum_tx_isolation iso) {
  DBUG_TRACE;
  cde_isolation_level_t trx_isolation_level = TRANSACTION_SNAPSHOT;

  switch (iso) {
    case ISO_REPEATABLE_READ:
      trx_isolation_level = TRANSACTION_SNAPSHOT;
      break;
    case ISO_READ_COMMITTED:
      trx_isolation_level = READ_COMMITTED;
      break;
    case ISO_SERIALIZABLE:
      trx_isolation_level = SERIALIZABLE;
      break;
    case ISO_READ_UNCOMMITTED:
      trx_isolation_level = READ_COMMITTED;
      break;
    default:
      assert(false);
  }

  return trx_isolation_level;
}

void SingleStmtTrxStart(cde_isolation_level_t isoLevel) {
  CDE_ASSERT(TransactionInterface::GetCurrentTBlockState() ==
             DSTORE::TBlockState::TBLOCK_DEFAULT);

  /* start to handler current sql. */
  DSTORE::RetStatus ret = TransactionInterface::StartTrxCommand();
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);

  /* set snapshot for current statement. */
  TransactionInterface::SetIsolationLevel(isoLevel);
  TransactionInterface::ResetSnapshotCsn();
  ret = TransactionInterface::SetSnapShot();
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
}

void MultiStmtsTrxStart(cde_isolation_level_t isoLevel) {
  CDE_ASSERT(TransactionInterface::GetCurrentTBlockState() ==
             DSTORE::TBlockState::TBLOCK_DEFAULT);

  /* handler begin statement in semantic. */
  DSTORE::RetStatus ret = TransactionInterface::StartTrxCommand();
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
  ret = TransactionInterface::BeginTrxBlock();
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
  ret = TransactionInterface::CommitTrxCommand();
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);

  /* start to handler current sql. */
  ret = TransactionInterface::StartTrxCommand();
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);

  /* set snapshot for current trx. */
  TransactionInterface::SetIsolationLevel(isoLevel);
  TransactionInterface::ResetSnapshotCsn();
  ret = TransactionInterface::SetSnapShot();
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
}

void MultiStmtsTrxRunInProcess(cde_isolation_level_t isoLevel,
                               cde_trxinfo_t *trxInfo) {
  /* start to handler current sql. */
  DSTORE::RetStatus ret = TransactionInterface::StartTrxCommand();
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);

  /* set snapshot for current statement if isolation is READ_COMMITTED. */
  if (isoLevel == READ_COMMITTED) {
    TransactionInterface::ResetSnapshotCsn();
    ret = TransactionInterface::SetSnapShot();
    CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
    trxInfo->scanSnapshot.Init();
  }
}

void SingleStmtTrxCommit(cde_session_t *session) {
  DSTORE::TBlockState state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_STARTED);

  session->get_trxinfo()->endHeapScanBeforeTransactionCommitOrAbort();
  DSTORE::RetStatus ret = TransactionInterface::CommitTrxCommand();
  CdeResetTrxInfo(&session->trxinfo);

  DBUG_EXECUTE_IF("cde_singlestmttrx_commit_injection",
                  ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    CDE_LOG_ERROR(
        "SingleStmTrx commit failed. error code %lld, error msg is %s, thrd "
        "valid is %s.",
        GetDstoreErrcode(), GetDstoreErrmsg(),
        (session->IsThrdValid() ? "Y" : "N"));
    DBUG_EXECUTE_IF("cde_singlestmttrx_commit_injection", return;);
  }

  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
  state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_DEFAULT);
}

void MultiStmtsTrxCommit(cde_session_t *session) {
  DSTORE::TBlockState state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_INPROGRESS);

  DSTORE::RetStatus ret = TransactionInterface::EndTrxBlock();

  DBUG_EXECUTE_IF("cde_multistmttrx_commit_end_block_injection",
                  ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    CDE_LOG_ERROR(
        "MultiStmtTrx's commit end trx block step failed. error code %lld, "
        "error msg is %s, "
        "thrd valid is %s.",
        GetDstoreErrcode(), GetDstoreErrmsg(),
        session->IsThrdValid() ? "Y" : "N");
    DBUG_EXECUTE_IF("cde_multistmttrx_commit_end_block_injection",
                    TransactionInterface::CommitTrxCommand();
                    return;);
  }
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);

  state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_END);

  CdeResetTrxInfo(&session->trxinfo);

  session->get_trxinfo()->endHeapScanBeforeTransactionCommitOrAbort();
  ret = TransactionInterface::CommitTrxCommand();

  DBUG_EXECUTE_IF("cde_multistmttrx_commit_end_stmt_injection",
                  ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    CDE_LOG_ERROR(
        "MultiStmtTrx's commit end stmt step failed. error code %lld, error "
        "msg is %s, thrd "
        "valid is %s.",
        GetDstoreErrcode(), GetDstoreErrmsg(),
        session->IsThrdValid() ? "Y" : "N");
    DBUG_EXECUTE_IF("cde_multistmttrx_commit_end_stmt_injection", return;);
    CDE_ASSERT(false);
  }

  state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_DEFAULT);
}

void MultiStmtsTrxOneStmtEnd(cde_session_t *session) {
  DSTORE::TBlockState state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_INPROGRESS);

  DSTORE::RetStatus ret = TransactionInterface::CommitTrxCommand();

  DBUG_EXECUTE_IF("cde_multistmttrx_one_stmt_end_injection",
                  ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    CDE_LOG_ERROR(
        "MultiStmtTrx's one stmt end failed. error code %lld, error msg is %s, "
        "thrd valid is "
        "%s.",
        GetDstoreErrcode(), GetDstoreErrmsg(),
        session->IsThrdValid() ? "Y" : "N");
    DBUG_EXECUTE_IF("cde_multistmttrx_one_stmt_end_injection", return;);
  }

  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
  state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_INPROGRESS);
}

void SingleStmtTrxRollback(cde_session_t *session) {
  DSTORE::TBlockState state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_STARTED);

  session->get_trxinfo()->endHeapScanBeforeTransactionCommitOrAbort();
  DSTORE::RetStatus ret = TransactionInterface::AbortTrx(false);

  DBUG_EXECUTE_IF("cde_singlestmttrx_rollback_injection",
                  ret = DSTORE::DSTORE_FAIL;);
  DBUG_EXECUTE_IF("AbortTrx_injection", ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    CDE_LOG_ERROR(
        "SingleStmTrx rollback failed. error code %lld, error msg is %s, thrd "
        "valid is %s.",
        GetDstoreErrcode(), GetDstoreErrmsg(),
        (session->IsThrdValid() ? "Y" : "N"));
    DBUG_EXECUTE_IF("cde_singlestmttrx_rollback_injection", return;);
    DBUG_EXECUTE_IF("AbortTrx_injection", return;);
  }

  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
  state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_DEFAULT);

  CdeResetTrxInfo(&session->trxinfo);
}

void MultiStmtsTrxRollback(cde_session_t *session) {
  DSTORE::TBlockState state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_INPROGRESS);

  DSTORE::RetStatus ret = TransactionInterface::UserAbortTrxBlock();
  DBUG_EXECUTE_IF("cde_multistmttrx_rollback_user_abort_injection",
                  ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    CDE_LOG_ERROR(
        "MultiStmtTrx's rollback user abort trx block step failed. error code "
        "%lld, error msg "
        "is %s, thrd valid is %s.",
        GetDstoreErrcode(), GetDstoreErrmsg(),
        session->IsThrdValid() ? "Y" : "N");
    ret = DSTORE::DSTORE_FAIL;
    DBUG_EXECUTE_IF("cde_multistmttrx_rollback_user_abort_injection",
                    ret = DSTORE::DSTORE_SUCC;);
  }
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);

  state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT_DEBUG(state == DSTORE::TBlockState::TBLOCK_ABORT_PENDING);

  session->get_trxinfo()->endHeapScanBeforeTransactionCommitOrAbort();
  ret = TransactionInterface::AbortTrx();
  DBUG_EXECUTE_IF("cde_multistmttrx_rollback_abort_trx_injection",
                  ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    CDE_LOG_ERROR(
        "MultiStmtTrx's rollback abort trx step failed. error code %lld, error "
        "msg is %s, thrd "
        "valid is %s.",
        GetDstoreErrcode(), GetDstoreErrmsg(),
        session->IsThrdValid() ? "Y" : "N");
    ret = DSTORE::DSTORE_FAIL;
    DBUG_EXECUTE_IF("cde_multistmttrx_rollback_abort_trx_injection",
                    ret = DSTORE::DSTORE_SUCC;);
  }

  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
  state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_DEFAULT);

  CdeResetTrxInfo(&session->trxinfo);
}

void MultiStmtsTrxOneStmtRollback(cde_session_t *session) {
  DSTORE::TBlockState state = TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT(state == DSTORE::TBlockState::TBLOCK_INPROGRESS);

  DSTORE::RetStatus ret = TransactionInterface::RollbackLastSQLCmd();
  DBUG_EXECUTE_IF("cde_multistmttrx_one_stmt_rollback_injection",
                  ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    CDE_LOG_ERROR(
        "MultiStmtTrx's one stmt rollback failed. error code %lld, error msg "
        "is %s, thrd "
        "valid is %s.",
        GetDstoreErrcode(), GetDstoreErrmsg(),
        session->IsThrdValid() ? "Y" : "N");
    DBUG_EXECUTE_IF("cde_multistmttrx_one_stmt_rollback_injection",
                    ret = DSTORE::DSTORE_SUCC;);
  }

  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
  CDE_ASSERT(TransactionInterface::GetCurrentTBlockState() ==
             DSTORE::TBlockState::TBLOCK_INPROGRESS);
}

void CDEInitSnapshot(DSTORE::SnapshotData &snapshot, ulint lockType) {
  /* for no lock statement, use transaction snapshot
  see CdeTrxStartIfNotStarted. rc is statement level,
  rr and serializable are transaction level */
  if (lockType == LOCK_NONE) {
    CDESetSnapshotByTrans(snapshot);
  } else {
    /* for lock statement.
    rc, rr and serializable use current snapshot for compatible with MySQL.
    for example replace into statement may has two inner statement,
    the second statement need use current snapshot otherwise
    statement level snapshot which is smaller than former and
    could not see data committed after the statement begin time
    and before second inner statement. */
    CDESetSnapshotByCurrent(snapshot);
  }
}

AutonomousTrxGuard::AutonomousTrxGuard(bool needCreate, const char *location)
    : m_location(location) {
  if (needCreate == false) {
    return;
  }

  DSTORE::RetStatus ret = TransactionInterface::CreateAutonomousTransaction();
  if (unlikely(ret == DSTORE::DSTORE_FAIL)) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR("Create autonomous trx fail at %s.",
                                    location);
    m_abnormal = true;
    return;
  }

  ret = TransactionInterface::StartTrxCommand();
  if (unlikely(ret == DSTORE::DSTORE_FAIL)) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR("Start autonomous trx fail at %s.",
                                    location);
    m_abnormal = true;
    TransactionInterface::DestroyAutonomousTransaction();
    return;
  }

  ret = TransactionInterface::SetSnapShot();
  if (unlikely(ret == DSTORE::DSTORE_FAIL)) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR(
        "Set snapshot for autonomous trx fail at %s.", location);
    m_abnormal = true;
    ret = TransactionInterface::AbortTrx(false, true);
    if (unlikely(ret != DSTORE::DSTORE_SUCC)) {
      /* Abort temp trx should never fail unless caused by bugs. Even
      it failed, clean-up and status-reset will be finished anyway. So
      just warning.*/
      CDE_LOG_WARN_WITH_DSTORE_ERROR("Abort autonomous trx fail at %s.",
                                     m_location.c_str());
    }
    TransactionInterface::DestroyAutonomousTransaction();
    return;
  }
  m_trxCreated = true;
}

AutonomousTrxGuard::~AutonomousTrxGuard() {
  if (!m_trxCreated) {
    return;
  }
  CDE_ASSERT(m_abnormal == false);
  DSTORE::RetStatus ret = TransactionInterface::AbortTrx(false, true);
  if (unlikely(ret != DSTORE::DSTORE_SUCC)) {
    /* Abort temp trx should never fail unless caused by bugs. Even
    it failed, clean-up and status-reset will be finished anyway. So
    just warning.*/
    CDE_LOG_WARN_WITH_DSTORE_ERROR("Abort autonomous trx fail at %s.",
                                   m_location.c_str());
  }
  TransactionInterface::DestroyAutonomousTransaction();
  m_trxCreated = false;
}

void AutonomousTrxGuard::commit(const char *location) {
  if (!m_trxCreated) {
    return;
  }

  CDE_ASSERT(m_abnormal == false);
  DSTORE::RetStatus ret = TransactionInterface::CommitTrxCommand();
  if (unlikely(ret != DSTORE::DSTORE_SUCC)) {
    /* Commit should not failed unless coding bugs. Even if it failed
    and we return error to sever_layer, the sever_layer can't handle it. */
    CDE_LOG_FATAL_WITH_DSTORE_ERROR("Commit autonomous trx fail at %s.",
                                    location);
  }
  TransactionInterface::DestroyAutonomousTransaction();
  m_trxCreated = false;
}

} /* namespace CDE */
