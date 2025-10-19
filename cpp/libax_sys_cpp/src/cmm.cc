#include "axsys/cmm.hpp"

#include <ax_sys_api.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace axsys {

namespace {
struct ViewEntry {
  void* addr;
  size_t size;
  size_t offset;
  CacheMode mode;
};

struct Allocation {
  AX_U64 phy;
  size_t size;
  CacheMode mode;
  mutable std::mutex mtx;
  std::vector<ViewEntry> views;
  bool owned;
  void* base_vir;
  Allocation()
      : phy(0),
        size(0),
        mode(CacheMode::kNonCached),
        owned(false),
        base_vir(nullptr) {}
};

static void* DoMmap(AX_U64 phys, size_t size, CacheMode mode) {
  if (size > 0xFFFFFFFFu) return nullptr;
  AX_U32 sz = static_cast<AX_U32>(size);
  if (mode == CacheMode::kCached) {
    return AX_SYS_MmapCache(phys, sz);
  }
  return AX_SYS_Mmap(phys, sz);
}

static void* DoMmapFast(AX_U64 phys, size_t size, CacheMode mode) {
  if (size > 0xFFFFFFFFu) return nullptr;
  AX_U32 sz = static_cast<AX_U32>(size);
  if (mode == CacheMode::kCached) {
    return AX_SYS_MmapCacheFast(phys, sz);
  }
  return AX_SYS_MmapFast(phys, sz);
}
}  // namespace

struct CmmView::Impl {
  std::shared_ptr<Allocation> alloc;
  size_t offset;
  void* data;
  size_t size;
  CacheMode mode;
  Impl() : offset(0), data(nullptr), size(0), mode(CacheMode::kNonCached) {}
};

CmmView::CmmView() : impl_(nullptr) {}

CmmView::CmmView(Impl* impl) : impl_(impl) {}

CmmView::CmmView(CmmView&& other) noexcept : impl_(other.impl_) {
  other.impl_ = nullptr;
}

