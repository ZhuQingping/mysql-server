/*******************************************************************
 * Copyright (C) Huawei Technologies, 2025
 *
 * Separate Code and Hot Data Storage proc implementation
 *******************************************************************/
#include <string>

#include <mysql/plugin.h>
#include "mysql/components/services/log_builtins.h"
#include "sql/derror.h"
#include "sql/mysqld.h"
#include "sql/package/dbms_dstore_proc.h"
#include "sql/package/package_common.h"
#include "sql/protocol.h"
#include "sql/sql_plugin.h"

namespace dbms_dstore_maintenance {

LEX_CSTRING DBMS_DSTORE_MAINTENANCE_PROC_SCHEMA = {
    C_STRING_WITH_LEN("dbms_dstore_maintenance")};

static bool get_dstore_info_handlerton(THD *thd, plugin_ref plugin, void *arg) {
  handlerton *hton = plugin_data<handlerton *>(plugin);
  if (hton->state == SHOW_OPTION_YES && hton->db_type == DB_TYPE_DSTORE &&
      hton->show_dstore_info) {
    DstoreInfoParams *params = (DstoreInfoParams *)arg;
    hton->show_dstore_info(thd, params->func_name, params->func_arg,
                           params->res);
  }
  return false;
}

bool Sql_cmd_proc_show_dstore_info::pc_execute(THD *) {
  DBUG_ENTER("Sql_cmd_proc_show_dstore_info::pc_execute");
  DBUG_RETURN(false);
}

void Sql_cmd_proc_show_dstore_info::send_result(THD *thd, bool error) {
  DBUG_ENTER("Sql_cmd_proc_show_dstore_info::send_result");

  if (error) {
    DBUG_ASSERT(thd->is_error());
    DBUG_VOID_RETURN;
  }
  Protocol *protocol = thd->get_protocol();
  if (m_proc->send_result_metadata(thd)) {
    DBUG_VOID_RETURN;
  }

  String res;
  String param_name;
  String param_args;
  String *func_name = (*m_list)[0]->val_str(&param_name);
  String *func_args = (*m_list)[1]->val_str(&param_args);
  if ((nullptr == func_name) || (nullptr == func_args) ||
      !check_user_param_avaliable(func_name, func_args)) {
    res.append(
        "Invalid param length, the param length should be less than 128.");
  } else {
    std::string name = std::string(func_name->ptr(), func_name->length());
    DstoreInfoParams info_params;
    info_params.func_name = func_name->ptr();
    info_params.func_arg = func_args->ptr();
    info_params.res = &res;
    plugin_foreach(thd, get_dstore_info_handlerton, MYSQL_STORAGE_ENGINE_PLUGIN,
                   &info_params);
  }
  protocol->start_row();

  protocol->store_string(res.ptr(), res.length(), system_charset_info);
  if (protocol->end_row()) {
    DBUG_VOID_RETURN;
  }

  my_eof(thd);
  DBUG_VOID_RETURN;
}

bool Sql_cmd_proc_show_dstore_info::check_user_param_avaliable(String *name,
                                                               String *arg) {
  const int MAX_PARAMETER_LEN = 128;
  if (name->length() > MAX_PARAMETER_LEN || arg->length() > MAX_PARAMETER_LEN) {
    return false;
  }
  return true;
}
} /* namespace dbms_dstore_maintenance */