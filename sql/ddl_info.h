
#ifndef DDL_INFO_H_INCLUDED
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
#define DDL_INFO_H_INCLUDED

#include "sql/binlog.h"
#include "sql/log.h"

extern thread_local THD *replay_thd;

class Alter_table_ctx;

/* max len of ddl_info when only ddl_info in wal: 63K - 3 (1 for flag, 2 for
 * ddl_info_len) */
static constexpr size_t MAX_DDL_INFO_STR_LEN = MAX_DDL_INFO_SIZE - 3;
/* max len of ddl_info allowed when binlog in redo. Typically, the ddl_info_len
is 1.5 to 2 times trx_len. The entire buffer size is 63K. Here, we take the
lower bound of the proportion ratio (i.e., 1.5 times) to calculate the maximum
allowable size of ddl_info: 63K × 1.5 / (1.5 + 1). */
static constexpr size_t MAX_DDL_INFO_LEN_WITH_BINLOG =
    (MAX_DDL_INFO_SIZE * 3) / 5;

static_assert(MAX_DDL_INFO_LEN_WITH_BINLOG <=
              std::numeric_limits<uint16_t>::max());
static_assert(MAX_DDL_INFO_STR_LEN <= std::numeric_limits<uint16_t>::max());

enum mysql_ddl_info_trace_table_field {
  MYSQL_DDL_INFO_TRACE_FIELD_END_LSN = 0,
  MYSQL_DDL_INFO_TRACE_FIELD_DDL_INFO,
  MYSQL_DDL_INFO_TRACE_FIELD_COUNT
};

/**
  Check the configure table definition. (Borrowed code from TaurusDB)
*/
class Conf_table_intact : public Table_check_intact {
 public:
  explicit Conf_table_intact(THD *thd) : Table_check_intact(), m_thd(thd) {
    has_keys = true;
  }

 protected:
  void report_error(uint ecode, const char *fmt, ...) override
      MY_ATTRIBUTE((format(printf, 3, 4)));

 private:
  THD *m_thd;
};

/**
   Enumeration of DDL (Data Definition Language) command types.
   Used to classify different DDL operations for proper handling,
   particularly in replication scenarios.
 */
enum ddl_command_type {
  /**
     Not a DDL command or DDL command that doesn't require special handling.
     Default value indicating the operation is not a DDL command or doesn't
     require any special ACL or privilege handling.
   */
  DDLCOM_NONE = 0,

  /**
     Privilege-related DDL operation that requires ACL and grant cache reload
     on replica servers.
     Includes commands like CREATE/DROP USER, GRANT, REVOKE, etc. that modify
     user privileges and require cache invalidation on all servers.
   */
  DDLCOM_PRIVI,

  /**
     ACL-related DDL operation that requires invalidation with ACL lock on
     replicas. Includes stored procedure/function operations that need ACL
     validation but don't require full grant cache reload.
   */
  DDLCOM_ACL,

  /**
     Other types of DDL commands that don't affect privileges or ACLs directly.
     Includes standard schema modification commands like CREATE/ALTER/DROP
     TABLE, INDEX, etc. that only require standard DDL handling.
   */
  DDLCOM_OTHER
};

/**
   Determines the type of DDL (Data Definition Language) command based on SQL
   command enum.

   @param sql_command The SQL command type to classify

   @retval DDLCOM_OTHER General DDL commands (ALTER/CREATE/DROP objects)
   @retval DDLCOM_PRIVI Privilege-related commands (user/role management)
   @retval DDLCOM_ACL ACL-related commands (procedures/functions)
   @retval DDLCOM_NONE Not a DDL command
 */
ddl_command_type get_ddl_command_type(THD *thd);

/**
   Return the maximum allowed length of DDL information.

   @retval The maximum length of DDL information, considering the status of
   binlog and rds_write_binlog_into_redo.
 */
size_t max_ddl_info_len();

/**
   Generate DDL information for DStore CREATE TABLE statement.

   This function constructs a CREATE TABLE statement for replay purposes.
   It generates a query that includes a DROP TABLE IF EXISTS statement followed
   by the CREATE TABLE statement with a special comment for replay.

   @param thd Pointer to the THD object.
   @param create_info Pointer to the HA_CREATE_INFO structure containing
                      table creation information.
   @param form Pointer to the TABLE object representing the table structure.
   @param table_def Pointer to the data dictionary table object.
   @retval true Failure.
   @retval false Success.
 */
bool generate_dstore_create_ddl_info(THD *thd, HA_CREATE_INFO *create_info,
                                     TABLE *form, dd::Table *table_def);

/**
   Generate DDL information for DStore DROP TABLE/DROP DATABASE statement.

   This function constructs a DROP TABLE or DROP DATABASE statement for replay
   purposes. It generates a query with a special comment for replay.

   @param thd Pointer to the THD object.
   @retval true Failure.
   @retval false Success.
 */
bool generate_dstore_drop_ddl_info(THD *thd);

