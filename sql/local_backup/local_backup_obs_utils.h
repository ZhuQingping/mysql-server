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

#ifndef LOCAL_BACKUP_OBS_UTILS_H
#define LOCAL_BACKUP_OBS_UTILS_H

#include <memory.h>
#include <stdint.h>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifdef SUPPORT_OBS
#include "eSDKOBS.h"
#endif

#define MAX_KEY_SIZE 1000
#define NAMELEN 1024
#define MAX_RETRIES 3
#define REPORT_INTERVAL 3
#define OBS_STANDARD 0
#define KMS_KEY_LEN 37
#define KMS_SERVER_SIDE_ENCRYP_LEN 8

enum paramIndex {
  OBS_URL = 0,
  OBS_AK,
  OBS_SK,
  OBS_REGION,
  OBS_BUCKET_NAME,
  OBS_PARAMS_NUM,
  OBS_URI_STYLE,
  OBS_ENCRYPTION,
  OBS_KMS_KEY,
  OBS_SCC_DECRYPT
};

class lb_inf {
 public:
  typedef struct list_objects_t {
    std::string key;       /* The object name */
    int64_t last_modified; /* Last modification Unix timestamp */
    std::string etag;      /* The checksum of an object */
    uint64_t size;         /* The size of the object */
  } list_objects_t;
  typedef std::vector<list_objects_t> vObjects;
  bool lb_obs_meta_mode = 0;

  lb_inf() {}
  virtual int init(std::map<std::string, std::string> &paramsMap) = 0;
  virtual void deinit() = 0;
  virtual ~lb_inf() {}
  virtual int lb_put_object(const char *object_name, const char *buffer,
                            uint64_t buffer_length) = 0;
  virtual int lb_append_object(const char *object_name, const char *buffer,
                               uint64_t buffer_length,
                               std::string &append_position) = 0;
  virtual int lb_get_object(const char *object_name, char *buffer,
                            uint64_t start_byte, uint64_t read_length,
                            uint64_t &out_length) = 0;
  virtual int lb_copy_object(const char *pre_object_name,
                             const char *new_object_name) = 0;
  virtual int lb_list_all_object(bool &is_truncate, char *next_marker,
                                 vObjects *objects, const char *prefix) = 0;
  virtual int lb_rename_object(const char *pre_object_name,
                               const char *new_object_name) = 0;
  virtual int lb_batch_delete_objects(std::vector<std::string> &keys) = 0;
  virtual int lb_modify_object(const char *object_name, const char *buffer,
                               uint64_t buffer_length, uint64_t start_pos) = 0;
  virtual int lb_get_object_meta(const char *object_name, uint64_t &length) = 0;
  virtual const char *get_obs_status_name(int status) = 0;
};

#ifdef SUPPORT_OBS
/* This class encapsulates OBS SDK to upload, download, delete, list, copy, and
 * rename object files.*/
class lb_inf_obs_impl : public lb_inf {
  typedef struct list_object_callback_data {
    // general
    obs_status ret_status;
    const char *request_id;
    const char *message;
    // list objects
    int32_t is_truncated = false;
    vObjects *objects;
    char next_marker[NAMELEN];
  } list_object_callback_data;

  typedef struct put_object_callback_data {
    // general
    obs_status ret_status;
    const char *request_id;
    const char *message;
    // put objects
    char *put_buffer;
    uint64_t buffer_size;
    uint64_t cur_offset;
    std::string *position;  // next append position
    uint64_t object_length;
    const char *kms_key_id;
    const char *customer_key;
  } put_object_callback_data;

  typedef struct get_object_callback_data {
    // general
    obs_status ret_status;
    const char *request_id;
    const char *message;
    // get objects
    char *receive_buffer;
    uint64_t write_pos;
  } get_object_callback_data;

  typedef struct copy_object_callback_data {
    // general
    obs_status ret_status;
    const char *request_id;
    const char *message;
  } CallBackData;

  typedef struct rename_object_callback_data {
    // general
    obs_status ret_status;
    const char *requestId;
    const char *message;
  } rename_object_callback_data;

  typedef struct batch_delete_callback_data {
    // general
    obs_status ret_status;
    const char *requestId;
    const char *message;
    // batch delete objects
    uint16_t delete_failed_count;
  } batch_delete_callback_data;

