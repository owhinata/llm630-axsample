<<COPY_FROM:knowledge/spec_ax_sys_cpp.md>>
# libax_sys_cpp 仕様（ax_sys C++ ラッパ）

本ドキュメントは、AXERA SDK の ax_sys を C++ から扱うための軽量ラッパ `libax_sys_cpp` の公開インターフェイスと、実運用での注意点をまとめたものです。設計の裏側には踏み込みません。内容は現行ヘッダ（axsys/system.hpp, axsys/cmm.hpp, axsys/sys.hpp）に基づいています。

## ヘッダと名前空間
- ヘッダ: `axsys/system.hpp`（AX_SYS 初期化/後始末）、`axsys/cmm.hpp`（CMM: 割当/マップ/キャッシュ）、`axsys/sys.hpp`（アンブレラ）
- 名前空間: すべて `axsys`
- 型の基本方針: 物理アドレスは `uint64_t`、サイズ/オフセットは `size_t`

## axsys::System（RAII）
- 役割: 生成時に `AX_SYS_Init()`、破棄時に `AX_SYS_Deinit()` を実行
- API 概要
  - `System()`, `~System()`
  - `System(System&&)`, `System& operator=(System&&)`（ムーブのみ）
  - `bool Ok() const`  初期化の成否
- 注意: 通常はプロセス内で 1 つ生成すれば十分です（先頭でローカル変数として作成）。

## axsys::CmmView（メモリビュー／RAII）
- 生成
  - `CmmBuffer::Allocate(size, mode, token)` が返すベースビュー（オフセット0、サイズ=割当サイズ）
  - `CmmBuffer::MapView(offset, size, mode)`／`MapViewFast(...)` による追加ビュー
  - `CmmView::MakeBuffer()` で、ビューが参照する割当を共有する `CmmBuffer` を再取得可
- 主要メソッド
  - `void* Data() const`, `size_t Size() const`, `size_t Offset() const`, `CacheMode Mode() const`
  - `explicit operator bool() const`（ビューが有効か）
  - `void Reset()`（`AX_SYS_Munmap` によりマップ解除）
  - キャッシュ制御
    - `bool Flush()`／`bool Flush(size_t offset)`／`bool Flush(size_t offset, size_t size)`
    - `bool Invalidate()`／`bool Invalidate(size_t offset)`／`bool Invalidate(size_t offset, size_t size)`
    - 備考: AX_SYS 側は `AX_U32` サイズ制限のため、実装内部で 4GiB 超をチャンク分割して呼び出します。
  - 追加ビュー
    - `CmmView MapView(size_t offset, size_t size, CacheMode mode) const`
    - `CmmView MapViewFast(size_t offset, size_t size, CacheMode mode) const`
  - 診断
    - `uint64_t Phys() const`（このビュー起点の物理）
    - `void Dump(uintptr_t offset = 0) const`（ByVirt によるブロック情報の出力）
- 利用上の注意
  - `Reset()` 後の `Data()` は無効です。以降のアクセスは未定義です。
  - `MapViewFast` は AX_SYS の Fast API を使用します（アドレス同一性の確認などに有用）。

