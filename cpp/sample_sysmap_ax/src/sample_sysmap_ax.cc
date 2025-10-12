#include <ax_sys_api.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>

namespace {
constexpr uint32_t kTestLen = 0x1200000;  // 18 MiB

void PrintElapsed(const timeval& start, const timeval& end,
                  uint64_t bytes_per_copy, int copies) {
  int64_t sec = static_cast<int64_t>(end.tv_sec - start.tv_sec);
  int64_t usec = (end.tv_sec - start.tv_sec)
                     ? (end.tv_usec - start.tv_usec + 1000000)
                     : (end.tv_usec - start.tv_usec);
  const double total_sec =
      static_cast<double>(sec) + static_cast<double>(usec) / 1e6;
  const double per_copy_sec = (copies > 0) ? (total_sec / copies) : total_sec;
  const double mib = static_cast<double>(bytes_per_copy) / (1024.0 * 1024.0);
  printf("data size: %" PRIu64 " bytes (%.2f MiB)\n",
         static_cast<uint64_t>(bytes_per_copy), mib);
  printf("time: %.6f sec for %d copies\n", total_sec, copies);
  printf("      %.6f sec per %" PRIu64 " bytes (%.2f MiB)\n", per_copy_sec,
         static_cast<uint64_t>(bytes_per_copy), mib);
}

int DoTestUncached(AX_U64 phys_src, void* virt_src, AX_U64 phys_dst,
                   void* virt_dst) {
  // Optional: query block info for demonstration.
  AX_S32 mem_type_src = 0, mem_type_dst = 0;
  void* back_virt_src = nullptr;
  void* back_virt_dst = nullptr;
  AX_U32 blk_size_src = 0, blk_size_dst = 0;
  AX_SYS_MemGetBlockInfoByPhy(phys_src, &mem_type_src, &back_virt_src,
                              &blk_size_src);
  AX_SYS_MemGetBlockInfoByPhy(phys_dst, &mem_type_dst, &back_virt_dst,
                              &blk_size_dst);

  // Sanity copy + verify like the original sample.
  for (int i = 0; i < 0x20; ++i) {
    memcpy(static_cast<char*>(virt_src) + i, static_cast<char*>(virt_dst) + i,
           kTestLen - i);
    if (memcmp(static_cast<char*>(virt_src) + i,
               static_cast<char*>(virt_dst) + i, kTestLen - i) != 0) {
      printf("memcpy fail, i: %x\n", i);
    }
  }

  // Measure memcpy throughput.
  constexpr int kCopies = 50;
  timeval start{}, end{};
  gettimeofday(&start, nullptr);
  for (int i = 0; i < kCopies; ++i) {
    memcpy(virt_src, virt_dst, kTestLen);
  }
  gettimeofday(&end, nullptr);
  PrintElapsed(start, end, kTestLen, kCopies);

  return 0;
}

int DoTestCached(AX_U64 phys_src, void* virt_src, AX_U64 phys_dst,
                 void* virt_dst) {
  // Optional: ensure cache lines are in a known state.
  AX_SYS_MinvalidateCache(phys_src, virt_src, kTestLen);
  AX_SYS_MinvalidateCache(phys_dst, virt_dst, kTestLen);

  // Sanity copy + verify like the original sample.
  for (int i = 0; i < 0x20; ++i) {
    memcpy(static_cast<char*>(virt_src) + i, static_cast<char*>(virt_dst) + i,
           kTestLen - i);
    if (memcmp(static_cast<char*>(virt_src) + i,
               static_cast<char*>(virt_dst) + i, kTestLen - i) != 0) {
      printf("memcpy fail, i: %x\n", i);
    }
  }

  constexpr int kCopies = 50;
  timeval start{}, end{};
  gettimeofday(&start, nullptr);
  for (int i = 0; i < kCopies; ++i) {
    memcpy(virt_src, virt_dst, kTestLen);
  }
  gettimeofday(&end, nullptr);

  // Optional: flush after writes to push data out of CPU cache (not timed).
  AX_SYS_MflushCache(phys_src, virt_src, kTestLen);
  AX_SYS_MflushCache(phys_dst, virt_dst, kTestLen);

  PrintElapsed(start, end, kTestLen, kCopies);
  return 0;
}
}  // namespace

int main() {
  if (AX_SYS_Init() != 0) {
    printf("AX_SYS_Init failed\n");
    return -1;
  }

  // Uncached pair
  AX_U64 phys_src_nc = 0, phys_dst_nc = 0;
  void* virt_src_nc = nullptr;
  void* virt_dst_nc = nullptr;
  if (AX_SYS_MemAlloc(&phys_src_nc, &virt_src_nc, kTestLen, 0x4,
                      (const AX_S8*)"ax_sysmap_ax_nc") < 0) {
    printf("alloc src (non-cached) failed\n");
    return -1;
  }
  if (AX_SYS_MemAlloc(&phys_dst_nc, &virt_dst_nc, kTestLen, 0x4,
                      (const AX_S8*)"ax_sysmap_ax_nc") < 0) {
    printf("alloc dst (non-cached) failed\n");
    AX_SYS_MemFree(phys_src_nc, virt_src_nc);
    return -1;
  }

  // Cached pair
  AX_U64 phys_src_c = 0, phys_dst_c = 0;
  void* virt_src_c = nullptr;
  void* virt_dst_c = nullptr;
  if (AX_SYS_MemAllocCached(&phys_src_c, &virt_src_c, kTestLen, 0x4,
                            (const AX_S8*)"ax_sysmap_ax_c") < 0) {
    printf("alloc src (cached) failed\n");
    AX_SYS_MemFree(phys_src_nc, virt_src_nc);
    AX_SYS_MemFree(phys_dst_nc, virt_dst_nc);
    return -1;
  }
  if (AX_SYS_MemAllocCached(&phys_dst_c, &virt_dst_c, kTestLen, 0x4,
                            (const AX_S8*)"ax_sysmap_ax_c") < 0) {
    printf("alloc dst (cached) failed\n");
    AX_SYS_MemFree(phys_src_nc, virt_src_nc);
    AX_SYS_MemFree(phys_dst_nc, virt_dst_nc);
    AX_SYS_MemFree(phys_src_c, virt_src_c);
    return -1;
  }

  printf("malloc phy addr (uncached): %" PRIx64 ", %" PRIx64 "\n",
         static_cast<uint64_t>(phys_src_nc),
         static_cast<uint64_t>(phys_dst_nc));
  printf("malloc phy addr (cached):   %" PRIx64 ", %" PRIx64 "\n",
         static_cast<uint64_t>(phys_src_c), static_cast<uint64_t>(phys_dst_c));

  printf("Test uncached\n");
  DoTestUncached(phys_src_nc, virt_src_nc, phys_dst_nc, virt_dst_nc);

  printf("Test cached\n");
  DoTestCached(phys_src_c, virt_src_c, phys_dst_c, virt_dst_c);

  AX_SYS_MemFree(phys_src_nc, virt_src_nc);
  AX_SYS_MemFree(phys_dst_nc, virt_dst_nc);
  AX_SYS_MemFree(phys_src_c, virt_src_c);
  AX_SYS_MemFree(phys_dst_c, virt_dst_c);

  printf("sysmap_ax test pass\n");
  return 0;
}
