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

class WriteMetadataManager {
#ifdef NR
    using GcdHandle = common::GlobalCacheDirectoryHandleNr;
#else
    using GcdHandle = common::GlobalCacheDirectoryHandle;
#endif
    using C3POHandle = common::C3POHandle;

public:
    explicit WriteMetadataManager(C3POHandle *c3po, int run_node, int shared_cache_nid) noexcept;

    virtual ~WriteMetadataManager() noexcept;

    WriteMetadataManager(const WriteMetadataManager &) = delete;

    size_t IterativeReclaim(size_t count
#ifdef NR
                            ,
                            const NrFfi::NrMeta *nr_meta
#endif
    );

    size_t SampleReclaim(size_t count
#ifdef NR
                         ,
                         const NrFfi::NrMeta *nr_meta
#endif
    );

    size_t DoReclaim(
#ifdef NR
        const NrFfi::NrMeta *nr_meta
#endif
    );

    void Run();

    void Shutdown();

    void SetPageCache(const std::shared_ptr<common::BasePageCache> &pcache_p) { page_cache_ = pcache_p; }

private:
    void work_fn(std::stop_token stop_token);

    int exec_node_;
    int shared_cache_node_;
    size_t curr_switch_idx_;  // indicates where to start the reclaim check for next iteration

    C3POHandle *c3po_;

    // char buf_[4096];

    std::jthread worker_;
    std::shared_ptr<common::BasePageCache> page_cache_;
    std::condition_variable cv_;
};

class WriteMetadataManagerHandle {
    using C3POHandle = common::C3POHandle;

public:
    WriteMetadataManagerHandle(C3POHandle *c3po, int run_node, int shared_cache_nid,
                               const std::shared_ptr<AllocatableLocalMemoryRegion> &region) {
        LocalMemoryAllocator<WriteMetadataManager> wmeta_mgr_alloc(region);
        wmeta_mgr = wmeta_mgr_alloc.allocate();
        std::construct_at(wmeta_mgr, c3po, run_node, shared_cache_nid);
    }

    WriteMetadataManager *operator->() { return wmeta_mgr; }

private:
    WriteMetadataManager *wmeta_mgr;
};

}  // namespace rackobj::common

#endif  // RACKOBJ_WMETA_MANAGER_H