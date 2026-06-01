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

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "local_backup_obs_utils.h"
#include "my_securec.h"  // memcpy_s
#include "mysql/components/services/log_builtins.h"
#include "mysql/psi/mysql_file.h"
#include "mysqld_error.h"
#ifndef NDEBUG
#include "sql/local_backup/full_local_backup.h"
#endif
#include "sql/mysqld.h"
#include "sql/scc_util.h"

#ifdef NDEBUG
#define RETRY_INTERVAL 1000
#else
#define RETRY_INTERVAL 1
#endif

#define LB_ERR_LOG_MSG(message)                                   \
  do {                                                            \
    std::stringstream ss;                                         \
    ss << message;                                                \
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss.str().c_str()); \
  } while (0)

#define LB_WARN_LOG_MSG(message)                                    \
  do {                                                              \
    std::stringstream ss;                                           \
    ss << message;                                                  \
    LogErr(WARNING_LEVEL, ER_LOCAL_BACKUP_ERROR, ss.str().c_str()); \
  } while (0)

#ifdef SUPPORT_OBS
bool lb_inf_obs_impl::obs_initialized = false;
std::mutex lb_inf_obs_impl::initializeMutex_;

lb_inf_obs_impl::lb_inf_obs_impl() {}

lb_inf_obs_impl::~lb_inf_obs_impl() {}

/*
 * init obs sdk
 * @param[IN] obsParamsMap: stores all the parameters required by obs,
 *                          include url, ak, sk, region
 * @return OBS_STATUS_OK if success, other if failed
 */
int lb_inf_obs_impl::init(std::map<std::string, std::string> &paramsMap) {
  int ret = OBS_STATUS_OK;
  if (!obs_initialized) {
    std::lock_guard<std::mutex> guardDoor(lb_inf_obs_impl::initializeMutex_);
    if (!obs_initialized) {
      if ((ret = init_sdk())) {
        return ret;
      }
      obs_initialized = true;
    }
  }
  ret = memset_s(&option, sizeof(obs_options), 0, sizeof(obs_options));
  if (ret != 0) {
    return OBS_STATUS_ErrorUnknown;
  }

  std::map<std::string, std::string> real_obs_config(paramsMap);
  if (get_real_obs_config(paramsMap, real_obs_config)) {
    return OBS_STATUS_ErrorUnknown;
  }

  m_obsParamsMap[indexMap[OBS_URL]] = real_obs_config[indexMap[OBS_URL]];
  m_obsParamsMap[indexMap[OBS_BUCKET_NAME]] =
      real_obs_config[indexMap[OBS_BUCKET_NAME]];
  m_obsParamsMap[indexMap[OBS_AK]] = real_obs_config[indexMap[OBS_AK]];
  m_obsParamsMap[indexMap[OBS_SK]] = real_obs_config[indexMap[OBS_SK]];
  m_obsParamsMap[indexMap[OBS_URI_STYLE]] =
      real_obs_config[indexMap[OBS_URI_STYLE]];
  m_obsParamsMap[indexMap[OBS_ENCRYPTION]] =
      real_obs_config[indexMap[OBS_ENCRYPTION]];
  m_obsParamsMap[indexMap[OBS_KMS_KEY]] =
      real_obs_config[indexMap[OBS_KMS_KEY]];

  init_obs_options(&option);
  option.bucket_options.host_name =
      const_cast<char *>(m_obsParamsMap[indexMap[OBS_URL]].c_str());
  option.bucket_options.bucket_name =
      const_cast<char *>(m_obsParamsMap[indexMap[OBS_BUCKET_NAME]].c_str());
  option.bucket_options.access_key =
      const_cast<char *>(m_obsParamsMap[indexMap[OBS_AK]].c_str());
  option.bucket_options.secret_access_key =
      const_cast<char *>(m_obsParamsMap[indexMap[OBS_SK]].c_str());
  if (!m_obsParamsMap[indexMap[OBS_URI_STYLE]].empty()) {
    option.bucket_options.uri_style =
        obs_uri_style(std::stoi(m_obsParamsMap[indexMap[OBS_URI_STYLE]]));
  } else {
    option.bucket_options.uri_style = OBS_URI_STYLE_VIRTUALHOST;
  }
  option.bucket_options.storage_class = (obs_storage_class)storage_class;
  option.request_options.max_connected_time =
      static_cast<int>(rds_lb_obs_max_connected_time);
  set_encryption(m_obsParamsMap[indexMap[OBS_ENCRYPTION]],
                 m_obsParamsMap[indexMap[OBS_KMS_KEY]]);
  return OBS_STATUS_OK;
}
void lb_inf_obs_impl::deinit() {
  deinit_sdk();
  obs_initialized = false;
}
bool lb_inf_obs_impl::should_retry(uint8_t retries) {
  if (ERROR_RETRIES != retries) {
    std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_INTERVAL));
    return true;
  }
  return false;
}

bool lb_inf_obs_impl::get_decrypted_key(const char *origin_key,
                                        char **real_key) {
  if (origin_key == nullptr) {
    return false;
  }

  int real_key_len = 0;
  if (!scc_decrypt_retry(origin_key, strlen(origin_key), real_key,
                         &real_key_len, SCC_DECRYPT_MAX_RETRY)) {
    return false;
  } else {
    return true;
  }
}

bool lb_inf_obs_impl::get_real_obs_config(
    std::map<std::string, std::string> &origin_map,
    std::map<std::string, std::string> &real_map) {
  bool error = false;

  std::string scc_decrypt = origin_map[indexMap[OBS_SCC_DECRYPT]];

  if (scc_decrypt != "true") {
    return error;
  }

  const char *origin_access_key = origin_map[indexMap[OBS_AK]].c_str();
  const char *origin_secret_key = origin_map[indexMap[OBS_SK]].c_str();

  /* Decrypt the OBS AK */
  char *real_access_key = nullptr;
  error = get_decrypted_key(origin_access_key, &real_access_key);
  real_map[indexMap[OBS_AK]] =
      real_access_key == nullptr ? "" : real_access_key;
  if (real_access_key != nullptr) {
    free(real_access_key);
  }

  /* Decrypt the OBS SK */
  char *real_secret_key = nullptr;
  error |= get_decrypted_key(origin_secret_key, &real_secret_key);
  real_map[indexMap[OBS_SK]] =
      real_secret_key == nullptr ? "" : real_secret_key;
  if (real_secret_key != nullptr) {
    free(real_secret_key);
  }

  if (error) {
    LB_ERR_LOG_MSG("Decrypt OBS AK SK from mysql configuration file fail");
  }
  return error;
}

