// Capture RAW Bayer frames from SC850SL without enabling YUV channel.
// Method 2: Disable YUV output, capture RAW only from IFE dump.

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
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
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

// Default AI-ISP disabled for RAW capture only.
constexpr AX_BOOL kDefaultAiIsp = AX_FALSE;

struct PoolConfig {
  AX_U32 width;
  AX_U32 height;
  AX_U32 stride;
  AX_IMG_FORMAT_E format;
  AX_U32 block_count;
  AX_COMPRESS_MODE_E compress_mode;
  AX_U32 compress_level;
};

// Common pool configuration
// - Add a RAW10 pool matching SC850SL (3840x2160, stride 3840) so the IFE dump
//   node can allocate buffers for AX_VIN_GetRawFrame(). The IFE RAW dump path
//   relies on common pools, as in the BSP SDK samples.
// - Keep a dummy entry (0 blocks) to illustrate YUV is unused in this sample.
//   ConfigurePoolFloorplan() skips entries with block_count == 0.
const PoolConfig kCommonPools[] = {
    {static_cast<AX_U32>(kSensorWidth), static_cast<AX_U32>(kSensorHeight),
     static_cast<AX_U32>(kSensorStride), AX_FORMAT_BAYER_RAW_10BPP_PACKED, 8,
     AX_COMPRESS_MODE_NONE, 0},
    {0, 0, 0, AX_FORMAT_YUV420_SEMIPLANAR, 0, AX_COMPRESS_MODE_NONE, 0},
};

// Private RAW pool for IFE dump frames.
const PoolConfig kPrivatePools[] = {
    {static_cast<AX_U32>(kSensorWidth), static_cast<AX_U32>(kSensorHeight),
     static_cast<AX_U32>(kSensorStride), AX_FORMAT_BAYER_RAW_10BPP_PACKED, 4,
     AX_COMPRESS_MODE_LOSSY, 4},
};

std::atomic<bool> g_keep_running{true};
std::atomic<uint64_t> g_captured_frames{0};

// Save-to-stdout mode controls: when enabled, all diagnostics go to stderr and
// the main loop writes RAW frame bytes to stdout, then exits after N frames.
std::atomic<bool> g_save_frames_mode{false};
std::atomic<uint32_t> g_save_frames_remaining{0};
static std::atomic<uint32_t> g_skip_frames_count{30};

void InfoOut(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  FILE *out = g_save_frames_mode.load() ? stderr : stdout;
  vfprintf(out, fmt, args);
  va_end(args);
}

