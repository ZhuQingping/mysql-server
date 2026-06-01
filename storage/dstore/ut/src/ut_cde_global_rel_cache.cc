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

#include "common/cde_perf.h"
#include "common/cde_relation_cache.h"
#include "utils/ut_cde_test.h"

namespace CDE {

class ut_cde_global_rel_cache : public CDETEST {
 protected:
  static void SetUpTestCase() { cde_perf_counters::createInstance(); }

  static void TearDownTestCase() { cde_perf_counters::terminateInstance(); }

  void SetUp() override {}

  void TearDown() override {}

  void cde_ddl_insert_relation_inner() {
    dstore_relation_cache_instances =
        RelationCacheManager::DEFAULT_MAX_RELATION_CACHES;

    RelationCache *cache1 = relationCacheManager.getCache(1);
    RelationCache *cache2 = relationCacheManager.getCache(2);

    EXPECT_NE(cache1, cache2);
  }

  void cde_ddl_insert_relation_max_1_cache_inner() {
    dstore_relation_cache_instances = 1;

    RelationCache *cache1 = relationCacheManager.getCache(1);
    RelationCache *cache2 = relationCacheManager.getCache(2);
    RelationCache *cache3 = relationCacheManager.getCache(3);

    EXPECT_EQ(cache1, cache2);
    EXPECT_EQ(cache3, cache2);
  }

  void cde_ddl_insert_relation_max_1_cache_inner_the_same_oid_inner() {
    dstore_relation_cache_instances = 1;
    dstore_relation_cache_size_per_instance = 100;

    RelationCache *cache1 = relationCacheManager.getCache(1);
    RelationCache *cache2 = relationCacheManager.getCache(2);
    RelationCache *cache3 = relationCacheManager.getCache(3);
    /** all those cache pointers point to the same cache */
    EXPECT_EQ(cache1, cache2);
    EXPECT_EQ(cache3, cache2);

    session_rel_info *relInfo[3];
    for (uint i = 0; i < 3; ++i) {
      relInfo[i] = new session_rel_info(1);
      assert(relInfo[i]->rd_storage_releation == nullptr);
    }
    cache1->addUsedRelation(relInfo[0]);
    cache2->addUsedRelation(relInfo[1]);
    cache3->addUsedRelation(relInfo[2]);

    EXPECT_EQ(cache1->m_cache[relInfo[0]->m_tableId]->m_freeRelations.size(),
              (uint32_t)0);
    EXPECT_EQ(cache2->m_cache[relInfo[1]->m_tableId]->m_freeRelations.size(),
              (uint32_t)0);
    EXPECT_EQ(cache3->m_cache[relInfo[2]->m_tableId]->m_freeRelations.size(),
              (uint32_t)0);

    EXPECT_EQ(cache1->m_cache[relInfo[0]->m_tableId]->m_usedRelations.size(),
              (uint32_t)3);
    EXPECT_EQ(cache2->m_cache[relInfo[1]->m_tableId]->m_usedRelations.size(),
              (uint32_t)3);
    EXPECT_EQ(cache3->m_cache[relInfo[2]->m_tableId]->m_usedRelations.size(),
              (uint32_t)3);

    EXPECT_EQ(cache1->m_unusedRelations.size(), (uint32_t)0);

    cache1->releaseRelation(relInfo[0]);
    cache2->releaseRelation(relInfo[1]);
    cache3->releaseRelation(relInfo[2]);

    EXPECT_EQ(cache1->m_cache[relInfo[0]->m_tableId]->m_freeRelations.size(),
              (uint32_t)3);
    EXPECT_EQ(cache2->m_cache[relInfo[1]->m_tableId]->m_freeRelations.size(),
              (uint32_t)3);
    EXPECT_EQ(cache3->m_cache[relInfo[2]->m_tableId]->m_freeRelations.size(),
              (uint32_t)3);

    EXPECT_EQ(cache1->m_cache[relInfo[0]->m_tableId]->m_usedRelations.size(),
              (uint32_t)0);
    EXPECT_EQ(cache2->m_cache[relInfo[1]->m_tableId]->m_usedRelations.size(),
              (uint32_t)0);
    EXPECT_EQ(cache3->m_cache[relInfo[2]->m_tableId]->m_usedRelations.size(),
              (uint32_t)0);

    EXPECT_EQ(cache1->m_unusedRelations.size(), (uint32_t)3);

    relationCacheManager.destroy();
  }