void lb_inf_obs_impl::set_encryption(const std::string &encryption_param,
                                     const std::string &kms_key_param) {
  if (encryption_param.empty()) {
    return;
  }
  if (encryption_param != "0" && encryption_param != "1") {
    LB_WARN_LOG_MSG("obs encryption option not invalid, value: "
                    << encryption_param << ", don't open encryption.");
    encryption = false;
    return;
  }
  if (encryption_param == "1") {
    encryption = true;
    option.bucket_options.protocol = OBS_PROTOCOL_HTTPS;
    (void)memset_s(&kms_key, KMS_KEY_LEN, 0, KMS_KEY_LEN);
    int ret = strcpy_s(kms_key, KMS_KEY_LEN, kms_key_param.c_str());
    // LCOV_EXCL_START
    if (ret != 0) {
      LB_ERR_LOG_MSG("strcpy_s failed on obs set encryption of local backup.");
      return;
    }
    // LCOV_EXCL_STOP
    (void)memset_s(&kms_server_side_encryption, KMS_SERVER_SIDE_ENCRYP_LEN, 0,
                   KMS_SERVER_SIDE_ENCRYP_LEN);
    ret = strcpy_s(kms_server_side_encryption, KMS_SERVER_SIDE_ENCRYP_LEN,
                   "aws:kms");
    // LCOV_EXCL_START
    if (ret != 0) {
      LB_ERR_LOG_MSG("strcpy_s failed on obs set encryption of local backup.");
      return;
    }
    // LCOV_EXCL_STOP
  }
}

std::unique_ptr<server_side_encryption_params>
lb_inf_obs_impl::get_encryption_params(bool is_append_exist) {
  if (!encryption || is_append_exist) {
    return nullptr;
  }

  auto encryption_params = std::make_unique<server_side_encryption_params>();

  (void)memset_s(encryption_params.get(), sizeof(server_side_encryption_params),
                 0, sizeof(server_side_encryption_params));
  encryption_params->encryption_type = OBS_ENCRYPTION_KMS;
  if (kms_key[0] != '\0') {
    encryption_params->kms_key_id = get_kms_key();
  }
  encryption_params->kms_server_side_encryption = get_kms_server_size_encrypt();
  option.request_options.auth_switch = OBS_S3_TYPE;
  return encryption_params;
}

obs_status lb_inf_obs_impl::response_put_properties_callback(
    const obs_response_properties *properties, void *callback_data) {
  if (properties == nullptr || callback_data == nullptr) {
    return OBS_STATUS_InvalidArgument;
  }

  auto *data = static_cast<put_object_callback_data *>(callback_data);
  data->object_length = properties->content_length;
  data->kms_key_id = properties->kms_key_id;
  data->customer_key = properties->customer_key_md5;
  data->request_id = properties->request_id;
  return OBS_STATUS_OK;
}

template <typename CallBackData>
void lb_inf_obs_impl::response_complete_callback(obs_status status,
                                                 const obs_error_details *error,
                                                 void *callback_data) {
  if (callback_data == nullptr || error == nullptr) {
    LB_ERR_LOG_MSG(
        "Response complete callback function got invalid input parameter. "
        "Callback_data is null: "
        << (callback_data == nullptr)
        << " buffer is null: " << (error == nullptr));
    return;
  }
  auto *data = static_cast<CallBackData *>(callback_data);
  data->message = error->message;
  data->ret_status = status;
}

obs_status lb_inf_obs_impl::list_objects_callback(
    int is_truncated, const char *next_marker, int contents_count,
    const obs_list_objects_content *contents,
    int common_prefixes_count [[maybe_unused]],
    const char **common_prefixes [[maybe_unused]], void *callback_data) {
  list_object_callback_data *data =
      static_cast<list_object_callback_data *>(callback_data);

  if (data == nullptr || data->objects == nullptr || next_marker == nullptr ||
      contents == nullptr) {
    return OBS_STATUS_InvalidArgument;
  }

  data->is_truncated = is_truncated;
  if (EOK != memcpy_s(data->next_marker, NAMELEN, next_marker, NAMELEN)) {
    return OBS_STATUS_ErrorUnknown;
  }

  list_objects_t tmp;
  for (int i = 0; i < contents_count; i++) {
    tmp.key = (contents + i)->key;
    tmp.last_modified = (contents + i)->last_modified;
    tmp.etag = (contents + i)->etag;
    tmp.size = (contents + i)->size;
    data->objects->push_back(tmp);
  }

  return OBS_STATUS_OK;
}

obs_status lb_inf_obs_impl::get_metadata_callback(
    const obs_response_properties *properties, void *callback_data) {
  get_metadata_callback_data *data =
      (get_metadata_callback_data *)callback_data;

  if (data == nullptr) {
    return OBS_STATUS_InvalidArgument;
  }

  data->content_length = properties->content_length;
  return OBS_STATUS_OK;
}

int lb_inf_obs_impl::put_object_callback(int buffer_size, char *buffer,
                                         void *callback_data) {
  if (callback_data == nullptr || buffer == nullptr) {
    LB_ERR_LOG_MSG(
        "Put object callback function got invalid input parameter. "
        "Callback_data is null: "
        << (callback_data == nullptr)
        << " buffer is null: " << (buffer == nullptr));
    return OBS_STATUS_ErrorUnknown;
  }

  put_object_callback_data *data =
      static_cast<put_object_callback_data *>(callback_data);

  uint64_t toRead = 0;
  if (data->buffer_size) {
    toRead =
        ((data->buffer_size > (unsigned)buffer_size) ? (unsigned)buffer_size
                                                     : data->buffer_size);
    if (EOK != memcpy_s(buffer, buffer_size,
                        data->put_buffer + data->cur_offset, toRead)) {
      return OBS_STATUS_ErrorUnknown;
    }
  }

  data->buffer_size -= toRead;
  data->cur_offset += toRead;

  return toRead;
}

