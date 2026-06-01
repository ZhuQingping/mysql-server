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

#include "mysql/psi/mysql_memory.h"

#include "cde_alloc.h"
#include "common/error/dstore_error.h"
#include "common/memory/dstore_memory_allocator.h"
#include "errorcode/dstore_tuple_error_code.h"

namespace CDE {

/* Keys for registering allocations with performance schema.
Keep this list alphabetically sorted. */

/* PSI key to account the memory allocation from std containers. */
PSI_memory_key PSIKeyStd;

/* Please obey alphabetical order in the definitions above. */

#ifdef HAVE_PSI_MEMORY_INTERFACE
/**
Auxiliary array of performance schema 'PSI_memory_info'.
Each allocation appears in
performance_schema.memory_summary_global_by_event_name (and alike). And each
name in the view is picked from the list below:
1. If a key is specified, then the respective name is used.
2. Without a specified key, allocations from inside std containers use
PSIKeyStd.

Keep this list alphabetically sorted. */
static PSI_memory_info PSIInfo[] = {
    {&PSIKeyStd, "std", 0, 0, PSI_DOCUMENT_ME},
    /* Please obey alphabetical order in the definitions above. */
};
#endif /* HAVE_PSI_MEMORY_INTERFACE */

#ifdef HAVE_PSI_MEMORY_INTERFACE
static constexpr size_t numPSIInfo = sizeof(PSIInfo) / sizeof(PSIInfo[0]);
#endif /* HAVE_PSI_MEMORY_INTERFACE */

void DstoreAllocBoot() {
#ifdef HAVE_PSI_MEMORY_INTERFACE
  PSI_MEMORY_CALL(register_memory)("dstore", PSIInfo, numPSIInfo);
#endif /* HAVE_PSI_MEMORY_INTERFACE */
}

} /* namespace CDE */

void *AllocMemZeroForDtuple(uint32_t size) {
  if (!AllocSizeIsValid(size)) {
    DSTORE::StorageSetErrorCodeOnly(DSTORE::TUPLE_ERROR_TUPLE_TOO_BIG);
    return nullptr;
  }
  return CdeZalloc(size);
}