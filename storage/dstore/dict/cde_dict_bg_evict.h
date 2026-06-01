/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2025 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_dict_bg_evict.h
 *
 *
 * -------------------------------------------------------------------------
 */
#ifndef __CDE_DICT_BG_EVICT_H__
#define __CDE_DICT_BG_EVICT_H__

#include <string>
namespace CDE {
/**
Start eviciton thread
 */
void CdeBgEvictSrvInit();
/**
Stop eviction thread
 */
void CdeBgEvictSrvDeinit();
} /* namespace CDE */

#endif