int lb_inf_obs_impl::append_object_callback(int buffer_size, char *buffer,
                                            void *callback_data) {
  return lb_inf_obs_impl::put_object_callback(buffer_size, buffer,
                                              callback_data);
}

obs_status lb_inf_obs_impl::response_append_properties_callback(
    const obs_response_properties *properties, void *callback_data) {
  if ((nullptr == properties) || (nullptr == callback_data)) {
    return OBS_STATUS_InvalidParameter;
  }
  put_object_callback_data *data =
      static_cast<put_object_callback_data *>(callback_data);
  if (nullptr == properties->obs_next_append_position) {
    return OBS_STATUS_InvalidParameter;
  }

  *(data->position) = properties->obs_next_append_position;
  data->object_length = properties->content_length;
  data->kms_key_id = properties->kms_key_id;
  data->customer_key = properties->customer_key_md5;
  data->request_id = properties->request_id;

  return OBS_STATUS_OK;
}

obs_status lb_inf_obs_impl::get_object_callback(int buffer_size,
                                                const char *buffer,
                                                void *callback_data) {
  if (callback_data == nullptr) {
    LB_ERR_LOG_MSG(
        "Get object callback function got invalid input parameter. "
        "Callback_data is null.");
    return OBS_STATUS_ErrorUnknown;
  }
  get_object_callback_data *data =
      static_cast<get_object_callback_data *>(callback_data);
  uint64_t *pos = &(data->write_pos);
  if (EOK !=
      memcpy_s(data->receive_buffer + *pos, buffer_size, buffer, buffer_size)) {
    return OBS_STATUS_ErrorUnknown;
  }
  *pos += buffer_size;
  return OBS_STATUS_OK;
}

obs_status lb_inf_obs_impl::delete_objects_data_callback(
    int contents_count, obs_delete_objects *delobjs, void *callbackData) {
  batch_delete_callback_data *data =
      static_cast<batch_delete_callback_data *>(callbackData);

  if (callbackData == nullptr || delobjs == nullptr) {
    LB_ERR_LOG_MSG(
        "Delete objects callback function got invalid input parameter. "
        "Callback_data is null: "
        << (callbackData == nullptr)
        << " delobjs is null: " << (delobjs == nullptr));
    return OBS_STATUS_InvalidArgument;
  }

  for (int i = 0; i < contents_count; i++) {
    const obs_delete_objects *content = &(delobjs[i]);
    int ret_status = obs_status_string2int(content->code);
    if (ret_status) {
      data->delete_failed_count++;
      LB_WARN_LOG_MSG("Delete object result: object key: "
                      << content->key << "error code: " << content->code
                      << " and error message: " << content->message);
    }
  }
  return OBS_STATUS_OK;
}

int lb_inf_obs_impl::init_sdk() {
  obs_status ret_status = OBS_STATUS_BUTT;

  ret_status = obs_initialize(OBS_INIT_ALL);
  if (OBS_STATUS_OK != ret_status) {
    LB_ERR_LOG_MSG(
        "OBS initialize fail. Error code: " << get_obs_status_name(ret_status));
  }
  return ret_status;
}

void lb_inf_obs_impl::deinit_sdk() { obs_deinitialize(); }

int lb_inf_obs_impl::lb_put_object(const char *object_name, const char *buffer,
                                   uint64_t buffer_length) {
  put_object_callback_data callback_data;

  uint8_t retries = 0;
  obs_put_object_handler putobjectHandler = {
      {&response_put_properties_callback,
       &response_complete_callback<put_object_callback_data>},
      &put_object_callback,
      nullptr};

  do {
    memset_s(&callback_data, sizeof(callback_data), 0,
             sizeof(put_object_callback_data));
    callback_data.put_buffer = const_cast<char *>(buffer);
    callback_data.buffer_size = buffer_length;
    callback_data.ret_status = OBS_STATUS_BUTT;
    callback_data.cur_offset = 0;

    int mock_put_object = 0;

    obs_put_properties put_properties;
    init_put_properties(&put_properties);

    auto encryption_params = get_encryption_params();
#ifndef NDEBUG
    // LCOV_EXCL_START
    bool myThreadInited = my_thread_is_inited();
    if (!myThreadInited) {
      my_thread_init();  // For using DBUG_ to test
    }
    DBUG_EXECUTE_IF("mock_put_object", {
      char *receive_buffer = new char[buffer_length];
      put_object_callback(buffer_length, receive_buffer, &callback_data);
      obs_error_details error_detail;
      error_detail.message = "SUCCESS";
      response_complete_callback<put_object_callback_data>(
          OBS_STATUS_OK, &error_detail, &callback_data);
      obs_response_properties properties;
      properties.content_length = 512;
      response_put_properties_callback(&properties, &callback_data);
      callback_data.ret_status = OBS_STATUS_OK;
      DBUG_EXECUTE_IF("mock_put_object_retry", {
        if (retries == 0) {
          callback_data.ret_status = OBS_STATUS_NameLookupError;
        }
      });
      delete[] receive_buffer;
      mock_put_object = 1;
    });
    DBUG_EXECUTE_IF("mock_put_object_error", {
      callback_data.ret_status = OBS_STATUS_AccessDenied;
      mock_put_object = 1;
    });
    if (!myThreadInited) {
      my_thread_end();
    }
    // LCOV_EXCL_STOP
#endif
    if (!mock_put_object) {
      put_object(&option, const_cast<char *>(object_name), buffer_length,
                 &put_properties, encryption_params.get(), &putobjectHandler,
                 &callback_data);
    }
    if (OBS_STATUS_OK != callback_data.ret_status &&
        (retries % REPORT_INTERVAL == 0)) {
      LB_WARN_LOG_MSG("OBS error occur when put object, error code: "
                      << get_obs_status_name(callback_data.ret_status)
                      << " need to retry, retry_times: " << unsigned(retries)
                      << " and object_name: " << object_name);
    }
  } while (obs_status_is_retryable(callback_data.ret_status) &&
           should_retry(retries++));

  int ret = callback_data.ret_status;
  if (ret != OBS_STATUS_OK) {
    LB_ERR_LOG_MSG("OBS put object fail. Error code: "
                   << get_obs_status_name(callback_data.ret_status));
  }
  return ret;
}

