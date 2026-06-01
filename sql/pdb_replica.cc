/*
   Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

#include "sql/pdb_replica.h"
#include "mysql/components/services/log_builtins.h"  // LogErr
#include "sql/handler.h"                             // THD

static std::mutex pdb_replica_mutex;

bool pdb_promote_main(THD *thd, char *&promote_info) {
  std::unique_lock<std::mutex> lock(pdb_replica_mutex);

  handlerton *cde_hton = ha_resolve_by_legacy_type(thd, DB_TYPE_DSTORE);
  if (DBUG_EVALUATE_IF("pdb_promote_main_fail", true, false) ||
      cde_hton->pdb_promote(thd, promote_info)) {
    return true;
  }

  return false;
}

bool pdb_demote_main(THD *thd, char *&demote_info) {
  std::unique_lock<std::mutex> lock(pdb_replica_mutex);

  handlerton *cde_hton = ha_resolve_by_legacy_type(thd, DB_TYPE_DSTORE);
  if (DBUG_EVALUATE_IF("pdb_demote_main_fail", true, false) ||
      cde_hton->pdb_demote(thd, demote_info)) {
    return true;
  }

  return false;
}