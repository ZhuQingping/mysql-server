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

#ifndef __CDE_ALLOC_H__
#define __CDE_ALLOC_H__

#undef likely
#undef unlikely

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "include/my_alloc.h"
#include "my_sys.h"
#include "mysql/components/services/bits/psi_bits.h"
#include "mysql/psi/psi_memory.h"
#include "mysql/service_mysql_alloc.h"
#include "securec.h"
#include "sql/malloc_allocator.h"

namespace CDE {

/**
Keys for registering allocations with performance schema.
Pointers to these variables are supplied to PFS code via the pfs_info[]
array and the PFS code initializes them via PSI_MEMORY_CALL(register_memory)().
PSIKeyStd is used for std containers when the relevant PSI key is not provided.

Keep this list alphabetically sorted.
*/
extern PSI_memory_key PSIKeyStd;

/**
Setup the PSI related objects for Memory::*New*() functtions which require the
PFS key. This must be called before the first call to Memory::*New*().
*/
void DstoreAllocBoot();

/**
Namespace for the memory management with PSI supported

This namespace supports all memory allocations with or without PFS
instrumentation. Ideally memory allocation from Dstore layer MUST use following
APIs. In case of any missing API, feel free to introduce it here.
We don't use std APIs because they don't support PFS, and we don't use existing
MySQL server layer APIs directly because we would like to use our formal APIs to
wrap existing APIs scattered around the code base so that we precisely know
which API should be used for which scenario.

There are two parts of this namespace:
1. About the memory heap definition
The underlying of the memory heap named MemHeap comes from MEM_ROOT, we
provide an alias for it and encapsulate some relevant APIs, so the coding style
can strictly follow Dstore tyle. Also, the common used APIs are explained here
with some simple examples.

2. About the dynamic memory allocation
Some formal APIs are defined for necessary dynamic allocation with different
purposes. The reason we need these APIs is that PFS should be supported during
memory allocation, thus the allocations can be instrumented. Though we provide
two categories of APIs, one requires the PFS key provided, while the other
doesn't, we strongly suggest the one with PFS key requirement should be used
in our code.

To add a new PFS key, please define them around PSIKeyStd, also remember to
add them to the array named PSIInfo.

Currently we support these functions(Please remember to update the list when
introducing new APIs.):
a) Normal malloc/zalloc/realloc/free, etc.
  MallocWithKey
  Malloc
  ZallocWithKey
  Zalloc
  ReallocWithKey
  Realloc
  Free
b) Aligned malloc, etc.
  AlignedMallocWithKey
  AlignedMalloc
  AlignedFree
c) Normal new/delete, etc.
  NewWithKey
  New
  NewArrayWithKey
  NewArray
  Delete
  DeleteArray
d) std::make_unique equivalent APIs, etc.
  MakeUniqueWithKey
  MakeUnique
  MakeUniqueWithKey
  MakeUnique

The APIs with postfix 'WithKey' are recommended to use, so that PFS can be
supported when it is enabled.

Apart from above APIs, here we defined some alias for std containers, such as
* Vector for std::vector
* List for std::List
* Set for std::set
* UnorderedSet for std::unordered_set
* Map for std::map
* UnorderedMap for std::unordered_map

By using these alias, it is possible to allocate memory for the containers
instrumented by the PFS, with the specified PFS key, default PSIKeyStd.

There are examples in the function comment of each API, and there are also
quite a few runnable examples in ut_cde_alloc.cc for reference.
*/
namespace Memory {

/** Start of MemHeap section. */
/**
Define the alias for MEM_ROOT and MakeUniqueDestroyOnly relevant stuff so that
the callers in Dstore can be written in consistent way, and it is easy to know
which functions of MEM_ROOT can be used from this comment. In Dstore code, let
us just use the following alias and APIs, other APIs of MEM_ROOT can actually
ignored for now.

When a continuous memory is suitable for memory allocation, for example, we need
to keep some data on newly allocated memory for a period of time, and we would
prefer an easy deletion, it is very convenient to use the MEM_ROOT, which now
has an alias called MemHeap.

To initialize a MemHeap, let's always remember to pass a PSI memory key and the
size of memory to it. For example:
  MemHeap mem(PSIKeyStd, 1024);
If no key is necessary, let's just use PSI_NOT_INSTRUMENTED as the key.

To allocate a piece of memory, let's use:
  void *ptr = mem.Alloc(100);
To allocate an array, let's use:
  int *arr = mem.ArrayAlloc<int>(10, 100);

  struct A {
    A(int a, int b) : m_a(a), m_b(b) {}
    int m_a;
    int m_b;
  };

  A *ptr1 = mem.ArrayAlloc<A>(10, 1, 2);
In case we would like to shrink the heap for future reuse, let's use:
  mem.ClearForReuse();

If we would like to allocate an object on the memory heap, let's use:
  // No need to 'delete' object A if members of A don't require destruction.
  A *ptr2 = new (&mem) A(1, 2);
If we would like to make sure the allocated object must be destroyed, let's use:
  // The destructor of object would be called automatically.
  UniquePtrDestroyOnly<A> ptr3 = MakeUniqueDestroyOnly<A>(&mem, 1, 2);
*/
using MemHeap = MEM_ROOT;

/** std::unique_ptr, but only destroying. */
template <typename T>
using UniquePtrDestroyOnly = unique_ptr_destroy_only<T>;

/** Create specified object on memory heap, protected by std::unique_ptr. */
template <typename T, typename... Args>
UniquePtrDestroyOnly<T> MakeUniqueDestroyOnly(MemHeap *heap, Args &&... args) {
  return make_unique_destroy_only<T>(heap, std::forward<Args>(args)...);
}
/** End of MemHeap section. */

/**
Dynamically allocates memory of given size, which can be instrumented by PFS.

Example:
  int *a = static_cast<int*>(Memory::MallocWithKey(key, 10 * sizeof(int)));

@param[in]  key     PSI memory key for this allocation
@param[in]  size    size of memory to allocate

@return the memory allocated or nullptr if failed.
*/
inline void *MallocWithKey(PSI_memory_key key, std::size_t size) noexcept {
  return my_malloc(key, size, MYF(0));
}

/**
Dynamically allocates memory of given size, which would NOT be instrumented by
PFS.

Better NOT use this variant because of no instrumenting, unless having a good
reason.

Example:
  int *a = static_cast<int*>(Memory::Malloc(10 * sizeof(int)));

@param[in]  size    size of memory to allocate

@return the memory allocated or nullptr if failed.
*/
inline void *Malloc(std::size_t size) noexcept {
  return my_malloc(PSI_NOT_INSTRUMENTED, size, MYF(0));
}

/**
Dynamically allocates memory of given size, which is filled by zero and
can be instrumented by PFS.

Example:
  int *a = static_cast<int*>(Memory::ZallocWithKey(key, 10 * sizeof(int)));

@param[in]  key     PSI memory key for this allocation
@param[in]  size    size of memory to allocate

@return the memory allocated or nullptr if failed.
*/
inline void *ZallocWithKey(PSI_memory_key key, std::size_t size) noexcept {
  return my_malloc(key, size, MYF(MY_ZEROFILL));
}

/**
Dynamically allocates memory of given size, which is filled by zero and
would NOT be instrumented by PFS.

Better NOT use this variant because of no instrumenting, unless having a good
reason.

Example:
  int *a = static_cast<int*>(Memory::Zalloc(10 * sizeof(int)));

@param[in]  size    size of memory to allocate

@return the memory allocated or nullptr if failed.
*/
inline void *Zalloc(std::size_t size) noexcept {
  return my_malloc(PSI_NOT_INSTRUMENTED, size, MYF(MY_ZEROFILL));
}

/**
Expands or shrinks the specified memory, copying the memory area with size
equal the lesser of the new and the old size, and freeing the old memory,
if the specified memory is not nullptr. The new memory can be instrumented
by PFS.

Example:
  int *a = static_cast<int*>(Memory::MallocWithKey(key, 10 * sizeof(int)));
  a = static_cast<int*>(Memory::ReallocWithKey(key, a, 20 * sizeof(int)));

@param[in]      key     PSI memory key for this allocation
@param[in,out]  ptr     the specified memory to expand or shrink
@param[in]      size    new size of the memory to allocate

@return the new memory allocated or nullptr if failed
*/
inline void *ReallocWithKey(PSI_memory_key key, void *ptr,
                            std::size_t size) noexcept {
  return my_realloc(key, ptr, size, MYF(0));
}

/**
Expands or shrinks the specified memory, copying the memory area with size
equal the lesser of the new and the old size, and freeing the old memory,
if the specified memory is not nullptr. The new memory would NOT be
instrumented by PFS.

Better NOT use this variant because of no instrumenting, unless having a good
reason.

Example:
  int *a = static_cast<int*>(Memory::Malloc(10 * sizeof(int)));
  a = static_cast<int*>(Memory::Realloc(a, 20 * sizeof(int)));

@param[in,out]  ptr     the specified memory to expand or shrink
@param[in]      size    new size of the memory to allocate

@return the new memory allocated or nullptr if failed.
*/
inline void *Realloc(void *ptr, std::size_t size) noexcept {
  return my_realloc(PSI_NOT_INSTRUMENTED, ptr, size, MYF(0));
}

/**
Frees the specified memory, which must be allocated by one of the above
Malloc*(), Zalloc*(), Realloc*() variants.

Example:
  int *a = static_cast<int*>(Memory::MallocWithKey(key, 10 * sizeof(int)));
  Memory::Free(a);

@param[in,out]  ptr    the specified memory to free
*/
inline void Free(void *ptr) noexcept { my_free(ptr); }

/**
Structure to store the pointer to an aligned memory.
Here two pointers are maintained, one points to the aligned memory, the other
is the pointer to the raw memory before alignment. Caller only cares about
the aligned memory via the Get() API, only the AlignedFree can access the
pointer to raw memory to free it.
*/
struct AlignedMem {
 public:
  friend inline void AlignedFree(AlignedMem &aligned) noexcept;

