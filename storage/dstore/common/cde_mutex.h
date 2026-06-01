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

#ifndef __CDE_MUTEX_H__
#define __CDE_MUTEX_H__

#include "include/mysql/psi/mysql_mutex.h"

namespace CDE {

struct CdeSrcLocation {
  const char *m_fileName;
  size_t m_fileLine;
};

#define CDE_LOCATION_HERE (CdeSrcLocation{__FILE__, __LINE__})

/**
Registers mutex instrumentation information with the performance schema.

@param[in]  category  Instrumentation category name
@param[in]  info      Array of mutex instrumentation descriptors
@param[in]  count     Number of elements in the info array
*/
static inline void MutexRegister(const char *category, PSI_mutex_info *info,
                                 int count) {
  mysql_mutex_register(category, info, count);
}

/**
Initializes a mutex with optional attributes.

@param[in]  key   Performance schema key for instrumentation
@param[out] mtx  Pointer to the mutex to initialize
@param[in]  attr  Attributes for mutex initialization

@return Operation status (0 on success)
*/
static inline int MutexInit(PSI_mutex_key key, mysql_mutex_t *mtx,
                            const native_mutexattr_t *attr) {
  return mysql_mutex_init(key, mtx, attr);
}

/**
Initializes a mutex with source location tracking.

@param[in]  key      Performance schema key for instrumentation
@param[out] mtx     Pointer to the mutex to initialize
@param[in]  attr     Attributes for mutex initialization
@param[in]  srcFile  Source file name where initialization occurs
@param[in]  srcLine  Source line number where initialization occurs

@return Operation status (0 on success)
*/
static inline int MutexInitWithSource(PSI_mutex_key key, mysql_mutex_t *mtx,
                                      const native_mutexattr_t *attr,
                                      const char *srcFile, uint srcLine) {
  return mysql_mutex_init_with_src(key, mtx, attr, srcFile, srcLine);
}

/**
Acquires ownership of a mutex (blocking).

@param[in,out]  mtx  Mutex to acquire

@return Operation status (0 on success)
*/
static inline int MutexAcquire(mysql_mutex_t *mtx) {
  return mysql_mutex_lock(mtx);
}

/**
Acquires ownership of a mutex with source location tracking.

@param[in,out]  mtx    Mutex to acquire
@param[in]      srcFile Source file name where acquisition occurs
@param[in]      srcLine Source line number where acquisition occurs

@return Operation status (0 on success)
*/
static inline int MutexAcquireWithSource(mysql_mutex_t *mtx,
                                         const char *srcFile, uint srcLine) {
  return mysql_mutex_lock_with_src(mtx, srcFile, srcLine);
}

/**
Attempts to acquire ownership of a mutex (non-blocking).

@param[in,out]  mtx  Mutex to try acquiring

@return Operation status (0 if acquired successfully)
*/
static inline int MutexTryAcquire(mysql_mutex_t *mtx) {
  return mysql_mutex_trylock(mtx);
}

/**
Attempts to acquire ownership of a mutex with source location tracking.

@param[in,out]  mtx    Mutex to try acquiring
@param[in]      srcFile Source file name where attempt occurs
@param[in]      srcLine Source line number where attempt occurs

@return Operation status (0 if acquired successfully)
*/
static inline int MutexTryAcquireWithSource(mysql_mutex_t *mtx,
                                            const char *srcFile, uint srcLine) {
  return mysql_mutex_trylock_with_src(mtx, srcFile, srcLine);
}

/**
Releases ownership of a mutex.

@param[in,out]  mtx  Mutex to release

@return Operation status (0 on success)
*/
static inline int MutexRelease(mysql_mutex_t *mtx) {
  return mysql_mutex_unlock(mtx);
}

/**
Releases ownership of a mutex with source location tracking.

@param[in,out]  mtx    Mutex to release
@param[in]      srcFile Source file name where release occurs
@param[in]      srcLine Source line number where release occurs

@return Operation status (0 on success)
*/
static inline int MutexReleaseWithSource(mysql_mutex_t *mtx,
                                         const char *srcFile, uint srcLine) {
  return mysql_mutex_unlock_with_src(mtx, srcFile, srcLine);
}

/**
Destroys a mutex and releases associated resources.

@param[in,out]  mutex  Mutex to destroy

@return Operation status (0 on success)
*/
static inline int MutexDestroy(mysql_mutex_t *mtx) {
  return mysql_mutex_destroy(mtx);
}

/**
Destroys a mutex with source location tracking.

@param[in,out]  mtx    Mutex to destroy
@param[in]      srcFile Source file name where destruction occurs
@param[in]      srcLine Source line number where destruction occurs

@return Operation status (0 on success)
*/
static inline int MutexDestroyWithSource(mysql_mutex_t *mtx,
                                         const char *srcFile, uint srcLine) {
  return mysql_mutex_destroy_with_src(mtx, srcFile, srcLine);
}

/**
Asserts mtx the current thread owns the mutex.
It depends on the underlying implementation and may be a no-op function.

@param[in]  mtx  Mutex to check ownership
*/
static inline void MutexAssertOwner([[maybe_unused]] mysql_mutex_t *mtx) {
  mysql_mutex_assert_owner(mtx);
}

/**
Asserts mtx the current thread does NOT own the mutex.
It depends on the underlying implementation and may be a no-op function.

@param[in]  mtx  Mutex to check ownership
*/
static inline void MutexAssertNotOwner([[maybe_unused]] mysql_mutex_t *mtx) {
  mysql_mutex_assert_not_owner(mtx);
}

class CdeMutexGuard {
  /** Current mutex for RAII */
  mysql_mutex_t *m_mutex;
  bool m_isLocked = false;

