# libax_sys_cpp 仕様書（ax_sys C++ ラッパ）

本仕様は AXERA SDK の ax_sys 全体（AX_SYS_* と付随 API 群）を C++ から安全に扱うための薄いラッパライブラリ「libax_sys_cpp」の設計・API・運用指針をまとめたものです。まずは CMM（メモリ確保/マッピング/キャッシュ制御）から提供し、順次ほかの ax_sys 領域へ拡張します。目的は、責務の明確化と RAII によるリークや順序不整合の防止です。

## 1. 概要と責務
- ライブラリ名: `libax_sys_cpp`
- 主要型:
  - `CacheMode`: キャッシュ種別（`kNonCached`, `kCached`）。
  - `CmmView`: 単一のマッピングビュー（RAII）。生存期間＝`AX_SYS_Mmap*` の寿命。`Flush`/`Invalidate` を備える。
  - `CmmBuffer`: 物理割当の所有者。明示的な `Free` が必須（自動解放なし）。
- 目標:
  - 生存期間・責務を「所有（CmmBuffer）」と「閲覧/マップ（CmmView）」で明確化。
  - キャッシュ整合性に必要な操作をビューに集約し、呼び出し側の迷いを削減。

## 2. API（提案）

名前空間: すべて `axsys` 名前空間配下に配置します。

### 2.1 列挙とヘルパ
```
namespace axsys {
enum class CacheMode { kNonCached = 0, kCached = 1 };
}
```

### 2.2 CmmView（RAII ビュー）
- 生成方法:
  - `CmmBuffer::Allocate(size, mode, token)` → ベースマップを管理する `CmmView` を返す（0..size）
  - `CmmBuffer::MapView(offset, size, mode)` → 追加の `CmmView` を返す（複数同時可）
- インタフェース（例）:
```
namespace axsys {
class CmmView {
 public:
  // デフォルト構築（空ビュー）。ムーブ可・コピー不可
  CmmView();
  // ムーブ可・コピー不可
  CmmView(CmmView&&) noexcept; CmmView& operator=(CmmView&&) noexcept;
  ~CmmView(); // dtor で AX_SYS_Munmap(Data, Size)

  void*       Data()   const;  // マップ先の仮想アドレス
  uint32_t    Size()   const;  // マップサイズ
  uint32_t    Offset() const;  // バッファ先頭からのオフセット
  CacheMode   Mode()   const;  // キャッシュ/非キャッシュ

  // キャッシュ制御（AX_SYS_MflushCache/AX_SYS_MinvalidateCache）
  bool Flush();      // 物理(phy+offset), Data(), Size()
  bool Invalidate(); // 物理(phy+offset), Data(), Size()

  // 診断
  uint64_t    Phys()     const; // 割当物理 + offset

  // 有効性とリセット
  bool        Ok()   const;  // 有効なビューか（Data()!=nullptr && Size()>0）
  void        Reset();       // 有効なら Munmap して空状態に戻す
};
}
```

### 2.3 CmmBuffer（割当所有）
- 役割: `AX_SYS_MemAlloc(_Cached)` の所有者。内部に共有状態（`shared_ptr<Allocation>`）を持つ。
- 参照カウント: 全ての `CmmView` は同じ `Allocation` を共有。ビュー破棄で `use_count--`。`auto_free=true` かつ `use_count==0` で `AX_SYS_MemFree(phy, base_vir)`。
- インタフェース（例）:
```
namespace axsys {
class CmmBuffer {
 public:
  // デフォルトコンストラクタ（非所有状態）
  CmmBuffer();
  // ムーブ可・コピー不可
  CmmBuffer(CmmBuffer&&) noexcept;
  CmmBuffer& operator=(CmmBuffer&&) noexcept;
  CmmBuffer(const CmmBuffer&) = delete;
  CmmBuffer& operator=(const CmmBuffer&) = delete;

  // 割当: baseVir を管理する CmmView を返す（0..size の初期ビュー）。
  // 自動解放は行わないため、利用後は必ず Free() を呼ぶこと。
  CmmView Allocate(uint32_t size, CacheMode mode, const char* token);

  // ビュー作成（複数同時 OK）
  CmmView MapView(uint32_t offset, uint32_t size, CacheMode mode) const;

  // 手動解放（必須）
  // 仕様: まだ View が残っている場合はエラーを返す
  bool Free();

  // 診断
  uint64_t Phys() const; // 先頭物理
  uint32_t Size() const; // 割当サイズ

  // 追加: ダンプ（物理アドレス/サイズ、Map 中の仮想アドレス/サイズ/キャッシュモードを列挙）
  void Dump() const;

  // 追加: 検証（Allocation と全 MapView が妥当かを確認）
  // - AX_SYS_MemGetBlockInfoByPhy: 先頭物理が有効か、ブロックサイズ/キャッシュ種別を取得
  // - AX_SYS_MemGetBlockInfoByVirt: 各ビューの仮想→物理が期待どおりか
  // - AX_SYS_MemGetPartitionInfo: 物理が既知パーティション（例: anonymous）範囲内か
  // 返値: すべての検証に通れば true。失敗時は false を返し、必要に応じてログ出力
  bool Verify() const;
};
}
```

