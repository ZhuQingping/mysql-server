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

#ifndef WAL_SEMISYNC_SOURCE_H
#define WAL_SEMISYNC_SOURCE_H

#include <assert.h>
#include "my_inttypes.h"
#include "plugin/semisync/semisync.h"

extern PSI_memory_key key_ss_memory_LsnWaitSlot;

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key key_ss_mutex_LOCK_wallog_;
extern PSI_cond_key key_ss_cond_COND_wallog_send_;
#endif

extern unsigned int rpl_semi_sync_source_wait_for_replica_count_wal;

/**
  Semaphore waiting array, used by the master to wait for the flush lsn of
  the standby.
*/
constexpr int WAIT_SLOTS_ARRAY_LEN = 1024;

/**
  The length of wal corresponding to each element in the array.
*/
constexpr int WAIT_SLOTS_BLOCK_SIZE = 51200;

struct LsnWaitSlot {
  mysql_cond_t cond;
};

/**
  Wal_repl_semi_sync_master implements the semi-synchronous replication of wal.
  When the transaction is committed on the master, if the lsn required for the
  transaction commit is greater than the received_standby_lsn, it needs to wait.
  When the master receives the ack returned by the slave, if the lsn required
  for the waiting transaction commit is less than or equal to the flushed lsn
  in the ack, the transaction will be awakened.
 */
class Wal_repl_semi_sync_master : public ReplSemiSyncBase {
 private:
  /**
    A condition variable array. Used for waiting and waking up the transaction
    commit thread.
  */
  LsnWaitSlot *lsnWaitSlot = nullptr;

  /* True when init_object has been called. */
  bool init_done = false;

  /* Mutex that protects the following state variables and lsnWaitSlot. */
  mysql_mutex_t LOCK_wallog_;

  /* This is set to true when received_standby_lsn contains meaningful data. */
  bool received_standby_lsn_inited = false;

  /* The position in that file up to which we have the reply from any slaves. */
  uint64_t received_standby_lsn = 0;

  /* This is set to true when notify_start_lsn contains meaningful data. */
  bool notify_start_lsn_inited = false;

  /* The 'smallest' notify position. */
  uint64_t notify_start_lsn = 0;

  /* This is set to true when max_trx_lsn contains meaningful data. */
  bool max_trx_lsn_inited = false;

  /* The 'largest' position that a commit transaction is seeing. */
  uint64_t max_trx_lsn = 0;

  /* All global variables which can be set by parameters. */
  /* semi-sync is enabled on the master */
  volatile bool master_enabled = false;

  /* timeout period(ms) during tranx wait */
  unsigned long wait_timeout = 0;

  /* whether semi-sync is switched */
  bool state_ = false;

  void lock();
  void unlock();

  /* Is semi-sync replication on? */
  bool is_on() { return (state_); }

  void set_master_enabled(bool enabled) { master_enabled = enabled; }

  /* Switch semi-sync off because of timeout in transaction waiting. */
  int switch_off();

  void force_switch_on();

  /* Switch semi-sync on when slaves catch up. */
  int try_switch_on(uint64_t standby_flushed_lsn);

 public:
  Wal_repl_semi_sync_master();
  ~Wal_repl_semi_sync_master();

  bool get_master_enabled() { return master_enabled; }

  void set_trace_level(unsigned long trace_level) {
    trace_level_ = trace_level;
  }

  /* Set if the master has to wait for an ack from the salve or not. */
  void set_wait_no_replica(const void *val);

  /* Set the transaction wait timeout period, in milliseconds. */
  void set_wait_timeout(unsigned long timeout) { wait_timeout = timeout; }

  /**
    Initialize this class after MySQL parameters are initialized. this
    function should be called once at bootstrap time.

    @return 0: success;  non-zero: error
  */
  int init_object();

  /**
    Enable the object to enable semi-sync replication inside the master.

    @return 0: success;  non-zero: error
  */
  int enable_master();

  /**
    Disable the object to disable semi-sync replication inside the master.

    @return 0: success;  non-zero: error
  */
  int disable_master();

  /* Add a semi-sync replication slave. */
  void add_slave();

  /* Remove a semi-sync replication slave. */
  void remove_slave();

  /**
    The entry function for handling the ack returned by the slave.

    @param[in] server_id     slave server id number
    @param[in] packet        the ack packet sent by the slave to the master
    @param[in] packet_len    the length of the ack packet

    @return 0: success;  non-zero: error
  */
  int report_reply_wal_packet(uint32_t server_id, const uchar *packet,
                              ulong packet_len);

  /**
    In semi-sync replication, reports up to which wallog position we have
    received replies from the slave indicating that it already get the log data.
    If the lsn required for the transaction to commit in the waiting state is
    less than or equal to the standby_flushed_lsn, it will be awakened.

    @param[in] standby_flushed_lsn  the flushed lsn in the slave's ack
  */
  void report_reply_wal(uint64_t standby_flushed_lsn);

