#include <ax_sys_api.h>
#include <gtest/gtest.h>
#include <inttypes.h>
#include <string.h>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

/*
 * Case003: Verify/Dump (non-cached virt).
 * Purpose:
 * - Verify allocation; query ByVirt at base and +0x1000; expect consistent
 *   physical increment.
 */
TEST(CmmDumpVerify, Case003_NonCachedVerifyAndByVirt) {
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(1 * 1024 * 1024, CacheMode::kNonCached, "cmm_003");
  ASSERT_TRUE(r) << r.Message();
  axsys::CmmView v = r.MoveValue();

  // Verify() should succeed
  EXPECT_TRUE(buf.Verify());

  // ByVirt: base and +0x1000 should resolve and phys should be consistent
  AX_U64 phys0 = 0;
  AX_S32 cache_type0 = 0;
  ASSERT_EQ(0, AX_SYS_MemGetBlockInfoByVirt(v.Data(), &phys0, &cache_type0));
  AX_U64 phys1 = 0;
  AX_S32 cache_type1 = 0;
  void* v_off =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(v.Data()) + 0x1000);
  ASSERT_EQ(0, AX_SYS_MemGetBlockInfoByVirt(v_off, &phys1, &cache_type1));
  EXPECT_EQ(phys0 + 0x1000, phys1);
}

/*
 * Case003r: Verify that CmmView::Reset unmaps the view.
 * Steps:
 * - Keep base view; save pointer; Reset(); ByVirt on old pointer fails.
 */
TEST(CmmDumpVerify, Case003r_ResetUnmapsView) {
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(1 * 1024 * 1024, CacheMode::kNonCached, "cmm_003r");
  ASSERT_TRUE(r);
  axsys::CmmView v = r.MoveValue();
  void* old_v = v.Data();
  v.Reset();
  AX_U64 phys = 0;
  AX_S32 cache_type = 0;
  // After reset, query by old virtual should fail
  EXPECT_NE(0, AX_SYS_MemGetBlockInfoByVirt(old_v, &phys, &cache_type));
}

/*
 * Case017: Block info on cached virtual address.
 * Purpose:
 * - ByVirt on cached view succeeds; Verify() is true.
 */
TEST(CmmDumpVerify, Case017_CachedVirtDumpVerify) {
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(1 * 1024 * 1024, CacheMode::kCached, "cmm_017");
  ASSERT_TRUE(r);
  axsys::CmmView v = r.MoveValue();
  EXPECT_TRUE(buf.Verify());

  AX_U64 phys = 0;
  AX_S32 cache_type = 0;
  ASSERT_EQ(0, AX_SYS_MemGetBlockInfoByVirt(v.Data(), &phys, &cache_type));
}

/*
 * Case018: Block info on mapped non-cached address.
 * Purpose:
 * - ByVirt at 0, +0x1000, +0x11ef succeed; Verify() true.
 */
TEST(CmmDumpVerify, Case018_MappedNonCachedByVirtOffsets) {
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(1 * 1024 * 1024, CacheMode::kNonCached, "cmm_018");
  ASSERT_TRUE(r);
  axsys::CmmView v = r.MoveValue();
  EXPECT_TRUE(buf.Verify());

  AX_U64 p0 = 0, p1 = 0, p2 = 0;
  AX_S32 c0 = 0, c1 = 0, c2 = 0;
  ASSERT_EQ(0, AX_SYS_MemGetBlockInfoByVirt(v.Data(), &p0, &c0));
  void* v1 =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(v.Data()) + 0x1000);
  ASSERT_EQ(0, AX_SYS_MemGetBlockInfoByVirt(v1, &p1, &c1));
  void* v2 =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(v.Data()) + 0x11ef);
  ASSERT_EQ(0, AX_SYS_MemGetBlockInfoByVirt(v2, &p2, &c2));
  EXPECT_EQ(p0 + 0x1000, p1);
  EXPECT_LE(p0, p2);
}

/*
 * Case019: Block info on mapped cached address.
 * Purpose:
 * - ByVirt on cached alias/base; ByPhy on phys/base offsets succeed.
 */
TEST(CmmDumpVerify, Case019_MappedCachedAndByPhyOffsets) {
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(1 * 1024 * 1024, CacheMode::kNonCached, "cmm_019");
  ASSERT_TRUE(r);
  axsys::CmmView base = r.MoveValue();
  auto cr = buf.MapView(0, base.Size(), CacheMode::kCached);
  ASSERT_TRUE(cr);
  axsys::CmmView cache = cr.MoveValue();

  // ByVirt for both cached alias and base
  AX_U64 p_base = 0, p_cache = 0;
  AX_S32 ct_base = 0, ct_cache = 0;
  ASSERT_EQ(0, AX_SYS_MemGetBlockInfoByVirt(base.Data(), &p_base, &ct_base));
  ASSERT_EQ(0, AX_SYS_MemGetBlockInfoByVirt(cache.Data(), &p_cache, &ct_cache));
  EXPECT_EQ(p_base, p_cache);

  // Offsets must match
  void* c_off = reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(cache.Data()) + 0x1000);
  AX_U64 p_cache_off = 0;
  AX_S32 ct_cache_off = 0;
  ASSERT_EQ(0,
            AX_SYS_MemGetBlockInfoByVirt(c_off, &p_cache_off, &ct_cache_off));
  EXPECT_EQ(p_base + 0x1000, p_cache_off);

  // ByPhy for phys and offsets
  AX_S32 cache_type = 0;
  void* vir_out = nullptr;
  AX_U32 blk_sz = 0;
  ASSERT_EQ(
      0, AX_SYS_MemGetBlockInfoByPhy(p_base, &cache_type, &vir_out, &blk_sz));
  EXPECT_NE(vir_out, nullptr);
}

}  // namespace
