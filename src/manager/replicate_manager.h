#ifndef RACKOBJ_REPLICATE_MANAGER_H
#define RACKOBJ_REPLICATE_MANAGER_H

#include <condition_variable>
#include <optional>
#include <stop_token>
#include <thread>

#include "common/expected.h"
#include "core/c3.h"
#include "index/gcd.h"
#include "index/gcd_nr.h"
#include "shared_memory/local_work_allocator.h"
#include "shared_memory/region.h"
#include "tbb/concurrent_queue.h"

namespace rackobj::common {

class BlockId;
class CacheNode;

template <typename Policy>
class LocalMemoryObject;

template <typename Policy>
class EvictManager;

struct ReplicateWork {
    BlockId blkid;
    size_t cn_index;
};

template <typename Policy>
class ReplicateManager {
#ifdef NR
    using GcdHandle = common::GlobalCacheDirectoryHandleNr;
#else
    using GcdHandle = common::GlobalCacheDirectoryHandle;
#endif
    using C3POHandle = common::C3POHandle;

public:
    explicit ReplicateManager(int node, std::shared_ptr<SharedMemoryObject> shrd_cache,
                              const std::shared_ptr<AllocatableLocalMemoryRegion> &region) noexcept;

    virtual ~ReplicateManager() noexcept;

    ReplicateManager(const ReplicateManager &) = delete;

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

    bool ShouldReplicate(size_t cn_index) {
#ifdef REPLICATION
        // CHECK(cn_index < shared_cache_->Size()) << "Invalid cn_index: " << cn_index << " cache size: " <<
        // shared_cache_->Size();

        uint8_t count = replicate_cnt_[cn_index].load(std::memory_order_relaxed);
        if (count >= 2) {
            DLOG(INFO) << "cn replicate_cnt: " << static_cast<uint32_t>(count);
            return false;
        }

        while (!replicate_cnt_[cn_index].compare_exchange_weak(count, count + 1, std::memory_order_seq_cst)) {
            if (count >= 2) {
                DLOG(INFO) << "cn replicate_cnt: " << static_cast<uint32_t>(count);
                return false;
            }
        }
        return count + 1 == 2;
#else
        (void)cn_index;
        return false;
#endif
    }

    void ClearReplicate(size_t cn_index) {
#ifdef REPLICATION
        CHECK(cn_index < shared_cache_->Size())
            << "Invalid cn_index: " << cn_index << " cache size: " << shared_cache_->Size();
        uint8_t count = replicate_cnt_[cn_index].load(std::memory_order_relaxed);
        while (!replicate_cnt_[cn_index].compare_exchange_weak(count, 0, std::memory_order_acq_rel))
            ;
#endif
        (void)cn_index;
    }

    void StopReplicate(size_t cn_index) {
        CHECK(cn_index < shared_cache_->Size())
            << "Invalid cn_index: " << cn_index << " cache size: " << shared_cache_->Size();
        // int nid;

        // for (nid = 0; nid < NUM_NUMA; nid++) {
        uint8_t count = replicate_cnt_[cn_index].load(std::memory_order_seq_cst);
        while (!replicate_cnt_[cn_index].compare_exchange_weak(count, 3, std::memory_order_seq_cst))
            ;
        //}
    }

    bool enqueue(const BlockId &blkid, size_t shrd_cn_index);

    std::optional<struct ReplicateWork> try_dequeue() const;

    struct ReplicateWork dequeue_sync() const;

    expected<common::LocalCacheNode *, std::error_code> ReplicatePageLocal(const ReadHandle &rh,
                                                                           const common::BlockId &block_id
#ifdef NR
                                                                           ,
                                                                           const NrFfi::NrMeta *nr_meta
#endif /* NR */
    );

    expected<common::LocalCacheNode *, std::error_code> ReplicatePageLocal(size_t src_cn_index,
                                                                           const common::BlockId &block_id
#ifdef NR
                                                                           ,
                                                                           const NrFfi::NrMeta *nr_meta
#endif /* NR */
    );

#if 0
    std::optional<common::CacheNode*> SwapBlockId(common::CacheNode *shrd_cn, const common::BlockId& new_block
#ifdef NR
                                                  ,
                                                  const NrFfi::NrMeta *nr_meta
#endif /* NR */
    );
#endif

    void Run();

    void Shutdown();

    void SetC3PO(C3POHandle *c3po) { c3po_ = c3po; }

    void SetLocalObj(const std::shared_ptr<LocalMemoryObject<Policy>> &lcache_p) { local_cache_ = lcache_p; }

    void SetEvictManager(common::EvictManager<Policy> *evict_mgr) { evict_mgr_ = evict_mgr; }

private:
    void work_fn(std::stop_token stop_token);

    static constexpr size_t ReserveSize(size_t elements) {
        return static_cast<size_t>(static_cast<double>(elements) * 2);
    }

    int exec_node_;
    std::jthread worker_;

    C3POHandle *c3po_;

    std::shared_ptr<SharedMemoryObject> shared_cache_;
    std::shared_ptr<LocalMemoryObject<Policy>> local_cache_;
    std::shared_ptr<AllocatableLocalMemoryRegion> region_;

    tbb::memory_pool_allocator<struct ReplicateWork> *work_allocator_;
    tbb::memory_pool_allocator<struct ReplicateWork *> *item_allocator_;
    tbb::concurrent_bounded_queue<struct ReplicateWork *, tbb::memory_pool_allocator<struct ReplicateWork *>>
        *workqueue_;
    std::condition_variable cv_;

    std::shared_ptr<tbb::fixed_pool> work_pool_;
    std::shared_ptr<tbb::memory_pool<LocalMemoryAllocator<struct ReplicateWork *>>> item_pool_;

    EvictManager<Policy> *evict_mgr_;

    std::atomic<uint8_t> *replicate_cnt_;
};

}  // namespace rackobj::common

//#include "detail/replicate_manager.hpp"

#endif  // RACKOBJ_REPLICATE_MANAGER_H
