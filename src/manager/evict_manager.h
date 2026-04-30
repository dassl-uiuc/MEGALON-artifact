#ifndef RACKOBJ_EVICT_MANAGER_H
#define RACKOBJ_EVICT_MANAGER_H

#include <condition_variable>
#include <optional>
#include <stop_token>
#include <thread>

#include "core/c3.h"
#include "core/local_pages.h"
#include "core/object_slot.h"
#include "index/gcd.h"
#include "index/gcd_nr.h"

namespace rackobj::common {

class BlockId;
class CacheNode;

template <typename Policy>
class EvictManager {
#ifdef NR
    using GcdHandle = common::GlobalCacheDirectoryHandleNr;
#else
    using GcdHandle = common::GlobalCacheDirectoryHandle;
#endif
    using C3POHandle = common::C3POHandle;

    using ListNodeAllocator = RebindLocalMemoryByteAllocatorT<typename LocalPageList::ListNode>;

public:
    explicit EvictManager(int run_node, int shared_cache_nid,
                          const std::shared_ptr<AllocatableLocalMemoryRegion> &region) noexcept;

    virtual ~EvictManager() noexcept;

    EvictManager(const EvictManager &) = delete;

#if 0
    template <typename U>
    bool operator==(const LocalMemoryAllocator<U>&) const noexcept {
        return true;
    }

    template <typename U>
    bool operator!=(const LocalMemoryAllocator<U>&) const noexcept {
        return false;
    }
#endif

    size_t DoEvict(size_t to_evict, int target_node
#ifdef NR
                   ,
                   const NrFfi::NrMeta *nr_meta
#endif /* NR */
    ) {
        if (target_node == shared_cache_node_)
            return DoEvictShared(to_evict
#ifdef NR
                                 ,
                                 nr_meta
#endif /* NR */
            );
        else
            return DoEvictLocal(to_evict
#ifdef NR
                                ,
                                nr_meta
#endif /* NR */
            );
    }

    void Run();

    void Shutdown();

    void insert_shared(size_t cn_index) {
        local_page_list_->Insert(cn_index);
        shared_cache_cv_.notify_all();
    }

    void wakeup_local() { local_cache_cv_.notify_all(); }

    void SetC3PO(C3POHandle *c3po) { c3po_ = c3po; }

    void SetSharedMemoryObject(const std::shared_ptr<common::SharedMemoryObject> &pcache_p) {
        size_t nr_cachenodes;
        shared_page_cache_ = pcache_p;
        nr_cachenodes = shared_page_cache_->page_count_;
        shared_cache_threshold_ = nr_cachenodes * static_cast<size_t>(watermark_ratio_) / 100UL;
        local_page_list_ = std::make_unique<LocalPageList>(nr_cachenodes, ListNodeAllocator(local_region_));
        local_page_list_->Initialize(nr_cachenodes);
        LOG(INFO) << "EvictManager thread for SharedMemoryObject on node " << exec_node_ << " threshold_: ("
                  << shared_cache_threshold_ << " out of " << nr_cachenodes << ")";
    }

    void SetLocalMemoryObject(const std::shared_ptr<common::LocalMemoryObject<Policy>> &pcache_p) {
        size_t nr_cachenodes;
        local_page_cache_ = pcache_p;
        nr_cachenodes = local_page_cache_->page_count_;
        local_cache_threshold_ = nr_cachenodes * static_cast<size_t>(watermark_ratio_) / 100UL;
        LOG(INFO) << "EvictManager thread for LocalMemoryObject on node " << exec_node_ << " threshold_: ("
                  << local_cache_threshold_ << " out of " << nr_cachenodes << ")";
    }

    void SetReplicateManager(common::ReplicateManager<Policy> *repl_mgr_p) { repl_mgr_ = repl_mgr_p; }

private:
    void work_fn(std::stop_token stop_token, int target_node);

    static constexpr size_t ReserveSize(size_t elements) {
        return static_cast<size_t>(static_cast<double>(elements) * 2);
    }

    ssize_t NrToEvict(int target_node) {
        size_t inserted, threshold;

        if (target_node == shared_cache_node_) {
            inserted = static_cast<common::SharedMemoryObject *>(shared_page_cache_.get())->GetActivePageCount();
            threshold = shared_cache_threshold_;
        } else {
            inserted = static_cast<common::LocalMemoryObject<Policy> *>(local_page_cache_.get())->GetActivePageCount();
            threshold = local_cache_threshold_;
        }
        return static_cast<ssize_t>(inserted - threshold);
    }

    size_t DoEvictShared(size_t to_evict
#ifdef NR
                         ,
                         const NrFfi::NrMeta *nr_meta
#endif /* NR */
    );

    size_t DoEvictLocal(size_t to_evict
#ifdef NR
                        ,
                        const NrFfi::NrMeta *nr_meta
#endif /* NR */
    );

    int exec_node_;
    int shared_cache_node_;
    int watermark_ratio_;  // 0 <= ratio <= 100

    C3POHandle *c3po_;

    std::jthread shared_cache_worker_;
    std::shared_ptr<common::SharedMemoryObject> shared_page_cache_;
    std::condition_variable shared_cache_cv_;
    size_t shared_cache_threshold_;

    std::jthread local_cache_worker_;
    std::shared_ptr<common::LocalMemoryObject<Policy>> local_page_cache_;
    std::condition_variable local_cache_cv_;
    size_t local_cache_threshold_;

    std::shared_ptr<AllocatableLocalMemoryRegion> local_region_;

    common::ReplicateManager<Policy> *repl_mgr_;

    std::unique_ptr<LocalPageList> local_page_list_;

    size_t nr_evicted_;
};

}  // namespace rackobj::common

// #include "detail/evict_manager.hpp"

#endif  // RACKOBJ_EVICT_MANAGER_H
