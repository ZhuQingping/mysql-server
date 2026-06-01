/* Copyright (c) 2016 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include <cstdlib>
#include <cstring>
#include "m_ctype.h"
#include "map_helpers.h"
#include "my_sys.h"
#include "my_user.h"
#include "mysql/components/services/bits/psi_rwlock_bits.h"
#include "mysql/components/services/bits/thr_rwlock_bits.h"
#include "mysql/plugin_audit.h"
#include "mysql/psi/mysql_rwlock.h"
#include "mysql_com.h"
#include "sql/mysqld.h"  // sql_statement_names

#include "filter.h"
#include "my_list.h"
#include "my_securec.h"
#define IPV6_MAX_LENGTH 45

using account = struct {
  /* user + '@' + host + '\0' */
  char name[USERNAME_LENGTH + HOSTNAME_LENGTH + 2];
  size_t length;
};

using database = struct {
  char name[NAME_LEN + 1];
  size_t length;
};

using command = struct {
  /* has to be enought to hold one of the com_status_vars names */
  char name[100];
  size_t length;
};

using ip = struct {
  /* has to be enought to hold one ipv6 length*/
  char name[IPV6_MAX_LENGTH + 1];
  size_t length;
};

bool exclude_commands_arr[static_cast<uint>(SQLCOM_END) + 1];
bool include_commands_arr[static_cast<uint>(SQLCOM_END) + 1];

static collation_unordered_map<std::string, account *> *include_accounts;
static collation_unordered_map<std::string, account *> *exclude_accounts;

static collation_unordered_map<std::string, database *> *include_databases;
static collation_unordered_map<std::string, database *> *exclude_databases;

static collation_unordered_map<std::string, command *> *include_commands;
static collation_unordered_map<std::string, command *> *exclude_commands;

static LIST *anonymized_ip;

#if defined(HAVE_PSI_INTERFACE)

static PSI_rwlock_key key_LOCK_account_list;
static PSI_rwlock_key key_LOCK_database_list;
static PSI_rwlock_key key_LOCK_command_list;
static PSI_rwlock_key key_LOCK_ip_list;
static PSI_rwlock_info all_rwlock_list[] = {
    {&key_LOCK_account_list, "audit_log_filter::account_list",
     PSI_FLAG_SINGLETON, PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME},
    {&key_LOCK_database_list, "audit_log_filter::database_list",
     PSI_FLAG_SINGLETON, PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME},
    {&key_LOCK_account_list, "audit_log_filter::command_list",
     PSI_FLAG_SINGLETON, PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME},
    {&key_LOCK_ip_list, "audit_log_filter::ip_list", PSI_FLAG_SINGLETON,
     PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME}};

#endif

mysql_rwlock_t LOCK_account_list;
mysql_rwlock_t LOCK_database_list;
mysql_rwlock_t LOCK_command_list;
mysql_rwlock_t LOCK_ip_list;

/*
  Initialize account
*/
static void account_init(account *acc, const char *user, size_t user_length,
                         const char *host, size_t host_length) {
  size_t max_length = sizeof(acc->name) - 2;
  host_length = (user_length + host_length > max_length)
                    ? (max_length - user_length)
                    : host_length;

  memcpy(static_cast<char *>(acc->name), user, user_length);
  memcpy(acc->name + user_length + 1, host, host_length);  // NOLINT
  acc->name[user_length] = '@';                            // NOLINT
  acc->name[user_length + host_length + 1] = 0;            // NOLINT
  acc->length = user_length + host_length + 1;
}

/*
  Allocate memory and initialize new account
*/

static account *account_create(const char *user, size_t user_length,
                               const char *host, size_t host_length) {
  auto *acc = static_cast<account *>(
      my_malloc(key_memory_audit_log_accounts, sizeof(account), MYF(MY_FAE)));

  account_init(acc, user, user_length, host, host_length);

  return acc;
}

/*
  Initialize database
*/
static void database_init(database *db, const char *name, size_t length) {
  size_t max_length = sizeof(db->name) - 1;
  length = (length > max_length) ? max_length : length;

  memcpy(static_cast<char *>(db->name), name, length);
  db->name[length] = 0;  // NOLINT
  db->length = length;
}

/*
  Allocate memory and initialize new database
*/

static database *database_create(const char *name, size_t length) {
  auto *db = static_cast<database *>(
      my_malloc(key_memory_audit_log_databases, sizeof(database), MYF(MY_FAE)));

  database_init(db, name, length);

  return db;
}

