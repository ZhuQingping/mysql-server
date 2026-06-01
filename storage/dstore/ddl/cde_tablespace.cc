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

#include "my_dbug.h"
#include "mysqld_error.h"
#include "scope_guard.h"
#include "sql/current_thd.h"
#include "sql/dd/impl/properties_impl.h"
#include "sql/dd/properties.h"
#include "sql/dd/types/partition.h"
#include "sql/dd/types/table.h"
#include "sql/dd/types/tablespace.h"
#include "sql/local_backup/full_local_backup.h"
#include "sql/sql_base.h"
#include "sql/sql_tablespace.h"
#include "sql/sql_thd_internal_api.h"

#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/impl/cache/storage_adapter.h"
#include "sql/dd/impl/types/tablespace_impl.h"

#include "cde_tablespace.h"
#include "common/cde_alloc.h"
#include "common/cde_compare_utils.h"
#include "common/cde_errorcode.h"
#include "ddl/cde_ddl.h"
#include "ddl/cde_ddl_log.h"
#include "ddl/dd_helper.h"
#include "handler/ha_cde.h"

#include "errorcode/dstore_tablespace_error_code.h"
#include "framework/dstore_instance_interface.h"

namespace CDE {

/** The name of the hard-coded temporary tablespace. */
const char *dstore_temp_spacename = "dstore_temporary";

/** The name of the hard-coded default tablespace. */
const char *dstore_default_spacename = "dstore_default";

/** The name of the data dictionary tablespace. */
const char *dstore_global_spacename = "dstore_global";

/** field order in mysql.tablespaces */
static constexpr size_t INDEX_NAME = 1;
static constexpr size_t INDEX_PRIVATE_DATA = 3;
static constexpr size_t INDEX_ENGINE_TYPE = 5;

/** tablespace default minisize, 8M/per file * 5 file */
static constexpr uint64_t MIN_TABLESPACE_SIZE =
    8 * 1024 * 1024 * DSTORE::EXTENT_TYPE_COUNT;

/** global tablespace mgr responsible for tablespace create/drop/query */
TableSpaceMgr tablespaceMgr;

static std::map<const char *, DSTORE::TablespaceId> g_preDefinedTablespaces = {
    {dstore_default_spacename, CDE_DEFAULT_TABLE_SPACE_ID},
    {dstore_global_spacename, CDE_GLOBAL_TABLE_SPACE_ID},
    {dstore_temp_spacename, CDE_TMP_TABLE_SPACE_ID}};

TableSpaceMgr::~TableSpaceMgr() {
  for (auto createdSpace : m_tsNameCache) {
    Memory::Free(createdSpace.second->m_spaceName);
    Memory::Free(createdSpace.second);
  }
}

void TableSpaceMgr::InsertPreDefinedTablespaces(THD *thd) {
  const size_t len = 30 + sizeof("id=;max_size");
  const char *fmt = "id=%u;max_size=%llu";
  char se_private_data_dd[len] = {'\0'};
  auto &dc = *thd->dd_client();
  dd::cache::Dictionary_client::Auto_releaser releaser{&dc};
  for (auto space : g_preDefinedTablespaces) {
    std::unique_ptr<dd::Tablespace> tablespace(dynamic_cast<dd::Tablespace *>(
        new (std::nothrow) typename dd::Tablespace::Impl()));
    tablespace->set_name(space.first);
    tablespace->set_engine(DSTORE_ENGINE_NAME);
    snprintf_s(se_private_data_dd, len, len - 1, fmt, space.second,
               DSTORE::MAX_TABLESPACE_SIZE);
    tablespace->set_se_private_data({se_private_data_dd, len - 1});
    LEX_STRING tblspc_datafile_name = {const_cast<char *>(space.first),
                                       strlen(space.first)};
    tablespace->add_file()->set_filename(
        dd::make_string_type(tblspc_datafile_name));
    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLESPACE, "", space.first,
                     MDL_EXCLUSIVE, MDL_TRANSACTION);
    CDE_ASSERT(!thd->mdl_context.acquire_lock(
        &mdl_request, thd->variables.lock_wait_timeout));
    CDE_ASSERT(!dc.store(tablespace.get()));
    AddTableSpace(space.first, DSTORE::MAX_FILE_SIZE, space.second);
  }
}

