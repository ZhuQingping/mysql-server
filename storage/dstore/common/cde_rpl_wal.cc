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

#include "cde_rpl_wal.h"
#include <assert.h>
#include "sql/log.h"
#include "sql/rpl_handler.h"  // RUN_HOOK
#include "sql/rpl_wal_mgr.h"

void CdeWalFlushedLsnNotify(DSTORE::PdbId pdbId [[maybe_unused]],
                            uint64_t flushed_lsn) {
  assert(pdbId != 0 && flushed_lsn != 0);
  if (NO_HOOK(wallog_transmit) && NO_HOOK(wallog_io)) {
    return;
  }

  if (mysql_wal.is_wal_flushed_lsn_inited()) {
    mysql_wal.update_wal_flushed_lsn(flushed_lsn);
  }
}

void CdeWalWaitStandbyFlush(DSTORE::PdbId pdbId [[maybe_unused]],
                            uint64_t target_lsn) {
  assert(pdbId != 0 && target_lsn != 0);
  if (NO_HOOK(wallog_trx)) return;
  RUN_HOOK(wallog_trx, trx_wait, (target_lsn));
}

uint64_t CdeGetStandbyMinFlushedLsn(DSTORE::PdbId pdbId [[maybe_unused]]) {
  assert(pdbId != 0);
  if (NO_HOOK(wallog_trx)) return UINT64_MAX;
  uint64_t standby_min_flushed_lsn = 0;
  RUN_HOOK(wallog_trx, get_standby_flushed_lsn, (&standby_min_flushed_lsn));
  return standby_min_flushed_lsn;
}