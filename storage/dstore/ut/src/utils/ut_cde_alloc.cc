/*
   Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

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

#include <gtest/gtest.h>

#include "storage/dstore/common/cde_alloc.h"

#include "securec.h"

using namespace CDE;

namespace cde_alloc_unittest {

/** The fake PFS key for this unit test. */
static auto pfs_key = 10000;

/** Check if the provided pointer is properly aligned by max_align_t. */
inline bool is_aligned_by_max(void *ptr) {
  return reinterpret_cast<std::uintptr_t>(ptr) % alignof(max_align_t) == 0;
}

/** A simple class for pod type, which is not constructible. */
struct Pod_type {
  Pod_type(int32_t a, int32_t b) : m_a(a), m_b(b) {}
  int32_t m_a;
  int32_t m_b;
};

struct Sum {
  Sum(int32_t a, int32_t b) : m_result(a + b) {}
  int64_t m_result;
};

/** A simple class for non pod type, which is not constructible. */
struct Non_pod_type {
 public:
  Non_pod_type(int32_t a, int32_t b, std::string s)
      : m_a(a), m_b(b), m_s(s), m_sum(a, b) {
    ++s_count;
  }
  Non_pod_type(const Non_pod_type &other)
      : m_a(other.m_a), m_b(other.m_b), m_s(other.m_s), m_sum(other.m_sum) {
    ++s_count;
  }
  ~Non_pod_type() { --s_count; }

  int32_t m_a;
  int32_t m_b;
  std::string m_s;
  Sum m_sum;
  static size_t get_count() { return s_count; }

 private:
  static size_t s_count;
};
size_t Non_pod_type::s_count{0};

/** The class for pod type, which is constructible. */
struct Default_constructible_pod {
  Default_constructible_pod() : m_a(0), m_b(1) {}
  int32_t m_a;
  int32_t m_b;
};

/** The class for non pod type, which is constructible. */
struct Default_constructible_non_pod {
  Default_constructible_non_pod() : m_a(1), m_b(1), m_s("non-pod-string") {}
  int32_t m_a;
  int32_t m_b;
  std::string m_s;
};

template <typename T, bool With_pfs>
struct Dstore_alloc_param_wrapper {
  using type = T;
  static constexpr bool s_with_pfs = With_pfs;
};

/**
Define all the fundamental types and indicate if PFS should be used or not.
*/
using All_fundamental_types = ::testing::Types<
    /* with PFS */
    Dstore_alloc_param_wrapper<char, true>,
    Dstore_alloc_param_wrapper<unsigned char, true>,
    Dstore_alloc_param_wrapper<wchar_t, true>,
    Dstore_alloc_param_wrapper<int8_t, true>,
    Dstore_alloc_param_wrapper<uint8_t, true>,
    Dstore_alloc_param_wrapper<int16_t, true>,
    Dstore_alloc_param_wrapper<uint16_t, true>,
    Dstore_alloc_param_wrapper<int32_t, true>,
    Dstore_alloc_param_wrapper<uint32_t, true>,
    Dstore_alloc_param_wrapper<int64_t, true>,
    Dstore_alloc_param_wrapper<uint64_t, true>,
    Dstore_alloc_param_wrapper<float, true>,
    Dstore_alloc_param_wrapper<double, true>,
    Dstore_alloc_param_wrapper<long double, true>,
    /* no PFS */
    Dstore_alloc_param_wrapper<char, false>,
    Dstore_alloc_param_wrapper<unsigned char, false>,
    Dstore_alloc_param_wrapper<wchar_t, false>,
    Dstore_alloc_param_wrapper<int8_t, false>,
    Dstore_alloc_param_wrapper<uint8_t, false>,
    Dstore_alloc_param_wrapper<int16_t, false>,
    Dstore_alloc_param_wrapper<uint16_t, false>,
    Dstore_alloc_param_wrapper<int32_t, false>,
    Dstore_alloc_param_wrapper<uint32_t, false>,
    Dstore_alloc_param_wrapper<int64_t, false>,
    Dstore_alloc_param_wrapper<uint64_t, false>,
    Dstore_alloc_param_wrapper<float, false>,
    Dstore_alloc_param_wrapper<double, false>,
    Dstore_alloc_param_wrapper<long double, false>>;

/** Define all pod types and indicate if PFS should be used or not. */
using All_pod_types = ::testing::Types<
    /* with PFS */
    Dstore_alloc_param_wrapper<Pod_type, true>,
    /* no PFS */
    Dstore_alloc_param_wrapper<Pod_type, false>>;

/**
Define all constructible pod types and indicate if PFS should be used or not.
*/
using All_default_constructible_pod_types = ::testing::Types<
    /* with PFS */
    Dstore_alloc_param_wrapper<Default_constructible_pod, true>,
    /* no PFS */
    Dstore_alloc_param_wrapper<Default_constructible_pod, false>>;

/** Define all non pod types and indicate if PFS should be used or not. */
using All_non_pod_types = ::testing::Types<
    /* with PFS */
    Dstore_alloc_param_wrapper<Non_pod_type, true>,
    /* no PFS */
    Dstore_alloc_param_wrapper<Non_pod_type, false>>;

/**
Define all constructible non pod types and indicate if PFS should be used or
not.
*/
using All_default_constructible_non_pod_types = ::testing::Types<
    /* with PFS */
    Dstore_alloc_param_wrapper<Default_constructible_non_pod, true>,
    /* no PFS */
    Dstore_alloc_param_wrapper<Default_constructible_non_pod, false>>;

