/*****************************************************************************

Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
Date: 2017.8.29

*****************************************************************************/
#include "my_securec.h" /* Should be the first include */

#include <vector>
#include "rds_permission_control.h"

#include "log.h"
#include "map_helpers.h"
#include "mysql/psi/mysql_memory.h"
#include "mysql/psi/mysql_thread.h"
#include "mysqld_error.h"
#include "scope_guard.h"  // create_scope_guard
#include "sql/auth/auth_acls.h"
#include "sql/auth/dynamic_privilege_table.h"
#include "sql/auth/sql_acl.h"
#include "sql/mysqld.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/table.h"

#define GOTO_ERR(A) \
  {                 \
    error = A;      \
    goto err;       \
  }

PSI_rwlock_key key_rds_user_list_active;
PSI_mutex_key key_rds_user_list_update;
PSI_memory_key key_rds_user_memory;

/**
  Validate the list string value is.

  @param list_str  The string of list
                   (e.g., "xx,yy,zz")
  @param len       the length of the list string

  @retval TRUE  valid
  @retval FALSE invalid
*/
bool is_valid_list_value(const char *list_str, size_t len) {
  unsigned int i;

  /*if len ==0 return true,in case of unsigned number flipping*/
  if (len == 0) {
    return true;
  }

  /* Not allow whitespace in the string */
  for (i = 0; i < len; i++) {
    if (std::isspace(list_str[i])) {
      return false;
    }
  }
  /* Now check the empty name in the string */
  for (i = 0; i < (len - 1); i++) {
    if (list_str[i] == ',' && list_str[i + 1] == ',') {
      return false;
    }
  }
  /* Do not allow leading or trailing comma like ",admin1,ad," */
  return !(list_str[0] == ',' || list_str[len - 1] == ',');
}

/**
  Initialize the set vector of the list, and reload the set
  using the string of the list.

  @param list_str  The string of list
                   (e.g., "xx,yy,zz")
  @param list_set  The set vector of list
  @param lock      The lock of set
  @param key       The PSI_memory_key
  @param case_insensitive  Ignore upper/lower case

  @retval TRUE  valid
  @retval FALSE invalid
*/
bool list_init_and_reload(
#ifndef NDEBUG
    const char *dbug_str,
#endif
    const char *list_str, std::set<std::string> *list_set, mysql_rwlock_t *lock,
    PSI_memory_key key, bool case_insensitive) {
  mysql_rwlock_wrlock(lock);
  list_set->clear();
  uint len = strlen(list_str);
  char *origin_ptr = static_cast<char *>(my_malloc(key, len + 1, MYF(0)));
  // Actions needed to cleanup before leaving scope.
  auto cleanup_guard = create_scope_guard([&]() {
    mysql_rwlock_unlock(lock);
    my_free(origin_ptr);
    origin_ptr = nullptr;
  });

#ifndef NDEBUG
  if (dbug_str != nullptr) {
    DBUG_EXECUTE_IF(dbug_str, {
      my_free(origin_ptr);

      origin_ptr = nullptr;
    });
  }
#endif

  if (!(origin_ptr != nullptr &&
        strncpy_s(origin_ptr, len + 1, list_str, len) == EOK)) {
    return true;
  }

  char *last;
  char *input = origin_ptr;
  char *output = nullptr;
  while ((output = strtok_r(input, ",", &last)) != nullptr) {
    if (case_insensitive) my_caseup_str(system_charset_info, output);
    list_set->insert(std::string(output));
    input = nullptr;
  }

  return false;
}

template <class T>
std::unique_ptr<T> copy_unique(const std::unique_ptr<T> &source) {
  return source ? std::make_unique<T>(*source) : nullptr;
}

class RDS_reserved_users {
 public:
  RDS_reserved_users() = default;
  RDS_reserved_users(const RDS_reserved_users &) = delete;
  RDS_reserved_users &operator=(RDS_reserved_users const &) = delete;
  RDS_reserved_users(RDS_reserved_users &&) = delete;
  RDS_reserved_users &operator=(RDS_reserved_users &&) = delete;

  ~RDS_reserved_users() { release(); }

