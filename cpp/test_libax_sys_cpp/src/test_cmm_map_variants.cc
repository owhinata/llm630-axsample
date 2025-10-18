#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

/*
 * Case004: Mmap/Munmap (non-cached), pattern write and compare.
 * Purpose:
 * - Create second non-cached mapping; write pattern; full-range equals.
 */
TEST(CmmMapVariants, Case004_MapNonCachedAndCompare) {
  const uint32_t size = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  auto rbase = buf.Allocate(size, CacheMode::kNonCached, "cmm_004");
  ASSERT_TRUE(rbase) << rbase.Message();
  axsys::CmmView vbase = rbase.MoveValue();
  auto rmap = buf.MapView(0, size, CacheMode::kNonCached);
  ASSERT_TRUE(rmap) << rmap.Message();
  axsys::CmmView vmap = rmap.MoveValue();

  memset(vmap.Data(), 0x78, size);
  EXPECT_EQ(memcmp(vbase.Data(), vmap.Data(), size), 0);
}

/*
 * Case005: MmapCache + Flush + compare to base.
 * Purpose:
 * - Cached alias over non-cached base; Flush then equals base.
 */
TEST(CmmMapVariants, Case005_MapCachedFlushCompare) {
  const uint32_t size = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  auto rbase = buf.Allocate(size, CacheMode::kNonCached, "cmm_005");
  ASSERT_TRUE(rbase) << rbase.Message();
  axsys::CmmView vbase = rbase.MoveValue();
  auto rc = buf.MapView(0, size, CacheMode::kCached);
  ASSERT_TRUE(rc) << rc.Message();
  axsys::CmmView vcache = rc.MoveValue();

  memset(vcache.Data(), 0xfe, size);
  ASSERT_TRUE(vcache.Flush()) << "Flush failed";
  EXPECT_EQ(memcmp(vbase.Data(), vcache.Data(), size), 0);
}

/*
 * Case006: MmapCache + Invalidate + compare after base write.
 * Purpose:
 * - Invalidate cached alias; write base; full-range equals.
 */
TEST(CmmMapVariants, Case006_MapCachedInvalidateCompare) {
  const uint32_t size = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  auto rbase = buf.Allocate(size, CacheMode::kNonCached, "cmm_006");
  ASSERT_TRUE(rbase);
  axsys::CmmView vbase = rbase.MoveValue();
  auto rc = buf.MapView(0, size, CacheMode::kCached);
  ASSERT_TRUE(rc);
  axsys::CmmView vcache = rc.MoveValue();

  memset(vbase.Data(), 0xbc, size);
  memset(vcache.Data(), 0xfa, size);
  ASSERT_TRUE(vcache.Invalidate());
  memset(vbase.Data(), 0xbb, size);
  EXPECT_EQ(memcmp(vbase.Data(), vcache.Data(), size), 0);
}

/*
 * Case013: Cached subrange flush compare (expect pass in subrange).
 * Purpose:
 * - Flush [offset..offset+len); subrange equals.
 */
TEST(CmmMapVariants, Case013_CachedSubrangeFlushCompare) {
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 2;
  axsys::CmmBuffer buf;
  auto rbase = buf.Allocate(size, CacheMode::kNonCached, "cmm_013");
  ASSERT_TRUE(rbase);
  axsys::CmmView base = rbase.MoveValue();
  auto rc = buf.MapView(0, size, CacheMode::kCached);
  ASSERT_TRUE(rc);
  axsys::CmmView cached = rc.MoveValue();

  memset(base.Data(), 0xfd, size);
  memset(cached.Data(), 0xfe, size);
  ASSERT_TRUE(cached.Flush(offset, len));
  EXPECT_EQ(memcmp(static_cast<uint8_t*>(base.Data()) + offset,
                   static_cast<uint8_t*>(cached.Data()) + offset, len),
            0);
}

/*
 * Case014: Cached subrange flush then compare bigger range (expect fail).
 * Purpose:
 * - Compare [offset..cmp) with cmp>len; not equal.
 */
TEST(CmmMapVariants, Case014_CachedSubrangeFlushCompareBiggerFails) {
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 4;
  const uint32_t cmp = size / 2;  // bigger than flushed
  axsys::CmmBuffer buf;
  auto rbase = buf.Allocate(size, CacheMode::kNonCached, "cmm_014");
  ASSERT_TRUE(rbase);
  axsys::CmmView base = rbase.MoveValue();
  auto rc = buf.MapView(0, size, CacheMode::kCached);
  ASSERT_TRUE(rc);
  axsys::CmmView cached = rc.MoveValue();

  memset(base.Data(), 0xfd, size);
  memset(cached.Data(), 0xfe, size);
  ASSERT_TRUE(cached.Flush(offset, len));
  EXPECT_NE(memcmp(static_cast<uint8_t*>(base.Data()) + offset,
                   static_cast<uint8_t*>(cached.Data()) + offset, cmp),
            0);
}

}  // namespace
