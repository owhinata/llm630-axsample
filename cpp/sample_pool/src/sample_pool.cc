// AXERA SDK
#include <ax_sys_api.h>

// C system headers
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// C++ headers
#include <cstdint>

namespace {

constexpr int kCommPools = 3;

class SystemGuard {
 public:
  SystemGuard() : ok_(AX_SYS_Init() == 0) {
    if (!ok_) {
      printf("AX_SYS_Init failed\n");
    }
  }
  ~SystemGuard() {
    if (ok_) AX_SYS_Deinit();
  }
  bool ok() const { return ok_; }

 private:
  bool ok_;
};

class PoolManager {
 public:
  PoolManager() = default;

  bool Reset() {
    AX_S32 ret = AX_POOL_Exit();
    if (ret != 0) {
      printf("AX_POOL_Exit failed: 0x%X\n", static_cast<unsigned int>(ret));
      return false;
    }
    printf("AX_POOL_Exit success\n");
    return true;
  }

  bool SetCommonFloorplan() {
    memset(&floorplan_, 0, sizeof(floorplan_));
    // Three common pools: 1MiB, 2MiB, 3MiB, non-cached, MetaSize=0x2000
    floorplan_.CommPool[0].MetaSize = 0x2000;
    floorplan_.CommPool[0].BlkSize = 1 * 1024 * 1024;
    floorplan_.CommPool[0].BlkCnt = 5;
    floorplan_.CommPool[0].CacheMode = AX_POOL_CACHE_MODE_NONCACHE;
    floorplan_.CommPool[1].MetaSize = 0x2000;
    floorplan_.CommPool[1].BlkSize = 2 * 1024 * 1024;
    floorplan_.CommPool[1].BlkCnt = 5;
    floorplan_.CommPool[1].CacheMode = AX_POOL_CACHE_MODE_NONCACHE;
    floorplan_.CommPool[2].MetaSize = 0x2000;
    floorplan_.CommPool[2].BlkSize = 3 * 1024 * 1024;
    floorplan_.CommPool[2].BlkCnt = 5;
    floorplan_.CommPool[2].CacheMode = AX_POOL_CACHE_MODE_NONCACHE;

    for (int i = 0; i < kCommPools; ++i) {
      memset(floorplan_.CommPool[i].PartitionName, 0,
             sizeof(floorplan_.CommPool[i].PartitionName));
      // Must exist as a CMM partition name
      snprintf(reinterpret_cast<char*>(floorplan_.CommPool[i].PartitionName),
               sizeof(floorplan_.CommPool[i].PartitionName), "%s", "anonymous");
    }

    AX_S32 ret = AX_POOL_SetConfig(&floorplan_);
    if (ret != 0) {
      printf("AX_POOL_SetConfig failed: 0x%X\n",
             static_cast<unsigned int>(ret));
      return false;
    }
    printf("AX_POOL_SetConfig success\n");
    return true;
  }

  bool InitCommon() {
    AX_S32 ret = AX_POOL_Init();
    if (ret != 0) {
      printf("AX_POOL_Init failed: 0x%X\n", static_cast<unsigned int>(ret));
      return false;
    }
    printf("AX_POOL_Init success\n");
    return true;
  }

  AX_POOL CreateUserPool(uint32_t blk_size, uint32_t blk_cnt,
                         AX_POOL_CACHE_MODE_E cache_mode) {
    AX_POOL_CONFIG_T cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.MetaSize = 0x1000;
    cfg.BlkSize = blk_size;
    cfg.BlkCnt = blk_cnt;
    cfg.CacheMode = cache_mode;
    memset(cfg.PartitionName, 0, sizeof(cfg.PartitionName));
    snprintf(reinterpret_cast<char*>(cfg.PartitionName),
             sizeof(cfg.PartitionName), "%s", "anonymous");

    AX_POOL id = AX_POOL_CreatePool(&cfg);
    if (id == AX_INVALID_POOLID) {
      printf("AX_POOL_CreatePool failed (blk=%u)\n", blk_size);
    } else {
      printf("AX_POOL_CreatePool[%d] success\n", id);
    }
    return id;
  }

  bool DestroyPool(AX_POOL id) {
    AX_S32 ret = AX_POOL_DestroyPool(id);
    if (ret != 0) {
      printf("AX_POOL_DestroyPool[%d] failed: 0x%X\n", id,
             static_cast<unsigned int>(ret));
      return false;
    }
    printf("AX_POOL_DestroyPool[%d] success\n", id);
    return true;
  }

 private:
  AX_POOL_FLOORPLAN_T floorplan_{};
};

class BlockGuard {
 public:
  BlockGuard() : blk_(AX_INVALID_BLOCKID) {}
  ~BlockGuard() { Release(); }

  bool Get(AX_POOL pool_id, uint64_t want_size) {
    blk_ = AX_POOL_GetBlock(pool_id, want_size, nullptr);
    if (blk_ == AX_INVALID_BLOCKID) {
      printf("AX_POOL_GetBlock failed (pool=%d, size=%" PRIu64 ")\n", pool_id,
             static_cast<uint64_t>(want_size));
      return false;
    }
    printf("AX_POOL_GetBlock success! BlkId=0x%X\n", blk_);
    return true;
  }

  void Release() {
    if (blk_ != AX_INVALID_BLOCKID) {
      AX_S32 ret = AX_POOL_ReleaseBlock(blk_);
      if (ret != 0) {
        printf("AX_POOL_ReleaseBlock failed: 0x%X\n",
               static_cast<unsigned int>(ret));
      } else {
        printf("AX_POOL_ReleaseBlock success! BlockId=0x%X\n", blk_);
      }
      blk_ = AX_INVALID_BLOCKID;
    }
  }

