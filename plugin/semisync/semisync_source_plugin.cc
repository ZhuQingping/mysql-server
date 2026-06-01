/* Copyright (C) 2007 Google Inc.
   Copyright (c) 2008, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <stddef.h>
#include <sys/types.h>

#include "my_inttypes.h"
#include "my_macros.h"
#include "my_psi_config.h"
#include "mysql/psi/mysql_memory.h"
#include "mysql/psi/mysql_stage.h"
#include "plugin/semisync/semisync_source.h"
#include "plugin/semisync/semisync_source_ack_receiver.h"
#include "plugin/semisync/wal_semisync_source.h"
#include "sql/current_thd.h"
#include "sql/derror.h"  // ER_THD
#include "sql/mysqld.h"
#include "sql/protocol_classic.h"
#include "sql/raii/sentry.h"     // raii::Sentry
#include "sql/rpl_wal_common.h"  // RPL_FLAGS_REPLICA_WAL
#include "sql/sql_class.h"       // THD
#include "sql/sql_lex.h"         // thd->lex
#include "typelib.h"

#ifdef USE_OLD_SEMI_SYNC_TERMINOLOGY
#define SOURCE_NAME "master"
#define REPLICA_NAME "slave"
#define SEMI_SYNC_PLUGIN_NAME "rpl_semi_sync_master"
#define OTHER_SEMI_SYNC_PLUGIN_NAME "rpl_semi_sync_source"
#define WAIT_NO_REPLICA_NAME wait_no_slave
#define WAIT_NO_REPLICA_NAME_WAL wait_no_slave_wal
#define WAIT_FOR_REPLICA_COUNT_NAME wait_for_slave_count
#define STATUS_VAR_PREFIX "Rpl_semi_sync_master_"
#define DEPRECATED_SEMISYNC_LIBRARY
#else
#define SOURCE_NAME "source"
#define REPLICA_NAME "replica"
#define SEMI_SYNC_PLUGIN_NAME "rpl_semi_sync_source"
#define OTHER_SEMI_SYNC_PLUGIN_NAME "rpl_semi_sync_master"
#define WAIT_NO_REPLICA_NAME wait_no_replica
#define WAIT_NO_REPLICA_NAME_WAL wait_no_replica_wal
#define WAIT_FOR_REPLICA_COUNT_NAME wait_for_replica_count
#define STATUS_VAR_PREFIX "Rpl_semi_sync_source_"
#endif

ReplSemiSyncMaster *repl_semisync = nullptr;
Wal_repl_semi_sync_master *wal_repl_semisync = nullptr;
Ack_receiver *ack_receiver = nullptr;
Ack_receiver *wal_ack_receiver = nullptr;

/* The places at where semisync waits for binlog ACKs. */
enum enum_wait_point { WAIT_AFTER_SYNC, WAIT_AFTER_COMMIT };

static ulong rpl_semi_sync_source_wait_point = WAIT_AFTER_COMMIT;

thread_local bool THR_RPL_SEMI_SYNC_DUMP = false;
thread_local bool THR_RPL_SEMI_SYNC_WAL_DUMP = false;

static SERVICE_TYPE(registry) *reg_srv = nullptr;
SERVICE_TYPE(log_builtins) *log_bi = nullptr;
SERVICE_TYPE(log_builtins_string) *log_bs = nullptr;

/*
  The value of rpl_semi_sync_source_timeout_wal needs to be smaller than
  dstore_lock_wait_timeout by 10s.
*/
const int source_timeout_lower_than_lock_timeout = 10000; /* 10000ms */
static int check_rpl_semi_sync_source_timeout_wal(THD *thd, SYS_VAR *var,
                                                  void *save,
                                                  struct st_mysql_value *value);
static inline bool is_semi_sync_dump() { return THR_RPL_SEMI_SYNC_DUMP; }
static inline bool is_semi_sync_wal_dump() {
  return THR_RPL_SEMI_SYNC_WAL_DUMP;
}

static int repl_semi_report_binlog_update(Binlog_storage_param *,
                                          const char *log_file,
                                          my_off_t log_pos) {
  int error = 0;

  if (repl_semisync->getMasterEnabled()) {
    /*
      Let us store the binlog file name and the position, so that
      we know how long to wait for the binlog to the replicated to
      the slave in synchronous replication.
    */
    error = repl_semisync->writeTranxInBinlog(log_file, log_pos);
  }

  return error;
}

static int repl_semi_report_binlog_sync(Binlog_storage_param *,
                                        const char *log_file,
                                        my_off_t log_pos) {
  if (rpl_semi_sync_source_wait_point == WAIT_AFTER_SYNC)
    return repl_semisync->commitTrx(log_file, log_pos);
  return 0;
}

static int repl_semi_wal_wait(uint64_t lsn) {
  int error = 0;
  if (wal_repl_semisync->get_master_enabled()) {
    error = wal_repl_semisync->wait_standby_flush(lsn);
  }
  return error;
}

static int repl_semi_get_standby_flushed_lsn(uint64_t *ptr) {
  if (wal_repl_semisync->get_master_enabled()) {
    wal_repl_semisync->get_standby_flushed_lsn(ptr);
    return 0;
  }
  *ptr = UINT64_MAX;
  return 0;
}

static int repl_semi_report_before_dml(Trans_param *, int &) { return 0; }

static int repl_semi_report_before_commit(Trans_param *) { return 0; }

static int repl_semi_report_before_rollback(Trans_param *) { return 0; }

static int repl_semi_report_commit(Trans_param *param) {
  bool is_real_trans = param->flags & TRANS_IS_REAL_TRANS;

  if (rpl_semi_sync_source_wait_point == WAIT_AFTER_COMMIT && is_real_trans &&
      param->log_pos) {
    const char *binlog_name = param->log_file;
    return repl_semisync->commitTrx(binlog_name, param->log_pos);
  }
  return 0;
}

static int repl_semi_report_rollback(Trans_param *param) {
  return repl_semi_report_commit(param);
}

static int repl_semi_report_begin(Trans_param *, int &) { return 0; }

