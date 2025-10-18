/**
 * @file cmm.hpp
 * @brief C++11 interface for AX_SYS CMM utilities.
 *
 * This header exposes safe, RAII-friendly wrappers for AXERA CMM
 * (contiguous memory) operations. The API uses Result<T> to report
 * failures with error codes and lazily constructed messages.
 *
 * Thread-safety
 * - CmmBuffer is safe to use from multiple threads for read-only
 *   queries and for mapping, freeing or detaching with internal
 *   synchronization.
 * - CmmView is intended for single-thread ownership. Do not share the
 *   same CmmView instance across threads without external sync.
 *
 * Ownership model
 * - Allocate() creates an owned allocation. It must be released after
 *   all views are destroyed or reset (Free()).
 * - AttachExternal() attaches a non-owned physical range. Release via
 *   DetachExternal() after all views are destroyed.
 * - Free() applies to owned allocations only. DetachExternal() applies
 *   to attached external ranges only.
 *
 * Usage example
 * @code{.cpp}
 * using axsys::CmmBuffer; using axsys::CacheMode;
 *
 * CmmBuffer buf;
 * auto r = buf.Allocate(1024 * 1024, CacheMode::kNonCached, "demo");
 * if (!r) { fprintf(stderr, "%s\n", r.Message().c_str()); return; }
 * axsys::CmmView view = r.MoveValue();
 *
 * // Use mapped data
 * memset(view.Data(), 0, view.Size());
 *
 * // Flush a cached view range when needed
 * (void)view.Flush(0, 4096);
 *
 * // Destroy views before freeing the buffer
 * view.Reset();
 * (void)buf.Free();
 * @endcode
 *
 * @warning Free()/DetachExternal() fail while views are still alive.
 * @note MapView* size is limited to 4 GiB by underlying AX_SYS APIs.
 */
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "axsys/error.hpp"
#include "axsys/result.hpp"

namespace axsys {

enum class CacheMode { kNonCached = 0, kCached = 1 };

class CmmBuffer;  // fwd

/**
 * @brief A mapped view into a CMM allocation.
 *
 * Each CmmView represents a virtual mapping with an offset and size.
 * Views are lightweight and can be reset to unmap. The underlying
 * allocation is reference-counted via CmmBuffer and other views.
 */
class CmmView {
 public:
  CmmView();
  CmmView(CmmView&& other) noexcept;
  CmmView& operator=(CmmView&& other) noexcept;
  CmmView(const CmmView&) = delete;
  CmmView& operator=(const CmmView&) = delete;
  ~CmmView();

  /** @brief Base pointer of the mapping. */
  void* Data() const;
  /** @brief Size of the mapping in bytes. */
  size_t Size() const;
  /** @brief Cache mode of this view. */
  CacheMode Mode() const;

  /** @brief True if this view is valid (mapped). */
  explicit operator bool() const;
  /** @brief Unmap and invalidate this view. Safe to call multiple times. */
  void Reset();

  /**
   * @brief Flush cache lines in [offset, offset+size).
   * @param offset Start offset relative to this view.
   * @param size Number of bytes. SIZE_MAX means till end of view.
   * @return Result<void> error on failure.
   */
  Result<void> Flush(size_t offset = 0, size_t size = SIZE_MAX);

  /**
   * @brief Invalidate cache lines in [offset, offset+size).
   * @param offset Start offset relative to this view.
   * @param size Number of bytes. SIZE_MAX means till end of view.
   * @return Result<void> error on failure.
   */
  Result<void> Invalidate(size_t offset = 0, size_t size = SIZE_MAX);

  /**
   * @brief Create a sub-view within this view's range.
   * @param offset Offset relative to this view.
   * @param size Size of the sub-view.
   * @param mode Desired cache mode for the mapping.
   * @return Result<CmmView> new view on success.
   * @note Offsets are relative to the current view, not allocation base.
   */
  Result<CmmView> MapView(size_t offset, size_t size, CacheMode mode) const;

  /**
   * @brief Fast variant using AX_SYS fast mapping facilities.
   * @sa MapView
   */
  Result<CmmView> MapViewFast(size_t offset, size_t size, CacheMode mode) const;

  /**
   * @brief Create a buffer wrapper sharing this view's allocation.
   */
  Result<CmmBuffer> MakeBuffer() const;

  /** @name Diagnostics */
  ///@{
  /** @brief Physical address at the beginning of the allocation. */
  uint64_t Phys() const;
  /** @brief Offset (bytes) of this view within its allocation. */
  size_t Offset() const;

  void Dump(uintptr_t offset = 0) const;
  ///@}

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

  /**
   * @brief Allocate an owned CMM block and map the base view.
   * @param size Requested size in bytes (<= 4 GiB).
   * @param mode Cache mode for the base mapping.
   * @param token Allocation tag string passed to AX_SYS.
   * @return Result<CmmView> base view on success.
   */
  Result<CmmView> Allocate(size_t size, CacheMode mode, const char* token);

  /**
   * @brief Free an owned allocation.
   * @return Result<void> error if not owned, not allocated, or views remain.
   * @warning Fails while any views still reference the allocation.
   */
  Result<void> Free();

  /**
   * @brief Attach to an external (non-owned) physical range.
   * @param phys Physical address of the external range.
   * @param size Size of the range in bytes.
   * @return Result<void> error on failure.
   * @note Mutually exclusive with Allocate().
   */
  Result<void> AttachExternal(uint64_t phys, size_t size);

  /**
   * @brief Detach from the currently attached external range.
   * @return Result<void> error if no external range or views remain.
   */
  Result<void> DetachExternal();

  /**
   * @brief Map a view within the allocation.
   * @param offset Offset from allocation base.
   * @param size Size to map.
   * @param mode Cache mode for the mapping.
   * @return Result<CmmView> view on success.
   */
  Result<CmmView> MapView(size_t offset, size_t size, CacheMode mode) const;

  /**
   * @brief Fast mapping variant.
   * @sa MapView
   */
  Result<CmmView> MapViewFast(size_t offset, size_t size, CacheMode mode) const;

  /** @name Diagnostics */
  ///@{
  uint64_t Phys() const;
  size_t Size() const;
  void Dump(uintptr_t offset = 0) const;
  bool Verify() const;
  ///@}

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
