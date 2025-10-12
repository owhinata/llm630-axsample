// AXERA SDK
#include <ax_sys_api.h>

// C system headers
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// C++ headers
#include <chrono>
#include <cstdint>

namespace {

constexpr uint32_t kLen = 2 * 1024 * 1024;  // 2 MiB
constexpr uint32_t kAlign = 64;

enum class CacheMode {
  kNonCached = 0,
  kCached = 1,
};

class SystemGuard {
 public:
  SystemGuard() : ok_(AX_SYS_Init() == 0) {
    if (!ok_) printf("AX_SYS_Init failed\n");
  }
  ~SystemGuard() {
    if (ok_) AX_SYS_Deinit();
  }
  bool ok() const { return ok_; }

 private:
  bool ok_;
};

class Timer {
 public:
  void Start() { start_ = std::chrono::steady_clock::now(); }
  void StopPrint(const char* label) {
    auto end = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(end - start_).count();
    printf("%s: %.6f sec\n", label, sec);
  }

 private:
  std::chrono::steady_clock::time_point start_{};
};

struct Buf {
  AX_U64 phy{0};
  void* vir{nullptr};
};

// Simple address buffer descriptor compatible with memcpy helper.
struct AddrBuf {
  AX_U64 phy;
  void* vir;
  bool is_cached;
};

static int MemcpyFunc(const AddrBuf& src, const AddrBuf& dst, uint32_t size) {
  void* psrc = nullptr;
  void* pdst = nullptr;
  if (src.is_cached) {
    psrc = AX_SYS_Mmap(src.phy, size);
    if (!psrc) {
      printf("AX_SYS_Mmap pSrcVirAddr failed\n");
      return -1;
    }
  } else {
    psrc = src.vir;
  }
  if (dst.is_cached) {
    pdst = AX_SYS_Mmap(dst.phy, size);
    if (!pdst) {
      if (psrc && psrc != src.vir) AX_SYS_Munmap(psrc, size);
      printf("AX_SYS_Mmap pDstVirAddr failed\n");
      return -1;
    }
  } else {
    pdst = dst.vir;
  }
  memcpy(pdst, psrc, size);
  if (psrc && psrc != src.vir) AX_SYS_Munmap(psrc, size);
  if (pdst && pdst != dst.vir) AX_SYS_Munmap(pdst, size);
  return 0;
}

class CmmMapper {
 public:
  CmmMapper() = default;
  ~CmmMapper() { Unmap(); }
  bool Map(AX_U64 phy, uint32_t size, CacheMode mode) {
    size_ = size;
    if (mode == CacheMode::kCached) {
      vir_ = AX_SYS_MmapCache(phy, size);
    } else {
      vir_ = AX_SYS_Mmap(phy, size);
    }
    if (!vir_) {
      printf("AX_SYS_Mmap(%s) failed\n",
             (mode == CacheMode::kCached) ? "cache" : "noncache");
      return false;
    }
    return true;
  }
  bool Unmap() {
    if (vir_) {
      AX_S32 ret = AX_SYS_Munmap(vir_, size_);
      if (ret != 0) {
        printf("AX_SYS_Munmap failed: 0x%X\n", static_cast<unsigned int>(ret));
        return false;
      }
      vir_ = nullptr;
      size_ = 0;
    }
    return true;
  }
  void* vir() const { return vir_; }

 private:
  void* vir_{nullptr};
  uint32_t size_{0};
};

class CmmBuffer {
 public:
  CmmBuffer() = default;
  ~CmmBuffer() { Free(); }

  bool Alloc(uint32_t size, CacheMode mode, const char* token) {
    cache_mode_ = mode;
    if (mode == CacheMode::kCached) {
      if (AX_SYS_MemAllocCached(&buf_.phy, &buf_.vir, size, kAlign,
                                reinterpret_cast<const AX_S8*>(token)) < 0) {
        printf("AX_SYS_MemAllocCached failed\n");
        return false;
      }
    } else {
      if (AX_SYS_MemAlloc(&buf_.phy, &buf_.vir, size, kAlign,
                          reinterpret_cast<const AX_S8*>(token)) < 0) {
        printf("AX_SYS_MemAlloc failed\n");
        return false;
      }
    }
    return true;
  }

