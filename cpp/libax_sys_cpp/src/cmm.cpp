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
  size_t size;
  size_t offset;
  CacheMode mode;
};

struct Allocation {
  AX_U64 phy;
  size_t size;
  CacheMode mode;
  mutable std::mutex mtx;
  int open_maps;
  std::vector<ViewEntry> views;
  bool owned;
  void* base_vir;
  Allocation()
      : phy(0),
        size(0),
        mode(CacheMode::kNonCached),
        open_maps(0),
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
  if (!impl_) return;
  if (impl_->data) {
    if (impl_->size <= 0xFFFFFFFFu) {
      AX_SYS_Munmap(impl_->data, static_cast<AX_U32>(impl_->size));
    }
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

bool CmmView::Flush() {
  if (!impl_ || !impl_->data) return false;
  return Flush(0, impl_->size);
}

bool CmmView::Flush(size_t offset) {
  if (!impl_ || !impl_->data) return false;
  if (offset >= impl_->size) return false;
  return Flush(offset, impl_->size - offset);
}

bool CmmView::Flush(size_t offset, size_t size) {
  if (!impl_ || !impl_->alloc || !impl_->data) return false;
  if (offset >= impl_->size) return false;
  if (size == 0 || offset + size > impl_->size) return false;
  AX_U64 phys = impl_->alloc->phy + impl_->offset + offset;
  uintptr_t v = reinterpret_cast<uintptr_t>(impl_->data) + offset;
  size_t remain = size;
  while (remain > 0) {
    AX_U32 chunk =
        remain > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<AX_U32>(remain);
    AX_S32 ret = AX_SYS_MflushCache(phys, reinterpret_cast<void*>(v), chunk);
    if (ret != 0) return false;
    phys += chunk;
    v += chunk;
    remain -= chunk;
  }
  return true;
}

bool CmmView::Invalidate() {
  if (!impl_ || !impl_->data) return false;
  return Invalidate(0, impl_->size);
}

bool CmmView::Invalidate(size_t offset) {
  if (!impl_ || !impl_->data) return false;
  if (offset >= impl_->size) return false;
  return Invalidate(offset, impl_->size - offset);
}

bool CmmView::Invalidate(size_t offset, size_t size) {
  if (!impl_ || !impl_->alloc || !impl_->data) return false;
  if (offset >= impl_->size) return false;
  if (size == 0 || offset + size > impl_->size) return false;
  AX_U64 phys = impl_->alloc->phy + impl_->offset + offset;
  uintptr_t v = reinterpret_cast<uintptr_t>(impl_->data) + offset;
  size_t remain = size;
  while (remain > 0) {
    AX_U32 chunk =
        remain > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<AX_U32>(remain);
    AX_S32 ret =
        AX_SYS_MinvalidateCache(phys, reinterpret_cast<void*>(v), chunk);
    if (ret != 0) return false;
    phys += chunk;
    v += chunk;
    remain -= chunk;
  }
  return true;
}

CmmView CmmView::MapView(size_t offset, size_t size, CacheMode mode) const {
  if (!impl_ || !impl_->alloc) return CmmView();
  Allocation& a = *impl_->alloc;
  if (offset + size > impl_->size) return CmmView();
  size_t abs_off = impl_->offset + offset;

  void* v = DoMmap(a.phy + abs_off, size, mode);
  if (!v) return CmmView();

  {
    std::lock_guard<std::mutex> lk(a.mtx);
    a.open_maps += 1;
    ViewEntry e;
    e.addr = v;
    e.size = size;
    e.offset = abs_off;
    e.mode = mode;
    a.views.push_back(e);
  }

  CmmView::Impl* vi = new CmmView::Impl();
  vi->alloc = impl_->alloc;
  vi->data = v;
  vi->size = size;
  vi->offset = abs_off;
  vi->mode = mode;
  return CmmView(vi);
}

CmmView CmmView::MapViewFast(size_t offset, size_t size, CacheMode mode) const {
  if (!impl_ || !impl_->alloc) return CmmView();
  Allocation& a = *impl_->alloc;
  if (offset + size > impl_->size) return CmmView();
  size_t abs_off = impl_->offset + offset;

  void* v = DoMmapFast(a.phy + abs_off, size, mode);
  if (!v) return CmmView();

  {
    std::lock_guard<std::mutex> lk(a.mtx);
    a.open_maps += 1;
    ViewEntry e;
    e.addr = v;
    e.size = size;
    e.offset = abs_off;
    e.mode = mode;
    a.views.push_back(e);
  }

  CmmView::Impl* vi = new CmmView::Impl();
  vi->alloc = impl_->alloc;
  vi->data = v;
  vi->size = size;
  vi->offset = abs_off;
  vi->mode = mode;
  return CmmView(vi);
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
  if (impl_) {
    delete impl_;
  }
}

CmmView CmmBuffer::Allocate(size_t size, CacheMode mode, const char* token) {
  if (!impl_) return CmmView();
  if (impl_->alloc && impl_->alloc->phy != 0) {
    return CmmView();
  }
  if (size > 0xFFFFFFFFu) {
    printf("[CmmBuffer::Allocate] size too large (0x%zx)\n", size);
    return CmmView();
  }
  AX_U64 phy = 0;
  void* vir = nullptr;
  AX_S32 ret = 0;
  AX_U32 sz = static_cast<AX_U32>(size);
  if (mode == CacheMode::kCached) {
    ret = AX_SYS_MemAllocCached(&phy, &vir, sz, 0x1000,
                                reinterpret_cast<const AX_S8*>(token));
  } else {
    ret = AX_SYS_MemAlloc(&phy, &vir, sz, 0x1000,
                          reinterpret_cast<const AX_S8*>(token));
  }
  if (ret != 0) {
    return CmmView();
  }
  Allocation* a = new Allocation();
  a->phy = phy;
  a->size = size;
  a->mode = mode;
  a->owned = true;
  a->base_vir = vir;
  impl_->alloc = std::shared_ptr<Allocation>(a, [](Allocation* p) {
    if (!p) return;
    if (p->owned && p->phy != 0) {
      AX_S32 r = AX_SYS_MemFree(p->phy, p->base_vir);
      if (r != 0) {
        printf(
            "[CmmBuffer::Deleter] AX_SYS_MemFree failed: 0x%X (phy=0x%" PRIx64
            ")\n",
            static_cast<unsigned int>(r), static_cast<uint64_t>(p->phy));
      }
    }
    delete p;
  });

  // create base view by mapping 0..size
  return MapView(0, size, mode);
}

CmmView CmmBuffer::MapView(size_t offset, size_t size, CacheMode mode) const {
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
  vi->offset = offset;
  vi->data = v;
  vi->size = size;
  vi->mode = mode;
  return CmmView(vi);
}

bool CmmBuffer::AttachExternal(uint64_t phys, size_t size) {
  if (!impl_) return false;
  if (!impl_->alloc) {
    impl_->alloc = std::shared_ptr<Allocation>(new Allocation(),
                                               [](Allocation* p) { delete p; });
  }
  Allocation& a = *impl_->alloc;
  std::lock_guard<std::mutex> lk(a.mtx);
  if (a.open_maps != 0) return false;
  a.phy = static_cast<AX_U64>(phys);
  a.size = size;
  a.mode = CacheMode::kNonCached;
  a.views.clear();
  a.open_maps = 0;
  a.owned = false;
  return true;
}

bool CmmBuffer::Free() {
  if (!impl_ || !impl_->alloc) {
    printf("[CmmBuffer::Free] no allocation to free\n");
    return false;
  }
  // If other references (e.g., CmmView) exist, refuse to free.
  int64_t refs = impl_->alloc.use_count();
  if (refs > 1) {
    printf("[CmmBuffer::Free] references remain: %" PRId64 "\n", refs);
    return false;
  }
  impl_->alloc
      .reset();  // triggers custom deleter and AX_SYS_MemFree (if owned)
  return true;
}

uint64_t CmmBuffer::Phys() const {
  if (!impl_ || !impl_->alloc) return 0;
  return impl_->alloc->phy;
}

size_t CmmBuffer::Size() const {
  if (!impl_ || !impl_->alloc) return 0;
  return impl_->alloc->size;
}

void CmmBuffer::Dump(uintptr_t offset) const {
  if (!impl_ || !impl_->alloc) {
    printf("[CmmBuffer] empty\n");
    return;
  }
  const Allocation& a = *impl_->alloc;
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
  if (!impl_ || !impl_->alloc) return false;
  const Allocation& a = *impl_->alloc;
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

CmmView CmmBuffer::MapViewFast(size_t offset, size_t size,
                               CacheMode mode) const {
  if (!impl_ || !impl_->alloc) return CmmView();
  Allocation& a = *impl_->alloc;
  if (offset + size > a.size) return CmmView();

  void* v = DoMmapFast(a.phy + offset, size, mode);
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
  vi->offset = offset;
  vi->data = v;
  vi->size = size;
  vi->mode = mode;
  return CmmView(vi);
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

CmmBuffer CmmView::MakeBuffer() const {
  CmmBuffer buf;
  if (!impl_ || !impl_->alloc) return buf;
  buf.impl_ = new CmmBuffer::Impl();
  buf.impl_->alloc = impl_->alloc;
  return buf;
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
