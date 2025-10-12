# ax620e_bsp_sdk 調査メモ（sysmap 移植向け）

## 1. 目的と概要
- 目的: `ax620e_bsp_sdk/msp/sample/sysmap` の C 実装（`sample_sysmap.c`）を C++ に最小限で移植するための前提情報を整理する。
- 概要: AX のシステムメモリ（CMM）から物理メモリを確保し、`/dev/ax_sysmap` を介してキャッシュ／ノンキャッシュの仮想アドレスへ `mmap(2)` でマップして帯域・整合性を確認するサンプル。

## 2. 参照元サンプルと関連ファイル
- サンプル: `ax620e_bsp_sdk/msp/sample/sysmap/`
  - 実装: `ax620e_bsp_sdk/msp/sample/sysmap/sample_sysmap.c`
  - 説明: `ax620e_bsp_sdk/msp/sample/sysmap/README.md`
  - ビルド: `Makefile`, `Makefile.dynamic`, `Makefile.static`
- 使用デバイスノード: `/dev/ax_sysmap`
  - ノンキャッシュマップ: `open("/dev/ax_sysmap", O_RDWR | O_SYNC)`
  - キャッシュマップ: `open("/dev/ax_sysmap", O_RDWR)`

## 3. 必要ヘッダ／ライブラリ／パス
- ヘッダ（生成済み）: `ax620e_bsp_sdk/msp/out/arm64_glibc/include`
  - 主要: `ax_sys_api.h`, `ax_base_type.h`
  - 付随: `ax_global_type.h`, `ax_pool_type.h`（`ax_sys_api.h` からインクルード）
  - 標準: `<unistd.h>`, `<sys/mman.h>`, `<fcntl.h>`, `<sys/time.h>`, `<string.h>`, `<stdio.h>`
- ライブラリ（生成済み）: `ax620e_bsp_sdk/msp/out/arm64_glibc/lib`
  - 必須リンク: `libax_sys.so`（または `libax_sys.a`）
- 典型的なリンク例（CMake の概念レベル）:
  - インクルード: `target_include_directories(<tgt> PRIVATE ax620e_bsp_sdk/msp/out/arm64_glibc/include)`
  - ライブラリパス: `target_link_directories(<tgt> PRIVATE ax620e_bsp_sdk/msp/out/arm64_glibc/lib)`
  - リンク: `target_link_libraries(<tgt> PRIVATE ax_sys)`

## 4. 実行時要件
- `/dev/ax_sysmap` がデバイス上に存在し、適切な権限でアクセス可能であること。
- `AX_SYS_Init()` を最初に呼び出し、CMM の初期化を済ませること。
- 物理メモリ確保は `AX_SYS_MemAlloc()`（または `AX_SYS_MemAllocCached()`）を使用。
- マッピング解除とメモリ解放（`munmap`, `AX_SYS_MemFree`）を必ず行う。

## 5. 処理フロー（最小）
1. `AX_SYS_Init()` を呼ぶ。
2. `AX_SYS_MemAlloc(&phys0, &virt0, LEN, align, "ax_sysmap_test")` と `AX_SYS_MemAlloc(&phys1, &virt1, LEN, ...)` で確保。
3. `/dev/ax_sysmap` を `O_SYNC` 付き（非キャッシュ）または無し（キャッシュ）で `open`。
4. `mmap(NULL, LEN, PROT_READ|PROT_WRITE, MAP_SHARED, fd, physX)` で物理アドレスをオフセットにマップ。
5. `memcpy`/`memcmp` による整合性確認と反復コピーでスループット測定。
6. `munmap` → `close(fd)` → `AX_SYS_MemFree`（2つ）→ 必要なら `AX_SYS_Deinit()`。

補足: `ax_sys_api.h` には `/dev/ax_sysmap` を直接使わずに行う `AX_SYS_Mmap`/`AX_SYS_MmapCache` などの API もある。移植方針が「元サンプルに忠実」であればデバイスノード＋`mmap` を使用、簡潔化を優先するなら `AX_SYS_Mmap*` を検討できる。

## 6. C++ 移植の注意点
- ヘッダは `extern "C"` ガード済みのため、そのまま `#include <ax_sys_api.h>` が可能。
- RAII による後始末（FD, `mmap` 領域, CMM 確保領域）をクラスやスコープガードで管理すると安全。
- 例外は使わず戻り値で処理する（Google C++ Style）。エラー時は必ずリソース解放経路を確保。
- ログは `printf` ベースで十分（サンプル準拠）。必要なら `AX_SYS_SetLogLevel` も利用可。

## 7. 期待される出力例（原サンプル相当）
- 例:
  - `malloc phy addr: 14141a000, 14261a000`
  - `Test uncached` → `time used: <X.Y>S`
  - `Test cached` → `time used: <x.y>S`
  - `samp sysmap test pass`

## 8. トラブルシューティング
- `open /dev/ax_sysmap fail!`:
  - ドライバ未ロード／デバイス不在、権限不足。デバイスの存在と権限を確認。
- `map fail`（`MAP_FAILED`）:
  - `mmap` に渡すオフセットは「物理アドレス」。アラインメントやサイズが正しいか確認。
  - 必要に応じて `AX_SYS_MemGetBlockInfoByPhy` でブロック情報を確認。
- キャッシュ整合性:
  - キャッシュマップ時は `AX_SYS_MflushCache`/`AX_SYS_MinvalidateCache` の適用を検討。
- パフォーマンス差:
  - `O_SYNC`（非キャッシュ）は遅く、キャッシュは速いのが正常挙動。測定回数/長さは環境に合わせて調整。

## 9. CMake（最小の雛形イメージ）
- 例（コンセプト）:
  - `add_executable(sample_sysmap_cpp src/sample_sysmap.cc)`
  - `target_include_directories(sample_sysmap_cpp PRIVATE ax620e_bsp_sdk/msp/out/arm64_glibc/include)`
  - `target_link_directories(sample_sysmap_cpp PRIVATE ax620e_bsp_sdk/msp/out/arm64_glibc/lib)`
  - `target_link_libraries(sample_sysmap_cpp PRIVATE ax_sys)`

本メモを出発点に、`cpp/` 配下へ C++ 版 sysmap サンプルを最小構成で移植してください。
