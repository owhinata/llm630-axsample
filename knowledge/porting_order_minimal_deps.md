# ax620e_bsp_sdk 移植ガイド（依存最小からの実装順）

## 目的
- `ax620e_bsp_sdk/msp/sample` を `cpp/` に段階的に移植する際、
  依存関係が少ない・基本APIの理解に役立つサンプルから着手して開発効率を上げる。

## 基本方針
- まずは AX_SYS / AX_POOL / 周辺の単機能 API を押さえる。
- ISP/VIN/IVPS/VO などマルチモジュール連携が必要な領域は後段へ。
- 各ステップで計測・検証（ログ、時間計測など）を入れて学習効果を最大化。

## 推奨実装順（最小依存 → 実用機能）
1. `pool` → `cpp/sample_pool`
   - ねらい: 共有/専用プール、Blockハンドル、物理/メタ、mmap/unmap を一通り体験。
   - 依存: `ax_sys_api.h`, `ax_pool_type.h`（リンク: `ax_sys`）
   - 参照: `ax620e_bsp_sdk/msp/sample/pool/sample_pool.c`

2. `cmm`（代表ケースの抜粋） → `cpp/sample_cmm`
   - ねらい: `AX_SYS_MemAlloc(Cached)`, `AX_SYS_Mmap(Cache)`, `AX_SYS_MflushCache`/`MinvalidateCache` を確実に理解。
   - 依存: `ax_sys_api.h`（リンク: `ax_sys`）
   - 参照: `ax620e_bsp_sdk/msp/sample/cmm/README.md`, `sample_cmm.c`
   - 提案: 001/002/004/005/006 を最初に対応。

3. `cipher`（TRNG → AES） → `cpp/sample_trng`, `cpp/sample_aes`
   - ねらい: 周辺IPの基本API（乱数・共通鍵暗号）を小さく検証。
   - 依存: `ax_cipher_api.h`, `ax_sys_api.h`（リンク: `ax_cipher`, `ax_sys`）
   - 参照: `ax620e_bsp_sdk/msp/sample/cipher/sample_trng.c`, `sample_aes.c`

4. `gzipd` → `cpp/sample_gzipd`
   - ねらい: 実ファイルI/O + CMM + タイル処理という現実的なワークロードで API を確認。
   - 依存: `ax_gzipd_api.h`, `ax_sys_api.h`（リンク: `ax_gzipd`, `ax_sys`）
   - 参照: `ax620e_bsp_sdk/msp/sample/gzipd/sample_gzipd_test.c`

5. `rtc`（任意） → `cpp/sample_rtc`
   - ねらい: Linux `/dev/rtc` の基本操作（AXライブラリに非依存）。
   - 注意: NTP制御や権限に配慮。ビルド/動作は独立。
   - 参照: `ax620e_bsp_sdk/msp/sample/rtc/sample_rtc.c`

（以降の候補）
- `ivps`, `venc`, `vdec`, `vo` などはモジュール間の連結が前提のため後段に回し、
  最終的に `vin_*` 系へ段階的に進む（`sample_vin` の調査/分割統治に活用）。

## 実装メモ（横断）
- 計時: `std::chrono::steady_clock` を使用し、サイズ/コピー数/秒を併記。
- ログ: 初期化・ハンドル値・物理/仮想アドレス・失敗時の戻り値を明示。
- CMake: `ax620e_bsp_sdk/msp/out/arm64_glibc/{include,lib}` を `include_directories`/`link_directories` し、
  `target_link_libraries` に `ax_sys` 等を指定。
- フォーマット/リンタ: `-DCMAKE_BUILD_TYPE=Contribution` で自動実行（Google Style, cpplint）。

## 参照パス
- サンプル: `ax620e_bsp_sdk/msp/sample/*`
- SDKヘッダ: `ax620e_bsp_sdk/msp/out/arm64_glibc/include`
- SDKライブラリ: `ax620e_bsp_sdk/msp/out/arm64_glibc/lib`

以上の順序で進めることで、最小限の依存から段階的に知識を積み上げ、
後半のマルチモジュール連携サンプルへの移行をスムーズにします。
