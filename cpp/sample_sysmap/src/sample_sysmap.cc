// AXERA SDK
#include <ax_sys_api.h>

// C system headers
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// C++ headers
#include <chrono>
#include <cstdint>

namespace {
constexpr const char* kSysmapDev = "/dev/ax_sysmap";
constexpr uint32_t kTestLen = 0x1200000;  // 18 MiB
constexpr int kCopies = 50;

class ThroughputTimer {
 public:
  void Start() { start_ = std::chrono::steady_clock::now(); }
  void StopAndReport(uint64_t bytes_per_copy, int copies) {
    auto end = std::chrono::steady_clock::now();
    const double total_sec =
        std::chrono::duration<double>(end - start_).count();
    const double per_copy_sec = (copies > 0) ? (total_sec / copies) : total_sec;
    const double mib = static_cast<double>(bytes_per_copy) / (1024.0 * 1024.0);
    printf("data size: %" PRIu64 " bytes (%.2f MiB)\n",
           static_cast<uint64_t>(bytes_per_copy), mib);
    printf("time: %.6f sec for %d copies\n", total_sec, copies);
    printf("      %.6f sec per %" PRIu64 " bytes (%.2f MiB)\n", per_copy_sec,
           static_cast<uint64_t>(bytes_per_copy), mib);
  }

 private:
  std::chrono::steady_clock::time_point start_{};
};

class BufferPair {
 public:
  BufferPair()
      : phys_src(0), phys_dst(0), virt_src(nullptr), virt_dst(nullptr) {}
  bool Allocate(const char* token) {
    if (AX_SYS_MemAlloc(&phys_src, &virt_src, kTestLen, 0x4,
                        reinterpret_cast<const AX_S8*>(token)) < 0) {
      printf("alloc src buffer failed\n");
      return false;
    }
    if (AX_SYS_MemAlloc(&phys_dst, &virt_dst, kTestLen, 0x4,
                        reinterpret_cast<const AX_S8*>(token)) < 0) {
      printf("alloc dst buffer failed\n");
      AX_SYS_MemFree(phys_src, virt_src);
      phys_src = 0;
      virt_src = nullptr;
      return false;
    }
    return true;
  }
  void Free() {
    if (phys_src || virt_src) AX_SYS_MemFree(phys_src, virt_src);
    if (phys_dst || virt_dst) AX_SYS_MemFree(phys_dst, virt_dst);
    phys_src = phys_dst = 0;
    virt_src = virt_dst = nullptr;
  }
  AX_U64 phys_src;
  AX_U64 phys_dst;
  void* virt_src;
  void* virt_dst;
};

class SysmapMapper {
 public:
  explicit SysmapMapper(bool cached)
      : cached_(cached), fd_(-1), map_src_(nullptr), map_dst_(nullptr) {}
  bool Open() {
    int flags = O_RDWR | (cached_ ? 0 : O_SYNC);
    fd_ = open(kSysmapDev, flags);
    if (fd_ == -1) {
      printf("open %s fail!\n", kSysmapDev);
      return false;
    }
    return true;
  }
  bool Map(AX_U64 phys_src, AX_U64 phys_dst) {
    map_src_ = reinterpret_cast<char*>(mmap(
        nullptr, kTestLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, phys_src));
    map_dst_ = reinterpret_cast<char*>(mmap(
        nullptr, kTestLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, phys_dst));
    if (map_src_ == MAP_FAILED || map_dst_ == MAP_FAILED) {
      printf("map fail, %" PRIuPTR ", %" PRIuPTR "\n",
             reinterpret_cast<uintptr_t>(map_src_),
             reinterpret_cast<uintptr_t>(map_dst_));
      Unmap();
      return false;
    }
    return true;
  }
  void Unmap() {
    if (map_src_ && map_src_ != MAP_FAILED) munmap(map_src_, kTestLen);
    if (map_dst_ && map_dst_ != MAP_FAILED) munmap(map_dst_, kTestLen);
    map_src_ = map_dst_ = nullptr;
    if (fd_ != -1) {
      close(fd_);
      fd_ = -1;
    }
  }
  char* Src() const { return map_src_; }
  char* Dst() const { return map_dst_; }

 private:
  bool cached_;
  int fd_;
  char* map_src_;
  char* map_dst_;
};

void SanityCopy(char* dst, const char* src) {
  for (int i = 0; i < 0x20; ++i) {
    memcpy(dst + i, src + i, kTestLen - i);
    if (memcmp(dst + i, src + i, kTestLen - i) != 0) {
      printf("memcpy fail, i: %x\n", i);
    }
  }
}

bool RunOneCase(bool cached, const BufferPair& bufs) {
  SysmapMapper mapper(cached);
  if (!mapper.Open()) return false;
  if (!mapper.Map(bufs.phys_src, bufs.phys_dst)) return false;

  SanityCopy(mapper.Src(), mapper.Dst());

  ThroughputTimer t;
  t.Start();
  for (int i = 0; i < kCopies; ++i) {
    memcpy(mapper.Src(), mapper.Dst(), kTestLen);
  }
  t.StopAndReport(kTestLen, kCopies);

  mapper.Unmap();
  return true;
}

}  // namespace

int main() {
  if (AX_SYS_Init() != 0) {
    printf("AX_SYS_Init failed\n");
    return -1;
  }

  BufferPair bufs;
  if (!bufs.Allocate("ax_sysmap_test")) {
    return -1;
  }

  printf("malloc phy addr: %" PRIx64 ", %" PRIx64 "\n",
         static_cast<uint64_t>(bufs.phys_src),
         static_cast<uint64_t>(bufs.phys_dst));

  printf("Test uncached\n");
  RunOneCase(false, bufs);

  printf("Test cached\n");
  RunOneCase(true, bufs);

  bufs.Free();

  printf("sysmap test pass\n");
  return 0;
}
