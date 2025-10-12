#include <ax_sys_api.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
constexpr const char* kSysmapDev = "/dev/ax_sysmap";
constexpr unsigned int kTestLen = 0x1200000;  // 18 MiB

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

int DoSysmapTest(bool cached, AX_U64 phys_src, AX_U64 phys_dst) {
  int flags = O_RDWR | (cached ? 0 : O_SYNC);
  int fd = open(kSysmapDev, flags);
  if (fd == -1) {
    printf("open %s fail!\n", kSysmapDev);
    return -1;
  }

  char* map_src = reinterpret_cast<char*>(mmap(
      nullptr, kTestLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys_src));
  char* map_dst = reinterpret_cast<char*>(mmap(
      nullptr, kTestLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys_dst));

  if (map_src == MAP_FAILED || map_dst == MAP_FAILED) {
    printf("map fail, %" PRIuPTR ", %" PRIuPTR "\n",
           reinterpret_cast<uintptr_t>(map_src),
           reinterpret_cast<uintptr_t>(map_dst));
    if (map_src && map_src != MAP_FAILED) munmap(map_src, kTestLen);
    if (map_dst && map_dst != MAP_FAILED) munmap(map_dst, kTestLen);
    close(fd);
    return -1;
  }

  for (int i = 0; i < 0x20; ++i) {
    memcpy(map_src + i, map_dst + i, kTestLen - i);
    if (memcmp(map_src + i, map_dst + i, kTestLen - i) != 0) {
      printf("memcpy fail, i: %x\n", i);
    }
  }

  timeval start{}, end{};
  gettimeofday(&start, nullptr);
  constexpr int kCopies = 50;
  for (int i = 0; i < kCopies; ++i) {
    memcpy(map_src, map_dst, kTestLen);
  }
  gettimeofday(&end, nullptr);
  PrintElapsed(start, end, kTestLen, kCopies);

  munmap(map_src, kTestLen);
  munmap(map_dst, kTestLen);
  close(fd);
  return 0;
}
}  // namespace

int main() {
  if (AX_SYS_Init() != 0) {
    printf("AX_SYS_Init failed\n");
    return -1;
  }

  AX_U64 phys_src = 0;
  AX_U64 phys_dst = 0;
  void* virt_src = nullptr;
  void* virt_dst = nullptr;

  if (AX_SYS_MemAlloc(&phys_src, &virt_src, kTestLen, 0x4,
                      (const AX_S8*)"ax_sysmap_test") < 0) {
    printf("alloc src buffer failed\n");
    return -1;
  }
  if (AX_SYS_MemAlloc(&phys_dst, &virt_dst, kTestLen, 0x4,
                      (const AX_S8*)"ax_sysmap_test") < 0) {
    printf("alloc dst buffer failed\n");
    AX_SYS_MemFree(phys_src, virt_src);
    return -1;
  }

  printf("malloc phy addr: %" PRIx64 ", %" PRIx64 "\n",
         static_cast<uint64_t>(phys_src), static_cast<uint64_t>(phys_dst));

  printf("Test uncached\n");
  DoSysmapTest(false, phys_src, phys_dst);

  printf("Test cached\n");
  DoSysmapTest(true, phys_src, phys_dst);

  AX_SYS_MemFree(phys_src, virt_src);
  AX_SYS_MemFree(phys_dst, virt_dst);

  printf("sysmap test pass\n");
  return 0;
}