void TableSpaceMgr::Init() {
  MutexInit(0, &m_mutex, MY_MUTEX_INIT_FAST);

  THD *thd = current_thd;
  if (opt_initialize) {
    InsertPreDefinedTablespaces(thd);
  } else {
    using std::placeholders::_1;
    using std::placeholders::_2;
    using std::placeholders::_3;
    LoadAllCreatedSpace(
        thd, std::bind(&TableSpaceMgr::AddTableSpace, this, _1, _2, _3));
  }
}

TableSpace *TableSpaceMgr::AddTableSpace(const char *spaceName,
                                         uint64_t maxSize,
                                         DSTORE::TablespaceId spaceId) {
  CdeMutexGuard guard(&m_mutex, CDE_LOCATION_HERE);
  TableSpace *space = GetSpaceByName(spaceName);
  if (space) {
    CDE_LOG_ERROR(
        "trying to create tablespace:%s, spaceId:%d, but has exist with "
        "spaceId:%d",
        spaceName, spaceId, space->m_spaceId);
    return nullptr;
  }

  space = static_cast<TableSpace *>(Memory::Zalloc(sizeof(TableSpace)));
  space->m_maxSize = maxSize;
  space->m_spaceId = spaceId;
  int spaceNameLen = strlen(spaceName);
  space->m_spaceName = static_cast<char *>(Memory::Zalloc(spaceNameLen + 1));
  DBUG_EXECUTE_IF("addtablespace_zalloc_failed", {
    Memory::Free(space->m_spaceName);
    space->m_spaceName = nullptr;
  });

  if (!space->m_spaceName) {
    CDE_LOG_ERROR("CdeZalloc SpaceName fail! len = %d", spaceNameLen);
    Memory::Free(space);
    return nullptr;
  }
  if (memcpy_s(space->m_spaceName, spaceNameLen + 1, spaceName, spaceNameLen) !=
      EOK) {
    CDE_LOG_ERROR("addTableSpace memcpy_s spacename:%s fail!", spaceName);
    Memory::Free(space->m_spaceName);
    Memory::Free(space);
    return nullptr;
  }

  m_tsNameCache.emplace(space->m_spaceName, space);
  m_tsIdCache.emplace(space->m_spaceId, space);
  CDE_LOG_SYSTEM("AddTableSpace spaceId:%u spacename:%s", space->m_spaceId,
                 space->m_spaceName);
  return space;
}

TableSpace *TableSpaceMgr::GetSpaceByName(const char *name) {
  MutexAssertOwner(&m_mutex);
  auto existSpace = m_tsNameCache.find(name);
  if (existSpace == m_tsNameCache.end()) {
    return nullptr;
  }

  return existSpace->second;
}

DSTORE::TablespaceId TableSpaceMgr::GetSpaceIdByName(const char *name) {
  CdeMutexGuard guard(&m_mutex, CDE_LOCATION_HERE);
  TableSpace *space = GetSpaceByName(name);
  if (!space) {
    return DSTORE::INVALID_TABLESPACE_ID;
  }

  return space->m_spaceId;
}