int lb_inf_obs_impl::lb_append_object(const char *object_name,
                                      const char *buffer,
                                      uint64_t buffer_length,
                                      std::string &append_position) {
  put_object_callback_data callback_data;

  uint8_t retries = 0;
  obs_append_object_handler appendobjectHandler = {
      {&response_append_properties_callback,
       &response_complete_callback<put_object_callback_data>},
      &append_object_callback};
  if (append_position.empty()) {
    append_position = "0";
  }
  do {
    memset_s(&callback_data, sizeof(callback_data), 0,
             sizeof(put_object_callback_data));
    callback_data.put_buffer = const_cast<char *>(buffer);
    callback_data.buffer_size = buffer_length;
    callback_data.ret_status = OBS_STATUS_BUTT;
    callback_data.cur_offset = 0;
    callback_data.position = &append_position;
    int mock_append_object = 0;

    obs_put_properties put_properties;
    init_put_properties(&put_properties);

    std::string position = append_position;
    bool is_append_exist = (position != "0");
    auto encryption_params = get_encryption_params(is_append_exist);
#ifndef NDEBUG
    // LCOV_EXCL_START
    bool myThreadInited = my_thread_is_inited();
    if (!myThreadInited) {
      my_thread_init();  // For using DBUG_ to test
    }
    DBUG_EXECUTE_IF("mock_append_object", {
      char *receive_buffer = new char[buffer_length];
      append_object_callback(buffer_length, receive_buffer, &callback_data);
      obs_error_details error_detail;
      error_detail.message = "SUCCESS";
      response_complete_callback<put_object_callback_data>(
          OBS_STATUS_OK, &error_detail, &callback_data);
      obs_response_properties properties;
      properties.content_length = 512;
      properties.obs_next_append_position = "mock_append_object";
      response_append_properties_callback(&properties, &callback_data);
      callback_data.ret_status = OBS_STATUS_OK;
      DBUG_EXECUTE_IF("mock_append_object_retry", {
        if (retries == 0) {
          callback_data.ret_status = OBS_STATUS_NameLookupError;
        }
      });
#ifndef IS_DSTORE_BACKUP_TOOL
      DBUG_EXECUTE_IF("mock_append_too_many_objects", {
        // Mock too many objects
        increase_current_lb_object_count(1000);
      });
#endif
      delete[] receive_buffer;
      mock_append_object = 1;
    });
    DBUG_EXECUTE_IF("mock_append_object_error", {
      callback_data.ret_status = OBS_STATUS_AccessDenied;
      mock_append_object = 1;
    });
    if (!myThreadInited) {
      my_thread_end();
    }
    // LCOV_EXCL_STOP
#endif
    if (!mock_append_object) {
      append_object(&option, const_cast<char *>(object_name), buffer_length,
                    position.c_str(), &put_properties, encryption_params.get(),
                    &appendobjectHandler, &callback_data);
    }
    if (OBS_STATUS_OK != callback_data.ret_status &&
        (retries % REPORT_INTERVAL == 0)) {
      LB_WARN_LOG_MSG("OBS error occur when append object, error code: "
                      << get_obs_status_name(callback_data.ret_status)
                      << " need to retry, retry_times: " << unsigned(retries)
                      << " and object_name: " << object_name);
    }
  } while (obs_status_is_retryable(callback_data.ret_status) &&
           should_retry(retries++));

  int ret = callback_data.ret_status;
  if (ret != OBS_STATUS_OK) {
    LB_ERR_LOG_MSG("OBS append object fail. Error code: "
                   << get_obs_status_name(callback_data.ret_status));
  }

  return ret;
}

int lb_inf_obs_impl::lb_get_object(const char *object_name, char *buffer,
                                   uint64_t start_byte, uint64_t read_length,
                                   uint64_t &out_length) {
  get_object_callback_data callback_data;

  obs_object_info object_info;

  // OBS config status has been checked before calling this function.
  memset_s(&object_info, sizeof(object_info), 0, sizeof(obs_object_info));
  object_info.key = const_cast<char *>(object_name);

  obs_get_conditions getcondition;
  memset_s(&getcondition, sizeof(getcondition), 0, sizeof(obs_get_conditions));
  init_get_properties(&getcondition);
  // The starting position of the reading
  getcondition.start_byte = start_byte;
  // Read length, default is 0: read to the end of the object
  getcondition.byte_count = read_length;

  obs_get_object_handler get_object_handler = {
      {nullptr, &response_complete_callback<get_object_callback_data>},
      &get_object_callback};

  uint8_t retries = 0;
  do {
    memset_s(&callback_data, sizeof(callback_data), 0,
             sizeof(get_object_callback_data));
    callback_data.receive_buffer = buffer;
    callback_data.ret_status = OBS_STATUS_BUTT;
    callback_data.write_pos = 0;
    int mock_get_object = 0;
#ifndef NDEBUG
    // LCOV_EXCL_START
    bool myThreadInited = my_thread_is_inited();
    if (!myThreadInited) {
      my_thread_init();  // For using DBUG_ to test
    }
    DBUG_EXECUTE_IF("mock_get_object", {
      int buffer_length = static_cast<int>(read_length);
      char *receive_buffer = new char[buffer_length];
      (void)memset_s(receive_buffer, buffer_length, '1', buffer_length);
      obs_error_details error_detail;
      error_detail.message = "SUCCESS";
      response_complete_callback<get_object_callback_data>(
          OBS_STATUS_OK, &error_detail, &callback_data);
      get_object_callback(buffer_length, receive_buffer, &callback_data);
      delete[] receive_buffer;
      callback_data.ret_status = OBS_STATUS_OK;
      DBUG_EXECUTE_IF("mock_get_object_retry", {
        if (retries == 0) {
          callback_data.ret_status = OBS_STATUS_NameLookupError;
        }
      });
      mock_get_object = 1;
    });
    DBUG_EXECUTE_IF("mock_get_object_error", {
      callback_data.ret_status = OBS_STATUS_AccessDenied;
      mock_get_object = 1;
    });
    if (!myThreadInited) {
      my_thread_end();
    }
    // LCOV_EXCL_STOP
#endif

    if (!mock_get_object) {
      get_object(&option, &object_info, &getcondition, 0, &get_object_handler,
                 &callback_data);
    }
    if (OBS_STATUS_OK != callback_data.ret_status &&
        (retries % REPORT_INTERVAL == 0)) {
      LB_WARN_LOG_MSG("OBS error occur when get object, error code: "
                      << get_obs_status_name(callback_data.ret_status)
                      << " , need to retry, retry_times: " << unsigned(retries)
                      << " and object name: " << object_name);
    }
  } while (obs_status_is_retryable(callback_data.ret_status) &&
           should_retry(retries++));
  out_length = callback_data.write_pos;
  int ret = callback_data.ret_status;
  if (ret != OBS_STATUS_OK) {
    LB_ERR_LOG_MSG("OBS get object fail. Error code: "
                   << get_obs_status_name(callback_data.ret_status));
    return ret;
  }
  return ret;
}