/* MemHeap */
TEST(Dstore_alloc_memheap, memheap_important_keys_verify) {
  size_t count = 10;
  Memory::MemHeap mem(PSIKeyStd, 64);
  char *ptr = static_cast<char *>(mem.Alloc(1024));
  memset_s(ptr, 1024, 0, 1024);

  int *arr = mem.ArrayAlloc<int>(count, 100);
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(arr[i], static_cast<int>(100));
  }

  Non_pod_type *obj_arr;
  obj_arr = mem.ArrayAlloc<Non_pod_type>(count, 1, 2, "non-pod-type");
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(obj_arr[i].m_a, static_cast<int>(1));
    EXPECT_EQ(obj_arr[i].m_b, static_cast<int>(2));
    EXPECT_EQ(obj_arr[i].m_s, "non-pod-type");
  }

  for (size_t i = 0; i < count; ++i) {
    obj_arr[i].~Non_pod_type();
  }

  mem.ClearForReuse();

  Pod_type *obj = new (&mem) Pod_type(1, 2);
  EXPECT_EQ(obj->m_a, static_cast<int>(1));
  EXPECT_EQ(obj->m_b, static_cast<int>(2));

  {
    Memory::UniquePtrDestroyOnly<Pod_type> obj1 =
        Memory::MakeUniqueDestroyOnly<Pod_type>(&mem, 10, 20);
    EXPECT_EQ((*obj1).m_a, static_cast<int>(10));
    EXPECT_EQ((*obj1).m_b, static_cast<int>(20));

    Memory::UniquePtrDestroyOnly<Non_pod_type> obj2 =
        Memory::MakeUniqueDestroyOnly<Non_pod_type>(&mem, 10, 20,
                                                    "non-pod-type");
    EXPECT_EQ((*obj2).m_a, static_cast<int>(10));
    EXPECT_EQ((*obj2).m_b, static_cast<int>(20));
    EXPECT_EQ((*obj2).m_s, "non-pod-type");

    Memory::UniquePtrDestroyOnly<int> obj3 =
        Memory::MakeUniqueDestroyOnly<int>(&mem, 123);
    EXPECT_EQ((*obj3), static_cast<int>(123));
  }

  mem.ClearForReuse();
}

/* Malloc/Free for fundamental types */
template <typename T>
class Dstore_alloc_malloc_free_fundamental_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_malloc_free_fundamental_types);
TYPED_TEST_P(Dstore_alloc_malloc_free_fundamental_types, fundamental_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  type *ptr =
      with_pfs
          ? static_cast<type *>(Memory::MallocWithKey(pfs_key, sizeof(type)))
          : static_cast<type *>(Memory::Malloc(sizeof(type)));
  EXPECT_TRUE(is_aligned_by_max(ptr));
  Memory::Free(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_malloc_free_fundamental_types,
                            fundamental_types);
INSTANTIATE_TYPED_TEST_SUITE_P(FundamentalTypes,
                               Dstore_alloc_malloc_free_fundamental_types,
                               All_fundamental_types);

/* Zalloc/Free for fundamental types */
template <typename T>
class Dstore_alloc_zalloc_free_fundamental_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_zalloc_free_fundamental_types);
TYPED_TEST_P(Dstore_alloc_zalloc_free_fundamental_types, fundamental_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  type *ptr =
      with_pfs
          ? static_cast<type *>(Memory::ZallocWithKey(pfs_key, sizeof(type)))
          : static_cast<type *>(Memory::Zalloc(sizeof(type)));
  EXPECT_TRUE(is_aligned_by_max(ptr));
  Memory::Free(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_zalloc_free_fundamental_types,
                            fundamental_types);
INSTANTIATE_TYPED_TEST_SUITE_P(FundamentalTypes,
                               Dstore_alloc_zalloc_free_fundamental_types,
                               All_fundamental_types);

/* Realloc/Free for fundamental types */
template <typename T>
class Dstore_alloc_realloc_free_fundamental_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_realloc_free_fundamental_types);
TYPED_TEST_P(Dstore_alloc_realloc_free_fundamental_types, fundamental_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  {
    /* Allocating through realloc and release through free. */
    auto ptr =
        with_pfs ? static_cast<type *>(
                       Memory::ReallocWithKey(pfs_key, nullptr, sizeof(type)))
                 : static_cast<type *>(Memory::Realloc(nullptr, sizeof(type)));
    EXPECT_TRUE(is_aligned_by_max(ptr));
    Memory::Free(ptr);
  }

  {
    /* Allocating through realloc and release through realloc. */
    auto ptr =
        with_pfs ? static_cast<type *>(
                       Memory::ReallocWithKey(pfs_key, nullptr, sizeof(type)))
                 : static_cast<type *>(Memory::Realloc(nullptr, sizeof(type)));
    EXPECT_TRUE(is_aligned_by_max(ptr));
    Memory::Realloc(ptr, 0);
  }

  {
    /* Allocating through malloc and extending the memory through realloc. */
    auto ptr =
        with_pfs
            ? static_cast<type *>(Memory::MallocWithKey(pfs_key, sizeof(type)))
            : static_cast<type *>(Memory::Malloc(sizeof(type)));
    EXPECT_TRUE(is_aligned_by_max(ptr));

    /* Write something so that the behavior of realloc can be verified. */
    *ptr = 0xA;

    ptr = with_pfs
              ? static_cast<type *>(
                    Memory::ReallocWithKey(pfs_key, ptr, 10 * sizeof(type)))
              : static_cast<type *>(Memory::Realloc(ptr, 10 * sizeof(type)));
    EXPECT_TRUE(is_aligned_by_max(ptr));

    /* Verify the content is kept unchanged. */
    EXPECT_EQ(ptr[0], static_cast<type>(0xA));

    /* Write more stuff to the memory. */
    for (size_t i = 1; i < 10; ++i) ptr[i] = 0xB;

    ptr = with_pfs
              ? static_cast<type *>(
                    Memory::ReallocWithKey(pfs_key, ptr, 100 * sizeof(type)))
              : static_cast<type *>(Memory::Realloc(ptr, 100 * sizeof(type)));
    EXPECT_TRUE(is_aligned_by_max(ptr));

    /* Verify the content is kept unchanged. */
    EXPECT_EQ(ptr[0], static_cast<type>(0xA));
    for (size_t i = 1; i < 10; ++i) EXPECT_EQ(ptr[i], static_cast<type>(0xB));

    /* Write some more stuff to the memory. */
    for (size_t i = 10; i < 100; ++i) ptr[i] = 0xC;

    ptr = with_pfs
              ? static_cast<type *>(
                    Memory::ReallocWithKey(pfs_key, ptr, 1000 * sizeof(type)))
              : static_cast<type *>(Memory::Realloc(ptr, 1000 * sizeof(type)));
    EXPECT_TRUE(is_aligned_by_max(ptr));

    /* Verify the content is kept unchanged. */
    EXPECT_EQ(ptr[0], static_cast<type>(0xA));
    for (size_t i = 1; i < 10; ++i) EXPECT_EQ(ptr[i], static_cast<type>(0xB));
    for (size_t i = 10; i < 100; ++i) EXPECT_EQ(ptr[i], static_cast<type>(0xC));

    Memory::Free(ptr);
  }

  {
    /* Allocating through malloc and shrinking the memory through realloc. */
    auto ptr = with_pfs
                   ? static_cast<type *>(
                         Memory::MallocWithKey(pfs_key, 10 * sizeof(type)))
                   : static_cast<type *>(Memory::Malloc(10 * sizeof(type)));
    EXPECT_TRUE(is_aligned_by_max(ptr));

    /* Write something so that the behavior of realloc can be verified. */
    for (size_t i = 0; i < 10; ++i) ptr[i] = 0xA;

    ptr = with_pfs
              ? static_cast<type *>(
                    Memory::ReallocWithKey(pfs_key, ptr, 5 * sizeof(type)))
              : static_cast<type *>(Memory::Realloc(ptr, 5 * sizeof(type)));
    EXPECT_TRUE(is_aligned_by_max(ptr));

    /* Verify the left content is kept unchanged. */
    for (size_t i = 0; i < 5; i++) EXPECT_EQ(ptr[i], static_cast<type>(0xA));

    Memory::Free(ptr);
  }
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_realloc_free_fundamental_types,
                            fundamental_types);