/*
  Initialize command
*/
static void command_init(command *cmd, const char *name, size_t length) {
  size_t max_length = sizeof(cmd->name) - 1;
  length = (length > max_length) ? max_length : length;

  memcpy(static_cast<char *>(cmd->name), name, length);
  cmd->name[length] = 0;  // NOLINT
  cmd->length = length;
}

/*
  Allocate memory and initialize new command
*/

static command *command_create(const char *name, size_t length) {
  auto *cmd = static_cast<command *>(
      my_malloc(key_memory_audit_log_commands, sizeof(command), MYF(MY_FAE)));

  command_init(cmd, name, length);

  return cmd;
}

static void ip_init(ip *output_ip, const char *name, size_t length) {
  size_t max_length = sizeof(output_ip->name) - 1;
  length = (length > max_length) ? max_length : length;
  memcpy(output_ip->name, name, length);
  output_ip->name[length] = 0;
  output_ip->length = length;
}

/*
  create ip key
*/
static ip *ip_create(const char *name, size_t length) {
  ip *obj_ip =
      (ip *)my_malloc(key_memory_audit_log_ip, sizeof(ip), MYF(MY_FAE));
  if (obj_ip == nullptr) return nullptr;
  ip_init(obj_ip, name, length);
  return obj_ip;
}

/*
  Remove enclosing quotes from string if any.
*/
static void unquote_string(char *string, size_t *string_length) {
  if (string[0] == '\'' && string[*string_length - 1] == '\'') {
    *string_length -= 2;
    my_memmove(string, *string_length, string + 1, *string_length);  // NOLINT
    string[*string_length] = 0;                                      // NOLINT
  }
}

static void account_map_reset(
    collation_unordered_map<std::string, account *> *hash) {
  DBUG_EXECUTE_IF("simulate_audit_account_map_nullptr", hash = nullptr;);
  if (hash) {
    for (auto it : *hash) {
      my_free(it.second);
    }
    hash->clear();
  }
}

/*
  Parse comma-separated list of accounts and add it into account list.
  Empty user name is allowed.
*/
static void account_list_from_string(
    collation_unordered_map<std::string, account *> *hash, const char *string) {
  char *string_copy = my_strdup(PSI_NOT_INSTRUMENTED, string, MYF(MY_FAE));
  char *entry = string_copy;
  int string_length = strlen(string_copy);
  char user[USERNAME_LENGTH + 1], host[HOSTNAME_LENGTH + 1];
  size_t user_length, host_length;

  account_map_reset(hash);

  while (entry - string_copy < string_length) {
    size_t entry_length = 0;
    bool quote = false;
    account *acc;

    while (*entry == ' ') {
      entry++;  // NOLINT
    }

    entry_length = 0;
    while (
        ((entry[entry_length] != ' ' && entry[entry_length] != ',') || quote) &&
        entry[entry_length] != 0) {
      if (entry[entry_length] == '\'') {
        quote = quote == 0;
      }
      entry_length++;
    }

    entry[entry_length] = 0;  // NOLINT

    parse_user(entry, entry_length, static_cast<char *>(user), &user_length,
               static_cast<char *>(host), &host_length);
    unquote_string(static_cast<char *>(user), &user_length);
    unquote_string(static_cast<char *>(host), &host_length);
    my_casedn_str(system_charset_info, host);

    acc = account_create(static_cast<char *>(user), user_length,
                         static_cast<char *>(host), host_length);
    auto res_pair = hash->emplace(
        std::string(static_cast<char *>(acc->name), acc->length), acc);
    if (!res_pair.second) {
      my_free(acc);
    }
    entry += entry_length + 1;  // NOLINT
  }

  my_free(string_copy);
}

static void database_map_reset(
    collation_unordered_map<std::string, database *> *hash) {
  DBUG_EXECUTE_IF("simulate_audit_database_map_nullptr", hash = nullptr;);
  if (hash) {
    for (auto it : *hash) {
      my_free(it.second);
    }
    hash->clear();
  }
}

