#ifndef SHM_OBJECT_HANDLE_H
#define SHM_OBJECT_HANDLE_H

#include <random>
#include <system_error>
#include <tuple>

#include "common/config.h"
#include "common/constants.h"
#include "common/expected.h"
#include "common/helper.h"
#include "common/thread_local.h"
#include "core/c3.h"
#include "core/object_slot.h"
#include "index/lcd.h"
#include "ipc/tigon_client.hpp"
#include "ipc/tigon_server.hpp"
#include "shared_memory/allocator.h"
#include "shared_memory/region.h"

namespace rackobj::common {

template <typename Policy>
class EvictManager;

template <typename Policy>
class ReplicateManager;

class FlushManagerHandle;

}  // namespace rackobj::common

namespace rackobj::lib {

using common::C3POHandle;

template <typename Policy>
class SharedMemoryObjectHandle {
    using FetchBlockResult = expected<size_t, PosixError>;
    using FetchBlockFn = std::function<FetchBlockResult(const common::BlockId&, void*)>;
    friend class TigonIPCServer;

public:
    typedef enum status { owned_hit = 0, owned_miss = 1, remote_hit = 2, remote_miss = 3 } status_e;

    SharedMemoryObjectHandle(const int node, const RackOBJConfig cfg);

    common::LocalMemoryObject<Policy>* RegisterLocalMem();

    ~SharedMemoryObjectHandle();

    size_t Read(const common::BlockId& block_id, uint8_t* ptr, size_t count, size_t offset_into_page,
                ThreadLocalMeta<Policy>* local_meta);

    size_t Get(const common::BlockId& block_id, uint8_t* ptr, size_t count, size_t offset_into_page,
               ThreadLocalMeta<Policy>* local_meta);

    expected<common::CacheNode*, std::error_code> CreatePage(const common::BlockId& block_id, const uint8_t* write_data,
                                                             size_t count, ThreadLocalMeta<Policy>* local_meta);

    expected<common::CacheNode*, std::error_code> PreloadAdmit(const common::BlockId& block_id,
                                                               const uint8_t* write_data, size_t count,
                                                               ThreadLocalMeta<Policy>* local_meta);

    /**
     * collapse replicas, switch page to rw shared
     * return:
     *   errc::not_enough_memory: no memory to allocate wmeta
     *   errc::operation_not_permitted: no gcd entry
     */
    std::error_code SwitchRW(const common::WriteHandle& wh, ThreadLocalMeta<Policy>* local_meta);

    size_t Write(const common::BlockId& block_id, const uint8_t* ptr, size_t count, size_t offset_into_page,
                 ThreadLocalMeta<Policy>* local_meta);

    size_t Put(const common::BlockId& block_id, const uint8_t* ptr, size_t count, size_t offset_into_page,
               ThreadLocalMeta<Policy>* local_meta);

    // for benchmark use
    void SetWmetaWaterMark(size_t wmeta_water_mark) { c3po_->SetWmetaWaterMark(wmeta_water_mark); }

    void ClearMovementCounter() {
        memset(partition_to_shared_, 0, sizeof(partition_to_shared_));
        memset(shared_to_partition_, 0, sizeof(shared_to_partition_));
    }

    void StartReplMgr();

    inline void cache_flush_data(const common::ReadHandle& rh);

    inline void cache_flush_metadata(const common::ReadHandle& rh);

private:
    std::shared_ptr<common::AllocatableLocalMemoryRegion> sc_shm_region_;  // Small Coherent Region
    std::shared_ptr<common::AllocatableLocalMemoryRegion> nc_shm_region_;  // Non-coherent Region
    std::shared_ptr<common::SharedMemoryObject> cache_shrd_ptr_;

    std::shared_ptr<common::AllocatableLocalMemoryRegion> local_region_[NUM_NUMA];
    std::shared_ptr<common::LocalMemoryObject<Policy>> cache_lcl_ptr_[LOGICAL_NODE_NUM];

