/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2025 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_stats_sampler.cc
 *
 *
 * IDENTIFICATION
 * src/cde_stats_sampler.cc
 *
 * -------------------------------------------------------------------------
 */
#include "dict/cde_stats_sampler.h"
#include "common/cde_def.h"
#include "random"

namespace CDE {

constexpr uint32_t MAX_RANDOM_VALUE = 0x7FFFFFFF;

thread_local std::mt19937 g_randGen{std::random_device{}()};

void OptstatsBlockSamplerInit(BlockSampler bs, BlockNumber nblocks,
                              uint32_t samplesize, uint32_t seed) {
  /* measured table size */
  bs->N = nblocks;
  /* If we decide to reduce samplesize for tables that have less or not much
  more than samplesize blocks, here is the place to do it.*/
  bs->n = samplesize;
  /* blocks scanned so far */
  bs->t = 0;
  /* blocks selected so far */
  bs->m = 0;
  /* set seed */
  g_randGen.seed(seed);
}

/**
Select a random value R uniformly distributed in (0 - 1)
*/
double OptstatsAnlRandomFract() {
  std::uniform_int_distribution<uint32_t> dist(0, MAX_RANDOM_VALUE);
  return ((double)dist(g_randGen) + 1) / ((double)MAX_RANDOM_VALUE + 2);
}

bool OptstatsBlockSamplerHasMore(const BlockSampler bs) {
  return (bs->t < bs->N) && (bs->m < bs->n);
}

BlockNumber OptstatsBlockSamplerNext(BlockSampler bs) {
  /* remaining blocks */
  BlockNumber K = bs->N - bs->t;
  /* blocks still to sample */
  uint32_t k = bs->n - bs->m;
  /* probability to skip block */
  double p;
  /* random */
  double V;
  /* hence K > 0 and k > 0 */
  CDE_ASSERT(OptstatsBlockSamplerHasMore(bs));

  if ((BlockNumber)k >= K) {
    /* need all the rest */
    bs->m++;
    return bs->t++;
  }

  /* It is not obvious that this code matches Knuth's Algorithm S.
  Knuth says to skip the current block with probability 1 - k/K.
  If we are to skip, we should advance t (hence decrease K), and
  repeat the same probabilistic test for the next block.  The naive
  implementation thus requires an OptstatsAnlRandomFract() call for each block
  number.  But we can reduce this to one OptstatsAnlRandomFract() call per
  selected block, by noting that each time the while-test succeeds,
  we can reinterpret V as a uniform random number in the range 0 to p.
  Therefore, instead of choosing a new V, we just adjust p to be
  the appropriate fraction of its former value, and our next loop
  makes the appropriate probabilistic test.

  We have initially K > k > 0.  If the loop reduces K to equal k,
  the next while-test must fail since p will become exactly zero
  (we assume there will not be roundoff error in the division).
  (Note: Knuth suggests a "<=" loop condition, but we use "<" just
  to be doubly sure about roundoff error.)  Therefore K cannot become
  less than k, which means that we cannot fail to select enough blocks.*/
  V = OptstatsAnlRandomFract();
  p = 1.0 - (double)k / (double)K;
  while (V < p) {
    /* skip */
    bs->t++;
    /* keep K == N - t */
    K--;

    /* adjust p to be new cutoff point in reduced range */
    p *= 1.0 - (double)k / (double)K;
  }
  /* select */
  bs->m++;
  return bs->t++;
}
} /* namespace CDE */
