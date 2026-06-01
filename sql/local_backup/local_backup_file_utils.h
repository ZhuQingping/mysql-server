/* Copyright (c) 2025, Huawei and/or its affiliates.

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

#ifndef LOCAL_BACKUP_FILE_UTILS_H
#define LOCAL_BACKUP_FILE_UTILS_H

#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>

extern uint32_t rds_local_backup_file_interface_type;
extern uint32_t rds_local_backup_sleep_interval;
extern uint32_t rds_local_backup_sleep_time_ms;
extern bool rds_dstore_lb_use_io_thread;
extern uint32_t rds_dstore_lb_global_io_retry_number;
extern uint32_t rds_dstore_lb_local_io_retry_times;
extern int32_t rds_dstore_lb_local_io_hang_timeout_ms;
extern int32_t rds_dstore_lb_local_io_thread_quit_timeout_ms;
extern uint32_t rds_dstore_lb_io_buffer_size;

namespace CDE {

/* Local backup file interface type. */
enum class LBFileInterfaceType : uint32_t {
  SYS_CALL_TYPE = 0,
  MYSYS_CALL_TYPE
};

/**
  Open file for local backup. Choose sys call or mysys call depends on parameter
  rds_local_backup_file_interface_type.

  @param[in]  file The file path.
  @param[in]  oflag The flag determines the type of access used.

  @return File descriptor of the opened file, or -1 on error.
*/
int LBOpen(const char *file, int oflag);

/**
  Open file for local backup. Choose sys call or mysys call depends on parameter
  rds_local_backup_file_interface_type.

  @param[in]  file The file path.
  @param[in]  oflag The flag determines the type of access used.
  @param[in]  mode The mode of the created file.

  @return File descriptor of the opened file, or -1 on error.
*/
int LBopenOther(const char *file, int oflag, mode_t mode);

/**
  Close file for local backup. Choose sys call or mysys call depends on
  parameter rds_local_backup_file_interface_type.

  @param[in]  fd The file descriptor.

  @retval 0 if successful
  @retval -1 in case of errors
*/
int LBClose(int fd);

/**
  Truncate file for local backup. Choose sys call or mysys call depends on
  parameter rds_local_backup_file_interface_type.

  @param[in]  fd The file descriptor.
  @param[in]  length The new file size.

  @return 0 if successful, otherwise on error.
*/
int LBFtruncate(int fd, uint64_t length);

/**
  Get file status for local backup. Choose sys call or mysys call depends on
  parameter rds_local_backup_file_interface_type.

  @param[in]  fd The file descriptor.
  @param[out] buf The file status.

  @retval 0 if successful
  @retval -1 in case of errors
*/
int LBFstat(int fd, struct stat *buf);

/**
  Get file status for local backup. Choose sys call or mysys call depends on
  parameter rds_local_backup_file_interface_type.

  @param[in]  file The file path.
  @param[out] buf The file status.

  @retval 0 if successful
  @retval -1 in case of errors
*/
int LBStat(const char *file, struct stat *buf);

/**
  Read file for local backup. Choose sys call or mysys call depends on parameter
  rds_local_backup_file_interface_type.

  @param[in]  fd The file descriptor.
  @param[out] buf The buffer to read data into.
  @param[in]  length Number of bytes to read.
  @param[in]  offset Position to read from.

  @return Number of bytes read, or -1 on error.
*/
ssize_t LBPread(int fd, void *buf, uint64_t length, uint64_t offset);

/**
  Write file for local backup. Choose sys call or mysys call depends on
  parameter rds_local_backup_file_interface_type.

  @param[in]  fd The file descriptor.
  @param[out] buf The buffer to write data from.
  @param[in]  length Number of bytes to write.
  @param[in]  offset Position to write to.

  @return Number of bytes actually written, or -1 on error.
*/
ssize_t LBPwrite(int fd, void *buf, uint64_t length, uint64_t offset);

/**
  Sync data in file to disk for local backup. Choose sys call or mysys call
  depends on parameter rds_local_backup_file_interface_type.

  @param[in]  fd The file descriptor.

  @retval 0 if successful
  @retval -1 in case of errors
*/
int LBFsync(int fd);

/**
  Make directory for local backup. Choose sys call or mysys call depends on
  parameter rds_local_backup_file_interface_type.

  @param[in]  path The directory path.
  @param[in]  mode The permission bits mode.

  @retval 0 if successful
  @retval -1 in case of errors
*/
int LBMkdir(const char *path, __mode_t mode);

/**
  Destory all LbIoContext in thread local during do local full backup
*/
void DestoryLbIoContextSet();

/**
  Try to clear and destory io context from rigid io context set when the context
  thread have leave
*/
void TryClearRigidIoContextSet();

}  // namespace CDE

#endif