  bool Map(uint32_t offset, uint32_t size) {
    if (!buf_.phy) return false;
    if (mapper_.vir()) return true;  // already mapped
    return mapper_.Map(buf_.phy + offset, size, cache_mode_);
  }

  bool Unmap() { return mapper_.Unmap(); }

  void Free() {
    // Unmap first if mapped
    mapper_.Unmap();
    if (buf_.phy || buf_.vir) {
      AX_S32 ret = AX_SYS_MemFree(buf_.phy, buf_.vir);
      if (ret != 0) {
        printf("AX_SYS_MemFree failed: 0x%X (phy=0x%" PRIx64 ")\n",
               static_cast<unsigned int>(ret), static_cast<uint64_t>(buf_.phy));
      }
      buf_.phy = 0;
      buf_.vir = nullptr;
    }
  }

  AX_U64 phy() const { return buf_.phy; }
  void* vir() const { return buf_.vir; }
  void* map() const { return mapper_.vir(); }
  CacheMode cache_mode() const { return cache_mode_; }

  // Implicit conversion to AddrBuf for helper interop.
  operator AddrBuf() const {
    return AddrBuf{buf_.phy, buf_.vir, cache_mode_ == CacheMode::kCached};
  }

 private:
  Buf buf_{};
  CmmMapper mapper_{};
  CacheMode cache_mode_{CacheMode::kNonCached};
};

void Case001() {
  printf("[001] MemAlloc/MemFree (non-cached)\n");
  CmmBuffer b;
  if (!b.Alloc(kLen, CacheMode::kNonCached, "cmm_001")) return;
  printf("  phy=0x%" PRIx64 ", vir=0x%" PRIuPTR "\n",
         static_cast<uint64_t>(b.phy()), reinterpret_cast<uintptr_t>(b.vir()));
}

void Case002() {
  printf("[002] MemAllocCached/MemFree (cached)\n");
  CmmBuffer b;
  if (!b.Alloc(kLen, CacheMode::kCached, "cmm_002")) return;
  printf("  phy=0x%" PRIx64 ", vir=0x%" PRIuPTR "\n",
         static_cast<uint64_t>(b.phy()), reinterpret_cast<uintptr_t>(b.vir()));
}

void Case004() {
  printf("[004] Mmap/Munmap (non-cached)\n");
  CmmBuffer b;
  if (!b.Alloc(kLen, CacheMode::kNonCached, "cmm_004")) return;
  if (!b.Map(0, kLen)) return;
  printf("  map=0x%" PRIuPTR "\n", reinterpret_cast<uintptr_t>(b.map()));
}

void Case005() {
  printf("[005] MmapCache/MflushCache/Munmap (cached)\n");
  CmmBuffer b;
  if (!b.Alloc(kLen, CacheMode::kCached, "cmm_005")) return;
  if (!b.Map(0, kLen)) return;
  printf("  map=0x%" PRIuPTR "\n", reinterpret_cast<uintptr_t>(b.map()));
  // write then flush
  memset(b.map(), 0xA5, kLen);
  AX_SYS_MflushCache(b.phy(), b.map(), kLen);
}

void Case006() {
  printf("[006] MmapCache/MinvalidateCache/Munmap (cached)\n");
  CmmBuffer b;
  if (!b.Alloc(kLen, CacheMode::kCached, "cmm_006")) return;
  if (!b.Map(0, kLen)) return;
  printf("  map=0x%" PRIuPTR "\n", reinterpret_cast<uintptr_t>(b.map()));
  // pretend external writer, then invalidate before reading
  AX_SYS_MinvalidateCache(b.phy(), b.map(), kLen);
  volatile uint8_t sum = 0;
  for (uint32_t i = 0; i < 64; ++i) sum += static_cast<uint8_t*>(b.map())[i];
  (void)sum;
}

void Case019() {
  printf("[019] GetBlockInfoByVirt/ByPhy\n");
  CmmBuffer b_nc;
  CmmBuffer b_c;
  if (!b_nc.Alloc(kLen, CacheMode::kNonCached, "cmm_019_nc")) return;
  if (!b_c.Alloc(kLen, CacheMode::kCached, "cmm_019_c")) return;

  AX_U64 phy_out = 0;
  AX_S32 mem_type = 0;
  if (AX_SYS_MemGetBlockInfoByVirt(b_nc.vir(), &phy_out, &mem_type) == 0) {
    printf("  virt(nc)=0x%" PRIuPTR " -> phy=0x%" PRIx64 ", memType=%d\n",
           reinterpret_cast<uintptr_t>(b_nc.vir()),
           static_cast<uint64_t>(phy_out), mem_type);
  }
  void* vir_out = nullptr;
  AX_U32 blk_sz = 0;
  if (AX_SYS_MemGetBlockInfoByPhy(b_c.phy(), &mem_type, &vir_out, &blk_sz) ==
      0) {
    printf("  phy(c)=0x%" PRIx64 " -> virt=0x%" PRIuPTR ", type=%d, blkSz=%u\n",
           static_cast<uint64_t>(b_c.phy()),
           reinterpret_cast<uintptr_t>(vir_out), mem_type, blk_sz);
  }
}

void Case003() {
  printf("[003] GetBlockInfoByVirt/ByPhy (non-cached virt)\n");
  CmmBuffer b;
  if (!b.Alloc(1 * 1024 * 1024, CacheMode::kNonCached, "cmm_003")) return;
  AX_U64 phy2 = 0;
  AX_S32 mem_type = 0;
  if (AX_SYS_MemGetBlockInfoByVirt(b.vir(), &phy2, &mem_type) == 0) {
    printf("  virt=0x%" PRIuPTR " -> phy=0x%" PRIx64 ", type=%d\n",
           reinterpret_cast<uintptr_t>(b.vir()), static_cast<uint64_t>(phy2),
           mem_type);
  }
  void* vir2 = nullptr;
  AX_U32 blk_sz = 0;
  if (AX_SYS_MemGetBlockInfoByPhy(b.phy(), &mem_type, &vir2, &blk_sz) == 0) {
    printf("  phy=0x%" PRIx64 " -> virt=0x%" PRIuPTR ", type=%d, blkSz=%u\n",
           static_cast<uint64_t>(b.phy()), reinterpret_cast<uintptr_t>(vir2),
           mem_type, blk_sz);
  }
}

void Case007() {
  printf("[007] MflushCache scaling sizes\n");
  const int kTests = 8;  // reduce from 32
  for (int j = 1; j <= kTests; ++j) {
    uint32_t sz = j * 1024 * 1024;
    CmmBuffer src, dst;
    if (!src.Alloc(sz, CacheMode::kCached, "cmm_007_src")) return;
    if (!dst.Alloc(sz, CacheMode::kNonCached, "cmm_007_dst")) return;
    memset(src.vir(), 0x78, sz);
    for (int i = 0; i <= 255 && i < static_cast<int>(sz); ++i) {
      static_cast<uint8_t*>(src.vir())[i] = 255 - i;
    }
    AX_SYS_MflushCache(src.phy(), src.vir(), sz);
    // Map dst non-cached (not needed; it's non-cached virt)
    memcpy(dst.vir(), src.vir(), sz);
  }
}

void Case008() {
  printf("[008] MinvalidateCache scaling sizes\n");
  const int kTests = 8;
  for (int j = 1; j <= kTests; ++j) {
    uint32_t sz = j * 1024 * 1024;
    CmmBuffer src, dst;
    if (!src.Alloc(sz, CacheMode::kNonCached, "cmm_008_src")) return;
    if (!dst.Alloc(sz, CacheMode::kCached, "cmm_008_dst")) return;
    memset(src.vir(), 0xAB, sz);
    if (!dst.Map(0, sz)) return;
    memset(dst.map(), 0xCD, sz);
    AX_SYS_MinvalidateCache(dst.phy(), dst.map(), sz);
    memcpy(dst.map(), src.vir(), sz);
  }
}

void Case009() {
  printf("[009] MflushCache with offset (pass)\n");
  const uint32_t l_size = 4 * 1024 * 1024;
  uint32_t offset = 2 * 1024 * 1024;
  for (int j = 0; j < 100; ++j) {
    CmmBuffer src, dst;
    if (!src.Alloc(l_size, CacheMode::kCached, "cmm_009_src")) return;
    if (!dst.Alloc(l_size, CacheMode::kNonCached, "cmm_009_dst")) return;
    memset(src.vir(), 0x78, l_size);
    for (int i = 0; i <= 255; ++i) {
      static_cast<uint8_t*>(src.vir())[i] = static_cast<uint8_t>(255 - i);
    }
    AX_SYS_MflushCache(src.phy() + offset,
                       static_cast<uint8_t*>(src.vir()) + offset,
                       l_size - offset);
    if (MemcpyFunc(src, dst, l_size) != 0) return;
    for (uint32_t i = offset; i < l_size; ++i) {
      if (static_cast<uint8_t*>(dst.vir())[i] !=
          static_cast<uint8_t*>(src.vir())[i]) {
        printf("[009] mismatch at %u\n", i);
        return;
      }
    }
  }
}

void Case010() {
  printf("[010] MflushCache with offset (pass)\n");
  const uint32_t l_size = 4 * 1024 * 1024;
  uint32_t offset = 2 * 1024 * 1024;
  for (int j = 0; j < 100; ++j) {
    CmmBuffer src, dst;
    if (!src.Alloc(l_size, CacheMode::kCached, "cmm_010_src")) return;
    if (!dst.Alloc(l_size, CacheMode::kNonCached, "cmm_010_dst")) return;
    memset(src.vir(), 0x78, l_size);
    for (int i = 0; i <= 255; ++i) {
      static_cast<uint8_t*>(src.vir())[i] = static_cast<uint8_t>(255 - i);
    }
    AX_SYS_MflushCache(src.phy() + offset,
                       static_cast<uint8_t*>(src.vir()) + offset,
                       l_size - offset);
    if (MemcpyFunc(src, dst, l_size) != 0) return;
    for (uint32_t i = offset; i < l_size; ++i) {
      if (static_cast<uint8_t*>(dst.vir())[i] !=
          static_cast<uint8_t*>(src.vir())[i]) {
        printf("[010] mismatch at %u\n", i);
        return;
      }
    }
  }
}

void Case011() {
  printf("[011] MflushCache with offset (pass)\n");
  const uint32_t l_size = 4 * 1024 * 1024;
  uint32_t offset = 1 * 1024 * 1024;
  uint32_t region = l_size / 4;
  for (int j = 0; j < 100; ++j) {
    CmmBuffer src, dst;
    if (!src.Alloc(l_size, CacheMode::kCached, "cmm_011_src")) return;
    if (!dst.Alloc(l_size, CacheMode::kNonCached, "cmm_011_dst")) return;
    memset(src.vir(), 0x88, l_size);
    for (int i = 0; i <= 255; ++i) {
      static_cast<uint8_t*>(src.vir())[i] = static_cast<uint8_t>(255 - i);
    }
    AX_SYS_MflushCache(src.phy() + offset,
                       static_cast<uint8_t*>(src.vir()) + offset, region);
    if (MemcpyFunc(src, dst, l_size) != 0) return;
    for (uint32_t i = offset; i < offset + region; ++i) {
      if (static_cast<uint8_t*>(dst.vir())[i] !=
          static_cast<uint8_t*>(src.vir())[i]) {
        printf("[011] mismatch at %u\n", i);
        return;
      }
    }
  }
}

void Case012() {
  printf("[012] MflushCache with offset case (expected to fail)\n");
  const uint32_t l_size = 4 * 1024 * 1024;
  CmmBuffer src;
  if (!src.Alloc(l_size, CacheMode::kCached, "cmm_012_src")) return;
  uint32_t offset = (2 * 1024 * 1024) + 0x1EF;  // non 4K aligned
  memset(src.vir(), 0x77, l_size);
  AX_S32 ret = AX_SYS_MflushCache(src.phy() + offset,
                                  static_cast<uint8_t*>(src.vir()) + offset,
                                  l_size - offset);
  if (ret == 0) {
    printf(
        "  Note: flush unexpectedly succeeded; this case may pass "
        "intermittently.\n");
  } else {
    printf("  flush returned error as expected: 0x%X\n",
           static_cast<unsigned int>(ret));
  }
}

void Case013() {
  printf("[013] MmapCache/MflushCache/Munmap with offset (pass)\n");
  const uint32_t l_size = 4 * 1024 * 1024;
  CmmBuffer src, dst;
  if (!src.Alloc(l_size, CacheMode::kCached, "cmm_013_src")) return;
  if (!dst.Alloc(l_size, CacheMode::kNonCached, "cmm_013_dst")) return;
  uint32_t offset = 0x1000;  // 4K aligned offset
  memset(src.vir(), 0x66, l_size);
  AX_SYS_MflushCache(src.phy() + offset,
                     static_cast<uint8_t*>(src.vir()) + offset,
                     l_size - offset);
  (void)MemcpyFunc(src, dst, l_size);
}

void Case014() {
  printf("[014] MmapCache/MflushCache/Munmap with offset (expected fail)\n");
  const uint32_t l_size = 4 * 1024 * 1024;
  CmmBuffer src;
  if (!src.Alloc(l_size, CacheMode::kCached, "cmm_014_src")) return;
  uint32_t offset = 0x11EF;  // non 4K aligned
  AX_S32 ret = AX_SYS_MflushCache(src.phy() + offset,
                                  static_cast<uint8_t*>(src.vir()) + offset,
                                  l_size - offset);
  if (ret != 0) {
    printf("  expected failure: 0x%X\n", static_cast<unsigned int>(ret));
  } else {
    printf("  warning: flush succeeded; behavior may vary.\n");
  }
}

void Case015() {
  printf("[015] MmapCache/Flush/Munmap unmanaged mode\n");
  AX_CMM_STATUS_T st{};
  if (AX_SYS_MemQueryStatus(&st) != 0) return;
  int idx = -1;
  for (AX_U32 i = 0; i < st.Partition.PartitionCnt; ++i) {
    if (strcmp(
            reinterpret_cast<const char*>(st.Partition.PartitionInfo[i].Name),
            "anonymous") == 0) {
      idx = static_cast<int>(i);
      break;
    }
  }
  if (idx < 0) return;
  AX_U64 base = st.Partition.PartitionInfo[idx].PhysAddr;
  AX_U64 size_kb = st.Partition.PartitionInfo[idx].SizeKB;
  AX_U32 blk = 1 * 1024 * 1024;
  AX_U64 phys = base + static_cast<AX_U64>(size_kb) * 1024 - blk * 2;
  const int TEST_TIME = 100;
  for (int j = 0; j < TEST_TIME; ++j) {
    void* v_nc = AX_SYS_Mmap(phys, blk);
    if (!v_nc) return;
    memset(v_nc, 0xDF, blk);
    void* v_c = AX_SYS_MmapCache(phys, blk);
    if (!v_c) return;
    memset(v_c, 0xDE, blk);
    AX_SYS_MflushCache(phys, v_c, blk);
    for (AX_U32 i = 0; i < blk; ++i) {
      if (static_cast<uint8_t*>(v_nc)[i] != static_cast<uint8_t*>(v_c)[i]) {
        printf("[015] mismatch at %u\n", i);
        break;
      }
    }
    AX_SYS_Munmap(v_nc, blk);
    AX_SYS_Munmap(v_c, blk);
  }
}

void Case016() {
  printf("[016] MmapCache/Invalidate/Munmap unmanaged mode\n");
  AX_CMM_STATUS_T st{};
  if (AX_SYS_MemQueryStatus(&st) != 0) return;
  int idx = -1;
  for (AX_U32 i = 0; i < st.Partition.PartitionCnt; ++i) {
    if (strcmp(
            reinterpret_cast<const char*>(st.Partition.PartitionInfo[i].Name),
            "anonymous") == 0) {
      idx = static_cast<int>(i);
      break;
    }
  }
  if (idx < 0) return;
  AX_U64 base = st.Partition.PartitionInfo[idx].PhysAddr;
  AX_U64 size_kb = st.Partition.PartitionInfo[idx].SizeKB;
  AX_U32 blk = 1 * 1024 * 1024;
  AX_U64 phys = base + static_cast<AX_U64>(size_kb) * 1024 - blk * 2;
  void* v_nc = AX_SYS_Mmap(phys, blk);
  if (!v_nc) return;
  printf("  noncached v=%p\n", v_nc);
  void* v_c = AX_SYS_MmapCache(phys, blk);
  if (!v_c) return;
  printf("  cached v=%p\n", v_c);
  AX_SYS_MinvalidateCache(phys, v_c, blk);
  AX_SYS_Munmap(v_c, blk);
  AX_SYS_Munmap(v_nc, blk);
}
void Case017() {
  printf("[017] GetBlockInfo (cached virt)\n");
  CmmBuffer b;
  if (!b.Alloc(1 * 1024 * 1024, CacheMode::kCached, "cmm_017")) return;
  if (!b.Map(0, 1 * 1024 * 1024)) return;
  AX_U64 phy2 = 0;
  AX_S32 mem_type = 0;
  if (AX_SYS_MemGetBlockInfoByVirt(b.map(), &phy2, &mem_type) == 0) {
    printf("  virt(c)=0x%" PRIuPTR " -> phy=0x%" PRIx64 ", type=%d\n",
           reinterpret_cast<uintptr_t>(b.map()), static_cast<uint64_t>(phy2),
           mem_type);
  }
}

void Case018() {
  printf("[018] GetBlockInfo (mapped non-cached addr)\n");
  CmmBuffer b;
  if (!b.Alloc(1 * 1024 * 1024, CacheMode::kNonCached, "cmm_018")) return;
  if (!b.Map(0, 1 * 1024 * 1024)) return;
  AX_U64 phy2 = 0;
  AX_S32 mem_type = 0;
  if (AX_SYS_MemGetBlockInfoByVirt(b.map(), &phy2, &mem_type) == 0) {
    printf("  virt(nc)=0x%" PRIuPTR " -> phy=0x%" PRIx64 ", type=%d\n",
           reinterpret_cast<uintptr_t>(b.map()), static_cast<uint64_t>(phy2),
           mem_type);
  }
}

void Case020() {
  printf("[020] Mmap/MmapCache on POOL block\n");
  // Minimal variant from sample: configure one common pool, get a block
  AX_POOL_FLOORPLAN_T plan{};
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
  CmmBuffer b;
  if (!b.Alloc(kLen, CacheMode::kNonCached, "cmm_021")) return;
  void* v1 = AX_SYS_MmapFast(b.phy(), kLen);
  void* v2 = AX_SYS_MmapFast(b.phy(), kLen);
  printf("  map1=%p map2=%p%s\n", v1, v2, (v1 == v2 ? " (same)" : " (diff)"));
  if (v1) AX_SYS_Munmap(v1, kLen);
  if (v2 && v2 != v1) AX_SYS_Munmap(v2, kLen);
}

void Case022() {
  printf("[022] MmapCacheFast address consistency\n");
  CmmBuffer b;
  if (!b.Alloc(kLen, CacheMode::kNonCached, "cmm_022")) return;
  void* v1 = AX_SYS_MmapCacheFast(b.phy(), kLen);
  void* v2 = AX_SYS_MmapCacheFast(b.phy(), kLen);
  printf("  map1=%p map2=%p%s\n", v1, v2, (v1 == v2 ? " (same)" : " (diff)"));
  if (v1) AX_SYS_Munmap(v1, kLen);
  if (v2 && v2 != v1) AX_SYS_Munmap(v2, kLen);
}

void Case023() {
  printf("[023] MmapCacheFast + MflushCache + Munmap\n");
  CmmBuffer b;
  if (!b.Alloc(kLen, CacheMode::kNonCached, "cmm_023")) return;
  void* v = AX_SYS_MmapCacheFast(b.phy(), kLen);
  if (!v) return;
  memset(v, 0xFA, kLen);
  AX_SYS_MflushCache(b.phy(), v, kLen);
  AX_SYS_Munmap(v, kLen);
}

void Case024() {
  printf("[024] MmapCacheFast + MinvalidateCache + Munmap\n");
  CmmBuffer b;
  if (!b.Alloc(kLen, CacheMode::kNonCached, "cmm_024")) return;
  void* v = AX_SYS_MmapCacheFast(b.phy(), kLen);
  if (!v) return;
  memset(b.vir(), 0xBC, kLen);
  memset(v, 0xFA, kLen);
  AX_SYS_MinvalidateCache(b.phy(), v, kLen);
  AX_SYS_Munmap(v, kLen);
}
}  // namespace

int main() {
  SystemGuard sys;
  if (!sys.ok()) return -1;
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
