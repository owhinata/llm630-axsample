#include <gtest/gtest.h>
#include <string.h>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

/*
 * Case021: MmapFast address consistency and data parity.
 * Steps:
 * - Base non-cached + fast non-cached; write via fast; compare full range;
 *   map second fast view; addresses equal.
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

/*
 * Case022: MmapCacheFast address consistency (cached fast mapping).
 * Steps:
 * - Two cached fast views over same range must have identical addresses.
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

/*
 * Case023: MmapCacheFast + Flush + compare (SDK-aligned).
 * Steps:
 * - Base non-cached memset; cached-fast memset; Flush; full-range equals.
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

/*
 * Case024: MmapCacheFast + Invalidate + compare (SDK-aligned).
 * Steps:
 * - Init base/cache; Invalidate cache; write base; full-range equals.
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
