/*******************************************************************
 * Copyright (C) Huawei Technologies, 2025
 *
 * Separate Code and Hot Data Storage proc implementation
 *******************************************************************/
#include <string>

#include "mysql/components/services/log_builtins.h"
#include "sql/derror.h"
#include "sql/mysqld.h"
#include "sql/package/package_common.h"
#include "sql/package/rpl_wal_proc.h"
#include "sql/pdb_replica.h"
#include "sql/protocol.h"

namespace rpl_wal {

LEX_CSTRING RPL_WAL_PROC_SCHEMA = {C_STRING_WITH_LEN("rpl_wal")};

bool Sql_cmd_proc_pdb_promote::pc_execute(THD *thd) {
  char *promote_info = nullptr;
  bool error = pdb_promote_main(thd, promote_info);
  if (DBUG_EVALUATE_IF("rpl_wal_proc_execute_fail", true, false) || error) {
    my_error(ER_INTERNAL_ERROR, MYF(0), promote_info);
  };
  return error;
}

bool Sql_cmd_proc_pdb_demote::pc_execute(THD *thd) {
  char *demote_info = nullptr;
  bool error = pdb_demote_main(thd, demote_info);
  if (DBUG_EVALUATE_IF("rpl_wal_proc_execute_fail", true, false) || error) {
    my_error(ER_INTERNAL_ERROR, MYF(0), demote_info);
  };
  return error;
}

} /* namespace rpl_wal */