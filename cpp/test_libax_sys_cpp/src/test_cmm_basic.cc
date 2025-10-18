#include <gtest/gtest.h>
#include <inttypes.h>
#include <string.h>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

/*
 * Case001: Non-cached allocation and mapping sanity check.
 * Purpose:
 * - Verify a physically contiguous CMM block can be allocated in
 *   non-cached mode and mapped to a virtual address.
 * Steps:
 * - CmmBuffer::Allocate(size, kNonCached, token)
 * - Print phys/virt (here: assert non-zero/non-null)
 * Expected:
 * - Non-zero phys and non-null virt. No errors.
 */
TEST(CmmBasic, AllocateNonCached) {  // mirrors Case001
  constexpr uint32_t kLen = 2 * 1024 * 1024;
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(kLen, CacheMode::kNonCached, "gtest_001");
  ASSERT_TRUE(r) << r.Message();
  axsys::CmmView v = r.MoveValue();
  EXPECT_NE(buf.Phys(), 0u);
  EXPECT_NE(v.Data(), nullptr);
  EXPECT_EQ(v.Size(), kLen);
}

/*
 * Case002: Cached allocation and mapping sanity check.
 * Purpose:
 * - Verify allocation and mapping in cached mode.
 * Steps:
 * - CmmBuffer::Allocate(size, kCached, token)
 * Expected:
 * - Non-zero phys and non-null virt. No errors.
 */
TEST(CmmBasic, AllocateCached) {  // mirrors Case002
  constexpr uint32_t kLen = 2 * 1024 * 1024;
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(kLen, CacheMode::kCached, "gtest_002");
  ASSERT_TRUE(r) << r.Message();
  axsys::CmmView v = r.MoveValue();
  EXPECT_NE(buf.Phys(), 0u);
  EXPECT_NE(v.Data(), nullptr);
  EXPECT_EQ(v.Size(), kLen);
}

/*
 * Case003r (variant for Result API): Verify CmmView::Reset unmaps the view.
 * Steps:
 * - Allocate non-cached; keep base view; call Reset()
 * - Subsequent access becomes invalid (here we assert Data()==nullptr)
 * - CmmBuffer::Free() succeeds with no remaining views
 */
TEST(CmmBasic, ViewResetThenFree) {  // mirrors Case001r
  constexpr uint32_t kLen = 2 * 1024 * 1024;
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(kLen, CacheMode::kNonCached, "gtest_001r");
  ASSERT_TRUE(r) << r.Message();
  axsys::CmmView v = r.MoveValue();
  ASSERT_NE(v.Data(), nullptr);
  v.Reset();
  EXPECT_EQ(v.Data(), nullptr);
  auto fr = buf.Free();
  EXPECT_TRUE(fr) << fr.Message();
}

/*
 * Case001v: Buffer dtor while view survives; free occurs on last view.
 * Steps:
 * - Allocate inside scope; move view out; let buffer dtor run first
 * - View remains valid; Reset() then releases mapping
 * Expected:
 * - View valid before Reset(); invalid after Reset().
 */
TEST(CmmBasic, BufferDtorWhileViewSurvives) {  // mirrors Case001v
  constexpr uint32_t kLen = 2 * 1024 * 1024;
  axsys::CmmView v;
  {
    axsys::CmmBuffer buf;
    auto r = buf.Allocate(kLen, CacheMode::kNonCached, "gtest_001v");
    ASSERT_TRUE(r) << r.Message();
    v = r.MoveValue();
    EXPECT_NE(v.Data(), nullptr);
  }
  // After buffer dtor, view remains valid; reset should release mapping.
  EXPECT_NE(v.Data(), nullptr);
  v.Reset();
  EXPECT_EQ(v.Data(), nullptr);
}

/*
 * Case003 (subset): Verify MapView within range succeeds.
 * Steps:
 * - Allocate 1MiB non-cached; MapView(0x1000, 0x2000, non-cached)
 * Expected:
 * - Returned view is valid and size matches request.
 */
TEST(CmmBasic, MapViewInRange) {  // subset of Case003
  axsys::CmmBuffer buf;
  auto base = buf.Allocate(1 * 1024 * 1024, CacheMode::kNonCached, "gtest_003");
  ASSERT_TRUE(base) << base.Message();
  axsys::CmmView v = base.MoveValue();
  auto sub = buf.MapView(0x1000, 0x2000, CacheMode::kNonCached);
  ASSERT_TRUE(sub) << sub.Message();
  axsys::CmmView vs = sub.MoveValue();
  EXPECT_NE(vs.Data(), nullptr);
  EXPECT_EQ(vs.Size(), 0x2000u);
}

/*
 * Case003 (subset): Out-of-range mapping should fail.
 * Steps:
 * - Allocate 0x4000; MapView(0x3000, 0x2000) exceeds end
 * Expected:
 * - Result is error (kOutOfRange).
 */
TEST(CmmBasic, MapViewOutOfRangeFails) {
  axsys::CmmBuffer buf;
  auto base = buf.Allocate(0x4000, CacheMode::kNonCached, "gtest_oor");
  ASSERT_TRUE(base) << base.Message();
  auto bad = buf.MapView(0x3000, 0x2000, CacheMode::kNonCached);
  ASSERT_FALSE(bad);
}

}  // namespace