  /**
  Constructor.

  @param[in]  aligned     the pointer to the aligned memory
  @param[in]  original    the pointer to the original memory
  @param[in]  size        the size of the aligned memory
  */
  AlignedMem(void *aligned, void *original, size_t size)
      : m_aligned(aligned), m_original(original), m_size(size) {}

  /**
  Get the aligned memory to use.

  @return the aligned memory, nullptr if this is an invalid memory.
  */
  void *Get() const { return m_aligned; }

  /** Get the size of the aligned memory. */
  size_t GetSize() const { return m_size; }

  /**
  @return true if this memory is valid, otherwise false.
  */
  bool IsValid() const { return m_aligned != nullptr && m_original != nullptr; }

 private:
  /**
  Invalid the memory, setting both to nullptr.
  */
  void Invalid() {
    m_aligned = nullptr;
    m_original = nullptr;
    m_size = 0;
  }

 private:
  /**
  The pointer to the aligned memory, used for storage, or nullptr if invalid.
  */
  void *m_aligned;

  /**
  The pointer to the original memory, used for free, or nullptr if invalid.
  */
  void *m_original;

  /** The size of the aligned memory. */
  size_t m_size;
};

/**
Dynamically allocates memory of given size, which can be instrumented by PFS.
Aligns the starting address to the requested alignment which MUST be a number
of power of 2, otherwise the returned value is undefined. If the alignement
is not bigger than alignof(std::max_align_t), simply use the MallocWithKey.

Example:
  AlignedMem mem;
  mem = Memory::AlignedMallocWithKey(key, 10 * sizeof(int), 8, true);
  if (!mem.IsValid()) // failure
  memset(mem.Get(), 1, 10 * sizeof(int));

@param[in]  key                PSI memory key for this allocation
@param[in]  size               size of memory to allocate
@param[in]  alignment          alignment of this allocation, which MUST be
                               power of 2
@param[in]  zeroInitialized    true if zeroes should be filled into this memory

@return the memory allocated, nullptr if failed or alignment is too big.
*/
inline AlignedMem AlignedMallocWithKey(PSI_memory_key key, std::size_t size,
                                       std::size_t alignment,
                                       bool zeroInitialized) noexcept {
  uint64_t totalLen = size + alignment;
  void *mem;

  assert(alignment > 0 && (alignment & (alignment - 1)) == 0);

  if (!zeroInitialized) {
    mem = MallocWithKey(key, totalLen);
  } else {
    mem = ZallocWithKey(key, totalLen);
  }

  if (unlikely(mem == nullptr)) return Memory::AlignedMem(nullptr, nullptr, 0);

  void *aligned = mem;
  [[maybe_unused]] auto ret = std::align(alignment, size, aligned, totalLen);
  assert(ret != nullptr);

  return Memory::AlignedMem(aligned, mem, size);
}

/**
Dynamically allocates memory of given size, which would NOT be instrumented by
PFS. Aligns the starting address to the requested alignment which MUST be a
number of power of 2, otherwise the returned value is undefined. if the
alignment is not bigger than alignof(std::max_align_t), simply use the
Malloc.

Better NOT use this variant because of no instrumenting, unless having a good
reason.

Example:
  AlignedMem mem;
  mem = Memory::AlignedMalloc(10 * sizeof(int), 8, true);
  if (!mem.IsValid()) // failure
  memset(mem.Get(), 1, 10 * sizeof(int));

@param[in]  size               size of memory to allocate
@param[in]  alignment          alignment of this allocation, which MUST be
                               power of 2
@param[in]  zeroInitialized    true if zeroes should be filled into thie memory

@return the memory allocated, nullptr if failed or alignment is too big.
*/
inline AlignedMem AlignedMalloc(std::size_t size, std::size_t alignment,
                                bool zeroInitialized) noexcept {
  return AlignedMallocWithKey(PSI_NOT_INSTRUMENTED, size, alignment,
                              zeroInitialized);
}

/**
Frees the specified aligned memory, which must be allocated by one of above
AlignedMalloc* variants.

Example:
  AlignedMem mem;
  mem = Memory::AlignedMalloc(10 * sizeof(int), 8, true);
  if (!mem.IsValid()) // failure
  memset(mem.Get(), 1, 10 * sizeof(int));

  AlignedFree(mem);

@param[in,out]  mem    the aligned memory object to free, the object would
                       become invalid too.
*/
inline void AlignedFree(AlignedMem &mem) noexcept {
  if (unlikely(!mem.IsValid())) {
    /* The New variants should not set only one nullptr. */
    assert(mem.m_original == mem.m_aligned);
    return;
  }

  assert(reinterpret_cast<uintptr_t>(mem.m_original) <=
         reinterpret_cast<uintptr_t>(mem.m_aligned));
  Memory::Free(mem.m_original);
  mem.Invalid();
}

/**
Class to allocate memory for array allocation.

The reason to introduce this class is to simplify the maintenance of the memory
for an array. As we know, if an array is allocated for
non-trivially-destructible types, during destruction, each object has to be
destroyed one by one. However, the number of objects is not out of box with the
pointer to the array. So this class will maintain a metadata area to remember
the size of the array in bytes, the number of objects can be retrieved from
metadata, so the destrucion can be done accurately.

The memory layout looks like this:

|______Metadata______|______...Data...______|
\                   /
alignof(max_align_t)

So an extra area called metadata would be allocated in front of the data area,
where the data length would be remembered and whose size is always
s_metadataLen.

The returned allocated address would start from the first byte of the data
area, and the metadata starting address can be deduced from pointer to the data
area.

The specific API DeleteArray must be called to free this kind of allocation,
to make sure the memory and objects are legitimately freed.
*/
class ArrayAlloc {
  using LengthType = size_t;