備考:
- 本仕様では `CmmBuffer::Allocate()` は初期ビュー `CmmView` を返し、baseVir をそのビューが管理します。既存の `CmmBuffer` と同様にデフォルトコンストラクタで非所有状態を作れます。自動解放は行いません。必ず `Free()` を呼んで割当を解放してください（View が残っている場合はエラー）。

## 3. 内部設計（所有と寿命）
- `Allocation`（非公開）
  - フィールド: `phy`, `size`, `mode`, `use_count`
  - 同期: `CmmBuffer::Allocate/MapView/Free` と `CmmView` の dtor で共有状態を更新する際は mutex で保護する（`use_count` を atomic にするだけでは十分でないため）。
  - 解放: `CmmBuffer::Free()` が呼ばれ、`use_count==0` を満たす場合に `AX_SYS_MemFree(phy, <適切なvir>)` を実施。
    - 備考: 本SDKでは `AX_SYS_MemFree(phy, alias_vir)` が成功する観測があるため、実装では一時的に `AX_SYS_Mmap*` で vir を確保して解放に用いることができます（永続的な base_vir の保持は不要）。
- `CmmView`
  - `shared_ptr<Allocation>` を保持。`dtor` で `AX_SYS_Munmap` を行う。
  - 生成時の `AX_SYS_Mmap*` は `CmmBuffer::Allocate/MapView` 側で実行し、エラーを戻り値でハンドリングする（CmmView の ctor ではマップしない）。
  - `Flush`/`Invalidate` は `Allocation::phy + offset` を用いる。
- `CmmBuffer`
  - `Allocate` で `Allocation` を生成し、初期ビューを返す。
  - `MapView` は同じ `Allocation` を共有する `CmmView` を返す（複数同時 OK）。
  - `Dump`/`Verify` のため、生成した `CmmView` の生存（仮想アドレス/サイズ/モード）を内部レジストリで追跡する（スレッド安全）。

## 4. AX_SYS API との対応
- 確保/解放
  - `AX_SYS_MemAlloc` / `AX_SYS_MemAllocCached`
  - `AX_SYS_MemFree(phy, base_vir)`
- マップ/アンマップ
  - `AX_SYS_Mmap`（non-cached）、`AX_SYS_MmapCache`（cached）
  - `AX_SYS_MmapFast` / `AX_SYS_MmapCacheFast`（必要に応じて）
  - `AX_SYS_Munmap`（`CmmView` dtor）
- キャッシュ制御
  - `AX_SYS_MflushCache` / `AX_SYS_MinvalidateCache`
- ブロック情報
  - `AX_SYS_MemGetBlockInfoByVirt` / `AX_SYS_MemGetBlockInfoByPhy`
  - `AX_SYS_MemGetPartitionInfo`（検証用途: 物理アドレスが既知パーティション範囲内かを確認）

## 5. 仕様上の注意（観測結果と推奨）
- 実機観測（本プロジェクトの sample_cmm より）
  - alias_vir（`AX_SYS_Mmap*` で得た別エイリアス）に対する `AX_SYS_MemFree(phy, alias_vir)` が成功（0x0）。
  - `AX_SYS_MemAlloc` で得た base_vir に対する `AX_SYS_Munmap(base_vir, size)` も成功（0x0）。
  - ただし運用上は「エイリアスは Munmap、最終解放は base_vir で MemFree」という手順を推奨（将来の SDK 変更やポータビリティ確保のため）。
- エイリアス併用（cached/non-cached）
  - 併用可能。整合性は呼び出し側責務。
  - CPU 書き込み→他者参照前は `flush`、他者書き込み→CPU 参照前は `invalidate` を徹底。