INSTANTIATE_TYPED_TEST_SUITE_P(FundamentalTypes,
                               Dstore_alloc_realloc_free_fundamental_types,
                               All_fundamental_types);

/* Aligned malloc/free for fundamental types */
template <typename T>
class Dstore_alloc_aligned_malloc_free_fundamental_types
    : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_aligned_malloc_free_fundamental_types);
TYPED_TEST_P(Dstore_alloc_aligned_malloc_free_fundamental_types,
             fundamental_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;

  for (size_t alignment = 2; alignment <= 1024 * 1024; alignment *= 2) {
    size_t size = sizeof(type) * alignment;
    /* Zero filled. */
    Memory::AlignedMem mem =
        with_pfs ? Memory::AlignedMallocWithKey(pfs_key, size, alignment, true)
                 : Memory::AlignedMalloc(size, alignment, true);
    EXPECT_TRUE(mem.IsValid());
    EXPECT_TRUE(reinterpret_cast<std::uintptr_t>(mem.Get()) % alignment == 0);
    EXPECT_EQ(*reinterpret_cast<uint8_t *>(mem.Get()), 0);
    EXPECT_EQ(mem.GetSize(), size);
    memset_s(mem.Get(), mem.GetSize(), 1, size);
    EXPECT_EQ(*reinterpret_cast<uint8_t *>(mem.Get()), 1);
    EXPECT_EQ(*(reinterpret_cast<uint8_t *>(mem.Get()) + 1), 1);
    Memory::AlignedFree(mem);
    EXPECT_TRUE(!mem.IsValid());

    /* No zero filled. */
    mem = with_pfs
              ? Memory::AlignedMallocWithKey(pfs_key, size, alignment, false)
              : Memory::AlignedMalloc(size, alignment, false);
    EXPECT_TRUE(mem.IsValid());
    EXPECT_TRUE(reinterpret_cast<std::uintptr_t>(mem.Get()) % alignment == 0);
    EXPECT_EQ(mem.GetSize(), size);
    memset_s(mem.Get(), mem.GetSize(), 1, size);
    EXPECT_EQ(*reinterpret_cast<uint8_t *>(mem.Get()), 1);
    EXPECT_EQ(*(reinterpret_cast<uint8_t *>(mem.Get()) + 1), 1);
    Memory::AlignedFree(mem);
    EXPECT_TRUE(!mem.IsValid());
  }
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_aligned_malloc_free_fundamental_types,
                            fundamental_types);
INSTANTIATE_TYPED_TEST_SUITE_P(
    FundamentalTypes, Dstore_alloc_aligned_malloc_free_fundamental_types,
    All_fundamental_types);

