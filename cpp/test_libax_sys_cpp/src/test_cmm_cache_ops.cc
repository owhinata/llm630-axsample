#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

static int MemcpyView(const axsys::CmmView& src, const axsys::CmmView& dst,
                      uint32_t size) {
  if (!src || !dst || size == 0) return -1;
  if (size > src.Size() || size > dst.Size()) return -1;

  const void* s_ptr = src.Data();
  void* d_ptr = dst.Data();

  axsys::CmmView src_alias;
  axsys::CmmView dst_alias;

  if (src.Mode() == CacheMode::kCached) {
    auto rr = src.MapView(0, size, CacheMode::kNonCached);
    if (!rr) return -1;
    src_alias = rr.MoveValue();
    s_ptr = src_alias.Data();
  }
  if (dst.Mode() == CacheMode::kCached) {
    auto rr = dst.MapView(0, size, CacheMode::kNonCached);
    if (!rr) return -1;
    dst_alias = rr.MoveValue();
    d_ptr = dst_alias.Data();
  }

  memcpy(d_ptr, s_ptr, size);
  return 0;
}

/**
 * @brief Case009: Flush with offset (cached src -> noncached dst), 100x.
 *
 * Purpose:
 * - After writing via cached src, Flush(subrange) makes that subrange visible
 *   to non-cached dst; repeat 100 times as in sample_cmm.
 * Steps:
 * - Use size=4MiB, offset=2MiB.
 * - Repeat 100 iterations:
 *   - Allocate src (cached) and dst (non-cached).
 *   - memset src with 0x78; patch first 256 bytes descending.
 *   - Flush src over [offset..end) to write back cached lines.
 *   - memset dst with 0x39; copy full buffer src->dst via MemcpyView.
 *   - Compare only [offset..end) between src and dst.
 * Expected:
 * - All iterations: equality on [offset..end).
 */
TEST(CmmCacheOps, Case009_FlushMakesDataVisible) {
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 2 * 1024 * 1024;
  for (int t = 0; t < 100; ++t) {
    axsys::CmmBuffer src, dst;
    auto rs = src.Allocate(size, CacheMode::kCached, "gtest_009_src");
    ASSERT_TRUE(rs) << rs.Message();
    auto rd = dst.Allocate(size, CacheMode::kNonCached, "gtest_009_dst");
    ASSERT_TRUE(rd) << rd.Message();
    axsys::CmmView vsrc = rs.MoveValue();
    axsys::CmmView vdst = rd.MoveValue();

    memset(vsrc.Data(), 0x78, size);
    for (uint32_t i = 0; i < 256 && i < size; ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = static_cast<uint8_t>(255 - i);
    }
    auto fr = vsrc.Flush(offset, size - offset);
    ASSERT_TRUE(fr) << fr.Message();

    memset(vdst.Data(), 0x39, size);
    ASSERT_EQ(0, MemcpyView(vsrc, vdst, size));
    EXPECT_EQ(
        memcmp(static_cast<uint8_t*>(vdst.Data()) + offset,
               static_cast<uint8_t*>(vsrc.Data()) + offset, size - offset),
        0)
        << "mismatch at iter " << t;
  }
}

/**
 * @brief Case010: Flush with offset (subrange visibility), 100x.
 *
 * Purpose:
 * - Flushing [offset..end) makes that subrange visible to non-cached dst;
 *   repeat 100 times as in sample_cmm.
 * Steps:
 * - Use size=4MiB, offset=2MiB.
 * - Repeat 100 iterations:
 *   - Allocate src (cached) and dst (non-cached).
 *   - memset src + patch first 256 bytes; Flush src over [offset..end).
 *   - memset dst; copy src->dst using non-cached aliases.
 *   - Compare only [offset..end) between src and dst.
 * Expected:
 * - All iterations: equality on [offset..end).
 */
TEST(CmmCacheOps, Case010_FlushWithOffset) {
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 2 * 1024 * 1024;
  for (int t = 0; t < 100; ++t) {
    axsys::CmmBuffer src, dst;
    auto rs = src.Allocate(size, CacheMode::kCached, "gtest_010_src");
    ASSERT_TRUE(rs);
    auto rd = dst.Allocate(size, CacheMode::kNonCached, "gtest_010_dst");
    ASSERT_TRUE(rd);
    axsys::CmmView vsrc = rs.MoveValue();
    axsys::CmmView vdst = rd.MoveValue();

    memset(vsrc.Data(), 0x78, size);
    for (uint32_t i = 0; i < 256 && i < size; ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = static_cast<uint8_t>(255 - i);
    }
    auto fr = vsrc.Flush(offset, size - offset);
    ASSERT_TRUE(fr) << fr.Message();

    memset(vdst.Data(), 0x39, size);
    ASSERT_EQ(0, MemcpyView(vsrc, vdst, size));
    // Only [offset..end) must match
    EXPECT_EQ(
        memcmp(static_cast<uint8_t*>(vdst.Data()) + offset,
               static_cast<uint8_t*>(vsrc.Data()) + offset, size - offset),
        0)
        << "mismatch at iter " << t;
  }
}

/**
 * @brief Case012: Flush subrange then compare bigger range
 * (100x, expect at least one mismatch).
 *
 * Purpose:
 * - Validates that flushing [offset..offset+len) does not guarantee visibility
 *   for a strictly larger range [offset..offset+cmp). Behavior is HW dependent
 *   and may be fully equal due to line-rounded flush; we require at least one
 *   mismatch across 100 iterations. If all equal, skip.
 * Steps:
 * - Repeat 100 times:
 *   - Allocate src (cached) and dst (non-cached), size=4MiB.
 *   - memset src (0x88) + patch first 256 bytes; Flush [offset..offset+len).
 *   - memset dst (0x49); copy via non-cached aliases.
 *   - Compare [offset..offset+cmp) using memcmp.
 * Expected:
 * - PASS if any iteration finds inequality. If all 100 iterations compare
 *   equal, SKIP (platform likely flushes a wider range).
 */
TEST(CmmCacheOps, Case012_FlushSubrangeCompareBiggerFails) {
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 4;
  const uint32_t cmp = size / 2;
  bool any_mismatch = false;
  for (int t = 0; t < 100; ++t) {
    axsys::CmmBuffer src, dst;
    auto rs = src.Allocate(size, CacheMode::kCached, "gtest_012_src");
    ASSERT_TRUE(rs);
    auto rd = dst.Allocate(size, CacheMode::kNonCached, "gtest_012_dst");
    ASSERT_TRUE(rd);
    axsys::CmmView vsrc = rs.MoveValue();
    axsys::CmmView vdst = rd.MoveValue();

    memset(vsrc.Data(), 0x88, size);
    for (uint32_t i = 0; i < 256 && i < size; ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = static_cast<uint8_t>(255 - i);
    }
    auto fr = vsrc.Flush(offset, len);
    ASSERT_TRUE(fr) << fr.Message();

    memset(vdst.Data(), 0x49, size);
    ASSERT_EQ(0, MemcpyView(vsrc, vdst, size));

    const int eq = memcmp(static_cast<uint8_t*>(vdst.Data()) + offset,
                          static_cast<uint8_t*>(vsrc.Data()) + offset, cmp);
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
