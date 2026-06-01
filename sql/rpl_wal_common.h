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

#ifndef RPL_WAL_COMMON_H
#define RPL_WAL_COMMON_H

#include "my_byteorder.h"
#include "my_inttypes.h"

/**
  Version number for wal replication.
*/
constexpr uint8_t rpl_wal_default_version = 0;

/**
  Identifies wal replication, distinguish from binlog replication.
*/
constexpr uint32_t RPL_FLAGS_REPLICA_WAL = 1;

/**
  Message type used for master-slave communication.
*/
enum class RplWalMsgType : uint8_t {
  RPL_WAL_REQUEST_DUMP_MSG = 0,
  RPL_WAL_HEARTBEAT_MSG = 1,
  RPL_WAL_DATA_MSG = 2,
  RPL_WAL_ACK_MSG = 3,
  RPL_WAL_MSG_TYPE_MAX_COUNT
};

/**
  Struct to pass common header about a message: type, version,
  flags, server id.
*/
typedef struct Rpl_comm_header {
  RplWalMsgType type;
  uint8_t version;
  uint16_t flags;
  uint32_t server_id;
} Rpl_comm_header;

/**
  After the connection is established,
  the standby send flushed lsn to the master.
*/
typedef struct Rpl_request_dump_msg {
  Rpl_comm_header common_header;
  uint64_t wal_stream_id;
  uint64_t flushed_lsn;
} Rpl_request_dump_msg;

/**
  Heartbeat message to be send by master at its idle time
  to ensure master's online status to slave.
*/
typedef struct Rpl_wal_heartbeat_msg {
  Rpl_comm_header common_header;
  uint64_t master_flushed_lsn;
} Rpl_wal_heartbeat_msg;

/**
  Wal data message, master send to standby.
*/
typedef struct Rpl_wal_data_msg {
  Rpl_comm_header common_header;
  uint32_t wal_len;
  uint64_t wal_stream_id;
  uint64_t start_lsn;
  uint8_t wal_data[];
} Rpl_wal_data_msg;

/**
  Ack message, include received lsn, flushed lsn, replayed lsn,
  the current version only use flushed lsn.
*/
typedef struct Rpl_wal_ack_msg {
  Rpl_comm_header common_header;
  uint64_t received_lsn;
  uint64_t flushed_lsn;
  uint64_t replayed_lsn;

  /**
    Serialize the ACK message fields into the provided buffer.

    @param[out] buffer  Pointer to the buffer where serialized data will be
    stored.
  */
  void serialize(unsigned char *buffer) const {
    uint32_t offset = 0;
    buffer[offset] = static_cast<uint8_t>(common_header.type);
    offset += sizeof(common_header.type);
    buffer[offset] = common_header.version;
    offset += sizeof(common_header.version);
    int2store(buffer + offset, common_header.flags);
    offset += sizeof(common_header.flags);
    int4store(buffer + offset, common_header.server_id);
    offset += sizeof(common_header.server_id);
    int8store(buffer + offset, received_lsn);
    offset += sizeof(received_lsn);
    int8store(buffer + offset, flushed_lsn);
    offset += sizeof(flushed_lsn);
    int8store(buffer + offset, replayed_lsn);
    offset += sizeof(replayed_lsn);
  }

  /**
    Deserialize the ACK message fields from the provided buffer.

    @param[in] buffer  Pointer to the buffer containing serialized data.
  */
  void deserialize(const unsigned char *buffer) {
    uint32_t offset = 0;
    common_header.type = static_cast<RplWalMsgType>(buffer[offset]);
    offset += sizeof(uint8_t);
    common_header.version = buffer[offset];
    offset += sizeof(uint8_t);
    common_header.flags = uint2korr(buffer + offset);
    offset += sizeof(uint16_t);
    common_header.server_id = uint4korr(buffer + offset);
    offset += sizeof(uint32_t);
    received_lsn = uint8korr(buffer + offset);
    offset += sizeof(uint64_t);
    flushed_lsn = uint8korr(buffer + offset);
    offset += sizeof(uint64_t);
    replayed_lsn = uint8korr(buffer + offset);
    offset += sizeof(uint64_t);
  }
} Rpl_wal_ack_msg;

/**
  @class Rpl_wal_msg_utils

  Used to process messages sent during master-slave communication.
*/
class Rpl_wal_msg_utils {
 public:
  /**
    Init the common message header.

    @param[in] msg_header  Rpl_comm_header will be sent.
    @param[in] type        Message type.
    @param[in] server_id   Server id.
    @param[in] flags       Flags, the current version set to 0.
    @param[in] version     Version number.
  */
  static void init_common_msg_header(Rpl_comm_header *msg_header,
                                     RplWalMsgType type, uint32_t server_id,
                                     uint16_t flags,
                                     uint8_t version = rpl_wal_default_version);

  /**
    Fill the Rpl_wal_heartbeat_msg header.

    @param[in] msg_header           Rpl_wal_heartbeat_msg will be sent.
    @param[in] master_flushed_lsn   Flushed lsn on master.
  */
  static void fill_heartbeat_msg(Rpl_wal_heartbeat_msg *msg_header,
                                 uint64_t master_flushed_lsn);

  /**
    Read the Rpl_request_dump_msg header.

    @param[in]  msg_header     Rpl_request_dump_msg will be read.
    @param[out] wal_stream_id  Wal stream id.
    @param[out] flushed_lsn    Flushed lsn on slave.
    @param[out] server_id      Server id.
    @param[out] flags          Flags of request wal dump.
  */
  static bool read_request_dump_msg(Rpl_request_dump_msg *msg_header,
                                    uint64_t &wal_stream_id,
                                    uint64_t &flushed_lsn, uint32_t &server_id,
                                    uint16_t &flags);

  /**
    Get the max header size of these message types.

    @return It returns max header size.
  */
  static size_t get_max_header_size();
};
#endif  // RPL_WAL_COMMON_H