static void database_list_from_string(
    collation_unordered_map<std::string, database *> *hash,
    const char *string) {
  const char *entry = string;

  database_map_reset(hash);

  while (*entry != 0) {
    size_t entry_length = 0;
    bool quote = false;
    char name[NAME_LEN + 1];
    size_t name_length = 0;

    while (*entry == ' ') {
      entry++;  // NOLINT
    }

    while (
        ((entry[entry_length] != ' ' && entry[entry_length] != ',') || quote) &&
        entry[entry_length] != 0) {
      if (quote && entry[entry_length] == '`' &&  // NOLINT
          entry[entry_length + 1] == '`') {       // NOLINT
        name[name_length++] = '`';                // NOLINT
        entry_length += 1;
      } else if (entry[entry_length] == '`') {  // NOLINT
        quote = quote == 0;
      } else if (name_length < NAME_LEN) {
        name[name_length++] = entry[entry_length];  // NOLINT
      }
      entry_length++;
    }

    if (name_length > 0) {
      database *db;
      name[name_length] = 0;  // NOLINT
      db = database_create(static_cast<char *>(name), name_length);
      auto res_pair = hash->emplace(
          std::string(static_cast<char *>(db->name), db->length), db);
      if (!res_pair.second) {
        my_free(db);
      }
    }

    entry += entry_length;  // NOLINT

    if (*entry == ',') {
      entry++;  // NOLINT
    }
  }
}

static void command_map_reset(
    collation_unordered_map<std::string, command *> *hash) {
  DBUG_EXECUTE_IF("simulate_audit_command_map_nullptr", hash = nullptr;);
  if (hash) {
    for (auto it : *hash) {
      my_free(it.second);
    }
    hash->clear();
  }
}

/*
  Parse comma-separated list of command and add it into command hash.
*/
static void command_list_from_string(
    collation_unordered_map<std::string, command *> *hash,
    bool *commands_filter_arr, const char *string) {
  const char *entry = string;
  int ret, i;
  command_map_reset(hash);
  my_memset(commands_filter_arr, SQLCOM_END + 1, 0, SQLCOM_END + 1);

  while (*entry != 0) {
    size_t len = 0;

    while (*entry == ' ' || *entry == ',') {
      entry++;  // NOLINT
    }

    while (entry[len] != ' ' && entry[len] != ',' && entry[len] != 0) {
      len++;
    }

    if (len > 0) {
      command *cmd = command_create(entry, len);
      my_casedn_str(&my_charset_utf8mb3_general_ci, cmd->name);

      for (i = 0; i < (SQLCOM_END + 1); i++)

      {
        ret = strcmp(sql_statement_names[i].str, cmd->name);  // NOLINT
        if (ret == 0) {
          commands_filter_arr[i] = true;  // NOLINT
          break;
        }
      }
      auto res_pair = hash->emplace(
          std::string(static_cast<char *>(cmd->name), cmd->length), cmd);
      if (!res_pair.second) {
        my_free(cmd);
      }
    }

    entry += len;  // NOLINT
  }
}
static void ip_list_from_string(LIST **root, const char *str) {
  LIST *pre_root = nullptr;
  list_free(*root, 1);
  *root = nullptr;

  while (*str) {
    size_t len = 0;

    while (*str == ' ' || *str == ',') str++;

    while (str[len] != ' ' && str[len] != ',' && str[len] != 0) len++;

    if (len > 0) {
      ip *obj = ip_create(str, len);
      if (obj == nullptr) continue;
      my_casedn_str(&my_charset_utf8mb3_general_ci, obj->name);

      pre_root = *root;
      if ((*root = list_cons((void *)obj, *root)) == nullptr) {
        my_free(obj);
        list_free(pre_root, 1);
        pre_root = nullptr;
      }
    }

    str += len;
  }
}

/* public interface */