static int repl_semi_binlog_dump_start(Binlog_transmit_param *param,
                                       const char *log_file, my_off_t log_pos) {
  long long semi_sync_slave = 0;

  /*
    Check if the replica has identified itself as a semisync replica
    by setting a user variable.  The old library sets the user
    variable rpl_semi_sync_slave on the session, and the new library
    sets rpl_semi_sync_replica.  The value returned through the
    argument will be whatever the replica has set it to in the
    session, or 0 if the replica has not set it.
  */
  get_user_var_int("rpl_semi_sync_replica", &semi_sync_slave, nullptr);
  if (semi_sync_slave == 0)
    get_user_var_int("rpl_semi_sync_slave", &semi_sync_slave, nullptr);

  if (semi_sync_slave != 0) {
    if (ack_receiver->add_slave(current_thd, param->flags)) {
      LogErr(ERROR_LEVEL, ER_SEMISYNC_FAILED_REGISTER_REPLICA_TO_RECEIVER);
      return -1;
    }

    THR_RPL_SEMI_SYNC_DUMP = true;

    /* One more semi-sync slave */
    repl_semisync->add_slave();

    /* Tell server it will observe the transmission.*/
    param->set_observe_flag();

    /*
      Let's assume this semi-sync slave has already received all
      binlog events before the filename and position it requests.
    */
    repl_semisync->handleAck(param->server_id, log_file, log_pos);
  } else
    param->set_dont_observe_flag();

  LogErr(INFORMATION_LEVEL, ER_SEMISYNC_START_BINLOG_DUMP_TO_REPLICA,
         semi_sync_slave != 0 ? "semi-sync" : "asynchronous", param->server_id,
         log_file, (unsigned long)log_pos);
  return 0;
}

static int repl_semi_binlog_dump_end(Binlog_transmit_param *param) {
  bool semi_sync_slave = is_semi_sync_dump();

  LogErr(INFORMATION_LEVEL, ER_SEMISYNC_STOP_BINLOG_DUMP_TO_REPLICA,
         semi_sync_slave ? "semi-sync" : "asynchronous", param->server_id);

  if (semi_sync_slave) {
    ack_receiver->remove_slave(current_thd);
    /* One less semi-sync slave */
    repl_semisync->remove_slave();
    THR_RPL_SEMI_SYNC_DUMP = false;
  }
  return 0;
}

static int repl_semi_reserve_header(Binlog_transmit_param *,
                                    unsigned char *header, unsigned long size,
                                    unsigned long *len) {
  if (is_semi_sync_dump())
    *len += repl_semisync->reserveSyncHeader(header, size);
  return 0;
}

static int repl_semi_before_send_event(Binlog_transmit_param *param,
                                       unsigned char *packet, unsigned long,
                                       const char *log_file, my_off_t log_pos) {
  if (!is_semi_sync_dump()) return 0;

  return repl_semisync->updateSyncHeader(packet, log_file, log_pos,
                                         param->server_id);
}

static int repl_semi_after_send_event(Binlog_transmit_param *param,
                                      const char *event_buf, unsigned long,
                                      const char *skipped_log_file,
                                      my_off_t skipped_log_pos) {
  if (is_semi_sync_dump()) {
    if (skipped_log_pos > 0)
      repl_semisync->skipSlaveReply(event_buf, param->server_id,
                                    skipped_log_file, skipped_log_pos);
    else {
      THD *thd = current_thd;
      /*
        Possible errors in reading slave reply are ignored deliberately
        because we do not want dump thread to quit on this. Error
        messages are already reported.
      */
      (void)repl_semisync->readSlaveReply(
          thd->get_protocol_classic()->get_net(), event_buf);
      thd->clear_error();
    }
  }
  return 0;
}

static int repl_semi_reset_master(Binlog_transmit_param *) {
  if (repl_semisync->resetMaster()) return 1;
  return 0;
}

static int repl_semi_wallog_dump_start(Wallog_transmit_param *param,
                                       uint64 log_pos) {
  long long semi_sync_slave = 0;

  /*
    Check if the replica has identified itself as a semisync replica
    by setting a user variable.  The old library sets the user
    variable rpl_semi_sync_slave_wal on the session, and the new library
    sets rpl_semi_sync_replica_wal.  The value returned through the
    argument will be whatever the replica has set it to in the
    session, or 0 if the replica has not set it.
  */
  get_user_var_int("rpl_semi_sync_replica_wal", &semi_sync_slave, nullptr);
  if (semi_sync_slave == 0)
    get_user_var_int("rpl_semi_sync_slave_wal", &semi_sync_slave, nullptr);

  if (semi_sync_slave != 0) {
    if (wal_ack_receiver->add_slave(current_thd, RPL_FLAGS_REPLICA_WAL)) {
      LogErr(ERROR_LEVEL, ER_WAL_RPL_FAILED_REGISTER_REPLICA_TO_RECEIVER);
      return -1;
    }

    THR_RPL_SEMI_SYNC_WAL_DUMP = true;

    /* One more semi-sync slave */
    wal_repl_semisync->add_slave();

    /* Tell server it will observe the transmission.*/
    param->set_observe_flag();

    /*
      Let's assume this semi-sync slave has already received all
      binlog events before the filename and position it requests.
    */
    wal_repl_semisync->handle_wal_ack(param->server_id, log_pos);
  } else
    param->set_dont_observe_flag();

  LogErr(INFORMATION_LEVEL, ER_WAL_RPL_START_WALLOG_DUMP_TO_REPLICA,
         semi_sync_slave != 0 ? "semi-sync" : "asynchronous", param->server_id,
         (unsigned long)log_pos);
  return 0;
}

static int repl_semi_wallog_dump_end(Wallog_transmit_param *param) {
  bool semi_sync_slave = is_semi_sync_wal_dump();

  LogErr(INFORMATION_LEVEL, ER_WAL_RPL_STOP_WALLOG_DUMP_TO_REPLICA,
         semi_sync_slave ? "semi-sync" : "asynchronous", param->server_id);

  if (semi_sync_slave) {
    wal_ack_receiver->remove_slave(current_thd);
    /* One less semi-sync slave */
    wal_repl_semisync->remove_slave();
    THR_RPL_SEMI_SYNC_WAL_DUMP = false;
  }
  return 0;
}

static int repl_semi_wallog_reserve_header(Wallog_transmit_param *,
                                           unsigned char *header,
                                           unsigned long size,
                                           unsigned long *len) {
  if (is_semi_sync_wal_dump())
    *len += wal_repl_semisync->reserve_sync_header(header, size);
  return 0;
}

