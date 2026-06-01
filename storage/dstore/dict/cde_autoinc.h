/*
   Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

#ifndef __CDE_AUTOINC_H__
#define __CDE_AUTOINC_H__

#include <stdint.h>
#include <thread>

struct TABLE;
struct Field;

namespace CDE {

struct session_index_info;
struct cde_trxinfo_t;
struct cde_dict_t;

typedef std::set<cde_dict_t *, std::less<cde_dict_t *>> TrxTableSet;

/** Support three lock-mode of autoinc controled by cde_autoinc_lock_mode,
which with the same behavior as innodb_autoinc_lock_mode. */
enum AutoincLockMode : uint32_t {
  AUTOINC_LOCK_TRADITIONAL = 0,
  AUTOINC_LOCK_CONSECUTIVE,
  AUTOINC_LOCK_INTERLEAVED
};

extern uint32_t g_autoincLockMode;

/** Autoinc related session-level context. As a member of dstore_handler_t. */
struct SessionAutoincCtx {
  /** When autoinc is obtained in batches, this represent last
  value of current batch. */
  uint64_t m_lastVal = 0;
  /** From @@auto_increment_increment. */
  uint64_t m_autoincIncrement = 1;
  /** From @@auto_increment_offset. */
  uint64_t m_autoincOffset = 0;
  /** True if we were asked to skip AUTOINC locking for the table. */
  bool m_noAutoincLocking = false;
};

/** Autoinc related global-level context. As a member of cde_dict_t. */
class DictAutoinc {
 private:
  /** Obey the sequence that acquire m_tableAutoincLock before
  m_mutex. Only acquired when current SQL-statement need table-level
  autoinc lock. */
  std::mutex m_tableAutoincLock;
  /** Mutex to protect other member of this class. */
  std::mutex m_mutex;
  /** Autoinc value to give to the next inserted row. */
  uint64_t m_autoinc = 0;
  /** Flag to indicate whether autoinc value has been initialized. */
  bool m_initialized = false;
  /** The autoinc-lock is a statement-level lock, so the period
  of holding the autoinc-lock must be within the life cycle of a
  statement, which will not change thread. Therefore we can use
  thread::id to represent the owner. Protected by m_mutex. */
  std::thread::id m_ownerId = std::thread::id{};

 public:
  void MutexEnter() { m_mutex.lock(); }
  void MutexExit() { m_mutex.unlock(); }

  /**
  Only be called when current SQL-statement need table-level autoinc
  lock. This function will acquire the table-level autoinc lock and
  enter the mutex of DictAutoinc.
  */
  void TableAutoincLock() {
    MutexEnter();
    if (TableAutoincLockByCurrent()) {
      return;
    }
    /* Release the mutex to avoid deadlocks. */
    MutexExit();
    m_tableAutoincLock.lock();
    MutexEnter();
    CDE_ASSERT_DEBUG(m_ownerId == std::thread::id{});
    m_ownerId = std::this_thread::get_id();
  }

  /**
  Check whether the current thread, i.e. the current SQL-statement,
  owns the autoinc lock when lock-mode is AUTOINC_LOCK_TRADITIONAL.

  @return true if own the lock, otherwise false.
  */
  bool TableAutoincLockByCurrent() {
    /* TODO: add mutex_own of m_mutex at here after base function
    of mutex of Dstore-handler implemented. */
    return m_ownerId == std::this_thread::get_id();
  }

  /**
  Check whether the the autoinc lock is owned by other thread, i.e.
  other SQL-statement, when lock-mode is AUTOINC_LOCK_TRADITIONAL.

  @return true if it is owned by other thread, otherwise false.
  */
  bool TableAutoincLockedByOthers() {
    return m_ownerId != std::thread::id{} &&
           m_ownerId != std::this_thread::get_id();
  }

  /**
  Release the autoinc lock when lock-mode is AUTOINC_LOCK_TRADITIONAL.
  */
  void TableAutoincUnLock() {
    MutexEnter();
    m_tableAutoincLock.unlock();
    m_ownerId = std::thread::id{};
    MutexExit();
  }

  /**
  Set autoinc value, note that the function must be called under
  the protection of mutex or other scenarios without concurrency.

  @param[in]  value  The value witch is set to m_autoinc.
  */
  void SetAutoinc(uint64_t value) { m_autoinc = value; }

