# ax620e_bsp_sdk 調査メモ（cmm 移植向け）

## 1. 目的と概要
- 目的: `ax620e_bsp_sdk/msp/sample/cmm` の C 実装（`sample_cmm.c`）の要点を整理し、C++ での最小移植（代表ケース）に備える。
- 概要: CMM（AX_SYS のメモリ管理）API の基本を網羅。確保/解放、キャッシュ/非キャッシュのマップ、キャッシュのフラッシュ/無効化、ブロック情報の取得など。
- 参考: `/dev/ax_sysmap` を使う `sysmap` とは別経路。`cmm` は AX_SYS の純正 API 群（`AX_SYS_Mmap*`）を直接利用する。
  - 併読: [knowledge/ax620e_bsp_sdk_sysmap.md](ax620e_bsp_sdk_sysmap.md)

## 2. 参照元サンプルと関連ファイル
- サンプル: `ax620e_bsp_sdk/msp/sample/cmm/`
  - 実装: `sample_cmm.c`（多数のケースを一括実装）
  - 説明: `README.md`（ケース一覧が記載）
- 必要ヘッダ/ライブラリ
  - ヘッダ: `ax_sys_api.h`（`AX_SYS_*`, `AX_POOL_*` など）
  - ライブラリ: `libax_sys.so`（または `.a`）

## 3. 代表 API（最小移植で扱うもの）
- 確保/解放
  - `AX_SYS_MemAlloc(AX_U64* phy, void** vir, AX_U32 size, AX_U32 align, const AX_S8* token)`
  - `AX_SYS_MemAllocCached(...)`
  - `AX_SYS_MemFree(AX_U64 phy, void* vir)`
- マップ/アンマップ
  - `AX_SYS_Mmap(AX_U64 phy, AX_U32 size)`           → 非キャッシュでマップ
  - `AX_SYS_MmapCache(AX_U64 phy, AX_U32 size)`      → キャッシュでマップ
  - `AX_SYS_MmapFast/AX_SYS_MmapCacheFast`           → 繰り返しの高速マップ（代表では任意）
  - `AX_SYS_Munmap(void* vir, AX_U32 size)`
- キャッシュ制御
  - `AX_SYS_MflushCache(AX_U64 phy, void* vir, AX_U32 size)`       → CPU→メモリへ書き戻し
  - `AX_SYS_MinvalidateCache(AX_U64 phy, void* vir, AX_U32 size)`  → メモリ→CPU再読込（無効化）
- ブロック情報
  - `AX_SYS_MemGetBlockInfoByVirt(void* vir, AX_U64* phy, AX_S32* memType)`
  - `AX_SYS_MemGetBlockInfoByPhy(AX_U64 phy, AX_S32* memType, void** vir, AX_U32* blkSize)`

## 4. 最小移植で対象とするケース（提案）
- 001: MemAlloc/MemFree（non-cached）
- 002: MemAllocCached/MemFree（cached）
- 004: Mmap/Munmap（non-cached マップ）
- 005: MmapCache/MflushCache/Munmap（cached マップ + 書き戻し）
- 006: MmapCache/MinvalidateCache/Munmap（cached マップ + 読み直し）
- 019: GetBlockInfoByVirt/ByPhy（cached/非cached の扱い確認、範囲外でも安全に）

※ `README.md` の全ケース移植は段階的に。まずは上記で基本動作・整合性が確認できる。

## 5. 代表フロー（例: cached バッファのコピー/検証）
1. `AX_SYS_Init()`
2. `AX_SYS_MemAllocCached(&phy_src, &vir_src, LEN, align, token)`
3. `AX_SYS_MemAllocCached(&phy_dst, &vir_dst, LEN, align, token)`
4. `void* map_src = AX_SYS_MmapCache(phy_src, LEN)`
5. `void* map_dst = AX_SYS_MmapCache(phy_dst, LEN)`
6. 事前に `AX_SYS_MinvalidateCache(phy_src, map_src, LEN)` / `MinvalidateCache(phy_dst, map_dst, LEN)`（必要に応じて）
7. `memcpy(map_dst, map_src, LEN)` を複数回（タイミング計測は `std::chrono::steady_clock`）
8. 書き込み経路では `AX_SYS_MflushCache(phy_dst, map_dst, LEN)` を適切に適用
9. `AX_SYS_Munmap(map_src, LEN)` / `AX_SYS_Munmap(map_dst, LEN)`
10. `AX_SYS_MemFree(phy_src, vir_src)` / `AX_SYS_MemFree(phy_dst, vir_dst)`
11. `AX_SYS_Deinit()`

