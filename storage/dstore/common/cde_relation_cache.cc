/*
 * This file is part of the cde-dstore project.
 * Copyright (c) 2025 Huawei Technologies Co.,Ltd.
 */
#include "common/cde_relation_cache.h"
#include "dict/cde_relation.h"

#include <stdio.h>
#include <string.h>

#include "my_macros.h"

namespace CDE {
ulong dstore_relation_cache_size_per_instance, dstore_relation_cache_instances;

/**
  Container for all relation cache instances in the system.
*/
RelationCacheManager relationCacheManager;

void RelationCache::destroy() {
  std::vector<uint64_t> tableIds;
  std::transform(m_cache.begin(), m_cache.end(), std::back_inserter(tableIds),
                 [](const auto &pair) { return pair.first; });

  for (uint64_t tableId : tableIds) removeRelation(tableId);

  CDE_ASSERT_DEBUG(m_cache.empty());
  CDE_ASSERT_DEBUG(m_unusedRelations.empty());
}

void RelationCacheManager::destroy() {
  for (uint32_t i = 0; i < dstore_relation_cache_instances; ++i)
    m_relationCache[i].destroy();
}

void RelationCacheManager::lock() {
  for (uint i = 0; i < dstore_relation_cache_instances; i++)
    m_relationCache[i].lock();
}

void RelationCacheManager::unlock() {
  for (uint i = 0; i < dstore_relation_cache_instances; i++)
    m_relationCache[i].unlock();
}

void RelationCacheManager::freeRelation(uint64_t tableId) {
  for (uint i = 0; i < dstore_relation_cache_instances; i++)
    m_relationCache[i].removeRelation(tableId);
}

void RelationCache::freeUnusedRelationsIfNecessary() {
  /*
    We have too many free relations around let us try to get rid of them.
  */
  if (m_relationsCount > dstore_relation_cache_size_per_instance &&
      !m_unusedRelations.empty()) {
    auto relInfoToFree = m_unusedRelations.front();
    RelationCacheElement *el = m_cache[relInfoToFree->m_tableId].get();
    CDE_ASSERT(nullptr !=
               el);  // it is comming from unused list, should not be nullptr
    el->m_freeRelations.remove(relInfoToFree);
    m_relationsCount--;
    if (el->m_usedRelations.empty() && el->m_freeRelations.empty())
      m_cache.erase(relInfoToFree->m_tableId);
    delete relInfoToFree;

    m_unusedRelations.pop_front();
  }
}

void RelationCache::addUsedRelation(session_rel_info *relInfo) {
  /*
    Try to get relation cache element representing this relation
    in the cache from the hash.
  */
  if (0 == m_cache.count(relInfo->m_tableId)) {
    /** Allocate new RelationCacheElement and add it to the cache */
    m_cache.emplace(relInfo->m_tableId,
                    std::make_unique<RelationCacheElement>());
  }

  RelationCacheElement *el = m_cache[relInfo->m_tableId].get();

  /* Add relation to the in use relations list */
  el->m_usedRelations.push_front(relInfo);

  m_relationsCount++;
}

void RelationCache::removeRelation(uint64_t tableId) {
  if (0 == m_cache.count(tableId)) return;

  RelationCacheElement *el = m_cache[tableId].get();
  // it's OK for relation to not exist in this cache
  if (nullptr == el) {
    m_cache.erase(tableId);
    return;
  }

  // There cannot be a relation in usedRelations list
  // before we remove relation. At least in debug
  // mode. In release mode when exception is thrown
  // it can happen - so we remove the used relations
  // too, but we have debug assert that m_usedRelations
  // is empty for any table id.
  CDE_ASSERT_DEBUG(el->m_usedRelations.empty());

  for (auto usedRelation : el->m_usedRelations) {
    m_relationsCount--;
    delete usedRelation;
  }
  el->m_usedRelations.clear();

  for (auto relation : el->m_freeRelations) {
    m_unusedRelations.remove(relation);
    m_relationsCount--;
    delete relation;
  }

  el->m_freeRelations.clear();

  m_cache.erase(tableId);
}

session_rel_info *RelationCache::getRelation(uint64_t tableId) {
  session_rel_info *relInfo = nullptr;

  const auto elemIt = m_cache.find(tableId);
  if (elemIt == m_cache.end()) return nullptr;
  RelationCacheElement *el = elemIt->second.get();

  if ((relInfo = el->m_freeRelations.front())) {
    /*
      remove relation from list of unused relation objects for this
      tableId in this cache.
    */
    el->m_freeRelations.remove(relInfo);

    /* remove relation from unused tables list for this cache. */
    m_unusedRelations.remove(relInfo);

    /*
      Add relation to list of used relation objects for this relation
      in the relation cache.
    */
    el->m_usedRelations.push_front(relInfo);
  }

  return relInfo;
}

void RelationCache::releaseRelation(session_rel_info *relInfo) {
  RelationCacheElement *el = m_cache[relInfo->m_tableId].get();
  CDE_ASSERT_DEBUG(nullptr != el);
  CDE_ASSERT_DEBUG(std::find(el->m_usedRelations.begin(),
                             el->m_usedRelations.end(),
                             relInfo) != el->m_usedRelations.end());

  /* Remove relation from the list of used relation for the table id in this
  cache. */
  el->m_usedRelations.remove(relInfo);
  /* Add relation to the list of unused objects for the table id in this cache.
   */
  el->m_freeRelations.push_front(relInfo);
  /* Also link it last in the list of unused relation objects for the cache. */
  m_unusedRelations.push_back(relInfo);

  /*
    We free the least used relation, not the subject relation, to keep the LRU
    order. Note that in most common case the below call won't free anything.
  */
  freeUnusedRelationsIfNecessary();
}

}  // namespace CDE