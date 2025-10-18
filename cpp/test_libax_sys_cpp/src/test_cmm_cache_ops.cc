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

/*
 * Case009: Flush with offset (cached src -> noncached dst).
 * Purpose:
 * - After writing via cached src, Flush makes data visible to non-cached dst.
 * Steps:
 * - Allocate src(cached)/dst(noncached); init src pattern; Flush(); copy
 * Expected:
 * - Full-range compare equals.
 */
TEST(CmmCacheOps, FlushMakesDataVisible) {  // mirrors Case009
  const uint32_t size = 4 * 1024 * 1024;
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
  auto fr = vsrc.Flush();
  ASSERT_TRUE(fr) << fr.Message();

  memset(vdst.Data(), 0x39, size);
  ASSERT_EQ(0, MemcpyView(vsrc, vdst, size));
  EXPECT_EQ(memcmp(vdst.Data(), vsrc.Data(), size), 0);
}

/*
 * Case010: Flush with offset (repeat of 009 on subrange).
 * Purpose:
 * - Flushing [offset..end) makes that subrange visible.
 * Steps:
 * - Flush(offset, size-offset); copy; compare only [offset..end)
 * Expected:
 * - Subrange equality.
 */
TEST(CmmCacheOps, FlushWithOffset) {  // mirrors Case010
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 2 * 1024 * 1024;
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
  EXPECT_EQ(memcmp(static_cast<uint8_t*>(vdst.Data()) + offset,
                   static_cast<uint8_t*>(vsrc.Data()) + offset, size - offset),
            0);
}

/*
 * Case012: Flush subrange then compare bigger range (expect fail).
 * Purpose:
 * - Flushing [offset..offset+len) must not make larger [offset..offset+cmp)
 *   fully visible.
 * Expected:
 * - Bigger range compare fails (not equal).
 */
TEST(CmmCacheOps, FlushSubrangeThenCompareBiggerRangeFails) {  // Case012
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 4;
  const uint32_t cmp = size / 2;
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

  // [offset..cmp) should NOT fully match since cmp > flushed length.
  EXPECT_NE(memcmp(static_cast<uint8_t*>(vdst.Data()) + offset,
                   static_cast<uint8_t*>(vsrc.Data()) + offset, cmp),
            0);
}

}  // namespace