  AX_POOL PoolId() const { return AX_POOL_Handle2PoolId(blk_); }
  AX_U64 Phys() const { return AX_POOL_Handle2PhysAddr(blk_); }
  AX_U64 MetaPhys() const { return AX_POOL_Handle2MetaPhysAddr(blk_); }
  AX_BLK Handle() const { return blk_; }

 private:
  AX_BLK blk_;
};

class PoolMapping {
 public:
  explicit PoolMapping(AX_POOL pool) : pool_(pool), mapped_(false) {}
  ~PoolMapping() { Unmap(); }

  bool Map() {
    AX_S32 ret = AX_POOL_MmapPool(pool_);
    if (ret != 0) {
      printf("AX_POOL_MmapPool failed: 0x%X\n", static_cast<unsigned int>(ret));
      return false;
    }
    mapped_ = true;
    printf("AX_POOL_MmapPool success\n");
    return true;
  }

  bool Unmap() {
    if (!mapped_) return true;
    AX_S32 ret = AX_POOL_MunmapPool(pool_);
    if (ret != 0) {
      printf("AX_POOL_MunmapPool failed: 0x%X\n",
             static_cast<unsigned int>(ret));
      return false;
    }
    mapped_ = false;
    printf("AX_POOL_MunmapPool success\n");
    return true;
  }

  void* BlockVir(AX_BLK blk) const { return AX_POOL_GetBlockVirAddr(blk); }
  void* MetaVir(AX_BLK blk) const { return AX_POOL_GetMetaVirAddr(blk); }

 private:
  AX_POOL pool_;
  bool mapped_;
};

}  // namespace

int main() {
  SystemGuard sys;
  if (!sys.ok()) return -1;

  printf("sample_pool (C++) begin\n\n");

  PoolManager pm;
  if (!pm.Reset()) return -1;
  if (!pm.SetCommonFloorplan()) return -1;
  if (!pm.InitCommon()) return -1;

  // Create three user pools: 1MiB x2 (noncache), 2MiB x3 (noncache), 3MiB x2
  // (cached)
  AX_POOL user0 =
      pm.CreateUserPool(1 * 1024 * 1024, 2, AX_POOL_CACHE_MODE_NONCACHE);
  if (user0 == AX_INVALID_POOLID) return -1;
  AX_POOL user1 =
      pm.CreateUserPool(2 * 1024 * 1024, 3, AX_POOL_CACHE_MODE_NONCACHE);
  if (user1 == AX_INVALID_POOLID) return -1;
  AX_POOL user2 =
      pm.CreateUserPool(3 * 1024 * 1024, 2, AX_POOL_CACHE_MODE_CACHED);
  if (user2 == AX_INVALID_POOLID) return -1;

  // Get a block from user0
  BlockGuard blk;
  bool ok = blk.Get(user0, 1 * 1024 * 1024);
  if (ok) {
    AX_POOL pool_id = blk.PoolId();
    printf("AX_POOL_Handle2PoolId success!(BlockId:0x%X --> PoolId=%d)\n",
           blk.Handle(), pool_id);

    AX_U64 phys = blk.Phys();
    printf(
        "AX_POOL_Handle2PhysAddr success!(BlockId:0x%X --> PhyAddr=0x%" PRIx64
        ")\n",
        blk.Handle(), static_cast<uint64_t>(phys));
    AX_U64 meta_phys = blk.MetaPhys();
    printf(
        "AX_POOL_Handle2MetaPhysAddr success!(BlockId:0x%X --> "
        "MetaPhyAddr=0x%" PRIx64 ")\n",
        blk.Handle(), static_cast<uint64_t>(meta_phys));

    // Map pool, then get virtual addresses
    PoolMapping mapping(pool_id);
    if (mapping.Map()) {
      void* block_vir = mapping.BlockVir(blk.Handle());
      if (!block_vir) {
        printf("AX_POOL_GetBlockVirAddr failed\n");
      } else {
        printf("AX_POOL_GetBlockVirAddr success! blockVirAddr=0x%" PRIuPTR "\n",
               reinterpret_cast<uintptr_t>(block_vir));
        for (int i = 0; i < 20; ++i) {
          reinterpret_cast<AX_S32*>(block_vir)[i] = i;
        }
        for (int i = 0; i < 20; ++i) {
          printf("%d,", reinterpret_cast<AX_S32*>(block_vir)[i]);
        }
        printf("\n");
      }

      void* meta_vir = mapping.MetaVir(blk.Handle());
      if (!meta_vir) {
        printf("AX_POOL_GetMetaVirAddr failed\n");
      } else {
        printf("AX_POOL_GetMetaVirAddr success! metaVirAddr=0x%" PRIuPTR "\n",
               reinterpret_cast<uintptr_t>(meta_vir));
        for (int i = 0; i < 20; ++i) {
          reinterpret_cast<AX_S32*>(meta_vir)[i] = i * 2;
        }
        for (int i = 0; i < 20; ++i) {
          printf("%d,", reinterpret_cast<AX_S32*>(meta_vir)[i]);
        }
        printf("\n");
      }
      // Release the block before unmapping the pool to avoid BUSY errors.
      blk.Release();
      // mapping dtor will unmap (explicit unmap for clarity)
      mapping.Unmap();
    }
  }

  printf("\nsample_pool (C++) end\n");
  // Block already released above if mapping succeeded
  pm.DestroyPool(user2);
  pm.DestroyPool(user1);
  pm.DestroyPool(user0);
  // Final AX_POOL_Exit to mirror the sample's cleanup
  AX_POOL_Exit();
  return 0;
}
