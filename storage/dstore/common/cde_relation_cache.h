/*
 * This file is part of the cde-dstore project.
 * Copyright (c) 2025 Huawei Technologies Co.,Ltd.
 */

#ifndef RELATION_CACHE_INCLUDED
#define RELATION_CACHE_INCLUDED

#include <assert.h>
#include <stddef.h>
#include <sys/types.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "lex_string.h"
#include "my_base.h"

#include <list>
#include "my_psi_config.h"
#include "mysql/components/services/bits/mysql_mutex_bits.h"
#include "mysql/components/services/bits/psi_mutex_bits.h"
#include "mysql/psi/mysql_mutex.h"
#include "sql/handler.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_plist.h"
#include "sql/system_variables.h"
#include "sql/table.h"

#include "dict/cde_relation.h"

namespace CDE {

class RelationCacheElement;

extern ulong dstore_relation_cache_size_per_instance,
    dstore_relation_cache_instances;

/**
  Cache for relation objects. Each thread may temporary need a relation object
  to lock a table. Mainly for foreign key check. Such relation should be needed
  only temporarily by a thread and later could be reused by other thread.
*/
class RelationCache {
 public:
  /** Acquire lock on relation cache instance. */
  void lock() { m_lock.lock(); }
  /** Release lock on relation cache instance. */
  void unlock() { m_lock.unlock(); }

  /**
    Get an unused relation from the cache.

    @param[in]  tableId - table id for which we need relation

    @note Caller should own lock on the table cache.

    @retval non-NULL - pointer relation
    @retval NULL     - failed to get a relation
  */
  session_rel_info *getRelation(uint64_t tableId);

  /**
    Put used relation instance back to the relation cache and mark
    it as unused.

    @note Caller should own lock on the table cache.

    @param[in]  relInfo - relation to be returned
  */
  void releaseRelation(session_rel_info *relation);

  /**
     Add newly created relations object which is going to be used right away
    to the cache.

    @note Caller should own lock on the table cache.

    @param[in] relInfo reltion to be added
  */
  void addUsedRelation(session_rel_info *relation);

  /**
    Remove all relations associated with table id and free them.

    @param[in]  tableId - table id for which remove relalation
   */
  void removeRelation(uint64_t tableId);

  /**
   * Destroy all instances of relations from this cache,
   * relations should be released first, thus
   * there should not be any relations in use.
   */
  void destroy();

 private:
  friend class ut_cde_global_rel_cache;

  /**
    The relation cache lock protects the following data:

    1) m_unused_tables list.
    2) m_cache hash.
    3) m_usedRelations, m_freeRelations lists in RelationCacheElement in
       this cache.
    4) m_relationsCount - total number of TABLE objects in this cache.
  */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) std::mutex m_lock;

  /**
    The hash of RelationCacheElements objects. Each relation cache element
    has a list of in use and free session_rel_info objects. Each relation
    cache element is per table id. When thread obtains a session_rel_info
    object, it gets it from m_freeRelations list and when adds an inuse relation
    for a given table id - it stores it into m_usedRelations list.
  */
  std::unordered_map<uint64_t, std::unique_ptr<RelationCacheElement>> m_cache;

  /**
    List that contains all relations (session_rel_info) in this particular
    cache that are not in use by any thread. Recently used relations are
    appended to the end of the list. Thus the beginning of the list contains
    relations which have been least recently used.
  */
  std::list<session_rel_info *> m_unusedRelations;

  /**
    Total number of relations this particular cache (both in use
    by threads and not in use).
  */
  uint m_relationsCount{0};

  /**
    Free unused relations if total number of relations objects
    in cache has exceeded dstore_relation_cache_size_per_instance
    limit.
  */
  void freeUnusedRelationsIfNecessary();
};

class RelationCacheManager {
 public:
  /** Maximum supported number of relation cache instances. */
  static const int MAX_RELATION_CACHES = 64;

  /** Default number of relation cache instances */
  static const int DEFAULT_MAX_RELATION_CACHES = 16;

  /** Get instance of relation cache to be used by particular connection.
    @param[in] threadId id of a thread for which the cache is needed

    @return pointer to relationCache if exists
            nullptr otherwise
  */
  RelationCache *getCache(my_thread_id threadId) {
    return &m_relationCache[threadId % dstore_relation_cache_instances];
  }

  /**
   Remove and free all relations objects for the table id from
   all relation cache instances. All relations that belong
   to an table id should be first released.

   @param  tableId    table id for which remove all relations

   @note Caller should own lock on RelationCacheManager
  */
  void freeRelation(uint64_t tableId);
  /**
   Remove and free all relations in all instances.
   The relation should be first released.
   @note Caller should own lock on RelationCacheManager
  */
  void destroy();

  /** lock whole RelationCacheManager */
  void lock();
  /** unlock RelationCacheManager */
  void unlock();

 private:
  /**
    An array of RelationCache instances.
    Only the first dstore_relation_cache_instances elements in it are used.
  */
  RelationCache m_relationCache[MAX_RELATION_CACHES];

  friend class ut_cde_global_rel_cache;
};

extern RelationCacheManager relationCacheManager;

/**
  Element that represents the relation for table id - in the specific relation
  cache.
*/
class RelationCacheElement {
 private:
  /**
     list of used session_rel_info objects for an table id
     in this table cache (one such list per relation cache).
   */
  std::list<session_rel_info *> m_usedRelations;
  /**
     list of unused session_rel_info objects for an table id
     in this table cache (one such list per relation cache).
   */
  std::list<session_rel_info *> m_freeRelations;

 public:
  RelationCacheElement() = default;

  friend class RelationCache;
  friend class RelationCacheManager;

  friend class ut_cde_global_rel_cache;
};

} /* namespace CDE */
#endif /* RELATION_CACHE_INCLUDED */
