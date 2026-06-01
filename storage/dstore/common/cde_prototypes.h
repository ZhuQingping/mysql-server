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

#ifndef __CDE_PROTOTYPES_H__
#define __CDE_PROTOTYPES_H__

#include "sql/field.h"

/**
@brief Check if the field is a virtual generated column.

@param[in] field  Field to be checked

@return true if is virtual generated column, otherwise false.
*/
static inline bool IsVirtualGeneratedField(const Field *field) {
  return field->gcol_info && !field->stored_in_db;
}

/**
@brief Check if the field is a stored generated column.

@param[in] field  Field to be checked

@return true if is stored generated column, otherwise false.
*/
static inline bool IsStoredGeneratedField(const Field *field) {
  return field->gcol_info && field->stored_in_db;
}

#endif  // __CDE_PROTOTYPES_H__
