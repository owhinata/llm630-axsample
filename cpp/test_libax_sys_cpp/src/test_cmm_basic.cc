#include <gtest/gtest.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <cstdlib>

#include "axsys/sys.hpp"

namespace {

using axsys::CacheMode;

// Read /proc/ax_proc/mem_cmm_info and extract used_kb and block_number.
// Returns true on success; false if file not found or parse failed.
static bool ReadCmmUseInfo(int* used_kb, int* block_number) {
  if (!used_kb || !block_number) return false;
  FILE* f = fopen("/proc/ax_proc/mem_cmm_info", "r");
  if (!f) return false;
  char line[512];
  bool found = false;
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, "total size=") && strstr(line, "used=") &&
        strstr(line, "block_number=")) {
      char* p_used = strstr(line, "used=");
      char* p_blk = strstr(line, "block_number=");
      if (p_used && p_blk) {
        int u = 0, b = 0;
        char* kb = strstr(p_used, "KB");
        if (kb) {
          *kb = '\0';
          u = atoi(p_used + 5);
          *kb = 'K';
        }
        b = atoi(p_blk + strlen("block_number="));
        *used_kb = u;
        *block_number = b;
        found = true;
        break;
      }
    }
  }
  fclose(f);
  return found;
}

// Check presence of a block by name and length (in KB) in mem_cmm_info.
static bool HasBlockByNameAndLengthKB(const char* tag, int length_kb) {
  if (!tag) return false;
  FILE* f = fopen("/proc/ax_proc/mem_cmm_info", "r");
  if (!f) return false;
  char line[1024];
  bool found = false;
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, "|-Block:") && strstr(line, "name=\"")) {
      if (strstr(line, tag) != nullptr) {
        char* p_len = strstr(line, "length=");
        if (p_len) {
          int len_kb = atoi(p_len + 7);
          if (len_kb == length_kb) {
            found = true;
            break;
          }
        }
      }
    }
  }
  fclose(f);
  return found;
}

// Check if a given address currently appears inside any mapping range in
// /proc/self/maps. Returns true if the address lies within a mapped interval.
static bool IsAddressMapped(const void* addr) {
  if (!addr) return false;
  const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return false;
  char line[512];
  bool mapped = false;
  while (fgets(line, sizeof(line), f)) {
    // Expected line prefix: "start-end perms ..." where start/end are hex.
    char* dash = strchr(line, '-');
    if (!dash) continue;
    *dash = '\0';
    const char* start_hex = line;
    const char* end_hex = dash + 1;
    // strtoull stops at first non-hex character (e.g., space before perms).
    uintptr_t start = static_cast<uintptr_t>(strtoull(start_hex, nullptr, 16));
    uintptr_t end = static_cast<uintptr_t>(strtoull(end_hex, nullptr, 16));
    if (start <= a && a < end) {
      mapped = true;
      break;
    }
  }
  fclose(f);
  return mapped;
}

/**
 * @brief Case001: Non-cached allocation and mapping sanity check.
 *
 * Purpose:
 * - Verify a contiguous CMM block can be allocated in non-cached mode.
 * Steps:
 * - Call CmmBuffer::Allocate(size,kNonCached,token).
 * - Read back phys via CmmBuffer::Phys() and virt via view.Data().
 * - Assert phys != 0 and virt != nullptr.
 * Expected:
 * - Non-zero phys and non-null virt; no errors.
 */
TEST(CmmBasic, Case001_AllocateNonCached) {
  constexpr uint32_t kLen = 2 * 1024 * 1024;
  for (int i = 0; i < 10; ++i) {
    axsys::CmmBuffer buf;
    auto r = buf.Allocate(kLen, CacheMode::kNonCached, "gtest_001");
    ASSERT_TRUE(r) << r.Message();
    axsys::CmmView v = r.MoveValue();
    EXPECT_NE(buf.Phys(), 0u);
    EXPECT_NE(v.Data(), nullptr);
    EXPECT_EQ(v.Size(), kLen);
  }
}

