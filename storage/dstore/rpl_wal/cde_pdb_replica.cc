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

#include "rpl_wal/cde_pdb_replica.h"
#include "common/cde_def.h"
#include "diagnose/dstore_pdb_replica_mgr_single_diagnose.h"

namespace CDE {

DSTORE::RetStatus CdeDoSinglePdbPromote(char *&promote_info) {
  DSTORE::RetStatus result =
      DSTORE::StoragePdbReplicaMgrSingleDiagnose::SinglePdbPromote(
          promote_info);
  CDE_ASSERT(promote_info != nullptr);

  return result;
}

DSTORE::RetStatus CdeDoSinglePdbDemote(char *&demote_info) {
  DSTORE::RetStatus result =
      DSTORE::StoragePdbReplicaMgrSingleDiagnose::SinglePdbDemote(demote_info);
  CDE_ASSERT(demote_info != nullptr);

  return result;
}
}  // namespace CDE