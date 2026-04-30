#ifndef RACKOBJ_WMETA_MANAGER_H
#define RACKOBJ_WMETA_MANAGER_H

#include <condition_variable>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>

#include "core/object_slot.h"
#include "index/gcd.h"
#include "index/gcd_nr.h"
#include "shared_memory/region.h"

namespace rackobj::common {

class C3POHandle;

class PartitionMetadataManager {
    using GcdHandle = common::GlobalCacheDirectoryHandle;
    using C3POHandle = common::C3POHandle;

public:
    explicit PartitionMetadataManager(C3POHandle *c3po, std::vector<int> run_nodes, std::vector<int> target_partitions,
                                      int shared_cache_nid) noexcept;

    virtual ~PartitionMetadataManager() noexcept;

    PartitionMetadataManager(const PartitionMetadataManager &) = delete;

    size_t IterativeReclaim(size_t count);

    size_t SampleReclaim(size_t count);

    size_t DoReclaim(int target_partition);

    void Run();

    void Shutdown();

    void SetPageCache(const std::shared_ptr<common::BasePageCache> &pcache_p) { page_cache_ = pcache_p; }

private:
    void work_fn(std::stop_token stoken, int exec_node, int target_partition);

    std::vector<int> exec_nodes_;
    std::vector<int> target_partitions_;
    int shared_cache_node_;
    size_t curr_switch_idx_;  // indicates where to start the reclaim check for next iteration

    C3POHandle *c3po_;

    std::vector<std::jthread> workers_;
    std::shared_ptr<common::BasePageCache> page_cache_;
    std::condition_variable cv_;
};

class PartitionMetadataManagerrHandle {
    using C3POHandle = common::C3POHandle;

public:
    PartitionMetadataManagerrHandle(C3POHandle *c3po, std::vector<int> run_nodes, std::vector<int> target_partitions,
                                    int shared_cache_nid, const std::shared_ptr<AllocatableLocalMemoryRegion> &region) {
        LocalMemoryAllocator<PartitionMetadataManager> pmeta_mgr_alloc(region);
        pmeta_mgr = pmeta_mgr_alloc.allocate();
        std::construct_at(pmeta_mgr, c3po, run_nodes, target_partitions, shared_cache_nid);
    }

    PartitionMetadataManager *operator->() { return pmeta_mgr; }

private:
    PartitionMetadataManager *pmeta_mgr;
};

}  // namespace rackobj::common

#endif  // RACKOBJ_WMETA_MANAGER_H