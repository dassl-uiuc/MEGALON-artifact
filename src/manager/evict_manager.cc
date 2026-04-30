// #ifndef RACKOBJ_SHARED_MEMORY_DETAIL_EVICT_MANAGER_HPP
// #define RACKOBJ_SHARED_MEMORY_DETAIL_EVICT_MANAGER_HPP

#include "evict_manager.h"

#include <memory>

// #include "shm_obj_handle.h"
#include "absl/log/log.h"
#include "common/constants.h"
#include "core/object_slot.h"
#include "globals.h"
#include "replicate_manager.h"

namespace rackobj::common {

using namespace std::chrono_literals;

template <typename Policy>
EvictManager<Policy>::EvictManager(int run_node, int shared_cache_nid,
                                   const std::shared_ptr<AllocatableLocalMemoryRegion> &region) noexcept
    : exec_node_(run_node),
      shared_cache_node_(shared_cache_nid),
      watermark_ratio_(EVICT_WATERMARK),
      shared_cache_threshold_(0),
      local_cache_threshold_(0),
      local_region_(region),
      local_page_list_(nullptr),
      nr_evicted_(0) {}

template <typename Policy>
EvictManager<Policy>::~EvictManager() noexcept {}

/* Implementation without considering partial failure */
template <typename Policy>
size_t EvictManager<Policy>::DoEvictShared(size_t to_evict
#ifdef NR
                                           ,
                                           const NrFfi::NrMeta *nr_meta
#endif /* NR */
) {
    CacheNode *cn;
    WriteMetadata *wmeta;
    std::optional<common::GCDEntry> entry_optional;
    LocalPageList::ListNode *cur, *next;
    size_t nr_evicted = 0;
    bool is_wlocked = false;
    expected<std::optional<size_t>, NrGcdDeleteError> old_wmeta_idx_optional;

    LOG(FATAL) << " not implemented for logical node ";
    auto current_numa = static_cast<int>(GetCurrentNuma());
    CHECK(current_numa != shared_cache_node_) << "Read running on mem node";

    local_page_list_->LockList();
    cur = local_page_list_->GetListHead();
    do {
        /* Escape loop if local page list is empty */
        if (!local_page_list_->GetCountLocked()) break;

        size_t cn_index = cur->cn_index();
        cn = shared_page_cache_->cache_slot_.GetCacheNode(cn_index);
        /* Skip if cn has referencee or still dirty */
        if (!cn->ShouldEvict()) {
            if (cur == local_page_list_->GetListTail()) {
                // hit list tail
                DLOG(INFO) << "EvictManager::DoEvictLocal hit tail @" << cur;
                local_page_list_->UnlockList();
                local_page_list_->LockList();
                cur = local_page_list_->GetListHead();
            } else
                cur = local_page_list_->NextListNodeLocked(cur);
            continue;
        }
        local_page_list_->UnlockList();

        // if page_cache.bitmap[cn_index].is_dirty():
        //    skip this cn

        wmeta = nullptr;
        entry_optional = c3po_->Gcd()->GetAnchor(cn->GetBlockId()
#ifdef NR
                                                     ,
                                                 nr_meta
#endif /* NR */
        );
        if (!entry_optional.has_value()) {
            // Entry does not exist in GCD
            LOG(FATAL) << "BUG:EvictManager::DoEvictShared entry @" << cn << "does not exist in GCD";
            local_page_list_->LockList();
            cur = local_page_list_->NextListNodeLocked(cur);
            continue;  // skip this node
        } else if (entry_optional->wmeta_idx_ != std::nullopt) {
            // CacheNode is in read-write shared mode
            wmeta = c3po_->Scr_meta()->GetWmeta(entry_optional->wmeta_idx_.value());
            is_wlocked = wmeta->WLockOnly();
#ifdef NR
            if (!c3po_->CheckNotificationWrite(nr_meta)) {
#else
            // recheck metadata update, prevent time-to-check and time-to-modify problem, use gcd_get again
            std::optional<common::GCDEntry> recheck_optional = c3po_->Gcd()->Get(cn->GetBlockId());
            if (!isEqual(recheck_optional, entry_optional)) {
#endif /* NR */
                wmeta->WUnlockOnly();
                is_wlocked = false;
                local_page_list_->LockList();
                cur = local_page_list_->NextListNodeLocked(cur);
                DLOG(WARNING) << " page @" << cn_index << " with block_id " << cn->GetBlockId() << " is not valid";
                continue;  // skip this node
            }
            if (shared_page_cache_->scr_bitmap_.IsDirty(cn_index)) {
                wmeta->WUnlockOnly();
                is_wlocked = false;
                local_page_list_->LockList();
                cur = local_page_list_->NextListNodeLocked(cur);
                continue;  // skip this node
            }
        }

        old_wmeta_idx_optional = c3po_->Delete(cn->GetBlockId()
#ifdef NR
                                                   ,
                                               nr_meta
#endif /* NR */
        );
        if (wmeta && is_wlocked) {
            wmeta->WUnlockOnly();
            is_wlocked = false;
        }
        if (!old_wmeta_idx_optional.has_value()) {
            switch (old_wmeta_idx_optional.error()) {
                case NrGcdDeleteError::NR_GCD_DELETE_ENTRY_NOEXIST:
                    // Entry does not exist in GCD
                    LOG(FATAL) << "BUG: EvictManager::DoEvictShared entry @" << cn << "does not exist in GCD";
                    cn->ClearEvict();
                    local_page_list_->LockList();
                    cur = local_page_list_->NextListNodeLocked(cur);
                    continue;  // skip this node
                default:
                    LOG(FATAL) << "BUG: EvictManager::DoEvictShared entry @" << cn << " delete error "
                               << static_cast<int>(old_wmeta_idx_optional.error());
            }
        }
        if (old_wmeta_idx_optional.value().has_value()) {
            CHECK(old_wmeta_idx_optional.value().value() == entry_optional->wmeta_idx_.value())
                << "BUG: EvictManager::DoEvictShared entry @" << cn << " wmeta_idx_ mismatch";
            c3po_->Scr_meta()->RecycleWmeta(static_cast<size_t>(old_wmeta_idx_optional.value().value()));
        }

        local_page_list_->LockList();
        next = local_page_list_->NextListNodeLocked(cur);
        /* local page list remove @cn */
        local_page_list_->EvictLocked(cur);
        local_page_list_->UnlockList();

        cn->SetLength(0);
        cn->Reinitialize(BlockId());
        cn->ClearEvict();
        // repl_mgr_->ClearReplicate(cn_index);

        /* acquire free list lock outside of ll_lock
         * to prevent contention with Admit::ReserveNode */
        local_page_list_->RecycleListNode(cur);

        /* policy list remove cn */
        shared_page_cache_->scr_bitmap_.RecycleCacheNode(cn_index);
        nr_evicted++;

        DLOG(INFO) << "EvictManager::DoEvict @" << cn << " with block_id " << cn->GetBlockId() << " (" << nr_evicted
                   << " of " << to_evict << ")";

        local_page_list_->LockList();
        cur = next;
    } while (nr_evicted < to_evict);
    local_page_list_->UnlockList();

    return nr_evicted;
}

/* Implementation without considering partial failure */
template <typename Policy>
size_t EvictManager<Policy>::DoEvictLocal(size_t to_evict
#ifdef NR
                                          ,
                                          const NrFfi::NrMeta *nr_meta
#endif /* NR */
) {
    LocalCacheNode *cur, *next;
    size_t nr_evicted = 0;
    bool succeed;

    LOG(FATAL) << " not implemented for logical node ";
    auto current_numa = static_cast<int>(GetCurrentNuma());
    CHECK(current_numa != shared_cache_node_) << "Read running on mem node";

    local_page_cache_->policy_.LockList();
    cur = local_page_cache_->policy_.GetListHead();
    do {
        /* Escape loop if policy list is empty */
        if (!local_page_cache_->policy_.GetCountLocked()) break;

        /* Skip if cn has referencee */
        if (!cur->ShouldEvict() || !cur->EvictTryLock()) {
            if (cur == local_page_cache_->policy_.GetListTail()) {
                // hit list tail
                DLOG(INFO) << "EvictManager::DoEvictLocal hit tail @" << cur;
                local_page_cache_->policy_.UnlockList();
                local_page_cache_->policy_.LockList();
                cur = local_page_cache_->policy_.GetListHead();
            } else
                cur = local_page_cache_->policy_.NextCacheNodeLocked(cur);
            continue;
        }
        repl_mgr_->StopReplicate(cur->GetIndex());

        /* ### Evict cn from policy list ###
         * Since GCD->Delete never fails,
         * 1. unlock ll_lock before delete from GCD
         * 2. delete cn from GCD
         * 3. While list unlocked, another thread can be in the middle of Admit::Insert -> ::Evict
         *     3-a. Admit::Insert inserts at tail of list
         *     3-b. Admit::Evict evicts exactly inserted cn
         *     3-c. TODO: (sj) should consider contention with this Admit procedure
         * --> protected by ll_lock, lock contention with Admit::Insert & ::Evict */

        local_page_cache_->policy_.UnlockList();
        /* TODO: discuss about lock ordering (holds wlock in ll_lock) */

        succeed = c3po_->Gcd()->DeleteLocal(cur->GetBlockId(), exec_node_
#ifdef NR
                                            ,
                                            nr_meta
#endif /* NR */
        );

        local_page_cache_->policy_.LockList();

        next = local_page_cache_->policy_.NextCacheNodeLocked(cur);
        // CHECK(succeed) << "BUG: EvictManager::DoEvictLocal entry @" << cur << "does not exist in GCD";
        (void)succeed;

        /* policy list remove cn */
        local_page_cache_->policy_.EvictLocked(cur);
        nr_evicted++;

        local_page_cache_->policy_.UnlockList();
#if 0
        if (!evicted_tail)
            evicted_tail = cur;
        cur->next = evicted_head;
        cur->prev = nullptr;
        evicted_head = cur;
#endif

        cur->ClearEvict();
        cur->SetLength(0);

        repl_mgr_->ClearReplicate(cur->GetIndex());

        cur->Unlock();

        /* acquire free list lock outside of ll_lock
         * to prevent contention with Admit::ReserveNode */
        local_page_cache_->policy_.RecycleCacheNode(cur);

        local_page_cache_->policy_.LockList();
        DLOG(INFO) << "EvictManager::DoEvict @" << cur << " with block_id " << cur->GetBlockId() << " (" << nr_evicted
                   << " of " << to_evict << ")";
        cur = next;
    } while (nr_evicted < to_evict);
    local_page_cache_->policy_.UnlockList();

    /* TODO: Reclaim evicted cn to free list:
     *  * Bulk
     *      + acquire spinlock once
     *      + acquire free_list lock outside of ll_lock
     *      - consumer should wait for every gcd overhead (reclaimation is slow)
     *  * One by one
     *      + serve free cn quickly (single reclaimation is relatively fast)
     *      - acquires and drops spin lock a lot
     */
#if 0
    RecycleCacheNodeBulk(evicted_head, evicted_tail);
#endif

    return nr_evicted;
}

template <typename Policy>
void EvictManager<Policy>::Run() {
    shared_cache_worker_ =
        std::jthread(std::bind(&EvictManager::work_fn, this, std::placeholders::_1, shared_cache_node_));
    local_cache_worker_ = std::jthread(std::bind(&EvictManager::work_fn, this, std::placeholders::_1, exec_node_));
    LOG(INFO) << "EvictManager starts on node " << exec_node_;
}

template <typename Policy>
void EvictManager<Policy>::Shutdown() {
    local_cache_worker_.request_stop();
    shared_cache_worker_.request_stop();
    shared_cache_cv_.notify_all();
    local_cache_cv_.notify_all();
    LOG(INFO) << "EvictManager on node " << exec_node_ << " stop requested";
    local_cache_worker_.join();
    shared_cache_worker_.join();
    LOG(INFO) << "EvictManager on node " << exec_node_ << " joined, nr_evicted_: " << nr_evicted_;
}

/* TODO: follow wmeta mgr in supporting logical node */
template <typename Policy>
void EvictManager<Policy>::work_fn(std::stop_token stoken, int target_node) {
#ifdef NR
    const NrFfi::NrMeta *nr_meta;
#endif
    std::mutex m;
    std::unique_lock<std::mutex> ul(m);

    pinThreadtoNumaNode(exec_node_);
#ifdef NR
    nr_meta = c3po_->Gcd()->GetNrMetaTid(true);
    if (!nr_meta) {
        LOG(FATAL) << "Eviction thread for "
                   << ((target_node == shared_cache_node_) ? "SharedMemoryObject" : "LocalMemoryObject") << " on node "
                   << exec_node_ << " registration failed!!";
        return;
    }
#endif
    std::chrono::nanoseconds sleep_duration(EVICT_MGR_SLEEP_INTERVAL_NS);

    while (!stoken.stop_requested()) {
        ssize_t to_evict = NrToEvict(target_node);
        size_t nr_evicted;
        if (to_evict <= 0) {
            std::condition_variable &cv_ = (target_node == shared_cache_node_) ? shared_cache_cv_ : local_cache_cv_;
            cv_.wait(ul, [this, &stoken, target_node]() {
                ssize_t to_evict = NrToEvict(target_node);
                DLOG(INFO) << "EvictManager thread for "
                           << ((target_node == shared_cache_node_) ? "SharedMemoryObject" : "LocalMemoryObject")
                           << " while wait nr_to_evict: " << to_evict;
                return NrToEvict(target_node) > 0 || stoken.stop_requested();
            });
            DLOG(INFO) << "EvictManager thread for "
                       << ((target_node == shared_cache_node_) ? "SharedMemoryObject" : "LocalMemoryObject")
                       << " on node " << exec_node_ << "woke up";
            continue;
        }
        DLOG(INFO) << "EvictManager thread for "
                   << ((target_node == shared_cache_node_) ? "SharedMemoryObject" : "LocalMemoryObject")
                   << " nr_to_evict: " << to_evict;

        // workload here...
        nr_evicted = DoEvict(static_cast<size_t>(to_evict), target_node
#ifdef NR
                             ,
                             nr_meta
#endif /* NR */
        );

        DLOG(INFO) << "EvictManager for "
                   << ((target_node == shared_cache_node_) ? "SharedMemoryObject" : "LocalMemoryObject")
                   << "reclaimed (" << nr_evicted << " of " << to_evict << ") CacheNodes in total";
        nr_evicted_ += nr_evicted;

        // std::this_thread::sleep_for(1000ms);
        std::this_thread::sleep_for(sleep_duration);
    }
#ifdef NR
    c3po_->Gcd()->UnRegisterThread(nr_meta, true);
#endif
}

template class EvictManager<lib::CurrPolicy>;

// #endif

}  // namespace rackobj::common

// #endif /* RACKOBJ_SHARED_MEMORY_DETAIL_EVICT_MANAGER_HPP */
