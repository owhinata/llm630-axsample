#include <ax_sys_api.h>
#include <gtest/gtest.h>
#include <inttypes.h>
#include <string.h>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

/**
 * @brief Case003: Verify/ByVirt on non-cached mapping (Dump parity).
 *
 * Purpose:
 * - Verify allocation and ByVirt consistency on a non-cached mapping and
 *   exercise Dump semantics.
 * - CmmView::Dump(offset) must query ByVirt at (virt + offset).
 * Steps:
 * - Allocate 1MiB non-cached; call Verify().
 * - Call v.Dump(0) and v.Dump(0x1000) for diagnostics.
 * - Query ByVirt at base and +0x1000.
 * - Compare phys(+0x1000) with phys(base) + 0x1000.
 * Expected:
 * - Verify() is true; ByVirt(base/+0x1000) succeeds; phys delta is 0x1000.
 */
TEST(CmmDumpVerify, Case003_NonCachedVerifyAndByVirt) {
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(1 * 1024 * 1024, CacheMode::kNonCached, "cmm_003");
  ASSERT_TRUE(r) << r.Message();
  axsys::CmmView v = r.MoveValue();

  // Verify() should succeed
  EXPECT_TRUE(buf.Verify());

  // Diagnostic: Dump uses ByVirt at (virt + offset)
  v.Dump(0);
  v.Dump(0x1000);

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

/**
 * @brief Case003r: Reset unmaps the view (ByVirt fails on old VA).
 *
 * Purpose:
 * - Demonstrate that after CmmView::Reset(), the previous virtual address is
 *   no longer valid: ByVirt on the old pointer must fail.
 * - Also show Dump semantics: CmmView::Dump(offset) queries ByVirt at
 *   (virt + offset) before reset.
 * Steps:
 * - Allocate 1MiB non-cached; obtain base view.
 * - Call v.Dump(0) (diagnostic) and verify ByVirt(base) succeeds.
 * - Save base pointer, then call v.Reset().
 * - Call ByVirt(old_ptr) and expect failure.
 * Expected:
 * - Pre-reset: ByVirt(base) succeeds. Post-reset: ByVirt(old_ptr) fails.
 */
TEST(CmmDumpVerify, Case003r_ResetUnmapsView) {
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(1 * 1024 * 1024, CacheMode::kNonCached, "cmm_003r");
  ASSERT_TRUE(r);
  axsys::CmmView v = r.MoveValue();

  // Pre-reset diagnostics and check
  v.Dump(0);
  AX_U64 phys_before = 0;
  AX_S32 ct_before = 0;
  ASSERT_EQ(0,
            AX_SYS_MemGetBlockInfoByVirt(v.Data(), &phys_before, &ct_before));

  void* old_v = v.Data();
  v.Reset();

  // After reset, query by old virtual should fail
  AX_U64 phys = 0;
  AX_S32 cache_type = 0;
  EXPECT_NE(0, AX_SYS_MemGetBlockInfoByVirt(old_v, &phys, &cache_type));
}

/**
 * @brief Case017: Block info on cached virtual address (Dump/ByVirt parity).
 *
 * Purpose:
 * - Match sample_cmm Case017 semantics. CmmView::Dump must query ByVirt at
 *   (virt + offset), and CmmBuffer::Dump must query ByPhy at (phys + offset).
 *   Confirm consistency even on a cached virtual mapping.
 * Steps:
 * - Allocate 1MiB (kCached) and MoveValue() base view.
 * - Call buf.Dump() for ByPhy at base phys (diagnostic).
 * - Call v.Dump(0) and v.Dump(0x1000) for ByVirt at base and +0x1000
 * (diagnostic).
 * - Independently verify ByVirt(base) succeeds and that the phys at
 *   (base + 0x1000) equals (base_phys + 0x1000).
 * Expected:
 * - Verify() is true. ByVirt(base/+0x1000) succeeds and the phys delta equals
 *   0x1000.
 */
TEST(CmmDumpVerify, Case017_CachedVirtDumpVerify) {
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(1 * 1024 * 1024, CacheMode::kCached, "cmm_017");
  ASSERT_TRUE(r);
  axsys::CmmView v = r.MoveValue();
  EXPECT_TRUE(buf.Verify());

  // Diagnostic parity with sample_cmm Case017
  buf.Dump();
  v.Dump(0);
  v.Dump(0x1000);

  // Assert ByVirt(base) and ByVirt(base+0x1000)
  AX_U64 phys0 = 0, phys1 = 0;
  AX_S32 ct0 = 0, ct1 = 0;
  ASSERT_EQ(0, AX_SYS_MemGetBlockInfoByVirt(v.Data(), &phys0, &ct0));
  void* v_off =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(v.Data()) + 0x1000);
  ASSERT_EQ(0, AX_SYS_MemGetBlockInfoByVirt(v_off, &phys1, &ct1));
  EXPECT_EQ(phys0 + 0x1000, phys1);
}

/**
 * @brief Case018: ByVirt on mapped non-cached address (Dump offsets).
 *
 * Purpose:
 * - Verify ByVirt on multiple offsets for a non-cached mapping, and exercise
 *   Dump semantics at the same offsets.
 * Steps:
 * - Allocate 1MiB non-cached; call v.Dump(0), v.Dump(0x1000), v.Dump(0x11ef).
 * - Query ByVirt at base, +0x1000, +0x11ef.
 * Expected:
 * - All ByVirt queries return 0; phys(base+0x1000) == phys(base)+0x1000.
 */
TEST(CmmDumpVerify, Case018_MappedNonCachedByVirtOffsets) {
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(1 * 1024 * 1024, CacheMode::kNonCached, "cmm_018");
  ASSERT_TRUE(r);
  axsys::CmmView v = r.MoveValue();
  EXPECT_TRUE(buf.Verify());

  // Diagnostic dumps
  v.Dump(0);
  v.Dump(0x1000);
  v.Dump(0x11ef);

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

/**
 * @brief Case019: ByVirt/ByPhy on cached alias and base (Dump parity).
 *
 * Purpose:
 * - Confirm ByVirt/ByPhy consistency across the base (non-cached) view and a
 *   cached alias. Exercise Dump semantics for both views and ByPhy offsets.
 * Steps:
 * - Allocate 1MiB non-cached; MapView over full range with kCached to get an
 *   alias view.
 * - Call base.Dump(0), cache.Dump(0), cache.Dump(0x1000).
 * - Query ByVirt(base), ByVirt(cache), ByVirt(cache+0x1000).
 * - Obtain base phys and call ByPhy(phys).
 * Expected:
 * - phys(base) == phys(cache); phys(cache+0x1000) == phys(base) + 0x1000;
 *   ByPhy returns a non-null virt for phys(base).
 */
TEST(CmmDumpVerify, Case019_MappedCachedAndByPhyOffsets) {
  axsys::CmmBuffer buf;
  auto r = buf.Allocate(1 * 1024 * 1024, CacheMode::kNonCached, "cmm_019");
  ASSERT_TRUE(r);
  axsys::CmmView base = r.MoveValue();
  auto cr = buf.MapView(0, base.Size(), CacheMode::kCached);
  ASSERT_TRUE(cr);
  axsys::CmmView cache = cr.MoveValue();

  // Diagnostic dumps
  base.Dump(0);
  cache.Dump(0);
  cache.Dump(0x1000);

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