static int repl_semi_wallog_before_send_event(Wallog_transmit_param *param,
                                              unsigned char *packet,
                                              unsigned long, uint64 log_pos) {
  DBUG_EXECUTE_IF("wal_sender_set_sync_failed", return 1;);
  if (!is_semi_sync_wal_dump()) return 0;

  return wal_repl_semisync->set_master_sync_bit(packet, log_pos,
                                                param->server_id);
}

static int repl_semi_wallog_after_send_event(Wallog_transmit_param *param
                                             [[maybe_unused]],
                                             const char *event_buf) {
  DBUG_EXECUTE_IF("wal_sender_net_flush_failed", return 1;);
  if (is_semi_sync_wal_dump()) {
    THD *thd = current_thd;
    /*
      Possible errors in reading slave reply are ignored deliberately
      because we do not want dump thread to quit on this. Error
      messages are already reported.
    */
    (void)wal_repl_semisync->read_slave_wal_reply(
        thd->get_protocol_classic()->get_net(), event_buf);

    thd->clear_error();
  }
  return 0;
}

static int repl_semi_wallog_reset_master(Wallog_transmit_param *) {
  if (wal_repl_semisync->reset_master()) return 1;
  return 0;
}

/*
  semisync system variables
 */
static void fix_rpl_semi_sync_source_timeout(MYSQL_THD thd, SYS_VAR *var,
                                             void *ptr, const void *val);

static void fix_rpl_semi_sync_source_trace_level(MYSQL_THD thd, SYS_VAR *var,
                                                 void *ptr, const void *val);

static void fix_rpl_semi_sync_source_wait_no_replica(MYSQL_THD thd,
                                                     SYS_VAR *var, void *ptr,
                                                     const void *val);

static void fix_rpl_semi_sync_source_enabled(MYSQL_THD thd, SYS_VAR *var,
                                             void *ptr, const void *val);

static void fix_rpl_semi_sync_source_wait_for_replica_count(MYSQL_THD thd,
                                                            SYS_VAR *var,
                                                            void *ptr,
                                                            const void *val);

/*
  wal semisync system variables
 */
static void fix_rpl_semi_sync_source_timeout_wal(MYSQL_THD thd, SYS_VAR *var,
                                                 void *ptr, const void *val);

static void fix_rpl_semi_sync_source_trace_level_wal(MYSQL_THD thd,
                                                     SYS_VAR *var, void *ptr,
                                                     const void *val);

static void fix_rpl_semi_sync_source_wait_no_replica_wal(MYSQL_THD thd,
                                                         SYS_VAR *var,
                                                         void *ptr,
                                                         const void *val);

static void fix_rpl_semi_sync_source_enabled_wal(MYSQL_THD thd, SYS_VAR *var,
                                                 void *ptr, const void *val);

static void fix_rpl_semi_sync_source_wait_point(MYSQL_THD thd, SYS_VAR *var,
                                                void *ptr, const void *val);

static MYSQL_SYSVAR_BOOL(
    enabled, rpl_semi_sync_source_enabled, PLUGIN_VAR_OPCMDARG,
    "Enable semi-synchronous replication source (disabled by default). ",
    nullptr,                            // check
    &fix_rpl_semi_sync_source_enabled,  // update
    0);

static MYSQL_SYSVAR_BOOL(
    enabled_wal, rpl_semi_sync_source_enabled_wal, PLUGIN_VAR_OPCMDARG,
    "Enable wal semi-synchronous replication source (disabled by default). ",
    nullptr,                                // check
    &fix_rpl_semi_sync_source_enabled_wal,  // update
    0);

static MYSQL_SYSVAR_ULONG(
    timeout, rpl_semi_sync_source_timeout, PLUGIN_VAR_OPCMDARG,
    "The timeout value (in milliseconds) for semi-synchronous replication on "
    "the source. If less than "
    "rpl_semi_sync_" SOURCE_NAME "_wait_for_" REPLICA_NAME
    "_count "
    "replicas have replied after this amount of time, switch to asynchronous "
    "replication.",
    nullptr,                           // check
    fix_rpl_semi_sync_source_timeout,  // update
    10000, 0, ~0UL, 1);

static MYSQL_SYSVAR_ULONG(
    timeout_wal, rpl_semi_sync_source_timeout_wal, PLUGIN_VAR_OPCMDARG,
    "The timeout value (in milliseconds) for semi-synchronous replication on "
    "the source. If less than "
    "rpl_semi_sync_" SOURCE_NAME "_wait_for_" REPLICA_NAME
    "_count_wal "
    "replicas have replied after this amount of time, switch to asynchronous "
    "replication.",
    check_rpl_semi_sync_source_timeout_wal,  // check
    fix_rpl_semi_sync_source_timeout_wal,    // update
    10000, 0, ~0UL, 1);

#define DEFINE_WAIT_NO_REPLICA(NAME)                                           \
  static MYSQL_SYSVAR_BOOL(                                                    \
      NAME, rpl_semi_sync_source_wait_no_replica, PLUGIN_VAR_OPCMDARG,         \
      "If enabled, revert to asynchronous replication only if less "           \
      "than "                                                                  \
      "rpl_semi_sync_" SOURCE_NAME "_wait_for_" REPLICA_NAME                   \
      "_count "                                                                \
      "replicas have replied when "                                            \
      "rpl_semi_sync_" SOURCE_NAME                                             \
      "_timeout "                                                              \
      "seconds have passed. If disabled, revert to asynchronous "              \
      "replication also as soon as the number of connected replicas "          \
      "drops below "                                                           \
      "rpl_semi_sync_" SOURCE_NAME "_wait_for_" REPLICA_NAME "_count.",        \
      nullptr /*check*/, &fix_rpl_semi_sync_source_wait_no_replica /*update*/, \
      1);
#ifdef USE_OLD_SEMI_SYNC_TERMINOLOGY
DEFINE_WAIT_NO_REPLICA(wait_no_slave)
#else
DEFINE_WAIT_NO_REPLICA(wait_no_replica)
#endif

#define DEFINE_WAIT_NO_REPLICA_WAL(NAME)                                    \
  static MYSQL_SYSVAR_BOOL(                                                 \
      NAME, rpl_semi_sync_source_wait_no_replica_wal, PLUGIN_VAR_OPCMDARG,  \
      "If enabled, revert to asynchronous replication only if less "        \
      "than "                                                               \
      "rpl_semi_sync_" SOURCE_NAME "_wait_for_" REPLICA_NAME                \
      "_count_wal "                                                         \
      "replicas have replied when "                                         \
      "rpl_semi_sync_" SOURCE_NAME                                          \
      "_timeout_wal "                                                       \
      "seconds have passed. If disabled, revert to asynchronous "           \
      "replication also as soon as the number of connected replicas "       \
      "drops below "                                                        \
      "rpl_semi_sync_" SOURCE_NAME "_wait_for_" REPLICA_NAME "_count_wal.", \
      nullptr /*check*/,                                                    \
      &fix_rpl_semi_sync_source_wait_no_replica_wal /*update*/, 1);