  /**
    Initialize hash and mutex.

    Called on startup.

    @return state
    @retval 0 success
    @retval >0 failure
  */
  int init() {
    int error = 0;
    if (initialized) {
      // NOLINTNEXTLINE
      sql_print_information("%s",
                            "RDS reserved user struct already initialized.");
      GOTO_ERR(0);
    }

    // NOLINTNEXTLINE(modernize-make-unique)
    user_list.push_back(std::unique_ptr<malloc_unordered_set<std::string>>(
        new malloc_unordered_set<std::string>(key_rds_user_memory)));
    // NOLINTNEXTLINE(modernize-make-unique)
    user_list.push_back(std::unique_ptr<malloc_unordered_set<std::string>>(
        new malloc_unordered_set<std::string>(key_rds_user_memory)));

    register_psi_keys();
    if (mysql_rwlock_init(key_rds_user_list_active, &active_lock)) {
      // NOLINTNEXTLINE
      sql_print_error("%s", "failure to init RDS reserved user lock.");
      GOTO_ERR(1);
    }
    if (mysql_mutex_init(key_rds_user_list_update, &update_mutex,
                         MY_MUTEX_INIT_FAST)) {
      // NOLINTNEXTLINE
      sql_print_error("%s", "failure to init RDS reserved user mutex.");
      GOTO_ERR(2);
    }
    active_list_index = 0;
    initialized = true;
  err:
    switch (error) {
      case 2:
        mysql_rwlock_destroy(&active_lock);
        [[fallthrough]];
      case 1:
        user_list.clear();
        [[fallthrough]];
      default:
        break;
    }
    return error;
  }

  /**
    Release hash and mutex (Coverity does not like free()).

    Called on server shutdown.
  */
  void release() {
    if (initialized) {
      user_list.clear();
      mysql_rwlock_destroy(&active_lock);
      mysql_mutex_destroy(&update_mutex);
      active_list_index = 0;
      initialized = false;
    }
  }

  /**
    Generate new reserved users according to given users' string.
    Remove old reserved users.

    @return state
    @retval 0 success
    @retval >0 failure
  */
  int update(const char *reserved_users) {
    char *origin_ptr = nullptr;
    const char *delim = ",";
    char *input = nullptr;
    char *output = nullptr;
    uint len = 0;
    int error = 0;

    if (!initialized) {
      sql_print_error("%s", "RDS reserved user struct is not initialized.");
      return 1;
    }

    (void)mysql_mutex_lock(&update_mutex);

    DBUG_ASSERT(user_list.size() == 2);  // NOLINT

    std::unique_ptr<malloc_unordered_set<std::string>> update_list = nullptr;

    len = (nullptr == reserved_users) ? 0 : strlen(rds_reserved_users_ptr);
    if (0 == len) {
      user_list[(active_list_index + 1) % 2]->clear();
      sql_print_information("%s", "RDS reserved user list is empty.");
      GOTO_ERR(0);
    }
    /* +1 the terminating zero */
    origin_ptr =
        static_cast<char *>(my_malloc(key_rds_user_memory, len + 1, MYF(0)));
    if (origin_ptr == nullptr) {
      sql_print_error(
          "%s",
          "failure to allocate memory while updating RDS reserved user list.");
      GOTO_ERR(1);
    }

    if (my_strncpy(origin_ptr, len + 1, reserved_users, len) == NULL) {
      sql_print_error("%s",
                      "failure to copy given string while updating RDS "
                      "reserved user list.");
      GOTO_ERR(1);
    }
    input = origin_ptr;

    update_list = std::move(user_list[(active_list_index + 1) % 2]);
    update_list->clear();

    while ((output = strtok(input, delim)) != nullptr) {
      if (push_user(output, update_list) != 0) {
        error = 2;
        /**
          Current list is corrupted, But if error != 0,
          active_list_index will not change, next time we still modify
          this list.
        */
        break;
      }
      input = nullptr;
    }

    user_list[(active_list_index + 1) % 2] = std::move(update_list);
  err:
    // switch user list if no error
    if (0 == error) {
      (void)mysql_rwlock_wrlock(&active_lock);
      active_list_index = (active_list_index + 1) % 2;
      (void)mysql_rwlock_unlock(&active_lock);
    }
    (void)mysql_mutex_unlock(&update_mutex);
    if (origin_ptr != nullptr) {
      my_free(origin_ptr);
    }
    return error;
  }

  /**
    Check if given user name is an reserved user name.

    @retval ture yes
    @retval false no
  */
  bool is_user_reserved(const char *user) {
    bool ret = false;
    if (!initialized) {
      return ret;
    }
    (void)mysql_rwlock_rdlock(&active_lock);

    if (!user_list[active_list_index]->empty()) {
      ret = (user_list[active_list_index]->end() !=
             user_list[active_list_index]->find(user));
    }
    (void)mysql_rwlock_unlock(&active_lock);
    return ret;
  }