 public:
  /**
  Dynamically allocates memory of given size.

  @param[in]  key     PSI memory key for this allocation
  @param[in]  size    size of memory in bytes to allocate

  @return the pointer to the allocated address, nullptr if failed.
  */
  static inline void *Alloc(PSI_memory_key key, std::size_t size) noexcept {
    const auto length = size + s_metadataLen;
    auto *mem = Memory::MallocWithKey(key, length);
    *(static_cast<LengthType *>(mem)) = size;
    return static_cast<uint8_t *>(mem) + s_metadataLen;
  }

  /**
  Releases memory which was allocated by ArrayAlloc::Alloc().

  @param[in,out]  ptr    pointer to the memory to release
  */
  static inline void Free(void *ptr) noexcept {
    if (unlikely(ptr == nullptr)) return;
    Memory::Free(Deduce(ptr));
  }

  /**
  Returns the length of the allocated memory in bytes.

  @param[in]  ptr    pointer to memory allocated through ArrayAlloc::Alloc()

  @return the length of the allocated memory in bytes, or random value if the
          ptr is not returned by the ArrayAlloc::Alloc().
  */
  static inline LengthType ArrayLength(void *ptr) {
    return *reinterpret_cast<LengthType *>(Deduce(ptr));
  }

 private:
  /**
  Helper function which deduces the original starting address of one pointer,
  which must be returned by ArrayAlloc::Alloc().

  @param[in]  ptr    pointer to deduce which must be returned by Alloc()

  @return the deduced original starting address of ptr.
  */
  static inline void *Deduce(void *ptr) noexcept {
    return static_cast<uint8_t *>(ptr) - s_metadataLen;
  }