## 6. C++ 移植設計メモ
- RAII 構成（pool/sysmap と同様のパターン）
  - SystemGuard: `AX_SYS_Init/Deinit`
  - CmmBuffer: `AX_SYS_MemAlloc(_Cached)/MemFree` の生存期間管理
  - CmmMapper: `AX_SYS_Mmap(_Cache)/Munmap` をラップ（cached フラグで分岐）
  - CacheCtrl: `MflushCache/MinvalidateCache` の適用ユーティリティ
  - Timer: `std::chrono::steady_clock` で合計時間/コピー当たり時間を表示（既存サンプルに合わせる）
- ログ/検証
  - `printf` ベースで物理/仮想アドレス、サイズ、結果を明示
  - `memcmp` による整合性チェック

## 7. CMake（最小の雛形イメージ）
- 例（概念）:
  - `add_executable(sample_cmm src/sample_cmm.cc)`
  - `target_include_directories(sample_cmm PRIVATE ax620e_bsp_sdk/msp/out/arm64_glibc/include)`
  - `target_link_directories(sample_cmm PRIVATE ax620e_bsp_sdk/msp/out/arm64_glibc/lib)`
  - `target_link_libraries(sample_cmm PRIVATE ax_sys)`

## 8. 注意点・トラブルシューティング
- キャッシュ一貫性
  - CPU 書き込み後にデバイス/他者が読む場合は `MflushCache`、メモリ/デバイス書き込み後に CPU が読む場合は `MinvalidateCache`。
  - cached マップで `memcmp` する前に、必要な側のキャッシュ制御を適用。
- オフセット指定
  - `AX_SYS_Mmap*` の offset は物理アドレスをそのまま渡す設計（`sample_cmm` でオフセット検証ケースあり）。
- エラー
  - 返値 < 0 は失敗。メッセージに戻り値を 0x%X で併記すると追跡しやすい。
- `sysmap` との違い
  - `sysmap` は `/dev/ax_sysmap` と `mmap(2)` を直接使用（`O_SYNC` の有無でキャッシュを切替）。
  - `cmm` は AX_SYS の抽象 API を使用（`AX_SYS_Mmap` / `AX_SYS_MmapCache`）。

このメモをもとに、まずは代表ケース（001/002/004/005/006/019）を `cpp/sample_cmm` として最小構成で移植してください。

## 9. 実装状況と設計メモ（cpp/sample_cmm）
- 実装状況
  - 001〜024 の全テストケースを実装。実行順は昇順。
  - 012/014 は元サンプル同様「失敗を期待」のケース。環境差で成功する場合があり、その際は注意ログを出力。
  - 015/016 は「非托管モード」の検証（CMM領域の物理アドレスを直接Mmap/MmapCache）。

- 設計メモ（本リポジトリのC++実装方針）
  - CacheMode enum（kNonCached/kCached）でブール表現を排除。
  - CmmBuffer に CmmMapper を内包し、Map/Unmap を統合。Free 時は Unmap→MemFree の順。
  - CmmBuffer に AddrBuf への暗黙変換演算子を実装。MemcpyFunc などの補助関数へ素直に渡せる。
  - MemcpyFunc は参照元サンプルの流儀を踏襲（キャッシュ側は Mmap、一方が非キャッシュなら生ポインタ）。
  - 計時は必要箇所で std::chrono::steady_clock を使用可能な構成（今回は最小適用）。

- ログ/整合性
  - 返値は 0x%X で出力。非4Kアラインのオフセット（例: 0x11EF）は挙動が不安定な場合があるためログで可視化。
