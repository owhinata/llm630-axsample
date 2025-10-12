// AXERA SDK
#include <ax_sys_api.h>

// C system headers
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// libax_sys_cpp
#include "axsys/cmm.hpp"

namespace {

constexpr uint32_t kLen = 2 * 1024 * 1024;  // 2 MiB

class SystemGuard {
 public:
  SystemGuard() : ok_(AX_SYS_Init() == 0) {
    if (!ok_) printf("AX_SYS_Init failed\n");
  }
  ~SystemGuard() {
    if (ok_) AX_SYS_Deinit();
  }
  bool Ok() const { return ok_; }

 private:
  bool ok_;
};

void Case001() {
  printf("[001] MemAlloc/MemFree (non-cached)\n");
  axsys::CmmBuffer buf;
  axsys::CmmView v =
      buf.Allocate(kLen, axsys::CacheMode::kNonCached, "cmm_001");
  printf("  phy=0x%" PRIx64 ", v=0x%" PRIuPTR "\n",
         static_cast<uint64_t>(buf.Phys()),
         reinterpret_cast<uintptr_t>(v.Data()));
}

void Case002() {
  printf("[002] MemAllocCached/MemFree (cached)\n");
  axsys::CmmBuffer buf;
  axsys::CmmView v = buf.Allocate(kLen, axsys::CacheMode::kCached, "cmm_002");
  printf("  phy=0x%" PRIx64 ", v=0x%" PRIuPTR "\n",
         static_cast<uint64_t>(buf.Phys()),
         reinterpret_cast<uintptr_t>(v.Data()));
}

void Case003() {
  printf("[003] Verify/Dump (non-cached virt)\n");
  axsys::CmmBuffer buf;
  (void)buf.Allocate(1 * 1024 * 1024, axsys::CacheMode::kNonCached, "cmm_003");
  bool ok = buf.Verify();
  printf("  verify=%s\n", ok ? "true" : "false");
  buf.Dump();
}

void Case004() {
  printf("[004] Mmap/Munmap (non-cached)\n");
  axsys::CmmBuffer buf;
  axsys::CmmView v =
      buf.Allocate(kLen, axsys::CacheMode::kNonCached, "cmm_004");
  printf("  map=0x%" PRIuPTR "\n", reinterpret_cast<uintptr_t>(v.Data()));
}

void Case005() {
  printf("[005] MmapCache/Flush/Munmap (cached)\n");
  axsys::CmmBuffer buf;
  axsys::CmmView v = buf.Allocate(kLen, axsys::CacheMode::kCached, "cmm_005");
  memset(v.Data(), 0xA5, kLen);
  v.Flush();
}

void Case006() {
  printf("[006] MmapCache/Invalidate/Munmap (cached)\n");
  axsys::CmmBuffer buf;
  axsys::CmmView v = buf.Allocate(kLen, axsys::CacheMode::kCached, "cmm_006");
  v.Invalidate();
  volatile uint8_t sum = 0;
  for (uint32_t i = 0; i < 64; ++i) sum += static_cast<uint8_t*>(v.Data())[i];
  (void)sum;
}

void Case007() {
  printf("[007] MflushCache scaling sizes\n");
  const int kTests = 8;
  for (int j = 1; j <= kTests; ++j) {
    uint32_t sz = j * 1024 * 1024;
    axsys::CmmBuffer src, dst;
    axsys::CmmView vsrc =
        src.Allocate(sz, axsys::CacheMode::kCached, "cmm_007_src");
    axsys::CmmView vdst =
        dst.Allocate(sz, axsys::CacheMode::kNonCached, "cmm_007_dst");
    memset(vsrc.Data(), 0x78, sz);
    for (int i = 0; i <= 255 && i < static_cast<int>(sz); ++i) {
      static_cast<uint8_t*>(vsrc.Data())[i] = 255 - i;
    }
    vsrc.Flush();
    memcpy(vdst.Data(), vsrc.Data(), sz);
  }
}

void Case008() {
  printf("[008] MinvalidateCache scaling sizes\n");
  const int kTests = 8;
  for (int j = 1; j <= kTests; ++j) {
    uint32_t sz = j * 1024 * 1024;
    axsys::CmmBuffer src, dst;
    axsys::CmmView vsrc =
        src.Allocate(sz, axsys::CacheMode::kNonCached, "cmm_008_src");
    axsys::CmmView vdst =
        dst.Allocate(sz, axsys::CacheMode::kCached, "cmm_008_dst");
    memset(vsrc.Data(), 0xAB, sz);
    memset(vdst.Data(), 0xCD, sz);
    vdst.Invalidate();
    memcpy(vdst.Data(), vsrc.Data(), sz);
  }
}

void Case009() {
  printf("[009] Flush with offset (cached src -> noncached dst)\n");
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 2 * 1024 * 1024;
  axsys::CmmBuffer src, dst;
  axsys::CmmView vsrc =
      src.Allocate(size, axsys::CacheMode::kCached, "cmm_009_src");
  axsys::CmmView vdst =
      dst.Allocate(size, axsys::CacheMode::kNonCached, "cmm_009_dst");
  memset(vsrc.Data(), 0x78, size);
  for (uint32_t i = 0; i <= 255 && i < size; ++i) {
    static_cast<uint8_t*>(vsrc.Data())[i] = 255 - i;
  }
  axsys::CmmView vflush =
      src.MapView(offset, size - offset, axsys::CacheMode::kCached);
  (void)vflush.Flush();
  memcpy(vdst.Data(), vsrc.Data(), size);
  bool ok = true;
  for (uint32_t i = offset; i < size; ++i) {
    if (static_cast<uint8_t*>(vdst.Data())[i] !=
        static_cast<uint8_t*>(vsrc.Data())[i]) {
      printf("  mismatch at %u: dst=0x%x src=0x%x\n", i,
             static_cast<unsigned>(static_cast<uint8_t*>(vdst.Data())[i]),
             static_cast<unsigned>(static_cast<uint8_t*>(vsrc.Data())[i]));
      ok = false;
      break;
    }
  }
  printf("  result: %s\n", ok ? "pass" : "fail");
}

void Case010() {
  printf("[010] Flush with offset (repeat of 009)\n");
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 2 * 1024 * 1024;
  axsys::CmmBuffer src, dst;
  axsys::CmmView vsrc =
      src.Allocate(size, axsys::CacheMode::kCached, "cmm_010_src");
  axsys::CmmView vdst =
      dst.Allocate(size, axsys::CacheMode::kNonCached, "cmm_010_dst");
  memset(vsrc.Data(), 0x78, size);
  for (uint32_t i = 0; i <= 255 && i < size; ++i) {
    static_cast<uint8_t*>(vsrc.Data())[i] = 255 - i;
  }
  axsys::CmmView vflush =
      src.MapView(offset, size - offset, axsys::CacheMode::kCached);
  (void)vflush.Flush();
  memcpy(vdst.Data(), vsrc.Data(), size);
  bool ok = true;
  for (uint32_t i = offset; i < size; ++i) {
    if (static_cast<uint8_t*>(vdst.Data())[i] !=
        static_cast<uint8_t*>(vsrc.Data())[i]) {
      ok = false;
      break;
    }
  }
  printf("  result: %s\n", ok ? "pass" : "fail");
}

void Case011() {
  printf("[011] Flush subrange with offset (expect pass)\n");
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 4;
  axsys::CmmBuffer src, dst;
  axsys::CmmView vsrc =
      src.Allocate(size, axsys::CacheMode::kCached, "cmm_011_src");
  axsys::CmmView vdst =
      dst.Allocate(size, axsys::CacheMode::kNonCached, "cmm_011_dst");
  memset(vsrc.Data(), 0x88, size);
  for (uint32_t i = 0; i <= 255 && i < size; ++i) {
    static_cast<uint8_t*>(vsrc.Data())[i] = 255 - i;
  }
  axsys::CmmView vflush = src.MapView(offset, len, axsys::CacheMode::kCached);
  (void)vflush.Flush();
  memcpy(vdst.Data(), vsrc.Data(), size);
  bool ok = true;
  for (uint32_t i = offset; i < offset + len; ++i) {
    if (static_cast<uint8_t*>(vdst.Data())[i] !=
        static_cast<uint8_t*>(vsrc.Data())[i]) {
      ok = false;
      break;
    }
  }
  printf("  result: %s\n", ok ? "pass" : "fail");
}

void Case012() {
  printf("[012] Flush subrange then compare bigger range (expect fail)\n");
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  const uint32_t len = size / 4;  // flushed length
  const uint32_t cmp = size / 2;  // compare longer
  axsys::CmmBuffer src, dst;
  axsys::CmmView vsrc =
      src.Allocate(size, axsys::CacheMode::kCached, "cmm_012_src");
  axsys::CmmView vdst =
      dst.Allocate(size, axsys::CacheMode::kNonCached, "cmm_012_dst");
  memset(vsrc.Data(), 0x88, size);
  for (uint32_t i = 0; i <= 255 && i < size; ++i) {
    static_cast<uint8_t*>(vsrc.Data())[i] = 255 - i;
  }
  axsys::CmmView vflush = src.MapView(offset, len, axsys::CacheMode::kCached);
  (void)vflush.Flush();
  memcpy(vdst.Data(), vsrc.Data(), size);
  bool ok = true;
  for (uint32_t i = offset; i < offset + cmp; ++i) {
    if (static_cast<uint8_t*>(vdst.Data())[i] !=
        static_cast<uint8_t*>(vsrc.Data())[i]) {
      printf("  expected mismatch at %u: dst=0x%x src=0x%x\n", i,
             static_cast<unsigned>(static_cast<uint8_t*>(vdst.Data())[i]),
             static_cast<unsigned>(static_cast<uint8_t*>(vsrc.Data())[i]));
      ok = false;
      break;
    }
  }
  printf("  result: %s (expected fail)\n", ok ? "pass" : "fail");
}

void Case013() {
  printf("[013] MmapCache + Flush subrange + compare (expect pass)\n");
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  axsys::CmmView base =
      buf.Allocate(size, axsys::CacheMode::kNonCached, "cmm_013_base");
  axsys::CmmView cached = buf.MapView(0, size, axsys::CacheMode::kCached);
  memset(cached.Data(), 0xFE, size);
  axsys::CmmView sub = buf.MapView(offset, size / 2, axsys::CacheMode::kCached);
  (void)sub.Flush();
  bool ok = true;
  for (uint32_t i = offset; i < offset + size / 2; ++i) {
    if (static_cast<uint8_t*>(base.Data())[i] !=
        static_cast<uint8_t*>(cached.Data())[i]) {
      ok = false;
      break;
    }
  }
  printf("  result: %s\n", ok ? "pass" : "fail");
}

void Case014() {
  printf("[014] MmapCache + Flush subrange + compare bigger (expect fail)\n");
  const uint32_t size = 4 * 1024 * 1024;
  const uint32_t offset = 1 * 1024 * 1024;
  axsys::CmmBuffer buf;
  axsys::CmmView base =
      buf.Allocate(size, axsys::CacheMode::kNonCached, "cmm_014_base");
  axsys::CmmView cached = buf.MapView(0, size, axsys::CacheMode::kCached);
  memset(cached.Data(), 0x66, size);
  axsys::CmmView sub = buf.MapView(offset, size / 2, axsys::CacheMode::kCached);
  (void)sub.Flush();
  bool ok = true;
  for (uint32_t i = offset; i < size; ++i) {
    if (static_cast<uint8_t*>(base.Data())[i] !=
        static_cast<uint8_t*>(cached.Data())[i]) {
      printf("  expected mismatch at %u\n", i);
      ok = false;
      break;
    }
  }
  printf("  result: %s (expected fail)\n", ok ? "pass" : "fail");
}

void Case015() {
  printf("[015] External attach + cached/noncached views + Flush\n");
  axsys::CmmBuffer::PartitionInfo part;
  if (!axsys::CmmBuffer::FindAnonymous(&part)) return;
  const uint32_t block_size = 1 * 1024 * 1024;
  const uint64_t phys =
      part.phys + static_cast<uint64_t>(part.size_kb) * 1024 - block_size * 2;
  axsys::CmmBuffer buf;
  if (!buf.AttachExternal(phys, block_size)) return;
  axsys::CmmView nc = buf.MapView(0, block_size, axsys::CacheMode::kNonCached);
  axsys::CmmView c = buf.MapView(0, block_size, axsys::CacheMode::kCached);
  if (!nc.Ok() || !c.Ok()) return;
  memset(nc.Data(), 0xDF, block_size);
  memset(c.Data(), 0xDE, block_size);
  (void)c.Flush();
  bool ok = true;
  for (uint32_t i = 0; i < block_size; ++i) {
    if (static_cast<uint8_t*>(nc.Data())[i] !=
        static_cast<uint8_t*>(c.Data())[i]) {
      ok = false;
      break;
    }
  }
  printf("  result: %s\n", ok ? "pass" : "fail");
}

void Case016() {
  printf("[016] External attach + cached/noncached views + Invalidate\n");
  axsys::CmmBuffer::PartitionInfo part;
  if (!axsys::CmmBuffer::FindAnonymous(&part)) return;
  const uint32_t block_size = 1 * 1024 * 1024;
  const uint64_t phys =
      part.phys + static_cast<uint64_t>(part.size_kb) * 1024 - block_size * 2;
  axsys::CmmBuffer buf;
  if (!buf.AttachExternal(phys, block_size)) return;
  axsys::CmmView nc = buf.MapView(0, block_size, axsys::CacheMode::kNonCached);
  axsys::CmmView c = buf.MapView(0, block_size, axsys::CacheMode::kCached);
  if (!nc.Ok() || !c.Ok()) return;
  memset(nc.Data(), 0xBC, block_size);
  memset(c.Data(), 0xFA, block_size);
  (void)c.Invalidate();
  memset(nc.Data(), 0xBB, block_size);
  bool ok = true;
  for (uint32_t i = 0; i < block_size; ++i) {
    if (static_cast<uint8_t*>(nc.Data())[i] !=
        static_cast<uint8_t*>(c.Data())[i]) {
      ok = false;
      break;
    }
  }
  printf("  result: %s\n", ok ? "pass" : "fail");
}

void Case017() {
  printf("[017] Verify/Dump (cached virt)\n");
  axsys::CmmBuffer buf;
  (void)buf.Allocate(1 * 1024 * 1024, axsys::CacheMode::kCached, "cmm_017");
  bool ok = buf.Verify();
  printf("  verify=%s\n", ok ? "true" : "false");
  buf.Dump();
}

void Case018() {
  printf("[018] Verify/Dump (mapped non-cached addr)\n");
  axsys::CmmBuffer buf;
  (void)buf.Allocate(1 * 1024 * 1024, axsys::CacheMode::kNonCached, "cmm_018");
  bool ok = buf.Verify();
  printf("  verify=%s\n", ok ? "true" : "false");
  buf.Dump();
}

void Case019() {
  printf("[019] Verify/Dump via lib\n");
  axsys::CmmBuffer buf;
  (void)buf.Allocate(1 * 1024 * 1024, axsys::CacheMode::kCached, "cmm_019");
  bool ok = buf.Verify();
  printf("  verify=%s\n", ok ? "true" : "false");
  buf.Dump();
}

void Case020() {
  printf("[020] POOL block + Mmap/MmapCache (short)\n");
  AX_POOL_FLOORPLAN_T plan;
  memset(&plan, 0, sizeof(plan));
  plan.CommPool[0].MetaSize = 0x1000;
  plan.CommPool[0].BlkSize = 3 * 1024 * 1024;
  plan.CommPool[0].BlkCnt = 1;
  plan.CommPool[0].CacheMode = AX_POOL_CACHE_MODE_NONCACHE;
  snprintf(reinterpret_cast<char*>(plan.CommPool[0].PartitionName),
           sizeof(plan.CommPool[0].PartitionName), "%s", "anonymous");
  AX_POOL_Exit();
  if (AX_POOL_SetConfig(&plan) != 0) return;
  if (AX_POOL_Init() != 0) return;
  AX_BLK blk =
      AX_POOL_GetBlock(AX_INVALID_POOLID, plan.CommPool[0].BlkSize, nullptr);
  if (blk == AX_INVALID_BLOCKID) return;
  AX_U64 phys = AX_POOL_Handle2PhysAddr(blk);
  void* map_nc = AX_SYS_Mmap(phys, plan.CommPool[0].BlkSize);
  if (map_nc) AX_SYS_Munmap(map_nc, plan.CommPool[0].BlkSize);
  void* map_c = AX_SYS_MmapCache(phys, plan.CommPool[0].BlkSize);
  if (map_c) {
    AX_SYS_MflushCache(phys, map_c, plan.CommPool[0].BlkSize);
    AX_SYS_MinvalidateCache(phys, map_c, plan.CommPool[0].BlkSize);
    AX_SYS_Munmap(map_c, plan.CommPool[0].BlkSize);
  }
  AX_POOL_ReleaseBlock(blk);
  AX_POOL_Exit();
}

void Case021() {
  printf("[021] MmapFast address consistency\n");
  axsys::CmmBuffer buf;
  (void)buf.Allocate(kLen, axsys::CacheMode::kNonCached, "cmm_021");
  axsys::CmmView v1 = buf.MapViewFast(0, kLen, axsys::CacheMode::kNonCached);
  axsys::CmmView v2 = buf.MapViewFast(0, kLen, axsys::CacheMode::kNonCached);
  printf("  map1=%p map2=%p%s\n", v1.Data(), v2.Data(),
         (v1.Data() == v2.Data() ? " (same)" : " (diff)"));
}

void Case022() {
  printf("[022] MmapCacheFast address consistency\n");
  axsys::CmmBuffer buf;
  (void)buf.Allocate(kLen, axsys::CacheMode::kNonCached, "cmm_022");
  axsys::CmmView v1 = buf.MapViewFast(0, kLen, axsys::CacheMode::kCached);
  axsys::CmmView v2 = buf.MapViewFast(0, kLen, axsys::CacheMode::kCached);
  printf("  map1=%p map2=%p%s\n", v1.Data(), v2.Data(),
         (v1.Data() == v2.Data() ? " (same)" : " (diff)"));
}

void Case023() {
  printf("[023] MmapCacheFast + MflushCache + Munmap\n");
  axsys::CmmBuffer buf;
  (void)buf.Allocate(kLen, axsys::CacheMode::kNonCached, "cmm_023");
  axsys::CmmView v = buf.MapViewFast(0, kLen, axsys::CacheMode::kCached);
  if (!v.Ok()) return;
  memset(v.Data(), 0xFA, kLen);
  v.Flush();
  // no explicit Free
}

void Case024() {
  printf("[024] MmapCacheFast + MinvalidateCache + Munmap\n");
  axsys::CmmBuffer buf;
  (void)buf.Allocate(kLen, axsys::CacheMode::kNonCached, "cmm_024");
  axsys::CmmView v = buf.MapViewFast(0, kLen, axsys::CacheMode::kCached);
  if (!v.Ok()) return;
  v.Invalidate();
  buf.Free();
}

}  // namespace

int main() {
  SystemGuard sys;
  if (!sys.Ok()) return -1;
  printf("sample_cmm (C++) begin\n\n");

  Case001();
  Case002();
  Case003();
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

  printf("\nsample_cmm (C++) end\n");
  return 0;
}