    std::shared_ptr<common::TigonIPCServer<Policy>> ipc_servers[LOGICAL_NODE_NUM];
    std::shared_ptr<common::TigonIPCClient> ipc_clients[LOGICAL_NODE_NUM][LOGICAL_NODE_NUM];

    common::SharedMemoryObject* cache_;  // shared cache

    std::unique_ptr<C3POHandle> c3po_;

    int shared_cache_node_;

    std::random_device rdev_;
    std::mt19937_64 reng_;

    std::unique_ptr<common::LocalCacheDirectoryHandle> lcds_[LOGICAL_NODE_NUM];

    std::unique_ptr<common::ReplicateManager<Policy>> repl_mgr_[LOGICAL_NODE_NUM];

    std::unique_ptr<common::EvictManager<Policy>> evict_mgr_[LOGICAL_NODE_NUM];

    std::unique_ptr<common::FlushManagerHandle> flush_mgr_[LOGICAL_NODE_NUM];

    RackOBJConfig cfg_;

    std::optional<common::CacheNode*> SwapBlockId(const common::BlockId& new_block, FetchBlockFn fetch_block_fn,
                                                  ThreadLocalMeta<Policy>* local_meta);

    size_t CopyToUserBufferCXL(const common::ReadHandle& rh, void* ptr, size_t count, size_t offset_into_page);

    size_t CopyToUserBufferLocal(const common::ReadHandle& rh, void* ptr, size_t count, size_t offset_into_page);

    size_t CopyFromUserBufferCXL(const common::WriteHandle& wh, const void* ptr, size_t count, size_t offset_into_page);

    size_t CopyFromUserBufferLocal(const common::WriteHandle& wh, const void* ptr, size_t count,
                                   size_t offset_into_page);

    common::ReadLocation selectCacheNode(common::ReadHandle& rh);

    void checkCacheNode(common::WriteHandle& wh);

    expected<size_t, common::ReadErrno> ReadFromLocal(struct common::ReadHandle& rh, uint8_t* ptr, size_t count,
                                                      size_t offset_into_page, ThreadLocalMeta<Policy>* local_meta);

    expected<size_t, common::ReadErrno> ReadFromCXL(struct common::ReadHandle& rh, uint8_t* ptr, size_t count,
                                                    size_t offset_into_page, ThreadLocalMeta<Policy>* local_meta);

    expected<size_t, common::WriteErrno> WriteToLocal(const struct common::WriteHandle& wh, const uint8_t* ptr,
                                                      size_t count, size_t offset_into_page,
                                                      ThreadLocalMeta<Policy>* local_meta);

    expected<size_t, common::WriteErrno> WriteToCXL(struct common::WriteHandle& wh, const uint8_t* ptr, size_t count,
                                                    size_t offset_into_page, ThreadLocalMeta<Policy>* local_meta);

public:
    /* Tigon-like indexing helper functions */
    std::tuple<size_t, size_t> PartitionToShared(const common::BlockId& block_id, const size_t target_partition);
    std::tuple<size_t, size_t> PartitionToSharedGet(const common::BlockId& block_id, const size_t target_partition);

private:
    // this function assumes smeta write lock is held already
    std::optional<size_t> SharedToPartition(const common::BlockId& block_id, common::WriteMetadata* smeta,
                                            const size_t target_partition);

    struct alignas(64) CacheLineAlignedUint64 {
        uint64_t value;
        char padding[64 - sizeof(uint64_t)];  // Pad to cache line size
    };
    static_assert(sizeof(CacheLineAlignedUint64) == 64, "CacheLineAlignedUint64 must be exactly 64 bytes");

    CacheLineAlignedUint64 partition_to_shared_[LOGICAL_NODE_NUM];
    CacheLineAlignedUint64 shared_to_partition_[LOGICAL_NODE_NUM];

    std::atomic_uint64_t read_admit_retry_count_[LOGICAL_NODE_NUM];
    std::atomic_uint64_t read_coherence_retry_count_[LOGICAL_NODE_NUM];
};

}  // namespace rackobj::lib

// #include "shm_obj_handle.hpp"

#endif  // SHM_OBJECT_HANDLE_H