 private:
  /** The metadata area length. */
  static constexpr auto s_metadataLen = alignof(max_align_t);

  /** The storage size of the allocation size must fit into the metadata. */
  static_assert(sizeof(LengthType) <= s_metadataLen,
                "Metadata length is too small.");
};

/**
Dynamically allocates an object of type T on a new memory area of the exact
size, which can be instrumented by PFS. Constructs the object of type T with
provided Args if exists.

Example:
  int *ptr1 = Memory::NewWithKey<int>(key);

  int *ptr2 = Memory::NewWithKey<int>(key, 1);
  assert(*ptr2 == 1);

  struct A {
    A(int a, int b) : m_a(a), m_b(b) {}
    int m_a;
    int m_b;
  };
  A *ptr3 = Memory::NewWithKey<A>(key, 1, 2);
  assert(ptr3->m_a == 1);
  assert(ptr3->m_b == 1);

@param[in]  key     PSI memory key for this allocation
@param[in]  args    arguments which would be forwarded to the constructor of T

@return the pointer to a constructed object T or nullptr in case of any failure.
*/
template <typename T, typename... Args>
inline T *NewWithKey(PSI_memory_key key, Args &&... args) noexcept {
  auto mem = MallocWithKey(key, sizeof(T));
  if (unlikely(mem == nullptr)) return nullptr;
  try {
    new (mem) T(std::forward<Args>(args)...);
  } catch (...) {
    Memory::Free(mem);
    return nullptr;
  }

  return static_cast<T *>(mem);
}

/**
Dynamically allocates an object of type T on a new memory area of the exact
size, which would NOT be instrumented by PFS. Constructs the object of type T
with provided Args if exists.

Better NOT use this variant because of no instrumenting, unless having a good
reason.

Example:
  int *ptr1 = Memory::New<int>();

  int *ptr2 = Memory::New<int>(1);
  assert(*ptr2 == 1);

  struct A {
    A(int a, int b) : m_a(a), m_b(b) {}
    int m_a;
    int m_b;
  };
  A *ptr3 = Memory::New<A>(1, 2);
  assert(ptr3->m_a == 1);
  assert(ptr3->m_b == 1);

@param[in]  args    arguments which would be passed to the constructor of T

@return the pointer to a constructed object T or nullptr in case of any failure.
*/
template <typename T, typename... Args>
inline T *New(Args &&... args) noexcept {
  return NewWithKey<T>(PSI_NOT_INSTRUMENTED, std::forward<Args>(args)...);
}

/**
Dynamically allocates an array of type T on a new memory area of the exact size,
which can be instrumented by PFS. Constructs each object of type T with provided
Args if exsits.

Example:
  int *ptr1 = Memory::NewArrayWithKey<int>(key, 2);

  int *ptr2 = Memory::NewArrayWithKey<int>(key, 2, 1);
  assert(ptr2[0] == 1);
  assert(ptr2[1] == 1);

  struct A {
    A(int a, int b) : m_a(a), m_b(b) {}
    int m_a;
    int m_b;
  };
  A *ptr3 = Memory::NewArrayWithKey<A>(key, 2, 1, 2);
  assert(ptr3[0]->m_a == 1);
  assert(ptr3[0]->m_b == 2);
  assert(ptr3[1]->m_a == 1);
  assert(ptr3[1]->m_b == 2);

@param[in]  key      PSI memory key for this allocation
@param[in]  count    size of the array to allocate
@param[in]  Args     arguments which would be passed to the constructor of
                     each object T

@return the pointer to the start address of the array where all the constructed
object T(s) are accommodated or nullptr in case of any failure.
*/
template <typename T, typename... Args>
inline T *NewArrayWithKey(PSI_memory_key key, size_t count,
                          Args &&... args) noexcept {
  using AllocImpl = Memory::ArrayAlloc;
  auto mem = AllocImpl::Alloc(key, sizeof(T) * count);
  if (unlikely(mem == nullptr)) return nullptr;

  size_t offset = 0;
  try {
    for (; offset < sizeof(T) * count; offset += sizeof(T)) {
      new (reinterpret_cast<uint8_t *>(mem) + offset)
          T(std::forward<Args>(args)...);
    }
  } catch (...) {
    for (; offset != 0; offset -= sizeof(T)) {
      reinterpret_cast<T *>(reinterpret_cast<std::uintptr_t>(mem) + offset -
                            sizeof(T))
          ->~T();
    }

    AllocImpl::Free(mem);
    return nullptr;
  }

  return static_cast<T *>(mem);
};

/**
Dynamically allocates an array of type T on a new memory area of the exact size,
which would NOT be instrumented by PFS. Constructs each object of type T with
provided Args if exists.

Better NOT use this variant because of no instrumenting, unless having a good
reason.

Example:
  int *ptr1 = Memory::NewArray<int>(2);

  int *ptr2 = Memory::NewArray<int>(2, 1);
  assert(ptr2[0] == 1);
  assert(ptr2[1] == 1);

  struct A {
    A(int a, int b) : m_a(a), m_b(b) {}
    int m_a;
    int m_b;
  };
  A *ptr3 = Memory::NewArray<A>(2, 1, 2);
  assert(ptr3[0]->m_a == 1);
  assert(ptr3[0]->m_b == 2);
  assert(ptr3[1]->m_a == 1);
  assert(ptr3[1]->m_b == 2);

@param[in]  count    size of the array to allocate
@param[in]  Args     arguments which would be passed to the constructor of
                     each object T

@return the pointer to the start address of the array where all the constructed
object T(s) are accommodated or nullptr in case of any failure.
*/
template <typename T, typename... Args>
inline T *NewArray(size_t count, Args &&... args) noexcept {
  return NewArrayWithKey<T>(PSI_NOT_INSTRUMENTED, count,
                            std::forward<Args>(args)...);
}

/**
Deletes the object of type T, which must be created via the Memory::NewWithKey()
or Memory::New() variants. Frees the memory occupied by the object too.

Example:
  int *ptr1 = Memory::New<int>();
  Memory::Delete(ptr1);

@param[in]  ptr    pointer to the object to delete
*/
template <typename T>
inline void Delete(T *ptr) noexcept {
  if (unlikely(ptr == nullptr)) return;
  ptr->~T();
  Memory::Free(ptr);
}

/**
Deletes the array of type T, which must be created via the Memory::NewArray*()
variants. Frees the memory occupied by the array too.

Example:
  int *ptr = Memory::NewArray<int>(2, 1);
  Memory::DeleteArray(ptr);

@param[in]  ptr    pointer to the start address of the array to delete
*/
template <typename T>
inline void DeleteArray(T *ptr) noexcept {
  if (unlikely(ptr == nullptr)) return;

  using AllocImpl = Memory::ArrayAlloc;
  const auto arrayLen = AllocImpl::ArrayLength(ptr);
  for (size_t offset = 0; offset < arrayLen; offset += sizeof(T)) {
    reinterpret_cast<T *>(reinterpret_cast<std::uintptr_t>(ptr) + offset)->~T();
  }

  AllocImpl::Free(ptr);
}

namespace Detail {
/** A base structure for allocator below, without PFS supported. */
template <typename T>
struct AllocatorBase {
  /** Constructor. */
  explicit AllocatorBase(PSI_memory_key /* key */) {}