int TableSpaceMgr::UpdateTableSpaceName(const char *oldSpaceName,
                                        const char *newSpaceName) {
  CdeMutexGuard guard(&m_mutex, CDE_LOCATION_HERE);
  auto oldSpace = GetSpaceByName(oldSpaceName);
  DBUG_EXECUTE_IF("rename_tablespace_old_not_exist", { oldSpace = nullptr; });
  if (!oldSpace) {
    CDE_LOG_ERROR("while alter rename tablespace missing:%s", oldSpaceName);
    return HA_ERR_TABLESPACE_MISSING;
  }

  auto newSpace = GetSpaceByName(newSpaceName);
  if (newSpace) {
    CDE_LOG_ERROR("while alter rename tablespace to %s has exist",
                  newSpaceName);
    return HA_ERR_TABLESPACE_EXISTS;
  }

  auto originOldSpaceName = oldSpace->m_spaceName;
  int oldSpaceNameLen = strlen(originOldSpaceName);
  int newSpaceNameLen = strlen(newSpaceName);
  if (oldSpaceNameLen >= newSpaceNameLen) {
    m_tsNameCache.erase(originOldSpaceName);
    int ret = strcpy_s(originOldSpaceName, oldSpaceNameLen + 1, newSpaceName);
    if (ret) {
      CDE_LOG_ERROR("strcpy_s failed on tablespace change!");
      return HA_ERR_INTERNAL_ERROR;
    }
  } else {
    auto localNewSpaceName =
        static_cast<char *>(Memory::Zalloc(newSpaceNameLen + 1));
    if (!localNewSpaceName) {
      CDE_LOG_ERROR("CdeZalloc Change SpaceName fail! len = %d",
                    newSpaceNameLen);
      return HA_ERR_INTERNAL_ERROR;
    }

    if (memcpy_s(localNewSpaceName, newSpaceNameLen + 1, newSpaceName,
                 newSpaceNameLen) != EOK) {
      CDE_LOG_ERROR("updateTableSpaceName memcpy_s new spacename:%s fail!",
                    newSpaceName);
      Memory::Free(localNewSpaceName);
      return HA_ERR_INTERNAL_ERROR;
    }

    m_tsNameCache.erase(originOldSpaceName);
    Memory::Free(originOldSpaceName);
    oldSpace->m_spaceName = localNewSpaceName;
  }

  m_tsNameCache.emplace(oldSpace->m_spaceName, oldSpace);
  CDE_LOG_SYSTEM(
      "UpdateTableSpaceName spaceId:%u from oldspacename:%s to "
      "newspacename:%s, new is %s",
      oldSpace->m_spaceId, oldSpaceName, newSpaceName, oldSpace->m_spaceName);
  return CDE_OK;
}

void TableSpaceMgr::DeleteTableSpace(DSTORE::TablespaceId spaceId) {
  CdeMutexGuard guard(&m_mutex, CDE_LOCATION_HERE);
  auto spaceIter = m_tsIdCache.find(spaceId);
  if (spaceIter == m_tsIdCache.end()) {
    CDE_LOG_WARN("deleteTableSpace spaceId:%d not exist in cache", spaceId);
    return;
  }

  auto space = spaceIter->second;
  CDE_LOG_SYSTEM("DeleteTableSpace spaceId is:%u name is %s", spaceId,
                 space->m_spaceName);
  m_tsIdCache.erase(spaceIter);
  m_tsNameCache.erase(space->m_spaceName);
  Memory::Free(space->m_spaceName);
  Memory::Free(space);
}

