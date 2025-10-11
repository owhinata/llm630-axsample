#include <ax_base_type.h>
#include <ax_buffer_tool.h>
#include <ax_global_type.h>
#include <ax_isp_3a_api.h>
#include <ax_isp_api.h>
#include <ax_mipi_rx_api.h>
#include <ax_sensor_struct.h>
#include <ax_sys_api.h>
#include <ax_vin_api.h>
#include <ax_vin_error_code.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr AX_U8 kPipeId = 0;
constexpr AX_U8 kDevId = 0;
constexpr AX_U8 kRxDevId = 0;
constexpr AX_U8 kClockId = 0;
constexpr AX_U8 kI2cAddr = 0x36;  // dummy address; sensor driver may override
constexpr char kSensorLibPath[] = "/opt/lib/libsns_sc850sl.so";
constexpr char kSensorObjectName[] = "gSnssc850slObj";
constexpr char kAiIspBinPath[] = "/opt/etc/sc850sl_sdr_mode3_switch_mode7.bin";

constexpr AX_U32 kMipiDataRate = 1440;  // Mbps
constexpr AX_S32 kSensorWidth = 3840;
constexpr AX_S32 kSensorHeight = 2160;
constexpr AX_S32 kSensorStride = 3840;
constexpr AX_F32 kSensorFrameRate = 20.0F;

constexpr AX_BOOL kDefaultAiIsp = AX_TRUE;

struct PoolConfig {
  AX_U32 width;
  AX_U32 height;
  AX_U32 stride;
  AX_IMG_FORMAT_E format;
  AX_U32 block_count;
  AX_COMPRESS_MODE_E compress_mode;
  AX_U32 compress_level;
};

const PoolConfig kCommonPools[] = {
    {static_cast<AX_U32>(kSensorWidth), static_cast<AX_U32>(kSensorHeight),
     static_cast<AX_U32>(kSensorStride), AX_FORMAT_YUV420_SEMIPLANAR, 3,
     AX_COMPRESS_MODE_LOSSY, 4},
    {1280, 720, 1280, AX_FORMAT_YUV420_SEMIPLANAR, 2, AX_COMPRESS_MODE_NONE, 0},
};

const PoolConfig kPrivatePools[] = {
    {static_cast<AX_U32>(kSensorWidth), static_cast<AX_U32>(kSensorHeight),
     static_cast<AX_U32>(kSensorStride), AX_FORMAT_BAYER_RAW_10BPP_PACKED, 4,
     AX_COMPRESS_MODE_LOSSY, 4},
};

std::atomic<bool> g_keep_running{true};
std::atomic<uint64_t> g_captured_frames{0};

void PrintFrameRate() {
  uint64_t previous_count = 0;
  while (g_keep_running.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    uint64_t current = g_captured_frames.load();
    uint64_t diff = current - previous_count;
    previous_count = current;
    std::printf("[sample_vin] FPS: %" PRIu64 "\n", diff);
  }
}

void SignalHandler(int signo) {
  std::fprintf(stderr, "Caught signal %d, stopping...\n", signo);
  g_keep_running.store(false);
}

class SensorLibrary {
 public:
  SensorLibrary() = default;
  ~SensorLibrary() { Reset(); }

  AX_SENSOR_REGISTER_FUNC_T *Load(const char *path, const char *symbol) {
    Reset();
    handle_ = dlopen(path, RTLD_LAZY);
    if (!handle_) {
      std::fprintf(stderr, "dlopen %s failed: %s\n", path, dlerror());
      return nullptr;
    }
    auto *object =
        reinterpret_cast<AX_SENSOR_REGISTER_FUNC_T *>(dlsym(handle_, symbol));
    if (!object) {
      std::fprintf(stderr, "dlsym %s failed: %s\n", symbol, dlerror());
      Reset();
      return nullptr;
    }
    return object;
  }

 private:
  void Reset() {
    if (handle_) {
      dlclose(handle_);
      handle_ = nullptr;
    }
  }

  void *handle_{nullptr};
};

AX_U32 ComputeBlockSize(const PoolConfig &cfg) {
  AX_FRAME_COMPRESS_INFO_T compress_info{};
  compress_info.enCompressMode = cfg.compress_mode;
  compress_info.u32CompressLevel = cfg.compress_level;
  return AX_VIN_GetImgBufferSize(cfg.height, cfg.stride, cfg.format,
                                 &compress_info, 0);
}

