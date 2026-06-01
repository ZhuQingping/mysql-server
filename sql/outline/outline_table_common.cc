/* Copyright (c) 2023, Huawei and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/outline/outline_table_common.h"
#include "sql/outline/outline_digest.h"

#include "errmsg.h"
#include "my_sys.h"
#include "mysql/components/services/log_builtins.h"
#include "mysqld_error.h"
#include "mysys_err.h"

namespace im {

const char *outline_error_msg[] = {
    "Outline type or position is invalid",
    "Outline optimizer's hint or digest is invalid",
    "Outline index's hint, digest, or scope is invalid",
    "Cannot mix USE INDEX and FORCE INDEX"};

bool Outline_record::check_valid(const char **msg) const {
  if (type == Outline_type::UNKNOWN || pos < 1) {
    *msg = outline_error_msg[0];
    return false;
  } else if (type == Outline_type::OPTIMIZER) {
    if (blank(digest) || blank(hint)) {
      *msg = outline_error_msg[1];
      return false;
    }
    return true;
  } else {
    return index_hint_valid(msg);
  }
}

bool Outline_record::index_hint_valid(const char **msg) const {
  if (blank(digest) || scope.str.str == nullptr ||
      ((type != Outline_type::INDEX_USE) && blank(hint))) {
    *msg = outline_error_msg[2];
    return false;
  }

  System_outline *sys_outline = System_outline::instance();
  String_outline s_schema(schema.str ? schema.str : "");
  String_outline s_digest(digest.str);
  ulonglong slot = 1;
  Statement_outline *stmt_outline = sys_outline->find(slot, s_schema, s_digest);
  bool is_invalid = false;
  if (stmt_outline) {
    const Statement_outline_vector &outlines = stmt_outline->outlines();
    uint outline_size = outlines.size();
    uint outline_nr = 0;
    while (outline_nr < outline_size) {
      if (pos == outlines[outline_nr]->get_pos()) {
        if ((type == Outline_type::INDEX_USE &&
             outlines[outline_nr]->get_type() == Outline_type::INDEX_FORCE) ||
            (type == Outline_type::INDEX_FORCE &&
             outlines[outline_nr]->get_type() == Outline_type::INDEX_USE)) {
          is_invalid = true;
          *msg = outline_error_msg[3];
          break;
        }
      }
      outline_nr++;
    }
  }
  return !is_invalid;
}

/**
  Try to parse the optimizer hint and check the valid.

  @param[in]      thd       thread context

  @retval         true      valid
  @retval         false     not valid
*/
bool Outline_record::optimizer_hint_valid(THD *thd) {
  DBUG_ENTER("Outline_record::optimizer_hint_valid");
  LEX_CSTRING str =
      to_lex_cstring(strmake_root(thd->mem_root, hint.str, hint.length));

  if (parse_optimizer_hint(thd, str)) return true;

  return false;
}

} /* namespace im */