/** CREATE a tablespace.
@param[in]      hton            Handlerton of DStore
@param[in]      thd             Connection
@param[in]      alter_info      How to do the command
@param[in,out]  dd_space        Tablespace metadata
@return MySQL error code*/
static int CdeCreateTableSpace(THD *thd, st_alter_tablespace *alter_info,
                               dd::Tablespace *dd_space) {
  DBUG_TRACE;

  /* Prohibit create tablespace when local backup is running. */
  if (get_full_lb_running()) {
    CDE_LOG_ERROR(
        "Creating tablespace is not allowed when local backup is running.");
    return HA_ERR_GENERIC;
  }

  uint64_t maxSize = alter_info->max_size;
  if (maxSize == 0) {
    maxSize = DSTORE::MAX_TABLESPACE_SIZE;
  } else if ((maxSize < MIN_TABLESPACE_SIZE) ||
             (maxSize > DSTORE::MAX_TABLESPACE_SIZE)) {
    push_warning_printf(thd, Sql_condition::SL_WARNING,
                        ER_ILLEGAL_HA_CREATE_OPTION,
                        "Value specified for max_size is wrong, range is [40M, "
                        "32768T], 0 represent max");
    return ER_TABLESPACE_MAXSIZE_INVALID;
  }

  CdeTrxStartIfNotStarted(thd, true);
  DSTORE::TablespaceId spaceId;
  DSTORE::RetStatus status = TableSpace_Interface::AllocTablespaceId(
      DSTORE::g_defaultPdbId, alter_info->max_size, &spaceId);
  if (status != DSTORE::DSTORE_SUCC) {
    if (GetDstoreErrcode() == DSTORE::TBS_ERROR_TABLESPACE_ID_USE_UP) {
      return ER_TABLESPACEID_USED_UP;
    }
    return HA_ERR_GENERIC;
  }

  bool writeLogFail = false;
  bool ret = CdeDdlLog::GetInstance()->LogDropTableSpaceId(current_thd, spaceId,
                                                           true, &writeLogFail);
  if (ret == CDE_FAIL) {
    CDE_LOG_ERROR("Log drop tablespace fail, space id:[%u]", spaceId);
    if (writeLogFail == true) {
      CDE_LOG_WARN(
          "Drop the newly created tablespace due to write log fail, space "
          "id:[%u]",
          spaceId);
      DeleteTableSpace(spaceId);
    }
    return HA_ERR_GENERIC;
  }

  DBUG_EXECUTE_IF("crash_after_write_tablespace_ddl_log", { DBUG_SUICIDE(); });

  uint64_t fileIdCnt = 0;
  DSTORE::FileId fileIds[DSTORE::MAX_SPACE_FILE_COUNT] = {0};
  status = TableSpace_Interface::BatchAllocAndAddDataFile(
      DSTORE::g_defaultPdbId, spaceId, fileIds, &fileIdCnt, true);
  if (status == DSTORE::DSTORE_SUCC) {
    auto space = tablespaceMgr.AddTableSpace(alter_info->tablespace_name,
                                             alter_info->max_size, spaceId);
    if (!space) {
      return HA_ERR_GENERIC;
    }

    dd::Properties &p = dd_space->se_private_data();
    p.set(ddSpaceKeyStrings[DD_SPACE_ID], spaceId);
    p.set(ddSpaceKeyStrings[DD_MAX_SIZE], maxSize);
    return CDE_OK;
  }

  return HA_ERR_GENERIC;
}

/** DROP a tablespace.
@param[in]      hton            Handlerton of DStore
@param[in]      thd             Connection
@param[in]      alter_info      How to do the command
@param[in]      dd_space        Tablespace metadata
@return MySQL error code*/
static int CdeDropTableSpace(THD *thd, st_alter_tablespace * /*alter_info*/,
                             const dd::Tablespace *dd_space) {
  int error = 0;

  DBUG_TRACE;

  /* Prohibit drop tablespace when local backup is running. */
  if (get_full_lb_running()) {
    CDE_LOG_ERROR(
        "Dropping tablespace is not allowed when local backup is running.");
    return HA_ERR_GENERIC;
  }

  auto dd_space_name = dd_space->name();

  DSTORE::TablespaceId spaceId;
  /* Be sure that this tablespace is known and valid. */
  if (dd_space->se_private_data().get(ddSpaceKeyStrings[DD_SPACE_ID],
                                      &spaceId) ||
      spaceId == 0) {
    return HA_ERR_TABLESPACE_MISSING;
  }

  /* Prohibit drop tablespace when some segment related to it. */
  if (CdeDdlLog::GetInstance()->RemainTablespaceDDL(spaceId)) {
    CDE_LOG_ERROR(
        "ReplayRecordsBackGround thread still has some ddl operation about "
        "tablespace:%u",
        spaceId);
    return HA_ERR_GENERIC;
  }

  CdeCreateOrGetSession(thd);
  uint8_t result = TableSpace_Interface::IsRemainTablespace(
      DSTORE::g_defaultPdbId, spaceId, false);
  if (result != DSTORE::REMAIN_TABLESPACE) {
    /* The DD knows about it but the actual tablespace is missing.
    Allow it to be dropped from the DD. */
    return 0;
  }

  CdeTrxStartIfNotStarted(thd, true);

  bool ret = CdeDdlLog::GetInstance()->LogDropTableSpaceId(thd, spaceId, false);
  if (ret == CDE_FAIL) {
    CDE_LOG_ERROR("Drop tablespace: %s fail due to LogDropTableSpaceId fail.",
                  dd_space_name.c_str());
    return CDE_FAIL;
  }

  DBUG_EXECUTE_IF("crash_after_drop_tablespace_write_tablespace_ddl_log",
                  { DBUG_SUICIDE(); });

  return error;
}

