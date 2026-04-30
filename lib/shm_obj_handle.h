#ifndef SHM_OBJECT_HANDLE_H
#define SHM_OBJECT_HANDLE_H

#include <random>
#include <system_error>

#include "common/config.h"
#include "common/constants.h"
#include "common/expected.h"
#include "common/helper.h"
#include "common/thread_local.h"
#include "core/c3.h"
#include "core/object_slot.h"
#include "index/lcd.h"
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

public:
    SharedMemoryObjectHandle(const int node, const RackOBJConfig cfg);

#ifdef NR
    const NrFfi::NrMeta* RegisterNRThread(int rid);

    void UnRegisterNRThread(const NrFfi::NrMeta* nr_meta);
#endif /* NR */

    common::LocalMemoryObject<Policy>* RegisterLocalMem(int logical_node_id);

    ~SharedMemoryObjectHandle();

    size_t Read(const common::BlockId& block_id, uint8_t* ptr, size_t count, size_t offset_into_page,
                ThreadLocalMeta<Policy>* local_meta);

    size_t Get(const common::BlockId& block_id, uint8_t* ptr, size_t count, size_t offset_into_page,
               ThreadLocalMeta<Policy>* local_meta);

    expected<common::CacheNode*, std::error_code> Admit(const common::BlockId& block_id,
                                                        ThreadLocalMeta<Policy>* local_meta);

    expected<common::CacheNode*, std::error_code> AdmitWrite(const common::BlockId& block_id, const uint8_t* write_data,
                                                             ThreadLocalMeta<Policy>* local_meta);

    expected<common::CacheNode*, std::error_code> CreateEntry(const common::BlockId& block_id,
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

    // for internal use
    void SetWmetaWaterMark(size_t wmeta_water_mark) { c3po_->SetWmetaWaterMark(wmeta_water_mark); }

    void StartReplMgr();

    void ClearReclaimCount();

    inline void cache_flush_data(const common::ReadHandle& rh);

    inline void cache_flush_metadata(const common::ReadHandle& rh);

private:
    std::shared_ptr<common::AllocatableLocalMemoryRegion> sc_shm_region_;  // Small Coherent Region
    std::shared_ptr<common::AllocatableLocalMemoryRegion> nc_shm_region_;  // Non-coherent Region
    std::shared_ptr<common::SharedMemoryObject> cache_shrd_ptr_;

    std::shared_ptr<common::AllocatableLocalMemoryRegion> local_region_[NUM_NUMA];
    std::shared_ptr<common::LocalMemoryObject<Policy>> cache_lcl_ptr_[LOGICAL_NODE_NUM];

    common::SharedMemoryObject* cache_;  // shared cache

    std::unique_ptr<C3POHandle> c3po_;

    int shared_cache_node_;

    std::random_device rdev_;
    std::mt19937_64 reng_;
    // std::uniform_int_distribution<int> nid_dist_;

    std::unique_ptr<common::ReplicateManager<Policy>> repl_mgr_[LOGICAL_NODE_NUM];

    std::unique_ptr<common::EvictManager<Policy>> evict_mgr_[LOGICAL_NODE_NUM];

    std::unique_ptr<common::FlushManagerHandle> flush_mgr_[LOGICAL_NODE_NUM];

    // TODO: maybe add a separate evict function
    // void Evict(const common::BlockId& block_id);

    // TODO: maybe add a separate fetch function
    // void FetchBlock(const common::BlockId& block_id);

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

    expected<size_t, common::ReadErrno> ReadFromLocalExclusive(struct common::ReadHandle& rh, uint8_t* ptr,
                                                               size_t count, size_t offset_into_page,
                                                               ThreadLocalMeta<Policy>* local_meta);

    expected<size_t, common::ReadErrno> ReadFromLocal(struct common::ReadHandle& rh, uint8_t* ptr, size_t count,
                                                      size_t offset_into_page, ThreadLocalMeta<Policy>* local_meta);

    expected<size_t, common::ReadErrno> ReadFromCXL(struct common::ReadHandle& rh, uint8_t* ptr, size_t count,
                                                    size_t offset_into_page, ThreadLocalMeta<Policy>* local_meta);

    expected<size_t, common::WriteErrno> WriteToLocal(struct common::WriteHandle& wh, const uint8_t* ptr, size_t count,
                                                      size_t offset_into_page, ThreadLocalMeta<Policy>* local_meta);

    expected<size_t, common::WriteErrno> WriteToCXL(struct common::WriteHandle& wh, const uint8_t* ptr, size_t count,
                                                    size_t offset_into_page, ThreadLocalMeta<Policy>* local_meta);

    std::atomic_uint64_t read_admit_retry_count_[LOGICAL_NODE_NUM];
    std::atomic_uint64_t read_coherence_retry_count_[LOGICAL_NODE_NUM];
};

}  // namespace rackobj::lib

// #include "shm_obj_handle.hpp"

#endif  // SHM_OBJECT_HANDLE_H