  typedef struct get_metadata_callback_data {
    // general
    obs_status ret_status;
    const char *requestId;
    const char *message;
    // get metadata
    uint64_t content_length;
  } get_metadata_callback_data;

 public:
  lb_inf_obs_impl();
  ~lb_inf_obs_impl();
  int init(std::map<std::string, std::string> &paramsMap) override;
  void deinit() override;

  /**
   * This function is used to upload an OBS object from a given buffer
   * @param[IN] object_name: object name
   * @param[IN] buffer: buffer that will uploaded to the OBS
   * @param[IN] buffer_length: the size of buffer
   * @return OBS_STATUS_OK if success, other if failed
   */
  int lb_put_object(const char *object_name, const char *buffer,
                    uint64_t buffer_length) override;

  /**
   * This function is used to append an OBS object from a given buffer
   * @param[IN] object_name: object name
   * @param[IN] buffer: buffer that will uploaded to the OBS
   * @param[IN] buffer_length: the size of buffer
   * @param[IN] append_position: the position on which last append, next append
   * will start on it
   * @return OBS_STATUS_OK if success, other if failed
   */
  int lb_append_object(const char *object_name, const char *buffer,
                       uint64_t buffer_length,
                       std::string &append_position) override;

  /**
   * This function is used to download an object from obs to a given buffer
   * @param[IN] object_name: object name
   * @param[IN/OUT] buffer: buffer to store object content
   * @param[IN] start_byte: the start byte to read from object
   * @param[IN] read_length: the length to read, 0: read to the end of file
   * @param[out] out_length: the length of the reading stream from object
   * @return OBS_STATUS_OK if success, other if failed
   */
  int lb_get_object(const char *object_name, char *buffer, uint64_t start_byte,
                    uint64_t read_length, uint64_t &out_length) override;

  /**
   * This function is used to copy an object in OBS
   * @param[IN] pre_object_name: object name to be copyed
   * @param[IN] new_object_name: the new object name
   * @return OBS_STATUS_OK if success, other if failed
   */
  int lb_copy_object(const char *pre_object_name,
                     const char *new_object_name) override;

  /**
   * This function is used to rename an object in OBS
   * @param[IN] pre_object_name: object name to be renamed
   * @param[IN] new_object_name: the new object name
   * @return OBS_STATUS_OK if success, other if failed
   */
  int lb_rename_object(const char *pre_object_name,
                       const char *new_object_name) override;

  /**
   * This function is used to list all objects in OBS with a given prefix.
   * A maximum of MAX_KEY_SIZE objects can be obtained by using this function.
   * If the total number of objects with the given prefix is greater than
   * MAX_KEY_SIZE, is_truncated is set to true, indicating that the results
   * are truncated. Also, the next_marker will be set to indicate the
   * start postion of the next retrieval.
   *
   * @param[IN/OUT] is_truncate: used to indicate if the result is truncated
   * @param[IN/OUT] next_marker: the start position of next retrieval
   * @param[IN/OUT] objects: the vector of the listed object.
   * @param[IN] prefix: the prefix of the object to be listed.
   * @return OBS_STATUS_OK if success, other if failed
   */
  int lb_list_all_object(bool &is_truncate, char *next_marker,
                         vObjects *objects, const char *prefix) override;

  /**
   * This function is used to delete a batch of objects in OBS
   * @param[IN] pre_object_name: object name to be renamed
   * @param[IN/OUT] new_object_name: the new object name
   * @return OBS_STATUS_OK if success, other if failed
   */
  int lb_batch_delete_objects(std::vector<std::string> &keys) override;

  /**
   * This function is used to modify a object in OBS
   * @param[IN] object_name: object name
   * @param[IN] buffer: buffer that will be used to modify the object
   * @param[IN] buffer_length: the length to be write
   * @param[IN] start_pos: the start position to write
   * @return OBS_STATUS_OK if success, other if failed
   */
  int lb_modify_object(const char *object_name, const char *buffer,
                       uint64_t buffer_length, uint64_t start_pos) override;

  /**
   * This function is used to get metadata from object in OBS
   * @param[IN] object_name: object name
   * @param[IN/OUT] length: the object length
   * @return OBS_STATUS_OK if success, other if failed
   */
  int lb_get_object_meta(const char *object_name, uint64_t &length) override;

