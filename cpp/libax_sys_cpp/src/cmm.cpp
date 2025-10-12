#include "axsys/cmm.hpp"

#include <ax_sys_api.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <memory>
#include <mutex>
#include <vector>

namespace axsys {

namespace {
struct ViewEntry {
  void* addr;
  uint32_t size;
  uint32_t offset;
  CacheMode mode;
};

struct Allocation {
  AX_U64 phy;
  uint32_t size;
  CacheMode mode;
  mutable std::mutex mtx;
  int open_maps;
  std::vector<ViewEntry> views;
  Allocation() : phy(0), size(0), mode(CacheMode::kNonCached), open_maps(0) {}
};

static void* DoMmap(AX_U64 phys, uint32_t size, CacheMode mode) {
  if (mode == CacheMode::kCached) {
    return AX_SYS_MmapCache(phys, size);
  }
  return AX_SYS_Mmap(phys, size);
}
}  // namespace

struct CmmView::Impl {
  std::shared_ptr<Allocation> alloc;
  void* data;
  uint32_t size;
  uint32_t offset;
  CacheMode mode;
  Impl() : data(nullptr), size(0), offset(0), mode(CacheMode::kNonCached) {}
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

uint32_t CmmView::Size() const { return impl_ ? impl_->size : 0; }

uint32_t CmmView::Offset() const { return impl_ ? impl_->offset : 0; }

CacheMode CmmView::Mode() const {
  return impl_ ? impl_->mode : CacheMode::kNonCached;
}

uint64_t CmmView::Phys() const {
  return impl_ && impl_->alloc ? (impl_->alloc->phy + impl_->offset) : 0;
}

bool CmmView::Ok() const {
  return impl_ && impl_->data != nullptr && impl_->size > 0;
}

void CmmView::Reset() {
  if (!impl_) return;
  if (impl_->data) {
    AX_SYS_Munmap(impl_->data, impl_->size);
    if (impl_->alloc) {
      std::lock_guard<std::mutex> lk(impl_->alloc->mtx);
      // remove from registry
      for (std::vector<ViewEntry>::iterator it = impl_->alloc->views.begin();
           it != impl_->alloc->views.end(); ++it) {
        if (it->addr == impl_->data) {
          impl_->alloc->views.erase(it);
          break;
        }
      }
      if (impl_->alloc->open_maps > 0) {
        impl_->alloc->open_maps -= 1;
      }
    }
  }
  delete impl_;
  impl_ = nullptr;
}

struct CmmBuffer::Impl {
  std::shared_ptr<Allocation> alloc;
  Impl() {}
};

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
  // Do not auto free; caller must call Free()
  if (impl_) delete impl_;
}

CmmView CmmBuffer::Allocate(uint32_t size, CacheMode mode, const char* token) {
  if (!impl_) return CmmView();
  if (impl_->alloc && impl_->alloc->phy != 0) {
    return CmmView();
  }
  AX_U64 phy = 0;
  void* vir = nullptr;
  AX_S32 ret = 0;
  if (mode == CacheMode::kCached) {
    ret = AX_SYS_MemAllocCached(&phy, &vir, size, 0x40,
                                reinterpret_cast<const AX_S8*>(token));
  } else {
    ret = AX_SYS_MemAlloc(&phy, &vir, size, 0x40,
                          reinterpret_cast<const AX_S8*>(token));
  }
  if (ret != 0) {
    return CmmView();
  }
  impl_->alloc.reset(new Allocation());
  impl_->alloc->phy = phy;
  impl_->alloc->size = size;
  impl_->alloc->mode = mode;

  // create base view by mapping 0..size
  return MapView(0, size, mode);
}

CmmView CmmBuffer::MapView(uint32_t offset, uint32_t size,
                           CacheMode mode) const {
  if (!impl_ || !impl_->alloc) return CmmView();
  Allocation& a = *impl_->alloc;
  if (offset + size > a.size) return CmmView();

  void* v = DoMmap(a.phy + offset, size, mode);
  if (!v) return CmmView();

  {
    std::lock_guard<std::mutex> lk(a.mtx);
    a.open_maps += 1;
    ViewEntry e;
    e.addr = v;
    e.size = size;
    e.offset = offset;
    e.mode = mode;
    a.views.push_back(e);
  }

  CmmView::Impl* vi = new CmmView::Impl();
  vi->alloc = impl_->alloc;
  vi->data = v;
  vi->size = size;
  vi->offset = offset;
  vi->mode = mode;
  return CmmView(vi);
}

bool CmmBuffer::Free() {
  if (!impl_ || !impl_->alloc) return false;
  Allocation& a = *impl_->alloc;
  std::lock_guard<std::mutex> lk(a.mtx);
  if (a.open_maps > 0) {
    return false;  // views remain
  }
  // create an alias to free; prefer non-cached map
  void* alias = AX_SYS_Mmap(a.phy, a.size);
  if (!alias) alias = AX_SYS_MmapCache(a.phy, a.size);
  if (!alias) return false;
  AX_S32 ret = AX_SYS_MemFree(a.phy, alias);
  AX_SYS_Munmap(alias, a.size);
  if (ret != 0) return false;
  impl_->alloc.reset();
  return true;
}

uint64_t CmmBuffer::Phys() const {
  if (!impl_ || !impl_->alloc) return 0;
  return impl_->alloc->phy;
}

uint32_t CmmBuffer::Size() const {
  if (!impl_ || !impl_->alloc) return 0;
  return impl_->alloc->size;
}

void CmmBuffer::Dump() const {
  if (!impl_ || !impl_->alloc) {
    printf("[CmmBuffer] empty\n");
    return;
  }
  const Allocation& a = *impl_->alloc;
  printf("[CmmBuffer] phy=0x%" PRIx64 ", size=%u, maps=%d\n",
         static_cast<uint64_t>(a.phy), a.size, a.open_maps);
  std::lock_guard<std::mutex> lk(a.mtx);
  for (size_t i = 0; i < a.views.size(); ++i) {
    const ViewEntry& e = a.views[i];
    printf("  view[%zu]: v=%p off=0x%x size=0x%x mode=%s\n", i, e.addr,
           e.offset, e.size,
           (e.mode == CacheMode::kCached ? "cached" : "nonc"));
  }
}

bool CmmBuffer::Verify() const {
  if (!impl_ || !impl_->alloc) return false;
  const Allocation& a = *impl_->alloc;
  // Check phys
  AX_S32 mem_type = 0;
  void* vir_out = nullptr;
  AX_U32 blk_size = 0;
  if (AX_SYS_MemGetBlockInfoByPhy(a.phy, &mem_type, &vir_out, &blk_size) != 0) {
    return false;
  }
  if (blk_size < a.size) {
    return false;
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
  // Each view virt->phys
  std::lock_guard<std::mutex> lk(a.mtx);
  for (size_t i = 0; i < a.views.size(); ++i) {
    const ViewEntry& e = a.views[i];
    AX_U64 phys2 = 0;
    if (AX_SYS_MemGetBlockInfoByVirt(e.addr, &phys2, &mem_type) != 0) {
      return false;
    }
    if (phys2 != a.phy) {  // base phys must match
      return false;
    }
  }
  return true;
}

}  // namespace axsys