// LCOV_EXCL_START
int lb_inf_obs_impl::lb_copy_object(const char *pre_object_name,
                                    const char *new_object_name) {
  char eTag[OBS_COMMON_LEN_256] = {0};
  int64_t lastModified;
  obs_status ret_status = OBS_STATUS_BUTT;

  obs_copy_destination_object_info objectinfo;
  objectinfo.destination_bucket = option.bucket_options.bucket_name;
  objectinfo.destination_key = const_cast<char *>(new_object_name);
  objectinfo.etag_return = eTag;
  objectinfo.etag_return_size = sizeof(eTag);
  objectinfo.last_modified_return = &lastModified;

  obs_response_handler responseHandler = {
      nullptr, &response_complete_callback<copy_object_callback_data>};

  uint8_t retries = 0;
  do {
    obs_put_properties putProperties;
    init_put_properties(&putProperties);

    int mock_copy_object = 0;
    DBUG_EXECUTE_IF("mock_copy_object", {
      ret_status = OBS_STATUS_OK;
      mock_copy_object = 1;
    });

    if (!mock_copy_object) {
      copy_object(&option, const_cast<char *>(pre_object_name), nullptr,
                  &objectinfo, 1, &putProperties, NULL, &responseHandler,
                  &ret_status);
    }
    if (OBS_STATUS_OK != ret_status && (retries % REPORT_INTERVAL == 0)) {
      LB_WARN_LOG_MSG("OBS error occur when copy object, error code: "
                      << get_obs_status_name(ret_status)
                      << " , need to retry, retry_times: " << unsigned(retries)
                      << "pre_object_name: " << pre_object_name
                      << " , new_object_name: " << new_object_name);
    }
  } while (obs_status_is_retryable(ret_status) && should_retry(retries++));

  if (ret_status != OBS_STATUS_OK) {
    LB_ERR_LOG_MSG("OBS copy object fail. Error code: "
                   << get_obs_status_name(ret_status));
  }
  return ret_status;
}

int lb_inf_obs_impl::lb_rename_object(const char *pre_object_name,
                                      const char *new_object_name) {
  obs_status ret_status = OBS_STATUS_BUTT;

  obs_response_handler response_handler = {
      nullptr, &response_complete_callback<rename_object_callback_data>};

  uint8_t retries = 0;
  do {
    int mock_rename_object = 0;
    DBUG_EXECUTE_IF("mock_rename_object", {
      ret_status = OBS_STATUS_OK;
      mock_rename_object = 1;
    });

    if (!mock_rename_object) {
      rename_object(&option, const_cast<char *>(pre_object_name),
                    const_cast<char *>(new_object_name), &response_handler,
                    &ret_status);
    }
    if (OBS_STATUS_OK != ret_status && (retries % REPORT_INTERVAL == 0)) {
      LB_WARN_LOG_MSG("OBS error occur when rename object, error code: "
                      << get_obs_status_name(ret_status)
                      << " need to retry, retry_times: " << unsigned(retries)
                      << " , pre_object_name: " << pre_object_name
                      << " , and new_object_name: " << new_object_name);
    }
  } while (obs_status_is_retryable(ret_status) && should_retry(retries++));

  if (ret_status != OBS_STATUS_OK) {
    LB_ERR_LOG_MSG("OBS rename object fail. Error code: "
                   << get_obs_status_name(ret_status));
  }
  return ret_status;
}
// LCOV_EXCL_STOP