  /** Copy constructor. */
  template <typename U>
  explicit AllocatorBase(const AllocatorBase<U> &other) {}

  /**
  Do the real allocation.
  @param[in]  size    size of memory to allocate
  @return the allocated memory or nullptr if failed.
  */
  void *AllocateImpl(size_t size) { return Memory::Malloc(size); }
};

/** A base structure for allocator below, with PFS supported. */
template <typename T>
struct AllocatorBaseWithKey {
  /**
  Constructor.
  @param[in]  key    PSI memory key for the allocated memory
  */
  explicit AllocatorBaseWithKey(PSI_memory_key key) : m_key(key) {}

  /** Copy constructor. */
  template <typename U>
  explicit AllocatorBaseWithKey(const AllocatorBaseWithKey<U> &other)
      : AllocatorBaseWithKey(other.GetKey()) {}

  /** Get the PSI memory key for this allocator. */
  PSI_memory_key GetKey() const { return m_key; }

  /**
  Do the real allocation.
  @param[in]  size    size of memory to allocate
  @return the allocated memory or nullptr if failed.
  */
  void *AllocateImpl(size_t size) { return Memory::MallocWithKey(m_key, size); }

 private:
  /** The PSI memory key for this allocator. */
  const PSI_memory_key m_key;
};
}  // namespace Detail

#ifdef HAVE_PSI_MEMORY_INTERFACE
constexpr bool WITH_PFS = true;
#else
constexpr bool WITH_PFS = false;
#endif /* HAVE_PSI_MEMORY_INTERFACE */

/**
Allocator which allows std containers to manage the memory through
our own malloc and free variants, in this case, it should be Memory::Malloc*()
variants and Memory::Free().

By using this allocator, the memory allocated by std containers can be
instrumented by PFS with the specified key, default is PSIKeyStd.

Apart from the std containers, this allocator is also suitable for the case
which allows a standard user specified allocator.

Example:
  // Use the default PSI key
  std::vector<int, Memory::Allocator<int>> vec1;
  vec1.push_back(10);

  // Use the specified PSI key
  Memory::Allocator<int> allocator(key);
  std::vector<int, Memory::Allocator<int>> vec2(allocator);
  vec2.push_back(20);

  auto *vec3 = new std::vector<int, Memory::Allocator<int>>(
      Memory::Allocator<int>(key));
  vec3->push_back(30);
  delete vec3;
*/
template <typename T,
          typename AllocatorBase =
              std::conditional_t<WITH_PFS, Detail::AllocatorBaseWithKey<T>,
                                 Detail::AllocatorBase<T>>>
class Allocator : public AllocatorBase {
 public:
  using pointer = T *;
  using const_pointer = const T *;
  using reference = T &;
  using const_reference = const T &;
  using value_type = T;
  using size_type = size_t;
  using difference_type = ptrdiff_t;