/** ALTER an tablespace's options.
@param[in]      alter_info      How to do the command
@param[in]      old_dd_space    Tablespace metadata
@param[in]      new_dd_space    Tablespace metadata
@return MySQL error code*/
static int CdeAlterTableSpaceAttrs(st_alter_tablespace *alter_info,
                                   const dd::Tablespace *old_dd_space,
                                   dd::Tablespace *new_dd_space) {
  DBUG_TRACE;

  /* now our tablespace only support change name */
  if (alter_info->ts_alter_tablespace_type != ALTER_TABLESPACE_RENAME) {
    return HA_ADMIN_NOT_IMPLEMENTED;
  }

  DSTORE::TablespaceId spaceId = 0;
  /* Be sure that this tablespace is known and valid. */
  if (old_dd_space->se_private_data().get(ddSpaceKeyStrings[DD_SPACE_ID],
                                          &spaceId) ||
      spaceId == 0) {
    return HA_ERR_TABLESPACE_MISSING;
  }

  /* ALTER_TABLESPACE_RENAME */
  const char *from = old_dd_space->name().c_str();
  const char *to = new_dd_space->name().c_str();

  if (0 == strcmp(from, dstore_temp_spacename) ||
      0 == strcmp(from, dstore_default_spacename) ||
      0 == strcmp(from, dstore_global_spacename)) {
    /* Allow these names if the caller is putting a
    table into one of these by CREATE/ALTER TABLE */
    my_printf_error(ER_WRONG_TABLESPACE_NAME,
                    "dstore: %s is a reserved tablespace name.", MYF(0), from);
    return HA_WRONG_CREATE_OPTION;
  }

  return tablespaceMgr.UpdateTableSpaceName(from, to);
}

bool CdeIsValidTablespaceName(ts_command_type ts_cmd, const char *name) {
  int err = 0;

  if (ts_cmd == DROP_TABLESPACE &&
      0 == strcmp(name, dstore_default_spacename)) {
    return true;
  }

  /* This prefix is reserved by dstore for use in internal tablespace
  names. */
  const char reserved_space_name_prefix[] = "dstore_";

  /* Validation at the SQL layer should already be completed at this
  stage.  Re-assert that the length is valid. */
  if (validate_tablespace_name_length(name)) {
    my_printf_error(ER_WRONG_TABLESPACE_NAME,
                    "DStore: Tablespace name `%s` is too long.", MYF(0), name);
    return HA_WRONG_CREATE_OPTION;
  }

  /* The tablespace name cannot start with `dstore_`. */
  if (strlen(name) >= sizeof(reserved_space_name_prefix) - 1 &&
      0 == memcmp(name, reserved_space_name_prefix,
                  sizeof(reserved_space_name_prefix) - 1)) {
    /* Use a different message for reserved names */
    if (0 == strcmp(name, dstore_temp_spacename) ||
        0 == strcmp(name, dstore_default_spacename) ||
        0 == strcmp(name, dstore_global_spacename)) {
      /* Allow these names if the caller is putting a
      table into one of these by CREATE/ALTER TABLE */
      if (ts_cmd != TS_CMD_NOT_DEFINED) {
        my_printf_error(ER_WRONG_TABLESPACE_NAME,
                        "DStore: `%s` is a reserved"
                        " DStore's tablespace name.",
                        MYF(0), name);
        err = HA_WRONG_CREATE_OPTION;
      }
    } else {
      my_printf_error(ER_WRONG_TABLESPACE_NAME,
                      "DStore: Tablespace names starting"
                      " with `%s` are reserved.",
                      MYF(0), reserved_space_name_prefix);
      err = HA_WRONG_CREATE_OPTION;
    }
  }

  return (0 == err);
}