/**
 * @brief Case001r: View Reset and block release is reflected in CMM info.
 *
 * Purpose:
 * - Verify that Reset() unmaps the view and that Free() releases the
 *   allocation, and that this is observable in /proc/ax_proc/mem_cmm_info.
 * Steps:
 * - Snapshot CMM usage: read used_kb and block_number.
 * - Allocate a 2 MiB non-cached block with tag "gtest_001r" and get the base
 *   view (v = Allocate(...).MoveValue()).
 * - Confirm that mem_cmm_info contains a Block entry with
 *   name="gtest_001r" and length=2048KB.
 * - While v is alive, call Free() on the buffer and assert it fails
 *   (allocation cannot be freed while views remain).
 * - Call v.Reset(); assert Data()==nullptr; then call buf.Free() and assert it
 *   succeeds.
 * - Read CMM usage again and assert used_kb/block_number have not increased.
 * - Confirm that the named Block entry has disappeared from mem_cmm_info.
 * Expected:
 * - Allocate exposes a single named block (gtest_001r, 2048KB) that vanishes
 *   after Reset + Free; usage counters do not increase.
 */
TEST(CmmBasic, Case001r_ViewResetThenFree) {
  int used_before = -1, blocks_before = -1;
  (void)ReadCmmUseInfo(&used_before, &blocks_before);

  constexpr uint32_t kLen = 2 * 1024 * 1024;
  constexpr uint32_t kLenKB = kLen / 1024;
  constexpr const char* kTag = "gtest_001r";

  axsys::CmmBuffer buf;
  auto r = buf.Allocate(kLen, CacheMode::kNonCached, kTag);
  ASSERT_TRUE(r) << r.Message();
  axsys::CmmView v = r.MoveValue();
  // Named block should be present after allocation
  EXPECT_TRUE(HasBlockByNameAndLengthKB(kTag, kLenKB));
  ASSERT_NE(v.Data(), nullptr);
  auto er = buf.Free();
  EXPECT_FALSE(er);

  v.Reset();
  EXPECT_EQ(v.Data(), nullptr);
  auto fr = buf.Free();
  EXPECT_TRUE(fr) << fr.Message();

  int used_after = -1, blocks_after = -1;
  if (ReadCmmUseInfo(&used_after, &blocks_after)) {
    // After Reset/Free, usage and block count should not increase.
    if (used_before >= 0) {
      EXPECT_EQ(used_after, used_before);
    }
    if (blocks_before >= 0) {
      EXPECT_EQ(blocks_after, blocks_before);
    }
  }
  // Named block should be gone after Reset/Free
  EXPECT_FALSE(HasBlockByNameAndLengthKB(kTag, kLenKB));
}

/**
 * @brief Case001v: Buffer dtor while view survives.
 *
 * Purpose:
 * - Verify that the mapped virtual address remains valid after the buffer
 *   destructor runs (while a view survives), and that it is unmapped after
 *   the view is reset, as confirmed via /proc/self/maps.
 * Steps:
 * - Allocate a 2 MiB non-cached block in an inner scope and MoveValue() the
 *   base view out of the scope so that the buffer is destroyed.
 * - Record the view's base address and confirm it appears in /proc/self/maps.
 * - After the buffer dtor, confirm the address still appears in
 *   /proc/self/maps (view keeps the mapping alive).
 * - Call v.Reset() and confirm the address disappears from /proc/self/maps.
 * Expected:
 * - Address is mapped before Reset() and unmapped after Reset().
 */
