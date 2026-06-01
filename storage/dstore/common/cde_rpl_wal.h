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

#ifndef CDE_RPL_WAL_H
#define CDE_RPL_WAL_H

#include "common/dstore_common_utils.h"

void CdeWalFlushedLsnNotify(DSTORE::PdbId pdbId, uint64_t flushed_lsn);
void CdeWalWaitStandbyFlush(DSTORE::PdbId pdbId, uint64_t target_lsn);
uint64_t CdeGetStandbyMinFlushedLsn(DSTORE::PdbId pdbId);

#endif