  static_assert(alignof(T) <= alignof(std::max_align_t),
                "Allocator does not support over-aligned types.");

  /**
  Default constructor.

  @param[in] key     PSI memory key.
  */
  explicit Allocator(PSI_memory_key key = PSIKeyStd) : AllocatorBase(key) {}

  Allocator(const Allocator<T, AllocatorBase> &) = default;
  Allocator<T, AllocatorBase> &operator=(const Allocator<T, AllocatorBase> &) =
      default;
  Allocator(Allocator<T, AllocatorBase> &&) = default;
  Allocator<T, AllocatorBase> &operator=(Allocator<T, AllocatorBase> &&) =
      default;
  ~Allocator() = default;

  template <typename U>
  explicit Allocator(const Allocator<U, AllocatorBase> &other)
      : AllocatorBase(other) {}

  /** The rebind struct required by the implmentation. */
  template <typename U>
  struct rebind {
    using other = Memory::Allocator<U, AllocatorBase>;
  };

  /** Implementation of operator ==. */
  inline bool operator==(const Allocator<T, AllocatorBase> &) const {
    return true;
  }

  /** Implementation of operator !=. */
  inline bool operator!=(const Allocator<T, AllocatorBase> &other) const {
    return !(*this == other);
  }

  /**
  Return the max number of objects that can be allocated by this allocator.
  */
  size_t max_size() const {
    return std::numeric_limits<size_t>::max() / sizeof(T);
  }

  /**
  Allocates chunk of memory that can hold count objects of type T.
  Returned pointer is always valid. In case underlying allocation function was
  not able to fulfill the allocation request, this function will throw
  std::bad_alloc exception. Otherwise, returned pointer must be handed over to
  Allocator<T>::Deallocate() to free.

  @param[in] count   number of objects to allocate

  @return pointer to the allocated memory or nullptr if specified size is 0.
  In case of any failure, std::bad_alloc would be raised.
  */
  T *allocate(size_t count, const T * = nullptr) {
    if (unlikely(count == 0)) return nullptr;
    if (unlikely(count > max_size())) throw std::bad_alloc();

    T *p = static_cast<T *>(AllocatorBase::AllocateImpl(count * sizeof(T)));
    if (unlikely(p == nullptr)) throw std::bad_alloc();

    return p;
  }

  /**
  Releases the memory allocated through Allocator<T>::allocate()

  @param[in,out]  ptr      pointer to memory to free
  @param[in]      count    number of object allocated
  */
  void deallocate(T *ptr, size_t count [[maybe_unused]] = 0) {
    Memory::Free(ptr);
  }