TEST(CmmBasic, Case001v_BufferDtorWhileViewSurvives) {
  constexpr uint32_t kLen = 2 * 1024 * 1024;
  axsys::CmmView v;
  const void* addr = nullptr;
  {
    axsys::CmmBuffer buf;
    auto r = buf.Allocate(kLen, CacheMode::kNonCached, "gtest_001v");
    ASSERT_TRUE(r) << r.Message();
    v = r.MoveValue();

    // While buffer is alive, the view's address must be mapped.
    addr = v.Data();
    ASSERT_NE(addr, nullptr);
    EXPECT_TRUE(IsAddressMapped(addr));
  }
  // After buffer dtor, view remains valid
  ASSERT_NE(v.Data(), nullptr);
  EXPECT_TRUE(IsAddressMapped(addr));

  // After Reset, the mapping should no longer be present at addr.
  v.Reset();
  EXPECT_EQ(v.Data(), nullptr);
  EXPECT_FALSE(IsAddressMapped(addr));
}

/**
 * @brief Case002: Cached allocation and repeated mapping sanity.
 *
 * Purpose:
 * - Validate cached-mode allocation succeeds repeatedly and each mapped view
 *   has the expected size.
 * Steps:
 * - For i = 0..15:
 *   - Call CmmBuffer::Allocate(kLen, kCached, "gtest_002").
 *   - Verify Phys() != 0, view.Data() != nullptr, and view.Size() == kLen.
 * Expected:
 * - All iterations succeed. For each iteration Phys() != 0,
 *   Data() != nullptr, and Size() == kLen.
 */
TEST(CmmBasic, Case002_AllocateCached) {
  constexpr uint32_t kLen = 2 * 1024 * 1024;
  axsys::CmmBuffer buf[16];
  for (int i = 0; i < 16; ++i) {
    auto r = buf[i].Allocate(kLen, CacheMode::kCached, "gtest_002");
    ASSERT_TRUE(r) << r.Message();
    axsys::CmmView v = r.MoveValue();
    EXPECT_NE(buf[i].Phys(), 0u);
    EXPECT_NE(v.Data(), nullptr);
    EXPECT_EQ(v.Size(), kLen);
  }
}

/**
 * @brief Case003 (subset): In-range MapView produces a valid sub-view.
 *
 * Purpose:
 * - Ensure a sub-range mapping returns a valid view with the requested offset
 *   and size, and that buffer metadata remains consistent.
 * Steps:
 * - Allocate 1MiB non-cached with tag ("gtest_003").
 * - Call MapView(0x1000, 0x2000, kNonCached) on the same buffer.
 * - Verify the sub-view has non-null Data(), Offset() == 0x1000 and
 *   Size() == 0x2000.
 * - Call buf.Verify() to check internal consistency.
 * Expected:
 * - Sub-view is valid; Offset/Size match the request; Verify() succeeds.
 */
TEST(CmmBasic, Case003_MapViewInRange) {
  axsys::CmmBuffer buf;
  auto base = buf.Allocate(1 * 1024 * 1024, CacheMode::kNonCached, "gtest_003");
  ASSERT_TRUE(base) << base.Message();
  axsys::CmmView v = base.MoveValue();
  auto sub = buf.MapView(0x1000, 0x2000, CacheMode::kNonCached);
  ASSERT_TRUE(sub) << sub.Message();
  axsys::CmmView vs = sub.MoveValue();
  EXPECT_NE(vs.Data(), nullptr);
  EXPECT_EQ(vs.Offset(), 0x1000u);
  EXPECT_EQ(vs.Size(), 0x2000u);
  ASSERT_TRUE(buf.Verify());
}

/**
 * @brief Case003 (subset): Out-of-range mapping fails.
 *
 * Purpose:
 * - Ensure out-of-range map requests are rejected.
 * Steps:
 * - Allocate 0x4000 non-cached; MapView(0x3000,0x2000,kNonCached).
 * - Verify the Result is error.
 * Expected:
 * - Error (kOutOfRange).
 */
TEST(CmmBasic, Case003_OutOfRangeMapFails) {
  axsys::CmmBuffer buf;
  auto base = buf.Allocate(0x4000, CacheMode::kNonCached, "gtest_oor");
  ASSERT_TRUE(base) << base.Message();
  auto bad = buf.MapView(0x3000, 0x2000, CacheMode::kNonCached);
  ASSERT_FALSE(bad);
}

}  // namespace