#ifdef USE_OLD_SEMI_SYNC_TERMINOLOGY
DEFINE_WAIT_NO_REPLICA_WAL(wait_no_slave_wal)
#else
DEFINE_WAIT_NO_REPLICA_WAL(wait_no_replica_wal)
#endif

static MYSQL_SYSVAR_ULONG(trace_level, rpl_semi_sync_source_trace_level,
                          PLUGIN_VAR_OPCMDARG,
                          "The tracing level for semi-sync replication.",
                          nullptr,                                // check
                          &fix_rpl_semi_sync_source_trace_level,  // update
                          32, 0, ~0UL, 1);

static MYSQL_SYSVAR_ULONG(trace_level_wal, rpl_semi_sync_source_trace_level_wal,
                          PLUGIN_VAR_OPCMDARG,
                          "The tracing level for wal semi-sync replication.",
                          nullptr,                                    // check
                          &fix_rpl_semi_sync_source_trace_level_wal,  // update
                          32, 0, ~0UL, 1);

static const char *wait_point_names[] = {"AFTER_SYNC", "AFTER_COMMIT", NullS};
static TYPELIB wait_point_typelib = {array_elements(wait_point_names) - 1, "",
                                     wait_point_names, nullptr};
static MYSQL_SYSVAR_ENUM(
    wait_point,                      /* name     */
    rpl_semi_sync_source_wait_point, /* var      */
    PLUGIN_VAR_OPCMDARG,             /* flags    */
    "The semisync source plugin can wait for replica replies at one of two "
    "alternative points: AFTER_SYNC or AFTER_COMMIT. "
    "AFTER_SYNC is the default value. AFTER_SYNC means that the "
    "source-side semisynchronous plugin waits for the replies just after it "
    "has synced the binary log file (or would have synced, but may have "
    "skipped it, when sync_binlog!=1), but before it has committed in the "
    "engine on the source side. Therefore, it guarantees that no other "
    "sessions on the source can see the effects of the transaction before "
    "the replica has received it. "
    "AFTER_COMMIT means that the source-side semisynchronous plugin "
    "waits for the replies from the replica just after the source has "
    "committed the transaction in the engine, and before it sends an ACK "
    "packet to the client session. Other sessions may see the effects of "
    "the transaction before it has been replicated, even though the current "
    "session is still waiting for the replies from the replica.",
    nullptr,                             /* check()  */
    fix_rpl_semi_sync_source_wait_point, /* update() */
    WAIT_AFTER_SYNC,                     /* default  */
    &wait_point_typelib                  /* typelib  */
);

#define DEFINE_WAIT_FOR_REPLICA_COUNT(NAME)                             \
  static MYSQL_SYSVAR_UINT(                                             \
      NAME,                                        /* name  */          \
      rpl_semi_sync_source_wait_for_replica_count, /* var   */          \
      PLUGIN_VAR_OPCMDARG,                         /* flags */          \
      "The number of replicas that need to acknowledge that they have " \
      "received a transaction, before the transaction can complete on " \
      "the source.",                                                    \
      nullptr /* check */,                                              \
      &fix_rpl_semi_sync_source_wait_for_replica_count, /* update */    \
      1, 1, 65535, 1);
#ifdef USE_OLD_SEMI_SYNC_TERMINOLOGY
DEFINE_WAIT_FOR_REPLICA_COUNT(wait_for_slave_count)
#else
DEFINE_WAIT_FOR_REPLICA_COUNT(wait_for_replica_count)
#endif

static SYS_VAR *semi_sync_master_system_vars[] = {
    MYSQL_SYSVAR(enabled),
    MYSQL_SYSVAR(timeout),
    MYSQL_SYSVAR(WAIT_NO_REPLICA_NAME),
    MYSQL_SYSVAR(trace_level),
    MYSQL_SYSVAR(wait_point),
    MYSQL_SYSVAR(WAIT_FOR_REPLICA_COUNT_NAME),
    MYSQL_SYSVAR(enabled_wal),
    MYSQL_SYSVAR(timeout_wal),
    MYSQL_SYSVAR(WAIT_NO_REPLICA_NAME_WAL),
    MYSQL_SYSVAR(trace_level_wal),
    nullptr,
};
static void fix_rpl_semi_sync_source_timeout(MYSQL_THD, SYS_VAR *, void *ptr,
                                             const void *val) {
  *static_cast<unsigned long *>(ptr) = *static_cast<const unsigned long *>(val);
  repl_semisync->setWaitTimeout(rpl_semi_sync_source_timeout);
  return;
}

static int check_rpl_semi_sync_source_timeout_wal(
    THD *thd, SYS_VAR *, void *save, struct st_mysql_value *value) {
  handlerton *cde_hton = ha_resolve_by_legacy_type(thd, DB_TYPE_DSTORE);
  if (cde_hton == nullptr) {
    return 0;
  }
  int lock_wait_timeout = cde_hton->get_lock_wait_timeout(thd);
  long long new_val = 0;
  value->val_int(value, &new_val);
  if (lock_wait_timeout > source_timeout_lower_than_lock_timeout &&
      new_val + source_timeout_lower_than_lock_timeout >= lock_wait_timeout) {
    *(long long *)save = new_val;
    return 1;
  }
  *(long long *)save = new_val;
  return 0;
}

/**
 * Adjusts the semi-synchronous replication wait point on the source server.
 *
 * This function is called when the 'rpl_semi_sync_source_wait_point' system
 * variable is modified. It updates the internal wait point configuration and
 * handles special compatibility logic when switching from WAIT_AFTER_COMMIT
 * mode.
 *
 * @param thd MySQL thread handle
 * @param var System variable structure
 * @param ptr Pointer to the storage location for the wait point value
 * @param val Pointer to the new value being set
 *
 */