 private:
  /**
    register psi keys
  */
  void register_psi_keys() {
#ifdef HAVE_PSI_INTERFACE
    int count = 0;
    const char *category = "rds_user";
    PSI_rwlock_info all_rds_rwlocks[] = {
        {&key_rds_user_list_active, "rds_user_active_lock", 0,
         PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME}};
    count = sizeof(all_rds_rwlocks) / sizeof(all_rds_rwlocks[0]);
    mysql_rwlock_register(category, all_rds_rwlocks, count);  // NOLINT
    PSI_mutex_info all_rds_mutexes[] = {
        {&key_rds_user_list_update, "rds_user_update_mutex", 0,
         PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME}};
    count = sizeof(all_rds_mutexes) / sizeof(all_rds_mutexes[0]);
    mysql_mutex_register(category, all_rds_mutexes, count);  // NOLINT
    static PSI_memory_info all_rds_memorys[] = {
        {&key_rds_user_memory, "rds_user_memory", 0, PSI_VOLATILITY_UNKNOWN,
         PSI_DOCUMENT_ME}};
    count = sizeof(all_rds_memorys) / sizeof(all_rds_memorys[0]);
    mysql_memory_register(category, all_rds_memorys, count);  // NOLINT
#endif
  }

  /**
    Add given user into the given hash

    @param  user   given user name
    @param  hash   given hash
    @return        state
    @retval 0 success
    @retval >0 failure
  */
  static int push_user(
      const char *user,
      const std::unique_ptr<malloc_unordered_set<std::string>> &hash) {
    LEX_STRING *new_elt = nullptr;
    char *new_elt_buffer = nullptr;
    size_t user_len = strlen(user);
    int error = 0;
    if (user_len == 0u) {
      // NOLINTNEXTLINE
      sql_print_error(
          "%s", "Found empty string while updating RDS reserved user list.");
      GOTO_ERR(1);
    }
    // NOLINTNEXTLINE
    if (my_multi_malloc(key_rds_user_memory, MYF(0), &new_elt,
                        sizeof(LEX_STRING), &new_elt_buffer, user_len + 1,
                        NullS) == nullptr) {
      // NOLINTNEXTLINE
      sql_print_error(
          "%s",
          "failure to allocate memory while updating RDS reserved user list.");
      GOTO_ERR(1);
    }
    new_elt->str = new_elt_buffer;

    if (my_strncpy(new_elt_buffer, user_len + 1, user, user_len) == NULL) {
      // NOLINTNEXTLINE
      sql_print_error("%s",
                      "failure to copy given string while updating RDS "
                      "reserved user list.");
      GOTO_ERR(1);
    }
    new_elt->length = user_len;

    if (!hash->emplace(new_elt->str).second) {
      sql_print_warning("%s",
                        "Skip duplicate user while updating RDS "
                        "reserved user list.");
    }
    my_free(new_elt);
    new_elt = nullptr;
  err:
    if (nullptr != new_elt && 0 != error) {
      my_free(new_elt);
    }
    return error;
  }

 private:
  bool initialized{false};
  std::vector<std::unique_ptr<malloc_unordered_set<std::string>>> user_list;

  int active_list_index{0};      // indicate which hash is in use
  mysql_rwlock_t active_lock{};  // protect the active hash
  mysql_mutex_t update_mutex;    // protect from concurrent updating
};

RDS_reserved_users *get_rds_reserved_users() {
  static RDS_reserved_users s_rds_users;
  return &s_rds_users;
}

/**
  Initialize the reserved user

  Called when server startup.

  @return state
  @retval 0 success
  @retval >0 failure
*/
int rds_reserved_users_init() { return get_rds_reserved_users()->init(); }

/**
  Free the reserved users

  Called at server shutdown.

  @return state
  @retval 0 success
  @retval >0 failure
*/
void rds_reserved_users_free() { get_rds_reserved_users()->release(); }

/**
  Update reserved users according to given string

  @return state
  @retval 0 success
  @retval >0 failure
*/
int rds_reserved_users_update(const char *users) {
  return get_rds_reserved_users()->update(users);
}

/**
  Check if a user is in the hash of reserved users.

  @return search result
  @retval TRUE  found
  @retval FALSE not found
*/
bool is_rds_reserved_user(const char *user) {
  bool ret = false;
  if (nullptr != user && 0 != strlen(user)) {
    ret = get_rds_reserved_users()->is_user_reserved(user);
  }
  return ret;
}

