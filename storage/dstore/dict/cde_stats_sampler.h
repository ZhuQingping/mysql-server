/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2025 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_stats_sampler.h
 *
 *
 * -------------------------------------------------------------------------
 */
#ifndef __CDE_STATS_SAMPLER_H
#define __CDE_STATS_SAMPLER_H
#include "common/dstore_common_utils.h"

namespace CDE {

using DSTORE::BlockNumber;

/* Data structure for Algorithm S from Knuth 3.4.2 */
typedef struct {
  BlockNumber N; /* number of blocks, known in advance */
  uint32_t n;    /* desired sample size */
  BlockNumber t; /* current block number */
  uint32_t m;    /* blocks selected so far */
} BlockSamplerData;

typedef BlockSamplerData *BlockSampler;

/**
optstats_blocksampler_init -- prepare for random sampling of blocknumbers
BlockSampler is used for stage one of our new two-stage tuple
sampling mechanism as discussed on pgsql-hackers 2004-04-02 (subject
"Large DB").  It selects a random sample of samplesize blocks out of
the nblocks blocks in the table.  If the table has less than
samplesize blocks, all blocks are selected.

Since we know the total number of blocks in advance, we can use the
straightforward Algorithm S from Knuth 3.4.2, rather than Vitter's
algorithm.

@param[out]     bs   BlockSampler
@param[in]      nblocks  table total blocknum
@param[in]      samplesize  sample size
@param[in]      seed random seed
*/
void OptstatsBlockSamplerInit(BlockSampler bs, BlockNumber nblocks,
                              uint32_t samplesize, uint32_t seed);

/**
Check whether random numbers need to be generated.
@param[in]      bs  BlockSampler
@return true if need generated.
*/
bool OptstatsBlockSamplerHasMore(const BlockSampler bs);

/**
Randomly select the BlockNum to be sampled.

@param[in]      bs  BlockSampler
@return BlockNumber sample BlockNum
*/
BlockNumber OptstatsBlockSamplerNext(BlockSampler bs);

} /* namespace CDE */
#endif  // __CDE_STATS_SAMPLER_H
