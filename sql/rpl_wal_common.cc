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

#include <assert.h>
#include <algorithm>

#include "sql/rpl_wal_common.h"
#include "sql/sql_class.h"

void Rpl_wal_msg_utils::init_common_msg_header(Rpl_comm_header *msg_header,
                                               RplWalMsgType type,
                                               uint32_t server_id,
                                               uint16_t flags,
                                               uint8_t version) {
  assert(type < RplWalMsgType::RPL_WAL_MSG_TYPE_MAX_COUNT &&
         version == rpl_wal_default_version);

  msg_header->type = type;
  msg_header->version = version;
  int4store(pointer_cast<uchar *>(&(msg_header->server_id)), server_id);
  int2store(pointer_cast<uchar *>(&(msg_header->flags)), flags);
}

void Rpl_wal_msg_utils::fill_heartbeat_msg(Rpl_wal_heartbeat_msg *msg_header,
                                           uint64_t master_flushed_lsn) {
  assert(msg_header->common_header.type ==
         RplWalMsgType::RPL_WAL_HEARTBEAT_MSG);

  int8store(pointer_cast<uchar *>(&(msg_header->master_flushed_lsn)),
            master_flushed_lsn);
}

bool Rpl_wal_msg_utils::read_request_dump_msg(Rpl_request_dump_msg *msg_header,
                                              uint64_t &wal_stream_id,
                                              uint64_t &flushed_lsn,
                                              uint32_t &server_id,
                                              uint16_t &flags) {
  if ((msg_header->common_header.type !=
       RplWalMsgType::RPL_WAL_REQUEST_DUMP_MSG) ||
      (msg_header->common_header.version != rpl_wal_default_version)) {
    return true;
  }
  flags = uint2korr(pointer_cast<uchar *>(&(msg_header->common_header.flags)));
  server_id =
      uint4korr(pointer_cast<uchar *>(&(msg_header->common_header.server_id)));
  wal_stream_id =
      uint8korr(pointer_cast<uchar *>(&(msg_header->wal_stream_id)));
  flushed_lsn = uint8korr(pointer_cast<uchar *>(&(msg_header->flushed_lsn)));
  return false;
}

size_t Rpl_wal_msg_utils::get_max_header_size() {
  static const size_t max_header_size = std::max<size_t>(
      {sizeof(Rpl_wal_data_msg), sizeof(Rpl_wal_ack_msg),
       sizeof(Rpl_wal_heartbeat_msg), sizeof(Rpl_request_dump_msg)});
  return max_header_size;
}
