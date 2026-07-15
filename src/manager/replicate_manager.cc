// #ifndef RACKOBJ_SHARED_MEMORY_DETAIL_REPLICATE_MANAGER_HPP
// #define RACKOBJ_SHARED_MEMORY_DETAIL_REPLICATE_MANAGER_HPP

#include "replicate_manager.h"

#include <memory>

#include "core/local_cache_node.h"
#include "evict_manager.h"
#include "globals.h"
#include "shm_obj_handle.h"

namespace rackobj::common {

using lib::SharedMemoryObjectHandle;
using namespace std::chrono_literals;

template <typename Policy>
ReplicateManager<Policy>::ReplicateManager(int node, std::shared_ptr<SharedMemoryObject> shared_cache,
                                           const std::shared_ptr<AllocatableLocalMemoryRegion> &region,
                                           bool replication_enabled) noexcept
    : exec_node_(node),
      replication_enabled_(replication_enabled),
      shared_cache_(shared_cache),
      region_(region),
      replicate_cnt_(nullptr) {
    LocalMemoryByteAllocator byte_allocator(region);
    size_t bufsize = (REPL_WQ_SIZE + ReserveSize(REPL_WQ_SIZE)) * sizeof(struct ReplicateWork);
    void *buffer = byte_allocator.allocate(bufsize);
    LOG(INFO) << "ReplicateManager bufsize: " << bufsize << " bytes";
    work_pool_ = std::make_shared<tbb::fixed_pool>(buffer, bufsize);

    LocalMemoryAllocator<std::remove_pointer_t<decltype(work_allocator_)>> work_alloc(region);
    work_allocator_ = work_alloc.allocate();
    std::construct_at(work_allocator_, *work_pool_);

    LocalMemoryAllocator<struct ReplicateWork *> item(region);
    item_pool_ = std::make_shared<tbb::memory_pool<LocalMemoryAllocator<struct ReplicateWork *>>>(item);
    LocalMemoryAllocator<std::remove_pointer_t<decltype(item_allocator_)>> item_alloc(region);
    item_allocator_ = item_alloc.allocate();
    std::construct_at(item_allocator_, *item_pool_);

    LocalMemoryAllocator<std::remove_pointer_t<decltype(workqueue_)>> queue_alloc(region);
    workqueue_ = queue_alloc.allocate();
    std::construct_at(workqueue_, *item_allocator_);
    workqueue_->set_capacity(REPL_WQ_SIZE);

    LocalMemoryAllocator<std::atomic<uint8_t>> refcnt_allocator(region);
    replicate_cnt_ = refcnt_allocator.allocate(shared_cache->Size());
    memset(replicate_cnt_, 0, shared_cache->Size() * sizeof(std::atomic<uint8_t>));
}

template <typename Policy>
ReplicateManager<Policy>::~ReplicateManager() noexcept {
    DLOG(INFO) << "Deallocating " << TypeToString(workqueue_) << " at " << (void *)workqueue_;
    std::destroy_at(workqueue_);

    DLOG(INFO) << "Deallocating " << TypeToString(item_allocator_) << " at " << (void *)item_allocator_;
    std::destroy_at(item_allocator_);

    DLOG(INFO) << "Deallocating " << TypeToString(work_allocator_) << " at " << (void *)work_allocator_;
    std::destroy_at(work_allocator_);
    // TODO: destroy allocated memory region
}

template <typename Policy>
bool ReplicateManager<Policy>::enqueue(const BlockId &blkid, size_t shrd_cn_index) {
    if (!replication_enabled_) return false;
    if (replication_suspended_.load(std::memory_order_relaxed)) return false;

    struct ReplicateWork *work_item;
    bool succeed;

    if (workqueue_->size() >= workqueue_->capacity()) return false;

    try {
        work_item = work_allocator_->allocate(1);
        if (!work_item) return false;
    } catch (std::bad_alloc const &) {
        return false;
    }
    work_item->blkid = blkid;
    work_item->cn_index = shrd_cn_index;
    succeed = workqueue_->try_push(std::move(work_item));
    if (!succeed) {
        work_allocator_->deallocate(work_item, sizeof(*work_item));
        return false;
    }
    cv_.notify_all();
    return succeed;
}

template <typename Policy>
std::optional<struct ReplicateWork> ReplicateManager<Policy>::try_dequeue() const {
    struct ReplicateWork work;
    struct ReplicateWork *workp;
    if (!workqueue_->try_pop(workp)) return std::nullopt;
    work = *workp;
    work_allocator_->deallocate(workp, sizeof(*workp));

    return work;
}

template <typename Policy>
struct ReplicateWork ReplicateManager<Policy>::dequeue_sync() const {
    struct ReplicateWork work;
    struct ReplicateWork *workp;
    workqueue_->pop(workp);
    work = *workp;
    work_allocator_->deallocate(workp, sizeof(*workp));
    return work;
}

/**
 * Replicate CXL shared page -> local page cache for logical node exec_node_.
 * Only succeeds when the GCD entry is in RO mode (no wmeta_idx_).
 */
template <typename Policy>
expected<common::LocalCacheNode *, std::error_code> ReplicateManager<Policy>::ReplicatePageLocal(
    const ReadHandle &rh, const common::BlockId &block_id
#ifdef NR
    ,
    const NrFfi::NrMeta *nr_meta
#endif /* NR */
) {
    size_t shrd_cn_index = rh.cn_index.value();
    return ReplicatePageLocal(shrd_cn_index, block_id
#ifdef NR
                              ,
                              nr_meta
#endif /* NR */
    );
}

template <typename Policy>
expected<common::LocalCacheNode *, std::error_code> ReplicateManager<Policy>::ReplicatePageLocal(
    size_t src_cn_index, const common::BlockId &block_id
#ifdef NR
    ,
    const NrFfi::NrMeta *nr_meta
#endif
) {
    common::CacheNode *src_cn;
    size_t src_length;
    void *page;
    common::LocalCacheNode *local_cn;
    std::optional<common::GCDEntry> entry_optional;
    common::NrGcdError error;
    std::error_code errc;
    bool evicted;

    DCHECK(verifyThreadNuma(RidToNumaNode(exec_node_)))
        << "ReplicateManager[" << exec_node_ << "] running on wrong NUMA node";

    /* 1. Reserve a cache node from the local free list */
    local_cn = local_cache_->policy_.ReserveCacheNode();
    if (!local_cn) {
        // Local DRAM is full and eviction is disabled: suspend replication for process lifetime.
        replication_suspended_.store(true, std::memory_order_relaxed);
        DLOG(WARNING) << "ReplicateManager[" << exec_node_ << "]: local cache full, suspending replication";
        return unexpected(std::make_error_code(std::errc::not_enough_memory));
    }

    /* 2. GCD get entry */
    entry_optional = c3po_->Gcd()->GetAnchor(block_id
#ifdef NR
                                             ,
                                             nr_meta
#endif
    );

    /* 3. Validate: must still be on CXL and in RO mode */
    if (!c3po_->CacheNodeIndexOnCxl(entry_optional).has_value() ||
        src_cn_index != c3po_->CacheNodeIndexOnCxl(entry_optional).value()) {
        DLOG(WARNING) << "src_cn_index mismatch with GCD entry";
        errc = std::make_error_code(std::errc::no_such_file_or_directory);
        goto failed;
    }
    if (entry_optional->wmeta_idx_.has_value()) {
        DLOG(WARNING) << "src_cn page is RW shared; refusing to replicate";
        errc = std::make_error_code(std::errc::device_or_resource_busy);
        goto failed;
    }

    src_cn = shared_cache_->cache_slot_.GetCacheNode(src_cn_index);
    page = shared_cache_->page_data_.GetDataSlot(src_cn_index);

    src_length = src_cn->GetLength();
    if (src_length == 0) src_length = SLOT_SIZE;
    memcpy(local_cn->GetDataSlot(), page, src_length);

    /* 4. Coherence check after copy */
    if (!c3po_->CheckNotification(
#ifdef NR
            nr_meta
#endif
            )) {
        DLOG(WARNING) << "CXL page @" << src_cn_index << " with block_id " << block_id << " is not valid";
        errc = std::make_error_code(std::errc::resource_unavailable_try_again);
        goto failed;
    }

    /* 5. Fill local_cn metadata */
    CHECK(src_length) << "ReplicatePageLocal: src_cn length is 0";
    local_cn->SetLength(src_length);
    local_cn->Reinitialize(block_id);

    /* 6. Insert into GCD: logical node L -> NR array slot L+1 */
    error = c3po_->Gcd()->CheckAndInsert(local_cn->GetBlockId(), reinterpret_cast<size_t>(local_cn), exec_node_ + 1
#ifdef NR
                                         ,
                                         nr_meta
#endif /* NR */
    );
    switch (error) {
        case common::NrGcdError::GCD_NO_ERROR:
            break;
        case common::NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED:
        case common::NrGcdError::GCD_SLOT_UPDATE_FAILED:
            DLOG(WARNING) << "local move fail";
            errc = std::make_error_code(std::errc::resource_unavailable_try_again);
            goto failed;
        default:
            LOG(FATAL) << "Wrong error code";
    }

    /* 7. Insert into local LRU policy */
    evicted = local_cache_->policy_.Insert(local_cn, [](const common::LocalCacheNode &cn) {
        DLOG(INFO) << "ReplicatePageLocal: Evicted @" << &cn << " from Local cache";
    });
    (void)evicted;

    ClearReplicate(src_cn_index);
    return local_cn;

failed:
    ClearReplicate(src_cn_index);
    local_cache_->policy_.RecycleCacheNode(local_cn);
    return unexpected(errc);
}

template <typename Policy>
void ReplicateManager<Policy>::Run() {
    worker_ = std::jthread(std::bind(&ReplicateManager::work_fn, this, std::placeholders::_1));
    LOG(INFO) << "ReplicateManager starts on logical node " << exec_node_;
}

template <typename Policy>
void ReplicateManager<Policy>::Shutdown() {
    worker_.request_stop();
    cv_.notify_all();
    LOG(INFO) << "ReplicateManager on logical node " << exec_node_ << " stop requested";
    worker_.join();
    LOG(INFO) << "ReplicateManager on logical node " << exec_node_ << " joined";
}

template <typename Policy>
void ReplicateManager<Policy>::work_fn(std::stop_token stoken) {
#ifdef NR
    const NrFfi::NrMeta *nr_meta;
#endif
    std::optional<struct ReplicateWork> work;
    std::mutex m;
    std::unique_lock<std::mutex> ul(m);

    pinThreadtoNumaNode(RidToNumaNode(exec_node_));
#ifdef NR
    nr_meta = c3po_->Gcd()->GetNrMetaTid(exec_node_, true);
    if (!nr_meta) {
        LOG(FATAL) << "Replication thread registration failed for logical node " << exec_node_;
        return;
    }
#endif

    work = try_dequeue();
    while (!stoken.stop_requested()) {
        if (!work.has_value()) {
            cv_.wait_for(ul, 100ms, [this, &stoken]() { return !workqueue_->empty() || stoken.stop_requested(); });
            work = try_dequeue();
            continue;
        }

        auto result = ReplicatePageLocal(work->cn_index, work->blkid
#ifdef NR
                                         ,
                                         nr_meta
#endif /* NR */
        );
        if (result.has_value()) {
            DLOG(INFO) << "ReplicateManager replicated src_cn @" << work->cn_index << " to logical node " << exec_node_
                       << " with block_id " << work->blkid;
            work = try_dequeue();
        } else {
            switch (static_cast<std::errc>(result.error().value())) {
                /* Local cache is full: drain the entire queue and suspend */
                case std::errc::not_enough_memory: {
                    DLOG(WARNING) << "ReplicateManager[" << exec_node_
                                  << "]: local cache full, draining queue and suspending";
                    struct ReplicateWork *raw;
                    while (workqueue_->try_pop(raw)) {
                        ClearReplicate(raw->cn_index);
                        work_allocator_->deallocate(raw, sizeof(*raw));
                    }
                    work = std::nullopt;
                    break;
                }
                /* CXL CN found to be RW shared -> skip */
                case std::errc::device_or_resource_busy:
                    DLOG(WARNING) << "ReplicateManager: ReplicatePageLocal failed with device_or_resource_busy";
                    work = try_dequeue();
                    break;
                /* GCD entry no longer contains the CXL CN -> skip */
                case std::errc::no_such_file_or_directory:
                    DLOG(WARNING) << "ReplicateManager: ReplicatePageLocal failed with no_such_file_or_directory";
                    work = try_dequeue();
                    break;
                /* Coherence check failed -> retry after brief sleep */
                case std::errc::resource_unavailable_try_again:
                    DLOG(WARNING) << "ReplicateManager: ReplicatePageLocal failed with resource_unavailable_try_again";
                    std::this_thread::sleep_for(200ms);
                    break;
                default:
                    break;
            }
        }
    }
#ifdef NR
    c3po_->Gcd()->UnRegisterThread(nr_meta, true);
#endif
}

template class ReplicateManager<lib::CurrPolicy>;

}  // namespace rackobj::common

// #endif // RACKOBJ_SHARED_MEMORY_DETAIL_REPLICATE_MANAGER_HPP
