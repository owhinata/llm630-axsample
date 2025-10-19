#include <gtest/gtest.h>
#include <string.h>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

static bool FindAnonymous(axsys::CmmBuffer::PartitionInfo* out) {
  return axsys::CmmBuffer::FindAnonymous(out);
}

/**
 * @brief Case015: External attach + cached/noncached views + Flush.
 *
 * Purpose:
 * - Ensure cached alias over external PA is visible to non-cached after Flush.
 * Steps:
 * - AttachExternal(phys,size=1MiB); MapView(0,size,kNonCached) and kCached.
 * - memset nc=0xDF; memset cached=0xDE; Flush(cached).
 * - Compare nc vs cached over full range.
 * Expected:
 * - Full-range equality.
 */
TEST(CmmExternal, Case015_AttachFlushMakesEqual) {
  axsys::CmmBuffer::PartitionInfo part;
  if (!FindAnonymous(&part)) GTEST_SKIP() << "anonymous partition missing";
  const uint32_t block_size = 1 * 1024 * 1024;
  const uint64_t phys =
      part.phys + static_cast<uint64_t>(part.size_kb) * 1024 - block_size * 2;

  const uint32_t kTests = 10;  // lighter than sample but same logic
  for (uint32_t t = 0; t < kTests; ++t) {
    axsys::CmmBuffer buf;
    ASSERT_TRUE(buf.AttachExternal(phys, block_size)) << "attach failed";
    auto nc_r = buf.MapView(0, block_size, CacheMode::kNonCached);
    auto c_r = buf.MapView(0, block_size, CacheMode::kCached);
    ASSERT_TRUE(nc_r && c_r);
    axsys::CmmView nc = nc_r.MoveValue();
    axsys::CmmView c = c_r.MoveValue();
    memset(nc.Data(), 0xdf, block_size);
    memset(c.Data(), 0xde, block_size);
    ASSERT_TRUE(c.Flush());
    EXPECT_EQ(memcmp(nc.Data(), c.Data(), block_size), 0);
  }
}

/**
 * @brief Case016: External attach + cached/noncached views + Invalidate.
 *
 * Purpose:
 * - Ensure cached alias reflects non-cached writes after Invalidate.
 * Steps:
 * - AttachExternal; MapView nc/cached over [0,size].
 * - memset nc=0xBC; memset cached=0xFA; Invalidate(cached).
 * - Write nc=0xBB; compare nc vs cached over full range.
 * Expected:
 * - Full-range equality.
 */
TEST(CmmExternal, Case016_AttachInvalidateMakesEqual) {
  axsys::CmmBuffer::PartitionInfo part;
  if (!FindAnonymous(&part)) GTEST_SKIP() << "anonymous partition missing";
  const uint32_t block_size = 1 * 1024 * 1024;
  const uint64_t phys =
      part.phys + static_cast<uint64_t>(part.size_kb) * 1024 - block_size * 2;

  const uint32_t kTests = 10;  // lighter than sample; same behavior
  for (uint32_t t = 0; t < kTests; ++t) {
    axsys::CmmBuffer buf;
    ASSERT_TRUE(buf.AttachExternal(phys, block_size));
    auto nc_r = buf.MapView(0, block_size, CacheMode::kNonCached);
    auto c_r = buf.MapView(0, block_size, CacheMode::kCached);
    ASSERT_TRUE(nc_r && c_r);
    axsys::CmmView nc = nc_r.MoveValue();
    axsys::CmmView c = c_r.MoveValue();
    memset(nc.Data(), 0xbc, block_size);
    memset(c.Data(), 0xfa, block_size);
    ASSERT_TRUE(c.Invalidate());
    memset(nc.Data(), 0xbb, block_size);
    EXPECT_EQ(memcmp(nc.Data(), c.Data(), block_size), 0);
  }
}

}  // namespace
