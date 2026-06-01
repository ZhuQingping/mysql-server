/* Copyright (c) 2023, 2024, Huawei Technologies Co., Ltd. All Rights Reserved.

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

#ifndef SQL_RECYCLE_BIN_RECYCLE_INCLUDED
#define SQL_RECYCLE_BIN_RECYCLE_INCLUDED

#include "mysql/components/services/bits/psi_memory_bits.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"

class THD;

namespace im {

namespace recycle_bin {

/* Init recycle bin scheduler context */
void recycle_scheduler_init();

/* Deinit recycle bin scheduler context */
void recycle_scheduler_deinit();

/* Start recycle scheduler thread */
bool recycle_scheduler_start(bool bootstrap);

/* Recycle bin memory instrument */
extern PSI_memory_key key_memory_recycle;

enum recycle_bin_cmd_type {
  RECYCLE_BIN_NONE = 0,
  RECYCLE_BIN_RECYCLE_TABLE = 1,
  RECYCLE_BIN_RECYCLE_DB = 2,
  RECYCLE_BIN_PURGE = 4,
  RECYCLE_BIN_RESTORE_TABLE = 8,
  RECYCLE_BIN_RESTORE_DB = 16,
  RECYCLE_BIN_TRUNCATE_TABLE = 32
};

/* Thread local state */
class Recycle_state {
 public:
  explicit Recycle_state() : m_type(RECYCLE_BIN_NONE) {}

  explicit Recycle_state(const Recycle_state &other) : m_type(other.m_type) {}

  void set_type(recycle_bin_cmd_type type) { m_type = type; }

  recycle_bin_cmd_type get_type() { return m_type; }

  bool is_priv_relax() {
    return (m_type & RECYCLE_BIN_PURGE) ||
           (m_type & RECYCLE_BIN_RESTORE_TABLE) ||
           (m_type & RECYCLE_BIN_RESTORE_DB);
  }

  void reset() { m_type = RECYCLE_BIN_NONE; }

  bool is_recycle() {
    return m_type == RECYCLE_BIN_RECYCLE_TABLE ||
           m_type == RECYCLE_BIN_RECYCLE_DB ||
           m_type == RECYCLE_BIN_TRUNCATE_TABLE;
  }

  bool is_restore() {
    return m_type == RECYCLE_BIN_RESTORE_TABLE ||
           m_type == RECYCLE_BIN_RESTORE_DB;
  }

  bool is_purge() { return m_type == RECYCLE_BIN_PURGE; }

  void set_autoinc_flag(bool has_autoinc) { m_has_autoinc = has_autoinc; }

  bool has_autoinc_col() { return m_has_autoinc; }

 private:
  /* label thread state to treat specially */
  recycle_bin_cmd_type m_type;
  bool m_has_autoinc = false;
};

class Thd_recycle_state_guard {
  THD *m_thd;
  enum_sql_command m_cmd;
  recycle_bin_cmd_type m_type;

 public:
  Thd_recycle_state_guard(THD *thd) {
    m_thd = thd;
    m_cmd = thd->lex->sql_command;
    m_type = thd->recycle_state->get_type();
  }
  ~Thd_recycle_state_guard() {
    m_thd->lex->sql_command = m_cmd;
    m_thd->recycle_state->set_type(m_type);
  }
};

/* Lex local state */
class Recycle_lex {
 public:
  explicit Recycle_lex(THD *thd) : m_thd(thd), m_backed_up_lex(thd->lex) {
    thd->lex = &m_lex;
    lex_start(thd);
  }
  ~Recycle_lex() {
    lex_end(&m_lex);
    m_thd->lex = m_backed_up_lex;
  }

 private:
  THD *m_thd;
  LEX *m_backed_up_lex;
  LEX m_lex;
};

} /* namespace recycle_bin */

} /* namespace im */
#endif
