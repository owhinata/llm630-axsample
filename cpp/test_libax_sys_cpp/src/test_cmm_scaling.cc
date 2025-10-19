#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

/**
 * @brief Case007: MflushCache scaling sizes.
 *
 * Purpose:
 * - For sizes j=1..32 MiB, verify that after Flush on a cached source,
 *   a non-cached destination sees the updated data.
 * Steps:
 * - Prepare src(cached) and dst(non-cached) of size j MiB.
 * - memset src (cached) with a pattern, then Flush() to clean to memory.
 * - Copy src->dst using non-cached aliases (avoid cached alias for copy).
 * - Compare full range (aliases).
 * Expected:
 * - Full-range equality for each size j.
 */
TEST(CmmScaling, Case007_FlushScalingSizes) {
  const uint32_t kTests = 32;
  for (uint32_t j = 1; j <= kTests; ++j) {
    const uint32_t sz = j * 1024 * 1024;
    axsys::CmmBuffer src, dst;
    auto rs = src.Allocate(sz, CacheMode::kCached, "cmm_007_src");
    if (!rs) GTEST_SKIP() << "alloc fail at " << j << ": " << rs.Message();
    auto rd = dst.Allocate(sz, CacheMode::kNonCached, "cmm_007_dst");
    if (!rd) GTEST_SKIP() << "alloc fail at dst " << j;
    axsys::CmmView vsrc = rs.MoveValue();
    axsys::CmmView vdst = rd.MoveValue();

    memset(vsrc.Data(), 0x78, sz);
    for (uint32_t i = 0; i < 256 && i < sz; ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = static_cast<uint8_t>(255 - i);
    }
    ASSERT_TRUE(vsrc.Flush());

    // Mapping-aware copy
    auto s_alias = src.MapView(0, sz, CacheMode::kNonCached);
    auto d_alias = dst.MapView(0, sz, CacheMode::kNonCached);
    ASSERT_TRUE(s_alias && d_alias);
    axsys::CmmView sa = s_alias.MoveValue();
    axsys::CmmView da = d_alias.MoveValue();
    memcpy(da.Data(), sa.Data(), sz);
    EXPECT_EQ(memcmp(sa.Data(), da.Data(), sz), 0);
  }
}

/**
 * @brief Case008: MinvalidateCache scaling sizes.
 *
 * Purpose:
 * - For j=1..32MiB, after Invalidate, cached dst reflects src data.
 * Steps:
 * - Prepare src (non-cached) and dst (cached) views of the same size.
 * - memset dst (cached) with a pattern, then Flush() to clean to memory.
 * - Copy src->dst using non-cached aliases (avoid cached alias for copy).
 * - Invalidate dst (cached) to drop stale lines.
 * - Compare src vs dst over the full range.
 * Expected:
 * - Full-range equality for each size.
 */
TEST(CmmScaling, Case008_InvalidateScalingSizes) {
  const uint32_t kTests = 32;
  for (uint32_t j = 1; j <= kTests; ++j) {
    const uint32_t sz = j * 1024 * 1024;
    axsys::CmmBuffer src, dst;
    auto rs = src.Allocate(sz, CacheMode::kNonCached, "cmm_008_src");
    if (!rs) GTEST_SKIP() << "alloc fail at " << j << ": " << rs.Message();
    auto rd = dst.Allocate(sz, CacheMode::kCached, "cmm_008_dst");
    if (!rd) GTEST_SKIP() << "alloc fail at dst " << j;
    axsys::CmmView vsrc = rs.MoveValue();
    axsys::CmmView vdst = rd.MoveValue();

    memset(vsrc.Data(), 0xFF, sz);
    for (uint32_t i = 0; i < 256 && i < sz; ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = static_cast<uint8_t>(255 - i);
    }
    memset(vdst.Data(), 0xEE, sz);
    for (uint32_t i = 0; i < 256 && i < sz; ++i) {
      static_cast<uint8_t*>(vdst.Data())[i] = static_cast<uint8_t>(i);
    }
    ASSERT_TRUE(vdst.Flush());

    // Copy into cached dest, then invalidate
    auto s_alias = src.MapView(0, sz, CacheMode::kNonCached);
    auto d_alias = dst.MapView(0, sz, CacheMode::kNonCached);
    ASSERT_TRUE(s_alias && d_alias);
    axsys::CmmView sa = s_alias.MoveValue();
    axsys::CmmView da = d_alias.MoveValue();
    memcpy(da.Data(), sa.Data(), sz);

    ASSERT_TRUE(vdst.Invalidate());
    EXPECT_EQ(memcmp(vsrc.Data(), vdst.Data(), sz), 0);
  }
}

}  // namespace