  void cde_ddl_free_unused_relations_if_nessecary_inner() {
    dstore_relation_cache_instances = 1;
    /** limit the number of allowed unused relations to 1 */
    dstore_relation_cache_size_per_instance = 1;

    RelationCache *cache = relationCacheManager.getCache(1);
    uint64_t tableId = 1;

    session_rel_info *relInfo[3];
    for (uint i = 0; i < 3; ++i) {
      relInfo[i] = new session_rel_info(tableId);
      assert(relInfo[i]->rd_storage_releation == nullptr);
    }

    cache->addUsedRelation(relInfo[0]);
    cache->addUsedRelation(relInfo[1]);
    cache->addUsedRelation(relInfo[2]);

    EXPECT_EQ(cache->m_cache[tableId]->m_freeRelations.size(), (uint32_t)0);
    EXPECT_EQ(cache->m_unusedRelations.size(), (uint32_t)0);
    EXPECT_EQ(cache->m_cache[tableId]->m_usedRelations.size(), (uint32_t)3);
    /** we have 3 relations in use, now release them all */
    cache->releaseRelation(relInfo[0]);
    cache->releaseRelation(relInfo[1]);
    cache->releaseRelation(relInfo[2]);

    /** since we have limit to 1 unused relation, there should be only one
        unused relation left */
    EXPECT_EQ(cache->m_cache[tableId]->m_freeRelations.size(), (uint32_t)1);
    EXPECT_EQ(cache->m_unusedRelations.size(), (uint32_t)1);
    EXPECT_EQ(cache->m_cache[tableId]->m_usedRelations.size(), (uint32_t)0);

    relationCacheManager.destroy();
  }

  /** [startOid, endOid) */
  static void addRelations(const uint32_t numberOfRelationsPerOid,
                           my_thread_id threadId, Oid startOid, Oid endOid) {
    while (startOid < endOid) {
      for (uint32_t i = 0; i < numberOfRelationsPerOid; ++i) {
        session_rel_info *relInfo(new session_rel_info(startOid));

        RelationCache *cache = relationCacheManager.getCache(threadId);
        std::lock_guard<RelationCache> lock(*cache);
        cache->addUsedRelation(relInfo);
      }
      startOid++;
    }
  }