int lb_inf_obs_impl::lb_list_all_object(bool &is_truncate, char *next_marker,
                                        vObjects *objects, const char *prefix) {
  list_object_callback_data callback_data;

  obs_list_objects_handler list_bucket_objects_handler = {
      {nullptr, &response_complete_callback<list_object_callback_data>},
      &list_objects_callback};

  uint8_t retries = 0;

  do {
    memset_s(&callback_data, sizeof(callback_data), 0,
             sizeof(list_object_callback_data));
    int mock_list_object = 0;
    callback_data.objects = objects;
    callback_data.ret_status = OBS_STATUS_BUTT;
    callback_data.is_truncated = is_truncate;
    callback_data.next_marker[0] = 0;

    objects->clear();
#ifndef NDEBUG
    bool myThreadInited = my_thread_is_inited();
    if (!myThreadInited) {
      my_thread_init();  // For using DBUG_ to test
    }
    DBUG_EXECUTE_IF("mock_list_object", {
      obs_list_objects_content list_obj;
      list_obj.key = "taurus/dstore/bak/test/mtr/full_backup/object-name+0@0@0";
      list_obj.last_modified = 123;
      list_obj.etag = "etag";
      list_obj.size = 512;
      list_objects_callback(false, next_marker, 1, &list_obj, 0, nullptr,
                            &callback_data);
      callback_data.ret_status = OBS_STATUS_OK;
      DBUG_EXECUTE_IF("mock_list_object_retry", {
        if (retries == 0) {
          callback_data.ret_status = OBS_STATUS_NameLookupError;
        }
      });
      mock_list_object = 1;
    });
    DBUG_EXECUTE_IF("mock_multi_list_object", {
      obs_list_objects_content list_obj[2];
      list_obj[0].key =
          "taurus/dstore/bak/test/mtr/full_backup/object-name+0@0@0";
      list_obj[0].last_modified = 123;
      list_obj[0].etag = "etag";
      list_obj[0].size = 512;
      list_obj[1].key =
          "taurus/dstore/bak/test/mtr/full_backup/merged-object-name+1@0@0";
      list_obj[1].last_modified = 123;
      list_obj[1].etag = "etag";
      list_obj[1].size = 512;
      list_objects_callback(false, next_marker, 2, list_obj, 0, nullptr,
                            &callback_data);
      callback_data.ret_status = OBS_STATUS_OK;
      mock_list_object = 1;
    });
    DBUG_EXECUTE_IF("mock_list_object_error", {
      callback_data.ret_status = OBS_STATUS_AccessDenied;
      mock_list_object = 1;
    });
    if (!myThreadInited) {
      my_thread_end();
    }
#endif

    if (!mock_list_object) {
      list_bucket_objects(&option, const_cast<char *>(prefix), next_marker,
                          nullptr, MAX_KEY_SIZE, &list_bucket_objects_handler,
                          &callback_data);
    }

    is_truncate = callback_data.is_truncated;
    if (EOK !=
        memcpy_s(next_marker, NAMELEN, callback_data.next_marker, NAMELEN)) {
      LB_ERR_LOG_MSG(
          "OBS error occur when copy next_marker in list all object");
      return OBS_STATUS_ErrorUnknown;
    }

    if (OBS_STATUS_OK != callback_data.ret_status &&
        (retries % REPORT_INTERVAL == 0)) {
      LB_WARN_LOG_MSG("OBS error occur when list all object, error code: "
                      << get_obs_status_name(callback_data.ret_status)
                      << " , need to retry, retry_times: " << unsigned(retries)
                      << " and prefix: " << prefix);
    }
  } while (obs_status_is_retryable(callback_data.ret_status) &&
           should_retry(retries++));

  if (OBS_STATUS_OK != callback_data.ret_status) {
    LB_ERR_LOG_MSG("OBS list all object fail. Error code: "
                   << get_obs_status_name(callback_data.ret_status));
  }
  return callback_data.ret_status;
}

int lb_inf_obs_impl::lb_batch_delete_objects(std::vector<std::string> &keys) {
  batch_delete_callback_data callback_data;
  callback_data.delete_failed_count = 0;

  obs_object_info del_obj[MAX_KEY_SIZE];
  for (size_t i = 0; i < keys.size(); i++) {
    del_obj[i].key = const_cast<char *>(keys[i].c_str());
    del_obj[i].version_id = nullptr;
  }

  obs_delete_object_info del_obj_info;
  (void)memset_s(&del_obj_info, sizeof(obs_delete_object_info), 0,
                 sizeof(obs_delete_object_info));
  del_obj_info.keys_number = keys.size();

  obs_delete_object_handler handler = {
      {nullptr, &response_complete_callback<batch_delete_callback_data>},
      &delete_objects_data_callback};

  uint8_t retries = 0;
  do {
    int mock_delete_object = 0;
#ifndef NDEBUG
    // LCOV_EXCL_START
    bool myThreadInited = my_thread_is_inited();
    if (!myThreadInited) {
      my_thread_init();  // For using DBUG_ to test
    }
    DBUG_EXECUTE_IF("mock_delete_object", {
      obs_delete_objects obs_delete_obj;
      obs_delete_obj.key =
          "taurus/dstore/bak/test/mtr/full_backup/object-name+0@0@0";
      obs_delete_obj.code = "0";
      obs_delete_obj.message = "Success";
      delete_objects_data_callback(1, &obs_delete_obj, &callback_data);
      callback_data.ret_status = OBS_STATUS_OK;
      DBUG_EXECUTE_IF("mock_delete_object_retry", {
        if (retries == 0) {
          callback_data.ret_status = OBS_STATUS_NameLookupError;
        }
      });
      mock_delete_object = 1;
    });
    DBUG_EXECUTE_IF("mock_delete_object_error", {
      callback_data.ret_status = OBS_STATUS_AccessDenied;
      callback_data.delete_failed_count = 1;
      mock_delete_object = 1;
    });
    if (!myThreadInited) {
      my_thread_end();
    }
    // LCOV_EXCL_STOP
#endif

    if (!mock_delete_object) {
      batch_delete_objects(&option, del_obj, &del_obj_info, 0, &handler,
                           &callback_data);
    }
    if (OBS_STATUS_OK != callback_data.ret_status &&
        (retries % REPORT_INTERVAL == 0)) {
      LB_WARN_LOG_MSG("OBS error occur when batch delete objects, error code: "
                      << get_obs_status_name(callback_data.ret_status)
                      << " need to retry and retry_times: "
                      << unsigned(retries));
    }
  } while (obs_status_is_retryable(callback_data.ret_status) &&
           should_retry(retries++));

  if (OBS_STATUS_OK != callback_data.ret_status) {
    LB_ERR_LOG_MSG("OBS batch delete object fail. Error code: "
                   << get_obs_status_name(callback_data.ret_status));
  }

  if (callback_data.delete_failed_count > 0) {
    LB_WARN_LOG_MSG("There are "
                    << callback_data.delete_failed_count
                    << " OBS objects delete fail. This may be caused by server "
                       "restart during the last removal objects procedure.");
  }

  return callback_data.ret_status;
}