static void fix_rpl_semi_sync_source_wait_point(MYSQL_THD, SYS_VAR *, void *ptr,
                                                const void *val) {
  *static_cast<unsigned long *>(ptr) = *static_cast<const unsigned long *>(val);
  ulong new_val = *static_cast<const unsigned long *>(val);
  if (new_val == WAIT_AFTER_COMMIT && rds_binlog_skip_run_after_commit_hook) {
    rds_binlog_skip_run_after_commit_hook = false;
    LogErr(INFORMATION_LEVEL, ER_SEMISYNC_AFTER_COMMIT_DISABLE_SKIP_AFTER);
  } else if (new_val == WAIT_AFTER_SYNC &&
             !rds_binlog_skip_run_after_commit_hook) {
    rds_binlog_skip_run_after_commit_hook = true;
    LogErr(INFORMATION_LEVEL, ER_SEMISYNC_AFTER_COMMIT_DISABLE_SKIP_AFTER);
  }

  return;
}

static void fix_rpl_semi_sync_source_timeout_wal(MYSQL_THD, SYS_VAR *,
                                                 void *ptr, const void *val) {
  *static_cast<unsigned long *>(ptr) = *static_cast<const unsigned long *>(val);
  wal_repl_semisync->set_wait_timeout(rpl_semi_sync_source_timeout_wal);
  return;
}

static void fix_rpl_semi_sync_source_trace_level(MYSQL_THD, SYS_VAR *,
                                                 void *ptr, const void *val) {
  *static_cast<unsigned long *>(ptr) = *static_cast<const unsigned long *>(val);
  repl_semisync->setTraceLevel(rpl_semi_sync_source_trace_level);
  ack_receiver->setTraceLevel(rpl_semi_sync_source_trace_level);
  return;
}

static void fix_rpl_semi_sync_source_trace_level_wal(MYSQL_THD, SYS_VAR *,
                                                     void *ptr,
                                                     const void *val) {
  *static_cast<unsigned long *>(ptr) = *static_cast<const unsigned long *>(val);
  wal_repl_semisync->set_trace_level(rpl_semi_sync_source_trace_level_wal);
  wal_ack_receiver->setTraceLevel(rpl_semi_sync_source_trace_level_wal);
  return;
}

static void fix_rpl_semi_sync_source_enabled(MYSQL_THD, SYS_VAR *, void *ptr,
                                             const void *val) {
  *static_cast<bool *>(ptr) = *static_cast<const bool *>(val);
  if (rpl_semi_sync_source_enabled) {
    if (repl_semisync->enableMaster() != 0)
      rpl_semi_sync_source_enabled = false;
    else if (ack_receiver->start()) {
      repl_semisync->disableMaster();
      rpl_semi_sync_source_enabled = false;
    }
  } else {
    if (repl_semisync->disableMaster() != 0)
      rpl_semi_sync_source_enabled = true;
    ack_receiver->stop();
  }

  return;
}

static void fix_rpl_semi_sync_source_enabled_wal(MYSQL_THD, SYS_VAR *,
                                                 void *ptr, const void *val) {
  *static_cast<bool *>(ptr) = *static_cast<const bool *>(val);
  if (rpl_semi_sync_source_enabled_wal) {
    if (wal_repl_semisync->enable_master() != 0)
      rpl_semi_sync_source_enabled_wal = false;
    else if (wal_ack_receiver->start()) {
      wal_repl_semisync->disable_master();
      rpl_semi_sync_source_enabled_wal = false;
    }
  } else {
    if (wal_repl_semisync->disable_master() != 0)
      rpl_semi_sync_source_enabled_wal = true;
    wal_ack_receiver->stop();
  }

  return;
}

static void fix_rpl_semi_sync_source_wait_for_replica_count(MYSQL_THD,
                                                            SYS_VAR *, void *,
                                                            const void *val) {
  (void)repl_semisync->setWaitSlaveCount(
      *static_cast<const unsigned int *>(val));
}

static void fix_rpl_semi_sync_source_wait_no_replica(MYSQL_THD, SYS_VAR *,
                                                     void *ptr,
                                                     const void *val) {
  if (rpl_semi_sync_source_wait_no_replica != *static_cast<const bool *>(val)) {
    *static_cast<bool *>(ptr) = *static_cast<const bool *>(val);
    repl_semisync->set_wait_no_replica(val);
  }
}

static void fix_rpl_semi_sync_source_wait_no_replica_wal(MYSQL_THD, SYS_VAR *,
                                                         void *ptr,
                                                         const void *val) {
  if (rpl_semi_sync_source_wait_no_replica_wal !=
      *static_cast<const bool *>(val)) {
    *static_cast<bool *>(ptr) = *static_cast<const bool *>(val);
    wal_repl_semisync->set_wait_no_replica(val);
  }
}

Trans_observer trans_observer = {
    sizeof(Trans_observer),  // len

    repl_semi_report_before_dml,       // before_dml
    repl_semi_report_before_commit,    // before_commit
    repl_semi_report_before_rollback,  // before_rollback
    repl_semi_report_commit,           // after_commit
    repl_semi_report_rollback,         // after_rollback
    repl_semi_report_begin,            // begin
};

Binlog_storage_observer storage_observer = {
    sizeof(Binlog_storage_observer),  // len

    repl_semi_report_binlog_update,  // report_update
    repl_semi_report_binlog_sync,    // after_sync
};

Wallog_trx_observer wallog_trx_observer = {
    sizeof(Wallog_trx_observer),  // len

    repl_semi_wal_wait,                 // trx_wait
    repl_semi_get_standby_flushed_lsn,  // get_standby_flushed_lsn
};

Binlog_transmit_observer transmit_observer = {
    sizeof(Binlog_transmit_observer),  // len

    repl_semi_binlog_dump_start,  // start
    repl_semi_binlog_dump_end,    // stop
    repl_semi_reserve_header,     // reserve_header
    repl_semi_before_send_event,  // before_send_event
    repl_semi_after_send_event,   // after_send_event
    repl_semi_reset_master,       // reset
};

Wallog_transmit_observer wallog_transmit_observer = {
    sizeof(Wallog_transmit_observer),  // len

    repl_semi_wallog_dump_start,         // wallog_transmit_start
    repl_semi_wallog_dump_end,           // wallog_transmit_stop
    repl_semi_wallog_reserve_header,     // wallog_reserve_header
    repl_semi_wallog_before_send_event,  // wallog_before_send_event
    repl_semi_wallog_after_send_event,   // wallog_after_send_event
    repl_semi_wallog_reset_master,       // wallog_after_reset_master
};

