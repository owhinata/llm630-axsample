#include <gtest/gtest.h>
#include <string.h>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

/**
 * @brief Case021: MmapFast address consistency and data parity.
 *
 * Purpose:
 * - Validate fast mapping parity with base and address stability.
 * Steps:
 * - Allocate base (non-cached) size=4MiB; MapViewFast(0,size,kNonCached).
 * - memset via fast view; compare base vs fast over full range.
 * - Map a second fast view and assert both addresses are identical.
 * Expected:
 * - Full-range equality and identical fast-mapped addresses.
 */
TEST(CmmFast, Case021_MmapFastAddressAndDataParity) {
  const uint32_t size = 4 * 1024 * 1024;
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(size, CacheMode::kNonCached, "cmm_021");
  ASSERT_TRUE(r);
  axsys::CmmView vbase = r.MoveValue();
  auto fr = buf.MapViewFast(0, size, CacheMode::kNonCached);
  ASSERT_TRUE(fr);
  axsys::CmmView vmap = fr.MoveValue();

  memset(vmap.Data(), 0x78, size);
  EXPECT_EQ(memcmp(vbase.Data(), vmap.Data(), size), 0);

  auto fr2 = buf.MapViewFast(0, size, CacheMode::kNonCached);
  ASSERT_TRUE(fr2);
  axsys::CmmView vmap2 = fr2.MoveValue();
  EXPECT_EQ(vmap.Data(), vmap2.Data());
}

/**
 * @brief Case022: MmapCacheFast address consistency.
 *
 * Purpose:
 * - Confirm cached-fast mappings for same range reuse the same address.
 * Steps:
 * - Allocate base (non-cached); MapViewFast(0,size,kCached) twice.
 * - Compare the two cached-fast virtual addresses.
 * Expected:
 * - Addresses are identical.
 */
TEST(CmmFast, Case022_MmapCacheFastAddressConsistency) {
  const uint32_t size = 4 * 1024 * 1024;
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(size, CacheMode::kNonCached, "cmm_022");
  ASSERT_TRUE(r);
  auto fr = buf.MapViewFast(0, size, CacheMode::kCached);
  ASSERT_TRUE(fr);
  axsys::CmmView v1 = fr.MoveValue();
  memset(v1.Data(), 0x78, size);
  auto fr2 = buf.MapViewFast(0, size, CacheMode::kCached);
  ASSERT_TRUE(fr2);
  axsys::CmmView v2 = fr2.MoveValue();
  EXPECT_EQ(v1.Data(), v2.Data());
}

/**
 * @brief Case023: MmapCacheFast + Flush + compare.
 *
 * Purpose:
 * - Ensure cached-fast writes become visible to base after Flush.
 * Steps:
 * - Allocate base (non-cached) size=1MiB; memset base to 0xFD.
 * - Map cached-fast view; memset cached-fast to 0xFE; Flush(cached-fast).
 * - Compare base vs cached-fast over full range.
 * Expected:
 * - Full-range equality.
 */
TEST(CmmFast, Case023_FastCachedFlushCompare) {
  const uint32_t size = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(size, CacheMode::kNonCached, "cmm_023");
  ASSERT_TRUE(r);
  axsys::CmmView vbase = r.MoveValue();
  memset(vbase.Data(), 0xfd, size);
  auto fr = buf.MapViewFast(0, size, CacheMode::kCached);
  ASSERT_TRUE(fr);
  axsys::CmmView vcache = fr.MoveValue();
  memset(vcache.Data(), 0xfe, size);
  ASSERT_TRUE(vcache.Flush());
  EXPECT_EQ(memcmp(vbase.Data(), vcache.Data(), size), 0);
}

/**
 * @brief Case024: MmapCacheFast + Invalidate + compare.
 *
 * Purpose:
 * - Ensure cached-fast reflects base writes after Invalidate.
 * Steps:
 * - Allocate base (non-cached) size=1MiB; MapViewFast(0,size,kCached).
 * - memset base=0xBC; memset cached-fast=0xFA; Invalidate(cached-fast).
 * - Write base=0xBB; compare base vs cached-fast over full range.
 * Expected:
 * - Full-range equality.
 */
TEST(CmmFast, Case024_FastCachedInvalidateCompare) {
  const uint32_t size = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(size, CacheMode::kNonCached, "cmm_024");
  ASSERT_TRUE(r);
  axsys::CmmView vbase = r.MoveValue();
  auto fr = buf.MapViewFast(0, size, CacheMode::kCached);
  ASSERT_TRUE(fr);
  axsys::CmmView vcache = fr.MoveValue();

  memset(vbase.Data(), 0xbc, size);
  memset(vcache.Data(), 0xfa, size);
  ASSERT_TRUE(vcache.Invalidate());
  memset(vbase.Data(), 0xbb, size);
  EXPECT_EQ(memcmp(vbase.Data(), vcache.Data(), size), 0);
}

}  // namespace
