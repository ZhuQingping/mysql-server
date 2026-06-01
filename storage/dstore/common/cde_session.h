/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_session.h
 *
 *
 * IDENTIFICATION
 * src/cde_cde.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef __CDE_SESSION_H__
#define __CDE_SESSION_H__

#include "sql_class.h"
#include "sql_string.h"
// clang-format off
#include "framework/dstore_session_interface.h"
#include "framework/dstore_thread_interface.h"
#include "transaction/dstore_transaction_interface.h"

#include "cde_perf.h"
#include "cde_alloc.h"
#include "dict/cde_dict.h"
#include "dict/cde_relation.h"
#include "heap/dstore_heap_interface.h"
#include "framework/dstore_session_interface.h"
#include "framework/dstore_thread_interface.h"
#include "transaction/dstore_transaction_interface.h"


namespace CDE {

struct cde_trxinfo_t {
  bool check_foreigns; /*!< normally true, but if the user
                       wants to suppress foreign key checks,
                       (in table imports, for example) we
                       set this false */
  cde_dict_index_t *err_index;
  String err_msg;
  THD *my_thd;
  uint32_t autoinc_row_num{0}; /*!< no. of AUTO-INC rows required for
                      an SQL statement. This is useful for
                      multi-row INSERTs */

  /** Tables that were modified by this transaction. */
  TrxTableVec m_modTables;

  /** Tables whose autoinc lock owned by this transaction. */
  TrxTableSet m_autoincLockedTables;

  uint32_t m_sqltablesInused = 0;  /*!< number of dstore tables
                              used in the processing of the current
                              SQL statement in MySQL */

  DSTORE::SnapshotData scanSnapshot;

  /** HeapScan handlers used in this trx.
   *  we use Set and only insert, duplicate is ignore
   *  clear and reset in endHeapScanBeforeTransactionCommitOrAbort
   */
  Memory::Set<DSTORE::HeapScanHandler **> heapScanHandlers;

  cde_trxinfo_t() : check_foreigns(true), err_index(nullptr), my_thd(nullptr) {
    scanSnapshot.Init();
  }

  ~cde_trxinfo_t() {}

  void append_err_msg(const String &msg);
  void append_err_msg(const char *msg, size_t len);
  void append_err_msg_with_quote(const String &msg);
  void append_err_msg_with_quote(const char *msg, size_t len);


  void appendHeapScanHandler(DSTORE::HeapScanHandler **heapScanHandler);
  void removeHeapScanHandler(DSTORE::HeapScanHandler **heapScanHandler);
  // cause heap scan use transaction memory context to construct,
  // before transction commit or rollback, it will reset this memory context
  // so we need invoke endHeapScan to avoid some HeapScanHandler member leak
  void endHeapScanBeforeTransactionCommitOrAbort();
};

/**
Reset transaction info.

@param[in]  trxInfo  transaction info.

@return void
*/
void CdeResetTrxInfo(cde_trxinfo_t *trxInfo);

class cde_session_t {
 public:
  cde_session_t() {
    MutexInit(0, &m_mutex, MY_MUTEX_INIT_FAST);
  }
  ~cde_session_t() {
    MutexDestroy(&m_mutex);
    delete m_ddlLogRelation;
    m_ddlLogRelation = nullptr;
  }
  DSTORE::ThreadContextInterface *get_dstore_thrd() { return dstore_thrd; }
  DSTORE::StorageSession *get_dstore_session() { return dstore_session; }
  cde_trxinfo_t *get_trxinfo() { return &trxinfo; }
  //  private:
  DSTORE::StorageSession *dstore_session{nullptr};
  DSTORE::ThreadContextInterface *dstore_thrd{nullptr};

  cde_trxinfo_t trxinfo;

  mysql_mutex_t m_mutex;
  /** Point to relation info of ddl log table, valid only in ddl query.*/
  session_rel_info *m_ddlLogRelation = nullptr;
  /** Whether session is in an uninterrupted phase. */
  bool m_inUninterruptablePhase = false;
  /** Whether session is mark killed. */
  bool m_setInterrupt = false;

  /**
  Checks if current thrd is same as the value saved in session
  which is set in init.

  @return true if same.
  */
  inline bool IsThrdValid() {
    return DSTORE::ThreadContextInterface::GetCurrentThreadContext() ==
           dstore_thrd;
  }
};

class TempTrxGuard {
 public:
  TempTrxGuard(const char *location);
  ~TempTrxGuard();
  void commit(const char *location);
  bool IsAbnormal() { return m_abnormal; }
  bool IsTrxCreated() { return m_trxCreated; }
 private:
  bool m_trxCreated = false;
  bool m_abnormal = false;
  std::string m_location;
  DSTORE::ThreadContextInterface *m_thrd = nullptr;
};

/**
Below functions about getting session or trx context.
1.CdeCreateOrGetTrxinfo() and CdeSession() will create session or trx if not
exist. 2.CdeCreateOrGetSession() is called by CdeCreateOrGetTrxinfo().
3.CdeGetSession(), CdeGetSessionRef() and CdeGetTrxinfo()
only get session or trx and check they are valid.

For transaction correnctness, CdeCreateOrGetTrxinfo() is only called when
possiable starting a new transaction and CdeCreateOrGetSession() is called in
open() and info_impl() otherwise use Get type functions.

Currently CdeTrxinfo() is called in two functions:
1.CdeTrxStartIfNotStarted() start a new trx which is called in create(),
rename_table(),delete_table(),external_lock() and start_stmt(lock table).
2.CdeStartTrxAndConsistentSnapshot() when execute start stransaction
with snapshot.
*/

/**
Obtain the private handler of cde session specific data
if not exist then create new one.

@param[in,out]  thd MySQL thread handler.

@return reference to private handler
*/
cde_session_t *&CdeCreateOrGetSession(THD *thd);

/**
Get cde session and check it is valid.

@param[in]  thd MySQL thread handler.
@param[in]  assign current thrd to cde session.
@return session ptr
*/
cde_session_t *CdeGetSession(THD *thd, bool assign = true);

/**
Get cde session ref and check it is valid.

@param[in]  thd MySQL thread handler.

@return session ptr
*/
cde_session_t *&CdeGetSessionRef(THD *thd);

/**
Get cde trx and check it is valid.
if not exist then create new one(session).

@param[in]  thd MySQL thread handler.

@return trx ptr
*/
cde_trxinfo_t *CdeCreateOrGetTrxinfo(THD *thd);

/**
Get cde trx and check it is valid.

@param[in]  thd MySQL thread handler.

@return trx ptr
*/
cde_trxinfo_t *CdeGetTrxinfo(THD *thd);
} /* namespace CDE */
#endif /* __CDE_SESSION_H__ */
