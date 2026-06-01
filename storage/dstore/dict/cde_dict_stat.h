/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_dict_stat.h
 *
 *
 * -------------------------------------------------------------------------
 */

#ifndef __CDE_DICT_STAT_H__
#define __CDE_DICT_STAT_H__

#include "dict/cde_dict.h"
#include "handler.h"
#include "handler/ha_cde.h"
namespace CDE {
enum stat_update_option {
  RECALC_STATS,
  RECALC_PERSIST,
  EMPTY_TABLE,
  FETCH_STATS_FROM_PERSIST
};

/** Functor that compares two C strings.
 * Can be used as a comparator for
e.g. std::map that uses char* as keys.
*/
struct StrcmpFunctor {
  bool operator()(const char *a, const char *b) const {
    return (strcmp(a, b) < 0);
  }
};

/** Auxiliary map used for sorting indexes by name in dict_stats_save(). */
typedef std::map<const char *, cde_dict_index_t *, StrcmpFunctor> IndexMapT;

void CdeDictTableStatsInit(cde_dict_t *table, THD *thd);
int CdeDictTableStatsUpdate(cde_dict_t *table, stat_update_option opt,
                            THD *thd);
int CdeDictStatsSyncToHa(cde_dict_t *table, uint flag, ha_statistics &ha_stats);

/**
Recalculate statistical table information.

@param[in]      thd Connection contex.
@param[in,out]  table Pointer to the cde_dict_t object.
@param[in]      skipSample: true: skip sample not update table row
                                  and recalc others.
                            false: normally recalc. default: false

@return 0 on success, -1 on error.
*/
int CdeDictTableStatsRecalc(THD *thd, cde_dict_t *table,
                            bool skipSample = false);

/**
The first time the dictionary statistics information is initialized.
It is retrieved from the persistent table.

@param[in,out]  table Pointer to the cde_dict_t object.
@param[in]      thd Connection contex.

@return 0 on success, -1 on error.
*/
int CdeDictStatsFetchFromPs(cde_dict_t *table, THD *thd);

/**
Reset the statistics data of cde_dict_t.
e.g: when create table or truncate table.

@param[in,out]  table Pointer to the cde_dict_t object.

@return 0 on success, -1 on error.
*/
int CdeDictStatsReset(cde_dict_t *table);

/**
Delete the record from the dstore statistical information persistence table.

@param[in]      dbTableName The name of the table to drop.
@param[in]      thd Connection contex.

@return 0 on success, -1 on error.
*/
int DstoreDictStatDropTable(const std::string &dbTableName, THD *thd);

/**
Modify the table name in the statistical information persistence table.

@param[in]      thd Connection contex.
@param[in]      from The current table name.
@param[in]      to The new table name.

@return 0 on success, -1 on error.
*/
int DstoreDictStatRenameTable(THD *thd, const std::string &from,
                              const std::string &to);

/**
Removes the information for a particular index's stats from the persistent
storage if it exists and if there is data stored for this index.

@param[in]      thd Connection contex.
@param[in]      tableName The table name contains the index.
@param[in]      indexName The index name that to be dropped.

@return 0 on success, -1 on error.
*/
int DictStatDropIndex(THD *thd, cde_dict_t *dictTable,
                      const std::string &indexName);

/**
Renames an index in dstore persistent stats storage.

@param[in]      thd Connection contex.
@param[in]      tableName The table name contains the index.
@param[in]      oldIndexName The old index name that to be renamed.
@param[in]      newIndexName The new index name.

@return 0 on success, -1 on error.
*/
int DictStatRenameIndex(THD *thd, const std::string &tableName,
                        const std::string &oldIndexName,
                        const std::string &newIndexName);

/**
Since no dict_table_close(), deinitialize it explicitly.
Do same with innodb.

@param[in,out]  table Pointer to the cde_dict_t object.
*/
void DstoreDictTableStatsDeinit(cde_dict_t *table);

/**
Persist statistical information into the table.

@param[in,out]  table Pointer to the cde_dict_t object.
@param[in]      thd   Connection contex.
@param[in]      index Only update the index statistics if not nullptr.

@return 0 on success, -1 on error.
*/
int DstoreDictStatsSave(cde_dict_t *table, THD *thd,
                        cde_dict_index_t *index = nullptr);

/**
Fetches or calculates new estimates for index statistics.

@param[in]      table Pointer to the cde_dict_t object.
@param[in,out]  index Pointer to the cde_dict_index_t object.
@param[in]      thd Connection contex.
*/
void DstoreDictStatsUpdateForIndex(cde_dict_t *table, cde_dict_index_t *index,
                                   THD *thd);

/**
Reset index statistic value when create new index

@param[in]      index Pointer to the cde_dict_index_t object.
@return true for success
*/
int CdeDictStatsResetIndex(cde_dict_index_t *index);
} /* namespace CDE */
#endif