  /* Wake up the transactions waiting between start_lsn and end_lsn. */
  void notify_wait_trx(uint64_t start_lsn, uint64_t end_lsn);

  /**
    Map the lsn to the corresponding array index.

    @param[in] lsn  Used to calculate the array index

    @return the array index
  */
  int compute_wait_slot_no(uint64_t lsn);

  /**
    Commit a transaction in the final step. If semi-sync is switch on, the
    function will wait to see whether wallog-dump thread get the reply for
    the log data of the transaction. If the wait times out, semi-sync status
    will be switched off and all other transaction would not wait either.

    @param[in] target_lsn   lsn required for transaction commit

    @return 0: success;  non-zero: error
  */
  int wal_commit_trx(uint64_t target_lsn);

  /**
    The master waits for the standby to return the flush lsn
    before submitting.

    @param[in] target_lsn  lsn required for transaction commit.

    @return 0: success;  non-zero: error
  */
  int wait_standby_flush(uint64_t target_lsn);

  /**
    The master retrieves the maximum flushed lsn returned by the slave.

    @param[out] standby_flushed_lsn  Get the flushed lsn of standby
  */
  void get_standby_flushed_lsn(uint64_t *standby_flushed_lsn);

  // Export internal statistics for semi-sync replication.
  void set_export_stats();

  /**
    Reserve space in the replication event packet header:
    . slave semi-sync off: 1 byte - (0)
    . slave semi-sync on:  3 byte - (0, 0xef, 0/1}

    @param[in] header   the header buffer
    @param[in] size     size of the header buffer

    @return size of the bytes reserved for header
  */
  int reserve_sync_header(unsigned char *header, unsigned long size);

  /**
    Set the sync bit in the packet header to indicate to the slave whether
    the master will wait for the reply of the event.  If semi-sync is switched
    off and we detect that the slave is catching up, we switch semi-sync on.

    @param[in] packet         the packet containing the replication event
    @param[in] lsn            the event ending position's file offset
    @param[in] server_id      master server id number

    @return 0: success;  non-zero: error
  */
  int set_master_sync_bit(unsigned char *packet, uint64_t lsn,
                          uint32_t server_id);

  /**
    Read the slave's wal reply so that we know how much progress the slave makes
    on receive replication events.

    @param[in] net         the connection to master
    @param[in] event_buf   pointer to the event packet

    @return 0: success;  non-zero: error
  */
  int read_slave_wal_reply(NET *net, const char *event_buf);

  /**
    'reset master' command is issued from the user and semi-sync need to
    go off for that.
  */
  int reset_master();

  void handle_wal_ack(int server_id [[maybe_unused]],
                      uint64_t standby_flushed_lsn) {
    lock();

    report_reply_wal(standby_flushed_lsn);

    unlock();
  }
};

/* System and status variables for the master component. */
extern bool rpl_semi_sync_source_enabled_wal;
extern char rpl_semi_sync_source_status_wal;
extern unsigned long rpl_semi_sync_source_clients_wal;
extern unsigned long rpl_semi_sync_source_timeout_wal;
extern unsigned long rpl_semi_sync_source_trace_level_wal;
extern unsigned long rpl_semi_sync_source_off_times_wal;
extern unsigned long rpl_semi_sync_source_wait_timeouts_wal;
extern unsigned long long rpl_semi_sync_source_net_wait_num_wal;
extern unsigned long long rpl_semi_sync_source_trx_wait_time_wal;
extern unsigned long rpl_semi_sync_source_yes_transactions_wal;
extern unsigned long rpl_semi_sync_source_no_transactions_wal;
extern unsigned long rpl_semi_sync_source_timefunc_fails_wal;
extern unsigned long rpl_semi_sync_source_wait_sessions_wal;
extern unsigned long rpl_semi_sync_source_wait_pos_backtraverse_wal;
extern unsigned long rpl_semi_sync_source_avg_trx_wait_time_wal;
extern unsigned long rpl_semi_sync_source_avg_net_wait_time_wal;
extern unsigned long long rpl_semi_sync_source_trx_wait_num_wal;
extern unsigned long long rpl_semi_sync_source_net_wait_time_wal;
// no use
extern unsigned long rpl_semi_sync_source_num_timeouts_wal;
/**
  This indicates whether we should keep waiting if no semi-sync slave
  is available.
     0           : stop waiting if detected no available semi-sync slave.
     1 (default) : keep waiting until timeout even no available semi-sync slave.
*/
extern bool rpl_semi_sync_source_wait_no_replica_wal;
#endif /* SEMISYNC_SOURCE_H */