int CdeAlterTablespace(handlerton *, THD *thd, st_alter_tablespace *alter_info,
                       const dd::Tablespace *old_ts_def,
                       dd::Tablespace *new_ts_def) {
  int error = 0; /* return zero for success */
  DBUG_TRACE;

  switch (alter_info->ts_cmd_type) {
    case CREATE_TABLESPACE:
      error = CdeCreateTableSpace(thd, alter_info, new_ts_def);
      break;
    case ALTER_TABLESPACE:
      error = CdeAlterTableSpaceAttrs(alter_info, old_ts_def, new_ts_def);
      break;
    case DROP_TABLESPACE:
      error = CdeDropTableSpace(thd, alter_info, old_ts_def);
      break;
    default:
      error = HA_ADMIN_NOT_IMPLEMENTED;
  }

  if (error) {
    /* These are the most common message params */
    const char *ibd_type = "TABLESPACE";
    const char *undo_type = "UNDO TABLESPACE";
    uint32_t code = ER_CREATE_FILEGROUP_FAILED;
    const char *subject = ibd_type;

    /* Modify those params as needed. */
    switch (alter_info->ts_cmd_type) {
      case CREATE_TABLESPACE:
        break;
      case ALTER_TABLESPACE:
        code = ER_ALTER_FILEGROUP_FAILED;
        break;
      case DROP_TABLESPACE:
        code = ER_DROP_FILEGROUP_FAILED;
        break;
      case CREATE_UNDO_TABLESPACE:
        subject = undo_type;
        break;
      case ALTER_UNDO_TABLESPACE:
        code = ER_ALTER_FILEGROUP_FAILED;
        subject = undo_type;
        break;
      case DROP_UNDO_TABLESPACE:
        code = ER_DROP_FILEGROUP_FAILED;
        subject = undo_type;
        break;
      case CREATE_LOGFILE_GROUP:
      case DROP_LOGFILE_GROUP:
      case ALTER_LOGFILE_GROUP:
        subject = "LOGFILE GROUP";
        break;
      case ALTER_ACCESS_MODE_TABLESPACE:
        subject = "ACCESS MODE";
        break;
      case CHANGE_FILE_TABLESPACE:
        subject = "CHANGE FILE";
        break;
      case TS_CMD_NOT_DEFINED:
        subject = "UNKNOWN";
        break;
    }

    switch (alter_info->ts_cmd_type) {
      case CREATE_TABLESPACE:
        if (error == ER_TABLESPACE_MAXSIZE_INVALID ||
            error == ER_TABLESPACEID_USED_UP) {
          my_error(error, MYF(0), subject, alter_info->tablespace_name);
          error = HA_ERR_GENERIC;
        } else {
          my_error(code, MYF(0), subject, alter_info->tablespace_name);
        }
        break;
      case ALTER_TABLESPACE:
      case DROP_TABLESPACE:
        my_error(code, MYF(0), subject, alter_info->tablespace_name);
        break;
      case CREATE_UNDO_TABLESPACE:
      case ALTER_UNDO_TABLESPACE:
      case DROP_UNDO_TABLESPACE:
      case CREATE_LOGFILE_GROUP:
      case ALTER_ACCESS_MODE_TABLESPACE:
      case DROP_LOGFILE_GROUP:
      case ALTER_LOGFILE_GROUP:
      case CHANGE_FILE_TABLESPACE:
      case TS_CMD_NOT_DEFINED:
        my_error(ER_FEATURE_UNSUPPORTED, MYF(0), subject, "by DStore");
        break;
    }
  }

  return error;
}

void TableSpaceMgrInit() { tablespaceMgr.Init(); }

DSTORE::TablespaceId CdeGetSpaceIdByName(const char *spaceName) {
  if (!spaceName) return CDE_DEFAULT_TABLE_SPACE_ID;

  return tablespaceMgr.GetSpaceIdByName(spaceName);
}

template <typename Table>
DSTORE::TablespaceId CdeGetSpaceIdByDD(const Table *table) {
  /** cause we record tablespace id in private data before,
  now we use it first, later we may switch to use dd_table->tablespace_id();
  */
  DSTORE::TablespaceId spaceId = CDE_DEFAULT_TABLE_SPACE_ID;
  CDE_ASSERT(!table->se_private_data().get(object_reltablespace, &spaceId));
  return spaceId;
}