/**
  Validate the value for the system variable for RDS reserved users

  Note: the system variable for RDS reserved users (rds_reserved_users)
  is for internal use, and to simplify the logic, we do not allow
  whitespace as well as empty name in it (Documented).

  @param names  The string value for the system variable for the
                RDS reserved users (e.g., "admin1,admin2,admin3")
  @param len    the length of the names string

  @retval TRUE  valid
  @retval FALSE invalid
*/
bool is_valid_rds_reserved_users(const char *names, size_t len) {
  return is_valid_list_value(names, len);
}

/**
  Check if local user has the given privilege
*/
int check_global_access_noerr(THD *thd, Access_bitmask want_access) {
  DBUG_ENTER("check_global_access_noerr");  // NOLINT
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (thd->security_context()->check_access(want_access)) {
    DBUG_RETURN(0);
  }
  DBUG_RETURN(1);
#else
  DBUG_RETURN(0);
#endif
}

/**
   Access check regarding the RDS reserved users

   @retval
    0  ok
   @retval
    1  Access denied.  In this case an error is sent to the client
 */
int check_reserved_user(THD *thd, List<LEX_USER> &list, const char *command) {
  DBUG_ENTER("check_reserved_user");  // NOLINT
  LEX_USER *tmp_user_name;
  List_iterator<LEX_USER> user_list(list);

  if (check_global_access_noerr(thd, SUPER_ACL) == 0) {
    DBUG_RETURN(0);
  }

  LEX_CSTRING sec_user = thd->security_context()->user();
  if (nullptr != sec_user.str && 0 != sec_user.length &&
      is_rds_reserved_user(sec_user.str)) {
    DBUG_RETURN(0);
  }

  while ((tmp_user_name = user_list++) != nullptr) {
    if (tmp_user_name->user.str != nullptr) {
      if (is_rds_reserved_user(tmp_user_name->user.str)) {
        char account[HOSTNAME_LENGTH + USERNAME_CHAR_LENGTH + 6];
        my_snprintf(account, sizeof(account), sizeof(account), "'%s'@'%s'",
                    tmp_user_name->user.str, tmp_user_name->host.str);

        my_error(ER_CANNOT_USER, MYF(0), command, account);  // NOLINT
        DBUG_RETURN(1);
      }
    }
  }

  DBUG_RETURN(0);
}

/**
   @retval
    0  ok
   @retval
    1  Access denied.
 */
int check_reserved_user_db(THD *thd, const char *user, const char *db) {
  DBUG_ENTER("check_reserved_user_db");  // NOLINT
  if (check_global_access_noerr(thd, SUPER_ACL) == 0) {
    DBUG_RETURN(0);
  }

  LEX_CSTRING sec_user = thd->security_context()->user();
  if (nullptr != sec_user.str && is_rds_reserved_user(sec_user.str)) {
    DBUG_RETURN(0);
  }

  if (nullptr == user || !is_rds_reserved_user(user)) {
    DBUG_RETURN(0);
  }

  Security_context *sctx = thd->security_context();
  // NOLINTNEXTLINE
  my_error(ER_DBACCESS_DENIED_ERROR, MYF(0), sctx->priv_user().str,
           sctx->priv_host().str, db);
  DBUG_RETURN(1);
}

/**
  Access check regarding the RDS reserved users for uninstall plugin or
  component

  @retval 0  ok
  @retval 1  Access denied.  In this case an error is sent to the client
 */
int check_install_uninstall_plugin_user(THD *thd, const char *command) {
  DBUG_ENTER("check_plugin_reserved_user");

  if (!check_global_access_noerr(thd, SUPER_ACL)) {
    DBUG_RETURN(0);
  }

  LEX_CSTRING sec_user = thd->security_context()->user();
  LEX_CSTRING sec_host = thd->security_context()->host();
  if (NULL != sec_user.str && 0 != sec_user.length &&
      is_rds_reserved_user(sec_user.str)) {
    DBUG_RETURN(0);
  } else {
    char account[HOSTNAME_LENGTH + USERNAME_CHAR_LENGTH + 6];
    (void)my_sprintf(account, sizeof(account), "'%s'@'%s'", sec_user.str,
                     sec_host.str);
    my_error(ER_CANNOT_USER, MYF(0), command, account);
    DBUG_RETURN(1);
  }
}

