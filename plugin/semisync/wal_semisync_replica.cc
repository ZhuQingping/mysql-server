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

#include "plugin/semisync/wal_semisync_replica.h"

#include <assert.h>
#include <sys/types.h>

#include "my_byteorder.h"
#include "my_dbug.h"
#include "mysql.h"
#include "sql/current_thd.h"
#include "sql/debug_sync.h"
#include "sql/rpl_wal_common.h"

bool rpl_semi_sync_replica_enabled_wal;
char rpl_semi_sync_replica_status_wal = 0;
unsigned long rpl_semi_sync_replica_trace_level_wal;

int Wal_repl_semi_sync_slave::init_object() {
  int result = 0;
  const char *kWho = "Wal_repl_semi_sync_slave::init_object";

  if (init_done_) {
    LogErr(WARNING_LEVEL, ER_SEMISYNC_FUNCTION_CALLED_TWICE, kWho);
    return 1;
  }
  init_done_ = true;

  /* References to the parameter works after set_options(). */
  set_slave_enabled(rpl_semi_sync_replica_enabled_wal);
  set_trace_level(rpl_semi_sync_replica_trace_level_wal);

  return result;
}

int Wal_repl_semi_sync_slave::slave_read_sync_header(
    const char *header, unsigned long total_len, bool *need_reply,
    const char **payload, unsigned long *payload_len) {
  const char *kWho = "Wal_repl_semi_sync_slave::slave_read_sync_header";
  int read_res = 0;
  function_enter(kWho);

  if ((unsigned char)(header[0]) == kPacketMagicNum &&
      DBUG_EVALUATE_IF("missing_magic_number", 0, 1)) {
    *need_reply = (header[1] & kPacketFlagSync);
    *payload_len = total_len - 2;
    *payload = header + 2;

    if (trace_level_ & kTraceDetail)
      LogErr(INFORMATION_LEVEL, ER_SEMISYNC_REPLICA_REPLY, kWho, *need_reply);
  } else {
    LogErr(ERROR_LEVEL, ER_SEMISYNC_MISSING_MAGIC_NO_FOR_SEMISYNC_PKT,
           total_len);
    read_res = -1;
  }

  return function_exit(kWho, read_res);
}

int Wal_repl_semi_sync_slave::slave_wal_start(Wallog_IO_param *param) {
  bool semi_sync = get_slave_enabled();

  LogErr(INFORMATION_LEVEL, ER_WAL_RPL_REPLICA_START,
         semi_sync ? "semi-sync" : "asynchronous", param->user, param->host,
         param->port, param->flushed_lsn);

  DBUG_EXECUTE_IF("rpl_semisync_slave_wal_start_failed", { return 1; };);

  if (semi_sync && !rpl_semi_sync_replica_status_wal)
    rpl_semi_sync_replica_status_wal = 1;
  return 0;
}

int Wal_repl_semi_sync_slave::slave_wal_stop(Wallog_IO_param *) {
  if (rpl_semi_sync_replica_status_wal) rpl_semi_sync_replica_status_wal = 0;
  if (mysql_reply) mysql_close(mysql_reply);
  mysql_reply = nullptr;
  return 0;
}

int Wal_repl_semi_sync_slave::slave_wal_reply(MYSQL *mysql, uint32_t server_id,
                                              uint64_t received_lsn,
                                              uint64_t flushed_lsn,
                                              uint64_t replayed_lsn) {
  const char *kWho = "Wal_repl_semi_sync_slave::slave_wal_reply";
  NET *net = &mysql->net;
  uchar reply_buffer[sizeof(Rpl_wal_ack_msg)];
  int reply_res;

  function_enter(kWho);

  DBUG_EXECUTE_IF("rpl_semisync_before_send_ack", {
    const char act[] = "now SIGNAL sending_ack WAIT_FOR continue";
    assert(opt_debug_sync_timeout > 0);
    assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  };);

  /* Prepare the buffer of the reply. */
  Rpl_wal_ack_msg ack_msg;
  ack_msg.common_header.type = RplWalMsgType::RPL_WAL_ACK_MSG;
  ack_msg.common_header.version = rpl_wal_default_version;
  ack_msg.common_header.flags = 0;
  ack_msg.common_header.server_id = server_id;
  ack_msg.received_lsn = received_lsn;
  ack_msg.flushed_lsn = flushed_lsn;
  ack_msg.replayed_lsn = replayed_lsn;
  ack_msg.serialize(reply_buffer);

  net_clear(net, false);
  /* Send the reply. */
  reply_res = my_net_write(net, reply_buffer, sizeof(reply_buffer));
  if (!reply_res && DBUG_EVALUATE_IF("replica_send_reply_failed", 0, 1)) {
    reply_res = net_flush(net);
    if (reply_res || DBUG_EVALUATE_IF("replica_net_flush_reply_failed", 1, 0))
      LogErr(ERROR_LEVEL, ER_WAL_RPL_REPLICA_NET_FLUSH_REPLY_FAILED);
  } else {
    LogErr(ERROR_LEVEL, ER_WAL_RPL_REPLICA_SEND_REPLY_FAILED, net->last_error,
           net->last_errno);
  }

  /**
    The progress of the internal state of the NET object differs a bit between
    compressed and non-compressed protocol. For compressed protocol, it is
    necessary to call net_clear when switching between reading and writing.
    For non-compressed protocol, it does not work when we call net_clear here.
  */
  if (net->compress) net_clear(net, false);
  return function_exit(kWho, reply_res);
}