  static void getRelations(my_thread_id threadId,
                           const uint32_t numberOfRelationsToGet,
                           std::set<Oid> &retrievedOids) {
    RelationCache *cache = relationCacheManager.getCache(threadId);
    uint32_t numberOfRelationsRetrieved{0};
    /** only released relations can be retrieved */
    while (numberOfRelationsRetrieved < numberOfRelationsToGet) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(10));
      std::lock_guard<RelationCache> lock(*cache);
      uint32_t numberOfUnunsedRelationsInCache =
          cache->m_unusedRelations.size();
      if (numberOfUnunsedRelationsInCache > 0) {
        uint64_t expectedOid = cache->m_unusedRelations.front()->m_tableId;
        session_rel_info *rel =
            cache->getRelation(cache->m_unusedRelations.front()->m_tableId);
        EXPECT_EQ(rel->m_tableId, expectedOid);
        retrievedOids.insert(rel->m_tableId);
        EXPECT_EQ(cache->m_unusedRelations.size(),
                  numberOfUnunsedRelationsInCache - 1);
        ++numberOfRelationsRetrieved;
      }
    }
  }

  /** [startOid, endOid) */
  static void relaseRelationsAtRandom(my_thread_id threadId, uint32_t startOid,
                                      uint32_t endOid) {
    CDE_ASSERT_DEBUG(startOid < endOid);
    uint32_t numberOfRelationsToRelease = endOid - startOid;
    std::srand(std::time(0));
    std::set<Oid> oids_released;
    RelationCache *cache = relationCacheManager.getCache(threadId);
    while (oids_released.size() < numberOfRelationsToRelease) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(10));
      Oid randomOid = startOid + (std::rand() % (endOid - startOid));
      if (oids_released.count(randomOid) != 0) continue;
      std::lock_guard<RelationCache> lock(*cache);
      if (cache->m_cache.count(randomOid) == 0)  // oid still not added
        continue;
      RelationCacheElement *el = cache->m_cache[randomOid].get();
      cache->releaseRelation(el->m_usedRelations.front());
      oids_released.insert(randomOid);
    }
  }

  void releaseAllInUseRelationsFromAllCachesInUse() {
    for (uint i = 0; i < dstore_relation_cache_instances; ++i) {
      std::list<session_rel_info *> relationsToRelease;
      for (auto &relCacheElement :
           relationCacheManager.m_relationCache[i].m_cache) {
        for (auto usedRelation : relCacheElement.second->m_usedRelations)
          relationsToRelease.push_back(usedRelation);
      }
      for (auto relationToRelease : relationsToRelease)
        relationCacheManager.m_relationCache[i].releaseRelation(
            relationToRelease);
    }
  }

  void
  three_threads_add_to_the_same_cache_get_release_at_random_1_rel_per_oid_inner() {
    dstore_relation_cache_instances = 3;
    dstore_relation_cache_size_per_instance = 5001;
    /** Make sure out threads will end up in the same cache */
    my_thread_id storeThreadId = 1, getThreadId = 4, releaseThreadId = 7;
    EXPECT_EQ(relationCacheManager.getCache(storeThreadId),
              relationCacheManager.getCache(getThreadId));
    EXPECT_EQ(relationCacheManager.getCache(storeThreadId),
              relationCacheManager.getCache(releaseThreadId));

    std::vector<std::thread> threads;

    std::set<Oid> retrievedOids;
    threads.emplace_back(addRelations, 1, storeThreadId, 0, 5000);
    threads.emplace_back(getRelations, getThreadId, 5000,
                         std::ref(retrievedOids));
    threads.emplace_back(relaseRelationsAtRandom, releaseThreadId, 0, 5000);

    for (uint i = 0; i < 3; ++i) threads[i].join();

    EXPECT_EQ(retrievedOids.size(), (uint32_t)5000);

    /** Ok, now release all again to be able to remove relations in destroy */
    releaseAllInUseRelationsFromAllCachesInUse();

    relationCacheManager.destroy();
  }

  void
  nine_threads_add_to_different_caches_get_release_at_random_1_rel_per_oid_inner() {
    dstore_relation_cache_instances = 3;
    dstore_relation_cache_size_per_instance = 5001;
    uint32_t numberOfThreads = 9;
    /** Make sure out threads will end up in the same cache */
    my_thread_id storeThreadIdCache1 = 1, getThreadIdCache1 = 4,
                 releaseThreadIdCache1 = 7;
    EXPECT_EQ(relationCacheManager.getCache(storeThreadIdCache1),
              relationCacheManager.getCache(getThreadIdCache1));
    EXPECT_EQ(relationCacheManager.getCache(storeThreadIdCache1),
              relationCacheManager.getCache(releaseThreadIdCache1));

    my_thread_id storeThreadIdCache2 = 2, getThreadIdCache2 = 5,
                 releaseThreadIdCache2 = 8;
    EXPECT_EQ(relationCacheManager.getCache(storeThreadIdCache2),
              relationCacheManager.getCache(getThreadIdCache2));
    EXPECT_EQ(relationCacheManager.getCache(storeThreadIdCache2),
              relationCacheManager.getCache(releaseThreadIdCache2));

    my_thread_id storeThreadIdCache3 = 3, getThreadIdCache3 = 6,
                 releaseThreadIdCache3 = 9;
    EXPECT_EQ(relationCacheManager.getCache(storeThreadIdCache3),
              relationCacheManager.getCache(getThreadIdCache3));
    EXPECT_EQ(relationCacheManager.getCache(storeThreadIdCache3),
              relationCacheManager.getCache(releaseThreadIdCache3));

    EXPECT_NE(relationCacheManager.getCache(storeThreadIdCache1),
              relationCacheManager.getCache(storeThreadIdCache2));
    EXPECT_NE(relationCacheManager.getCache(storeThreadIdCache1),
              relationCacheManager.getCache(storeThreadIdCache3));
    EXPECT_NE(relationCacheManager.getCache(storeThreadIdCache2),
              relationCacheManager.getCache(storeThreadIdCache3));

    std::vector<std::thread> threads;

    /** cache1 */
    std::set<Oid> retrievedOidsCache1;
    threads.emplace_back(addRelations, 1, storeThreadIdCache1, 0, 5000);
    threads.emplace_back(getRelations, getThreadIdCache1, 5000,
                         std::ref(retrievedOidsCache1));
    threads.emplace_back(relaseRelationsAtRandom, releaseThreadIdCache1, 0,
                         5000);

    /** cache2 */
    std::set<Oid> retrievedOidsCache2;
    threads.emplace_back(addRelations, 1, storeThreadIdCache2, 0, 5000);
    threads.emplace_back(getRelations, getThreadIdCache2, 5000,
                         std::ref(retrievedOidsCache2));
    threads.emplace_back(relaseRelationsAtRandom, releaseThreadIdCache2, 0,
                         5000);

    /** cache3 */
    std::set<Oid> retrievedOidsCache3;
    threads.emplace_back(addRelations, 1, storeThreadIdCache3, 0, 5000);
    threads.emplace_back(getRelations, getThreadIdCache3, 5000,
                         std::ref(retrievedOidsCache3));
    threads.emplace_back(relaseRelationsAtRandom, releaseThreadIdCache3, 0,
                         5000);

    for (uint i = 0; i < numberOfThreads; ++i) threads[i].join();

    EXPECT_EQ(retrievedOidsCache1.size(), (uint32_t)5000);
    EXPECT_EQ(retrievedOidsCache2.size(), (uint32_t)5000);
    EXPECT_EQ(retrievedOidsCache3.size(), (uint32_t)5000);

    releaseAllInUseRelationsFromAllCachesInUse();
    relationCacheManager.destroy();
  }

  void remove_rel_from_all_caches_inner() {
    dstore_relation_cache_instances = 3;
    dstore_relation_cache_size_per_instance = 50001;
    uint32_t numberOfThreads = 3;
    /** Make sure that threads will end up in the same cache */
    my_thread_id storeThreadIdCache1 = 1, storeThreadIdCache2 = 2,
                 storeThreadIdCache3 = 3;

    EXPECT_NE(relationCacheManager.getCache(storeThreadIdCache1),
              relationCacheManager.getCache(storeThreadIdCache2));
    EXPECT_NE(relationCacheManager.getCache(storeThreadIdCache1),
              relationCacheManager.getCache(storeThreadIdCache3));
    EXPECT_NE(relationCacheManager.getCache(storeThreadIdCache2),
              relationCacheManager.getCache(storeThreadIdCache3));

    std::vector<std::thread> threads;

    threads.emplace_back(addRelations, 10, storeThreadIdCache1, 0, 5000);
    threads.emplace_back(addRelations, 10, storeThreadIdCache2, 0, 5000);
    threads.emplace_back(addRelations, 10, storeThreadIdCache3, 0, 5000);

    for (uint i = 0; i < numberOfThreads; ++i) threads[i].join();

    RelationCache *cache1 = relationCacheManager.getCache(storeThreadIdCache1);
    RelationCache *cache2 = relationCacheManager.getCache(storeThreadIdCache2);
    RelationCache *cache3 = relationCacheManager.getCache(storeThreadIdCache3);

    EXPECT_EQ(cache1->m_cache.size(), (uint32_t)5000);
    EXPECT_EQ(cache2->m_cache.size(), (uint32_t)5000);
    EXPECT_EQ(cache3->m_cache.size(), (uint32_t)5000);

    EXPECT_EQ(cache1->m_cache[0]->m_usedRelations.size(), (uint32_t)10);
    EXPECT_EQ(cache2->m_cache[0]->m_usedRelations.size(), (uint32_t)10);
    EXPECT_EQ(cache3->m_cache[0]->m_usedRelations.size(), (uint32_t)10);

    /** release all relations for oid = 1 in all caches */
    for (uint32_t i = 0; i < 10; ++i) {
      cache1->releaseRelation(cache1->m_cache[0]->m_usedRelations.front());
      cache2->releaseRelation(cache2->m_cache[0]->m_usedRelations.front());
      cache3->releaseRelation(cache3->m_cache[0]->m_usedRelations.front());
    }

    EXPECT_EQ(cache1->m_cache[0]->m_usedRelations.size(), (uint32_t)0);
    EXPECT_EQ(cache2->m_cache[0]->m_usedRelations.size(), (uint32_t)0);
    EXPECT_EQ(cache3->m_cache[0]->m_usedRelations.size(), (uint32_t)0);

    EXPECT_EQ(cache1->m_cache.size(), (uint32_t)5000);
    EXPECT_EQ(cache2->m_cache.size(), (uint32_t)5000);
    EXPECT_EQ(cache3->m_cache.size(), (uint32_t)5000);

    /** now we should be able to remove this relation */
    relationCacheManager.freeRelation(0);

    EXPECT_EQ(cache1->m_cache.size(), (uint32_t)4999);
    EXPECT_EQ(cache2->m_cache.size(), (uint32_t)4999);
    EXPECT_EQ(cache3->m_cache.size(), (uint32_t)4999);

    EXPECT_EQ(cache1->m_cache.count(0), (uint32_t)0);
    EXPECT_EQ(cache2->m_cache.count(0), (uint32_t)0);
    EXPECT_EQ(cache3->m_cache.count(0), (uint32_t)0);

    /** free the reset of caches entries */
    for (uint32_t oid = 1; oid < 5000; ++oid) {
      for (uint32_t i = 0; i < 10; ++i) {
        cache1->releaseRelation(cache1->m_cache[oid]->m_usedRelations.front());
        cache2->releaseRelation(cache2->m_cache[oid]->m_usedRelations.front());
        cache3->releaseRelation(cache3->m_cache[oid]->m_usedRelations.front());
      }
      relationCacheManager.freeRelation(oid);
    }

    EXPECT_EQ(cache1->m_cache.size(), (uint32_t)0);
    EXPECT_EQ(cache2->m_cache.size(), (uint32_t)0);
    EXPECT_EQ(cache3->m_cache.size(), (uint32_t)0);
  }

  void add_X_used_relation_by_Y_threads_Z_rel_per_oid(
      const uint32_t numberOfThreads, const uint32_t numberOfRelationsPerThread,
      const uint32_t numberOfRelPerOid) {
    dstore_relation_cache_instances =
        RelationCacheManager::DEFAULT_MAX_RELATION_CACHES;
    dstore_relation_cache_size_per_instance =
        numberOfRelationsPerThread * numberOfRelPerOid;

    std::vector<std::thread> threads;

    for (uint i = 0; i < numberOfThreads; ++i)
      threads.emplace_back(
          addRelations, numberOfRelPerOid, i, i * numberOfRelationsPerThread,
          i * numberOfRelationsPerThread + numberOfRelationsPerThread);

    for (uint i = 0; i < numberOfThreads; ++i) threads[i].join();

    for (uint i = 0; i < numberOfThreads; ++i) {
      RelationCache *cache = relationCacheManager.getCache(i);
      EXPECT_EQ(cache->m_unusedRelations.size(), (uint32_t)0);
      for (Oid oid = i * numberOfRelationsPerThread;
           oid < i * numberOfRelationsPerThread + numberOfRelationsPerThread;
           ++oid) {
        EXPECT_EQ(cache->m_cache.count(oid), (uint32_t)1);
        /** check that any other thread doesnt have this relation */
        for (uint j = 0; j < i; ++j)
          EXPECT_EQ(relationCacheManager.getCache(j)->m_cache.count(oid),
                    (uint32_t)0);
        for (uint j = i + 1; j < numberOfThreads; ++j)
          EXPECT_EQ(relationCacheManager.getCache(j)->m_cache.count(oid),
                    (uint32_t)0);
        RelationCacheElement *cacheElement = cache->m_cache[oid].get();
        EXPECT_EQ(cacheElement->m_freeRelations.size(), (uint32_t)0);
        EXPECT_EQ(cacheElement->m_usedRelations.size(),
                  (uint32_t)numberOfRelPerOid);
        EXPECT_EQ(cacheElement->m_usedRelations.front()->m_tableId, oid);
      }
    }

    for (uint i = 0; i < numberOfThreads; ++i) {
      RelationCache *cache = relationCacheManager.getCache(i);
      EXPECT_EQ(cache->m_unusedRelations.size(), (uint32_t)0);
      uint32_t unusedRelationsPerCache [[maybe_unused]] = 0;
      for (Oid oid = i * numberOfRelationsPerThread;
           oid < i * numberOfRelationsPerThread + numberOfRelationsPerThread;
           ++oid) {
        RelationCacheElement *elem = cache->m_cache[oid].get();
        ASSERT_TRUE(elem->m_usedRelations.size() == numberOfRelPerOid);
        for (uint32_t j = 0; j < numberOfRelPerOid; ++j) {
          cache->releaseRelation(elem->m_usedRelations.front());

          assert(cache->m_unusedRelations.size() ==
                 (uint32_t)++unusedRelationsPerCache);
          EXPECT_EQ(elem->m_freeRelations.size(), (uint32_t)j + 1);
          EXPECT_EQ(elem->m_usedRelations.size(),
                    (uint32_t)numberOfRelPerOid - j - 1);
        }
      }
    }

    relationCacheManager.destroy();
  }
};