CmmView& CmmView::operator=(CmmView&& other) noexcept {
  if (this != &other) {
    Reset();
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

CmmView::~CmmView() { Reset(); }

void* CmmView::Data() const { return impl_ ? impl_->data : nullptr; }

size_t CmmView::Size() const { return impl_ ? impl_->size : 0; }

CacheMode CmmView::Mode() const {
  return impl_ ? impl_->mode : CacheMode::kNonCached;
}

uint64_t CmmView::Phys() const {
  return impl_ && impl_->alloc ? (impl_->alloc->phy + impl_->offset) : 0;
}

size_t CmmView::Offset() const { return impl_ ? impl_->offset : 0; }

CmmView::operator bool() const {
  return impl_ && impl_->data != nullptr && impl_->size > 0;
}

void CmmView::Reset() {
  // Thread-safety note: Each CmmView instance should be owned by a single
  // thread. Concurrent calls to Reset() on the same instance are not supported.
  if (!impl_) return;

  // Capture impl_ and set to nullptr immediately to prevent double-free
  Impl* local_impl = impl_;
  impl_ = nullptr;

  if (local_impl->data) {
    if (local_impl->size <= 0xFFFFFFFFu) {
      AX_SYS_Munmap(local_impl->data, static_cast<AX_U32>(local_impl->size));
    }
    if (local_impl->alloc) {
      std::lock_guard<std::mutex> lk(local_impl->alloc->mtx);
      // remove from registry
      for (std::vector<ViewEntry>::iterator it =
               local_impl->alloc->views.begin();
           it != local_impl->alloc->views.end(); ++it) {
        if (it->addr == local_impl->data) {
          local_impl->alloc->views.erase(it);
          break;
        }
      }
    }
  }
  delete local_impl;
}

Result<void> CmmView::Flush(size_t offset, size_t size) {
  if (!impl_ || !impl_->alloc || !impl_->data) {
    return Result<void>::Error(ErrorCode::kNotInitialized, [] {
      return std::string("View not initialized");
    });
  }
  if (offset >= impl_->size) {
    return Result<void>::Error(ErrorCode::kOutOfRange, [] {
      return std::string("Offset out of range");
    });
  }

  // Handle SIZE_MAX: flush to end of view
  size_t actual_size = size;
  if (size == SIZE_MAX || offset + size > impl_->size) {
    actual_size = impl_->size - offset;
  }

  if (actual_size == 0) {
    return Result<void>::Error(ErrorCode::kInvalidArgument,
                               [] { return std::string("Zero length"); });
  }

  AX_U64 phys = impl_->alloc->phy + impl_->offset + offset;
  uintptr_t v = reinterpret_cast<uintptr_t>(impl_->data) + offset;
  size_t remain = actual_size;
  while (remain > 0) {
    AX_U32 chunk =
        remain > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<AX_U32>(remain);
    AX_S32 ret = AX_SYS_MflushCache(phys, reinterpret_cast<void*>(v), chunk);
    if (ret != 0) {
      return Result<void>::Error(ErrorCode::kFlushFailed, [] {
        return std::string("AX_SYS_MflushCache failed");
      });
    }
    phys += chunk;
    v += chunk;
    remain -= chunk;
  }
  return Result<void>::Ok();
}

Result<void> CmmView::Invalidate(size_t offset, size_t size) {
  if (!impl_ || !impl_->alloc || !impl_->data) {
    return Result<void>::Error(ErrorCode::kNotInitialized, [] {
      return std::string("View not initialized");
    });
  }
  if (offset >= impl_->size) {
    return Result<void>::Error(ErrorCode::kOutOfRange, [] {
      return std::string("Offset out of range");
    });
  }

  // Handle SIZE_MAX: invalidate to end of view
  size_t actual_size = size;
  if (size == SIZE_MAX || offset + size > impl_->size) {
    actual_size = impl_->size - offset;
  }

  if (actual_size == 0) {
    return Result<void>::Error(ErrorCode::kInvalidArgument,
                               [] { return std::string("Zero length"); });
  }

  AX_U64 phys = impl_->alloc->phy + impl_->offset + offset;
  uintptr_t v = reinterpret_cast<uintptr_t>(impl_->data) + offset;
  size_t remain = actual_size;
  while (remain > 0) {
    AX_U32 chunk =
        remain > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<AX_U32>(remain);
    AX_S32 ret =
        AX_SYS_MinvalidateCache(phys, reinterpret_cast<void*>(v), chunk);
    if (ret != 0) {
      return Result<void>::Error(ErrorCode::kInvalidateFailed, [] {
        return std::string("AX_SYS_MinvalidateCache failed");
      });
    }
    phys += chunk;
    v += chunk;
    remain -= chunk;
  }
  return Result<void>::Ok();
}

Result<CmmView> CmmView::MapView(size_t offset, size_t size,
                                 CacheMode mode) const {
  if (!impl_ || !impl_->alloc) {
    return Result<CmmView>::Error(ErrorCode::kNoAllocation, [] {
      return std::string("No allocation for view");
    });
  }
  if (offset + size > impl_->size) {
    return Result<CmmView>::Error(ErrorCode::kOutOfRange, [=] {
      char buf[128];
      snprintf(buf, sizeof(buf), "MapView out of range (off=0x%zx size=0x%zx)",
               offset, size);
      return std::string(buf);
    });
  }
  Allocation& a = *impl_->alloc;
  size_t abs_off = impl_->offset + offset;
  void* v = DoMmap(a.phy + abs_off, size, mode);
  if (!v) {
    return Result<CmmView>::Error(ErrorCode::kMapFailed, [] {
      return std::string("AX_SYS_Mmap failed");
    });
  }
  std::unique_ptr<CmmView::Impl> vi(new CmmView::Impl());
  vi->alloc = impl_->alloc;
  vi->data = v;
  vi->size = size;
  vi->offset = abs_off;
  vi->mode = mode;
  try {
    std::lock_guard<std::mutex> lk(a.mtx);
    ViewEntry e{v, size, abs_off, mode};
    a.views.push_back(e);
  } catch (...) {
    if (size <= 0xFFFFFFFFu) {
      AX_SYS_Munmap(v, static_cast<AX_U32>(size));
    }
    throw;
  }
  return Result<CmmView>::Ok(CmmView(vi.release()));
}

Result<CmmView> CmmView::MapViewFast(size_t offset, size_t size,
                                     CacheMode mode) const {
  if (!impl_ || !impl_->alloc) {
    return Result<CmmView>::Error(ErrorCode::kNoAllocation, [] {
      return std::string("No allocation for view");
    });
  }
  if (offset + size > impl_->size) {
    return Result<CmmView>::Error(ErrorCode::kOutOfRange, [=] {
      char buf[128];
      snprintf(buf, sizeof(buf),
               "MapViewFast out of range (off=0x%zx size=0x%zx)", offset, size);
      return std::string(buf);
    });
  }
  Allocation& a = *impl_->alloc;
  size_t abs_off = impl_->offset + offset;
  void* v = DoMmapFast(a.phy + abs_off, size, mode);
  if (!v) {
    return Result<CmmView>::Error(ErrorCode::kMapFailed, [] {
      return std::string("AX_SYS_MmapFast failed");
    });
  }
  std::unique_ptr<CmmView::Impl> vi(new CmmView::Impl());
  vi->alloc = impl_->alloc;
  vi->data = v;
  vi->size = size;
  vi->offset = abs_off;
  vi->mode = mode;
  try {
    std::lock_guard<std::mutex> lk(a.mtx);
    ViewEntry e{v, size, abs_off, mode};
    a.views.push_back(e);
  } catch (...) {
    if (size <= 0xFFFFFFFFu) {
      AX_SYS_Munmap(v, static_cast<AX_U32>(size));
    }
    throw;
  }
  return Result<CmmView>::Ok(CmmView(vi.release()));
}

struct CmmBuffer::Impl {
  std::shared_ptr<Allocation> alloc;
  std::mutex alloc_mtx;
  Impl() : alloc() {}
};

Result<CmmBuffer> CmmView::MakeBuffer() const {
  if (!impl_ || !impl_->alloc) {
    return Result<CmmBuffer>::Error(ErrorCode::kNoAllocation, [] {
      return std::string("No allocation to make buffer");
    });
  }
  CmmBuffer buf;
  // Exception-safe: use unique_ptr
  std::unique_ptr<CmmBuffer::Impl> impl_ptr(new CmmBuffer::Impl());
  impl_ptr->alloc = impl_->alloc;
  buf.impl_ = impl_ptr.release();
  return Result<CmmBuffer>::Ok(std::move(buf));
}

void CmmView::Dump(uintptr_t offset) const {
  if (!impl_ || !impl_->alloc || !impl_->data) {
    printf("[CmmView] empty\n");
    return;
  }
  printf("[CmmView] base_v=%p size=0x%zx mode=%s\n", impl_->data, impl_->size,
         (impl_->mode == CacheMode::kCached ? "cached" : "nonc"));
  // Range check relative to this view
  if (offset >= impl_->size) {
    printf("  [Dump] offset 0x%" PRIxPTR " out of range for view size 0x%zx\n",
           offset, impl_->size);
    return;
  }
  void* virt = reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(impl_->data) + offset);
  AX_U64 phys = 0;
  AX_S32 cache_type = 0;
  if (AX_SYS_MemGetBlockInfoByVirt(virt, &phys, &cache_type) == 0) {
    printf("  ByVirt: v=%p -> phy=0x%" PRIx64 ", cacheType=%d\n", virt,
           static_cast<uint64_t>(phys), cache_type);
  } else {
    printf("  ByVirt: query failed (v=%p)\n", virt);
  }
}

CmmBuffer::CmmBuffer() : impl_(new Impl()) {}

CmmBuffer::CmmBuffer(CmmBuffer&& other) noexcept : impl_(other.impl_) {
  other.impl_ = nullptr;
}

CmmBuffer& CmmBuffer::operator=(CmmBuffer&& other) noexcept {
  if (this != &other) {
    if (impl_) delete impl_;
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

CmmBuffer::~CmmBuffer() {
  if (impl_) {
    delete impl_;
  }
}

Result<CmmView> CmmBuffer::Allocate(size_t size, CacheMode mode,
                                    const char* token) {
  if (!impl_) {
    impl_ = new Impl();
  }
  {
    std::lock_guard<std::mutex> lk(impl_->alloc_mtx);
    if (impl_->alloc) {
      return Result<CmmView>::Error(ErrorCode::kAlreadyInitialized, [] {
        return std::string("Buffer already allocated or attached");
      });
    }
    if (size > 0xFFFFFFFFu) {
      return Result<CmmView>::Error(ErrorCode::kMemoryTooLarge, [size] {
        char buf[96];
        snprintf(buf, sizeof(buf), "Size too large: 0x%zx", size);
        return std::string(buf);
      });
    }
    AX_U64 phy = 0;
    void* vir = nullptr;
    AX_S32 ret = 0;
    const AX_U32 sz = static_cast<AX_U32>(size);
    if (mode == CacheMode::kCached) {
      ret = AX_SYS_MemAllocCached(&phy, &vir, sz, 0x1000,
                                  reinterpret_cast<const AX_S8*>(token));
    } else {
      ret = AX_SYS_MemAlloc(&phy, &vir, sz, 0x1000,
                            reinterpret_cast<const AX_S8*>(token));
    }
    if (ret != 0) {
      return Result<CmmView>::Error(ErrorCode::kAllocationFailed, [] {
        return std::string("AX_SYS_MemAlloc failed");
      });
    }
    // Exception-safe: use unique_ptr until fully constructed
    std::unique_ptr<Allocation> a(new Allocation());
    a->phy = phy;
    a->size = size;
    a->mode = mode;
    a->owned = true;
    a->base_vir = vir;

    // Transfer ownership to shared_ptr with custom deleter
    // If this throws, unique_ptr will clean up (but won't call AX_SYS_MemFree)
    // So we need try-catch
    try {
      impl_->alloc =
          std::shared_ptr<Allocation>(a.release(), [](Allocation* p) {
            if (!p) return;
            if (p->owned && p->phy != 0) {
              AX_S32 r = AX_SYS_MemFree(p->phy, p->base_vir);
              if (r != 0) {
                printf(
                    "[CmmBuffer::Deleter] AX_SYS_MemFree failed: 0x%X "
                    "(phy=0x%" PRIx64 ")\n",
                    static_cast<unsigned int>(r),
                    static_cast<uint64_t>(p->phy));
              }
            }
            delete p;
          });
    } catch (...) {
      // shared_ptr construction failed, need to free the allocation
      AX_SYS_MemFree(phy, vir);
      throw;
    }
  }

  // create base view by mapping 0..size
  return MapView(0, size, mode);
}

Result<void> CmmBuffer::Free() {
  if (!impl_) {
    return Result<void>::Error(ErrorCode::kNoAllocation, [] {
      return std::string("No allocation to free");
    });
  }
  std::lock_guard<std::mutex> lk(impl_->alloc_mtx);
  if (!impl_->alloc) {
    return Result<void>::Error(ErrorCode::kNoAllocation, [] {
      return std::string("No allocation to free");
    });
  }
  if (!impl_->alloc->owned) {
    return Result<void>::Error(ErrorCode::kNotOwned, [] {
      return std::string("Buffer does not own memory");
    });
  }
  int64_t refs = impl_->alloc.use_count();
  if (refs > 1) {
    return Result<void>::Error(ErrorCode::kReferencesRemain, [refs] {
      char buf[64];
      snprintf(buf, sizeof(buf), "References remain: %" PRId64, refs);
      return std::string(buf);
    });
  }
  impl_->alloc.reset();
  return Result<void>::Ok();
}

Result<void> CmmBuffer::AttachExternal(uint64_t phys, size_t size) {
  if (!impl_) {
    impl_ = new Impl();
  }
  std::lock_guard<std::mutex> lk(impl_->alloc_mtx);
  if (impl_->alloc) {
    return Result<void>::Error(ErrorCode::kAlreadyInitialized, [] {
      return std::string("Buffer already allocated or attached");
    });
  }
  // Exception-safe: construct with unique_ptr first
  std::unique_ptr<Allocation> a(new Allocation());
  a->phy = static_cast<AX_U64>(phys);
  a->size = size;
  a->mode = CacheMode::kNonCached;
  a->owned = false;

  // Transfer to shared_ptr
  std::shared_ptr<Allocation> alloc_holder(a.release(),
                                           [](Allocation* p) { delete p; });
  impl_->alloc = alloc_holder;
  return Result<void>::Ok();
}

Result<void> CmmBuffer::DetachExternal() {
  if (!impl_) {
    return Result<void>::Error(ErrorCode::kNoAllocation, [] {
      return std::string("No buffer to detach");
    });
  }
  std::lock_guard<std::mutex> lk(impl_->alloc_mtx);
  if (!impl_->alloc || impl_->alloc->owned) {
    return Result<void>::Error(ErrorCode::kNoAllocation, [] {
      return std::string("No external allocation attached");
    });
  }
  int64_t refs = impl_->alloc.use_count();
  if (refs > 1) {
    return Result<void>::Error(ErrorCode::kReferencesRemain, [refs] {
      char buf[64];
      snprintf(buf, sizeof(buf), "References remain: %" PRId64, refs);
      return std::string(buf);
    });
  }
  impl_->alloc.reset();
  return Result<void>::Ok();
}

Result<CmmView> CmmBuffer::MapView(size_t offset, size_t size,
                                   CacheMode mode) const {
  if (!impl_) {
    return Result<CmmView>::Error(ErrorCode::kNotInitialized, [] {
      return std::string("Buffer not initialized");
    });
  }

  // Thread-safe: capture shared_ptr under lock to prevent concurrent
  // Free/DetachExternal
  std::shared_ptr<Allocation> alloc_copy;
  {
    std::lock_guard<std::mutex> lk(impl_->alloc_mtx);
    alloc_copy = impl_->alloc;
  }

  if (!alloc_copy) {
    return Result<CmmView>::Error(ErrorCode::kNoAllocation, [] {
      return std::string("No allocation to map");
    });
  }
  Allocation& a = *alloc_copy;
  if (offset + size > a.size) {
    return Result<CmmView>::Error(ErrorCode::kOutOfRange, [=] {
      char buf[128];
      snprintf(buf, sizeof(buf), "MapView out of range (off=0x%zx size=0x%zx)",
               offset, size);
      return std::string(buf);
    });
  }

  void* v = DoMmap(a.phy + offset, size, mode);
  if (!v) {
    return Result<CmmView>::Error(ErrorCode::kMapFailed, [] {
      return std::string("AX_SYS_Mmap failed");
    });
  }

  // Exception-safe: use unique_ptr until fully constructed
  std::unique_ptr<CmmView::Impl> vi(new CmmView::Impl());
  vi->alloc = alloc_copy;  // Use the captured shared_ptr
  vi->offset = offset;
  vi->data = v;
  vi->size = size;
  vi->mode = mode;

  // Register view - if this throws, clean up the mmap
  try {
    std::lock_guard<std::mutex> lk(a.mtx);
    ViewEntry e;
    e.addr = v;
    e.size = size;
    e.offset = offset;
    e.mode = mode;
    a.views.push_back(e);
  } catch (...) {
    // Clean up the mmap on exception
    if (size <= 0xFFFFFFFFu) {
      AX_SYS_Munmap(v, static_cast<AX_U32>(size));
    }
    throw;
  }

  return Result<CmmView>::Ok(CmmView(vi.release()));
}

Result<CmmView> CmmBuffer::MapViewFast(size_t offset, size_t size,
                                       CacheMode mode) const {
  if (!impl_) {
    return Result<CmmView>::Error(ErrorCode::kNotInitialized, [] {
      return std::string("Buffer not initialized");
    });
  }

  // Thread-safe: capture shared_ptr under lock to prevent concurrent
  // Free/DetachExternal
  std::shared_ptr<Allocation> alloc_copy;
  {
    std::lock_guard<std::mutex> lk(impl_->alloc_mtx);
    alloc_copy = impl_->alloc;
  }

  if (!alloc_copy) {
    return Result<CmmView>::Error(ErrorCode::kNoAllocation, [] {
      return std::string("No allocation to map");
    });
  }
  Allocation& a = *alloc_copy;
  if (offset + size > a.size) {
    return Result<CmmView>::Error(ErrorCode::kOutOfRange, [=] {
      char buf[128];
      snprintf(buf, sizeof(buf),
               "MapViewFast out of range (off=0x%zx size=0x%zx)", offset, size);
      return std::string(buf);
    });
  }

  void* v = DoMmapFast(a.phy + offset, size, mode);
  if (!v) {
    return Result<CmmView>::Error(ErrorCode::kMapFailed, [] {
      return std::string("AX_SYS_MmapFast failed");
    });
  }

  // Exception-safe: use unique_ptr until fully constructed
  std::unique_ptr<CmmView::Impl> vi(new CmmView::Impl());
  vi->alloc = alloc_copy;  // Use the captured shared_ptr
  vi->offset = offset;
  vi->data = v;
  vi->size = size;
  vi->mode = mode;

  // Register view - if this throws, clean up the mmap
  try {
    std::lock_guard<std::mutex> lk(a.mtx);
    ViewEntry e;
    e.addr = v;
    e.size = size;
    e.offset = offset;
    e.mode = mode;
    a.views.push_back(e);
  } catch (...) {
    // Clean up the mmap on exception
    if (size <= 0xFFFFFFFFu) {
      AX_SYS_Munmap(v, static_cast<AX_U32>(size));
    }
    throw;
  }

  return Result<CmmView>::Ok(CmmView(vi.release()));
}

uint64_t CmmBuffer::Phys() const {
  if (!impl_) return 0;
  std::lock_guard<std::mutex> lk(impl_->alloc_mtx);
  if (!impl_->alloc) return 0;
  return impl_->alloc->phy;
}

size_t CmmBuffer::Size() const {
  if (!impl_) return 0;
  std::lock_guard<std::mutex> lk(impl_->alloc_mtx);
  if (!impl_->alloc) return 0;
  return impl_->alloc->size;
}

void CmmBuffer::Dump(uintptr_t offset) const {
  if (!impl_) {
    printf("[CmmBuffer] empty\n");
    return;
  }

  // Thread-safe: capture shared_ptr to prevent concurrent modifications
  std::shared_ptr<Allocation> alloc_copy;
  {
    std::lock_guard<std::mutex> lk(impl_->alloc_mtx);
    alloc_copy = impl_->alloc;
  }

  if (!alloc_copy) {
    printf("[CmmBuffer] empty\n");
    return;
  }
  const Allocation& a = *alloc_copy;
  printf("[CmmBuffer] phy=0x%" PRIx64 ", size=0x%zx, maps=%zu\n",
         static_cast<uint64_t>(a.phy), a.size, a.views.size());
  // ByPhy at base+offset
  AX_S32 cache_type = 0;
  void* vir_out = nullptr;
  AX_U32 blk_sz = 0;
  AX_U64 phy_q = a.phy + static_cast<AX_U64>(offset);
  AX_S32 r = AX_SYS_MemGetBlockInfoByPhy(phy_q, &cache_type, &vir_out, &blk_sz);
  if (r == 0) {
    printf("  ByPhy:  phy=0x%" PRIx64 " -> virt=%p, cacheType=%d, blkSz=0x%x\n",
           static_cast<uint64_t>(phy_q), vir_out, cache_type, blk_sz);
  } else {
    printf("  ByPhy:  query failed for phy=0x%" PRIx64 " (ret=0x%X)\n",
           static_cast<uint64_t>(phy_q), static_cast<unsigned int>(r));
  }
  std::lock_guard<std::mutex> lk(a.mtx);
  for (size_t i = 0; i < a.views.size(); ++i) {
    const ViewEntry& e = a.views[i];
    printf("  view[%zu]: v=%p off=0x%zx size=0x%zx mode=%s\n", i, e.addr,
           e.offset, e.size,
           (e.mode == CacheMode::kCached ? "cached" : "nonc"));
  }
}

bool CmmBuffer::Verify() const {
  if (!impl_) return false;

  // Thread-safe: capture shared_ptr to prevent concurrent modifications
  std::shared_ptr<Allocation> alloc_copy;
  {
    std::lock_guard<std::mutex> lk(impl_->alloc_mtx);
    alloc_copy = impl_->alloc;
  }

  if (!alloc_copy) return false;
  const Allocation& a = *alloc_copy;
  AX_S32 mem_type = 0;
  void* vir_out = nullptr;
  AX_U32 blk_size = 0;
  if (a.owned) {
    // Check phys for owned buffers
    if (AX_SYS_MemGetBlockInfoByPhy(a.phy, &mem_type, &vir_out, &blk_size) !=
        0) {
      return false;
    }
    if (blk_size != a.size) {
      return false;
    }
  }
  // Partition range check
  AX_CMM_PARTITION_INFO_T part;
  if (AX_SYS_MemGetPartitionInfo(&part) == 0) {
    bool in_range = false;
    for (AX_U32 i = 0; i < part.PartitionCnt; ++i) {
      AX_U64 base = part.PartitionInfo[i].PhysAddr;
      AX_U64 end =
          base + static_cast<AX_U64>(part.PartitionInfo[i].SizeKB) * 1024;
      if (a.phy >= base && (a.phy + a.size) <= end) {
        in_range = true;
        break;
      }
    }
    if (!in_range) return false;
  }
  // Each view: validate virt->phys mapping corresponds to base phys + offset
  std::lock_guard<std::mutex> lk(a.mtx);
  for (size_t i = 0; i < a.views.size(); ++i) {
    const ViewEntry& e = a.views[i];
    AX_U64 phys2 = 0;
    if (AX_SYS_MemGetBlockInfoByVirt(e.addr, &phys2, &mem_type) != 0) {
      return false;
    }
    if (phys2 < a.phy) return false;
    AX_U64 delta = phys2 - a.phy;
    if (delta != static_cast<AX_U64>(e.offset)) return false;
    if (e.offset + e.size > a.size) return false;
  }
  return true;
}

std::vector<CmmBuffer::PartitionInfo> CmmBuffer::QueryPartitions() {
  std::vector<PartitionInfo> v;
  AX_CMM_PARTITION_INFO_T part;
  if (AX_SYS_MemGetPartitionInfo(&part) != 0) return v;
  v.reserve(part.PartitionCnt);
  for (AX_U32 i = 0; i < part.PartitionCnt; ++i) {
    PartitionInfo pi;
    pi.name = reinterpret_cast<char*>(part.PartitionInfo[i].Name);
    pi.phys = part.PartitionInfo[i].PhysAddr;
    pi.size_kb = part.PartitionInfo[i].SizeKB;
    v.push_back(pi);
  }
  return v;
}

bool CmmBuffer::FindAnonymous(PartitionInfo* out) {
  if (!out) return false;
  auto parts = QueryPartitions();
  for (size_t i = 0; i < parts.size(); ++i) {
    if (parts[i].name == "anonymous") {
      *out = parts[i];
      return true;
    }
  }
  return false;
}

bool CmmBuffer::MemQueryStatus(CmmStatus* out) {
  if (!out) return false;
  AX_CMM_STATUS_T st;
  if (AX_SYS_MemQueryStatus(&st) != 0) return false;
  out->total_size = st.TotalSize;
  out->remain_size = st.RemainSize;
  out->block_count = st.BlockCnt;
  out->partitions.clear();
  for (AX_U32 i = 0; i < st.Partition.PartitionCnt; ++i) {
    PartitionInfo pi;
    pi.name = reinterpret_cast<char*>(st.Partition.PartitionInfo[i].Name);
    pi.phys = st.Partition.PartitionInfo[i].PhysAddr;
    pi.size_kb = st.Partition.PartitionInfo[i].SizeKB;
    out->partitions.push_back(pi);
  }
  return true;
}

}  // namespace axsys
