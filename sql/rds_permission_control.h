/*****************************************************************************

Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
Date: 2017.8.29

*****************************************************************************/

#ifndef RDS_PERMISSION_CONTROL_INCLUDED
#define RDS_PERMISSION_CONTROL_INCLUDED
#include "sql/sql_list.h"

#define RDS_ROOT_USER "root"

struct LEX;
struct Table_ref;
class THD;
struct LEX_USER;

/**
  Initialize the reserved user

  Called when server startup.

  @return state
  @retval 0 success
  @retval >=1 failure
*/
int rds_reserved_users_init();

/**
  Free the reserved user option variables.

  Called at server shutdown.
*/
void rds_reserved_users_free();

/**
  Update reserved users according to given string

  @param users  The string value of the users to update.

  @return state
  @retval 0 success
  @retval >0 failure
*/
int rds_reserved_users_update(const char *users);

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
bool is_valid_rds_reserved_users(const char *names, size_t len);

/**
  Check if local user has the given privilege

  @retval 0  have privilege.
  @retval 1  do not have privilege.
*/
int check_global_access_noerr(THD *thd, Access_bitmask want_access);

/**
   Access check regarding the RDS reserved users

   @retval 0  ok
   @retval 1  Access denied.  In this case an error is sent to the client
 */
int check_reserved_user(THD *thd, List<LEX_USER> &list, const char *command);

/**
   @retval 0  ok
   @retval 1  Access denied.
 */
int check_reserved_user_db(THD *thd, const char *user, const char *db);

/**
  Access check regarding the RDS reserved users for uninstall plugin or
  component

  @retval 0  ok
  @retval 1  Access denied.  In this case an error is sent to the client
 */
int check_install_uninstall_plugin_user(THD *thd, const char *command);

/*
  @return
    @retval 0 OK
    @retval 1  Access denied; But column or routine privileges might need to
      be checked also.
*/
int check_internal_db(THD *thd, const char *db);

/**
  Check if a user is in the hash of reserved users.

  @return search result
  @retval TRUE  found
  @retval FALSE not found
*/
bool is_rds_reserved_user(const char *user);
/**
  Validate whether the string value is dynamic privleges.

  @param name  The string of dynamic privilge
               (e.g., "session_variables_admin")
  @param len   the length of the name string

  @retval TRUE  valid
  @retval FALSE invalid
*/
bool is_valid_dynamic_privilege(THD *thd, const char *name);
/**
  Validate the value for the system variable for RDS restricted
  dynamic privleges.

  Note: the system variable (rds_restricted_dynamic_privileges_list)
  is not greater than 1024, and to simplify the logic, we do not allow
  whitespace as well as empty name in it (Documented).

  @param names  The string value for the system variable
                (e.g., "session_variables_admin,system_variables_admin")
  @param len    the length of the names string

  @retval TRUE  valid
  @retval FALSE invalid
*/
bool is_valid_dynamic_privileges_list(THD *thd, const char *names, size_t len);
/**
  Check if given dynamic privilege name is restricted.

  @retval TRUE  yes
  @retval FALSE no
*/
bool is_in_restricted_dynamic_privileges_list(const char *name);
/**
  Initialize and realod the restricted dynamic privileges

  Called when server startup.

  @return state
  @retval FALSE success
  @retval TRUE  failure
*/
bool rds_restricted_dynamic_privileges_list_init_and_reload();

/**
   Check whether the current user is a reserved user or a user with
   super privileges.
   Except for reserved users and users with super privileges, table
   schema changes, data modifications, and even drop database operations
   are forbidden to the Internal databases(mysql/sys).

   @param thd  Thread handler

   @return
   @retval 0  normal user
   @retval 1  is a super or reserved user
 */
bool precheck_user_permission(THD *thd);

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
bool check_internal_schema_permission(Access_bitmask want_access);

#endif  // RDS_PERMISSION_CONTROL_INCLUDED