#define SHOW_FNAME(name) rpl_semi_sync_source_show_##name

#define DEF_SHOW_FUNC(name, show_type)                             \
  static int SHOW_FNAME(name)(MYSQL_THD, SHOW_VAR * var, char *) { \
    repl_semisync->setExportStats();                               \
    wal_repl_semisync->set_export_stats();                         \
    var->type = show_type;                                         \
    var->value = (char *)&rpl_semi_sync_source_##name;             \
    return 0;                                                      \
  }

DEF_SHOW_FUNC(status, SHOW_BOOL)
DEF_SHOW_FUNC(clients, SHOW_LONG)
DEF_SHOW_FUNC(wait_sessions, SHOW_LONG)
DEF_SHOW_FUNC(trx_wait_time, SHOW_LONGLONG)
DEF_SHOW_FUNC(trx_wait_num, SHOW_LONGLONG)
DEF_SHOW_FUNC(net_wait_time, SHOW_LONGLONG)
DEF_SHOW_FUNC(net_wait_num, SHOW_LONGLONG)
DEF_SHOW_FUNC(avg_net_wait_time, SHOW_LONG)
DEF_SHOW_FUNC(avg_trx_wait_time, SHOW_LONG)
DEF_SHOW_FUNC(status_wal, SHOW_BOOL)
DEF_SHOW_FUNC(clients_wal, SHOW_LONG)
DEF_SHOW_FUNC(wait_sessions_wal, SHOW_LONG)
DEF_SHOW_FUNC(trx_wait_time_wal, SHOW_LONGLONG)
DEF_SHOW_FUNC(trx_wait_num_wal, SHOW_LONGLONG)
DEF_SHOW_FUNC(net_wait_time_wal, SHOW_LONGLONG)
DEF_SHOW_FUNC(net_wait_num_wal, SHOW_LONGLONG)
DEF_SHOW_FUNC(avg_net_wait_time_wal, SHOW_LONG)
DEF_SHOW_FUNC(avg_trx_wait_time_wal, SHOW_LONG)