/* Aligned malloc/free for pod types */
template <typename T>
class Dstore_alloc_aligned_malloc_free_pod_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_aligned_malloc_free_pod_types);
TYPED_TEST_P(Dstore_alloc_aligned_malloc_free_pod_types, pod_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  size_t size = sizeof(type) * 1000;
  size_t alignment = 8192;

  Memory::AlignedMem mem =
      with_pfs ? Memory::AlignedMallocWithKey(pfs_key, size, alignment, true)
               : Memory::AlignedMalloc(size, alignment, true);
  Memory::AlignedMem *memory = &mem;
  EXPECT_TRUE(memory->IsValid());
  EXPECT_TRUE(reinterpret_cast<std::uintptr_t>(memory->Get()) % alignment == 0);
  EXPECT_EQ(*reinterpret_cast<uint8_t *>(memory->Get()), 0);
  EXPECT_EQ(mem.GetSize(), size);
  type *ptr = new (memory->Get()) type(10, 20);
  EXPECT_EQ(ptr->m_a, 10);
  EXPECT_EQ(ptr->m_b, 20);
  Memory::AlignedFree(*memory);
  memory = nullptr;
  EXPECT_TRUE(!mem.IsValid());
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_aligned_malloc_free_pod_types,
                            pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(PodTypes,
                               Dstore_alloc_aligned_malloc_free_pod_types,
                               All_pod_types);

/* New/Delete for fundamental types */
template <typename T>
class Dstore_alloc_new_delete_fundamental_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_fundamental_types);
TYPED_TEST_P(Dstore_alloc_new_delete_fundamental_types, fundamental_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  type *ptr =
      with_pfs ? Memory::NewWithKey<type>(pfs_key, 1) : Memory::New<type>(1);
  EXPECT_TRUE(is_aligned_by_max(ptr));
  EXPECT_EQ(*ptr, static_cast<type>(1));
  Memory::Delete(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_fundamental_types,
                            fundamental_types);
INSTANTIATE_TYPED_TEST_SUITE_P(FundamentalTypes,
                               Dstore_alloc_new_delete_fundamental_types,
                               All_fundamental_types);

/* New/Delete for POD types */
template <typename T>
class Dstore_alloc_new_delete_pod_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_pod_types);
TYPED_TEST_P(Dstore_alloc_new_delete_pod_types, pod_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  type *ptr = with_pfs ? Memory::NewWithKey<type>(pfs_key, 1, 2)
                       : Memory::New<type>(1, 2);
  EXPECT_TRUE(is_aligned_by_max(ptr));
  EXPECT_EQ(ptr->m_a, 1);
  EXPECT_EQ(ptr->m_b, 2);
  Memory::Delete(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_pod_types, pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(PodTypes, Dstore_alloc_new_delete_pod_types,
                               All_pod_types);

/* New/Delete for non-POD types */
template <typename T>
class Dstore_alloc_new_delete_non_pod_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_non_pod_types);
TYPED_TEST_P(Dstore_alloc_new_delete_non_pod_types, non_pod_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  type *ptr = with_pfs ? Memory::NewWithKey<type>(pfs_key, 1, 2, "string")
                       : Memory::New<type>(1, 2, "string");
  EXPECT_TRUE(is_aligned_by_max(ptr));
  EXPECT_EQ(ptr->m_a, 1);
  EXPECT_EQ(ptr->m_b, 2);
  EXPECT_EQ(ptr->m_sum.m_result, 3);
  EXPECT_EQ(ptr->m_s, std::string("string"));
  EXPECT_EQ(type::get_count(), static_cast<size_t>(1));
  Memory::Delete(ptr);
  EXPECT_EQ(type::get_count(), static_cast<size_t>(0));
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_non_pod_types,
                            non_pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(NonPodTypes,
                               Dstore_alloc_new_delete_non_pod_types,
                               All_non_pod_types);

/* NewArray/DeleteArray for fundamental types */
template <typename T>
class Dstore_alloc_new_delete_arr_fundamental_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_arr_fundamental_types);
TYPED_TEST_P(Dstore_alloc_new_delete_arr_fundamental_types, fundamental_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  constexpr size_t count = 5;
  type *ptr = with_pfs ? Memory::NewArrayWithKey<type>(pfs_key, count, 1)
                       : Memory::NewArray<type>(count, 1);

  EXPECT_TRUE(is_aligned_by_max(ptr));
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(ptr[i], static_cast<type>(1));
  }

  Memory::DeleteArray(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_arr_fundamental_types,
                            fundamental_types);
INSTANTIATE_TYPED_TEST_SUITE_P(FundamentalTypes,
                               Dstore_alloc_new_delete_arr_fundamental_types,
                               All_fundamental_types);

/* NewArray/DeleteArray for POD types */
template <typename T>
class Dstore_alloc_new_delete_arr_pod_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_arr_pod_types);
TYPED_TEST_P(Dstore_alloc_new_delete_arr_pod_types, pod_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  constexpr size_t count = 5;
  type *ptr = with_pfs ? Memory::NewArrayWithKey<type>(pfs_key, count, 1, 3)
                       : Memory::NewArray<type>(count, 1, 3);

  EXPECT_TRUE(is_aligned_by_max(ptr));
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(ptr[i].m_a, 1);
    EXPECT_EQ(ptr[i].m_b, 3);
  }

  Memory::DeleteArray(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_arr_pod_types, pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(PodTypes, Dstore_alloc_new_delete_arr_pod_types,
                               All_pod_types);

/* NewArray/DeleteArray for non-POD types */
template <typename T>
class Dstore_alloc_new_delete_arr_non_pod_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_arr_non_pod_types);
TYPED_TEST_P(Dstore_alloc_new_delete_arr_non_pod_types, non_pod_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  constexpr size_t count = 10;
  type *ptr =
      with_pfs ? Memory::NewArrayWithKey<type>(pfs_key, count, 10, 20, "string")
               : Memory::NewArray<type>(count, 10, 20, "string");

  EXPECT_TRUE(is_aligned_by_max(ptr));
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(ptr[i].m_a, 10);
    EXPECT_EQ(ptr[i].m_b, 20);
    EXPECT_EQ(ptr[i].m_s, std::string("string"));
    EXPECT_EQ(ptr[i].m_sum.m_result, 30);
  }

  Memory::DeleteArray(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_arr_non_pod_types,
                            non_pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(NonPodTypes,
                               Dstore_alloc_new_delete_arr_non_pod_types,
                               All_non_pod_types);

/* NewArray/DeleteArray for default constructible fundamental types */
template <typename T>
class Dstore_alloc_new_delete_arr_constructible_fundamental_types
    : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_arr_constructible_fundamental_types);
TYPED_TEST_P(Dstore_alloc_new_delete_arr_constructible_fundamental_types,
             fundamental_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  constexpr size_t count = 10;
  type *ptr = with_pfs ? Memory::NewArrayWithKey<type>(pfs_key, count)
                       : Memory::NewArray<type>(count);

  EXPECT_TRUE(is_aligned_by_max(ptr));
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(ptr[i], type{});
  }

  Memory::DeleteArray(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(
    Dstore_alloc_new_delete_arr_constructible_fundamental_types,
    fundamental_types);
INSTANTIATE_TYPED_TEST_SUITE_P(
    FundamentalTypes,
    Dstore_alloc_new_delete_arr_constructible_fundamental_types,
    All_fundamental_types);

/* NewArray/DeleteArray for default constructible POD types */
template <typename T>
class Dstore_alloc_new_delete_arr_default_constructible_pod_types
    : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_new_delete_arr_default_constructible_pod_types);