## axsys::CmmBuffer（割当の共有所有）
- 役割: `AX_SYS_MemAlloc(_Cached)` による割当の所有者。複数の `CmmView` が同一割当（共有状態）を参照します。
- 主要メソッド
  - ライフサイクル
    - `CmmView Allocate(size_t size, CacheMode mode, const char* token)`
      - 4GiB 超の割当は AX_SYS の仕様上（`AX_U32`）失敗します。
    - `bool AttachExternal(uint64_t phys, size_t size)`（非所有の外部物理範囲にアタッチ）
    - `bool Free()`（自分以外の参照が残っていないときに成功。最後の参照破棄時に `AX_SYS_MemFree(phy, base_vir)` を実行）
  - ビュー生成
    - `CmmView MapView(size_t offset, size_t size, CacheMode mode) const`
    - `CmmView MapViewFast(size_t offset, size_t size, CacheMode mode) const`
  - 診断
    - `uint64_t Phys() const`, `size_t Size() const`
    - `void Dump(uintptr_t offset = 0) const`（ByPhy とビュー一覧）
    - `bool Verify() const`（ByPhy/ByVirt/Partition による簡易検証）
  - パーティション/ステータス
    - `struct PartitionInfo { std::string name; uint64_t phys; uint32_t size_kb; }`
    - `static std::vector<PartitionInfo> QueryPartitions()`
    - `static bool FindAnonymous(PartitionInfo* out)`
    - `struct CmmStatus { uint32_t total_size; remain_size; block_count; std::vector<PartitionInfo> partitions; }`
    - `static bool MemQueryStatus(CmmStatus* out)`
- 利用上の注意
  - 解放タイミング
    - `Free()` は「自分以外の参照（他の `CmmBuffer`/`CmmView`）が残っている場合」は失敗します。成功時は割当の参照を手放し、最終参照破棄時に内部デリータが `AX_SYS_MemFree(phy, base_vir)` を呼びます。
    - `~CmmBuffer()` は自分の参照を解放するだけです。最終参照が消えた時点でメモリが解放されます。
  - 外部アタッチは `Free()` で物理解放しません（非所有のため）。
  - 4GiB 制限: `MapView/MapViewFast` は単一ビューで 4GiB 超を扱えません。必要に応じて範囲分割してください。
  - 混在禁止: 同一物理範囲について、ライブラリ外で `AX_SYS_Mmap*` を直接行う運用は避けてください（寿命管理が分散）。

## 既知の挙動と注意
- `AX_SYS_MemGetBlockInfoByPhy` は解放直後でも成功を返す場合があり、厳密な「解放済み」判定には不向きです。`/proc/self/maps` と `AX_SYS_MemGetBlockInfoByVirt` を併用してください。
- キャッシュ整合性の基本則
  - CPU 書き込み → 他者が読む前に `Flush`
  - 他者書き込み → CPU が読む前に `Invalidate`

## 典型例
- 初期化と割当/解放
  - `axsys::System sys; if (!sys.Ok()) return -1;`
  - `axsys::CmmBuffer buf; auto v = buf.Allocate(1<<20, CacheMode::kCached, "tag");`
  - `memset(v.Data(), 0xAA, v.Size()); v.Flush();`
  - `auto nc = buf.MapView(0, v.Size(), CacheMode::kNonCached);`
  - `buf.Free();`

- 外部範囲
  - `axsys::CmmBuffer::PartitionInfo p; axsys::CmmBuffer::FindAnonymous(&p);`
  - `uint64_t phys = p.phys + (uint64_t)p.size_kb * 1024 - (1<<20);`
  - `axsys::CmmBuffer ext; ext.AttachExternal(phys, 1<<20);`
  - `auto c = ext.MapView(0, 1<<20, CacheMode::kCached); c.Invalidate();`

## ビルド/リンク
- 依存: AXERA SDK（`ax_sys`）
- 追加インクルード: `ax620e_bsp_sdk/msp/out/arm64_glibc/include`
- リンク: `ax_sys`
- 公開 I/F は C++11 互換（ヘッダは C++11 でコンパイル可能）

## スレッド安全性（概要）
- `CmmBuffer::Allocate/MapView/MapViewFast/Free`、および `CmmView` のデストラクタはスレッド安全（内部 mutex で保護）。
- 同一データ領域への同時 `Flush/Invalidate/書き込み` の整合は利用側で管理してください。

## 推奨事項
- 同一物理範囲を本ライブラリと生の `AX_SYS_*` で混在させない
- 単一ビューで 4GiB 超は作成できない（API 制約）。必要に応じて分割
- `Free()` は最終参照のみ成功。最後の参照が消えた時点で解放される