void audit_log_filter_init() {
#ifdef HAVE_PSI_INTERFACE
  mysql_rwlock_register(AUDIT_LOG_PSI_CATEGORY,
                        static_cast<PSI_rwlock_info *>(all_rwlock_list),
                        array_elements(all_rwlock_list));
#endif /* HAVE_PSI_INTERFACE */
  mysql_rwlock_init(key_LOCK_account_list, &LOCK_account_list);
  mysql_rwlock_init(key_LOCK_database_list, &LOCK_database_list);
  mysql_rwlock_init(key_LOCK_command_list, &LOCK_command_list);
  mysql_rwlock_init(key_LOCK_ip_list, &LOCK_ip_list);

  include_accounts = new collation_unordered_map<std::string, account *>(
      &my_charset_bin, key_memory_audit_log_accounts);

  exclude_accounts = new collation_unordered_map<std::string, account *>(
      &my_charset_bin, key_memory_audit_log_accounts);

  include_databases = new collation_unordered_map<std::string, database *>(
      &my_charset_bin, key_memory_audit_log_databases);

  exclude_databases = new collation_unordered_map<std::string, database *>(
      &my_charset_bin, key_memory_audit_log_databases);

  include_commands = new collation_unordered_map<std::string, command *>(
      &my_charset_bin, key_memory_audit_log_commands);

  exclude_commands = new collation_unordered_map<std::string, command *>(
      &my_charset_bin, key_memory_audit_log_commands);
  anonymized_ip = nullptr;

  my_memset(exclude_commands_arr, sizeof(exclude_commands_arr), 0,
            sizeof(exclude_commands_arr));  // NOLINT
  my_memset(include_commands_arr, sizeof(include_commands_arr), 0,
            sizeof(include_commands_arr));  // NOLINT
}

void audit_log_filter_destroy() {
  account_map_reset(include_accounts);
  delete include_accounts;
  include_accounts = nullptr;

  account_map_reset(exclude_accounts);
  delete exclude_accounts;
  exclude_accounts = nullptr;

  database_map_reset(include_databases);
  delete include_databases;
  include_databases = nullptr;

  database_map_reset(exclude_databases);
  delete exclude_databases;
  exclude_databases = nullptr;

  command_map_reset(include_commands);
  delete include_commands;
  include_commands = nullptr;

  command_map_reset(exclude_commands);
  delete exclude_commands;
  exclude_commands = nullptr;
  list_free(anonymized_ip, 1);

  mysql_rwlock_destroy(&LOCK_account_list);
  mysql_rwlock_destroy(&LOCK_database_list);
  mysql_rwlock_destroy(&LOCK_account_list);
  mysql_rwlock_destroy(&LOCK_command_list);
  mysql_rwlock_destroy(&LOCK_ip_list);
}

/*
  Parse and store the list of included accounts.
*/
void audit_log_set_include_accounts(const char *val) {
  mysql_rwlock_wrlock(&LOCK_account_list);
  account_list_from_string(include_accounts, val);
  mysql_rwlock_unlock(&LOCK_account_list);
}

/*
  Parse and store the list of excluded accounts.
*/
void audit_log_set_exclude_accounts(const char *val) {
  mysql_rwlock_wrlock(&LOCK_account_list);
  account_list_from_string(exclude_accounts, val);
  mysql_rwlock_unlock(&LOCK_account_list);
}

/*
  Check if account has to be included.
*/
bool audit_log_check_account_included(const char *user, size_t user_length,
                                      const char *host, size_t host_length) {
  account acc, fuzzy_acc;
  bool res;

  account_init(&acc, user, user_length, host, host_length);

  account_init(&fuzzy_acc, user, user_length, "%%", strlen("%%"));

  if (acc.length == 0 || fuzzy_acc.length == 0) return false;

  mysql_rwlock_rdlock(&LOCK_account_list);

  const auto it = include_accounts->find(
      std::string(static_cast<char *>(acc.name), acc.length));
  res = it == include_accounts->end() ? false : true;
  const auto it1 = include_accounts->find(
      std::string(static_cast<char *>(fuzzy_acc.name), fuzzy_acc.length));
  res = res || (it1 == include_accounts->end() ? false : true);

  mysql_rwlock_unlock(&LOCK_account_list);
  return res;
}

/*
  Check if account has to be excluded.
*/
bool audit_log_check_account_excluded(const char *user, size_t user_length,
                                      const char *host, size_t host_length) {
  account acc, fuzzy_acc;

  bool res;

  account_init(&acc, user, user_length, host, host_length);
  account_init(&fuzzy_acc, user, user_length, "%%", strlen("%%"));

  if (acc.length == 0 || fuzzy_acc.length == 0) return false;

  mysql_rwlock_rdlock(&LOCK_account_list);

  const auto it = exclude_accounts->find(
      std::string(static_cast<char *>(acc.name), acc.length));
  res = (it == exclude_accounts->end()) ? false : true;
  const auto it1 = exclude_accounts->find(
      std::string(static_cast<char *>(fuzzy_acc.name), fuzzy_acc.length));
  res = res || (it1 == exclude_accounts->end() ? false : true);

  mysql_rwlock_unlock(&LOCK_account_list);
  return res;
}

