// C++11 interface for AX_SYS CMM utilities
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "axsys/error.hpp"
#include "axsys/result.hpp"

namespace axsys {

enum class CacheMode { kNonCached = 0, kCached = 1 };

class CmmBuffer;  // fwd

// Thread-safety: Each CmmView instance should be owned and accessed by a single
// thread. The underlying allocation (shared via CmmBuffer) is thread-safe.
class CmmView {
 public:
  CmmView();
  CmmView(CmmView&& other) noexcept;
  CmmView& operator=(CmmView&& other) noexcept;
  CmmView(const CmmView&) = delete;
  CmmView& operator=(const CmmView&) = delete;
  ~CmmView();

  void* Data() const;
  size_t Size() const;
  CacheMode Mode() const;

  explicit operator bool() const;
  void Reset();

  // Flush cache for [offset, offset+size) range. SIZE_MAX means flush to end.
  Result<void> Flush(size_t offset = 0, size_t size = SIZE_MAX);

  // Invalidate cache for [offset, offset+size) range. SIZE_MAX means invalidate
  // to end.
  Result<void> Invalidate(size_t offset = 0, size_t size = SIZE_MAX);

  // Create an additional view within this view's range.
  // The offset/size are relative to this view, not the allocation base.
  Result<CmmView> MapView(size_t offset, size_t size, CacheMode mode) const;

  // Fast mapping variant within this view's range.
  Result<CmmView> MapViewFast(size_t offset, size_t size, CacheMode mode) const;

  // Obtain a buffer that shares this view's allocation.
  Result<CmmBuffer> MakeBuffer() const;

  // Diagnostics
  uint64_t Phys() const;
  size_t Offset() const;

  void Dump(uintptr_t offset = 0) const;

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
  Result<CmmView> Allocate(size_t size, CacheMode mode, const char* token);

  // Free the allocation (requires no remaining views)
  Result<void> Free();

  // Attach to an external (non-owned) physical range; enables MapView*
  // Free() will only check no open views and reset, without MemFree.
  Result<void> AttachExternal(uint64_t phys, size_t size);

  // Detach from the currently attached external range.
  Result<void> DetachExternal();

  // Create an additional view
  Result<CmmView> MapView(size_t offset, size_t size, CacheMode mode) const;

  // Create an additional view using AX_SYS_MmapFast/AX_SYS_MmapCacheFast
  Result<CmmView> MapViewFast(size_t offset, size_t size, CacheMode mode) const;

  // Diagnostics
  uint64_t Phys() const;
  size_t Size() const;
  void Dump(uintptr_t offset = 0) const;
  bool Verify() const;

  // Partition helpers (hide AX_SYS from apps)
  struct PartitionInfo {
    std::string name;
    uint64_t phys;
    uint32_t size_kb;
  };
  static std::vector<PartitionInfo> QueryPartitions();
  static bool FindAnonymous(PartitionInfo* out);

  // Wrappers for AX_SYS_MemQueryStatus / AX_SYS_MemGetMaxFreeRegionInfo
  struct CmmStatus {
    uint32_t total_size;
    uint32_t remain_size;
    uint32_t block_count;
    std::vector<PartitionInfo> partitions;
  };
  static bool MemQueryStatus(CmmStatus* out);

 private:
  friend class CmmView;
  struct Impl;  // internal
  Impl* impl_;
};

}  // namespace axsys
