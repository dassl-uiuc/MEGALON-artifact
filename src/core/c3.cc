#include "c3.h"

#include "local_cache_node.h"
#include "lru_policy.h"
#include "object_slot.h"

namespace rackobj::common {
using std::make_unique;
using std::unique_ptr;

std::ostream &operator<<(std::ostream &os, WriteErrno e) {
    switch (e) {
        case WriteErrno::NO_ERROR:
            return os << "NO_ERROR";
        case WriteErrno::PAGE_NOT_FOUND:
            return os << "PAGE_NOT_FOUND";
        case WriteErrno::META_OUTDATE:
            return os << "META_OUTDATE";
        case WriteErrno::PAGE_RO:
            return os << "PAGE_RO";
        case WriteErrno::MULTIPLE_REPLICA:
            return os << "MULTIPLE_REPLICA";
        case WriteErrno::MULTIPLE_REPLICA_NOT_ON_CXL:
            return os << "MULTIPLE_REPLICA_NOT_ON_CXL";
        case WriteErrno::PAGE_ON_REMOTE_NODE:
            return os << "PAGE_ON_REMOTE_NODE";
        default:
            return os << "UNKNOWN";
    }
}

C3PO::C3PO(const size_t num_entries, void *map_address, const int cxl_nid, const uint8_t *base_addr,
           const std::shared_ptr<common::AllocatableLocalMemoryRegion> &sc_shm_region, const size_t logical_scr_size)
    : gcd_(GcdHandle::CreateOrMap(num_entries, map_address, cxl_nid, base_addr, sc_shm_region)),
      scr_meta_(nullptr),
      scr_bitmap_ptr_(nullptr),
      logical_scr_size_(logical_scr_size) {}

C3POHandle::C3POHandle(C3PO *c3po, int cxl_nid) noexcept : c3po_(c3po), cxl_nid_(cxl_nid) {}
#define CEIL_DIV(a, b) (((a) + (b)-1) / (b))

unique_ptr<C3POHandle> C3POHandle::CreateOrMap(
    size_t num_entries, void *map_address, int cxl_nid, const uint8_t *base_addr,
    const std::shared_ptr<common::AllocatableLocalMemoryRegion> &sc_shm_region, const size_t logical_scr_size) {
    size_t perobjmetasize = CEIL_DIV(SEQLOCK_BITS, 8);  // sizeof(seqlock_t);
    size_t max_num_entries = logical_scr_size / perobjmetasize;
    auto actual_num_entries = std::min(num_entries, max_num_entries);

    // currently, we do not allocate on shared memory region
    LOG(INFO) << "C3 created with entries: " << actual_num_entries;
    C3PO *c3po = new C3PO(actual_num_entries, map_address, cxl_nid, base_addr, sc_shm_region, logical_scr_size);

    return make_unique<C3POHandle>(c3po, cxl_nid);
}

void C3POHandle::InitSharedMetadata(size_t num_slots,
                                    const std::shared_ptr<common::AllocatableLocalMemoryRegion> &sc_shm_region,
                                    SharedBitmap *bitmap) {
    size_t perobjmetasize = CEIL_DIV(SEQLOCK_BITS, 8);  // sizeof(seqlock_t);
    size_t wmeta_slots = c3po_->logical_scr_size_ / perobjmetasize;
    size_t max_wmeta_slots = static_cast<size_t>((static_cast<uint64_t>(num_slots) * 100) / WMETA_WATERMARK);
    wmeta_slots = std::min(wmeta_slots, max_wmeta_slots);
    c3po_->scr_meta_ = make_unique<SharedMetadata>(cxl_nid_, num_slots, wmeta_slots, sc_shm_region);
    c3po_->scr_bitmap_ptr_ = bitmap;

    size_t threshold = wmeta_slots * WMETA_WATERMARK / 100;
    c3po_->scr_meta_->SetWmetaThreshold(static_cast<size_t>(threshold));
    LOG(INFO) << "C3POHandle: wmeta_slots: " << wmeta_slots << ", threshold: " << threshold
              << ", per object metadata size: " << perobjmetasize;
}

void C3POHandle::CreateLocalSeqMap(int nid, const std::shared_ptr<common::AllocatableLocalMemoryRegion> &local_region) {
    c3po_->scr_meta_->CreateLocalSeqMap(nid, local_region);
}

void C3POHandle::CreateWmetaMgr(int nid, const std::shared_ptr<common::AllocatableLocalMemoryRegion> &local_region) {
    if (!wmeta_mgr_)
        wmeta_mgr_ = std::make_unique<common::WriteMetadataManagerHandle>(this, nid, cxl_nid_, local_region);
}

void C3POHandle::SetSharedPageCache(const std::shared_ptr<common::BasePageCache> &pcache_p) {
    if (wmeta_mgr_) (*wmeta_mgr_)->SetPageCache(pcache_p);
}

void C3POHandle::SetCaches(const std::shared_ptr<SharedMemoryObject> shared_cache,
                           std::shared_ptr<LocalMemoryObject<LruPolicy>> *local_cache_array) {
    shared_cache_ = shared_cache;
    local_cache_ = local_cache_array;
}

// this function is called under a LOCK (CXL node lock(wmeta lock)). Make sure of it.
void C3POHandle::MoveToLocal(const ReadHandle &rh, size_t cn_index
#ifdef NR
                             ,
                             const NrFfi::NrMeta *nr_meta
#endif
) {

    DLOG(INFO) << "Replicating page " << rh.key << " to local node " << rh.current_nid
               << " (access count: " << c3po_->scr_meta_->GetCurrentNodeCount(cn_index, rh.current_nid) << ")";

    // Validate cache configuration
    if (!shared_cache_ || !local_cache_ || rh.current_nid < 0 || rh.current_nid >= LOGICAL_NODE_NUM) {
        LOG(WARNING) << "Invalid cache configuration for replication";
        c3po_->scr_meta_->GetWmeta(rh.entry_optional->wmeta_idx_.value())->WUnlockOnly();
        return;
    }

    // Get shared and local cache pointers
    auto *shared_cache = reinterpret_cast<SharedMemoryObject *>(shared_cache_.get());
    auto *local_cache = reinterpret_cast<BasePageCache *>(local_cache_[rh.current_nid].get());

    if (!shared_cache || !local_cache) {
        LOG(WARNING) << "Null cache pointers";
        c3po_->scr_meta_->GetWmeta(rh.entry_optional->wmeta_idx_.value())->WUnlockOnly();
        return;
    }

    // Get source data from shared CXL cache
    CacheNode *src_cn = shared_cache->cache_slot_.GetCacheNode(cn_index);
    uint8_t *src_page = shared_cache->page_data_.GetDataSlot(cn_index);
    size_t src_length = src_cn->GetLength();

    if (!src_cn || !src_page) {
        LOG(WARNING) << "Invalid source cache node";
        c3po_->scr_meta_->GetWmeta(rh.entry_optional->wmeta_idx_.value())->WUnlockOnly();
        return;
    }

    // Use SLOT_SIZE if length is not set (common for read-only pages)
    if (src_length == 0) {
        src_length = SLOT_SIZE;
    }

    // Cast to LocalMemoryObject to access policy
    auto *local_obj = dynamic_cast<LocalMemoryObject<LruPolicy> *>(local_cache);
    if (!local_obj) {
        LOG(WARNING) << "Failed to cast to LocalMemoryObject";
        c3po_->scr_meta_->GetWmeta(rh.entry_optional->wmeta_idx_.value())->WUnlockOnly();
        return;
    }

    // 1. Reserve a local cache node from free list
    LocalCacheNode *local_cn = local_obj->policy_.ReserveCacheNode();
    if (!local_cn) {
        LOG(WARNING) << "No free local cache node available";
        c3po_->scr_meta_->GetWmeta(rh.entry_optional->wmeta_idx_.value())->WUnlockOnly();
        return;
    }

    auto ret = c3po_->gcd_->MoveIndexToLocal(rh.key, reinterpret_cast<void *>(local_cn), rh.current_nid, nr_meta);

    // 4. Handle GCD insertion result
    if (ret) {
        // 2. Copy data from shared to local
        memcpy(local_cn->GetDataSlot(), src_page, src_length);
        local_cn->SetLength(src_length);
        local_cn->Reinitialize(rh.key);

        // Successfully inserted into GCD, add to LRU policy
        bool evicted = local_obj->policy_.Insert(local_cn, [](const LocalCacheNode &cn) {
            DLOG(INFO) << "Evicted local cache node @" << &cn << " during replication";
        });
        (void)evicted;
        DLOG(INFO) << "Successfully replicated page " << rh.key << " to local node " << rh.current_nid;
        c3po_->scr_meta_->RecycleWmeta(rh.entry_optional->wmeta_idx_.value());
        shared_cache->scr_bitmap_.RecycleCacheNode(cn_index);
    } else {
        // Failed to insert - recycle the node back to free list
        DLOG(WARNING) << "Failed to update index to local";
        local_obj->policy_.RecycleCacheNode(local_cn);
        c3po_->scr_meta_->GetWmeta(rh.entry_optional->wmeta_idx_.value())->WUnlockOnly();
    }
}
void C3POHandle::MoveToLocalReadOnly(const ReadHandle &rh, size_t cn_index
#ifdef NR
                                     ,
                                     const NrFfi::NrMeta *nr_meta
#endif
) {

    DLOG(INFO) << "Replicating page " << rh.key << " to local node " << rh.current_nid
               << " (access count: " << c3po_->scr_meta_->GetCurrentNodeCount(cn_index, rh.current_nid) << ")";

    // Validate cache configuration
    if (!shared_cache_ || !local_cache_ || rh.current_nid < 0 || rh.current_nid >= LOGICAL_NODE_NUM) {
        LOG(WARNING) << "Invalid cache configuration for replication";
        return;
    }

    // Get shared and local cache pointers
    auto *shared_cache = reinterpret_cast<SharedMemoryObject *>(shared_cache_.get());
    auto *local_cache = reinterpret_cast<BasePageCache *>(local_cache_[rh.current_nid].get());

    if (!shared_cache || !local_cache) {
        LOG(WARNING) << "Null cache pointers";
        return;
    }

    // Get source data from shared CXL cache
    CacheNode *src_cn = shared_cache->cache_slot_.GetCacheNode(cn_index);
    uint8_t *src_page = shared_cache->page_data_.GetDataSlot(cn_index);
    size_t src_length = src_cn->GetLength();

    if (!src_cn || !src_page) {
        LOG(WARNING) << "Invalid source cache node";
        return;
    }

    // Use SLOT_SIZE if length is not set (common for read-only pages)
    if (src_length == 0) {
        src_length = SLOT_SIZE;
    }

    // Cast to LocalMemoryObject to access policy
    auto *local_obj = dynamic_cast<LocalMemoryObject<LruPolicy> *>(local_cache);
    if (!local_obj) {
        LOG(WARNING) << "Failed to cast to LocalMemoryObject";
        return;
    }

    // 1. Reserve a local cache node from free list
    LocalCacheNode *local_cn = local_obj->policy_.ReserveCacheNode();
    if (!local_cn) {
        LOG(WARNING) << "No free local cache node available";
        return;
    }

    auto ret = c3po_->gcd_->MoveIndexToLocal(rh.key, reinterpret_cast<void *>(local_cn), rh.current_nid, nr_meta);

    // 4. Handle GCD insertion result
    if (ret) {
        // 2. Copy data from shared to local
        memcpy(local_cn->GetDataSlot(), src_page, src_length);
        local_cn->SetLength(src_length);
        local_cn->Reinitialize(rh.key);

        // Successfully inserted into GCD, add to LRU policy
        bool evicted = local_obj->policy_.Insert(local_cn, [](const LocalCacheNode &cn) {
            DLOG(INFO) << "Evicted local cache node @" << &cn << " during replication";
        });
        (void)evicted;
        DLOG(INFO) << "Successfully replicated page " << rh.key << " to local node " << rh.current_nid;
        shared_cache->scr_bitmap_.RecycleCacheNode(cn_index);
    } else {
        // Failed to insert - recycle the node back to free list
        DLOG(WARNING) << "Failed to update index to local";
        local_obj->policy_.RecycleCacheNode(local_cn);
    }
}

bool C3POHandle::CheckCoherence(const BlockId &to_check, uint32_t nid
#ifdef NR
                                ,
                                const NrFfi::NrMeta *nr_meta
#endif
) {
#if defined(SCR) && defined(DYN_WMETA)
    // our system
    (void)nid;
    return Gcd()->CheckCoherence(to_check
#ifdef NR
                                 ,
                                 nr_meta
#endif
    );
#else
    (void)to_check;
    (void)nid;
#ifdef NR
    (void)nr_meta;
#endif
    return true;
#endif
}

bool C3POHandle::CheckNotification(
#ifdef NR
    const NrFfi::NrMeta *nr_meta
#endif
) {
#if defined(SCR) && defined(DYN_WMETA)
    // our system
    return Gcd()->CheckNotificationReset(
#ifdef NR
        nr_meta
#endif
    );
#else
#ifdef NR
    (void)nr_meta;
#endif
    return true;
#endif
}

bool C3POHandle::CheckNotificationWrite(
#ifdef NR
    const NrFfi::NrMeta *nr_meta
#endif
) {
    // our system
    return Gcd()->CheckNotificationReset(
#ifdef NR
        nr_meta
#endif
    );
}

bool C3POHandle::CheckCoherence(const std::optional<size_t> &cn_index, uint32_t nid
#ifdef NR
                                ,
                                const NrFfi::NrMeta *nr_meta
#endif
) {
    (void)nid;
#if defined(SCR) && defined(DYN_WMETA)
    // our system
    if (!cn_index.has_value()) return true;
    return Gcd()->CheckCoherence(cn_index.value()
#ifdef NR
                                     ,
                                 nr_meta
#endif
    );
#else
    (void)cn_index;
    (void)nid;
#ifdef NR
    (void)nr_meta;
#endif
    return true;
#endif
}

std::optional<size_t> C3POHandle::CacheNodeIndexOnArrayIdx(const std::optional<common::GCDEntry> &entry_optional,
                                                           int nid) {
    if (!entry_optional.has_value()) return std::nullopt;
    return entry_optional->cn_array_[nid].cn_idx_;
}

std::optional<size_t> C3POHandle::CacheNodeIndexOnLogicalNode(const std::optional<common::GCDEntry> &entry_optional,
                                                              int nid) {
    return CacheNodeIndexOnArrayIdx(entry_optional, nid + 1);
}

std::optional<size_t> C3POHandle::CacheNodeIndexOnCxl(const std::optional<common::GCDEntry> &entry_optional) {
    return CacheNodeIndexOnArrayIdx(entry_optional, cxl_nid_);
}

/* disabled for logical nodes */
std::optional<size_t> C3POHandle::FindRemoteCacheNodeIndex(const std::optional<common::GCDEntry> &entry_optional,
                                                           int current_nid, int &remote_nid) {
    for (int i = 0; i < LOGICAL_NODE_NUM + 1; i++) {
        if (i != cxl_nid_ && i != current_nid && ExistOnArrayIdx(entry_optional, i)) {
            remote_nid = i;
            return CacheNodeIndexOnArrayIdx(entry_optional, i);
        }
    }
    return std::nullopt;
}

bool C3POHandle::read_seq_start(ReadHandle &rh) {
    std::optional<size_t> cn_index;
    std::optional<size_t> wmeta_index;

    if (!rh.from_cxl) return true;

    cn_index = CacheNodeIndexOnCxl(rh.entry_optional);
    if (!cn_index.has_value()) {
        set_read_handle_errno(rh, ReadErrno::PAGE_NOT_FOUND);
        return false;
    }

    wmeta_index = rh.entry_optional->wmeta_idx_;
    if (wmeta_index.has_value()) {  // RW mode: enable read sequence lock
        common::WriteMetadata *wmeta = c3po_->scr_meta_->GetWmeta(wmeta_index.value());
        uint32_t cxl_seq = wmeta->RSeqBegin();
#ifndef FULL_COHERENCE
        std::optional<SharedMetadata::LocalSeqcountT> local_seq =
            c3po_->scr_meta_->GetLocalSeqcount(wmeta_index.value(), rh.current_nid);
        if (local_seq != std::nullopt && local_seq != cxl_seq) {
#ifdef C3_RWLOCK
            (void)wmeta->RSeqRetry(0);  // explicitly unlock
#endif
            c3po_->scr_meta_->UpdateLocalSeqcount(wmeta_index.value(), rh.current_nid, std::nullopt);
            set_read_handle_errno(rh, ReadErrno::PAGE_INCOHERENT);
            return false;
        }
        // update local cache state
        if (local_seq != std::nullopt) {
            rh.is_local_cpu_cached = true;
        }
#endif
        rh.seqcount = cxl_seq;
    }
    return true;
}

bool C3POHandle::read_seq_end(ReadHandle &rh
#ifdef NR
                              ,
                              const NrFfi::NrMeta *nr_meta
#endif
) {
    std::optional<size_t> cn_index;
    std::optional<size_t> wmeta_index;

    if (!rh.from_cxl) return true;

    cn_index = CacheNodeIndexOnCxl(rh.entry_optional);
    if (!cn_index.has_value()) {
        set_read_handle_errno(rh, ReadErrno::PAGE_NOT_FOUND);
        return false;
    }

    if (!CheckNotification(
#ifdef NR
            nr_meta
#endif
            )) {
        DLOG(WARNING) << (rh.from_cxl ? "CXL" : "local") << " page @" << rh.cn_index.value() << " with block_id "
                      << rh.key << " is not coherent";
        set_read_handle_errno(rh, ReadErrno::META_OUTDATE);
        return false;
    }

    // if (h.seq_num check fail) {
    wmeta_index = rh.entry_optional->wmeta_idx_;
    if (!wmeta_index.has_value()) {
        // READ-ONLY page path - track active_node via CacheNode
#ifdef PARTITIONED_NODE
        common::CacheNode *cn = shared_cache_->cache_slot_.GetCacheNode(cn_index.value());
        uint8_t active_node = cn->GetActiveNode();

        if (active_node == rh.current_nid) {
            auto current_node_cnt = c3po_->scr_meta_->IncrementCurrentNodeCount(cn_index.value(), rh.current_nid) + 1;
            if (current_node_cnt == CONSECUTIVE_ACCESS_THRESHOLD) {
                if (partition_ratio_ > 0.0) {
                    off_t lower = static_cast<off_t>(
                        (static_cast<long double>(key_space_) * partition_ratio_ * rh.current_nid) / LOGICAL_NODE_NUM);
                    off_t upper = static_cast<off_t>(
                        (static_cast<long double>(key_space_) * partition_ratio_ * (rh.current_nid + 1)) /
                        LOGICAL_NODE_NUM);
                    if (rh.key.GetOffset() >= lower && rh.key.GetOffset() < upper) {
                        MoveToLocalReadOnly(rh, cn_index.value()
#ifdef NR
                                                    ,
                                            nr_meta
#endif
                        );
                    }
                }
            }
        } else {
            cn->SetActiveNode(static_cast<uint8_t>(rh.current_nid));
            c3po_->scr_meta_->ResetCurrentNodeCount(cn_index.value(), rh.current_nid);
        }
#endif
        return true;
    }
    common::WriteMetadata *wmeta = c3po_->scr_meta_->GetWmeta(wmeta_index.value());

    if (wmeta->RSeqRetry(static_cast<uint32_t>(rh.seqcount))) {
#ifndef FULL_COHERENCE
        c3po_->scr_meta_->UpdateLocalSeqcount(wmeta_index.value(), rh.current_nid, std::nullopt);
#endif
        set_read_handle_errno(rh, ReadErrno::PAGE_INCOHERENT);
        return false;
    }
#ifndef FULL_COHERENCE
    if (!rh.is_local_cpu_cached)
        c3po_->scr_meta_->UpdateLocalSeqcount(wmeta_index.value(), rh.current_nid, rh.seqcount);
#endif
#ifdef PARTITIONED_NODE
    common::CacheNode *cn = shared_cache_->cache_slot_.GetCacheNode(cn_index.value());
    uint8_t active_node = cn->GetActiveNode();
    // LOG(INFO) << "Read page " << rh.key << " from node " << active_node << " to node " << rh.current_nid;
    if (active_node == rh.current_nid) {
        c3po_->scr_meta_->IncrementCurrentNodeCount(cn_index.value(), rh.current_nid);
        if (c3po_->scr_meta_->GetCurrentNodeCount(cn_index.value(), rh.current_nid) > CONSECUTIVE_ACCESS_THRESHOLD) {
            // LOG(INFO) << "Replicating page " << rh.key << " to local node " << rh.current_nid
            //           << " (active node: " << active_node << ")";
            // Replicate hot page from shared CXL cache to local DRAM
            if (partition_ratio_ > 0.0) {
                off_t lower = static_cast<off_t>(
                    (static_cast<long double>(key_space_) * partition_ratio_ * rh.current_nid) / LOGICAL_NODE_NUM);
                off_t upper = static_cast<off_t>(
                    (static_cast<long double>(key_space_) * partition_ratio_ * (rh.current_nid + 1)) /
                    LOGICAL_NODE_NUM);

                if (rh.key.GetOffset() >= lower && rh.key.GetOffset() < upper) {
                    MoveToLocalReadOnly(rh, cn_index.value()
#ifdef NR
                                                ,
                                        nr_meta
#endif
                    );
                } else {
                    // LOG(FATAL) << "Page " << rh.key << " is not in the partition,"
                    //            << " key_offset=" << rh.key.GetOffset() << ", key_space_=" << key_space_
                    //            << ", partition_ratio_=" << partition_ratio_ << ", LOGICAL_NODE_NUM=" <<
                    //            LOGICAL_NODE_NUM
                    //            << ", current_nid=" << rh.current_nid
                    //            << ", active_node=" << static_cast<uint64_t>(active_node) << ", lower=" <<
                    //            lower
                    //            << ", upper=" << upper << ", threshold=" << CONSECUTIVE_ACCESS_THRESHOLD
                    //            << ", node_count="
                    //            << c3po_->scr_meta_->GetCurrentNodeCount(cn_index.value(), rh.current_nid);
                }
            }
        }
    } else {
        c3po_->scr_meta_->ResetCurrentNodeCount(cn_index.value(), rh.current_nid);
        cn->SetActiveNode(static_cast<uint8_t>(rh.current_nid));
    }
#endif
    return true;
}

bool C3POHandle::flush_seq_start(ReadHandle &rh) {
    std::optional<size_t> cn_index;
    std::optional<size_t> wmeta_index;

    if (!rh.from_cxl) return true;

    cn_index = CacheNodeIndexOnCxl(rh.entry_optional);
    if (!cn_index.has_value()) {
        // set_read_handle_errno(rh, ReadErrno::PAGE_NOT_FOUND);
        return false;
    }

    wmeta_index = rh.entry_optional->wmeta_idx_;
    // CHECK(wmeta_index.has_value());

    // TODO: should not hit this (we currently do not switch back to ro)
    if (!wmeta_index.has_value()) return false;

    common::WriteMetadata *wmeta = c3po_->scr_meta_->GetWmeta(wmeta_index.value());
    uint32_t cxl_seq = wmeta->RSeqBegin();
#ifndef FULL_COHERENCE
    std::optional<SharedMetadata::LocalSeqcountT> local_seq =
        c3po_->scr_meta_->GetLocalSeqcount(wmeta_index.value(), rh.current_nid);
    if (local_seq != std::nullopt && local_seq != cxl_seq) {
        // do not update local seq map because we do not flush page
        // set_read_handle_errno(rh, ReadErrno::PAGE_INCOHERENT);
        return false;
    }
#endif
    rh.seqcount = cxl_seq;
    return true;
}

bool C3POHandle::flush_seq_end(ReadHandle &rh) {
    std::optional<size_t> cn_index;
    std::optional<size_t> wmeta_index;

    if (!rh.from_cxl) return true;

    cn_index = CacheNodeIndexOnCxl(rh.entry_optional);
    CHECK(cn_index.has_value());

    wmeta_index = rh.entry_optional->wmeta_idx_;
    CHECK(wmeta_index.has_value());

    common::WriteMetadata *wmeta = c3po_->scr_meta_->GetWmeta(wmeta_index.value());

    if (wmeta->RSeqRetry(static_cast<uint32_t>(rh.seqcount))) {
        // do not update local seq map because we do not flush page
        // set_read_handle_errno(rh, ReadErrno::PAGE_INCOHERENT);
        return false;
    }
    return true;
}

// TODO: think about local locking
bool C3POHandle::write_seq_start(WriteHandle &wh
#ifdef NR
                                 ,
                                 const NrFfi::NrMeta *nr_meta
#endif
) {
    std::optional<size_t> cn_index;
    std::optional<size_t> wmeta_index;

    if (!wh.from_cxl) {
        // TODO: hold local locks
        return true;
    }

    cn_index = CacheNodeIndexOnCxl(wh.entry_optional);
    if (!cn_index.has_value()) {
        set_write_handle_errno(wh, WriteErrno::PAGE_NOT_FOUND);
        return false;
    }

    // grab the lock
    wmeta_index = wh.entry_optional->wmeta_idx_;
    if (!wmeta_index.has_value()) {
        set_write_handle_errno(wh, WriteErrno::PAGE_RO);
        return false;
    }
    common::WriteMetadata *wmeta = c3po_->scr_meta_->GetWmeta(wmeta_index.value());
    if (!wmeta->WSeqBegin()) {
        // for dynamic wmeta
        set_write_handle_errno(wh, WriteErrno::META_OUTDATE);
        return false;
    }

#ifdef NR
    if (!CheckNotificationWrite(nr_meta)) {
        wmeta->WSeqEnd();
        set_write_handle_errno(wh, WriteErrno::META_OUTDATE);
        return false;
    }
#else
    // recheck metadata update, prevent time-to-check and time-to-modify problem, use gcd_get again
    std::optional<common::GCDEntry> recheck_optional = Gcd()->Get(wh.key);
    if (!isEqual(recheck_optional, wh.entry_optional)) {
        (void)c3po_->WUnlock(cn_index.value());
        DLOG(WARNING) << " page @" << wh.cn_index.value() << " with block_id " << wh.key << " is not valid";
        set_write_handle_errno(wh, WriteErrno::META_OUTDATE);
        return false;
    }
#endif

    return true;
}

void C3POHandle::write_seq_end(WriteHandle &wh, const NrFfi::NrMeta *nr_meta) {
    std::optional<size_t> cn_index;
    std::optional<size_t> wmeta_index;

    if (!wh.from_cxl) {
        // TODO: release local locks
        return;
    }

    cn_index = CacheNodeIndexOnCxl(wh.entry_optional);
    if (!cn_index.has_value()) {
        LOG(FATAL) << "page evicted during write";
        set_write_handle_errno(wh, WriteErrno::PAGE_NOT_FOUND);
        return;
    }

    wmeta_index = wh.entry_optional->wmeta_idx_;
    if (!wmeta_index.has_value()) {
        LOG(FATAL) << "page switched to RO when locked";
        set_write_handle_errno(wh, WriteErrno::PAGE_RO);
        return;
    }

    common::WriteMetadata *wmeta = c3po_->scr_meta_->GetWmeta(wmeta_index.value());
    auto seq = wmeta->WSeqEnd();  // seqlock unlock

    /**
     * We discussed that this would be incorrect for the very first time, since the seqlock begins
     * at zero. I have changed my mind on this. Since correct operation is to use write_seq_begin
     * before write_seq_end, this would only ever see even numbers, not including zero. So the first
     * time this is called, seq should be == 2
     */
    if (seq == 0) {
        // generate log entry for overflow to let readers know their cached value could be stale
        // some_function(wh.key);
        c3po_->gcd_->SeqCountWrapAround(wh.key, nr_meta);
    }
// update local seq count
#ifndef FULL_COHERENCE
    c3po_->scr_meta_->UpdateLocalSeqcount(wmeta_index.value(), wh.current_nid, seq);
#endif
}

/**
 *  remove the gcd entry
 *  Return value:
 *  - old_wmeta_index (>=0): remove succeeds
 *  - nullopt: remove succeeds, but wmeta does not exist
 *  - NrGcdDeleteError::NR_GCD_DELETE_ENTRY_NOEXIST: remove fails, entry does not exist
 */
expected<std::optional<ssize_t>, NrGcdDeleteError> C3POHandle::Delete(const BlockId &to_remove
#ifdef NR
                                                                      ,
                                                                      const NrFfi::NrMeta *nr_meta
#endif
) {
    std::optional<ssize_t> old_wmeta_idx_optional;

    old_wmeta_idx_optional = c3po_->gcd_->Delete(to_remove
#ifdef NR
                                                 ,
                                                 nr_meta
#endif /* NR */
    );
    if (!old_wmeta_idx_optional.has_value()) {
        // Entry does not exist in GCD
        LOG(INFO) << "C3POHandle::Delete entry with block_id " << to_remove << "does not exist";
        return unexpected(NrGcdDeleteError::NR_GCD_DELETE_ENTRY_NOEXIST);
    }
    // Successfully deleted from GCD
    DLOG(INFO) << "CXL page with block_id " << to_remove << " deleted from GCD";
    if (old_wmeta_idx_optional.value() >= 0) {
        return old_wmeta_idx_optional.value();
    }
    return std::nullopt;
}

void C3POHandle::StartManager() {
#ifdef DYN_WMETA
    if (wmeta_mgr_) (*wmeta_mgr_)->Run();
#endif
}

void C3POHandle::StopManager() {
#ifdef DYN_WMETA
    if (wmeta_mgr_) (*wmeta_mgr_)->Shutdown();
#endif
}

}  // namespace rackobj::common