  /**
   * This callback function is used within the lb_list_all_object function
   * to fetch the results from OBS SDK.
   *
   * @param[IN] is_truncate: used to indicate if the result is truncated
   * @param[IN] next_marker: the start position of next retrieval
   * @param[IN/OUT] callbackData: store the listed object information
   * @return OBS_STATUS_OK if success, other if failed
   */
  static obs_status list_objects_callback(
      int is_truncated, const char *next_marker, int contents_count,
      const obs_list_objects_content *contents, int common_prefixes_count,
      const char **common_prefixes, void *callbackData);

  /**
   * This callback function is used within the lb_put_object function
   * to get the result of uploading objects to OBS.
   *
   * @param[IN] buffer_size: the length of buffer to be uploaded
   * @param[IN] buffer: the uploaded buffer
   * @param[IN/OUT] callbackData: store the put object information
   * @return OBS_STATUS_OK if success, other if failed
   */
  static int put_object_callback(int buffer_size, char *buffer,
                                 void *callback_data);

  static obs_status response_put_properties_callback(
      const obs_response_properties *properties, void *callback_data);

  /**
   * This callback function is used within the lb_append_object function
   * to get the result of uploading objects to OBS.
   *
   * @param[IN] buffer_size: the length of buffer to be uploaded
   * @param[IN] buffer: the uploaded buffer
   * @param[IN/OUT] callbackData: store the append object information
   * @return OBS_STATUS_OK if success, other if failed
   */
  static int append_object_callback(int buffer_size, char *buffer,
                                    void *callback_data);

  static obs_status response_append_properties_callback(
      const obs_response_properties *properties, void *callback_data);

  /**
   * This callback function is used within the lb_batch_delete_objects
   * function to generate the result of delete objects in OBS.
   *
   * @param[IN] contents_count: the number of objects to be deleted
   * @param[IN] delobjs: the deleted object information
   * @return OBS_STATUS_OK if success, other if failed
   */
  static obs_status delete_objects_data_callback(int contents_count,
                                                 obs_delete_objects *delobjs,
                                                 void *callbackData);

  /**
   * This callback function is used for getting an object within
   * lb_get_object to generate the result of get objects from OBS.
   * @param[IN] buffer_size: buffer size
   * @param[IN] buffer: buffer of a temporary storage object
   * @param[OUT] callback_data: Callback data, from which obtain details
   * @return OBS_STATUS_OK if success, other if failed
   */
  static obs_status get_object_callback(int buffer_size, const char *buffer,
                                        void *callback_data);

  /**
   * This callback function is used for getting metadata of an object.
   * @param[IN] properties: response properties
   * @param[OUT] callback_data: Callback data, from which obtain details
   * @return OBS_STATUS_OK if success, other if failed
   */
  static obs_status get_metadata_callback(
      const obs_response_properties *properties, void *callback_data);

  /**
   * Covert the error code to obs status value
   * @param[IN] code: the string format of OBS status
   * @return OBS status value
   */
  static obs_status obs_status_string2int(const char *code) {
    std::stringstream strValue;
    strValue << code;
    unsigned int intValue;
    strValue >> intValue;
    return (obs_status)intValue;
  }

  void set_max_retry_time(uint16_t times) { ERROR_RETRIES = times; }

  /**
   * Covert the obs status to the error code string
   * @param[IN] status: obs status return code
   * @return OBS status in string format
   */
  const char *get_obs_status_name(int status) override {
    const char *status_name = obs_get_status_name(obs_status(status));
    if (nullptr == status_name) {
      return "INVALID_STATUS";
    }
    return status_name;
  }

 private:
  /**
   * Initialize a OBS client. we can only initialize once per process
   * @return 0 if success, other if failed
   */
  int init_sdk();
  void deinit_sdk();

  /* Used to indicate if OBS client is initialized */
  static bool obs_initialized;

  /* Mutex for obs client initialization */
  static std::mutex initializeMutex_;

  /* OBS storage type */
  int storage_class = OBS_STANDARD;

  /* obs setting */
  obs_options option;

  /* Max retry times when invoking OBS SDK fail */
  uint16_t ERROR_RETRIES = MAX_RETRIES;