 public:
  /**
  Constructor to acquire mutex
  @param[in]   in_mutex        input mutex
  @param[in]   location        defines source file and line in code where the
                               constructor of CdeMutexGuard is called
  */
  CdeMutexGuard(mysql_mutex_t *mutex, const CdeSrcLocation &location,
                bool lockNow = true)
      : m_mutex(mutex) {
    CDE_ASSERT_DEBUG(nullptr != mutex);
    if (lockNow) {
      MutexAcquireWithSource(mutex, location.m_fileName, location.m_fileLine);
      m_isLocked = true;
    }
  }

  /**
  Destructor to release mutex
  */
  ~CdeMutexGuard() { Clear(); }

  void Clear() {
    if (m_isLocked) {
      MutexRelease(m_mutex);
      m_isLocked = false;
    }
  }

  /** Disable copy construction and operator= */
  CdeMutexGuard(CdeMutexGuard const &) = delete;
  CdeMutexGuard &operator=(CdeMutexGuard const &) = delete;
};

enum class CdeRwlockOp : uint8_t { WR_LOCK = 0, RD_LOCK };

class CdeRwlockGuard {
  pthread_rwlock_t *m_rwlock{nullptr};
  bool m_isLocked = false;

 public:
  CdeRwlockGuard(pthread_rwlock_t *rwlock, CdeRwlockOp op) {
    CDE_ASSERT_DEBUG(rwlock != nullptr);
    m_rwlock = rwlock;
    int32_t err = 0;
    switch (op) {
      case CdeRwlockOp::RD_LOCK:
        err = pthread_rwlock_rdlock(m_rwlock);
        CDE_ASSERT(err == 0);
        break;
      case CdeRwlockOp::WR_LOCK:
        err = pthread_rwlock_wrlock(m_rwlock);
        CDE_ASSERT(err == 0);
        break;
      default:
        CDE_ASSERT(false);
    }
    m_isLocked = true;
  }

  ~CdeRwlockGuard() { Clear(); }

  void Clear() {
    if (m_isLocked) {
      int32_t err = pthread_rwlock_unlock(m_rwlock);
      CDE_ASSERT(err == 0);
      m_isLocked = false;
    }
  }

  /** Disable copy construction and operator= */
  CdeRwlockGuard(CdeRwlockGuard const &) = delete;
  CdeRwlockGuard &operator=(CdeRwlockGuard const &) = delete;
};

}  // namespace CDE

#endif  // __CDE_MUTEX_H__
