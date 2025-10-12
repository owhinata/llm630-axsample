// C++11 interface for AX_SYS CMM utilities
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace axsys {

enum class CacheMode { kNonCached = 0, kCached = 1 };

class CmmBuffer;  // fwd

class CmmView {
 public:
  CmmView();
  CmmView(CmmView&& other) noexcept;
  CmmView& operator=(CmmView&& other) noexcept;
  CmmView(const CmmView&) = delete;
  CmmView& operator=(const CmmView&) = delete;
  ~CmmView();

  void* Data() const;
  uint32_t Size() const;
  uint32_t Offset() const;
  CacheMode Mode() const;

  bool Flush();
  bool Invalidate();

  uint64_t Phys() const;

  bool Ok() const;
  void Reset();

 private:
  friend class CmmBuffer;
  struct Impl;  // internal
  explicit CmmView(Impl* impl);
  Impl* impl_;
};

class CmmBuffer {
 public:
  CmmBuffer();
  CmmBuffer(CmmBuffer&& other) noexcept;
  CmmBuffer& operator=(CmmBuffer&& other) noexcept;
  CmmBuffer(const CmmBuffer&) = delete;
  CmmBuffer& operator=(const CmmBuffer&) = delete;
  ~CmmBuffer();

  // Allocate ownership and return a base view (offset 0..size)
  CmmView Allocate(uint32_t size, CacheMode mode, const char* token);

  // Attach to an external (non-owned) physical range; enables MapView*
  // Free() will only check no open views and reset, without MemFree.
  bool AttachExternal(uint64_t phys, uint32_t size);

  // Create an additional view
  CmmView MapView(uint32_t offset, uint32_t size, CacheMode mode) const;

  // Create an additional view using AX_SYS_MmapFast/AX_SYS_MmapCacheFast
  CmmView MapViewFast(uint32_t offset, uint32_t size, CacheMode mode) const;

  // Free the allocation (requires no remaining views)
  bool Free();

  // Diagnostics
  uint64_t Phys() const;
  uint32_t Size() const;
  void Dump() const;
  bool Verify() const;

  // Partition helpers (hide AX_SYS from apps)
  struct PartitionInfo {
    std::string name;
    uint64_t phys;
    uint32_t size_kb;
  };
  static bool QueryPartitions(std::vector<PartitionInfo>* out);
  static bool FindAnonymous(PartitionInfo* out);

 private:
  struct Impl;  // internal
  Impl* impl_;
};

}  // namespace axsys