  /**
  Construct an object of specified type U, with provided arguments.
  @param[in,out]  p    pointer to the object to construct
  @param[in]      args arguments for the constructor
  */
  template <class U, class... Args>
  void construct(U *p, Args &&... args) {
    try {
      ::new ((void *)p) U(std::forward<Args>(args)...);
    } catch (...) {
      /*
        Do not expect exception from constructor.
        We don't use CDE_ASSERT here because it requires more file changes when
        writing unit test for the memory APIs. After we move CDE_ASSERT to a
        simpler separated file, we may replace assert with it.
      */
      assert(false);
    }
  }

  /**
  Destroy the object of type T.
  @param[in,out]  p    object to destroy
  */
  void destroy(T *p) {
    try {
      p->~T();
    } catch (...) {
      /* Do not expect exception from destructor. */
      assert(false);
    }
  }
};

namespace Detail {

/** Determines if an array of type T is provided. When we come to C++20, we
should use std::is_unbounded_array, etc. */
template <typename>
constexpr bool IsUnboundedArray = false;
template <typename T>
constexpr bool IsUnboundedArray<T[]> = true;

/**
Deleter which can be used to delete object of type T constructed by
Memory::New* variants. It is introduced for following MakeUnique variants.
*/
template <typename T>
struct Deleter {
  void operator()(T *ptr) { Delete(ptr); }
};

/**
Deleter which can be used to delete an array of type T constructed by
Memory::NewArray* variants. It is introduced for following MakeUnique variants.
*/
template <typename T>
struct ArrayDeleter {
  void operator()(T *ptr) { DeleteArray(ptr); }
};
}  // namespace Detail

/**
Dynamically allocates an object of type T on a new memory area of exact size,
which can be instrumented by PFS. Constructs the object of type T with provided
Args if exsists. Wraps the pointer to the object into the std::unique_ptr.

This template only takes effect when T is not an array type.

Example:
  int *ptr1 = Memory::MakeUniqueWithKey<int>(key);

  int *ptr2 = Memory::MakeUniqueWithKey<int>(key, 1);
  assert(*ptr2 == 1);

  struct A {
    A(int a, int b) : m_a(a), m_b(b) {}
    int m_a;
    int m_b;
  };
  A *ptr3 = Memory::MakeUniqueWithKey<A>(key, 1, 2);
  assert(ptr3->m_a == 1);
  assert(ptr3->m_b == 2);

@param[in]  key     PSI memory key for this allocator
@param[in]  args    arguments which would be passed to the constructor of
                    the object T

@return std::unique_ptr holding the pointer to an object of type T
*/
template <typename T, typename Deleter = Detail::Deleter<T>, typename... Args>
std::enable_if_t<!std::is_array<T>::value, std::unique_ptr<T, Deleter>>
MakeUniqueWithKey(PSI_memory_key key, Args &&... args) noexcept {
  return std::unique_ptr<T, Deleter>(
      Memory::NewWithKey<T>(key, std::forward<Args>(args)...));
}

/**
Dynamically allocates an object of type T on a new memory area of exact size,
which would NOT be instrumented by PFS. Constructs the object of type T with
provided Args if exists. Wraps the pointer to the object into the
std::unique_ptr.

This template only takes effect when T is not an array type.

Better NOT use this variant because of no instrumenting, unless having a good
reason.

Example:
  int *ptr1 = Memory::MakeUnique<int>();

  int *ptr2 = Memory::MakeUnique<int>(1);
  assert(*ptr2 == 1);

  struct A {
    A(int a, int b) : m_a(a), m_b(b) {}
    int m_a;
    int m_b;
  };
  A *ptr3 = Memory::MakeUnique<A>(1, 2);
  assert(ptr3->m_a == 1);
  assert(ptr3->m_b == 2);

@param[in]  args    arguments which would be passed to the constructor of
                    the object T

@return std::unique_ptr holding the pointer to an object of type T
*/
template <typename T, typename Deleter = Detail::Deleter<T>, typename... Args>
std::enable_if_t<!std::is_array<T>::value, std::unique_ptr<T, Deleter>>
MakeUnique(Args &&... args) noexcept {
  return std::unique_ptr<T, Deleter>(
      Memory::New<T>(std::forward<Args>(args)...));
}

/**
Dynamically allocates an array of type T on a new memory area of exact size,
which can be instrumented by PFS. Constructs each object of type T with
provided Args if exsists. Wraps the pointer to the object into the
std::unique_ptr.

This template only takes effect when T is an array type with unknown
compile-time bound.

Example:
  int *ptr1 = Memory::MakeUniqueWithKey<int[]>(key, 2);

  int *ptr2 = Memory::MakeUniqueWithKey<int[]>(key, 2, 1);
  assert(ptr2[0] == 1);
  assert(ptr2[1] == 1);

  struct A {
    A(int a, int b) : m_a(a), m_b(b) {}
    int m_a;
    int m_b;
  };
  A *ptr3 = Memory::MakeUniqueWithKey<A[]>(key, 2, 1, 2);
  assert(ptr3[0]->m_a == 1);
  assert(ptr3[0]->m_b == 2);
  assert(ptr3[1]->m_a == 1);
  assert(ptr3[1]->m_b == 2);

@param[in]  key      PSI memory key for the allocation
@param[in]  count    size of the array to allocate
@param[in]  Args     arguments which would be passed to the constructor of
                     each object T

@return the pointer to the start address of the array where all the constructed
object T(s) are accommodated or nullptr in case of any failure.
*/
template <typename T, typename... Args,
          typename Deleter = Detail::ArrayDeleter<std::remove_extent_t<T>>>
std::enable_if_t<Detail::IsUnboundedArray<T>, std::unique_ptr<T, Deleter>>
MakeUniqueWithKey(PSI_memory_key key, size_t size, Args &&... args) noexcept {
  return std::unique_ptr<T, Deleter>(
      Memory::NewArrayWithKey<std::remove_extent_t<T>>(
          key, size, std::forward<Args>(args)...));
}

/**
Dynamically allocates an array of type T on a new memory area of exact size,
which would NOT be instrumented by PFS. Constructs each object of type T with
provided Args if exists. Wraps the pointer to the object into the
std::unique_ptr.

This template only takes effect when T is an array type with unknown
compile-time bound.

Better NOT use this variant because of no instrumenting, unless having a good
reason.

Example:
  int *ptr1 = Memory::MakeUnique<int[]>(2);

  int *ptr2 = Memory::MakeUnique<int[]>(2, 1);
  assert(ptr2[0] == 1);
  assert(ptr2[1] == 1);

  struct A {
    A(int a, int b) : m_a(a), m_b(b) {}
    int m_a;
    int m_b;
  };
  A *ptr3 = Memory::MakeUnique<A[]>(2, 1, 2);
  assert(ptr3[0]->m_a == 1);
  assert(ptr3[0]->m_b == 2);
  assert(ptr3[1]->m_a == 1);
  assert(ptr3[1]->m_b == 2);

@param[in]  key      PSI memory key for the allocation
@param[in]  count    size of the array to allocate
@param[in]  Args     arguments which would be passed to the constructor of
                     each object T

@return the pointer to the start address of the array where all the constructed
object T(s) are accommodated or nullptr in case of any failure.
*/
template <typename T, typename... Args,
          typename Deleter = Detail::ArrayDeleter<std::remove_extent_t<T>>>
std::enable_if_t<Detail::IsUnboundedArray<T>, std::unique_ptr<T, Deleter>>
MakeUnique(size_t size, Args &&... args) noexcept {
  return std::unique_ptr<T, Deleter>(Memory::NewArray<std::remove_extent_t<T>>(
      size, std::forward<Args>(args)...));
}

/** Specializaiton of std::vector which uses Allocator with PSIKeyStd. */
template <typename T>
using Vector = std::vector<T, Memory::Allocator<T>>;

/** Specialization of std::list which uses Allocator with PSIKeyStd. */
template <typename T>
using List = std::list<T, Memory::Allocator<T>>;

/** Specialization of std::set which uses Allocator with PSIKeyStd. */
template <typename Key, typename Compare = std::less<Key>>
using Set = std::set<Key, Compare, Memory::Allocator<Key>>;

/** Specialization of std::unordered set which uses Allocator with PSIKeyStd. */
template <typename Key>
using UnorderedSet = std::unordered_set<Key, std::hash<Key>, std::equal_to<Key>,
                                        Memory::Allocator<Key>>;

/** Specialization of std::map which uses Allocator with PSIKeyStd. */
template <typename Key, typename Value, typename Compare = std::less<Key>>
using Map = std::map<Key, Value, Compare,
                     Memory::Allocator<std::pair<const Key, Value>>>;

/** Specialization of std::unordered map which uses Allocator with PSIKeyStd. */
template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename Key_equal = std::equal_to<Key>>
using UnorderedMap =
    std::unordered_map<Key, Value, Hash, Key_equal,
                       Memory::Allocator<std::pair<const Key, Value>>>;

}  // namespace Memory

}  // namespace CDE

#define CdeAlloc(size) malloc(size)
#define CdeZalloc(size)             \
  ({                                \
    void *__mem = malloc(size);     \
    memset_s(__mem, size, 0, size); \
    __mem;                          \
  })
#define CdeRealloc(ptr, size) realloc(ptr, size)
#define CdeFree(ptr) free(ptr)

/**
To allocates a block of memory and initializes the memory to zero.

@param[in]      size  required memory allocation size.

@return  memory pointer.
*/
void *AllocMemZeroForDtuple(uint32_t size);
inline void CdeFreeMemForDtuple(void *ptr) { CdeFree(ptr); }

#define CDE_MEM_ALIGNMENT (8)
#define CDE_MEM_ALIGN(x)                            \
  (((uintptr_t)(x) + ((CDE_MEM_ALIGNMENT) - (1))) & \
   ~((uintptr_t)((CDE_MEM_ALIGNMENT) - (1))))

#endif  // __CDE_ALLOC_H__