/**
   Generate DDL information for DStore ALTER TABLE/CREATE INDEX/DROP INDEX
   statement.

   This function constructs a DROP TABLE and a CREATE INDEX statement for replay
   purposes. It generates a query with a special comment for replay.

   @param thd Pointer to the THD object.
   @param create_info Pointer to the HA_CREATE_INFO structure containing
                      table creation information.
   @param old_table Pointer to the TABLE object representing the old table
   structure.
   @param table_def Pointer to the old data dictionary table object.
   @param new_table Pointer to the TABLE object representing the new table
   structure.
   @param new_table_def Pointer to the new data dictionary table object.
   @param alter_ctx  Pointer to the alter table context.
   @retval true Failure.
   @retval false Success.
 */
bool generate_dstore_alter_ddl_info(THD *thd, HA_CREATE_INFO *create_info,
                                    TABLE *old_table, dd::Table *old_table_def,
                                    TABLE *new_table, dd::Table *new_table_def,
                                    Alter_table_ctx *alter_ctx);

/**
   Replay DDL info from the buffer extracted from wal.

   This function processes DDL info stored in a buffer and executes the
   corresponding DDL operations. It ensures that the operations are executed
   only if the provided `end_lsn` is greater than the maximum LSN already
   recorded in the `ddl_info_replay_trace` table.

   @param end_lsn The end log sequence number (LSN) for the DDL operation.
   @param buffer Pointer to the buffer containing DDL info.
   @param buffer_size Size of the buffer containing DDL info.

   @retval false if the operation was successful or if the DDL info
           does not need to be replayed.
   @retval true if an error occurred during processing.
 */
bool replay_dd_query_info(uint64_t end_lsn, const unsigned char *buffer,
                          uint32_t buffer_size);

/**
   This function processes DDL info stored in a buffer and executes the
   corresponding DDL operations on the physical standby. It ensures that
   the operations are executed only if the provided end_lsn is greater than
   the maximum LSN already recorded in the ddl_info_replay_trace table.

   @param end_lsn The end log sequence number (LSN) for the DDL operation.
   @param buffer Pointer to the buffer containing DDL info.
   @param buffer_size The length of the DDL statement(s) in the buffer.
 */
void standby_replay_dd_query_info(uint64_t end_lsn, const unsigned char *buffer,
                                  uint32_t buffer_size);

/**
   Check if we should write DDL xid info.

   @param thd Pointer to the THD object representing the
              current thread.
   @retval true If DDL xid is needed.
   @retval false If DDL xid is not needed.
 */
bool need_ddl_info_xid(THD *thd);

/**
   Check if we should write DDL sql info.

   @param thd Pointer to the THD object representing the
              current thread.
   @retval true If DDL sql is needed.
   @retval false If DDL sql is not needed.
 */
bool need_ddl_info_sql(THD *thd);

/**
   Check if DDL information needs to be generated or stored.

   This function determines whether DDL information needs to be captured or
   processed based on the current thread's properties and the type of operation
   being performed.

   @param thd Pointer to the THD object representing the
              current thread.
   @retval true If DDL information needs to be processed.
   @retval false If no need to process DDL information.
 */
bool need_ddl_info(THD *thd);

/**
 * @brief Write transaction_id and DDL sql into the redo log.
 *
 * This function prepares a buffer with the transaction_id and DDL sql
 * and then commits it to the redo log using the `ha_commit_wals` function.
 *
 * @param thd The THD session object holding the transaction to commit.
 */
void write_only_ddl_info_into_redo(THD *thd);

/**
   Restore and replay DDL info from the file that has ddl_info stored.

   This function reads DDL information records from the file
   ddl_info_store_file.dat, replays each record using `replay_dd_query_info()`,
   and removes the storage file after all records have been processed. It
   ensures that DDL operations are replayed correctly during the restore
   process.
 */
void restore_replay_ddl_info();

/**
   Log DDL info that is replayed to the table "mysql.ddl_info_replay_trace".

   @param thd Pointer to the thread handle.
   @param lsn The log sequence number (LSN) associated with the DDL operation.
   @param ddl_info Pointer to the DDL info string.
   @param ddl_info_len Length of the DDL info string.

   @retval 0 if the operation was successful.
   @retval Non-zero error code otherwise.
 */
int log_ddl_info_to_trace(THD *thd, uint64_t lsn, const char *ddl_info,
                          size_t ddl_info_len);

/**
  If we use dstore as default engine and enable atomic-ddl for dstore,
  throttle the ddl concurrency to avoid log ddl fail due to alloc TD fail.
  The wait is bounded by global rds_dstore_ddl_throttle_timeout.

   @param thread_id Session id from THD::m_thread_id.
   @retval true if ddl concurrency is increased.
   @retval false if ddl concurrency is not increased.
 */
bool ddl_concurrency_throttle(THD *thd);

/**
   Correspond to ddl_concurrency_throttle, release the concurrency if
   DDL query end.

   @param thread_id Session id from THD::m_thread_id.
 */
void ddl_concurrency_release(uint64_t thread_id);

#endif /* DDL_INFO_H_INCLUDED */
