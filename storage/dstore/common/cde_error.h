/*
  Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef __CDE_ERROR_H__
#define __CDE_ERROR_H__

#include <cstdint>

constexpr int CDE_ERROR = -1;
constexpr int CDE_OK = 0;

/** put new handler errorcode for dstore here. the value of new handler
 * errorcode should begin from 10000. enum elements are ordered by module of
 * dstore. */
enum class HaDstoreErrE : int32_t {
  HA_DSTORE_ERR_FIRST = 10000,

  HA_DSTORE_ERR_INDEX_FAIL_FOR_HUGE_INDEX_TUPLE,
  HA_DSTORE_ERR_TUPLE_TOO_MANY_COLUMNS,
  HA_DSTORE_ERR_TRX_UNABLE_TO_ACCESS_CONTINUOUSLY,
  HA_DSTORE_ERR_SWAT_HIT_DAMAGE_PAGE,
  HA_DSTORE_ERR_TUPLE_TOO_BIG,
  HA_DSTORE_ERR_SKIP_WAIT,
  HA_DSTORE_ERR_TABLESPACE_FULL,
  HA_DSTORE_ERR_INPLACE_TUPLESORT,
  HA_DSTORE_ERR_INPLACE_OUT_OF_RESOURCES,
  HA_DSTORE_ERR_INPLACE_GET_EXPRESSION_VAL,
  HA_DSTORE_ERR_INPLACE_INVALID_USE_OF_NULL,

  HA_DSTORE_ERR_LAST,
};

#endif  // __CDE_ERROR_H__
