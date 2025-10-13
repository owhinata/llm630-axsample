# libax_sys_cpp Specification (ax_sys C++ Wrapper)

This document describes the public interface of `libax_sys_cpp`, a thin C++ wrapper around the AXERA SDK `ax_sys` APIs. It focuses on how to use the headers and important caveats for practical use. Internal design details are intentionally omitted.

## Headers and Namespace
- Headers: `axsys/system.hpp`, `axsys/cmm.hpp`, and umbrella `axsys/sys.hpp`
- Namespace: all symbols live under `axsys`
- Types: physical address uses `uint64_t`; sizes/offsets use `size_t`

## axsys::System (RAII)
- Purpose: Initialize/deinitialize AX_SYS automatically.
- API
  - `System()`, `~System()`
  - `System(System&&)`, `System& operator=(System&&)` (move only)
  - `bool Ok() const` — whether initialization succeeded
- Note: Typically create one instance per process (e.g., at program start).

## axsys::CmmView (memory view, RAII)
- Creation
  - Returned by `CmmBuffer::Allocate(size, mode, token)` (base view, offset 0)
  - Created by `CmmBuffer::MapView(offset, size, mode)` / `MapViewFast(...)`
  - `CmmView::MakeBuffer()` re-acquires a `CmmBuffer` sharing the allocation
- API
  - `void* Data() const`, `size_t Size() const`, `size_t Offset() const`, `CacheMode Mode() const`
  - `explicit operator bool() const` — view validity
  - `void Reset()` — unmaps (AX_SYS_Munmap)
  - Cache control
    - `bool Flush()` / `bool Flush(size_t offset)` / `bool Flush(size_t offset, size_t size)`
    - `bool Invalidate()` / `bool Invalidate(size_t offset)` / `bool Invalidate(size_t offset, size_t size)`
  - Additional views
    - `CmmView MapView(size_t offset, size_t size, CacheMode mode) const`
    - `CmmView MapViewFast(size_t offset, size_t size, CacheMode mode) const`
  - Diagnostics
    - `uint64_t Phys() const` — base physical address + view offset
    - `void Dump(uintptr_t offset = 0) const` — ByVirt block info
- Caveats
  - After `Reset()`, `Data()` is invalid; any access is undefined.
  - AX_SYS cache APIs accept `AX_U32` size; the implementation internally splits ranges >4GiB into chunks.
  - `MapViewFast` uses AX_SYS fast-mapping APIs, useful for address consistency checks.

## axsys::CmmBuffer (allocation owner, shared among views)
- Purpose: Owns the AX_SYS allocation (MemAlloc/MemAllocCached). Views share a single underlying allocation.
- API
  - Lifecycle
    - `CmmView Allocate(size_t size, CacheMode mode, const char* token)`
      - Fails if `size > 4GiB` (AX_SYS size limit).
    - `bool AttachExternal(uint64_t phys, size_t size)` — attach a non-owned physical range
    - `bool Free()` — succeeds only if this is the last reference; triggers MemFree in deleter
  - Views
    - `CmmView MapView(size_t offset, size_t size, CacheMode mode) const`
    - `CmmView MapViewFast(size_t offset, size_t size, CacheMode mode) const`
  - Diagnostics
    - `uint64_t Phys() const`, `size_t Size() const`
    - `void Dump(uintptr_t offset = 0) const` — ByPhy and active views
    - `bool Verify() const` — shallow validation via ByPhy/ByVirt/Partition
  - Partitions & status
    - `struct PartitionInfo { std::string name; uint64_t phys; uint32_t size_kb; }`
    - `static std::vector<PartitionInfo> QueryPartitions()`
    - `static bool FindAnonymous(PartitionInfo* out)`
    - `struct CmmStatus { uint32_t total_size; remain_size; block_count; std::vector<PartitionInfo> partitions; }`
    - `static bool MemQueryStatus(CmmStatus* out)`
- Caveats
  - Free timing
    - `Free()` fails if other references (buffers/views) remain; when it succeeds, the last reference runs the deleter and calls `AX_SYS_MemFree(phy, base_vir)`.
    - `~CmmBuffer()` just releases this reference; memory is freed when the final reference is gone.
  - External attach
    - `AttachExternal()` does not free physical memory on `Free()` (non-owned).
  - 4GiB size limit
    - `MapView/MapViewFast` cannot create a single view >4GiB due to AX_SYS `AX_U32` limits; split the range if needed.
  - Do not mix
    - Avoid mixing raw `AX_SYS_Mmap*` with views on the same physical range; lifetime management diverges.

## Known behavior & practical notes
- `AX_SYS_MemGetBlockInfoByPhy` is not a reliable “freed” indicator immediately after free; prefer `/proc/self/maps` and `AX_SYS_MemGetBlockInfoByVirt` to check view unmapping.
- Cache rules: Flush before others read CPU writes; Invalidate before CPU reads others’ writes.

## Typical usage
- Init and allocate
  - `axsys::System sys; if (!sys.Ok()) return -1;`
  - `axsys::CmmBuffer buf; auto v = buf.Allocate(1<<20, CacheMode::kCached, "tag");`
  - `memset(v.Data(), 0xAA, v.Size()); v.Flush();`
  - `auto nc = buf.MapView(0, v.Size(), CacheMode::kNonCached);`
  - `buf.Free();`

- External range
  - `axsys::CmmBuffer::PartitionInfo p; axsys::CmmBuffer::FindAnonymous(&p);`
  - `uint64_t phys = p.phys + (uint64_t)p.size_kb * 1024 - (1<<20);`
  - `axsys::CmmBuffer ext; ext.AttachExternal(phys, 1<<20);`
  - `auto c = ext.MapView(0, 1<<20, CacheMode::kCached); c.Invalidate();`

## Build & link
- Depends on AXERA SDK `ax_sys`
- Include: `ax620e_bsp_sdk/msp/out/arm64_glibc/include`
- Link: `ax_sys`
- Public headers are C++11 compatible

## Thread-safety (summary)
- `CmmBuffer::Allocate/MapView/MapViewFast/Free` and `CmmView` destructor are thread-safe (internal mutex).
- Coordinate concurrent data access and cache ops at the application level.

## Recommendations
- Do not mix raw `AX_SYS_*` mappings with this library on the same physical range
- Single view >4GiB is not supported; split ranges as needed
- `Free()` only succeeds when it is the last holder; memory is freed when the final reference goes away

