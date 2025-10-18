# axsys C++ API Specification

This document specifies the public C++ API that wraps AXERA AX_SYS.
It describes headers, types, method signatures, return values, and
observable behavior. It is not a design document.

## Headers and Namespace
- Namespace: `axsys`
- Primary headers:
  - `axsys/system.hpp` — AX_SYS lifecycle RAII
  - `axsys/cmm.hpp` — CMM buffer and views
  - `axsys/sys.hpp` — umbrella header including the above

## Error Handling
- All methods return `Result<T>` or `Result<void>`.
- On success: `result` is truthy and carries a value (for `T`).
- On failure: `result` is falsy, `result.Code()` is an `ErrorCode`,
  `result.Message()` provides a short diagnostic string.

### ErrorCode
```
enum class ErrorCode {
  kSuccess = 0,
  // General
  kInvalidArgument = 1, kOutOfRange = 2, kNotInitialized = 3,
  kAlreadyInitialized = 4,
  // Memory
  kAllocationFailed = 100, kMemoryTooLarge = 101, kNoAllocation = 102,
  kNotOwned = 103, kReferencesRemain = 104, kMemFreeFailed = 105,
  // View/Mapping
  kMapFailed = 200, kUnmapFailed = 201, kFlushFailed = 202,
  kInvalidateFailed = 203, kViewRegistrationFailed = 204,
  // System
  kSystemInitFailed = 300, kSystemCallFailed = 301,
  // Unknown
  kUnknown = 999,
};
```

## System (AX_SYS lifecycle)
- Header: `axsys/system.hpp`
- Class: `axsys::System`
- Purpose: Initialize AX_SYS in ctor, deinitialize in dtor.
- API:
  - `System();`
  - `~System();`
  - `System(System&&) noexcept;`
  - `System& operator=(System&&) noexcept;`
  - `bool Ok() const;` — true if `AX_SYS_Init` succeeded

## CMM Basics
- Cache mode: `enum class CacheMode { kNonCached = 0, kCached = 1 }`
- Sizes and offsets use `size_t`; physical addresses use `uint64_t`.
- A `CmmBuffer` owns or attaches to an allocation; `CmmView` represents
  a mapped virtual range on that allocation.

## CmmBuffer
- Header: `axsys/cmm.hpp`
- Class: `axsys::CmmBuffer`
- Construction and lifetime:
  - `CmmBuffer(); ~CmmBuffer();`
  - Move-only: move ctor/assign supported; copy disabled.

### Methods
- Allocation and ownership
  - `Result<CmmView> Allocate(size_t size, CacheMode mode, const char* token);`
    - Allocates an owned CMM block and returns the base view (offset 0).
    - Errors: `kAlreadyInitialized`, `kMemoryTooLarge`, `kAllocationFailed`.
  - `Result<void> Free();`
    - Frees an owned allocation.
    - Errors: `kNoAllocation`, `kNotOwned`, `kReferencesRemain`.
  - `Result<void> AttachExternal(uint64_t phys, size_t size);`
    - Attaches a non-owned physical range for mapping.
    - Errors: `kAlreadyInitialized`.
  - `Result<void> DetachExternal();`
    - Detaches a previously attached external range.
    - Errors: `kNoAllocation`, `kReferencesRemain`.

- Mapping
  - `Result<CmmView> MapView(size_t offset, size_t size, CacheMode mode) const;`
  - `Result<CmmView> MapViewFast(size_t offset, size_t size, CacheMode mode) const;`
    - Errors: `kNotInitialized`, `kNoAllocation`, `kOutOfRange`, `kMapFailed`,
      `kViewRegistrationFailed`.

- Diagnostics
  - `uint64_t Phys() const;`
  - `size_t Size() const;`
  - `void Dump(uintptr_t offset = 0) const;`
  - `bool Verify() const;`

- Partition and status helpers
  - `struct PartitionInfo { std::string name; uint64_t phys; uint32_t size_kb; };`
  - `static std::vector<PartitionInfo> QueryPartitions();`
  - `static bool FindAnonymous(PartitionInfo* out);`
  - `struct CmmStatus { uint32_t total_size; uint32_t remain_size; uint32_t block_count; std::vector<PartitionInfo> partitions; };`
  - `static bool MemQueryStatus(CmmStatus* out);`

### Notes
- `Allocate` and `AttachExternal` are mutually exclusive and only valid
  when the buffer is idle (no allocation present).
- `Free()` applies to owned allocations; `DetachExternal()` to attached
  external ranges.
- Mapping size is limited by AX_SYS `AX_U32` (≤ 4 GiB per view).

## CmmView
- Header: `axsys/cmm.hpp`
- Class: `axsys::CmmView`
- Purpose: Represents a mapped view of an allocation.

### Methods
- Introspection
  - `void* Data() const;`
  - `size_t Size() const;`
  - `size_t Offset() const;`
  - `CacheMode Mode() const;`
  - `explicit operator bool() const;`
  - `void Reset();` — unmap; idempotent

- Cache control
  - `Result<void> Flush(size_t offset = 0, size_t size = SIZE_MAX);`
  - `Result<void> Invalidate(size_t offset = 0, size_t size = SIZE_MAX);`
    - Errors: `kNotInitialized`, `kOutOfRange`, `kFlushFailed`,
      `kInvalidateFailed`.

- Additional views
  - `Result<CmmView> MapView(size_t offset, size_t size, CacheMode mode) const;`
  - `Result<CmmView> MapViewFast(size_t offset, size_t size, CacheMode mode) const;`

- Interop
  - `Result<CmmBuffer> MakeBuffer() const;` — buffer sharing the allocation

### Notes
- After `Reset()`, `Data()` becomes invalid.
- Offsets for `CmmView::MapView*` are relative to the current view.

## Minimal Examples
```cpp
#include "axsys/sys.hpp"
using axsys::CacheMode;

axsys::System sys; if (!sys.Ok()) return -1;
axsys::CmmBuffer buf;

auto r = buf.Allocate(1<<20, CacheMode::kNonCached, "demo");
if (!r) { fprintf(stderr, "%s\n", r.Message().c_str()); return -1; }
axsys::CmmView v = r.MoveValue();
memset(v.Data(), 0, v.Size());
v.Flush();
v.Reset();
buf.Free();
```

