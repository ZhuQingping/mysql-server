/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v2. You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 * KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the
 * Mulan PSL v2 for more details.
 * ---------------------------------------------------------------------------------------
 *
 * cde_pdb_replica.h
 *
 *
 *
 * IDENTIFICATION
 *        storage/dstore/rpl_wal/cde_pdb_replica.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef CDE_PDB_REPLICA_H
#define CDE_PDB_REPLICA_H

#include "common/dstore_common_utils.h"

namespace CDE {

/**
  Do single pdb promote.

  @param[out] promote_info result info of pdb promote.

  @retval DSTORE::RetStatus::SUCC success.
  @retval DSTORE::RetStatus::FAIL failed.
*/
DSTORE::RetStatus CdeDoSinglePdbPromote(char *&promote_info);

/**
  Do single pdb demote.

  @param[out] demote_info result info of pdb demote.

  @retval DSTORE::RetStatus::SUCC success.
  @retval DSTORE::RetStatus::FAIL failed.
*/
DSTORE::RetStatus CdeDoSinglePdbDemote(char *&demote_info);

}  // namespace CDE

#endif