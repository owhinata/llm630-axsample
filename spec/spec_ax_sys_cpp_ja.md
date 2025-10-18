# axsys C++ API 仕様書

本書は AXERA AX_SYS を包む C++ API の仕様を示します。ヘッダ、
型、メソッドのシグネチャ、返り値、観測可能な動作を記載します。
設計解説は含みません。

## ヘッダと名前空間
- 名前空間: `axsys`
- 主要ヘッダ:
  - `axsys/system.hpp` — AX_SYS ライフサイクル (RAII)
  - `axsys/cmm.hpp` — CMM バッファとビュー
  - `axsys/sys.hpp` — 上記を含むアンブレラヘッダ

## エラー処理
- すべてのメソッドは `Result<T>` または `Result<void>` を返します。
- 成功時: 真となり（`T` の場合）値を保持します。
- 失敗時: 偽となり、`Code()` は `ErrorCode`、`Message()` は簡潔な
  診断文字列を返します。

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

## System（AX_SYS ライフサイクル）
- ヘッダ: `axsys/system.hpp`
- クラス: `axsys::System`
- 目的: コンストラクタで AX_SYS を初期化し、デストラクタで解放。
- API:
  - `System();`
  - `~System();`
  - `System(System&&) noexcept;`
  - `System& operator=(System&&) noexcept;`
  - `bool Ok() const;` — `AX_SYS_Init` 成否

## CMM の基本
- キャッシュ: `enum class CacheMode { kNonCached = 0, kCached = 1 }`
- サイズ/オフセットは `size_t`、物理アドレスは `uint64_t` を使用。
- `CmmBuffer` は割当の所有/外部接続を表し、`CmmView` はその
  割当に対する仮想ビューを表します。

## CmmBuffer
- ヘッダ: `axsys/cmm.hpp`
- クラス: `axsys::CmmBuffer`
- ライフタイム:
  - `CmmBuffer(); ~CmmBuffer();`
  - ムーブ専用（コピー不可）。

### メソッド
- 割当と所有
  - `Result<CmmView> Allocate(size_t size, CacheMode mode, const char* token);`
    - 所有割当を作成し、基底ビュー（オフセット0）を返す。
    - エラー: `kAlreadyInitialized`, `kMemoryTooLarge`,
      `kAllocationFailed`。
  - `Result<void> Free();`
    - 所有割当を解放。
    - エラー: `kNoAllocation`, `kNotOwned`, `kReferencesRemain`。
  - `Result<void> AttachExternal(uint64_t phys, size_t size);`
    - 非所有の物理範囲に接続。
    - エラー: `kAlreadyInitialized`。
  - `Result<void> DetachExternal();`
    - 外部接続を切り離し。
    - エラー: `kNoAllocation`, `kReferencesRemain`。

- マッピング
  - `Result<CmmView> MapView(size_t offset, size_t size, CacheMode mode) const;`
  - `Result<CmmView> MapViewFast(size_t offset, size_t size, CacheMode mode) const;`
    - エラー: `kNotInitialized`, `kNoAllocation`, `kOutOfRange`,
      `kMapFailed`, `kViewRegistrationFailed`。

- 診断
  - `uint64_t Phys() const;`
  - `size_t Size() const;`
  - `void Dump(uintptr_t offset = 0) const;`
  - `bool Verify() const;`

- パーティション/ステータス
  - `struct PartitionInfo { std::string name; uint64_t phys; uint32_t size_kb; };`
  - `static std::vector<PartitionInfo> QueryPartitions();`
  - `static bool FindAnonymous(PartitionInfo* out);`
  - `struct CmmStatus { uint32_t total_size; uint32_t remain_size; uint32_t block_count; std::vector<PartitionInfo> partitions; };`
  - `static bool MemQueryStatus(CmmStatus* out);`

### 注意事項
- `Allocate` と `AttachExternal` は相互排他で、バッファがアイドル
  （未割当）時にのみ実行可能。
- 所有割当は `Free()`、外部接続は `DetachExternal()` を使用。
- マッピングサイズは AX_SYS の `AX_U32` 制約により 4GiB 以下。

## CmmView
- ヘッダ: `axsys/cmm.hpp`
- クラス: `axsys::CmmView`
- 目的: 割当に対する仮想ビュー。

### メソッド
- 参照
  - `void* Data() const;`
  - `size_t Size() const;`
  - `size_t Offset() const;`
  - `CacheMode Mode() const;`
  - `explicit operator bool() const;`
  - `void Reset();` — アンマップ（冪等）

- キャッシュ制御
  - `Result<void> Flush(size_t offset = 0, size_t size = SIZE_MAX);`
  - `Result<void> Invalidate(size_t offset = 0, size_t size = SIZE_MAX);`
    - エラー: `kNotInitialized`, `kOutOfRange`, `kFlushFailed`,
      `kInvalidateFailed`。

- 追加ビュー
  - `Result<CmmView> MapView(size_t offset, size_t size, CacheMode mode) const;`
  - `Result<CmmView> MapViewFast(size_t offset, size_t size, CacheMode mode) const;`

- 相互運用
  - `Result<CmmBuffer> MakeBuffer() const;` — 割当を共有するバッファ

### 注意事項
- `Reset()` 後は `Data()` が無効。
- `CmmView::MapView*` のオフセットは当該ビュー相対。

## 最小例
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

