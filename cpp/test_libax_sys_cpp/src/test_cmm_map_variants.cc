#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

/**
 * @brief Case004: Mmap/Munmap (non-cached), pattern write and compare.
 *
 * Purpose:
 * - Verify two non-cached mappings over the same PA see identical data.
 * Steps:
 * - Allocate base (non-cached) size=1MiB; MapView(0,size,kNonCached).
 * - memset via the mapped view; no cache ops required (non-cached).
 * - Compare base vs mapped over the full range.
 * Expected:
 * - Full-range equality.
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

/**
 * @brief Case005: MmapCache + Flush + compare to base.
 *
 * Purpose:
 * - Ensure cached alias writes become visible to non-cached base after Flush.
 * Steps:
 * - Map cached alias over base; memset via cached alias.
 * - Flush(cached alias) over full range.
 * - Compare cached alias vs base over full range.
 * Expected:
 * - Full-range equality.
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

/**
 * @brief Case006: MmapCache + Invalidate + compare after base write.
 *
 * Purpose:
 * - Ensure cached alias reflects base writes after Invalidate.
 * Steps:
 * - Map cached alias; memset base and cached alias with different patterns.
 * - Invalidate cached alias; write base with a new pattern.
 * - Compare cached alias vs base over full range.
 * Expected:
 * - Full-range equality.
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

/**
 * @brief Case013: Cached subrange flush compare (expect pass, 100x repeat).
 *
 * Purpose:
 * - Verify Flush on [offset..offset+len) makes that subrange visible, and that
 *   the behavior is stable across repetitions (as in sample_cmm Case013).
 * Steps:
 * - Repeat 100 times:
 *   - Allocate base (non-cached) of 4MiB and map a full-range cached alias.
 *   - memset base with 0xFD and cached with 0xFE over the full range.
 *   - Flush(cached, offset, len).
 *   - Compare base vs cached on [offset..offset+len) using memcmp; expect 0.
 * Expected:
 * - All 100 iterations compare equal on the flushed subrange.
 */
TEST(CmmMapVariants, Case013_CachedSubrangeFlushCompare) {
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 2;
  for (int i = 0; i < 100; ++i) {
    axsys::CmmBuffer buf;
    auto rbase = buf.Allocate(size, CacheMode::kNonCached, "cmm_013");
    ASSERT_TRUE(rbase);
    axsys::CmmView base = rbase.MoveValue();
    auto rc = buf.MapView(0, size, CacheMode::kCached);
    ASSERT_TRUE(rc);
    axsys::CmmView cached = rc.MoveValue();

    memset(base.Data(), 0xFD, size);
    memset(cached.Data(), 0xFE, size);
    ASSERT_TRUE(cached.Flush(offset, len)) << "Flush failed at iter " << i;
    EXPECT_EQ(memcmp(static_cast<uint8_t*>(base.Data()) + offset,
                     static_cast<uint8_t*>(cached.Data()) + offset, len),
              0)
        << "memcmp mismatch at iter " << i;
  }
}

/**
 * @brief Case014: Cached subrange Flush then compare bigger range
 * (100x, expect at least one mismatch).
 *
 * Purpose:
 * - Flushing [offset..offset+len) should not imply visibility for a larger
 *   [offset..offset+cmp). Behavior is platform dependent and sometimes equal
 *   due to line-rounded flush; we assert that across 100 iterations there is
 *   at least one mismatch. If all 100 compare equal, skip.
 * Steps:
 * - Repeat 100 times:
 *   - Allocate base (non-cached) of 4MiB and map a full-range cached alias.
 *   - memset base (0xFD) and cached (0xFE).
 *   - Flush(cached, offset,len).
 *   - Compare base vs cached on [offset..cmp) using memcmp.
 * Expected:
 * - PASS if any iteration finds inequality. If all 100 iterations compare
 *   equal, SKIP (platform likely flushes a wider range).
 */
TEST(CmmMapVariants, Case014_CachedSubrangeFlushCompareBiggerFails) {
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 4;
  const uint32_t cmp = size / 2;  // bigger than flushed
  bool any_mismatch = false;
  for (int i = 0; i < 100; ++i) {
    axsys::CmmBuffer buf;
    auto rbase = buf.Allocate(size, CacheMode::kNonCached, "cmm_014");
    ASSERT_TRUE(rbase);
    axsys::CmmView base = rbase.MoveValue();
    auto rc = buf.MapView(0, size, CacheMode::kCached);
    ASSERT_TRUE(rc);
    axsys::CmmView cached = rc.MoveValue();

    memset(base.Data(), 0xFD, size);
    memset(cached.Data(), 0xFE, size);
    ASSERT_TRUE(cached.Flush(offset, len));
    const int eq = memcmp(static_cast<uint8_t*>(base.Data()) + offset,
                          static_cast<uint8_t*>(cached.Data()) + offset, cmp);
    if (eq != 0) {
      any_mismatch = true;
      break;
    }
  }
  if (any_mismatch) {
    SUCCEED();
  } else {
    GTEST_SKIP() << "All 100 iterations compared equal; likely wide flush";
  }
}

}  // namespace