void PrintFrameRate() {
  uint64_t previous_count = 0;
  while (g_keep_running.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    uint64_t current = g_captured_frames.load();
    uint64_t diff = current - previous_count;
    previous_count = current;
    InfoOut("[sample_vin_raw] FPS: %" PRIu64 "\n", diff);
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
    if (!path || !symbol) {
      return nullptr;
    }
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
    // Skip pools explicitly configured with zero blocks.
    if (cfg.block_count == 0) {
      continue;
    }
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

  ret = AX_ISP_OpenSnsClk(kClockId, AX_SNS_CLK_24M);
  if (ret != 0) {
    std::fprintf(stderr, "AX_ISP_OpenSnsClk failed: 0x%x\n", ret);
    return ret;
  }
  return 0;
}

AX_S32 ConfigureVin(const AX_VIN_DEV_ATTR_T &dev_attr,
                    const AX_VIN_PIPE_ATTR_T &pipe_attr) {
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

  // IFE frame source configuration kept.
  ret = AX_VIN_SetPipeFrameSource(kPipeId, AX_VIN_FRAME_SOURCE_ID_IFE,
                                  AX_VIN_FRAME_SOURCE_TYPE_DEV);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetPipeFrameSource (IFE) failed: 0x%x\n", ret);
    return ret;
  }

  // Enable IFE dump node for RAW frame capture. AX_VIN_GetRawFrame reads
  // frames from the IFE dump path, which is disabled by default and must be
  // explicitly enabled per BSP SDK samples.
  AX_VIN_DUMP_ATTR_T dump_attr{};
  dump_attr.bEnable = AX_TRUE;
  dump_attr.nDepth = 3;  // Buffer depth for dump queue
  ret = AX_VIN_SetPipeDumpAttr(kPipeId, AX_VIN_PIPE_DUMP_NODE_IFE,
                               AX_VIN_DUMP_QUEUE_TYPE_DEV, &dump_attr);
  if (ret != AX_SUCCESS) {
    std::fprintf(stderr, "AX_VIN_SetPipeDumpAttr (IFE) failed: 0x%x\n", ret);
    return ret;
  }

  // Set IFE source depth to allow RAW frame queueing.
  ret = AX_VIN_SetPipeSourceDepth(kPipeId, AX_VIN_FRAME_SOURCE_ID_IFE, 3);
  if (ret != 0) {
    std::fprintf(stderr, "AX_VIN_SetPipeSourceDepth (IFE) failed: 0x%x\n", ret);
    return ret;
  }

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

  InfoOut("[sample_vin_raw] Sensor SC850SL %dx%d @ %.1ffps, AI ISP: %s\n",
          kSensorWidth, kSensorHeight, static_cast<double>(kSensorFrameRate),
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
  uint32_t save_frames = 0;  // When > 0, write N RAW frames to stdout and exit.
};

CommandLineOptions ParseOptions(int argc, char *argv[]) {
  CommandLineOptions opts;

  // Manual pre-scan for --save-frames N
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--save-frames") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr,
                     "Missing argument for --save-frames (expected N)\n");
        std::exit(-1);
      }
      int64_t n = std::strtol(argv[i + 1], nullptr, 10);
      if (n <= 0) {
        std::fprintf(stderr,
                     "Invalid value for --save-frames: %s (must be > 0)\n",
                     argv[i + 1]);
        std::exit(-1);
      }
      opts.save_frames = static_cast<uint32_t>(n);
      // Skip the value so getopt doesn't see it.
      argv[i][0] = '\0';
      argv[i + 1][0] = '\0';
      ++i;
    } else if (std::strcmp(argv[i], "--skip-frames") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: --skip-frames requires a number\n");
        std::exit(-1);
      }
      int64_t n = std::strtol(argv[i + 1], nullptr, 10);
      if (n < 0) {
        std::fprintf(stderr, "Error: --skip-frames must be >= 0\n");
        std::exit(-1);
      }
      g_skip_frames_count.store(static_cast<uint32_t>(n));
      argv[i][0] = '\0';
      argv[i + 1][0] = '\0';
      ++i;
    }
  }

  // Short options parsing
  int c = 0;
  optind = 1;  // Reset in case of repeated invocations.
  while ((c = getopt(argc, argv, "a:h")) != -1) {
    switch (c) {
      case 'a':
        opts.enable_ai_isp = (std::atoi(optarg) != 0) ? AX_TRUE : AX_FALSE;
        break;
      case 'h':
      default: {
        std::fprintf(
            stderr,
            "Usage: %s [-a enable_ai_isp] [--save-frames N] [--skip-frames N]\n"
            "\n"
            "Options:\n"
            "  -a 0|1           Enable AI ISP (default %d)\n"
            "  -h               Show this help\n"
            "  --save-frames N  Save N RAW frames to stdout and exit\n"
            "  --skip-frames N  Skip first N frames before saving (default: "
            "30)\n",
            argv[0], kDefaultAiIsp ? 1 : 0);
        std::exit(c == 'h' ? 0 : -1);
      }
    }
  }
  return opts;
}

}  // namespace

