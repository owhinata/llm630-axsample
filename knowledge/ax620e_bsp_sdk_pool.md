# ax620e_bsp_sdk 調査メモ（pool 移植向け）

## 1. 目的と概要
- 目的: `ax620e_bsp_sdk/msp/sample/pool` の C 実装（`sample_pool.c`）を C++ に最小限で移植するための要点を整理する。
- 概要: AX の共通/専用メモリプール（POOL）を構成し、ブロックの取得/アドレス解決/マップ/操作/解放までの基本フローを確認するサンプル。
- 参考: CMM/メモリマップの背景は sysmap ノートも参照。
  - [knowledge/ax620e_bsp_sdk_sysmap.md](ax620e_bsp_sdk_sysmap.md)

## 2. 参照元サンプルと関連ファイル
- サンプル: `ax620e_bsp_sdk/msp/sample/pool/`
  - 実装: `sample_pool.c`
  - 説明: `README.md`
- 必要 API/ヘッダ: `ax_sys_api.h`, `ax_pool_type.h`（`ax_sys_api.h` から取り込まれる）
- リンクライブラリ: `libax_sys.so`（または `.a`）

## 3. 使用 API（要点）
- 初期化/終了
  - `AX_SYS_Init()` / `AX_SYS_Deinit()`
- プール構成（共通プール）
  - `AX_POOL_Exit()` → `AX_POOL_SetConfig(AX_POOL_FLOORPLAN_T*)` → `AX_POOL_Init()`
  - `AX_POOL_FLOORPLAN_T::CommPool[i]` に `MetaSize`, `BlkSize`, `BlkCnt`, `CacheMode`, `PartitionName` を設定。
  - 例の `PartitionName` は `"anonymous"`（CMM ロード時に存在する分割名であること）。
- ユーザプール（専用プール）
  - `AX_POOL_CreatePool(AX_POOL_CONFIG_T*)` / `AX_POOL_DestroyPool(AX_POOL)`
  - `AX_POOL_CONFIG_T` に `MetaSize`, `BlkSize`, `BlkCnt`, `CacheMode`, `PartitionName`。
- ブロック取得/情報
  - `AX_POOL_GetBlock(AX_POOL pool, AX_U64 BlkSize, const AX_S8 *pPartitionName)`
    - `pool=AX_INVALID_POOLID` で共通プールから取得、特定の PoolId 指定で専用プールから取得。
  - `AX_POOL_Handle2PoolId(AX_BLK)` → `AX_POOL_Handle2PhysAddr(AX_BLK)` → `AX_POOL_Handle2MetaPhysAddr(AX_BLK)`
- マップ/アンマップ/仮想アドレス
  - `AX_POOL_MmapPool(AX_POOL)` / `AX_POOL_MunmapPool(AX_POOL)`
  - `AX_POOL_GetBlockVirAddr(AX_BLK)` / `AX_POOL_GetMetaVirAddr(AX_BLK)`
- 解放
  - `AX_POOL_ReleaseBlock(AX_BLK)`

## 4. 処理フロー（`sample_pool.c` 相当）
1. `AX_SYS_Init()`
2. 既存プールを `AX_POOL_Exit()`
3. `AX_POOL_FLOORPLAN_T` を 0 初期化し、共通プールを定義
   - 例: 1MiB/2MiB/3MiB の非キャッシュ共通プール、`MetaSize=0x2000`、`PartitionName="anonymous"`
4. `AX_POOL_SetConfig()` → `AX_POOL_Init()`
5. 専用プールを 3 つ作成（サイズ/個数/キャッシュモードを変える）
6. ブロックを取得（専用プール or 共通プール）
7. `Handle2PoolId/PhysAddr/MetaPhysAddr` で情報を確認
8. `AX_POOL_MmapPool()` → `GetBlockVirAddr/GetMetaVirAddr` → メモリ書き込み・表示
9. `AX_POOL_ReleaseBlock()` → `AX_POOL_MunmapPool()`
10. `AX_POOL_DestroyPool()`（作成した専用プールを3つとも）
11. `AX_POOL_Exit()` → `AX_SYS_Deinit()`

## 5. C++ 移植の設計メモ
- RAII 設計
  - SystemGuard: `AX_SYS_Init/Deinit` をスコープ管理。
  - PoolManager: `AX_POOL_Exit/SetConfig/Init` の安全な適用と、作成した PoolId の廃棄管理。
  - BlockGuard: `AX_POOL_GetBlock/ReleaseBlock` をオブジェクトで管理。
  - PoolMapping: `AX_POOL_MmapPool/MunmapPool` と `GetBlockVirAddr/GetMetaVirAddr` をラップ。
- ログ/計測
  - `printf` ベースで十分。必要に応じて `std::chrono::steady_clock` で計測。
- スタイル
  - Google C++ Style に準拠。cpplint の指摘（整数型、ヘッダ順）に注意。

## 6. CMake（最小の雛形イメージ）
- 例（概念）:
  - `add_executable(sample_pool_cpp src/sample_pool.cc)`
  - `target_include_directories(sample_pool_cpp PRIVATE ax620e_bsp_sdk/msp/out/arm64_glibc/include)`
  - `target_link_directories(sample_pool_cpp PRIVATE ax620e_bsp_sdk/msp/out/arm64_glibc/lib)`
  - `target_link_libraries(sample_pool_cpp PRIVATE ax_sys)`

## 7. 注意点・トラブルシューティング
- `PartitionName` は CMM の既存分割名と一致させること（例: `anonymous`）。
- ブロックサイズ/個数が不足した場合、`AX_POOL_GetBlock` が失敗する。
- `AX_POOL_MmapPool` は Pool 単位のマップである点に注意（ブロック個別ではない）。
- キャッシュモードは `AX_POOL_CACHE_MODE_NONCACHE / CACHED` から選択。
- 取り扱い順序（取得→マップ→使用→解放→アンマップ）を守る。

- エラー `0x800B0111 (AX_ERR_POOL_BUSY)` について:
  - 発生条件: ブロックを保持したまま `AX_POOL_MunmapPool` や `AX_POOL_DestroyPool` を実行。
  - 回避策: 使用後は必ず「ReleaseBlock → MunmapPool → DestroyPool」の順で処理する。
  - 実装例: `BlockGuard::Release()` を呼んだ後に `PoolMapping::Unmap()`、最後に `AX_POOL_DestroyPool()`。
  - 備考: リファレンスサンプル（ax620e_bsp_sdk/msp/sample/pool/sample_pool.c）も
    この順序（ReleaseBlock → MunmapPool → DestroyPool）で実装されている。

このメモを起点に、`cpp/sample_pool` へ C++ 版サンプルを実装してください。
