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

#ifndef __CDE_TABLESPACE_H__
#define __CDE_TABLESPACE_H__

#include <functional>
#include <string>
#include <unordered_map>

#include "common/cde_def.h"
#include "common/cde_mutex.h"
#include "tablespace/dstore_tablespace_interface.h"

namespace CDE {

/** The name of the hard-coded temporary tablespace. */
extern const char *dstore_temp_spacename;

/** The name of the hard-coded default tablespace. */
extern const char *dstore_default_spacename;

/** The name of the data dictionary tablespace. */
extern const char *dstore_global_spacename;

/** Hash a NUL terminated 'string' */
struct CharPtrHash {
  /** Hashing function
  @param[in]    ptr             NUL terminated string to hash
  @return the hash */
  size_t operator()(const char *ptr) const {
    std::hash<std::string> stringHash;
    return stringHash(ptr);
  }
};

/** Compare two 'strings' */
struct CharPtrCompare {
  /** Compare two NUL terminated strings
  @param[in]    lhs             Left hand side
  @param[in]    rhs             Right hand side
  @return true if the contents match */
  bool operator()(const char *lhs, const char *rhs) const {
    return (strcmp(lhs, rhs) == 0);
  }
};

struct TableSpace {
  DSTORE::TablespaceId m_spaceId;
  uint64_t m_maxSize;
  char *m_spaceName = nullptr;
};

class TableSpaceMgr {
  using TablespaceIdCacheMap =
      std::unordered_map<DSTORE::TablespaceId, TableSpace *>;
  using TablespaceNameCacheMap =
      std::unordered_map<const char *, TableSpace *, CharPtrHash,
                         CharPtrCompare>;

 public:
  ~TableSpaceMgr();

  void Init();

  void InsertPreDefinedTablespaces(THD *thd);

  TableSpace *AddTableSpace(const char *spaceName, uint64_t maxSize,
                            DSTORE::TablespaceId spaceId);

  int UpdateTableSpaceName(const char *oldSpaceName, const char *newSpaceName);

  void DeleteTableSpace(DSTORE::TablespaceId spaceId);

  TableSpace *GetSpaceByName(const char *name);

  DSTORE::TablespaceId GetSpaceIdByName(const char *name);

 private:
  TablespaceIdCacheMap m_tsIdCache;
  TablespaceNameCacheMap m_tsNameCache;
  mysql_mutex_t m_mutex;
};

/**
Initialize the tablespacemgr, load all tablespace info from dd.
*/
void TableSpaceMgrInit();

/**
Returns the space ID based on the tablespace name.

@param[in] spaceName        tablespace name

@return space ID.
*/
DSTORE::TablespaceId CdeGetSpaceIdByName(const char *spaceName);

/**
Returns the space ID based on the dd table.

@param[in] dd_table         table dictionary object

@return space ID.
*/
template <typename Table>
DSTORE::TablespaceId CdeGetSpaceIdByDD(const Table *table);

/**
Delete tablespace by space id

@param[in] spaceId         delete tablespace id

@return CDE_SUCC if success, CDE_FAIL otherwise
*/
bool DeleteTableSpace(DSTORE::TablespaceId spaceId);

using HandleTableSpaceCallback =
    std::function<TableSpace *(const char *, uint64_t, DSTORE::TablespaceId)>;
/**
Load all created tablespaces from mysql.tablespaces, then callback handle it

@param[in] spaceId         thd
@param[in] callback        callback to handle every record of mysql.tablespaces

*/
void LoadAllCreatedSpace(THD *thd, HandleTableSpaceCallback callback);

}  // namespace CDE

#endif