TYPED_TEST_P(Dstore_alloc_new_delete_arr_default_constructible_pod_types,
             pod_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  constexpr size_t count = 10;
  type *ptr = with_pfs ? Memory::NewArrayWithKey<type>(pfs_key, count)
                       : Memory::NewArray<type>(count);

  EXPECT_TRUE(is_aligned_by_max(ptr));
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(ptr[i].m_a, 0);
    EXPECT_EQ(ptr[i].m_b, 1);
  }

  Memory::DeleteArray(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(
    Dstore_alloc_new_delete_arr_default_constructible_pod_types, pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(
    PodTypes, Dstore_alloc_new_delete_arr_default_constructible_pod_types,
    All_default_constructible_pod_types);

/* NewArray/DeleteArray for default constructible non-POD types */
template <typename T>
class Dstore_alloc_new_delete_arr_default_constructible_non_pod_types
    : public ::testing::Test {};
TYPED_TEST_SUITE_P(
    Dstore_alloc_new_delete_arr_default_constructible_non_pod_types);
TYPED_TEST_P(Dstore_alloc_new_delete_arr_default_constructible_non_pod_types,
             non_POD_types) {
  using type = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  constexpr size_t count = 10;
  type *ptr = with_pfs ? Memory::NewArrayWithKey<type>(pfs_key, count)
                       : Memory::NewArray<type>(count);

  EXPECT_TRUE(is_aligned_by_max(ptr));
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(ptr[i].m_a, 1);
    EXPECT_EQ(ptr[i].m_b, 1);
    EXPECT_TRUE(ptr[i].m_s == std::string("non-pod-string"));
  }

  Memory::DeleteArray(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(
    Dstore_alloc_new_delete_arr_default_constructible_non_pod_types,
    non_POD_types);
INSTANTIATE_TYPED_TEST_SUITE_P(
    FundamentalTypes,
    Dstore_alloc_new_delete_arr_default_constructible_non_pod_types,
    All_default_constructible_non_pod_types);

/* Test the normal deleter for std::unique_ptr */
TEST(Dstore_alloc_new_delete, unique_ptr_with_deleter) {
  struct Int_deleter {
    void operator()(int *p) {
      std::cout << "Hello from custom deleter!\n";
      Memory::Delete(p);
    }
  };
  std::unique_ptr<int, Int_deleter> ptr(Memory::New<int>(1), Int_deleter{});
}

/* Test the failure from constructor for New */
TEST(Dstore_alloc_new_delete, fail_during_construct) {
  static int n_constructors = 0;
  static int n_destructors = 0;

  struct Type_throws_exception {
    Type_throws_exception(int a, int b) : m_a(a), m_b(b) {
      n_constructors++;
      throw std::runtime_error("Cannot construct");
    }

    ~Type_throws_exception() { ++n_destructors; }

    int m_a;
    int m_b;
  };

  bool exception_caught = false;
  try {
    auto ptr = Memory::NewWithKey<Type_throws_exception>(pfs_key, 1, 2);
    EXPECT_EQ(ptr, nullptr);
  } catch (std::runtime_error &) {
    exception_caught = true;
  }

  EXPECT_FALSE(exception_caught);
  EXPECT_EQ(n_constructors, 1);
  EXPECT_EQ(n_destructors, 0);
}

/* Test an allocation of size 0. */
TEST(Dstore_alloc_new_delete, zero_size_allocation) {
  auto ptr = Memory::NewWithKey<int>(pfs_key, 0);
  EXPECT_NE(ptr, nullptr);
  Memory::Delete(ptr);

  Memory::AlignedMem mem = Memory::AlignedMalloc(0, 8192, true);
  EXPECT_TRUE(mem.IsValid());
  EXPECT_TRUE(mem.GetSize() == 0);
  Memory::AlignedFree(mem);
  EXPECT_FALSE(mem.IsValid());
}

/* Test the array deleter for std::unique_ptr */
TEST(Dstore_alloc_new_delete_arr, unique_ptr_with_array_deleter) {
  struct Int_arr_deleter {
    void operator()(int *p) {
      std::cout << "Hello from custom array deleter!\n";
      Memory::DeleteArray(p);
    }
  };
  std::unique_ptr<int, Int_arr_deleter> ptr(Memory::NewArray<int>(10, 3),
                                            Int_arr_deleter{});
}

/* Test the NewArray and DeleteArray */
TEST(Dstore_alloc_new_delete_arr, n_constructible_pods) {
  constexpr size_t count = 5;
  auto ptr = Memory::NewArrayWithKey<Default_constructible_pod>(pfs_key, count);

  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(ptr[i].m_a, 0);
    EXPECT_EQ(ptr[i].m_b, 1);
  }

  Memory::DeleteArray(ptr);
}

/* Test the failed NewArray */
TEST(Dstore_alloc_new_delete_arr, fail_during_construct) {
  static int n_constructors = 0;
  static int n_destructors = 0;
  constexpr size_t count = 5;

  struct Type_throws_exception {
    Type_throws_exception(int a, int b) : m_a(a), m_b(b) {
      n_constructors++;
      if (n_constructors % (count - 1) == 0) {
        throw std::runtime_error("Cannot construct");
      }
    }

    ~Type_throws_exception() { ++n_destructors; }

    int m_a;
    int m_b;
  };

  bool exception_caught = false;
  try {
    auto ptr =
        Memory::NewArrayWithKey<Type_throws_exception>(pfs_key, count, 10, 20);
    EXPECT_EQ(ptr, nullptr);
  } catch (std::runtime_error &) {
    exception_caught = true;
  }

  EXPECT_FALSE(exception_caught);
  EXPECT_EQ(n_constructors, 4);
  EXPECT_EQ(n_destructors, 3);
}

/* Test the allocation of size zero for NewArray. */
TEST(Dstore_alloc_new_delete_arr, zero_size_allocation) {
  auto ptr = Memory::NewArrayWithKey<int>(pfs_key, 5);
  EXPECT_NE(ptr, nullptr);
  Memory::DeleteArray(ptr);
}

/* Prepare for allocator test. */
template <bool With_pfs, typename T>
struct Select_allocator_variant {
  using type = Memory::Detail::AllocatorBaseWithKey<T>;
};
template <typename T>
struct Select_allocator_variant<false, T> {
  using type = Memory::Detail::AllocatorBase<T>;
};
template <bool With_pfs, typename T>
using Select_allocator_variant_t =
    typename Select_allocator_variant<With_pfs, T>::type;

/* Allocator for fundamental types. */
template <typename T>
class Dstore_alloc_allocator_fundamental_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_allocator_fundamental_types);
TYPED_TEST_P(Dstore_alloc_allocator_fundamental_types, fundamental_types) {
  using T = typename TypeParam::type;
  using allocator_variant =
      Select_allocator_variant_t<TypeParam::s_with_pfs, T>;

  constexpr size_t count = 10;
  Memory::Allocator<T, allocator_variant> allocator1;
  Memory::Allocator<T, allocator_variant> allocator2 = allocator1;
  Memory::Allocator<T, allocator_variant> allocator3(allocator1);
  Memory::Allocator<T, allocator_variant> allocator4(pfs_key);

  EXPECT_TRUE(allocator1 == allocator2);
  EXPECT_TRUE(allocator1 == allocator3);
  EXPECT_FALSE(allocator2 != allocator3);
  EXPECT_FALSE(allocator1 != allocator4);

  auto ptr = allocator1.allocate(count);
  ptr[0] = std::numeric_limits<T>::max();
  EXPECT_EQ(ptr[0], std::numeric_limits<T>::max());
  ptr[count - 1] = std::numeric_limits<T>::min();
  EXPECT_EQ(ptr[count - 1], std::numeric_limits<T>::min());

  allocator1.deallocate(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_allocator_fundamental_types,
                            fundamental_types);
INSTANTIATE_TYPED_TEST_SUITE_P(FundamentalTypes,
                               Dstore_alloc_allocator_fundamental_types,
                               All_fundamental_types);

/* Allocator for pod types. */
template <typename T>
class Dstore_alloc_allocator_pod_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_allocator_pod_types);
TYPED_TEST_P(Dstore_alloc_allocator_pod_types, pod_types) {
  using T = typename TypeParam::type;
  using allocator_variant =
      Select_allocator_variant_t<TypeParam::s_with_pfs, T>;

  constexpr size_t count = 10;
  Memory::Allocator<T, allocator_variant> allocator1;
  Memory::Allocator<T, allocator_variant> allocator2 = allocator1;
  Memory::Allocator<T, allocator_variant> allocator3(allocator1);
  Memory::Allocator<T, allocator_variant> allocator4(pfs_key);

  EXPECT_TRUE(allocator1 == allocator2);
  EXPECT_TRUE(allocator1 == allocator3);
  EXPECT_FALSE(allocator2 != allocator3);
  EXPECT_FALSE(allocator1 != allocator4);

  auto ptr = allocator2.allocate(count);

  allocator2.construct(ptr, 101, 202);
  T *obj = reinterpret_cast<T *>(ptr);
  EXPECT_EQ(obj->m_a, static_cast<int>(101));
  EXPECT_EQ(obj->m_b, static_cast<int>(202));
  allocator2.destroy(obj);

  allocator2.deallocate(ptr);
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_allocator_pod_types, pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(PodTypes, Dstore_alloc_allocator_pod_types,
                               All_pod_types);

/* Test the request size exceeds the max_size() case. */
TEST(Dstore_alloc_allocator, max_size_is_exceeded) {
  struct obj_t {
    char m_a[128];
  };

  Memory::Allocator<obj_t> alloc(pfs_key);

  constexpr Memory::Allocator<obj_t>::size_type count =
      std::numeric_limits<Memory::Allocator<obj_t>::size_type>::max() /
          sizeof(obj_t) +
      1;

  obj_t *ptr = nullptr;
  bool exception = false;
  try {
    ptr = alloc.allocate(count);
  } catch (std::bad_alloc &) {
    exception = true;
  }

  EXPECT_TRUE(exception);
  EXPECT_TRUE(ptr == nullptr);
}

/* std containers for fundamental types. */
template <typename T>
class Dstore_alloc_std_fundamental_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_std_fundamental_types);
TYPED_TEST_P(Dstore_alloc_std_fundamental_types, fundamental_types) {
  using T = typename TypeParam::type;
  using allocator_variant =
      Select_allocator_variant_t<TypeParam::s_with_pfs, T>;

  { /* Basic test for normal definition std::vector with allocator */
    std::vector<T, Memory::Allocator<T, allocator_variant>> vec1;
    vec1.push_back(std::numeric_limits<T>::max());
    vec1.push_back(std::numeric_limits<T>::min());
    vec1.push_back(std::numeric_limits<T>::max());

    EXPECT_TRUE(is_aligned_by_max(&vec1[0]));
    EXPECT_EQ(vec1[0], std::numeric_limits<T>::max());
    EXPECT_EQ(vec1[1], std::numeric_limits<T>::min());
    EXPECT_EQ(vec1[2], std::numeric_limits<T>::max());
  }

  { /* Basic test for Memory::Vector. */
    Memory::Vector<T> vec2;
    vec2.push_back(std::numeric_limits<T>::max());
    vec2.push_back(std::numeric_limits<T>::min());
    vec2.push_back(std::numeric_limits<T>::max());

    EXPECT_TRUE(is_aligned_by_max(&vec2[0]));
    EXPECT_EQ(vec2[0], std::numeric_limits<T>::max());
    EXPECT_EQ(vec2[1], std::numeric_limits<T>::min());
    EXPECT_EQ(vec2[2], std::numeric_limits<T>::max());
  }

  { /* Basic test for different key, specified allocator and bulk insert */
    Memory::Vector<T> vec3(pfs_key);
    for (size_t i = 0; i < 10000; ++i) {
      vec3.push_back(std::numeric_limits<T>::max());
    }

    Memory::Allocator<T> allocator(pfs_key);
    Memory::Vector<T> vec4(allocator);
    for (size_t i = 0; i < 10000; ++i) {
      vec4.push_back(std::numeric_limits<T>::max());
    }

    auto *vec5 = new std::vector<T, Memory::Allocator<T>>(allocator);
    auto *vec6 =
        new std::vector<T, Memory::Allocator<T>>(Memory::Allocator<T>(pfs_key));
    for (size_t i = 0; i < 10000; ++i) {
      vec5->push_back(std::numeric_limits<T>::max());
      EXPECT_EQ(vec5->at(i), std::numeric_limits<T>::max());
      vec6->push_back(std::numeric_limits<T>::max());
      EXPECT_EQ(vec6->at(i), std::numeric_limits<T>::max());
    }
    delete vec5;
    delete vec6;
  }

  { /* Basic test for normal definition std::list with allocator */
    std::list<T, Memory::Allocator<T, allocator_variant>> list1;
    list1.push_back(std::numeric_limits<T>::max());
    list1.push_back(std::numeric_limits<T>::min());
    list1.push_back(std::numeric_limits<T>::max());

    EXPECT_EQ(list1.front(), std::numeric_limits<T>::max());
    list1.pop_front();
    EXPECT_EQ(list1.front(), std::numeric_limits<T>::min());
    list1.pop_front();
    EXPECT_EQ(list1.front(), std::numeric_limits<T>::max());
  }

  { /* Basic test for Memory::List. */
    Memory::List<T> list2;
    list2.push_back(std::numeric_limits<T>::max());
    list2.push_back(std::numeric_limits<T>::min());
    list2.push_back(std::numeric_limits<T>::max());

    EXPECT_EQ(list2.front(), std::numeric_limits<T>::max());
    list2.pop_front();
    EXPECT_EQ(list2.front(), std::numeric_limits<T>::min());
    list2.pop_front();
    EXPECT_EQ(list2.front(), std::numeric_limits<T>::max());
  }

  { /* Basic test for Memory::Map. */
    Memory::Map<T, T> map;
    constexpr size_t count = 100;
    for (size_t i = 0; i < count; ++i) {
      const auto [it, success] = map.insert({i, i * 2});
      EXPECT_EQ(it->first, static_cast<T>(i));
      EXPECT_TRUE(success);
      const auto [it1, success1] = map.insert({i + count, 2 * (i + count)});
      EXPECT_EQ(it1->first, static_cast<T>(i + count));
      EXPECT_TRUE(success1);
    }
  }

  { /* Basic test for Memory::UnorderedMap. */
    Memory::UnorderedMap<T, T> map;
    constexpr size_t count = 100;
    for (size_t i = 0; i < count; ++i) {
      const auto [it, success] = map.insert({i, i * 2});
      EXPECT_EQ(it->first, static_cast<T>(i));
      EXPECT_TRUE(success);
      const auto [it1, success1] = map.insert({(i + count), 2 * (i + count)});
      EXPECT_EQ(it1->first, static_cast<T>(i + count));
      EXPECT_TRUE(success1);
    }
  }

  { /* Basic test for Memory::Set. */
    Memory::Set<T> set;
    constexpr size_t count = 100;
    for (size_t i = 0; i < count; ++i) {
      auto r = set.insert(i);
      EXPECT_TRUE(r.second);
      r = set.insert(i + count);
      EXPECT_TRUE(r.second);
    }
  }

  { /* Basic test for Memory::UnorderedSet. */
    Memory::UnorderedSet<T> set;
    constexpr size_t count = 100;
    for (size_t i = 0; i < count; ++i) {
      auto r = set.insert(i);
      EXPECT_TRUE(r.second);
      r = set.insert(i + count);
      EXPECT_TRUE(r.second);
    }
  }
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_std_fundamental_types,
                            fundamental_types);
INSTANTIATE_TYPED_TEST_SUITE_P(FundamentalTypes,
                               Dstore_alloc_std_fundamental_types,
                               All_fundamental_types);

/* std containers for pod types. */
template <typename T>
class Dstore_alloc_std_pod_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_std_pod_types);
TYPED_TEST_P(Dstore_alloc_std_pod_types, pod_types) {
  using T = typename TypeParam::type;

  Memory::Vector<T> vec;
  Memory::List<T> list;

  for (uint8_t i = 0; i < 5; ++i) {
    vec.push_back({i, i + 1});
    EXPECT_EQ(vec[i].m_a, i);
    EXPECT_EQ(vec[i].m_b, i + 1);
    list.push_back({i * 2, (i + 1) * 2});
    EXPECT_EQ(list.back().m_a, i * 2);
    EXPECT_EQ(list.back().m_b, (i + 1) * 2);
  }
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_std_pod_types, pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(PodTypes, Dstore_alloc_std_pod_types,
                               All_pod_types);

/* std containers for non-pod types. */
template <typename T>
class Dstore_alloc_std_non_pod_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_std_non_pod_types);
TYPED_TEST_P(Dstore_alloc_std_non_pod_types, non_pod_types) {
  using T = typename TypeParam::type;

  Memory::Vector<T> vec;
  Memory::List<T> list;
  for (uint8_t i = 0; i < 5; ++i) {
    vec.push_back({i, i + 1, std::to_string(i)});
    EXPECT_EQ(vec[i].m_a, i);
    EXPECT_EQ(vec[i].m_b, i + 1);
    EXPECT_EQ(vec[i].m_s, std::to_string(i));
    list.push_back({i * 2, (i + 1) * 2, std::to_string(i)});
    EXPECT_EQ(list.back().m_a, i * 2);
    EXPECT_EQ(list.back().m_b, (i + 1) * 2);
    EXPECT_EQ(list.back().m_s, std::to_string(i));
  }
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_std_non_pod_types, non_pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(PodTypes, Dstore_alloc_std_non_pod_types,
                               All_non_pod_types);

/* std containers for default_pod_constructible types. */
template <typename T>
class Dstore_alloc_std_default_pod_constructible_types
    : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_std_default_pod_constructible_types);