TEST_F(ut_cde_global_rel_cache, cde_ddl_insert_relation) {
  cde_ddl_insert_relation_inner();
}

TEST_F(ut_cde_global_rel_cache, cde_ddl_insert_relation_max_1_cache) {
  cde_ddl_insert_relation_max_1_cache_inner();
}

TEST_F(ut_cde_global_rel_cache,
       cde_ddl_insert_relation_max_1_cache_inner_the_same_oid) {
  cde_ddl_insert_relation_max_1_cache_inner_the_same_oid_inner();
}

TEST_F(ut_cde_global_rel_cache,
       add_1000_used_relation_by_3_threads_1_rel_per_oid) {
  add_X_used_relation_by_Y_threads_Z_rel_per_oid(3, 1000, 1);
}

TEST_F(ut_cde_global_rel_cache,
       add_50000_used_relation_by_3_threads_10_rel_per_oid) {
  add_X_used_relation_by_Y_threads_Z_rel_per_oid(3, 1000, 10);
}

TEST_F(
    ut_cde_global_rel_cache,
    three_threads_add_to_the_same_cache_get_release_at_random_1_rel_per_oid) {
  three_threads_add_to_the_same_cache_get_release_at_random_1_rel_per_oid_inner();
}

TEST_F(
    ut_cde_global_rel_cache,
    nine_threads_add_to_different_caches_get_release_at_random_1_rel_per_oid) {
  nine_threads_add_to_different_caches_get_release_at_random_1_rel_per_oid_inner();
}

TEST_F(ut_cde_global_rel_cache, remove_rel_from_all_caches) {
  remove_rel_from_all_caches_inner();
}

TEST_F(ut_cde_global_rel_cache, cde_ddl_free_unused_relations_if_nessecary) {
  cde_ddl_free_unused_relations_if_nessecary_inner();
}

} /* namespace CDE */

GTEST_API_ int main(int argc, char **argv) {
  printf("Running main() from %s\n", __FILE__);

  MY_INIT("dstore_global_rel_cache-t");

  testing::InitGoogleTest(&argc, argv);
  int exit_code = RUN_ALL_TESTS();
  CDE::CDETEST::Destroy();

  return exit_code;
}
