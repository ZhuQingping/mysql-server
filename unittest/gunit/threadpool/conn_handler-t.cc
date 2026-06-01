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

/* See http://code.google.com/p/googletest/wiki/Primer */
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "sql/conn_handler/channel_info.h"
#include "sql/conn_handler/connection_handler_impl.h"
#include "sql/conn_handler/connection_handler_manager.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/sql_call.h"

extern ulong Connection_handler_manager::thread_handling;
extern ulong max_connections;
extern ulong opt_rds_admin_max_connections;
extern bool opt_rds_admin_port_using_per_thread;
extern bool opt_rds_local_socket_using_per_thread;
extern thread_local THD *current_thd;

extern void my_thread_global_end();
extern bool my_thread_global_init();

class MockChannelInfo : public Channel_info {
 public:
  MockChannelInfo() { m_nullThd = true; }

  MockChannelInfo(bool nullThd) { m_nullThd = nullThd; }

  THD *create_thd() {
    if (!m_nullThd) {
      THD *thd = new (std::nothrow) THD(false);
      return thd;
    }
    return nullptr;
  }

  Vio *create_and_init_vio() const override { return nullptr; }
  void send_error_and_close_channel(uint, int, bool) {}

 private:
  bool m_nullThd = true;
  bool is_admin_connection() const override { return true; }
};

class MockLocalChannelInfo : public MockChannelInfo {
 public:
  MockLocalChannelInfo(bool nullThd) : MockChannelInfo(nullThd) {}

 private:
  bool is_local_socket() const override { return true; }
  bool is_admin_connection() const override { return false; }
};

class MockOneThreadConnHandler : public One_thread_connection_handler {
 public:
  bool AddConnection(Channel_info *channel_info) {
    return add_connection(channel_info);
  }
};

class MockPerThreadConnHandler : public Per_thread_connection_handler {
 public:
  bool AddConnection(Channel_info *channel_info) {
    return add_connection(channel_info);
  }
};

TEST(ConnHandlerManager, ConnHandlerManagerTest) {
  Connection_handler_manager::init();
  Connection_handler *perThread = new Per_thread_connection_handler();
  Connection_handler_manager::get_instance()->load_connection_handler(
      perThread);
  Channel_info *channel = new MockChannelInfo();
  ASSERT_FALSE(channel->is_local_socket());
  Connection_handler_manager::get_instance()->process_new_connection(channel);

  // per thread
  Channel_info *channel2 = new MockLocalChannelInfo(true);
  MockPerThreadConnHandler *perThreadConnHandler =
      new MockPerThreadConnHandler();
  ASSERT_FALSE(perThreadConnHandler->AddConnection(channel2));

  // one thread
  Channel_info *channel3 = new MockLocalChannelInfo(true);
  MockOneThreadConnHandler *oneThreadConnHandler =
      new MockOneThreadConnHandler();
  ASSERT_TRUE(oneThreadConnHandler->AddConnection(channel3));

  // admin port
  opt_rds_admin_port_using_per_thread = true;
  Channel_info *channel4 = new MockChannelInfo(false);
  Connection_handler_manager::get_instance()->rds_admin_connection_count = 10;
  opt_rds_admin_max_connections = 0;
  max_connections = 0;
  current_thd = channel4->create_thd();
  current_thd->set_admin_connection(true);
  Connection_handler_manager::get_instance()->process_new_connection(channel4);
  ASSERT_FALSE(
      Connection_handler_manager::get_instance()->valid_connection_count(
          current_thd));
  ASSERT_FALSE(
      Connection_handler_manager::get_instance()->check_and_incr_conn_count(
          false, true));
  ASSERT_TRUE(
      Connection_handler_manager::get_instance()->check_and_incr_conn_count(
          true, true));
  Connection_handler_manager::get_instance()->dec_connection_count(true);

  // mock my_thread_init() fail
  my_thread_global_end();

  // one thread
  Channel_info *channel5 = new MockLocalChannelInfo(true);
  ASSERT_TRUE(oneThreadConnHandler->AddConnection(channel5));

  // per thread
  Channel_info *channel6 = new MockLocalChannelInfo(true);
  ASSERT_FALSE(perThreadConnHandler->AddConnection(channel6));
  // wait thread exit
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // reinit
  my_thread_global_init();
}