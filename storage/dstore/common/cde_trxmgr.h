/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_trxmgr.h
 *
 *
 * IDENTIFICATION
 * src/cde_trxmgr.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef __CDE_TRXMGR_H__
#define __CDE_TRXMGR_H__

#include <string>
#include "sql/handler.h"

#include "cde_srv.h"
#include "systable/dstore_relation.h"
#include "transaction/dstore_transaction_interface.h"
#include "transaction/dstore_transaction_struct.h"

namespace CDE {
class cde_session_t;

/*
 * this defined enmu should Keep Consistency with dstore_transaction_types.h
 */
enum cde_isolation_level_t {
  READ_UNCOMMITED,
  READ_COMMITTED,
  TRANSACTION_SNAPSHOT,
  SERIALIZABLE,
  UNSUPPORTED
};

/* Basic lock modes */
enum lock_mode {
  LOCK_IS = 0,          /* intention shared */
  LOCK_IX,              /* intention exclusive */
  LOCK_S,               /* shared */
  LOCK_X,               /* exclusive */
  LOCK_AUTO_INC,        /* locks the auto-inc counter of a table
                        in an exclusive mode */
  LOCK_NONE,            /* this is used elsewhere to note consistent read */
  LOCK_NUM = LOCK_NONE, /* number of lock modes */
  LOCK_NONE_UNSET = 255
};

cde_isolation_level_t CdeTrxMapIsolationLevel(enum_tx_isolation iso);

/**
Lock the tuple by relation's ctid and return tuple.

@param[in]      isoLevel       snapshot isolation level
@param[in]      relation       dstore table handler
@param[in]      ctid           id of the tuple to lock
@param[out]     tuple          the locked tuple
@param[in]      needWait       if we need to wait when the tuple is locked

@return CDE_OK or error code.
*/
int CdeLockTuple(cde_isolation_level_t isoLevel, DSTORE::Snapshot scanSnapshot,
                 DSTORE::StorageRelation relation, DSTORE::ItemPointer ctid,
                 DSTORE::HeapTuple **tuple, bool needWait = true);

/**
Do single statement trx commit.
@param[in]  session  current session.
@return void
*/
void SingleStmtTrxCommit(cde_session_t *session);

/**
Do multi statement trx commit.
@param[in]  session  current session.
@return void
*/
void MultiStmtsTrxCommit(cde_session_t *session);

/**
Do multi statement trx one sql end.
@param[in]  session  current session.
@return void
*/
void MultiStmtsTrxOneStmtEnd(cde_session_t *session);

/**
Do single statement trx rollback.
@param[in]  session  current session.
@return void
*/
void SingleStmtTrxRollback(cde_session_t *session);
void MultiStmtsTrxRollback(cde_session_t *session);
void MultiStmtsTrxOneStmtRollback(cde_session_t *session);

/**
Do single statement trx start.
@param[in]  isoLevel  isolation level.
@return void
*/
void SingleStmtTrxStart(cde_isolation_level_t isoLevel);

/**
Do multi statement trx run in process.
@param[in]  isoLevel  isolation level.
@param[in]  trxInfo trx.
@return void
*/
void MultiStmtsTrxRunInProcess(cde_isolation_level_t isoLevel,
                               cde_trxinfo_t *trxInfo);

/**
Do multi statement trx start.
@param[in]  isoLevel  isolation level.
@return void
*/
void MultiStmtsTrxStart(cde_isolation_level_t isoLevel);

/**
init snapshot value by isolation level and current sql executed.

@param[out]     snapshot snapshot to be set
@param[in]      lockType lock type

*/
void CDEInitSnapshot(DSTORE::SnapshotData &snapshot, ulint lockType);

/**
set snapshot value by current snapshot.

@param[out]  snapshot  snapshot to be set
*/
inline void CDESetSnapshotByCurrent(DSTORE::SnapshotData &snapshot) {
  snapshot.SetSnapshotType(DSTORE::SnapshotType::SNAPSHOT_MVCC);
  snapshot.SetCid(TransactionInterface::GetCurCid());
  snapshot.SetCsn(TransactionInterface::GetLatestSnapshotCsn());
};

/**
set snapshot value by transaction snapshot.

@param[out]      snapshot snapshot to be set
*/
inline void CDESetSnapshotByTrans(DSTORE::SnapshotData &snapshot) {
  snapshot.SetSnapshotType(DSTORE::SnapshotType::SNAPSHOT_MVCC);
  snapshot.SetCid(TransactionInterface::GetCurCid());
  snapshot.SetCsn(TransactionInterface::GetTransactionSnapshotCsn());
};

/**
Guard for start/commit/abort autonomous transaction
*/
class AutonomousTrxGuard {
 public:
  /** Constructor.
  @param[in]  needCreate     wether need to create autonomous transaction
  @param[in] location        the file location that called the constructor. */
  AutonomousTrxGuard(bool needCreate, const char *location);

  ~AutonomousTrxGuard();

  void commit(const char *location);

  bool IsAbnormal() { return m_abnormal; }

  bool IsTrxCreated() { return m_trxCreated; }

 private:
  bool m_trxCreated = false;
  bool m_abnormal = false;
  std::string m_location;
};

} /* namespace CDE */
#endif /* __CDE_TRXMGR_H__ */