/* plugin status variables */
static SHOW_VAR semi_sync_master_status_vars[] = {
    {STATUS_VAR_PREFIX "status", (char *)&SHOW_FNAME(status), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "clients", (char *)&SHOW_FNAME(clients), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "yes_tx", (char *)&rpl_semi_sync_source_yes_transactions,
     SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "no_tx", (char *)&rpl_semi_sync_source_no_transactions,
     SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "wait_sessions", (char *)&SHOW_FNAME(wait_sessions),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "no_times", (char *)&rpl_semi_sync_source_off_times,
     SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "timefunc_failures",
     (char *)&rpl_semi_sync_source_timefunc_fails, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "wait_pos_backtraverse",
     (char *)&rpl_semi_sync_source_wait_pos_backtraverse, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "tx_wait_time", (char *)&SHOW_FNAME(trx_wait_time),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "tx_waits", (char *)&SHOW_FNAME(trx_wait_num), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "tx_avg_wait_time",
     (char *)&SHOW_FNAME(avg_trx_wait_time), SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "net_wait_time", (char *)&SHOW_FNAME(net_wait_time),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "net_waits", (char *)&SHOW_FNAME(net_wait_num),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "net_avg_wait_time",
     (char *)&SHOW_FNAME(avg_net_wait_time), SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "status_wal", (char *)&SHOW_FNAME(status_wal), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "clients_wal", (char *)&SHOW_FNAME(clients_wal),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "yes_tx_wal",
     (char *)&rpl_semi_sync_source_yes_transactions_wal, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "no_tx_wal",
     (char *)&rpl_semi_sync_source_no_transactions_wal, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "wait_sessions_wal",
     (char *)&SHOW_FNAME(wait_sessions_wal), SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "no_times_wal",
     (char *)&rpl_semi_sync_source_off_times_wal, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "timefunc_failures_wal",
     (char *)&rpl_semi_sync_source_timefunc_fails_wal, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "wait_pos_backtraverse_wal",
     (char *)&rpl_semi_sync_source_wait_pos_backtraverse_wal, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "tx_wait_time_wal",
     (char *)&SHOW_FNAME(trx_wait_time_wal), SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "tx_waits_wal", (char *)&SHOW_FNAME(trx_wait_num_wal),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "tx_avg_wait_time_wal",
     (char *)&SHOW_FNAME(avg_trx_wait_time_wal), SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "net_wait_time_wal",
     (char *)&SHOW_FNAME(net_wait_time_wal), SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "net_waits_wal", (char *)&SHOW_FNAME(net_wait_num_wal),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {STATUS_VAR_PREFIX "net_avg_wait_time_wal",
     (char *)&SHOW_FNAME(avg_net_wait_time_wal), SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_LONG, SHOW_SCOPE_GLOBAL},
};

#ifdef HAVE_PSI_INTERFACE

PSI_mutex_key key_ss_mutex_LOCK_binlog_;
PSI_mutex_key key_ss_mutex_LOCK_wallog_;
PSI_mutex_key key_ss_mutex_Ack_receiver_mutex;
PSI_mutex_key key_ss_mutex_Wal_Ack_receiver_mutex;

static PSI_mutex_info all_semisync_mutexes[] = {
    {&key_ss_mutex_LOCK_binlog_, "LOCK_binlog_", 0, 0, PSI_DOCUMENT_ME},
    {&key_ss_mutex_LOCK_wallog_, "LOCK_wallog_", 0, 0, PSI_DOCUMENT_ME},
    {&key_ss_mutex_Ack_receiver_mutex, "Ack_receiver::m_mutex", 0, 0,
     PSI_DOCUMENT_ME},
    {&key_ss_mutex_Wal_Ack_receiver_mutex, "Wal_Ack_receiver::m_mutex", 0, 0,
     PSI_DOCUMENT_ME}};

PSI_cond_key key_ss_cond_COND_binlog_send_;
PSI_cond_key key_ss_cond_COND_wallog_send_;
PSI_cond_key key_ss_cond_Ack_receiver_cond;
PSI_cond_key key_ss_cond_Wal_Ack_receiver_cond;

static PSI_cond_info all_semisync_conds[] = {
    {&key_ss_cond_COND_binlog_send_, "COND_binlog_send_", 0, 0,
     PSI_DOCUMENT_ME},
    {&key_ss_cond_Ack_receiver_cond, "Ack_receiver::m_cond", 0, 0,
     PSI_DOCUMENT_ME},
    {&key_ss_cond_COND_wallog_send_, "COND_wallog_send_", 0, 0,
     PSI_DOCUMENT_ME},
    {&key_ss_cond_Wal_Ack_receiver_cond, "Wal_Ack_receiver::m_cond", 0, 0,
     PSI_DOCUMENT_ME}};

PSI_thread_key key_ss_thread_Ack_receiver_thread;
PSI_thread_key key_ss_thread_Wal_Ack_receiver_thread;

static PSI_thread_info all_semisync_threads[] = {
    {&key_ss_thread_Ack_receiver_thread, "Ack_receiver", "ss_ack",
     PSI_FLAG_SINGLETON | PSI_FLAG_THREAD_SYSTEM, 0, PSI_DOCUMENT_ME},
    {&key_ss_thread_Wal_Ack_receiver_thread, "Wal_Ack_receiver", "ss_ack",
     PSI_FLAG_SINGLETON | PSI_FLAG_THREAD_SYSTEM, 0, PSI_DOCUMENT_ME}};
#endif /* HAVE_PSI_INTERFACE */

#ifdef USE_OLD_SEMI_SYNC_TERMINOLOGY
PSI_stage_info stage_waiting_for_semi_sync_ack_from_replica = {
    0, "Waiting for semi-sync ACK from slave", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_waiting_for_semi_sync_replica = {
    0, "Waiting for semi-sync slave connection", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_reading_semi_sync_ack = {
    0, "Reading semi-sync ACK from slave", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_waiting_for_wal_semi_sync_ack_from_replica = {
    0, "Waiting for wal semi-sync ACK from slave", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_waiting_for_wal_semi_sync_replica = {
    0, "Waiting for wal semi-sync slave connection", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_reading_wal_semi_sync_ack = {
    0, "Reading wal semi-sync ACK from slave", 0, PSI_DOCUMENT_ME};
#else
PSI_stage_info stage_waiting_for_semi_sync_ack_from_replica = {
    0, "Waiting for semi-sync ACK from replica", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_waiting_for_semi_sync_replica = {
    0, "Waiting for semi-sync replica connection", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_reading_semi_sync_ack = {
    0, "Reading semi-sync ACK from replica", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_waiting_for_wal_semi_sync_ack_from_replica = {
    0, "Waiting for wal semi-sync ACK from replica", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_waiting_for_wal_semi_sync_replica = {
    0, "Waiting for wal semi-sync replica connection", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_reading_wal_semi_sync_ack = {
    0, "Reading wal semi-sync ACK from replica", 0, PSI_DOCUMENT_ME};
#endif

/* Always defined. */
PSI_memory_key key_ss_memory_TranxNodeAllocator_block;
PSI_memory_key key_ss_memory_LsnWaitSlot;

#ifdef HAVE_PSI_INTERFACE
PSI_stage_info *all_semisync_stages[] = {
    &stage_waiting_for_semi_sync_ack_from_replica,
    &stage_waiting_for_semi_sync_replica,
    &stage_reading_semi_sync_ack,
    &stage_waiting_for_wal_semi_sync_ack_from_replica,
    &stage_waiting_for_wal_semi_sync_replica,
    &stage_reading_wal_semi_sync_ack};

PSI_memory_info all_semisync_memory[] = {
    {&key_ss_memory_TranxNodeAllocator_block, "TranxNodeAllocator::block", 0, 0,
     PSI_DOCUMENT_ME},
    {&key_ss_memory_LsnWaitSlot, "LsnWaitSlot", 0, 0, PSI_DOCUMENT_ME}};

static void init_semisync_psi_keys(void) {
  const char *category = "semisync";
  int count;

  count = static_cast<int>(array_elements(all_semisync_mutexes));
  mysql_mutex_register(category, all_semisync_mutexes, count);

  count = static_cast<int>(array_elements(all_semisync_conds));
  mysql_cond_register(category, all_semisync_conds, count);

  count = static_cast<int>(array_elements(all_semisync_stages));
  mysql_stage_register(category, all_semisync_stages, count);

  count = static_cast<int>(array_elements(all_semisync_memory));
  mysql_memory_register(category, all_semisync_memory, count);

  count = static_cast<int>(array_elements(all_semisync_threads));
  mysql_thread_register(category, all_semisync_threads, count);
}
#endif /* HAVE_PSI_INTERFACE */

/**
  Return true if this is the new library and the old library is installed, or
  vice versa.

  @retval true This is semisync_master, and semisync_source is
  installed already, or this is semisync_source, and semisync_master
  is installed already.

  @retval false Otherwise
*/
static bool is_other_semi_sync_source_plugin_installed() {
  return is_sysvar_defined(OTHER_SEMI_SYNC_PLUGIN_NAME "_enabled");
}

static int semi_sync_master_plugin_init(void *p) {
  // Initialize error logging service.
  if (init_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs)) return 1;
  // Auto-deinitialize the error logging service if this function fails.
  bool success = false;
  raii::Sentry<> logging_service_guard{[&]() {
    if (!success) deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
  }};

  // Check for duplicate libraries.
  bool is_client =
      current_thd && current_thd->lex->sql_command == SQLCOM_INSTALL_PLUGIN;
  if (is_other_semi_sync_source_plugin_installed()) {
    /*
      Unfortunately, two semisync libraries don't make one sync library. :-)
      If user installs both the old-named library and the new-named
      library, we generate an error, since the two would interfere with
      each other.
    */
    if (is_client)
      my_error(ER_INSTALL_PLUGIN_CONFLICT_CLIENT, MYF(0), SEMI_SYNC_PLUGIN_NAME,
               OTHER_SEMI_SYNC_PLUGIN_NAME);
    else
      LogErr(ERROR_LEVEL, ER_INSTALL_PLUGIN_CONFLICT_LOG, SEMI_SYNC_PLUGIN_NAME,
             OTHER_SEMI_SYNC_PLUGIN_NAME);
    return 1;
  }

  if (rpl_semi_sync_source_wait_point == WAIT_AFTER_COMMIT &&
      rds_binlog_skip_run_after_commit_hook) {
    rds_binlog_skip_run_after_commit_hook = false;
    LogErr(INFORMATION_LEVEL, ER_SEMISYNC_AFTER_COMMIT_DISABLE_SKIP_AFTER);
  } else if (rpl_semi_sync_source_wait_point == WAIT_AFTER_SYNC &&
             !rds_binlog_skip_run_after_commit_hook) {
    rds_binlog_skip_run_after_commit_hook = true;
    LogErr(INFORMATION_LEVEL, ER_SEMISYNC_AFTER_SYNC_ENABLE_SKIP_AFTER);
  }

#ifdef HAVE_PSI_INTERFACE
  init_semisync_psi_keys();
#endif

#ifdef DEPRECATED_SEMISYNC_LIBRARY
  /*
    This function can be invoked in two contexts: either from the SQL
    statement INSTALL PLUGIN executed by a client, or during server
    startup, for example, in case --plugin-load is used.

    For INSTALL PLUGIN, return a warning to the client, so the person
    that issued INSTALL PLUGIN gets notified.

    In both cases, write a warning to the log, because the
    administrator needs to know that we are using an old library and
    make the new library available if it is not.
  */
  if (is_client)
    push_warning_printf(current_thd, Sql_condition::SL_NOTE,
                        ER_WARN_DEPRECATED_SYNTAX,
                        ER_THD(current_thd, ER_WARN_DEPRECATED_SYNTAX),
                        "rpl_semi_sync_master", "rpl_semi_sync_source");
  LogErr(WARNING_LEVEL, ER_DEPRECATE_MSG_WITH_REPLACEMENT,
         "rpl_semi_sync_master", "rpl_semi_sync_source");
#endif

  THR_RPL_SEMI_SYNC_DUMP = false;
  THR_RPL_SEMI_SYNC_WAL_DUMP = false;

  /*
    In case the plugin has been unloaded, and reloaded, we may need to
    re-initialize some global variables.
    These are initialized to zero by the linker, but may need to be
    re-initialized
  */
  rpl_semi_sync_source_no_transactions = 0;
  rpl_semi_sync_source_yes_transactions = 0;
  rpl_semi_sync_source_no_transactions_wal = 0;
  rpl_semi_sync_source_yes_transactions_wal = 0;

  repl_semisync = new ReplSemiSyncMaster();
  wal_repl_semisync = new Wal_repl_semi_sync_master();
  ack_receiver = new Ack_receiver();
  wal_ack_receiver = new Ack_receiver(true);

  if (repl_semisync->initObject()) return 1;
  if (wal_repl_semisync->init_object()) return 1;
  if (ack_receiver->init()) return 1;
  if (wal_ack_receiver->init()) return 1;
  if (register_trans_observer(&trans_observer, p)) return 1;
  if (register_binlog_storage_observer(&storage_observer, p)) return 1;
  if (register_wallog_trx_observer(&wallog_trx_observer, p)) return 1;
  if (register_binlog_transmit_observer(&transmit_observer, p)) return 1;
  if (register_wallog_transmit_observer(&wallog_transmit_observer, p)) return 1;

  success = true;
  return 0;
}

static int semi_sync_source_plugin_check_uninstall(void *) {
  int ret = (rpl_semi_sync_source_clients || rpl_semi_sync_source_clients_wal)
                ? 1
                : 0;
  if (ret) {
    my_error(ER_PLUGIN_CANNOT_BE_UNINSTALLED, MYF(0), SEMI_SYNC_PLUGIN_NAME,
             "Stop any active semisynchronous replicas of this source first.");
  }
  return ret;
}

static int semi_sync_master_plugin_deinit(void *p) {
  // the plugin was not initialized, there is nothing to do here
  if (ack_receiver == nullptr || repl_semisync == nullptr ||
      wal_repl_semisync == nullptr || wal_ack_receiver == nullptr)
    return 0;

  THR_RPL_SEMI_SYNC_DUMP = false;
  THR_RPL_SEMI_SYNC_WAL_DUMP = false;

  if (unregister_trans_observer(&trans_observer, p)) {
    LogErr(ERROR_LEVEL, ER_SEMISYNC_UNREGISTER_TRX_OBSERVER_FAILED);
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  }
  if (unregister_binlog_storage_observer(&storage_observer, p)) {
    LogErr(ERROR_LEVEL, ER_SEMISYNC_UNREGISTER_BINLOG_STORAGE_OBSERVER_FAILED);
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  }
  if (unregister_wallog_trx_observer(&wallog_trx_observer, p)) {
    LogErr(ERROR_LEVEL, ER_WAL_RPL_UNREGISTER_WALLOG_TRX_OBSERVER_FAILED);
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  }
  if (unregister_binlog_transmit_observer(&transmit_observer, p)) {
    LogErr(ERROR_LEVEL, ER_SEMISYNC_UNREGISTER_BINLOG_TRANSMIT_OBSERVER_FAILED);
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  }
  if (unregister_wallog_transmit_observer(&wallog_transmit_observer, p)) {
    LogErr(ERROR_LEVEL, ER_WAL_RPL_UNREGISTER_WALLOG_TRANSMIT_OBSERVER_FAILED);
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  }
  delete ack_receiver;
  ack_receiver = nullptr;
  delete wal_ack_receiver;
  wal_ack_receiver = nullptr;
  delete repl_semisync;
  repl_semisync = nullptr;
  delete wal_repl_semisync;
  wal_repl_semisync = nullptr;

  LogErr(INFORMATION_LEVEL, ER_SEMISYNC_UNREGISTERED_REPLICATOR);
  deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
  return 0;
}

struct Mysql_replication semi_sync_master_plugin = {
    MYSQL_REPLICATION_INTERFACE_VERSION};

/*
  Plugin library descriptor
*/

mysql_declare_plugin(semi_sync_master){
    MYSQL_REPLICATION_PLUGIN,
    &semi_sync_master_plugin,
    SEMI_SYNC_PLUGIN_NAME,
    PLUGIN_AUTHOR_ORACLE,
    "Source-side semi-synchronous replication.",
    PLUGIN_LICENSE_GPL,
    semi_sync_master_plugin_init,            /* Plugin Init */
    semi_sync_source_plugin_check_uninstall, /* Plugin Check uninstall */
    semi_sync_master_plugin_deinit,          /* Plugin Deinit */
    0x0100 /* 1.0 */,
    semi_sync_master_status_vars, /* status variables */
    semi_sync_master_system_vars, /* system variables */
    nullptr,                      /* config options */
    0,                            /* flags */
} mysql_declare_plugin_end;
