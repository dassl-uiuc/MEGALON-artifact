#ifndef RACKOBJ_FLUSH_MANAGER_H
#define RACKOBJ_FLUSH_MANAGER_H

#include <condition_variable>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>

#include "core/c3.h"
#include "core/object_slot.h"
#include "index/gcd.h"
#include "index/gcd_nr.h"
#include "shared_memory/region.h"

namespace rackobj::common {

class FlushManager {
#ifdef NR
    using GcdHandle = common::GlobalCacheDirectoryHandleNr;
#else
    using GcdHandle = common::GlobalCacheDirectoryHandle;
#endif
    using C3POHandle = common::C3POHandle;

public:
    explicit FlushManager(int run_node, int target_node, int shared_cache_nid) noexcept;

    virtual ~FlushManager() noexcept;

    FlushManager(const FlushManager &) = delete;

    size_t DoFlush(
#ifdef NR
        const NrFfi::NrMeta *nr_meta
#endif
    ) {
        if (target_node_ == shared_cache_node_)
            return DoFlushShared(
#ifdef NR
                nr_meta
#endif
            );
        else
            return DoFlushLocal();
    }

    size_t DoFlushShared(
#ifdef NR
        const NrFfi::NrMeta *nr_meta
#endif
    );

    size_t DoFlushLocal();

    void Run();

    void Shutdown();

    void SetPageCache(const std::shared_ptr<common::BasePageCache> &pcache_p) {
        size_t nr_cachenodes;
        page_cache_ = pcache_p;
        nr_cachenodes = page_cache_->page_count_;
        threshold_ = nr_cachenodes * static_cast<size_t>(watermark_ratio_) / 100UL;
        LOG(INFO) << "FlushManager thread for node " << target_node_ << " on node " << exec_node_ << " threshold_: ("
                  << threshold_ << " out of " << nr_cachenodes << ")";
    }

    void SetC3PO(C3POHandle *c3po) { c3po_ = c3po; }

private:
    void work_fn(std::stop_token stop_token);

    ssize_t NrToFlush() {
        size_t inserted;

        if (target_node_ == shared_cache_node_)
            inserted = static_cast<common::SharedMemoryObject *>(page_cache_.get())->GetDirtyPageCount();
        else
            inserted = threshold_;
        return static_cast<ssize_t>(inserted - threshold_);
    }

    int exec_node_;
    int target_node_;
    int shared_cache_node_;
    size_t curr_flush_idx_;  // indicates where to start the flush check for next iteration
    int watermark_ratio_;    // 0 <= ratio <= 100
    size_t threshold_;

    C3POHandle *c3po_;

    char buf_[SLOT_SIZE];

    std::jthread worker_;
    std::shared_ptr<common::BasePageCache> page_cache_;
    std::condition_variable cv_;
};

class FlushManagerHandle {
public:
    FlushManagerHandle(int run_node, int target_node, int shared_cache_nid,
                       const std::shared_ptr<AllocatableLocalMemoryRegion> &region) {
        LocalMemoryAllocator<FlushManager> flush_mgr_alloc(region);
        flush_mgr = flush_mgr_alloc.allocate();
        std::construct_at(flush_mgr, run_node, target_node, shared_cache_nid);
    }

    FlushManager *operator->() { return flush_mgr; }

private:
    FlushManager *flush_mgr;
};

}  // namespace rackobj::common

#endif