int main(int argc, char *argv[]) {
  const CommandLineOptions options = ParseOptions(argc, argv);
  const uint32_t save_frames_count = options.save_frames;
  const AX_BOOL enable_ai_isp = options.enable_ai_isp;
  if (save_frames_count > 0) {
    g_save_frames_mode.store(true);
    g_save_frames_remaining.store(save_frames_count);
  } else {
    g_save_frames_mode.store(false);
    g_save_frames_remaining.store(0);
  }

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
  int stdout_backup = -1;
  bool stdout_redirected = false;
  auto restore_stdout = [&]() {
    if (!stdout_redirected || stdout_backup < 0) {
      return;
    }
    if (fflush(stdout) != 0) {
      std::fprintf(stderr, "fflush failed when restoring stdout: %s\n",
                   std::strerror(errno));
    }
    if (dup2(stdout_backup, STDOUT_FILENO) < 0) {
      std::fprintf(stderr, "dup2 restore stdout failed: %s\n",
                   std::strerror(errno));
    }
    close(stdout_backup);
    stdout_backup = -1;
    stdout_redirected = false;
  };

  do {
    ret = InitializeSystem();
    if (ret != 0) {
      break;
    }
    system_initialized = true;

    // Initialize and start MIPI RX before sensor registration, matching
    // sample_vin.
    ret = SetupMipi();
    if (ret != 0) {
      break;
    }
    mipi_started = true;

    sensor = sensor_library.Load(kSensorLibPath, kSensorObjectName);
    if (!sensor) {
      std::fprintf(stderr, "Failed to load sensor lib: %s (%s)\n",
                   kSensorLibPath, kSensorObjectName);
      ret = -1;
      break;
    }

    ret = RegisterSensorToIsp(sensor);
    if (ret != 0) {
      break;
    }
    sensor_registered = true;
    sensor_clock_opened = true;

    AX_SNS_ATTR_T sensor_attr = BuildSensorAttr();
    if (sensor->pfn_sensor_set_mode) {
      AX_S32 mode_ret = sensor->pfn_sensor_set_mode(kPipeId, &sensor_attr);
      if (mode_ret != 0) {
        std::fprintf(stderr, "sensor_set_mode failed: 0x%x\n", mode_ret);
        ret = mode_ret;
        break;
      }
    }
    if (save_frames_count > 0 && !stdout_redirected) {
      if (fflush(stdout) != 0) {
        std::fprintf(stderr, "fflush failed before sensor init: %s\n",
                     std::strerror(errno));
      }
      stdout_backup = dup(STDOUT_FILENO);
      if (stdout_backup < 0) {
        std::fprintf(stderr, "dup stdout failed: %s\n", std::strerror(errno));
      } else {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull < 0) {
          std::fprintf(stderr, "open /dev/null failed: %s\n",
                       std::strerror(errno));
          close(stdout_backup);
          stdout_backup = -1;
        } else {
          if (dup2(devnull, STDOUT_FILENO) < 0) {
            std::fprintf(stderr, "dup2 /dev/null failed: %s\n",
                         std::strerror(errno));
            close(stdout_backup);
            stdout_backup = -1;
          } else {
            stdout_redirected = true;
          }
          close(devnull);
        }
      }
    }
    if (sensor->pfn_sensor_init) {
      sensor->pfn_sensor_init(kPipeId);
    }

    AX_VIN_DEV_ATTR_T dev_attr = BuildDevAttr();
    AX_VIN_PIPE_ATTR_T pipe_attr = BuildPipeAttr(enable_ai_isp);

    ret = ConfigureVin(dev_attr, pipe_attr);
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
    restore_stdout();

    if (!g_save_frames_mode.load()) {
      fps_thread = std::thread(PrintFrameRate);
    }
    InfoOut("sample_vin_raw (sc850sl) running. Press Ctrl+C to stop.\n");

    bool first_frame_logged = false;
    uint32_t empty_count = 0;
    while (g_keep_running.load()) {
      AX_IMG_INFO_T frame{};
      AX_S32 frame_ret = AX_VIN_GetRawFrame(kPipeId, AX_VIN_PIPE_DUMP_NODE_IFE,
                                            AX_SNS_HDR_FRAME_L, &frame, 1000);
      if (frame_ret == 0) {
        uint64_t frame_index =
            g_captured_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        const AX_VIDEO_FRAME_T &vf = frame.tFrameInfo.stVFrame;

        // If save mode is enabled, write RAW bytes to stdout and exit after N.
        if (g_save_frames_mode.load()) {
          // Skip initial frames for AE stabilization
          if (g_skip_frames_count.load() > 0) {
            g_skip_frames_count.fetch_sub(1);
            AX_VIN_ReleaseRawFrame(kPipeId, AX_VIN_PIPE_DUMP_NODE_IFE,
                                   AX_SNS_HDR_FRAME_L, &frame);
            continue;
          }

          const uint32_t stride = vf.u32PicStride[0];
          const uint32_t height = vf.u32Height;
          // RAW10 packed size per frame: stride * height * 10 / 8.
          const uint64_t size64 = static_cast<uint64_t>(stride) *
                                  static_cast<uint64_t>(height) * 10ULL / 8ULL;
          const uint32_t size_bytes = static_cast<uint32_t>(size64);

          void *vir = AX_SYS_Mmap(vf.u64PhyAddr[0], size_bytes);
          if (vir == nullptr) {
            std::fprintf(stderr,
                         "AX_SYS_Mmap failed for frame #%" PRIu64
                         " phys=0x%" PRIx64 " size=%u\n",
                         frame_index, static_cast<uint64_t>(vf.u64PhyAddr[0]),
                         size_bytes);
            AX_VIN_ReleaseRawFrame(kPipeId, AX_VIN_PIPE_DUMP_NODE_IFE,
                                   AX_SNS_HDR_FRAME_L, &frame);
            ret = -1;
            break;
          }

          size_t wrote = std::fwrite(vir, 1, size_bytes, stdout);
          std::fflush(stdout);
          AX_SYS_Munmap(vir, size_bytes);

          if (wrote != size_bytes) {
            std::fprintf(stderr,
                         "fwrite wrote %zu of %u bytes (frame #%" PRIu64 ")\n",
                         wrote, size_bytes, frame_index);
            AX_VIN_ReleaseRawFrame(kPipeId, AX_VIN_PIPE_DUMP_NODE_IFE,
                                   AX_SNS_HDR_FRAME_L, &frame);
            ret = -1;
            break;
          }

          AX_VIN_ReleaseRawFrame(kPipeId, AX_VIN_PIPE_DUMP_NODE_IFE,
                                 AX_SNS_HDR_FRAME_L, &frame);

          // Countdown and stop after N frames.
          uint32_t remaining = g_save_frames_remaining.fetch_sub(1) - 1;
          if (remaining == 0) {
            g_keep_running.store(false);
            break;
          }
          continue;
        }

        // Normal mode: periodic log to info output.
        if (!first_frame_logged || (frame_index % 60U) == 0U) {
          InfoOut("[sample_vin_raw] Frame #%" PRIu64 " seq %" PRIu64
                  " size %ux%u stride %u fmt %d pts %" PRIu64 "\n",
                  frame_index, static_cast<uint64_t>(vf.u64SeqNum), vf.u32Width,
                  vf.u32Height, vf.u32PicStride[0],
                  static_cast<int>(vf.enImgFormat),
                  static_cast<uint64_t>(vf.u64PTS));
          first_frame_logged = true;
        }

        empty_count = 0;
        AX_VIN_ReleaseRawFrame(kPipeId, AX_VIN_PIPE_DUMP_NODE_IFE,
                               AX_SNS_HDR_FRAME_L, &frame);
      } else if (frame_ret == AX_ERR_VIN_RES_EMPTY) {
        if (++empty_count % 30 == 0) {
          InfoOut("[sample_vin_raw] waiting for frames... %u empty polls\n",
                  empty_count);
        }
        continue;
      } else {
        std::fprintf(stderr, "AX_VIN_GetRawFrame failed: 0x%x\n", frame_ret);
        ret = frame_ret;
        break;
      }
    }
  } while (false);

  g_keep_running.store(false);
  restore_stdout();
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
    InfoOut("sample_vin_raw stopped.\n");
  } else {
    std::fprintf(stderr, "sample_vin_raw exited with error 0x%x\n", ret);
  }
  return ret;
}
