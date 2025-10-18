#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

/*
 * Case007: MflushCache scaling sizes.
 * Purpose:
 * - For j=1..32MiB, after Flush, non-cached copy reflects data.
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

/*
 * Case008: MinvalidateCache scaling sizes.
 * Purpose:
 * - For j=1..32MiB, after Invalidate, cached dst reflects src data.
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