template DSTORE::TablespaceId CdeGetSpaceIdByDD<dd::Table>(
    const dd::Table *table);
template DSTORE::TablespaceId CdeGetSpaceIdByDD<dd::Partition>(
    const dd::Partition *table);

bool DeleteTableSpace(DSTORE::TablespaceId spaceId) {
  uint8_t isRemain = TableSpace_Interface::IsRemainTablespace(
      DSTORE::g_defaultPdbId, spaceId, false);
  if (isRemain != DSTORE::REMAIN_TABLESPACE) {
    return CDE_FAIL;
  }

  uint64_t ddlXid = 0;
  if (spaceId == CDE_DEFAULT_TABLE_SPACE_ID) {
    DSTORE::FileId *fileIds = nullptr;
    uint32_t fileCount = 0;
    TableSpace_Interface::GetFileIdsByTablespaceId(
        DSTORE::g_defaultPdbId, spaceId, &fileIds, fileCount);
    DSTORE::RetStatus status = DSTORE::DSTORE_SUCC;
    if (fileCount > 0) {
      status = TableSpace_Interface::BatchFreeAndRemoveDataFile(
          DSTORE::g_defaultPdbId, spaceId, fileIds, fileCount, ddlXid);
      CDE_ASSERT(status == DSTORE::DSTORE_SUCC);
      uint64_t fileIdCnt = 0;
      status = TableSpace_Interface::BatchAllocAndAddDataFile(
          DSTORE::g_defaultPdbId, spaceId, fileIds, &fileIdCnt, true);
      CDE_ASSERT(status == DSTORE::DSTORE_SUCC);
    }
  } else {
    TableSpace_Interface::FreeRemainTablespace(DSTORE::g_defaultPdbId, spaceId,
                                               false, ddlXid);
    tablespaceMgr.DeleteTableSpace(spaceId);
  }

  return CDE_SUCC;
}

void LoadAllCreatedSpace(THD *thd, HandleTableSpaceCallback callback) {
  Table_ref tablespaceTbls[] = {Table_ref("mysql", "tablespaces", TL_READ)};
  if (open_trans_system_tables_for_read(thd, tablespaceTbls)) {
    CDE_LOG_ERROR("open mysql tablespaces table failed");
    return;
  }

  const auto close_tables =
      create_scope_guard([thd]() { close_trans_system_tables(thd); });

  TABLE *tablespaceTbl = tablespaceTbls[0].table;

  if (tablespaceTbl->file->ha_rnd_init(true)) return;

  auto rnd_cleanup = create_scope_guard(
      [tablespaceTbl]() { tablespaceTbl->file->ha_rnd_end(); });

  tablespaceTbl->use_all_columns();
  int res = tablespaceTbl->file->ha_rnd_next(tablespaceTbl->record[0]);

  String buffer;
  while (res == 0) {
    String *engineType =
        tablespaceTbl->field[INDEX_ENGINE_TYPE]->val_str(&buffer);
    std::string engine(engineType->ptr(), engineType->length());
    if (CdeStrcasecmp(engine.c_str(), DSTORE_ENGINE_NAME) == 0) {
      String *privateData =
          tablespaceTbl->field[INDEX_PRIVATE_DATA]->val_str(&buffer);
      std::unique_ptr<dd::Properties> spaceProperties(
          dd::Properties::parse_properties(
              {privateData->c_ptr(), privateData->length()}));
      uint64_t maxSize;
      spaceProperties->get(ddSpaceKeyStrings[DD_MAX_SIZE], &maxSize);
      DSTORE::TablespaceId spaceId;
      spaceProperties->get(ddSpaceKeyStrings[DD_SPACE_ID], &spaceId);

      String *name = tablespaceTbl->field[INDEX_NAME]->val_str(&buffer);
      std::string spaceName(name->ptr(), name->length());
      callback(spaceName.c_str(), maxSize, spaceId);
    }

    res = tablespaceTbl->file->ha_rnd_next(tablespaceTbl->record[0]);
  }
}

}  // namespace CDE