AX_S32 ConfigurePoolFloorplan(const std::vector<PoolConfig> &configs,
                              AX_POOL_FLOORPLAN_T *plan) {
  if (!plan) {
    return -1;
  }
  std::memset(plan, 0, sizeof(*plan));
  AX_U32 index = 0;
  for (const auto &cfg : configs) {
    if (index >= AX_MAX_COMM_POOLS) {
      std::fprintf(stderr, "Exceeded pool configuration capacity\n");
      return -1;
    }
    AX_POOL_CONFIG_T pool_config{};
    pool_config.MetaSize = 4 * 1024;
    pool_config.BlkSize = ComputeBlockSize(cfg);
    pool_config.BlkCnt = cfg.block_count;
    pool_config.CacheMode = AX_POOL_CACHE_MODE_NONCACHE;
    std::snprintf(reinterpret_cast<char *>(pool_config.PartitionName),
                  AX_MAX_PARTITION_NAME_LEN, "anonymous");
    plan->CommPool[index++] = pool_config;
  }
  return 0;
}

AX_S32 InitializeSystem() {
  AX_S32 ret = AX_SYS_Init();
  if (ret != 0) {
    std::fprintf(stderr, "AX_SYS_Init failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_POOL_Exit();
  if (ret != 0) {
    std::fprintf(stderr, "AX_POOL_Exit warning: 0x%x\n", ret);
  }

  AX_POOL_FLOORPLAN_T common_plan{};
  std::vector<PoolConfig> common_cfgs(std::begin(kCommonPools),
                                      std::end(kCommonPools));
  ret = ConfigurePoolFloorplan(common_cfgs, &common_plan);
  if (ret != 0) {
    return ret;
  }

  ret = AX_POOL_SetConfig(&common_plan);
  if (ret != 0) {
    std::fprintf(stderr, "AX_POOL_SetConfig failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_POOL_Init();
  if (ret != 0) {
    std::fprintf(stderr, "AX_POOL_Init failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_VIN_Init();
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_Init failed: 0x%x\n", ret);
    return ret;
  }

  std::vector<PoolConfig> private_cfgs(std::begin(kPrivatePools),
                                       std::end(kPrivatePools));
  AX_POOL_FLOORPLAN_T private_plan{};
  ret = ConfigurePoolFloorplan(private_cfgs, &private_plan);
  if (ret != 0) {
    return ret;
  }

  ret = AX_VIN_SetPoolAttr(&private_plan);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetPoolAttr failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_MIPI_RX_Init();
  if (ret != 0) {
    std::fprintf(stderr, "AX_MIPI_RX_Init failed: 0x%x\n", ret);
    return ret;
  }

  return 0;
}

void ShutdownSystem() {
  AX_MIPI_RX_DeInit();
  AX_VIN_Deinit();
  AX_POOL_Exit();
  AX_SYS_Deinit();
}

AX_S32 SetupMipi() {
  AX_MIPI_RX_DEV_T mipi_dev{};
  mipi_dev.eInputMode = AX_INPUT_MODE_MIPI;
  mipi_dev.tMipiAttr.ePhyMode = AX_MIPI_PHY_TYPE_DPHY;
  mipi_dev.tMipiAttr.eLaneNum = AX_MIPI_DATA_LANE_4;
  mipi_dev.tMipiAttr.nDataRate = kMipiDataRate;
  mipi_dev.tMipiAttr.nDataLaneMap[0] = 0;
  mipi_dev.tMipiAttr.nDataLaneMap[1] = 1;
  mipi_dev.tMipiAttr.nDataLaneMap[2] = 3;
  mipi_dev.tMipiAttr.nDataLaneMap[3] = 4;
  mipi_dev.tMipiAttr.nClkLane[0] = 2;
  mipi_dev.tMipiAttr.nClkLane[1] = 5;

  if (AX_MIPI_DATA_LANE_4 == mipi_dev.tMipiAttr.eLaneNum) {
    AX_MIPI_RX_SetLaneCombo(AX_LANE_COMBO_MODE_0);
  }

  AX_S32 ret = AX_MIPI_RX_SetAttr(kRxDevId, &mipi_dev);
  if (ret != 0) {
    std::fprintf(stderr, "AX_MIPI_RX_SetAttr failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_MIPI_RX_Reset(kRxDevId);
  if (ret != 0) {
    std::fprintf(stderr, "AX_MIPI_RX_Reset failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_MIPI_RX_Start(kRxDevId);
  if (ret != 0) {
    std::fprintf(stderr, "AX_MIPI_RX_Start failed: 0x%x\n", ret);
  }
  return ret;
}

AX_SNS_ATTR_T BuildSensorAttr() {
  AX_SNS_ATTR_T attr{};
  attr.nWidth = kSensorWidth;
  attr.nHeight = kSensorHeight;
  attr.fFrameRate = kSensorFrameRate;
  attr.eSnsMode = AX_SNS_LINEAR_MODE;
  attr.eRawType = AX_RT_RAW10;
  attr.eBayerPattern = AX_BP_RGGB;
  attr.bTestPatternEnable = AX_FALSE;
  return attr;
}

AX_VIN_DEV_ATTR_T BuildDevAttr() {
  AX_VIN_DEV_ATTR_T attr{};
  attr.bImgDataEnable = AX_TRUE;
  attr.bNonImgDataEnable = AX_FALSE;
  attr.eDevMode = AX_VIN_DEV_ONLINE;
  attr.eSnsIntfType = AX_SNS_INTF_TYPE_MIPI_RAW;
  for (AX_U32 i = 0; i < AX_HDR_CHN_NUM; ++i) {
    attr.tDevImgRgn[i] = {0, 0, kSensorWidth, kSensorHeight};
  }
  attr.tMipiIntfAttr.szImgVc[0] = 0;
  attr.tMipiIntfAttr.szImgVc[1] = 1;
  attr.tMipiIntfAttr.szImgDt[0] = 0x2B;
  attr.tMipiIntfAttr.szImgDt[1] = 0x2B;
  attr.tMipiIntfAttr.szInfoVc[0] = 31;
  attr.tMipiIntfAttr.szInfoVc[1] = 31;
  attr.tMipiIntfAttr.szInfoDt[0] = 63;
  attr.tMipiIntfAttr.szInfoDt[1] = 63;
  attr.ePixelFmt = AX_FORMAT_BAYER_RAW_10BPP_PACKED;
  attr.eBayerPattern = AX_BP_RGGB;
  attr.eSnsMode = AX_SNS_LINEAR_MODE;
  attr.eSnsOutputMode = AX_SNS_NORMAL;
  attr.tCompressInfo = {AX_COMPRESS_MODE_NONE, 0};
  attr.tFrameRateCtrl = {AX_INVALID_FRMRATE, AX_INVALID_FRMRATE};
  return attr;
}

AX_VIN_PIPE_ATTR_T BuildPipeAttr(AX_BOOL enable_ai_isp) {
  AX_VIN_PIPE_ATTR_T attr{};
  attr.ePipeWorkMode = AX_VIN_PIPE_NORMAL_MODE1;
  attr.tPipeImgRgn = {0, 0, kSensorWidth, kSensorHeight};
  attr.eBayerPattern = AX_BP_RGGB;
  attr.ePixelFmt = AX_FORMAT_BAYER_RAW_10BPP_PACKED;
  attr.eSnsMode = AX_SNS_LINEAR_MODE;
  attr.tCompressInfo = {AX_COMPRESS_MODE_LOSSY, 4};
  attr.tNrAttr.t3DnrAttr.tCompressInfo = {AX_COMPRESS_MODE_LOSSLESS, 0};
  attr.tNrAttr.tAinrAttr.tCompressInfo = {AX_COMPRESS_MODE_NONE, 0};
  attr.tFrameRateCtrl = {AX_INVALID_FRMRATE, AX_INVALID_FRMRATE};
  attr.bAiIspEnable = enable_ai_isp;
  return attr;
}

AX_VIN_CHN_ATTR_T BuildChannelAttr() {
  AX_VIN_CHN_ATTR_T attr{};
  attr.nWidth = kSensorWidth;
  attr.nHeight = kSensorHeight;
  attr.nWidthStride = kSensorStride;
  attr.eImgFormat = AX_FORMAT_YUV420_SEMIPLANAR;
  attr.nDepth = 1;
  attr.tCompressInfo = {AX_COMPRESS_MODE_LOSSY, 4};
  attr.tFrameRateCtrl = {AX_INVALID_FRMRATE, AX_INVALID_FRMRATE};
  return attr;
}

AX_S8 GetI2cDeviceNode(AX_U8 /*devId*/) {
  // AX620 uses I2C bus 0 for the primary sensor
  return 0;
}

AX_S32 RegisterSensorToIsp(AX_SENSOR_REGISTER_FUNC_T *sensor) {
  if (!sensor) {
    return -1;
  }
  AX_S32 ret = AX_ISP_RegisterSensor(kPipeId, sensor);
  if (ret != 0) {
    std::fprintf(stderr, "AX_ISP_RegisterSensor failed: 0x%x\n", ret);
    return ret;
  }

  if (sensor->pfn_sensor_set_bus_info) {
    AX_SNS_COMMBUS_T bus{};
    bus.I2cDev = GetI2cDeviceNode(kDevId);
    bus.busType = ISP_SNS_CONNECT_I2C_TYPE;
    ret = sensor->pfn_sensor_set_bus_info(kPipeId, bus);
    if (ret != 0) {
      std::fprintf(stderr, "pfn_sensor_set_bus_info failed: 0x%x\n", ret);
      return ret;
    }
  }

  if (sensor->pfn_sensor_set_slaveaddr) {
    ret = sensor->pfn_sensor_set_slaveaddr(kPipeId, kI2cAddr);
    if (ret != 0) {
      std::fprintf(stderr, "pfn_sensor_set_slaveaddr failed: 0x%x\n", ret);
      return ret;
    }
  }

  if (sensor->pfn_sensor_reset) {
    constexpr AX_U32 kResetGpio = 97;
    ret = sensor->pfn_sensor_reset(kPipeId, kResetGpio);
    if (ret != 0) {
      std::fprintf(stderr, "pfn_sensor_reset failed: 0x%x\n", ret);
      return ret;
    }
  }

  return 0;
}

AX_S32 ConfigureVin(const AX_VIN_DEV_ATTR_T &dev_attr,
                    const AX_VIN_PIPE_ATTR_T &pipe_attr,
                    const AX_VIN_CHN_ATTR_T &chn_attr) {
  AX_S32 ret =
      AX_VIN_CreateDev(kDevId, const_cast<AX_VIN_DEV_ATTR_T *>(&dev_attr));
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_CreateDev failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_VIN_SetDevAttr(kDevId, const_cast<AX_VIN_DEV_ATTR_T *>(&dev_attr));
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetDevAttr failed: 0x%x\n", ret);
    return ret;
  }

  AX_VIN_DEV_BIND_PIPE_T bind_pipe{};
  bind_pipe.nNum = 1;
  bind_pipe.nPipeId[0] = kPipeId;
  bind_pipe.nHDRSel[0] = 0x1;

  ret = AX_VIN_SetDevBindPipe(kDevId, &bind_pipe);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetDevBindPipe failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_VIN_SetDevBindMipi(kDevId, kRxDevId);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetDevBindMipi failed: 0x%x\n", ret);
    return ret;
  }

  ret =
      AX_VIN_CreatePipe(kPipeId, const_cast<AX_VIN_PIPE_ATTR_T *>(&pipe_attr));
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_CreatePipe failed: 0x%x\n", ret);
    return ret;
  }

  ret =
      AX_VIN_SetPipeAttr(kPipeId, const_cast<AX_VIN_PIPE_ATTR_T *>(&pipe_attr));
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetPipeAttr failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_VIN_SetPipeFrameSource(kPipeId, AX_VIN_FRAME_SOURCE_ID_IFE,
                                  AX_VIN_FRAME_SOURCE_TYPE_DEV);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetPipeFrameSource failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_VIN_SetPipeFrameSource(kPipeId, AX_VIN_FRAME_SOURCE_ID_YUV,
                                  AX_VIN_FRAME_SOURCE_TYPE_DEV);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetPipeFrameSource (YUV) failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_VIN_SetPipeSourceDepth(kPipeId, AX_VIN_FRAME_SOURCE_ID_IFE, 3);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetPipeSourceDepth (IFE) failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_VIN_SetPipeSourceDepth(kPipeId, AX_VIN_FRAME_SOURCE_ID_YUV, 3);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetPipeSourceDepth failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_VIN_SetChnAttr(kPipeId, AX_VIN_CHN_ID_MAIN,
                          const_cast<AX_VIN_CHN_ATTR_T *>(&chn_attr));
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetChnAttr failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_VIN_EnableChn(kPipeId, AX_VIN_CHN_ID_MAIN);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_EnableChn failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_VIN_SetChnFrameMode(kPipeId, AX_VIN_CHN_ID_MAIN,
                               AX_VIN_FRAME_MODE_RING);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetChnFrameMode failed: 0x%x\n", ret);
    return ret;
  }

  AX_VIN_FRAME_MODE_E frame_mode = AX_VIN_FRAME_MODE_OFF;
  AX_VIN_GetChnFrameMode(kPipeId, AX_VIN_CHN_ID_MAIN, &frame_mode);

  AX_VIN_CHN_ATTR_T confirmed{};
  AX_VIN_GetChnAttr(kPipeId, AX_VIN_CHN_ID_MAIN, &confirmed);
  std::printf(
      "[sample_vin] VIN configured: %ux%u stride %u format %d mode %d\n",
      confirmed.nWidth, confirmed.nHeight, confirmed.nWidthStride,
      confirmed.eImgFormat, frame_mode);

  return 0;
}

AX_S32 InitializeIsp(AX_SENSOR_REGISTER_FUNC_T *sensor,
                     const AX_SNS_ATTR_T &sns_attr, AX_BOOL enable_ai_isp) {
  AX_S32 ret = AX_ISP_Create(kPipeId);
  if (ret != 0) {
    std::fprintf(stderr, "AX_ISP_Create failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_ISP_SetSnsAttr(kPipeId, const_cast<AX_SNS_ATTR_T *>(&sns_attr));
  if (ret != 0) {
    std::fprintf(stderr, "AX_ISP_SetSnsAttr failed: 0x%x\n", ret);
    return ret;
  }

  AX_ISP_AE_REGFUNCS_T ae_funcs{};
  ae_funcs.pfnAe_Init = AX_ISP_ALG_AeInit;
  ae_funcs.pfnAe_Exit = AX_ISP_ALG_AeDeInit;
  ae_funcs.pfnAe_Run = AX_ISP_ALG_AeRun;
  ae_funcs.pfnAe_Ctrl = AX_ISP_ALG_AeCtrl;

  ret = AX_ISP_ALG_AeRegisterSensor(kPipeId, sensor);
  if (ret != 0) {
    std::fprintf(stderr, "AX_ISP_ALG_AeRegisterSensor failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_ISP_RegisterAeLibCallback(kPipeId, &ae_funcs);
  if (ret != 0) {
    std::fprintf(stderr, "AX_ISP_RegisterAeLibCallback failed: 0x%x\n", ret);
    return ret;
  }

  AX_ISP_AWB_REGFUNCS_T awb_funcs{};
  awb_funcs.pfnAwb_Init = AX_ISP_ALG_AwbInit;
  awb_funcs.pfnAwb_Exit = AX_ISP_ALG_AwbDeInit;
  awb_funcs.pfnAwb_Run = AX_ISP_ALG_AwbRun;
  awb_funcs.pfnAwb_Ctrl = AX_ISP_ALG_AwbCtrl;

  ret = AX_ISP_ALG_AwbRegisterSensor(kPipeId, sensor);
  if (ret != 0) {
    std::fprintf(stderr, "AX_ISP_ALG_AwbRegisterSensor failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_ISP_RegisterAwbLibCallback(kPipeId, &awb_funcs);
  if (ret != 0) {
    std::fprintf(stderr, "AX_ISP_RegisterAwbLibCallback failed: 0x%x\n", ret);
    return ret;
  }

  if (enable_ai_isp && std::strcmp(kAiIspBinPath, "null.bin") != 0) {
    ret = AX_ISP_LoadBinParams(kPipeId, kAiIspBinPath);
    if (ret != 0) {
      std::fprintf(stderr, "AX_ISP_LoadBinParams warning: 0x%x\n", ret);
    }
  }

  ret = AX_ISP_Open(kPipeId);
  if (ret != 0) {
    std::fprintf(stderr, "AX_ISP_Open failed: 0x%x\n", ret);
    return ret;
  }

  std::printf("[sample_vin] Sensor SC850SL %dx%d @ %.1ffps, AI ISP: %s\n",
              kSensorWidth, kSensorHeight,
              static_cast<double>(kSensorFrameRate),
              enable_ai_isp ? "enabled" : "disabled");

  return 0;
}

AX_S32 StartStreaming() {
  AX_S32 ret = AX_VIN_StartPipe(kPipeId);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_StartPipe failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_ISP_Start(kPipeId);
  if (ret != 0) {
    std::fprintf(stderr, "AX_ISP_Start failed: 0x%x\n", ret);
    return ret;
  }

  AX_VIN_DEV_ATTR_T dev_attr{};
  AX_VIN_GetDevAttr(kDevId, &dev_attr);
  ret = AX_VIN_EnableDev(kDevId);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_EnableDev failed: 0x%x\n", ret);
    return ret;
  }

  ret = AX_ISP_StreamOn(kPipeId);
  if (ret != 0) {
    std::fprintf(stderr, "AX_ISP_StreamOn failed: 0x%x\n", ret);
  }
  g_captured_frames.store(0);
  return ret;
}

void StopStreaming() {
  AX_ISP_StreamOff(kPipeId);
  AX_VIN_DisableDev(kDevId);
  AX_ISP_Stop(kPipeId);
  AX_VIN_StopPipe(kPipeId);
  AX_VIN_DisableChn(kPipeId, AX_VIN_CHN_ID_MAIN);

  AX_ISP_Close(kPipeId);

  AX_ISP_UnRegisterAwbLibCallback(kPipeId);
  AX_ISP_ALG_AwbUnRegisterSensor(kPipeId);
  AX_ISP_UnRegisterAeLibCallback(kPipeId);
  AX_ISP_ALG_AeUnRegisterSensor(kPipeId);

  AX_ISP_UnRegisterSensor(kPipeId);
  AX_ISP_CloseSnsClk(kClockId);

  AX_ISP_Destroy(kPipeId);
  AX_VIN_DestroyPipe(kPipeId);
  AX_VIN_DestroyDev(kDevId);
  AX_MIPI_RX_Stop(kRxDevId);
}

struct CommandLineOptions {
  AX_BOOL enable_ai_isp = kDefaultAiIsp;
};

CommandLineOptions ParseOptions(int argc, char *argv[]) {
  CommandLineOptions opts;
  int opt = 0;
  while ((opt = getopt(argc, argv, "a:h")) != -1) {
    switch (opt) {
      case 'a':
        opts.enable_ai_isp = (std::atoi(optarg) != 0) ? AX_TRUE : AX_FALSE;
        break;
      case 'h':
      default:
        std::printf("Usage: %s [-a enable_ai_isp]\n", argv[0]);
        std::exit(opt == 'h' ? 0 : -1);
    }
  }
  return opts;
}

}  // namespace

int main(int argc, char *argv[]) {
  const CommandLineOptions options = ParseOptions(argc, argv);
  const AX_BOOL enable_ai_isp = options.enable_ai_isp;

  struct sigaction sa;
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  g_keep_running.store(true);
  g_captured_frames.store(0);

  AX_S32 ret = 0;
  bool system_initialized = false;
  bool streaming_started = false;
  bool sensor_clock_opened = false;
  bool sensor_registered = false;
  bool vin_configured = false;
  bool isp_created = false;
  bool mipi_started = false;

  SensorLibrary sensor_library;
  AX_SENSOR_REGISTER_FUNC_T *sensor = nullptr;
  std::thread fps_thread;

  do {
    ret = InitializeSystem();
    if (ret != 0) {
      break;
    }
    system_initialized = true;

    ret = SetupMipi();
    if (ret != 0) {
      break;
    }
    mipi_started = true;

    sensor = sensor_library.Load(kSensorLibPath, kSensorObjectName);
    if (!sensor) {
      ret = -1;
      break;
    }

    ret = AX_ISP_OpenSnsClk(kClockId, AX_SNS_CLK_24M);
    if (ret != 0) {
      std::fprintf(stderr, "AX_ISP_OpenSnsClk failed: 0x%x\n", ret);
      break;
    }
    sensor_clock_opened = true;

    ret = RegisterSensorToIsp(sensor);
    if (ret != 0) {
      break;
    }
    sensor_registered = true;

    AX_SNS_ATTR_T sensor_attr = BuildSensorAttr();
    if (sensor->pfn_sensor_set_mode) {
      AX_S32 mode_ret = sensor->pfn_sensor_set_mode(kPipeId, &sensor_attr);
      if (mode_ret != 0) {
        std::fprintf(stderr, "sensor_set_mode failed: 0x%x\n", mode_ret);
        ret = mode_ret;
        break;
      }
    }
    if (sensor->pfn_sensor_init) {
      sensor->pfn_sensor_init(kPipeId);
    }

    AX_VIN_DEV_ATTR_T dev_attr = BuildDevAttr();
    AX_VIN_PIPE_ATTR_T pipe_attr = BuildPipeAttr(enable_ai_isp);
    AX_VIN_CHN_ATTR_T chn_attr = BuildChannelAttr();

    ret = ConfigureVin(dev_attr, pipe_attr, chn_attr);
    if (ret != 0) {
      break;
    }
    vin_configured = true;

    ret = InitializeIsp(sensor, sensor_attr, enable_ai_isp);
    if (ret != 0) {
      break;
    }
    isp_created = true;

    ret = StartStreaming();
    if (ret != 0) {
      break;
    }
    streaming_started = true;
    sensor_registered = false;  // StopStreaming handles sensor unregister.

    if (sensor->pfn_sensor_streaming_ctrl) {
      AX_S32 stream_ret = sensor->pfn_sensor_streaming_ctrl(kPipeId, AX_TRUE);
      if (stream_ret != 0) {
        std::fprintf(stderr, "sensor_streaming_ctrl start failed: 0x%x\n",
                     stream_ret);
      }
    }

    fps_thread = std::thread(PrintFrameRate);
    std::printf("sample_vin (sc850sl) running. Press Ctrl+C to stop.\\n");

    bool first_frame_logged = false;
    uint32_t empty_count = 0;
    while (g_keep_running.load()) {
      AX_IMG_INFO_T frame{};
      AX_S32 frame_ret =
          AX_VIN_GetYuvFrame(kPipeId, AX_VIN_CHN_ID_MAIN, &frame, 1000);
      if (frame_ret == 0) {
        uint64_t frame_index =
            g_captured_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        const AX_VIDEO_FRAME_T &vf = frame.tFrameInfo.stVFrame;
        if (!first_frame_logged || (frame_index % 60U) == 0U) {
          std::printf("[sample_vin] Frame #%" PRIu64 " seq %" PRIu64
                      " size %ux%u stride %u pts %" PRIu64 "\\n",
                      frame_index, vf.u64SeqNum, vf.u32Width, vf.u32Height,
                      vf.u32PicStride[0], vf.u64PTS);
          first_frame_logged = true;
        }
        empty_count = 0;
        AX_VIN_ReleaseYuvFrame(kPipeId, AX_VIN_CHN_ID_MAIN, &frame);
      } else if (frame_ret == AX_ERR_VIN_RES_EMPTY) {
        if (++empty_count % 30 == 0) {
          std::printf("[sample_vin] waiting for frames... %u empty polls\n",
                      empty_count);
        }
        continue;
      } else {
        std::fprintf(stderr, "AX_VIN_GetYuvFrame failed: 0x%x\n", frame_ret);
        ret = frame_ret;
        break;
      }
    }
  } while (false);

  g_keep_running.store(false);
  if (fps_thread.joinable()) {
    fps_thread.join();
  }

  if (streaming_started && sensor && sensor->pfn_sensor_streaming_ctrl) {
    sensor->pfn_sensor_streaming_ctrl(kPipeId, AX_FALSE);
  }

  if (streaming_started || isp_created || vin_configured) {
    StopStreaming();
    streaming_started = false;
    isp_created = false;
    vin_configured = false;
    sensor_registered = false;
    sensor_clock_opened = false;
    mipi_started = false;
  }

  if (sensor_registered) {
    AX_ISP_UnRegisterSensor(kPipeId);
    sensor_registered = false;
  }

  if (sensor_clock_opened) {
    AX_ISP_CloseSnsClk(kClockId);
    sensor_clock_opened = false;
  }

  if (mipi_started) {
    AX_MIPI_RX_Stop(kRxDevId);
    mipi_started = false;
  }

  if (system_initialized) {
    ShutdownSystem();
  }

  if (ret == 0) {
    std::printf("sample_vin stopped.\n");
  } else {
    std::fprintf(stderr, "sample_vin exited with error 0x%x\n", ret);
  }
  return ret;
}
