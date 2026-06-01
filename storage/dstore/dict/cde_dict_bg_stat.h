/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_dict_bg_stat.h
 *
 *
 * -------------------------------------------------------------------------
 */
#ifndef __CDE_DICT_BG_STAT_H__
#define __CDE_DICT_BG_STAT_H__

#include <string>

namespace CDE {
int CdeBgStatSrvInit();
int CdeBgStatSrvDeinit();

struct TrxTable;
/**
 * add a table to background queue to recalc statistics.
 *
 * @param trxTable ref of TrxTable object.
 * @param table_name table name.
 * @param ignoreDataChanged false: Determine whether to add to the background
 * update queue based on whether the table data changes meet the specified
 * conditions. true: Ignore data changes and directly add to the update
 * statistics queue. for example: when adding index to flush histogram. default:
 * false.
 */
void CdeAddStatTable(const TrxTable &trxTable, bool ignoreDataChanged = false);

/**
 * @brief Remove statistics table
 * @param tableId table ID
 * @details This function is used to remove the statistics table of the
 * specified object ID. Ensure that the background statistics service is not
 * null, and then call the recalc_pool_del function to delete the object ID.
 */
void CdeRemoveStatTable(uint64_t tableId);
} /* namespace CDE */

#endif