  bool encryption = false;
  char kms_key[KMS_KEY_LEN] = {0};
  char kms_server_side_encryption[KMS_SERVER_SIDE_ENCRYP_LEN] = {0};

  std::map<enum paramIndex, std::string> indexMap = {
      {OBS_URL, "obs_url"},
      {OBS_AK, "AK"},
      {OBS_SK, "SK"},
      {OBS_REGION, "obs_region"},
      {OBS_BUCKET_NAME, "bucket_name"},
      {OBS_URI_STYLE, "uri_style"},
      {OBS_ENCRYPTION, "encryption"},
      {OBS_KMS_KEY, "kms_key"},
      {OBS_SCC_DECRYPT, "scc_decrypt"}};

  std::map<std::string, std::string> m_obsParamsMap = {
      {"AK", ""},
      {"SK", ""},
      {"bucket_name", "noneed"},
      {"obs_port", "noneed"},
      {"obs_url", ""},
      {"obs_region", ""},
      {"uri_style", ""},
      {"encryption", ""},
      {"kms_key", ""}};

  /*
   * Callback when the server response is complete
   * @param[IN] status: return code
   * @param[IN] error: detailed error description
   * @param[OUT] callbackData: Callback data, from which obtain details
   */
  template <typename CallBackData>
  static void response_complete_callback(obs_status status,
                                         const obs_error_details *error,
                                         void *callbackData);
  bool should_retry(uint8_t retries);

  bool get_decrypted_key(const char *origin_key, char **real_key);
  bool get_real_obs_config(std::map<std::string, std::string> &origin_map,
                           std::map<std::string, std::string> &real_map);
  void set_encryption(const std::string &encryption_param,
                      const std::string &kms_key_param);
  char *get_kms_key() { return kms_key; }
  char *get_kms_server_size_encrypt() { return kms_server_side_encryption; }
  std::unique_ptr<server_side_encryption_params> get_encryption_params(
      bool is_append_exist = false);
};
#endif  // SUPPORT_OBS

// LCOV_EXCL_START
/*
 * This class implements functions such as uploading, downloading,
 * listing, copying, modifying, and renaming objects based on the
 * file system. It is used only for testing to simulate the behavior
 * of OBS.
 */
class lb_inf_file_impl : public lb_inf {
  // private:
  // static constexpr size_t OBJECT_META_SIZE = sizeof(uint64_t);
  // static constexpr uint32_t PAGE_SIZE = 16 * 1024;

 public:
  lb_inf_file_impl() {}
  ~lb_inf_file_impl() {}

  int init(std::map<std::string, std::string> &paramsMap) override;
  void deinit() override;
  int lb_put_object(const char *object_name, const char *buffer,
                    uint64_t buffer_length) override;
  int lb_append_object(const char *object_name, const char *buffer,
                       uint64_t buffer_length,
                       std::string &append_position) override;
  int lb_get_object(const char *object_name, char *buffer, uint64_t start_byte,
                    uint64_t read_length, uint64_t &out_length) override;
  int lb_copy_object(const char *pre_object_name,
                     const char *new_object_name) override;
  int lb_list_all_object(bool &is_truncate, char *next_marker,
                         vObjects *objects, const char *prefix) override;
  int lb_rename_object(const char *pre_object_name,
                       const char *new_object_name) override;
  int lb_batch_delete_objects(std::vector<std::string> &keys) override;
  int lb_modify_object(const char *object_name, const char *buffer,
                       uint64_t buffer_length, uint64_t start_pos) override;
  int lb_get_object_meta(const char *object_name, uint64_t &length) override;

  const char *get_obs_status_name(int status [[maybe_unused]]) override {
    return "local backup file mode error";
  }

 private:
  std::string convert_object_name(const char *object_name);

  int is_initialized = false;
};
// LCOV_EXCL_STOP

enum obs_obj_type {
  OBS_OBJ_TYPE_ONE_FILE = 0,
  OBS_OBJ_TYPE_MERGED_FILES = 1,
  OBS_OBJ_TYPE_MERGED_FILES_WITH_SEP_META = 2,
  OBS_OBJ_TYPE_MERGED_FILES_META = 3
};

#endif  // LOCAL_BACKUP_OBS_UTILS_H
