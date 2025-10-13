// AXERA SDK
#include <ax_sys_api.h>

// C system headers
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// libax_sys_cpp
#include "axsys/sys.hpp"

namespace {

constexpr uint32_t kLen = 2 * 1024 * 1024;  // 2 MiB

// Forward decl for helper defined later in this file
static bool AddrInProcMaps(void* addr);

// Definition inside anonymous namespace
static bool AddrInProcMaps(void* addr) {
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return false;
  char line[512];
  uintptr_t target = reinterpret_cast<uintptr_t>(addr);
  bool found = false;
  while (fgets(line, sizeof(line), f)) {
    char* dash = strchr(line, '-');
    if (!dash) continue;
    *dash = '\0';
    char* endptr = nullptr;
    uint64_t start = strtoull(line, &endptr, 16);
    if (endptr == line) continue;
    uint64_t end = strtoull(dash + 1, &endptr, 16);
    if (target >= start && target < end) {
      found = true;
      break;
    }
  }
  fclose(f);
  return found;
}

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
      // Example: total size=..., used=448KB(...),..., block_number=13
      char* p_used = strstr(line, "used=");
      char* p_blk = strstr(line, "block_number=");
      if (p_used && p_blk) {
        int u = 0, b = 0;
        // Parse used=NNNKB
        char* kb = strstr(p_used, "KB");
        if (kb) {
          *kb = '\0';
          // p_used points to 'used='; skip it
          u = atoi(p_used + 5);
          *kb = 'K';  // restore
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

// Mapping-aware memcpy helper using CmmView::MapView within the view range.
// - If a view is cached, map a temporary non-cached alias from offset 0 for
//   the requested size, then copy. Temporary views auto-unmap on scope exit.
static int MemcpyView(const axsys::CmmView& src, const axsys::CmmView& dst,
                      uint32_t size) {
  if (!src || !dst || size == 0) return -1;
  if (size > src.Size() || size > dst.Size()) return -1;

  const void* s_ptr = src.Data();
  void* d_ptr = dst.Data();

  axsys::CmmView src_alias;  // default invalid
  axsys::CmmView dst_alias;

  if (src.Mode() == axsys::CacheMode::kCached) {
    src_alias = src.MapView(0, size, axsys::CacheMode::kNonCached);
    if (!src_alias) return -1;
    s_ptr = src_alias.Data();
  }
  if (dst.Mode() == axsys::CacheMode::kCached) {
    dst_alias = dst.MapView(0, size, axsys::CacheMode::kNonCached);
    if (!dst_alias) return -1;
    d_ptr = dst_alias.Data();
  }

  memcpy(d_ptr, s_ptr, size);
  return 0;
}

// AX_SYS lifecycle is handled by axsys::System (RAII)

/**
 * @brief Case 001: Non-cached allocation and mapping sanity check.
 *
 * Purpose
 * - Verify that a physically contiguous CMM block can be allocated in
 *   non-cached mode and mapped to a virtual address.
 *
 * Steps (via libax_sys_cpp; underlying AX_SYS calls)
 * - CmmBuffer::Allocate(size, kNonCached, token)
 *   - AX_SYS_MemAlloc(..., size, 0x1000, token)
 *   - AX_SYS_Mmap(phys, size)
 * - Print physical and virtual addresses for visibility.
 * - No explicit free is required. Views are destroyed before buffers due to
 *   declaration order, so no Free error logs are expected.
 *
 * Note
 * - To prevent memory leaks, ensure each CmmView is destroyed before the
 *   owning CmmBuffer. Use block scope per iteration (as in this test) or call
 *   CmmView::Reset() explicitly before the buffer is destroyed/freed. If views
 *   remain, CmmBuffer::Free() will not release the allocation and will log
 *   "views remain".
 *
 * Expected
 * - Prints non-zero physical address and non-null virtual address.
 * - No errors; mapping succeeds in non-cached mode.
 */
void Case001() {
  printf("[001] MemAlloc/MemFree (non-cached)\n");
  axsys::CmmBuffer buf[10];
  for (int i = 0; i < 10; ++i) {
    axsys::CmmView v =
        buf[i].Allocate(kLen, axsys::CacheMode::kNonCached, "cmm_001");
    printf("  phy=0x%" PRIx64 ", v=%p\n", buf[i].Phys(), v.Data());
  }
  printf("\n");
}

/**
 * @brief Case 001r: Auto-free after view reset and buffer dtor.
 *
 * Purpose
 * - Verify that after resetting the view, when the CmmBuffer goes out of
 *   scope, the allocation is automatically freed (via shared_ptr deleter).
 *
 * Steps
 * - Allocate non-cached; capture phys; reset the returned base view.
 * - Exit scope so that CmmBuffer dtor runs; then query ByPhy on saved phys
 *   and expect failure.
 */
static void Case001r() {
  printf("[001r] Auto free after Reset + dtor\n");
  AX_U64 phys = 0;
  int used0 = -1, blk0 = -1;
  int used1 = -1, blk1 = -1;
  if (!ReadCmmUseInfo(&used0, &blk0)) {
    printf("  mem_cmm_info: unavailable\n");
  } else {
    printf("  mem_cmm_info before: used=%dKB blocks=%d\n", used0, blk0);
  }
  {
    axsys::CmmBuffer buf;
    axsys::CmmView v =
        buf.Allocate(kLen, axsys::CacheMode::kNonCached, "cmm_001r");
    phys = buf.Phys();
    void* old_v = v.Data();
    printf("  allocated phys=0x%" PRIx64 ", v=%p\n",
           static_cast<uint64_t>(phys), old_v);
    bool in_maps = AddrInProcMaps(old_v);
    printf("  /proc/self/maps has base_v: %s\n", in_maps ? "true" : "false");
    v.Reset();
    printf("  view reset\n");
    in_maps = AddrInProcMaps(old_v);
    printf("  /proc/self/maps has old_v: %s\n", in_maps ? "true" : "false");
    // buf dtor will run at end of this scope
  }
  if (ReadCmmUseInfo(&used1, &blk1)) {
    printf("  mem_cmm_info after dtor: used=%dKB blocks=%d\n", used1, blk1);
  }
  AX_S32 mem_type = 0;
  void* vir_out = nullptr;
  AX_U32 blk_sz = 0;
  AX_S32 r = AX_SYS_MemGetBlockInfoByPhy(phys, &mem_type, &vir_out, &blk_sz);
  printf("  ByPhy after dtor: ret=0x%X (expected fail)\n",
         static_cast<unsigned int>(r));
  if (used0 >= 0 && used1 >= 0) {
    printf("  mem_cmm_info delta used=%dKB blocks=%d\n", (used1 - used0),
           (blk1 - blk0));
  }
  printf("\n");
}

/**
 * @brief Case 001v: Buffer dtor while view survives, then view reset frees.
 *
 * Purpose
 * - Demonstrate that CmmView can outlive CmmBuffer safely. Memory stays
 *   allocated while the view holds the shared allocation; freeing happens
 *   when the last view is reset/destroyed.
 *
 * Steps
 * - Create a scope in which CmmBuffer allocates and returns a base view;
 *   move that view out of the scope so Buffer dtor runs first.
 * - Check ByPhy on phys (expect success: still allocated).
 * - Reset the surviving view; then ByPhy should fail (freed by deleter).
 */
static void Case001v() {
  printf("[001v] View survives Buffer; freed on last view reset\n");
  AX_U64 phys = 0;
  axsys::CmmView v;
  int used0 = -1, blk0 = -1;
  int used1 = -1, blk1 = -1;
  int used2 = -1, blk2 = -1;
  if (ReadCmmUseInfo(&used0, &blk0)) {
    printf("  mem_cmm_info before: used=%dKB blocks=%d\n", used0, blk0);
  } else {
    printf("  mem_cmm_info: unavailable\n");
  }
  {
    axsys::CmmBuffer buf;
    v = buf.Allocate(kLen, axsys::CacheMode::kNonCached, "cmm_001v");
    phys = buf.Phys();
    printf("  allocated phys=0x%" PRIx64 ", v=%p\n",
           static_cast<uint64_t>(phys), v.Data());
    // buf dtor here; view keeps allocation alive via shared_ptr
  }
  bool in_maps = AddrInProcMaps(v.Data());
  printf("  /proc/self/maps has v (after Buffer dtor): %s\n",
         in_maps ? "true" : "false");
  ReadCmmUseInfo(&used1, &blk1);
  if (used0 >= 0 && used1 >= 0) {
    printf(
        "  mem_cmm_info after Buffer dtor: used=%dKB blocks=%d (delta "
        "used=%dKB blocks=%d)\n",
        used1, blk1, (used1 - used0), (blk1 - blk0));
  }
  AX_S32 mem_type = 0;
  void* vir_out = nullptr;
  AX_U32 blk_sz = 0;
  AX_S32 r1 = AX_SYS_MemGetBlockInfoByPhy(phys, &mem_type, &vir_out, &blk_sz);
  printf("  ByPhy after Buffer dtor (expect success): ret=0x%X\n",
         static_cast<unsigned int>(r1));
  v.Reset();
  in_maps = AddrInProcMaps(v.Data());
  printf("  /proc/self/maps has v (after View reset): %s\n",
         in_maps ? "true" : "false");
  AX_S32 r2 = AX_SYS_MemGetBlockInfoByPhy(phys, &mem_type, &vir_out, &blk_sz);
  printf("  ByPhy after View reset (expect fail): ret=0x%X\n",
         static_cast<unsigned int>(r2));
  ReadCmmUseInfo(&used2, &blk2);
  if (used0 >= 0 && used2 >= 0) {
    printf(
        "  mem_cmm_info after View reset: used=%dKB blocks=%d (delta used=%dKB "
        "blocks=%d)\n",
        used2, blk2, (used2 - used0), (blk2 - blk0));
  }
  printf("\n");
}

/**
 * @brief Case 002: Cached allocation and mapping sanity check.
 *
 * Purpose
 * - Verify that a physically contiguous CMM block can be allocated in cached
 *   mode and mapped to a virtual address.
 *
 * Steps (via libax_sys_cpp; underlying AX_SYS calls)
 * - CmmBuffer::Allocate(size, kCached, token)
 *   - AX_SYS_MemAllocCached(..., size, 0x1000, token)
 *   - AX_SYS_MmapCache(phys, size) (via MapView in Allocate)
 * - Print physical and virtual addresses for visibility.
 * - No explicit free is required. Views are destroyed before buffers due to
 *   declaration order, so no Free error logs are expected.
 *
 * Expected
 * - Prints non-zero physical address and non-null virtual address.
 * - No errors; mapping succeeds in cached mode.
 */
void Case002() {
  printf("[002] MemAllocCached/MemFree (cached)\n");
  axsys::CmmBuffer buf[10];
  for (int i = 0; i < 10; ++i) {
    axsys::CmmView v =
        buf[i].Allocate(kLen, axsys::CacheMode::kCached, "cmm_002");
    printf("  phy=0x%" PRIx64 ", v=%p\n", buf[i].Phys(), v.Data());
  }
  printf("\n");
}

/**
 * @brief Case 003: Block info parity with SDK (non-cached).
 *
 * Purpose
 * - Match SDK test: confirm virt->phys mapping at base and +0x1000, and
 *   check block info by physical address.
 *
 * Steps (AX_SYS used internally via lib)
 * - Allocate 1 MiB non-cached: CmmBuffer::Allocate(...)
 *   - AX_SYS_MemAlloc / AX_SYS_Mmap
 * - ByVirt (base/+0x1000): CmmView::Dump(0) and CmmView::Dump(0x1000)
 *   - AX_SYS_MemGetBlockInfoByVirt on view->Data() + offset
 * - ByPhy (base phys): CmmBuffer::Dump()
 *   - AX_SYS_MemGetBlockInfoByPhy on (phys + 0)
 * - Verify allocation/partition validity: CmmBuffer::Verify()
 *   - AX_SYS_MemGetBlockInfoByPhy / AX_SYS_MemGetPartitionInfo /
 *     AX_SYS_MemGetBlockInfoByVirt (on registered views)
 *
 * Expected
 * - ByPhy line shows a valid virt and blkSz for the block
 * - ByVirt lines show consistent phys across base and +0x1000 with cacheType
 * - verify=true
 */
void Case003() {
  printf("[003] Verify/Dump (non-cached virt)\n");
  axsys::CmmBuffer buf;
  axsys::CmmView v =
      buf.Allocate(1 * 1024 * 1024, axsys::CacheMode::kNonCached, "cmm_003");
  buf.Dump();
  v.Dump(0);
  v.Dump(0x1000);
  printf("  verify=%s\n", buf.Verify() ? "true" : "false");
  printf("\n");
}

/**
 * @brief Case 003r: Verify that CmmView::Reset unmaps the view.
 *
 * Steps
 * - Allocate 1 MiB (non-cached) and keep the returned view.
 * - Save the view's virtual address; call CmmView::Reset() to unmap it.
 * - Call AX_SYS_MemGetBlockInfoByVirt on the saved address; expect failure.
 * - Call CmmBuffer::Dump() to show ByPhy info and that no views remain.
 */
static void Case003r() {
  printf("[003r] Verify view unmap by Reset\n");
  axsys::CmmBuffer buf;
  axsys::CmmView v =
      buf.Allocate(1 * 1024 * 1024, axsys::CacheMode::kNonCached, "cmm_003r");
  void* old_v = v.Data();
  printf("  base v=%p\n", old_v);
  bool in_maps = AddrInProcMaps(old_v);
  printf("  /proc/self/maps has base_v: %s\n", in_maps ? "true" : "false");
  v.Reset();
  AX_U64 phy2 = 0;
  AX_S32 cache_type = 0;
  AX_S32 ret = AX_SYS_MemGetBlockInfoByVirt(old_v, &phy2, &cache_type);
  printf("  ByVirt after Reset: ret=0x%X%s\n", static_cast<unsigned int>(ret),
         (ret == 0 ? " (unexpected success)" : " (expected fail)"));
  in_maps = AddrInProcMaps(old_v);
  printf("  /proc/self/maps has old_v: %s\n", in_maps ? "true" : "false");
  buf.Dump();
  printf("\n");
}

/**
 * @brief Case 004: Mmap/Munmap (non-cached), pattern write and compare.
 *
 * Purpose
 * - Align with SDK test: create a second non-cached mapping, write a pattern
 *   through it, and verify it matches the base mapping across the full block.
 *
 * Steps (via lib; AX_SYS called internally)
 * - Allocate 1 MiB non-cached: CmmBuffer::Allocate(size, kNonCached, token)
 *   - AX_SYS_MemAlloc / AX_SYS_Mmap
 * - Create another non-cached mapping: CmmBuffer::MapView(0, size, kNonCached)
 *   - AX_SYS_Mmap
 * - Write 0x78 pattern via the mapped view; print first 20 bytes
 * - Compare base view and mapped view over the entire size
 * - Views auto-unmap on scope exit (SDK uses explicit AX_SYS_Munmap)
 *
 * Expected
 * - Both addresses are valid (non-null)
 * - Full-range compare passes (result: pass)
 */
void Case004() {
  printf("[004] Mmap/Munmap (non-cached)\n");
  const uint32_t size = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  axsys::CmmView vbase =
      buf.Allocate(size, axsys::CacheMode::kNonCached, "cmm_004");
  axsys::CmmView vmap = buf.MapView(0, size, axsys::CacheMode::kNonCached);
  printf("  base=%p map=%p\n", vbase.Data(), vmap.Data());

  // write pattern via the mapped view and print first 20 bytes
  memset(vmap.Data(), 0x78, size);
  printf("  ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vmap.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");

  bool ok = true;
  for (uint32_t i = 0; i < size; ++i) {
    if (static_cast<uint8_t*>(vbase.Data())[i] !=
        static_cast<uint8_t*>(vmap.Data())[i]) {
      printf("  mismatch i=%u vbase=0x%x vmap=0x%x\n", i,
             static_cast<unsigned>(static_cast<uint8_t*>(vbase.Data())[i]),
             static_cast<unsigned>(static_cast<uint8_t*>(vmap.Data())[i]));
      ok = false;
      break;
    }
  }
  printf("  result: %s\n", ok ? "pass" : "fail");
  printf("\n");
}

/**
 * @brief Case 005: MmapCache + Flush (cached) with base compare.
 *
 * Purpose
 * - Align with SDK: create a cached mapping over a non-cached base, write via
 *   the cached view, flush, and compare against the base mapping.
 *
 * Steps (AX_SYS via lib)
 * - Allocate 1 MiB non-cached: CmmBuffer::Allocate(size, kNonCached)
 * - Map cached view: CmmBuffer::MapView(0, size, kCached)
 * - Write 0xfe via the cached view and call CmmView::Flush()
 * - Print first 16 bytes from the base (non-cached) view
 * - Compare base vs cached for the entire size; print result
 */
void Case005() {
  printf("[005] MmapCache/Flush/Munmap (cached)\n");
  const uint32_t size = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  axsys::CmmView vbase =
      buf.Allocate(size, axsys::CacheMode::kNonCached, "cmm_005");
  axsys::CmmView vcache = buf.MapView(0, size, axsys::CacheMode::kCached);
  printf("  base=%p map=%p\n", vbase.Data(), vcache.Data());

  memset(vcache.Data(), 0xfe, size);
  vcache.Flush();

  printf("  ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vbase.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");

  bool ok = true;
  for (uint32_t i = 0; i < size; ++i) {
    if (static_cast<uint8_t*>(vbase.Data())[i] !=
        static_cast<uint8_t*>(vcache.Data())[i]) {
      printf("  mismatch i=%u vbase=0x%x vcache=0x%x\n", i,
             static_cast<unsigned>(static_cast<uint8_t*>(vbase.Data())[i]),
             static_cast<unsigned>(static_cast<uint8_t*>(vcache.Data())[i]));
      ok = false;
      break;
    }
  }
  printf("  result: %s\n", ok ? "pass" : "fail");
  printf("\n");
}

/**
 * @brief Case 006: MmapCache + Invalidate (cached) with base compare.
 *
 * Purpose
 * - Align with SDK: create a cached mapping over a non-cached base,
 *   initialize both, invalidate cached view, modify base, and verify equality.
 *
 * Steps (AX_SYS via lib)
 * - Allocate 1 MiB non-cached base
 * - Map cached view
 * - Initialize: base=0xbc, cached=0xfa; dump first 16B from each (before)
 * - Invalidate cached view; modify base=0xbb
 * - Dump first 16B from each (after) and compare full range
 *
 * Expected
 * - Addresses are valid (non-null)
 * - Before invalidate: base(first 16B) shows 0xbc pattern; cache shows 0xfa
 * - After invalidate and base write 0xbb: base and cache first 16B match
 * - Full-range compare passes (result: pass); no errors occur
 */
void Case006() {
  printf("[006] MmapCache/Invalidate/Munmap (cached)\n");
  const uint32_t size = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  axsys::CmmView vbase =
      buf.Allocate(size, axsys::CacheMode::kNonCached, "cmm_006");
  axsys::CmmView vcache = buf.MapView(0, size, axsys::CacheMode::kCached);
  printf("  base=%p map=%p\n", vbase.Data(), vcache.Data());

  memset(vbase.Data(), 0xbc, size);
  memset(vcache.Data(), 0xfa, size);

  // Before invalidate
  printf("  base(before) : ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vbase.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");
  printf("  cache(before): ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vcache.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");

  bool invalidated = vcache.Invalidate();
  if (!invalidated) {
    printf("  invalidate failed\n");
    printf("  result: fail\n");
    return;
  }
  memset(vbase.Data(), 0xbb, size);

  // After invalidate
  printf("  base(after)  : ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vbase.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");
  printf("  cache(after) : ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vcache.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");

  bool ok = true;
  for (uint32_t i = 0; i < size; ++i) {
    if (static_cast<uint8_t*>(vbase.Data())[i] !=
        static_cast<uint8_t*>(vcache.Data())[i]) {
      printf("  mismatch i=%u vbase=0x%x vcache=0x%x\n", i,
             static_cast<unsigned>(static_cast<uint8_t*>(vbase.Data())[i]),
             static_cast<unsigned>(static_cast<uint8_t*>(vcache.Data())[i]));
      ok = false;
      break;
    }
  }
  printf("  result: %s\n", ok ? "pass" : "fail");
  printf("\n");
}

/**
 * @brief Case 007: Flush (MflushCache) with increasing sizes (SDK-aligned).
 *
 * Purpose
 * - Confirm that after writing via the cached source, flushing updates the
 *   physical region so a subsequent copy reflects the new data.
 *
 * Steps
 * - For j=1..32: size=j*1MiB
 * - Allocate src (cached) and dst (non-cached)
 * - Initialize src to 0x78 and override first 256 bytes as (255-i)
 * - Flush src caches (CmmView::Flush)
 * - Copy src->dst using MemcpyView (non-cached alias if needed)
 * - Full-range compare; accumulate pass/fail counts
 *
 * Expected
 * - All iterations pass (Total=32, Fail=0)
 */
void Case007() {
  printf("[007] MflushCache scaling sizes\n");
  const uint32_t kTests = 1;  // TODO(wip): 32
  uint32_t pass = 0, fail = 0;
  for (uint32_t j = 1; j <= kTests; ++j) {
    const uint32_t sz = j * 1024 * 1024;
    axsys::CmmBuffer src, dst;
    axsys::CmmView vsrc =
        src.Allocate(sz, axsys::CacheMode::kCached, "cmm_007_src");
    axsys::CmmView vdst =
        dst.Allocate(sz, axsys::CacheMode::kNonCached, "cmm_007_dst");

    memset(vsrc.Data(), 0x78, sz);
    for (uint32_t i = 0; i < 256 && i < sz; ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = static_cast<uint8_t>(255 - i);
    }
    bool flushed = vsrc.Flush();
    if (!flushed) {
      printf("  flush failed at j=%u size=0x%x\n", j, sz);
      ++fail;
      continue;
    }
    if (MemcpyView(vsrc, vdst, sz) != 0) {
      printf("  memcpy helper failed at j=%u size=0x%x\n", j, sz);
      ++fail;
      continue;
    }

    bool ok = true;
    for (uint32_t i = 0; i < sz; ++i) {
      if (static_cast<uint8_t*>(vdst.Data())[i] !=
          static_cast<uint8_t*>(vsrc.Data())[i]) {
        ok = false;
        break;
      }
    }
    if (ok) {
      ++pass;
    } else {
      ++fail;
    }
  }
  printf("  end. Total:%u, Pass:%u, Fail:%u\n", kTests, pass, fail);
  printf("\n");
}

/**
 * @brief Case 008: Invalidate (MinvalidateCache) with increasing sizes.
 *
 * Purpose
 * - Confirm that after copying into the physical region, invalidating the
 *   cached destination makes the cached view reflect the new data.
 *
 * Steps (SDK-aligned)
 * - For j = 1..32 MiB: size = j * 1MiB
 * - Allocate src (non-cached) and dst (cached)
 * - Initialize src = 0xFF (first 256: 255-i), dst = 0xEE (first 256: i)
 * - Copy src -> dst with MemcpyView (uses non-cached alias if needed)
 * - Invalidate the cached destination view (discard stale lines)
 * - Full-range compare src vs dst; count pass/fail
 *
 * Expected
 * - No errors; all iterations pass (Total=32, Fail=0)
 */
void Case008() {
  printf("[008] MinvalidateCache scaling sizes\n");
  const uint32_t kTests = 1;  // TODO(wip): 32
  uint32_t pass = 0, fail = 0;
  for (uint32_t j = 1; j <= kTests; ++j) {
    const uint32_t sz = j * 1024 * 1024;
    axsys::CmmBuffer src, dst;
    axsys::CmmView vsrc =
        src.Allocate(sz, axsys::CacheMode::kNonCached, "cmm_008_src");
    axsys::CmmView vdst =
        dst.Allocate(sz, axsys::CacheMode::kCached, "cmm_008_dst");

    memset(vsrc.Data(), 0xff, sz);
    for (uint32_t i = 0; i < 256 && i < sz; ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = static_cast<uint8_t>(255 - i);
    }

    memset(vdst.Data(), 0xee, sz);
    for (uint32_t i = 0; i < 256 && i < sz; ++i) {
      static_cast<uint8_t*>(vdst.Data())[i] = static_cast<uint8_t>(i);
    }

    bool flushed = vdst.Flush();
    if (!flushed) {
      printf("  flush failed at j=%u size=0x%x\n", j, sz);
      ++fail;
      continue;
    }

    if (MemcpyView(vsrc, vdst, sz) != 0) {
      printf("  memcpy helper failed at j=%u size=0x%x\n", j, sz);
      ++fail;
      continue;
    }

    bool invalidated = vdst.Invalidate();
    if (!invalidated) {
      printf("  invalidate failed at j=%u size=0x%x\n", j, sz);
      ++fail;
      continue;
    }

    bool ok = true;
    for (uint32_t i = 0; i < sz; ++i) {
      if (static_cast<uint8_t*>(vdst.Data())[i] !=
          static_cast<uint8_t*>(vsrc.Data())[i]) {
        ok = false;
        break;
      }
    }
    if (ok) {
      ++pass;
    } else {
      ++fail;
    }

    memset(vsrc.Data(), 0xbc, sz);
  }
  printf("  end. Total:%u, Pass:%u, Fail:%u\n", kTests, pass, fail);
  printf("\n");
}

/**
 * @brief Case 009: Flush with offset (cached src -> non-cached dst).
 *
 * Purpose
 * - Verify that flushing a cached source starting at a given offset makes
 *   that subrange visible to a non-cached destination copy.
 *
 * Steps
 * - Use kTests iterations (kTests=100), fixed size=4 MiB, offset=2 MiB
 * - Allocate src (cached) and dst (non-cached)
 * - Initialize src (0x78; first 256 bytes descending)
 * - Call vsrc.Flush(offset, size - offset) to flush the subrange
 * - Copy full buffer src->dst using MemcpyView (mapping-aware helper)
 * - Compare only [offset..end) for equality; accumulate pass/fail
 * - Print totals at the end
 *
 * Expected
 * - All iterations pass; region [offset..end) matches after flush
 */
void Case009() {
  printf("[009] Flush with offset (cached src -> noncached dst)\n");
  const uint32_t kTests = 1;  // TODO(wip): 100
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 2 * 1024 * 1024;
  uint32_t pass = 0, fail = 0;
  for (uint32_t t = 0; t < kTests; ++t) {
    axsys::CmmBuffer src, dst;
    axsys::CmmView vsrc =
        src.Allocate(size, axsys::CacheMode::kCached, "cmm_009_src");
    axsys::CmmView vdst =
        dst.Allocate(size, axsys::CacheMode::kNonCached, "cmm_009_dst");

    memset(vsrc.Data(), 0x78, size);
    for (uint32_t i = 0; i < 256 && i < size; ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = static_cast<uint8_t>(255 - i);
    }

    if (!vsrc.Flush(offset, size - offset)) {
      ++fail;
      continue;
    }

    memset(vdst.Data(), 0x39, size);

    if (MemcpyView(vsrc, vdst, size) != 0) {
      ++fail;
      continue;
    }

    bool ok = true;
    for (uint32_t i = offset; i < size; ++i) {
      if (static_cast<uint8_t*>(vdst.Data())[i] !=
          static_cast<uint8_t*>(vsrc.Data())[i]) {
        ok = false;
        break;
      }
    }
    if (ok)
      ++pass;
    else
      ++fail;

    memset(vdst.Data(), 0x93, size);
    memset(vsrc.Data(), 0x98, size);
  }
  printf("  end. Total:%u, Pass:%u, Fail:%u\n", kTests, pass, fail);
  printf("\n");
}

/**
 * @brief Case 010: Flush with offset (repeat validation of 009).
 *
 * Purpose
 * - Re-validate that flushing a cached source starting at a given offset
 *   makes that subrange visible to a non-cached destination copy.
 *
 * Steps
 * - Use kTests iterations (kTests=100), fixed size=4 MiB, offset=2 MiB
 * - Allocate src (cached) and dst (non-cached)
 * - Initialize src (0x78; first 256 bytes descending)
 * - Flush the subrange [offset..end) of src (vsrc.Flush(offset, size-offset))
 * - Copy full buffer src->dst using MemcpyView (mapping-aware helper)
 * - Compare only [offset..end) for equality; accumulate pass/fail and print
 * totals
 *
 * Expected
 * - All iterations pass; region [offset..end) matches after flush
 */
void Case010() {
  printf("[010] Flush with offset (repeat of 009)\n");
  const uint32_t kTests = 1;  // TODO(wip): 100
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 2 * 1024 * 1024;
  uint32_t pass = 0, fail = 0;
  for (uint32_t t = 0; t < kTests; ++t) {
    axsys::CmmBuffer src, dst;
    axsys::CmmView vsrc =
        src.Allocate(size, axsys::CacheMode::kCached, "cmm_010_src");
    axsys::CmmView vdst =
        dst.Allocate(size, axsys::CacheMode::kNonCached, "cmm_010_dst");

    memset(vsrc.Data(), 0x78, size);
    for (uint32_t i = 0; i < 256 && i < size; ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = static_cast<uint8_t>(255 - i);
    }

    if (!vsrc.Flush(offset, size - offset)) {
      ++fail;
      continue;
    }

    memset(vdst.Data(), 0x39, size);

    if (MemcpyView(vsrc, vdst, size) != 0) {
      ++fail;
      continue;
    }

    bool ok = true;
    for (uint32_t i = offset; i < size; ++i) {
      if (static_cast<uint8_t*>(vdst.Data())[i] !=
          static_cast<uint8_t*>(vsrc.Data())[i]) {
        ok = false;
        break;
      }
    }
    if (ok)
      ++pass;
    else
      ++fail;

    memset(vdst.Data(), 0x93, size);
    memset(vsrc.Data(), 0x98, size);
  }
  printf("  end. Total:%u, Pass:%u, Fail:%u\n", kTests, pass, fail);
  printf("\n");
}

/**
 * @brief Case 011: Flush subrange with offset (expect pass).
 *
 * Purpose
 * - Verify that flushing only a subrange [offset..offset+len) of a cached
 *   source makes exactly that region visible to a non-cached destination.
 *
 * Steps
 * - Use kTests iterations (kTests=100), size=4 MiB, offset=1 MiB, len=size/4
 * - Allocate src (cached) and dst (non-cached)
 * - Initialize src (0x88; first 256 bytes descending)
 * - Flush subrange [offset..offset+len) of src via vsrc.Flush(offset, len)
 * - Copy full buffer src->dst using MemcpyView (mapping-aware helper)
 * - Compare only [offset..offset+len) for equality; accumulate pass/fail and
 *   print totals
 *
 * Expected
 * - All iterations pass; region [offset..offset+len) matches after flush
 */
void Case011() {
  printf("[011] Flush subrange with offset (expect pass)\n");
  const uint32_t kTests = 1;  // TODO(wip): 100
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 4;
  uint32_t pass = 0, fail = 0;
  for (uint32_t t = 0; t < kTests; ++t) {
    axsys::CmmBuffer src, dst;
    axsys::CmmView vsrc =
        src.Allocate(size, axsys::CacheMode::kCached, "cmm_011_src");
    axsys::CmmView vdst =
        dst.Allocate(size, axsys::CacheMode::kNonCached, "cmm_011_dst");

    memset(vsrc.Data(), 0x88, size);
    for (uint32_t i = 0; i < 256 && i < size; ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = static_cast<uint8_t>(255 - i);
    }

    if (!vsrc.Flush(offset, len)) {
      ++fail;
      continue;
    }

    memset(vdst.Data(), 0x49, size);

    if (MemcpyView(vsrc, vdst, size) != 0) {
      ++fail;
      continue;
    }

    bool ok = true;
    for (uint32_t i = offset; i < offset + len; ++i) {
      if (static_cast<uint8_t*>(vdst.Data())[i] !=
          static_cast<uint8_t*>(vsrc.Data())[i]) {
        ok = false;
        break;
      }
    }
    if (ok)
      ++pass;
    else
      ++fail;

    memset(vdst.Data(), 0x93, size);
    memset(vsrc.Data(), 0x98, size);
  }
  printf("  end. Total:%u, Pass:%u, Fail:%u\n", kTests, pass, fail);
  printf("\n");
}

/**
 * @brief Case 012: Flush subrange then compare bigger range (expect fail).
 *
 * Purpose
 * - Verify that flushing only a subrange [offset..offset+len) does not make
 *   a larger region [offset..offset+cmp) fully visible (comparison should
 * fail).
 *
 * Steps
 * - Use kTests=100 iterations, size=4 MiB, offset=1 MiB, len=size/4,
 * cmp=size/2
 * - Allocate src (cached) and dst (non-cached)
 * - Initialize src (0x88; first 256 bytes descending)
 * - Flush [offset..offset+len) via vsrc.Flush(offset, len)
 * - Copy full buffer src->dst using MemcpyView
 * - Compare [offset..offset+cmp); accumulate pass/fail and print totals
 *
 * Expected
 * - Iterations report fail (expected), since cmp > flushed length
 */
void Case012() {
  printf("[012] Flush subrange then compare bigger range (expect fail)\n");
  const uint32_t kTests = 1;  // TODO(wip): 100
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 4;  // flushed length
  const uint32_t cmp = size / 2;  // compare longer than flushed
  uint32_t pass = 0, fail = 0;
  for (uint32_t t = 0; t < kTests; ++t) {
    axsys::CmmBuffer src, dst;
    axsys::CmmView vsrc =
        src.Allocate(size, axsys::CacheMode::kCached, "cmm_012_src");
    axsys::CmmView vdst =
        dst.Allocate(size, axsys::CacheMode::kNonCached, "cmm_012_dst");

    memset(vsrc.Data(), 0x88, size);
    for (uint32_t i = 0; i < 256 && i < size; ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = static_cast<uint8_t>(255 - i);
    }

    if (!vsrc.Flush(offset, len)) {
      ++fail;
      continue;
    }

    memset(vdst.Data(), 0x49, size);

    if (MemcpyView(vsrc, vdst, size) != 0) {
      ++fail;
      continue;
    }

    bool ok = true;
    for (uint32_t i = offset; i < offset + cmp; ++i) {
      if (static_cast<uint8_t*>(vdst.Data())[i] !=
          static_cast<uint8_t*>(vsrc.Data())[i]) {
        ok = false;
        break;
      }
    }
    if (ok)
      ++pass;
    else
      ++fail;

    memset(vdst.Data(), 0x93, size);
    memset(vsrc.Data(), 0x98, size);
  }
  printf("  end. Total:%u, Pass:%u, Fail:%u\n", kTests, pass, fail);
  printf("\n");
}

/**
 * @brief Case 013: Flush subrange + compare (expect pass).
 *
 * Purpose
 * - Verify that flushing the cached view's subrange [offset..offset+len)
 *   updates the physical region so the base (non-cached) view matches in that
 *   subrange.
 *
 * Steps
 * - Use kTests=100 iterations, size=4 MiB, offset=1 MiB, len=size/2
 * - Allocate base (non-cached) and cached view over the same block
 * - Initialize cached with 0xFE
 * - Flush cached subrange [offset..offset+len)
 * - Compare base vs cached for [offset..offset+len); accumulate pass/fail;
 * print totals
 *
 * Expected
 * - All iterations pass; region [offset..offset+len) matches after flush
 */
void Case013() {
  printf("[013] MmapCache + Flush subrange + compare (expect pass)\n");
  const uint32_t kTests = 1;  // TODO(wip): 100
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 2;
  uint32_t pass = 0, fail = 0;
  for (uint32_t t = 0; t < kTests; ++t) {
    axsys::CmmBuffer buf;
    axsys::CmmView base =
        buf.Allocate(size, axsys::CacheMode::kNonCached, "cmm_013_base");
    axsys::CmmView cached = buf.MapView(0, size, axsys::CacheMode::kCached);

    memset(base.Data(), 0xfd, size);
    memset(cached.Data(), 0xfe, size);

    if (!cached.Flush(offset, len)) {
      ++fail;
      continue;
    }

    bool ok = true;
    for (uint32_t i = offset; i < offset + len; ++i) {
      if (static_cast<uint8_t*>(base.Data())[i] !=
          static_cast<uint8_t*>(cached.Data())[i]) {
        ok = false;
        break;
      }
    }
    if (ok)
      ++pass;
    else
      ++fail;
  }
  printf("  end. Total:%u, Pass:%u, Fail:%u\n", kTests, pass, fail);
  printf("\n");
}

/**
 * @brief Case 014: Flush subrange + compare bigger (expect fail).
 *
 * Purpose
 * - Verify that flushing only a subrange [offset..offset+len) does not make
 *   the larger region [offset..end) fully coherent; comparison should fail.
 *
 * Steps
 * - Use kTests=100 iterations, size=4 MiB, offset=1 MiB, len=size/2
 * - Allocate base (non-cached) and cached view over the same block
 * - Initialize cached with 0x66
 * - Flush cached subrange [offset..offset+len)
 * - Compare [offset..end); accumulate pass/fail; print totals
 *
 * Expected
 * - Iterations report fail (expected), since compare range > flushed length
 */
void Case014() {
  printf("[014] MmapCache + Flush subrange + compare bigger (expect fail)\n");
  const uint32_t kTests = 1;  // TODO(wip): 100
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 8;
  uint32_t pass = 0, fail = 0;
  for (uint32_t t = 0; t < kTests; ++t) {
    axsys::CmmBuffer buf;
    axsys::CmmView base =
        buf.Allocate(size, axsys::CacheMode::kNonCached, "cmm_014_base");
    axsys::CmmView cached = buf.MapView(0, size, axsys::CacheMode::kCached);

    memset(base.Data(), 0x85, size);
    memset(cached.Data(), 0x66, size);

    if (!cached.Flush(offset, len)) {
      ++fail;
      continue;
    }

    bool ok = true;
    for (uint32_t i = offset; i < size; ++i) {
      if (static_cast<uint8_t*>(base.Data())[i] !=
          static_cast<uint8_t*>(cached.Data())[i]) {
        ok = false;
        break;
      }
    }
    if (ok)
      ++pass;
    else
      ++fail;

    memset(base.Data(), 0x88, size);
    memset(cached.Data(), 0x94, size);
  }
  printf("  end. Total:%u, Pass:%u, Fail:%u\n", kTests, pass, fail);
  printf("\n");
}

/**
 * @brief Case 015: External attach + cached/non-cached views + Flush.
 *
 * Purpose
 * - Verify that when writing via a cached view on an external (attached)
 *   physical range, calling Flush makes the non-cached view see the data.
 *
 * Steps
 * - Find the anonymous partition and pick a 1 MiB region near the end
 * - For kTests=100 iterations:
 *   - Attach external buffer (no ownership) at the chosen phys
 *   - Map non-cached and cached views over the same range
 *   - Write nc=0xDF, cached=0xDE; call cached.Flush()
 *   - Compare full range nc vs cached; accumulate pass/fail; print totals
 *
 * Expected
 * - All iterations pass; both views match after Flush
 */
void Case015() {
  printf("[015] External attach + cached/noncached views + Flush\n");
  axsys::CmmBuffer::PartitionInfo part;
  if (!axsys::CmmBuffer::FindAnonymous(&part)) return;

  const uint32_t block_size = 1 * 1024 * 1024;
  const uint64_t phys =
      part.phys + static_cast<uint64_t>(part.size_kb) * 1024 - block_size * 2;
  const uint32_t kTests = 1;  // TODO(wip): 100
  uint32_t pass = 0, fail = 0;

  for (uint32_t t = 0; t < kTests; ++t) {
    axsys::CmmBuffer buf;
    if (!buf.AttachExternal(phys, block_size)) {
      ++fail;
      continue;
    }
    axsys::CmmView nc =
        buf.MapView(0, block_size, axsys::CacheMode::kNonCached);
    axsys::CmmView c = buf.MapView(0, block_size, axsys::CacheMode::kCached);

    if (!nc || !c) {
      ++fail;
      continue;
    }

    memset(nc.Data(), 0xdf, block_size);
    memset(c.Data(), 0xde, block_size);

    if (!c.Flush()) {
      ++fail;
      continue;
    }

    bool ok = true;
    for (uint32_t i = 0; i < block_size; ++i) {
      if (static_cast<uint8_t*>(nc.Data())[i] !=
          static_cast<uint8_t*>(c.Data())[i]) {
        ok = false;
        break;
      }
    }
    if (ok)
      ++pass;
    else
      ++fail;
  }
  printf("  end. Total:%u, Pass:%u, Fail:%u\n", kTests, pass, fail);
  printf("\n");
}

/**
 * @brief Case 016: External attach + cached/non-cached views + Invalidate.
 *
 * Purpose
 * - Confirm that invalidating a cached view over an external (attached)
 *   physical range makes subsequent base (non-cached) writes visible.
 *
 * Steps
 * - Find anonymous partition; choose 1 MiB region (phys near end)
 * - For kTests=100 iterations:
 *   - Attach external buffer (non-owned)
 *   - Map non-cached (nc) and cached (c) views
 *   - Initialize nc=0xBC, c=0xFA; call c.Invalidate()
 *   - Modify base (nc=0xBB)
 *   - Compare full range nc vs c; accumulate pass/fail; print totals
 *
 * Expected
 * - All iterations pass; cached view reflects base writes after Invalidate
 */
void Case016() {
  printf("[016] External attach + cached/noncached views + Invalidate\n");
  axsys::CmmBuffer::PartitionInfo part;
  if (!axsys::CmmBuffer::FindAnonymous(&part)) return;

  const uint32_t block_size = 1 * 1024 * 1024;
  const uint64_t phys =
      part.phys + static_cast<uint64_t>(part.size_kb) * 1024 - block_size * 2;
  const uint32_t kTests = 1;  // TODO(wip): 100
  uint32_t pass = 0, fail = 0;

  for (uint32_t t = 0; t < kTests; ++t) {
    axsys::CmmBuffer buf;
    if (!buf.AttachExternal(phys, block_size)) {
      ++fail;
      continue;
    }
    axsys::CmmView nc =
        buf.MapView(0, block_size, axsys::CacheMode::kNonCached);
    axsys::CmmView c = buf.MapView(0, block_size, axsys::CacheMode::kCached);

    if (!nc || !c) {
      ++fail;
      continue;
    }

    memset(nc.Data(), 0xbc, block_size);
    memset(c.Data(), 0xfa, block_size);

    if (!c.Invalidate()) {
      ++fail;
      continue;
    }

    memset(nc.Data(), 0xbb, block_size);

    bool ok = true;
    for (uint32_t i = 0; i < block_size; ++i) {
      if (static_cast<uint8_t*>(nc.Data())[i] !=
          static_cast<uint8_t*>(c.Data())[i]) {
        ok = false;
        break;
      }
    }
    if (ok)
      ++pass;
    else
      ++fail;
  }
  printf("  end. Total:%u, Pass:%u, Fail:%u\n", kTests, pass, fail);
  printf("\n");
}

/**
 * @brief Case 017: Block info on cached virtual address.
 *
 * Purpose
 * - Inspect block info for a cached mapping using ByVirt at base and +0x1000,
 *   and ByPhy for the allocation, to confirm consistency.
 *
 * Steps
 * - Allocate 1 MiB cached; keep the returned view
 * - Call buf.Dump() for ByPhy (base phys)
 * - Call v.Dump(0) and v.Dump(0x1000) for ByVirt on cached virt
 * - Optionally verify() for allocation sanity
 *
 * Expected
 * - Consistent phys and cacheType across ByVirt queries; valid ByPhy info
 */
void Case017() {
  printf("[017] Block info (cached virt)\n");
  axsys::CmmBuffer buf;
  axsys::CmmView v =
      buf.Allocate(1 * 1024 * 1024, axsys::CacheMode::kCached, "cmm_017");
  buf.Dump();
  v.Dump(0);
  v.Dump(0x1000);
  printf("  verify=%s\n", buf.Verify() ? "true" : "false");
  printf("\n");
}

/**
 * @brief Case 018: Block info on mapped non-cached address.
 *
 * Purpose
 * - Mirror SDK test_018: query block info by virtual for a non-cached mapping
 *   at base, +0x1000, and at a non page-aligned offset (+0x11ef). Also query
 *   block info by physical for the allocation.
 *
 * Steps
 * - Allocate 1 MiB non-cached and keep the returned base view (already
 *   non-cached mapped).
 * - Verify allocation structure via CmmBuffer::Verify().
 * - Dump ByVirt at offsets: 0, 0x1000, 0x11ef using CmmView::Dump().
 * - Dump ByPhy for the allocation using CmmBuffer::Dump().
 *
 * Expected
 * - ByVirt queries succeed and report consistent physical address range and
 *   a non-cached cacheType. ByPhy returns valid virt, cacheType and block size.
 */
void Case018() {
  printf("[018] Block info (mapped non-cached)\n");
  axsys::CmmBuffer buf;
  axsys::CmmView v =
      buf.Allocate(1 * 1024 * 1024, axsys::CacheMode::kNonCached, "cmm_018");
  buf.Dump();
  v.Dump(0);
  v.Dump(0x1000);
  v.Dump(0x11ef);
  printf("  verify=%s\n", buf.Verify() ? "true" : "false");
  printf("\n");
}

/**
 * @brief Case 019: Block info on mapped cached address.
 *
 * Purpose
 * - Mirror SDK test_019: query block info by virtual for a cached mapping
 *   (alias) and for the base non-cached mapping, at base, +0x1000 and a
 *   non page-aligned offset. Also query block info by physical at base and
 *   offsets (+0x1000, +0x1ef).
 *
 * Steps
 * - Allocate 1 MiB as non-cached; keep the base view.
 * - Map a cached alias over the full range.
 * - Dump ByVirt on cached alias at 0, 0x1000, 0x11ef.
 * - Dump ByVirt on base non-cached at 0.
 * - Dump ByPhy for phys at 0, 0x1000, 0x1ef via CmmBuffer::Dump(offset).
 *
 * Expected
 * - ByVirt queries for both cached and non-cached virtual addresses succeed
 *   with consistent physical mapping; cacheType reflects each mapping.
 * - ByPhy returns a valid virtual, cacheType and block size for the phys
 *   within the allocation range.
 */
void Case019() {
  printf("[019] Block info (mapped cached)\n");
  axsys::CmmBuffer buf;
  axsys::CmmView vbase =
      buf.Allocate(1 * 1024 * 1024, axsys::CacheMode::kNonCached, "cmm_019");
  axsys::CmmView vcache =
      buf.MapView(0, vbase.Size(), axsys::CacheMode::kCached);
  // ByVirt on cached alias and base
  vbase.Dump(0);
  vcache.Dump(0);
  vcache.Dump(0x1000);
  vcache.Dump(0x11ef);
  // ByPhy on phys, phys+0x1000, phys+0x1ef
  buf.Dump(0);
  buf.Dump(0x1000);
  buf.Dump(0x1ef);
  printf("  verify=%s\n", buf.Verify() ? "true" : "false");
  printf("\n");
}

/**
 * @brief Case 020: POOL block mapping and cache ops.
 *
 * Purpose
 * - Mirror SDK test_020 using POOL: acquire a block, write/read via the
 *   pool-provided virtual, then map the physical non-cached and cached, read
 *   back, perform Flush/Invalidate, and clean up.
 *
 * Steps
 * - Configure a POOL (anonymous). Init and get one block (3 MiB).
 * - Get phys and pool block virtual address; write 20 ints [0..19] and print.
 * - Mmap non-cached by phys, print first 20 values, then munmap.
 * - Mmap cached by phys, print first 20 values; Flush and Invalidate; munmap.
 * - Release the block and exit POOL.
 *
 * TODO
 * - Replace direct AX_SYS_* calls with libax_sys_cpp abstractions once POOL
 *   helpers are provided (e.g., pool-backed views and cache ops).
 */
void Case020() {
  printf("[020] POOL block + Mmap/MmapCache\n");
  AX_POOL_FLOORPLAN_T plan;
  memset(&plan, 0, sizeof(plan));
  plan.CommPool[0].MetaSize = 0x1000;
  plan.CommPool[0].BlkSize = 3 * 1024 * 1024;
  plan.CommPool[0].BlkCnt = 1;
  plan.CommPool[0].CacheMode = AX_POOL_CACHE_MODE_NONCACHE;
  snprintf(reinterpret_cast<char*>(plan.CommPool[0].PartitionName),
           sizeof(plan.CommPool[0].PartitionName), "%s", "anonymous");

  if (AX_POOL_Exit() != 0) {
    printf("AX_POOL_Exit failed\n");
    return;
  }
  if (AX_POOL_SetConfig(&plan) != 0) {
    printf("AX_POOL_SetConfig failed\n");
    return;
  }
  if (AX_POOL_Init() != 0) {
    printf("AX_POOL_Init failed\n");
    return;
  }

  AX_U32 blk_size = static_cast<AX_U32>(plan.CommPool[0].BlkSize);
  AX_BLK blk = AX_POOL_GetBlock(AX_INVALID_POOLID, blk_size, nullptr);
  if (blk == AX_INVALID_BLOCKID) {
    printf("AX_POOL_GetBlock failed\n");
    AX_POOL_Exit();
    return;
  }
  printf("  AX_POOL_GetBlock ok, BlkId=0x%X\n", blk);

  AX_U64 phys = AX_POOL_Handle2PhysAddr(blk);
  if (!phys) {
    printf("AX_POOL_Handle2PhysAddr failed\n");
    AX_POOL_ReleaseBlock(blk);
    AX_POOL_Exit();
    return;
  }
  printf("  Phys=0x%" PRIx64 "\n", static_cast<uint64_t>(phys));

  void* pool_v = AX_POOL_GetBlockVirAddr(blk);
  if (!pool_v) {
    printf("AX_POOL_GetBlockVirAddr failed\n");
    AX_POOL_ReleaseBlock(blk);
    AX_POOL_Exit();
    return;
  }
  printf("  pool v=%p\n", pool_v);

  // write via pool virtual
  for (int i = 0; i < 20; ++i) {
    reinterpret_cast<int32_t*>(pool_v)[i] = i;
  }
  // dump first 16 bytes in Case004 format
  printf("  ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ", static_cast<unsigned>(static_cast<uint8_t*>(pool_v)[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");

  // map non-cached by phys and read
  void* v_nc = AX_SYS_Mmap(phys, blk_size);
  if (!v_nc) {
    printf("AX_SYS_Mmap failed\n");
  } else {
    printf("  mmap nonc v=%p size=0x%x\n", v_nc, blk_size);
    printf("  ");
    for (int i = 0; i < 16; ++i) {
      printf("%02x ", static_cast<unsigned>(static_cast<uint8_t*>(v_nc)[i]));
      if (i == 7) printf(" ");
    }
    printf("\n");
    AX_SYS_Munmap(v_nc, blk_size);
  }

  // map cached by phys and read; then flush/invalidate and unmap
  void* v_c = AX_SYS_MmapCache(phys, blk_size);
  if (!v_c) {
    printf("AX_SYS_MmapCache failed\n");
  } else {
    printf("  mmap cached v=%p size=0x%x\n", v_c, blk_size);
    printf("  ");
    for (int i = 0; i < 16; ++i) {
      printf("%02x ", static_cast<unsigned>(static_cast<uint8_t*>(v_c)[i]));
      if (i == 7) printf(" ");
    }
    printf("\n");
    (void)AX_SYS_MflushCache(phys, v_c, blk_size);
    (void)AX_SYS_MinvalidateCache(phys, v_c, blk_size);
    AX_SYS_Munmap(v_c, blk_size);
  }
  printf("\n");

  (void)AX_POOL_ReleaseBlock(blk);
  (void)AX_POOL_Exit();
}

/**
 * @brief Case 021: MmapFast address consistency and data parity.
 *
 * Purpose
 * - Mirror SDK test_021: ensure AX_SYS_MmapFast returns a stable address on
 *   repeated calls, and that data written via the fast-mapped view matches the
 *   base non-cached mapping.
 *
 * Steps
 * - Allocate 4 MiB non-cached; keep base view.
 * - Map a fast non-cached view over the full range; write 0x78 pattern and
 *   dump first 16 bytes.
 * - Compare entire range base vs fast-mapped view.
 * - Map a second fast view and check address equality with the first.
 *
 * Expected
 * - Full-range compare passes.
 * - Two fast-mapped addresses are identical.
 */
void Case021() {
  printf("[021] MmapFast address consistency\n");
  const uint32_t size = 4 * 1024 * 1024;
  axsys::CmmBuffer buf;
  axsys::CmmView vbase =
      buf.Allocate(size, axsys::CacheMode::kNonCached, "cmm_021");
  axsys::CmmView vmap = buf.MapViewFast(0, size, axsys::CacheMode::kNonCached);
  printf("  base=%p map=%p\n", vbase.Data(), vmap.Data());

  memset(vmap.Data(), 0x78, size);

  printf("  ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vmap.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");

  bool ok = true;
  for (uint32_t i = 0; i < size; ++i) {
    if (static_cast<uint8_t*>(vbase.Data())[i] !=
        static_cast<uint8_t*>(vmap.Data())[i]) {
      printf("  mismatch i=%u base=0x%x map=0x%x\n", i,
             static_cast<unsigned>(static_cast<uint8_t*>(vbase.Data())[i]),
             static_cast<unsigned>(static_cast<uint8_t*>(vmap.Data())[i]));
      ok = false;
      break;
    }
  }
  printf("  result: %s\n", ok ? "pass" : "fail");

  axsys::CmmView vmap2 = buf.MapViewFast(0, size, axsys::CacheMode::kNonCached);
  printf("  map1=%p map2=%p%s\n", vmap.Data(), vmap2.Data(),
         (vmap.Data() == vmap2.Data() ? " (same)" : " (diff)"));
  printf("\n");
}

/**
 * @brief Case 022: MmapCacheFast address consistency (cached fast mapping).
 *
 * Purpose
 * - Mirror SDK test_022: ensure AX_SYS_MmapCacheFast returns a stable address
 *   across repeated calls, and show a small hexdump after writing via the
 *   cached fast mapping.
 *
 * Steps
 * - Allocate 4 MiB non-cached (base remains unused for data compare here).
 * - Map a cached fast view; write 0x78 pattern; dump first 16 bytes.
 * - Map a second cached fast view and check if the address equals the first.
 *
 * Expected
 * - Two cached fast-mapped addresses are identical.
 */
void Case022() {
  printf("[022] MmapCacheFast address consistency\n");
  const uint32_t size = 4 * 1024 * 1024;
  axsys::CmmBuffer buf;
  (void)buf.Allocate(size, axsys::CacheMode::kNonCached, "cmm_022");
  axsys::CmmView v1 = buf.MapViewFast(0, size, axsys::CacheMode::kCached);
  if (!v1) {
    printf("  MmapCacheFast failed\n");
    return;
  }

  memset(v1.Data(), 0x78, size);

  printf("  ");
  for (int i = 0; i < 16; ++i) {
    printf("%x ", static_cast<unsigned>(static_cast<uint8_t*>(v1.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");

  axsys::CmmView v2 = buf.MapViewFast(0, size, axsys::CacheMode::kCached);
  printf("  map1=%p map2=%p%s\n", v1.Data(), v2.Data(),
         (v1.Data() == v2.Data() ? " (same)" : " (diff)"));
  printf("\n");
}

/**
 * @brief Case 023: MmapCacheFast + Flush + compare (SDK-aligned).
 *
 * Purpose
 * - Match SDK test_023: write 0xfd to base (non-cached), map cached-fast and
 *   write 0xfe there, flush cached view, then compare base vs cached data.
 *
 * Steps
 * - Allocate 1 MiB non-cached base view; memset base to 0xfd.
 * - Map cached-fast full-range view; memset to 0xfe; Flush() and verify.
 * - Print first 16 bytes for base and cache; compare full block; print result.
 */
void Case023() {
  printf("[023] MmapCacheFast + MflushCache + Munmap\n");
  const uint32_t size = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  axsys::CmmView vbase =
      buf.Allocate(size, axsys::CacheMode::kNonCached, "cmm_023");
  memset(vbase.Data(), 0xfd, size);

  axsys::CmmView vcache = buf.MapViewFast(0, size, axsys::CacheMode::kCached);
  if (!vcache) {
    printf("  MmapCacheFast failed\n");
    return;
  }
  memset(vcache.Data(), 0xfe, size);
  bool flushed = vcache.Flush();
  if (!flushed) {
    printf("  Flush failed\n");
    printf("  result: fail\n\n");
    return;
  }

  // dumps in Case004 hex format
  printf("  base  : ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vbase.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");
  printf("  cache : ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vcache.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");

  bool ok = true;
  for (uint32_t i = 0; i < size; ++i) {
    if (static_cast<uint8_t*>(vbase.Data())[i] !=
        static_cast<uint8_t*>(vcache.Data())[i]) {
      ok = false;
      break;
    }
  }
  printf("  result: %s\n\n", ok ? "pass" : "fail");
}

/**
 * @brief Case 024: MmapCacheFast + Invalidate + compare (SDK-aligned).
 *
 * Purpose
 * - Match SDK test_024: initialize base (0xbc) and cached-fast alias (0xfa),
 *   then Invalidate cached view, modify base to 0xbb, and verify both views
 *   show the same data. Prints hexdumps before/after like Case006.
 *
 * Steps
 * - Allocate 1 MiB non-cached base; map cached-fast alias.
 * - memset base=0xbc, cache=0xfa; print first 16 bytes of each (before).
 * - Invalidate cached view; write base=0xbb.
 * - Print first 16 bytes of each (after) and compare entire range.
 */
void Case024() {
  printf("[024] MmapCacheFast + MinvalidateCache + Munmap\n");
  const uint32_t size = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  axsys::CmmView vbase =
      buf.Allocate(size, axsys::CacheMode::kNonCached, "cmm_024");
  axsys::CmmView vcache = buf.MapViewFast(0, size, axsys::CacheMode::kCached);
  if (!vcache) {
    printf("  MmapCacheFast failed\n");
    return;
  }

  memset(vbase.Data(), 0xbc, size);
  memset(vcache.Data(), 0xfa, size);

  // Before invalidate
  printf("  base(before) : ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vbase.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");
  printf("  cache(before): ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vcache.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");

  bool invalidated = vcache.Invalidate();
  if (!invalidated) {
    printf("  Invalidate failed\n");
    printf("  result: fail\n\n");
    return;
  }
  memset(vbase.Data(), 0xbb, size);

  // After invalidate
  printf("  base(after)  : ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vbase.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");
  printf("  cache(after) : ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x ",
           static_cast<unsigned>(static_cast<uint8_t*>(vcache.Data())[i]));
    if (i == 7) printf(" ");
  }
  printf("\n");

  bool ok = true;
  for (uint32_t i = 0; i < size; ++i) {
    if (static_cast<uint8_t*>(vbase.Data())[i] !=
        static_cast<uint8_t*>(vcache.Data())[i]) {
      ok = false;
      break;
    }
  }
  printf("  result: %s\n\n", ok ? "pass" : "fail");
}

/**
 * @brief Case 025: MemQueryStatus wrapper test.
 *
 * Purpose
 * - Exercise CmmBuffer::MemQueryStatus to fetch total/remain/block count and
 *   partition list, printing a concise summary.
 */
static void Case025() {
  printf("[025] MemQueryStatus\n");
  axsys::CmmBuffer::CmmStatus st;
  if (!axsys::CmmBuffer::MemQueryStatus(&st)) {
    printf("  MemQueryStatus failed\n\n");
    return;
  }
  printf("  total=0x%x remain=0x%x blocks=%u\n", st.total_size, st.remain_size,
         st.block_count);
  printf("  partitions: %zu\n", st.partitions.size());
  for (size_t i = 0; i < st.partitions.size(); ++i) {
    const axsys::CmmBuffer::PartitionInfo& p = st.partitions[i];
    printf("    - name=%s phys=0x%" PRIx64 " size_kb=0x%x\n", p.name.c_str(),
           static_cast<uint64_t>(p.phys), p.size_kb);
  }
  printf("\n");
}

}  // namespace

int main() {
  axsys::System sys;
  if (!sys.Ok()) return -1;
  printf("sample_cmm (C++) begin\n\n");

  Case001();
  Case001r();
  Case001v();
  Case002();
  Case003();
  Case003r();
  Case004();
  Case005();
  Case006();
  Case007();
  Case008();
  Case009();
  Case010();
  Case011();
  Case012();
  Case013();
  Case014();
  Case015();
  Case016();
  Case017();
  Case018();
  Case019();
  Case020();
  Case021();
  Case022();
  Case023();
  Case024();
  Case025();

  printf("sample_cmm (C++) end\n");
  return 0;
}
// (moved to anonymous namespace above)
