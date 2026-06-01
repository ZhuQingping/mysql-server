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

#ifndef WAL_SEMISYNC_REPLICA_H
#define WAL_SEMISYNC_REPLICA_H

#include "my_inttypes.h"
#include "plugin/semisync/semisync.h"

/**
  The extension class for the slave of wal semi-synchronous replication.
*/
class Wal_repl_semi_sync_slave : public ReplSemiSyncBase {
 public:
  Wal_repl_semi_sync_slave() : slave_enabled_(false) {}
  ~Wal_repl_semi_sync_slave() = default;

  void set_trace_level(unsigned long trace_level) {
    trace_level_ = trace_level;
  }

  /**
    Initialize this class after MySQL parameters are initialized. This
    function should be called once at bootstrap time.
  */
  int init_object();

  bool get_slave_enabled() { return slave_enabled_; }
  void set_slave_enabled(bool enabled) { slave_enabled_ = enabled; }

  /**
    A slave reads the wal semi-sync packet header and separate the metadata from
    the payload data.

    @param[in] header        packet header pointer
    @param[in] total_len     total packet length: metadata + payload
    @param[in] need_reply    whether the master is waiting for the reply
    @param[in] payload       the wal log
    @param[in] payload_len   payload length

    @return 0: success;  non-zero: error
  */
  int slave_read_sync_header(const char *header, unsigned long total_len,
                             bool *need_reply, const char **payload,
                             unsigned long *payload_len);

  /**
    The standby replies with an ACK to the master, indicating its replication
    progress.

    @param[in] mysql          the mysql network connection
    @param[in] server_id      standby server id number
    @param[in] received_lsn   lsn received by the standby
    @param[in] flushed_lsn    lsn flushed by the standby
    @param[in] replayed_lsn   lsn replayed by the standby

    @return 0: success;  non-zero: error
  */
  int slave_wal_reply(MYSQL *mysql, uint32_t server_id, uint64_t received_lsn,
                      uint64_t flushed_lsn, uint64_t replayed_lsn);

  /**
    Set the semi-synchronous state of the standby server.

    @param[in] param          replication wal IO observer parameter

    @return 0: success;  non-zero: error
  */
  int slave_wal_start(Wallog_IO_param *param);

  /**
    Close the connection used to send the reply.

    @param[in] param          replication wal IO observer parameter

    @return 0: success;  non-zero: error
  */
  int slave_wal_stop(Wallog_IO_param *param);

 private:
  /* True when initObject has been called */
  bool init_done_ = false;
  bool slave_enabled_ = false;  /* semi-sycn is enabled on the slave */
  MYSQL *mysql_reply = nullptr; /* connection to send reply */
};

/* System and status variables for the slave component */
extern bool rpl_semi_sync_replica_enabled_wal;
extern unsigned long rpl_semi_sync_replica_trace_level_wal;
extern char rpl_semi_sync_replica_status_wal;

#endif /* WAL_SEMISYNC_REPLICA_H */
