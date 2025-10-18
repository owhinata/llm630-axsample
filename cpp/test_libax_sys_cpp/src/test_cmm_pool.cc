#include <gtest/gtest.h>

#include <cstdio>
#if defined(__has_include)
#if __has_include(<ax_pool_api.h>) && __has_include(<ax_sys_api.h>)
#include <ax_pool_api.h>
#include <ax_sys_api.h>
#define AXSYS_HAS_POOL 1
#endif
#endif
#include <inttypes.h>
#include <string.h>

namespace {

/*
 * Case020: POOL block mapping and cache ops.
 * Purpose:
 * - Acquire a POOL block, write via pool virtual; map phys non-cached and
 *   cached; perform Flush/Invalidate and cleanup.
 * Note:
 * - Skips if POOL headers/APIs are unavailable in this environment.
 */
TEST(CmmPool, Case020_PoolBlockMapAndCacheOps) {
#ifndef AXSYS_HAS_POOL
  GTEST_SKIP() << "POOL headers not available; skipping";
#else
  AX_POOL_FLOORPLAN_T plan;
  memset(&plan, 0, sizeof(plan));
  plan.CommPool[0].MetaSize = 0x1000;
  plan.CommPool[0].BlkSize = 3 * 1024 * 1024;
  plan.CommPool[0].BlkCnt = 1;
  plan.CommPool[0].CacheMode = AX_POOL_CACHE_MODE_NONCACHE;
  snprintf(reinterpret_cast<char*>(plan.CommPool[0].PartitionName),
           sizeof(plan.CommPool[0].PartitionName), "%s", "anonymous");

  if (AX_POOL_Exit() != 0) GTEST_SKIP() << "AX_POOL_Exit failed";
  if (AX_POOL_SetConfig(&plan) != 0) GTEST_SKIP() << "AX_POOL_SetConfig failed";
  if (AX_POOL_Init() != 0) GTEST_SKIP() << "AX_POOL_Init failed";

  AX_U32 blk_size = static_cast<AX_U32>(plan.CommPool[0].BlkSize);
  AX_BLK blk = AX_POOL_GetBlock(AX_INVALID_POOLID, blk_size, nullptr);
  if (blk == AX_INVALID_BLOCKID) {
    AX_POOL_Exit();
    GTEST_SKIP() << "AX_POOL_GetBlock failed";
  }

  AX_U64 phys = AX_POOL_Handle2PhysAddr(blk);
  ASSERT_NE(phys, 0u);
  void* pool_v = AX_POOL_GetBlockVirAddr(blk);
  ASSERT_NE(pool_v, nullptr);

  // write via pool virtual
  for (int i = 0; i < 20; ++i) {
    reinterpret_cast<int32_t*>(pool_v)[i] = i;
  }

  // map non-cached by phys and read
  void* v_nc = AX_SYS_Mmap(phys, blk_size);
  ASSERT_NE(v_nc, nullptr);
  // map cached by phys and read (flush/invalidate)
  void* v_c = AX_SYS_MmapCache(phys, blk_size);
  ASSERT_NE(v_c, nullptr);
  (void)AX_SYS_MflushCache(phys, v_c, blk_size);
  (void)AX_SYS_MinvalidateCache(phys, v_c, blk_size);
  AX_SYS_Munmap(v_c, blk_size);
  AX_SYS_Munmap(v_nc, blk_size);

  (void)AX_POOL_ReleaseBlock(blk);
  (void)AX_POOL_Exit();
  (void)AX_POOL_Exit();
#endif
}

}  // namespace
