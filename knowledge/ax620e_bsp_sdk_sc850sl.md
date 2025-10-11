# ax620e_bsp_sdk 調査メモ（SC850SL / VIN 開発向け）

## 1. 生成済み SDK 資産の場所
- ヘッダ: `ax620e_bsp_sdk/msp/out/arm64_glibc/include`
  - 中心となるヘッダ
    - `ax_sys_api.h`, `ax_pool_type.h`: システム初期化・共通プール設定。
    - `ax_buffer_tool.h`: 画像バッファサイズ算出ヘルパー (`AX_VIN_GetImgBufferSize`)。
    - `ax_mipi_rx_api.h`: MIPI RX 設定・制御。
    - `ax_vin_api.h`: VIN デバイス / パイプ / チャンネルの生成・開始停止 API。
    - `ax_sensor_struct.h`: `AX_SENSOR_REGISTER_FUNC_T` をはじめとするセンサドライバ関数群の型定義。
    - `ax_isp_api.h`, `ax_isp_3a_api.h`: ISP パイプ制御や 3A ライブラリ登録。
    - `ax_nt_ctrl_api.h`, `ax_nt_stream_api.h`: ネットチューニング（必要に応じて）。
    - `ax_engine_api.h`, `ax_interpreter.so` など NPU/Engine 周辺ヘッダ。今回の VIN 単体動作では主に初期化のみ利用。
  - その他のヘッダも `ax_***.h` が揃っているため、必要に応じて追跡できる。

- ライブラリ: `ax620e_bsp_sdk/msp/out/arm64_glibc/lib`
  - システム・周辺モジュール:
    - `libax_sys.so`, `libax_proton.so`, `libax_engine.so`, `libax_interpreter.so`, `libax_mipi.so`, `libax_ivps.so`, `libax_venc.so`, `libax_nt_stream.so`, `libax_nt_ctrl.so` など。
    - VIN 系の専用ライブラリは分離されておらず、上記 lib 群をリンクすることで `AX_VIN_*` API を利用可能。
  - センサドライバ:
    - `libsns_sc850sl.so`（および `.a`）。`dlsym` で得るオブジェクト名は `gSnssc850slObj`。
    - 他センサも同様に `libsns_*.so` の形で配置。

## 2. センサ関連リソース
- センサクラス実装: `ax620e_bsp_sdk/app/component/sensor/SC850SL.cpp`
  - `libsns_sc850sl.so` をロードし、`gSnssc850slObj` を参照している。API 呼び出し時のオブジェクト名確認に有用。
- 設定ファイル群:
  - `ax620e_bsp_sdk/app/component/resource/lite/ipc/sensor/sc850sl.json`
    - レーン数、クロック、HDR 関連のデフォルト設定、AI ISP 有効化フラグ、チャンネル解像度などが記載。
    - 必要な `bin` ファイル（例: `/opt/etc/sc850sl_sdr_mode3_switch_mode7.bin`）のパスもコメント付きで示されている。
  - `ax620e_bsp_sdk/app/component/resource/lite/ipc/pool.ini` の `#sc850sl single` セクション
    - プールのブロックサイズ・カウント例がまとまっており、VIN 用メモリプランの目安に利用可能。
  - `ax620e_bsp_sdk/build/projects/AX620Q_nand_arm32_k419/sensors/sc850sl/model.filelist`
    - デバイスにデプロイする際に必要なチューニング bin / ini ファイルの一覧。

## 3. 初期化・制御フローで参照する API の要点
1. **システム / プール**
   - `AX_SYS_Init` → `AX_POOL_Exit` → `AX_POOL_SetConfig` → `AX_POOL_Init`
   - プール構成は `AX_POOL_FLOORPLAN_T` (`ax_pool_type.h`) と `AX_VIN_GetImgBufferSize` を使って決める。
   - VIN 固有のプール設定は `AX_VIN_SetPoolAttr` で適用。

2. **MIPI RX**
   - `AX_MIPI_RX_Init` → `AX_MIPI_RX_SetAttr` → `AX_MIPI_RX_Reset` → `AX_MIPI_RX_Start`
   - レーンマッピングやデータレートは `SC850SL.cpp` の実装・`sc850sl.json` の値を参考にする。

3. **VIN**
   - `AX_VIN_CreateDev` → `AX_VIN_SetDevAttr` → `AX_VIN_SetDevBindPipe` / `AX_VIN_SetDevBindMipi`
   - `AX_VIN_CreatePipe` → `AX_VIN_SetPipeAttr` → `AX_VIN_SetPipeFrameSource`
   - `AX_VIN_SetChnAttr` → `AX_VIN_EnableChn`
   - ストリーミング開始時に `AX_VIN_StartPipe`, 停止時に `AX_VIN_StopPipe`, `AX_VIN_DestroyPipe/Dev`

4. **ISP / 3A**
   - センサドライバ登録: `AX_ISP_RegisterSensor`（`AX_SENSOR_REGISTER_FUNC_T` / `pfn_sensor_*`）
   - クロック: `AX_ISP_OpenSnsClk`
   - `AX_ISP_Create` → `AX_ISP_SetSnsAttr` → 3A 登録 (`AX_ISP_ALG_AeRegisterSensor`, `AX_ISP_RegisterAeLibCallback`, 同様に AWB)
   - AI ISP バイナリ適用: `AX_ISP_LoadBinParams`
   - ストリーミング: `AX_ISP_Start` / `AX_ISP_StreamOn`
   - 終了処理では `*_UnRegister*`, `AX_ISP_StreamOff`, `AX_ISP_CloseSnsClk`, `AX_ISP_Destroy`

## 4. ユースケース別補足
- **ダイナミックロード**: `libsns_sc850sl.so` は `/opt/lib` を想定。`dlopen` する際は環境変数 `LD_LIBRARY_PATH` などを調整する。
- **BIN ファイル配置**: `sc850sl.json` や `model.filelist` に記載の `/opt/etc/*.bin` が実機で必要。rsync 時に含める。
- **I2C バス**: `SC850SL.cpp` では 4lane, bus 0 前提。VIN サンプル実装でも `COMMON_ISP_GetI2cDevNode` 相当の処理で bus=0 を使う。
- **AI ISP 切替**: `sc850sl.json` の `enable_aiisp` と `sample_vin` の `AX_VIN_PIPE_ATTR_T::bAiIspEnable` を合わせる。AI ISP を無効にする場合は bin のロードもスキップ可。

## 5. 参考になるソース
- `ax620e_bsp_sdk/app/component/sensor/SC850SL.cpp`: センサ初期化順序・MIPI 設定の具体例。
- `ax620e_bsp_sdk/app/component/resource/lite/ipc/sensor/sc850sl.json`: JSON ベース設定。
- `ax620e_bsp_sdk/app/component/resource/lite/ipc/pool.ini`: 各センサ用のプールサイズ例。
- `ax620e_bsp_sdk/msp/sample/vin/sample_vin.c`: 複数センサ対応の C 実装。直接は参照しないが、エラー処理や 3A 登録の流れを確認する際に役立つ。

以上を踏まえて、`cpp/sample_vin` などの C++ 実装時は本メモを起点にヘッダ / ライブラリ / リソースを辿ると効率的です。