/*
    @return
    @retval 0 OK
    @retval 1  Access denied; But column or routine privileges might need to
      be checked also.
*/
int check_internal_db(THD *thd, const char *db) {
  DBUG_ENTER("check_internal_db");  // NOLINT
  if (check_global_access_noerr(thd, SUPER_ACL) == 0) {
    DBUG_RETURN(0);
  }

  if (is_rds_reserved_user(thd->security_context()->user().str)) {
    DBUG_RETURN(0);
  }

  if (!is_perfschema_db(db) && !is_mysql_db(db) && !is_sys_db(db) &&
      !is_infoschema_db(db)) {
    DBUG_RETURN(0);
  }
  // NOLINTNEXTLINE
  my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
           thd->security_context()->user().str,
           thd->security_context()->host_or_ip().str, db);
  DBUG_RETURN(1);
}

/**
  RDS Unrestricted Dynamic Privileges
*/

#define RDS_RESTRICTED_DYNAMIC_PRIVILEGES_LIST_MAX_LENGTH 1024

bool is_valid_dynamic_privilege(THD *thd, const char *name) {
  return iterate_all_dynamic_privileges(
      thd,
      /*
        For each registered dynamic privilege
        check if it is the input name.
      */
      [name](const char *str) -> bool { return !strcasecmp(str, name); });
}

bool is_valid_dynamic_privileges_list(THD *thd, const char *names, size_t len) {
  // Check whether the value is a valid list
  if (len > RDS_RESTRICTED_DYNAMIC_PRIVILEGES_LIST_MAX_LENGTH ||
      !is_valid_list_value(names, len))
    return false;

  char origin_ptr[RDS_RESTRICTED_DYNAMIC_PRIVILEGES_LIST_MAX_LENGTH] = {0};
  if (strncpy_s(origin_ptr, len + 1, names, len) != EOK) return false;

  char *input = origin_ptr;
  char *output = nullptr;
  char *last = nullptr;
  // Check whether each item is a dynamic privilege name
  while ((output = my_strtok_r(input, ",", &last)) != nullptr) {
    if (!is_valid_dynamic_privilege(thd, output)) return false;
    input = nullptr;
  }
  return true;
}

bool is_in_restricted_dynamic_privileges_list(const char *name) {
  bool ret = false;
  if (NULL == name || 0 == strlen(name)) return ret;

  mysql_rwlock_rdlock(&LOCK_restricted_dynamic_privileges_list);
  /**
    Because the name is from system dynamic privileges, which is stored in
    uppercase, and the SET is also stored in uppercase, therefore, there is no
    need to convert the letter case.
  */
  if (restricted_dynamic_privileges_set.find(name) !=
      restricted_dynamic_privileges_set.end()) {
    ret = true;
  }
  mysql_rwlock_unlock(&LOCK_restricted_dynamic_privileges_list);
  return ret;
}

bool rds_restricted_dynamic_privileges_list_init_and_reload() {
  return list_init_and_reload(
#ifndef NDEBUG
      "simulate_restricted_dynamic_privileges_list_malloc_fail",
#endif
      rds_restricted_dynamic_privileges_list_ptr,
      &restricted_dynamic_privileges_set,
      &LOCK_restricted_dynamic_privileges_list,
      key_memory_restricted_dynamic_privileges_list,
      true);  // Case Insensitive
}  // RDS Unrestricted Dynamic Privileges End

/**
   Check whether the current user is a reserved user or a user with
   super privileges.
   Except for reserved users and users with super privileges, table
   schema changes, data modifications, and even drop database operations
   are forbidden to the internal databases(mysql/sys).

   @param thd  Thread handler
   @return
   @retval 0  normal user
   @retval 1  is a super or reserved user
 */
bool precheck_user_permission(THD *thd) {
  return (is_rds_reserved_user(thd->security_context()->user().str) ||
          !check_global_access_noerr(thd, SUPER_ACL));
}

/**
   Check whether the current user has access to internal schema.
   When rds_internal_schema_ddl_control is set to 1, don't allow any
   DDL privileges on the internal schema, when rds_internal_schema_dml_control
   is set to 1, don't allow any write privileges on the internal schema.
   for normal users which mean non-reserved and non-super users.

   @param want_access  The privilege needs to be checked

   @return
   @retval false  OK
   @retval true   Access denied
 */
bool check_internal_schema_permission(Access_bitmask want_access) {
  bool check_ddl_permission = false;
  bool check_dml_permission = false;
  if (opt_rds_internal_schema_ddl_control) {
    check_ddl_permission = unlikely(want_access & DDL_OP_ACLS);
  }
  if (opt_rds_internal_schema_dml_control) {
    check_dml_permission = unlikely(want_access & DML_WR_OP_ACLS);
  }
  return (check_ddl_permission || check_dml_permission);
}