TYPED_TEST_P(Dstore_alloc_std_default_pod_constructible_types,
             std_default_pod_constructible_types) {
  using T = typename TypeParam::type;

  Memory::Vector<T> vec;
  Memory::List<T> list;
  Memory::Map<size_t, T> map;

  for (size_t i = 0; i < 10000; ++i) {
    vec.push_back({});
    EXPECT_EQ(vec[i].m_a, 0);
    EXPECT_EQ(vec[i].m_b, 1);
    list.push_back({});
    EXPECT_EQ(list.back().m_a, 0);
    EXPECT_EQ(list.back().m_b, 1);
    map.insert({i, {}});
    EXPECT_EQ(map[i].m_a, 0);
    EXPECT_EQ(map[i].m_b, 1);
  }
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_std_default_pod_constructible_types,
                            std_default_pod_constructible_types);
INSTANTIATE_TYPED_TEST_SUITE_P(NonPodTypes,
                               Dstore_alloc_std_default_pod_constructible_types,
                               All_default_constructible_pod_types);

/* std containers for default_non_pod_constructible types. */
template <typename T>
class Dstore_alloc_std_default_non_pod_constructible_types
    : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_std_default_non_pod_constructible_types);
TYPED_TEST_P(Dstore_alloc_std_default_non_pod_constructible_types,
             std_default_non_pod_constructible_types) {
  using T = typename TypeParam::type;

  Memory::Vector<T> vec;
  Memory::List<T> list;
  Memory::Map<size_t, T> map;
  std::string s("non-pod-string");

  for (size_t i = 0; i < 10000; ++i) {
    vec.push_back({});
    EXPECT_EQ(vec[i].m_a, 1);
    EXPECT_EQ(vec[i].m_b, 1);
    EXPECT_EQ(vec[i].m_s, s);
    list.push_back({});
    EXPECT_EQ(list.back().m_a, 1);
    EXPECT_EQ(list.back().m_b, 1);
    EXPECT_EQ(list.back().m_s, s);
    map.insert({i, {}});
    EXPECT_EQ(map[i].m_a, 1);
    EXPECT_EQ(map[i].m_b, 1);
    EXPECT_EQ(map[i].m_s, s);
  }
}
REGISTER_TYPED_TEST_SUITE_P(
    Dstore_alloc_std_default_non_pod_constructible_types,
    std_default_non_pod_constructible_types);