// LCOV_EXCL_START
int lb_inf_obs_impl::lb_modify_object(const char *object_name,
                                      const char *buffer,
                                      uint64_t buffer_length,
                                      uint64_t start_pos) {
  put_object_callback_data callback_data;

  uint8_t retries = 0;

  obs_put_properties put_properties;
  init_put_properties(&put_properties);
  obs_modify_object_handler modify_object_handler = {
      {nullptr, &response_complete_callback<put_object_callback_data>},
      &put_object_callback};

  do {
    memset_s(&callback_data, sizeof(callback_data), 0,
             sizeof(put_object_callback_data));
    callback_data.put_buffer = const_cast<char *>(buffer);
    callback_data.buffer_size = buffer_length;
    int mock_modify_object = 0;
    DBUG_EXECUTE_IF("mock_modify_object", {
      char *receive_buffer = new char[buffer_length];
      put_object_callback(buffer_length, receive_buffer, &callback_data);
      callback_data.ret_status = OBS_STATUS_OK;
      delete[] receive_buffer;
      mock_modify_object = 1;
    });
    DBUG_EXECUTE_IF("mock_modify_object_error", {
      char *receive_buffer = new char[buffer_length];
      put_object_callback(buffer_length, receive_buffer, &callback_data);
      callback_data.ret_status = OBS_STATUS_PartialFile;
      delete[] receive_buffer;
      mock_modify_object = 1;
    });
    if (!mock_modify_object) {
      modify_object(&option, const_cast<char *>(object_name), buffer_length,
                    start_pos, &put_properties, 0, &modify_object_handler,
                    &callback_data);
    }
    if (OBS_STATUS_OK != callback_data.ret_status &&
        (retries % REPORT_INTERVAL == 0)) {
      LB_WARN_LOG_MSG("OBS error occur when modify object, error code: "
                      << get_obs_status_name(callback_data.ret_status)
                      << " , need to retry, retry_times: " << unsigned(retries)
                      << " and object_name: " << object_name);
    }
  } while (obs_status_is_retryable(callback_data.ret_status) &&
           should_retry(retries++));

  int ret = callback_data.ret_status;
  if (ret != OBS_STATUS_OK) {
    LB_ERR_LOG_MSG("OBS modify object fail. Error code: "
                   << get_obs_status_name(callback_data.ret_status));
    return ret;
  }
  return ret;
}
// LCOV_EXCL_STOP

int lb_inf_obs_impl::lb_get_object_meta(const char *object_name,
                                        uint64_t &length) {
  obs_object_info object_info;
  (void)memset_s(&object_info, sizeof(obs_object_info), 0,
                 sizeof(obs_object_info));

  object_info.key = const_cast<char *>(object_name);
  object_info.version_id = nullptr;

  get_metadata_callback_data data;

  obs_response_handler response_handler = {
      &get_metadata_callback,
      &response_complete_callback<get_metadata_callback_data>};

  uint8_t retries = 0;
  do {
    (void)memset_s(&data, sizeof(get_metadata_callback_data), 0,
                   sizeof(get_metadata_callback_data));
    int mock_get_object_meta = 0;
#ifndef NDEBUG
    // LCOV_EXCL_START
    bool myThreadInited = my_thread_is_inited();
    if (!myThreadInited) {
      my_thread_init();  // For using DBUG_ to test
    }
    DBUG_EXECUTE_IF("mock_get_object_meta", {
      obs_response_properties properties;
      properties.content_length = 512;
      get_metadata_callback(&properties, &data);
      length = data.content_length;
      data.ret_status = OBS_STATUS_OK;
      DBUG_EXECUTE_IF("mock_get_object_meta_retry", {
        if (retries == 0) {
          data.ret_status = OBS_STATUS_NameLookupError;
        }
      });
      mock_get_object_meta = 1;
    });
    DBUG_EXECUTE_IF("mock_get_object_meta_error", {
      obs_response_properties properties;
      properties.content_length = 512;
      get_metadata_callback(&properties, &data);
      length = data.content_length;
      data.ret_status = OBS_STATUS_AccessDenied;
      mock_get_object_meta = 1;
    });
    if (!myThreadInited) {
      my_thread_end();
    }
    // LCOV_EXCL_STOP
#endif
    if (!mock_get_object_meta) {
      get_object_metadata(&option, &object_info, 0, &response_handler, &data);
    }
    if (OBS_STATUS_OK != data.ret_status && (retries % REPORT_INTERVAL == 0)) {
      LB_WARN_LOG_MSG("OBS error occur when get object meta, error code: "
                      << get_obs_status_name(data.ret_status)
                      << " , need to retry, retry_times: " << unsigned(retries)
                      << " and object_name: " << object_name);
    }
  } while (obs_status_is_retryable(data.ret_status) && should_retry(retries++));

  if (data.ret_status) {
    /* Just add warn here because the first time to archive a table
       does not exist any metadata file on disk. */
    LB_WARN_LOG_MSG("OBS get object metadata fail. Error code: "
                    << get_obs_status_name(data.ret_status));
  }
  length = data.content_length;
  return data.ret_status;
}
#endif  // SUPPORT_OBS

// LCOV_EXCL_START
/* Just for MTR test to simulate init OBS in file mode */
int lb_inf_file_impl::init(std::map<std::string, std::string> &paramsMap
                           [[maybe_unused]]) {
  is_initialized = true;
  return 0;
}

void lb_inf_file_impl::deinit() { is_initialized = false; }

int lb_inf_file_impl::lb_put_object(const char *object_name, const char *buffer,
                                    uint64_t buffer_length) {
  std::string object_path = convert_object_name(object_name);

  std::ofstream dumpFile;
  dumpFile.open(object_path,
                std::ios::out | std::ios::trunc | std::ios::binary);
  if (!dumpFile.is_open()) {
    LB_ERR_LOG_MSG("Cannot open file. Object name: " << object_path);
    return 1;
  }
  dumpFile.write(const_cast<char *>(buffer), buffer_length);
  dumpFile.close();
  return 0;
}

