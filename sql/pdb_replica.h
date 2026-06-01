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

#ifndef DEFINED_PDB_REPLICA_H
#define DEFINED_PDB_REPLICA_H

#include "sql/sql_class.h"

/**
  Do single pdb promote.

  @param[in] thd The thread context.
  @param[out] promote_info result info of pdb promote.

  @retval false success.
  @retval true failed.
*/
bool pdb_promote_main(THD *thd, char *&promote_info);

/**
  Do single pdb demote.

  @param[in] thd The thread context.
  @param[out] demote_info result info of pdb demote.

  @retval false success.
  @retval true failed.
*/
bool pdb_demote_main(THD *thd, char *&demote_info);

#endif