  /**
  Get autoinc value, note that the function must be called under
  the protection of mutex or other scenarios without concurrency.

  @return The value of m_autoinc.
  */
  uint64_t GetAutoinc() const { return m_autoinc; }

  /**
  Set inittialize state, note that the function must be called under
  the protection of mutex or other scenarios without concurrency.

  @param[in]  value  The value witch is set to m_initialized.
  */
  void SetInitialized(bool value) { m_initialized = value; }

  /**
  Get inittialize state, note that the function must be called under
  the protection of mutex or other scenarios without concurrency.

  @return true if autoinc value has been initialized, otherwise false.
  */
  bool GetInitialized() { return m_initialized; }

  /** Set autoinc value if greater, note that the function must be called
  under the protection of mutex or other scenarios without concurrency. */
  void SetAutoincIfGreater(uint64_t value) {
    if (value > m_autoinc) {
      m_autoinc = value;
    }
  }
};

/**
Currently autoinc values are not persisted so we need load maximum value
of autoinc from index at the first time the table is loaded.

@param[in,out]  autoincDict   Autoinc struct in cde_dict_t.
@param[in]      table         Table struct from SQL-layer.
@param[in]      ddAutoInvVal  Value of auto inc from table's DD se_private_data.
@param[in]      indexRelation Dstore storage-relation pointer of index which
                              contains the autoinc column.
@param[in]      indexColNum   The number of columns of index which contains
                              the autoinc column.

@return false if success otherwise true.
*/
bool InitAutoinc(DictAutoinc *autoincDict, const TABLE *table,
                 uint64_t ddAutoIncVal, DSTORE::StorageRelation indexRelation,
                 const uint32_t indexColNum);

/**
Enter the mutex of autoincDict, acquire the table-level autoinc lock
according to the definition of lock_mode.

@param[in]  autoincDict   Autoinc struct in cde_dict_t.
@param[in]  tableSet      Tableset for transaction to track autoinc-lock.
@param[in]  command       Current sql-command.
@param[in]  dictTable     cde_dict_t struct of the table.
*/
void LockAutoinc(DictAutoinc *autoincDict, TrxTableSet &tableSet, int command,
                 cde_dict_t *dictTable);

/**
Parse integer value of autoinc from field of autoinc. Type of field
must be integer or floating point, otherwise it will assert.

@param[in]  autoincField  Pointer to field of autoinc.

@return The value of autoinc parsed from the field.
*/
uint64_t ParseIntFromField(const Field *autoincField);

/**
Update the autoinc value by input parameter if it is greater than
the existing autoinc of table. This function is only used to update
autoinc after rows are inserted or field of autoinc is updated.

@param[in]  autoincCtx   Autoinc context struct in dstore_handler_t.
@param[in]  autoincDict  Autoinc struct in cde_dict_t.
@param[in]  autoinc      The input autoinc value.
@param[in]  colMaxValue  The max-value of the autoinc column defined by field.
*/
void UpdateTableAutoinc(SessionAutoincCtx *autoincCtx, DictAutoinc *autoincDict,
                        uint64_t autoinc, uint64_t colMaxValue);

/**
Compute the next autoinc value. For compatibility, the behavior is
the same as innobase_next_autoinc.

@param[in]  current      Current autoinc value.
@param[in]  desireCnt    Count of next autoinc values desired by caller.
@param[in]  step         Increment step defined by @@auto_increment_increment.
@param[in]  offset       Offset defined by @@auto_increment_offset.
@param[in]  colMaxValue  The max-value of the autoinc column defined by field.

@return The next autoinc value.
*/
uint64_t CalcNextAutoinc(uint64_t current, uint64_t desireCnt, uint64_t step,
                         uint64_t offset, uint64_t maxValue);

/**
Load the max value of autoinc from index which contains the autoinc column
when table is opening. During this period of time, there MUST be NO update
on the table.

@param[in]  table         Table struct from SQL-layer.
@param[in]  autoincIndex  Index which contains the autoinc column.
@param[out] value         The max value of autoinc loaded from index.

@return false if success otherwise true
*/
bool LoadMaxAutoincFromIndex(const TABLE *table,
                             DSTORE::StorageRelation indexRelation,
                             const uint32_t indexColNum, uint64_t *value);
} /* namespace CDE */
#endif  // __CDE_AUTOINC_H__