/*
  Parse and store the list of included databases.
*/
void audit_log_set_include_databases(const char *val) {
  mysql_rwlock_wrlock(&LOCK_database_list);
  database_list_from_string(include_databases, val);
  mysql_rwlock_unlock(&LOCK_database_list);
}

/*
  Parse and store the list of excluded databases.
*/
void audit_log_set_exclude_databases(const char *val) {
  mysql_rwlock_wrlock(&LOCK_database_list);
  database_list_from_string(exclude_databases, val);
  mysql_rwlock_unlock(&LOCK_database_list);
}

/*
  Check if database has to be included.
*/
bool audit_log_check_database_included(const char *name, size_t length) {
  bool res;

  if (length == 0) {
    return false;
  }

  mysql_rwlock_rdlock(&LOCK_database_list);

  const auto it = include_databases->find(std::string(name, length));
  res = (it == include_databases->end()) ? false : true;

  mysql_rwlock_unlock(&LOCK_database_list);
  return res;
}

/*
  Check if database has to be excluded.
*/
bool audit_log_check_database_excluded(const char *name, size_t length) {
  bool res;

  if (length == 0) {
    return false;
  }

  mysql_rwlock_rdlock(&LOCK_database_list);

  const auto it = exclude_databases->find(std::string(name, length));
  res = (it == exclude_databases->end()) ? false : true;

  mysql_rwlock_unlock(&LOCK_database_list);
  return res;
}

/*
  Parse and store the list of included commands.
*/
void audit_log_set_include_commands(const char *val) {
  mysql_rwlock_wrlock(&LOCK_command_list);
  // no need extra lock for exclude_commands_arr since the type is char for each
  // member
  command_list_from_string(include_commands,
                           (bool *)include_commands_arr,  // NOLINT
                           val);
  mysql_rwlock_unlock(&LOCK_command_list);
}

/*
  Parse and store the list of excluded commands.
*/
void audit_log_set_exclude_commands(const char *val) {
  mysql_rwlock_wrlock(&LOCK_command_list);
  command_list_from_string(exclude_commands,
                           (bool *)exclude_commands_arr,  // NOLINT
                           val);
  mysql_rwlock_unlock(&LOCK_command_list);
}

/*
  Check if command has to be included.
*/
bool audit_log_check_command_included(const char *name, size_t length) {
  bool res;

  if (length == 0) {
    return false;
  }

  mysql_rwlock_rdlock(&LOCK_command_list);
  const auto it = include_commands->find(std::string(name, length));
  res = (it == include_commands->end()) ? false : true;
  mysql_rwlock_unlock(&LOCK_command_list);

  return res;
}

/*
  Check if command has to be excluded.
*/
bool audit_log_check_command_excluded(const char *name, size_t length) {
  bool res;

  if (length == 0) {
    return false;
  }

  mysql_rwlock_rdlock(&LOCK_command_list);
  const auto it = exclude_commands->find(std::string(name, length));
  res = (it == exclude_commands->end()) ? false : true;
  mysql_rwlock_unlock(&LOCK_command_list);

  return res;
}

/*
  Parse and store the list of anonymized_ip
*/
void audit_log_set_anonymized_ip(const char *val) {
  mysql_rwlock_wrlock(&LOCK_ip_list);
  ip_list_from_string(&anonymized_ip, val);
  mysql_rwlock_unlock(&LOCK_ip_list);
}
/*
  check if ip in the ip segment, if yes return 1, if no return 0
  eg: ip_segment : 100.%, obj_ip : 100.95.1.1
*/
int check_if_ip_in_ip_segment(void *ip_segment, void *obj_ip) {
  const char *first = (const char *)ip_segment;
  const char *second = (const char *)obj_ip;
  while (*first != 0 && *second != 0) {
    if (*first == *second) {
      first++;
      second++;
    } else {
      if (*first == '%')
        return 1;
      else
        return 0;
    }
  }
  if (*first || *second) return 0;
  return 1;
}

/*
  Check if ip has to be anonymized.
*/
bool audit_log_check_ip_anonymized(const char *name, size_t length) {
  bool res;

  if (length == 0) return false;

  mysql_rwlock_rdlock(&LOCK_ip_list);
  res = list_walk(anonymized_ip, check_if_ip_in_ip_segment,
                  (uchar *)(const_cast<char *>(name))) == 1;
  mysql_rwlock_unlock(&LOCK_ip_list);

  return res;
}