- 例外およびエラー
  - 本ライブラリは例外を使わず、戻り値（bool/AX_S32）とログで通知。

## 6. スレッド安全性
- 最低限のスレッド安全性を提供します。
  - `CmmBuffer::Allocate`／`MapView`／`Free` はスレッド安全。
  - `CmmView` のデストラクタ（ビュー破棄に伴う参照解放）もスレッド安全。
  - それ以外のデータアクセス（Data()/Flush()/Invalidate() 等）の並行呼び出しは、呼び出し側で同期（ミューテックス等）してください。
  - 参照カウントや内部共有状態の更新はライブラリ内部の mutex で保護します。データの整合性（キャッシュ制御の順序など）は利用側の設計に依存します。

## 7. サンプルコード

### 7.1 典型: cached→Flush→non-cached で参照
```
axsys::CmmBuffer buf;
auto v_c = buf.Allocate(size, axsys::CacheMode::kCached, "tag");
memset(v_c.Data(), 0xAA, v_c.Size());
v_c.Flush(); // 他者（non-cached view/デバイス）が読む前に Flush
auto v_nc = buf.MapView(0, size, axsys::CacheMode::kNonCached);
// v_nc.Data() を読む
buf.Free();
```

### 7.2 典型: non-cached→cached 読み取り（Invalidate）
```
axsys::CmmBuffer buf;
auto v_nc = buf.Allocate(size, axsys::CacheMode::kNonCached, "tag");
// デバイス/他者が物理に書き込んだと仮定
auto v_c = buf.MapView(0, size, axsys::CacheMode::kCached);
v_c.Invalidate(); // CPU が読む前に Invalidate
// v_c.Data() を読む
buf.Free();
```

### 7.3 別オフセット/サイズのビュー
```
axsys::CmmBuffer buf;
auto v0 = buf.Allocate(size, axsys::CacheMode::kCached, "tag");
auto v1 = buf.MapView(0x1000, 0x2000, axsys::CacheMode::kCached);
auto v2 = buf.MapView(0x4000, 0x1000, axsys::CacheMode::kNonCached); // 併用例
buf.Dump();
buf.Free();
```

## 8. ビルド/リンク
- CMake ターゲット: `libax_sys_cpp`
  - includes: `ax620e_bsp_sdk/msp/out/arm64_glibc/include`
  - links: `ax_sys`
- 言語要件（重要）
  - 公開I/Fは C++11 互換（ヘッダは C++11 でコンパイル可能な記述に限定）
    - 例: move-only（ムーブCTor/代入）、std::mutex、std::unique_ptr、=delete などのC++11機能のみを使用
    - std::optional/string_view/shared_mutex など C++17 以降の型は I/F に露出させない
  - 実装内部はより新しい標準（C++14/17 など）を用いても良いが、ヘッダI/Fの互換性を壊さないこと
- コンパイラと標準指定
  - 本プロジェクトのクロスコンパイラは gcc-arm-9.2（g++ 9 系）
  - 既定のC++標準は C++14 系（gnu++14）であり、C++17 は既定では有効にならない
  - C++17 を使いたい場合は `-std=c++17`（または `-std=gnu++17`）を明示指定する

## 9. 運用ガイド
- 原則: ビュー（CmmView）は「作って使い終わったらスコープ終了で破棄」（RAII）。
- 解放: 本仕様では自動解放を行いません。必ず `CmmBuffer::Free()` を呼んで割当を解放してください（`Free()` はビューが残っている場合エラー）。
- デバッグ: 必要なら開いているビュー一覧を `CmmBuffer` に弱参照で保持（本仕様では必須ではない）。

## 10. 既存サンプルとの整合
- `cpp/sample_cmm` の観測結果と整合:
  - alias での `MemFree`／base_vir の `Munmap` が成功する挙動を確認済み。
  - ただしライブラリの標準手順は「alias は Munmap、最後に base_vir で MemFree」。
- `cpp/sample_sysmap`／`cpp/sample_pool` と併用可能。POOL ブロックの物理に対する `CmmView` 作成も同様に扱える（責務は同じ）。

---

本仕様は libax_sys_cpp の設計方針を定めるドキュメントです。実装に際しては、プロジェクト方針（`Allocate` の戻り値を `CmmView` のみにするか、`pair<CmmBuffer, CmmView>` にするか など）を最終決定し、API シグネチャを確定してください。