INSTANTIATE_TYPED_TEST_SUITE_P(
    PodTypes, Dstore_alloc_std_default_non_pod_constructible_types,
    All_default_constructible_non_pod_types);

/* MakeUnique for fundamental types. */
template <typename T>
class Dstore_alloc_make_unique_fundamental_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_make_unique_fundamental_types);
TYPED_TEST_P(Dstore_alloc_make_unique_fundamental_types, fundamental_types) {
  using T = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  auto ptr = with_pfs ? Memory::MakeUniqueWithKey<T>(pfs_key, 1)
                      : Memory::MakeUnique<T>(1);
  EXPECT_TRUE(is_aligned_by_max(ptr.get()));
  EXPECT_EQ(*ptr, static_cast<T>(1));
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_make_unique_fundamental_types,
                            fundamental_types);
INSTANTIATE_TYPED_TEST_SUITE_P(FundamentalTypes,
                               Dstore_alloc_make_unique_fundamental_types,
                               All_fundamental_types);

/* MakeUnique for pod types. */
template <typename T>
class Dstore_alloc_make_unique_pod_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_make_unique_pod_types);
TYPED_TEST_P(Dstore_alloc_make_unique_pod_types, pod_types) {
  using T = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  auto ptr = with_pfs ? Memory::MakeUniqueWithKey<T>(pfs_key, 1, 3)
                      : Memory::MakeUnique<T>(1, 3);
  EXPECT_TRUE(is_aligned_by_max(ptr.get()));
  EXPECT_EQ(ptr->m_a, static_cast<int32_t>(1));
  EXPECT_EQ(ptr->m_b, static_cast<int32_t>(3));
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_make_unique_pod_types, pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(PodTypes, Dstore_alloc_make_unique_pod_types,
                               All_pod_types);

/* MakeUnique for non-pod types. */
template <typename T>
class Dstore_alloc_make_unique_non_pod_types : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_make_unique_non_pod_types);
TYPED_TEST_P(Dstore_alloc_make_unique_non_pod_types, non_pod_types) {
  using T = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  std::string s("string");
  auto ptr = with_pfs ? Memory::MakeUniqueWithKey<T>(pfs_key, 1, 3, s)
                      : Memory::MakeUnique<T>(1, 3, s);
  EXPECT_TRUE(is_aligned_by_max(ptr.get()));
  EXPECT_EQ(ptr->m_a, static_cast<int32_t>(1));
  EXPECT_EQ(ptr->m_b, static_cast<int32_t>(3));
  EXPECT_EQ(ptr->m_s, s);
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_make_unique_non_pod_types,
                            non_pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(NonPodTypes,
                               Dstore_alloc_make_unique_non_pod_types,
                               All_non_pod_types);

/* MakeUnique for default constructible non-pod types. */
template <typename T>
class Dstore_alloc_make_unique_default_constructible_non_pod_types
    : public ::testing::Test {};
TYPED_TEST_SUITE_P(
    Dstore_alloc_make_unique_default_constructible_non_pod_types);
TYPED_TEST_P(Dstore_alloc_make_unique_default_constructible_non_pod_types,
             default_constructible_non_pod_types) {
  using T = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  auto ptr = with_pfs ? Memory::MakeUniqueWithKey<T>(pfs_key)
                      : Memory::MakeUnique<T>();
  EXPECT_TRUE(is_aligned_by_max(ptr.get()));
  EXPECT_EQ(ptr->m_a, static_cast<int32_t>(1));
  EXPECT_EQ(ptr->m_b, static_cast<int32_t>(1));
  EXPECT_EQ(ptr->m_s, std::string("non-pod-string"));
}
REGISTER_TYPED_TEST_SUITE_P(
    Dstore_alloc_make_unique_default_constructible_non_pod_types,
    default_constructible_non_pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(
    NonPodTypes, Dstore_alloc_make_unique_default_constructible_non_pod_types,
    All_default_constructible_non_pod_types);

/* MakeUnique for an array of fundamental types. */
template <typename T>
class Dstore_alloc_make_unique_array_fundamental_types
    : public ::testing::Test {};
TYPED_TEST_SUITE_P(Dstore_alloc_make_unique_array_fundamental_types);
TYPED_TEST_P(Dstore_alloc_make_unique_array_fundamental_types,
             array_fundamental_types) {
  using T = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  auto ptr = with_pfs ? Memory::MakeUniqueWithKey<T[]>(pfs_key, 3, 5)
                      : Memory::MakeUnique<T[]>(3, 5);
  EXPECT_TRUE(is_aligned_by_max(ptr.get()));
  EXPECT_EQ(ptr[0], static_cast<T>(5));
  EXPECT_EQ(ptr[1], static_cast<T>(5));
  EXPECT_EQ(ptr[2], static_cast<T>(5));

  auto ptr1 = with_pfs ? Memory::MakeUniqueWithKey<T[]>(pfs_key, 3)
                       : Memory::MakeUnique<T[]>(3);
  EXPECT_TRUE(is_aligned_by_max(ptr1.get()));
  EXPECT_EQ(ptr1[0], T{});
  EXPECT_EQ(ptr1[1], T{});
  EXPECT_EQ(ptr1[2], T{});
}
REGISTER_TYPED_TEST_SUITE_P(Dstore_alloc_make_unique_array_fundamental_types,
                            array_fundamental_types);
INSTANTIATE_TYPED_TEST_SUITE_P(FundamentalTypes,
                               Dstore_alloc_make_unique_array_fundamental_types,
                               All_fundamental_types);

/* MakeUnique for an array of default constructible non-pod types. */
template <typename T>
class Dstore_alloc_make_unique_array_default_constructible_non_pod_types
    : public ::testing::Test {};
TYPED_TEST_SUITE_P(
    Dstore_alloc_make_unique_array_default_constructible_non_pod_types);
TYPED_TEST_P(Dstore_alloc_make_unique_array_default_constructible_non_pod_types,
             array_default_constructible_non_pod_types) {
  using T = typename TypeParam::type;
  auto with_pfs = TypeParam::s_with_pfs;
  auto ptr = with_pfs ? Memory::MakeUniqueWithKey<T[]>(pfs_key, 3)
                      : Memory::MakeUnique<T[]>(3);
  EXPECT_TRUE(is_aligned_by_max(ptr.get()));
  EXPECT_EQ(ptr[0].m_a, static_cast<int32_t>(1));
  EXPECT_EQ(ptr[0].m_b, static_cast<int32_t>(1));
  EXPECT_EQ(ptr[0].m_s, std::string("non-pod-string"));
  EXPECT_EQ(ptr[1].m_a, static_cast<int32_t>(1));
  EXPECT_EQ(ptr[1].m_b, static_cast<int32_t>(1));
  EXPECT_EQ(ptr[1].m_s, std::string("non-pod-string"));
  EXPECT_EQ(ptr[2].m_a, static_cast<int32_t>(1));
  EXPECT_EQ(ptr[2].m_b, static_cast<int32_t>(1));
  EXPECT_EQ(ptr[2].m_s, std::string("non-pod-string"));
}
REGISTER_TYPED_TEST_SUITE_P(
    Dstore_alloc_make_unique_array_default_constructible_non_pod_types,
    array_default_constructible_non_pod_types);
INSTANTIATE_TYPED_TEST_SUITE_P(
    NonPodTypes,
    Dstore_alloc_make_unique_array_default_constructible_non_pod_types,
    All_default_constructible_non_pod_types);

}  // namespace cde_alloc_unittest