int lb_inf_file_impl::lb_get_object(const char *object_name, char *buffer,
                                    uint64_t start_byte, uint64_t read_length,
                                    uint64_t &out_length) {
  DBUG_EXECUTE_IF("CrashDuringRenameTable", {
    if (!is_initialized) {
      LB_ERR_LOG_MSG("OBS SDK is not initialized");
      return 1;
    }
  });
  std::ifstream file;

  std::string object_path = convert_object_name(object_name);

  file.open(object_path, std::ios::in | std::ios::binary);

  uint64_t file_size = 0;
  lb_get_object_meta(object_name, file_size);
  if (start_byte + read_length > file_size) {
    LB_ERR_LOG_MSG("FILE read object fail. Start Byte: "
                   << start_byte << " and Read length " << read_length);
    return 1;
  }

  if (!file.is_open()) {
    LB_ERR_LOG_MSG("Cannot open file. Object name: " << object_path);
    return 1;
  }
  file.seekg(start_byte);

  if (read_length == 0) {
    read_length = file_size - start_byte;
  }
  out_length = read_length;
  file.read(buffer, read_length);
  file.close();

  return 0;
}

/* Just for MTR test to copy objects in file mode */
int lb_inf_file_impl::lb_copy_object(const char *pre_object_name,
                                     const char *new_object_name) {
  std::string pre_object_path = convert_object_name(pre_object_name);
  std::string new_object_path = convert_object_name(new_object_name);
  if (access(pre_object_path.c_str(), 0)) {
    /* if the object to be copied does not exists, just report a warning*/
    LB_WARN_LOG_MSG("The object to be copied does not exists. Object name: "
                    << pre_object_path);
    return 0;
  }

  std::ifstream src(pre_object_path, std::ios::binary);
  std::ofstream dst(new_object_path, std::ios::binary);
  dst << src.rdbuf();
  return 0;
}

/* Just for MTR test to list objects in file mode */
int lb_inf_file_impl::lb_list_all_object(bool &is_truncate [[maybe_unused]],
                                         char *next_marker [[maybe_unused]],
                                         vObjects *objects [[maybe_unused]],
                                         const char *prefix [[maybe_unused]]) {
  return 0;
}

/* Just for MTR test to rename objects in file mode */
int lb_inf_file_impl::lb_rename_object(const char *pre_object_name,
                                       const char *new_object_name) {
  std::string pre_object_path = convert_object_name(pre_object_name);
  std::string new_object_path = convert_object_name(new_object_name);
  if (access(pre_object_path.c_str(), 0)) {
    /* if the object to be renamed does not exists, just report a warning*/
    LB_WARN_LOG_MSG("The object to be renamed does not exists. Object name: "
                    << pre_object_path);
    return 0;
  }

  int ret = rename(pre_object_path.c_str(), new_object_path.c_str());
  if (ret) {
    LB_ERR_LOG_MSG("FILE rename fail. Object name: "
                   << pre_object_path << " and return code " << ret);
    return 1;
  }

  return 0;
}

int lb_inf_file_impl::lb_batch_delete_objects(std::vector<std::string> &keys) {
  for (std::string key : keys) {
    std::string object_path = convert_object_name(key.c_str());
    if (access(object_path.c_str(), 0)) {
      LB_WARN_LOG_MSG("The object to be deleted does not exists. Object name: "
                      << object_path);
      return 0;
    }
    int ret = remove(object_path.c_str());
    if (ret) {
      LB_ERR_LOG_MSG("FILE batch delete fail. Object name: " << object_path);
      return 1;
    }
  }
  return 0;
}

/* Just used to modify the page when we have DML operation on archived table */
int lb_inf_file_impl::lb_modify_object(const char *object_name [[maybe_unused]],
                                       const char *buffer [[maybe_unused]],
                                       uint64_t buffer_length [[maybe_unused]],
                                       uint64_t start_pos [[maybe_unused]]) {
  return 0;
}

int lb_inf_file_impl::lb_get_object_meta(const char *object_name,
                                         uint64_t &length) {
  DBUG_EXECUTE_IF("CrashDuringRenameTable", {
    if (!is_initialized) {
      LB_ERR_LOG_MSG("OBS SDK is not initialized");
      return 1;
    }
  });
  std::string object_path = convert_object_name(object_name);

  if (access(object_path.c_str(), 0)) {
    /* Just add warn here because the first time to archive a table
       does not exist any metadata file on disk. */
    LB_WARN_LOG_MSG(
        "The meta object does not exists. Object name: " << object_path);
    return 0;
  }
  struct stat statbuf;
  stat(object_path.c_str(), &statbuf);

  length = static_cast<uint64_t>(statbuf.st_size);
  return 0;
}

std::string lb_inf_file_impl::convert_object_name(const char *object_name) {
  std::string object_path = object_name;
  if (object_path.find("global_meta.meta") != std::string::npos) {
    return object_path;
  }
  /* handle the combine_meta cases */
  if (object_path.find("combine_meta.meta") != std::string::npos) {
    return object_path;
  }
  size_t pos_version_sep = std::string::npos;
  size_t pos_dd_table_id_sep = std::string::npos;
  size_t pos_space_id_sep = std::string::npos;

  if (lb_obs_meta_mode) {
    pos_version_sep = object_path.rfind('/');
    pos_dd_table_id_sep = object_path.substr(0, pos_version_sep).rfind('/');
    pos_space_id_sep = object_path.substr(0, pos_dd_table_id_sep).rfind('/');
  } else {
    pos_space_id_sep = object_path.rfind('/');
  }

  if (pos_version_sep != std::string::npos) {
    object_path.replace(pos_version_sep, 1, "-");
  }

  if (pos_dd_table_id_sep != std::string::npos) {
    object_path.replace(pos_dd_table_id_sep, 1, "-");
  }

  if (pos_space_id_sep != std::string::npos) {
    object_path.replace(pos_space_id_sep, 1, "-");
  }
  return object_path;
}

int lb_inf_file_impl::lb_append_object(const char *object_name,
                                       const char *buffer,
                                       uint64_t buffer_length,
                                       std::string &append_position
                                       [[maybe_unused]]) {
  std::string object_path = convert_object_name(object_name);
  std::ofstream dumpFile;
  dumpFile.open(object_path, std::ios::app);
  if (!dumpFile.is_open()) {
    LB_ERR_LOG_MSG("Cannot open file. Object name: " << object_path);
    return 1;
  }
  dumpFile.write(const_cast<char *>(buffer), buffer_length);
  dumpFile.close();
  return 0;
}
// LCOV_EXCL_STOP
