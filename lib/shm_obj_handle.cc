#include "shm_obj_handle.h"

#include "core/local_cache_node.h"
#include "globals.h"
#include "manager/evict_manager.h"
#include "manager/flush_manager.h"
#include "manager/replicate_manager.h"
#include "original_syscalls.h"

namespace rackobj::lib {

using common::ReadErrno;
using common::ReadHandle;
using common::ReadLocation;
using common::WriteErrno;
using common::WriteHandle;
// using common::CacheNodeHandle;

template <typename Policy>
SharedMemoryObjectHandle<Policy>::SharedMemoryObjectHandle(const int node, const RackOBJConfig cfg)
    : shared_cache_node_(node),
      reng_(rdev_()),
      // nid_dist_(1, NUM_NUMA),
      cfg_(cfg),
      read_admit_retry_count_{0},
      read_coherence_retry_count_{0} {
    // round up num_pages to the nearest multiple of 64
    size_t shared_num_slots = static_cast<size_t>((static_cast<size_t>(cfg.GetNumSlots()) + 63) & ~63ULL);
    CHECK(numa_available() != -1) << "libnuma not available";

    CHECK(NUM_NUMA > 1) << "There is only 1 NUMA node";
    size_t num_slots = shared_num_slots / static_cast<size_t>(LOGICAL_NODE_NUM);

    LOG(INFO) << "Initializing cache with num element " << shared_num_slots << " on node " << node;

    sc_shm_region_ = std::make_shared<common::AllocatableLocalMemoryRegion>(node, cfg.GetSCRSize());
    nc_shm_region_ = std::make_shared<common::AllocatableLocalMemoryRegion>(node, cfg.GetNCRSize());
    cache_shrd_ptr_ =
        common::SharedMemoryObject::CreateSharedMemoryObject(shared_num_slots, sc_shm_region_, nc_shm_region_);
    cache_ = cache_shrd_ptr_.get();
    c3po_ = C3POHandle::CreateOrMap(shared_num_slots, (void *)0x6f0000000000, NUMA_MEM, cache_->GetPageBaseAddr(),
                                    sc_shm_region_, cfg.GetLogicalSCRSize());
    c3po_->InitSharedMetadata(shared_num_slots, sc_shm_region_, &cache_shrd_ptr_->scr_bitmap_);
    c3po_->SetPartitionRatio(cfg.GetKeySpace(), cfg.GetPartitionRatio());

    // allocate local memory on each physical numa node except node 0 (memory node)
    for (int i = 0; i < NUM_NUMA; i++) {
        if (i == node) continue;
        local_region_[i] = std::make_shared<common::AllocatableLocalMemoryRegion>(i, cfg.GetLocalSize());
    }

    for (int i = 0; i < LOGICAL_NODE_NUM; i++) {
        // Allocate local cache for each logical node
        int physical_nid = RidToNumaNode(i);
        c3po_->CreateLocalSeqMap(i, local_region_[physical_nid]);
#ifdef PARTITIONED_NODE
        cache_lcl_ptr_[i] =
            common::LocalMemoryObject<Policy>::CreateLocalMemoryObject(num_slots, local_region_[physical_nid]);
#endif
    }
    c3po_->CreateWmetaMgr(0, local_region_[RidToNumaNode(0)]);  // just hardcode wmeta manager to logical node 0
    c3po_->SetSharedPageCache(cache_shrd_ptr_);
#ifdef PARTITIONED_NODE
    c3po_->SetCaches(cache_shrd_ptr_, cache_lcl_ptr_);
#endif

    if (cfg.DoReplication()) {
        // Allocate per-logical-node local caches and wire up replication managers.
        // This block is unreachable in PARTITIONED_NODE builds (config.cc LOGs FATAL first).
        for (int i = 0; i < LOGICAL_NODE_NUM; i++) {
            int physical_nid = RidToNumaNode(i);
            cache_lcl_ptr_[i] =
                common::LocalMemoryObject<Policy>::CreateLocalMemoryObject(num_slots, local_region_[physical_nid]);
        }
        c3po_->SetCaches(cache_shrd_ptr_, cache_lcl_ptr_);

        for (int i = 0; i < LOGICAL_NODE_NUM; i++) {
            int physical_nid = RidToNumaNode(i);
            repl_mgr_[i] = std::make_unique<common::ReplicateManager<Policy>>(
                i, cache_shrd_ptr_, local_region_[physical_nid], /*replication_enabled=*/true);
            repl_mgr_[i]->SetC3PO(c3po_.get());
            repl_mgr_[i]->SetLocalObj(cache_lcl_ptr_[i]);
            repl_mgr_[i]->SetEvictManager(nullptr);
            repl_mgr_[i]->Run();
        }
    }

    c3po_->StartManager();
}

template <typename Policy>
SharedMemoryObjectHandle<Policy>::~SharedMemoryObjectHandle() {
    if (cfg_.DoReplication()) {
        for (int i = 0; i < LOGICAL_NODE_NUM; i++) {
            if (repl_mgr_[i]) repl_mgr_[i]->Shutdown();
        }
    }
    c3po_->StopManager();

#ifdef DYN_WMETA
    size_t write_page_count = c3po_->Scr_meta()->GetWmetaCount();
    uint64_t reclaim_count = c3po_->Scr_meta()->GetRecycleCount();
    uint64_t allocate_count = c3po_->Scr_meta()->GetAllocateCount();

    LOG(INFO) << "seqlock alloc ratio: (" << write_page_count << "|" << c3po_->Scr_meta()->GetWmetaSlotLen() << ") = "
              << static_cast<double>(write_page_count) * 100 / static_cast<double>(c3po_->Scr_meta()->GetWmetaSlotLen())
              << "%, reclaim count: " << reclaim_count << ", allocate count: " << allocate_count;
#endif

    auto cachep = cache_shrd_ptr_.get();
    size_t nr_active = cachep->GetActivePageCount();
    size_t nr_total = cachep->page_count_;

    LOG(INFO) << "num pages cxl: (active|free) " << nr_active << " out of " << nr_total << " ("
              << nr_active * 100 / nr_total << "%)";

    /* disabled for logical nodes */
    // for (int i = 0; i < LOGICAL_NODE_NUM; i++) {
    //     size_t nr_active, nr_free, nr_total;
    //     common::BasePageCache *cachep;

    //     if (i == shared_cache_node_)
    //         cachep = cache_shrd_ptr_.get();
    //     else
    //         cachep = cache_lcl_ptr_[i].get();
    //     nr_active = cachep->GetActivePageCount();
    //     nr_total = cachep->page_count_;

    //     nr_free = 0;

    //     LOG(INFO) << "num pages " << (i == shared_cache_node_ ? "cxl" : "local") << " cache "
    //               << "on node " << i << ": (active|free) " << nr_active << "|" << nr_free << " out of " << nr_total
    //               << " (" << nr_active * 100 / nr_total << "%)";
    // }

    LOG(INFO) << "===========================================================================\n";
}

template <typename Policy>
void SharedMemoryObjectHandle<Policy>::StartReplMgr() {
    if (!cfg_.DoReplication()) {
        LOG(WARNING) << "StartReplMgr called but replication is not enabled";
        return;
    }
    for (int i = 0; i < LOGICAL_NODE_NUM; i++) {
        if (repl_mgr_[i]) repl_mgr_[i]->Run();
    }
}

template <typename Policy>
void SharedMemoryObjectHandle<Policy>::ClearReclaimCount() {
    c3po_->Scr_meta()->ResetReclaimCount();
}

template <typename Policy>
size_t SharedMemoryObjectHandle<Policy>::CopyToUserBufferCXL(const ReadHandle &rh, void *ptr, size_t count,
                                                             size_t offset_into_page) {
    common::CacheNode *cn;

    CHECK(rh.from_cxl) << "CacheNode from Local";

    cn = cache_shrd_ptr_->cache_slot_.GetCacheNode(rh.cn_index.value());
    cn->DecreaseEvict();

    // size_t bytes_read = std::min(count, cn->GetLength());
    size_t bytes_read = std::min(count, static_cast<size_t>(SLOT_SIZE));
    // DLOG(INFO) << "memcpy(" << (void*)ptr << ", "
    //             << (void*)(cn->GetDataSlot() + offset_into_page)
    //             << ", " << bytes_read << ")";
    memcpy(ptr, cache_shrd_ptr_->page_data_.GetDataSlot(rh.cn_index.value()) + offset_into_page, bytes_read);
    // memcpy(ptr, cache_shrd_ptr_->page_data_.GetDataSlot(rh.cn_index.value()) + offset_into_page,
    //        32);  // only reading 32 bytes

    return bytes_read;
}

template <typename Policy>
size_t SharedMemoryObjectHandle<Policy>::CopyToUserBufferLocal(const ReadHandle &rh, void *ptr, size_t count,
                                                               size_t offset_into_page) {
    common::LocalCacheNode *local_cn;

    CHECK(!rh.from_cxl) << "CacheNode from CXL";

    local_cn = reinterpret_cast<common::LocalCacheNode *>(rh.cn_index.value());
    local_cn->DecreaseEvict();
    size_t bytes_read = std::min(count, local_cn->GetLength());
    memcpy(ptr, local_cn->GetDataSlot() + offset_into_page, bytes_read);

    return bytes_read;
}

/** Need to be performed with wlock */
template <typename Policy>
size_t SharedMemoryObjectHandle<Policy>::CopyFromUserBufferCXL(const WriteHandle &wh, const void *ptr, size_t count,
                                                               size_t offset_into_page) {
    CHECK(wh.from_cxl) << "CacheNode from Local";

    size_t bytes_write = std::min(count, (size_t)SLOT_SIZE - offset_into_page);
    uint8_t *page = cache_shrd_ptr_->page_data_.GetDataSlot(wh.cn_index.value());
    memcpy(page + offset_into_page, ptr, bytes_write);

    // flush data
#ifndef FULL_COHERENCE
    c3po_->cache_flush(reinterpret_cast<char *>(page + offset_into_page), bytes_write);
#endif

    return bytes_write;
}

// TODO: implement
template <typename Policy>
size_t SharedMemoryObjectHandle<Policy>::CopyFromUserBufferLocal(const WriteHandle &wh, const void *ptr, size_t count,
                                                                 size_t offset_into_page) {
    (void)wh;
    (void)ptr;
    (void)count;
    (void)offset_into_page;
    LOG(FATAL) << "not implemented";

    // memcpy(ptr, local_cn->GetDataSlot() + offset_into_page, bytes_read);

    return 0;
}

#ifdef NR
template <typename Policy>
const NrFfi::NrMeta *SharedMemoryObjectHandle<Policy>::RegisterNRThread(int rid) {
    std::thread::id tid = std::this_thread::get_id();
    DLOG(INFO) << "register nr thread local, tid: " << tid;
    return c3po_->Gcd()->GetNrMetaTid(rid, true);
}

template <typename Policy>
void SharedMemoryObjectHandle<Policy>::UnRegisterNRThread(const NrFfi::NrMeta *nr_meta) {
    std::thread::id tid = std::this_thread::get_id();
    DLOG(INFO) << "unregister nr thread local, tid: " << tid;
    c3po_->Gcd()->UnRegisterThread(nr_meta, true);
}
#endif

[[maybe_unused]] static inline bool shouldReplicate(const uint8_t *ptr) {
#ifdef BTREE_REPLICATE
    uint8_t node_type = ptr[0];
    return (node_type == 0);
#else
    (void)ptr;
    return true;
#endif
}

template <typename Policy>
common::LocalMemoryObject<Policy> *SharedMemoryObjectHandle<Policy>::RegisterLocalMem(int logical_node_id) {
    std::thread::id tid = std::this_thread::get_id();
    uint64_t numa = logical_node_id;
    DLOG(INFO) << "thread " << tid << " register local cache on numa " << numa;
    return cache_lcl_ptr_[numa].get();
}

/* Admit to CXL first */
template <typename Policy>
expected<common::CacheNode *, std::error_code> SharedMemoryObjectHandle<Policy>::Admit(
    const common::BlockId &block_id, ThreadLocalMeta<Policy> *local_meta) {
    expected<common::CacheNode *, std::error_code> new_node_result;
    size_t new_index;
    common::CacheNode *new_node;
#ifdef NR
    auto nr_meta = local_meta->nr_meta_per_thread_;
#endif

    // LOG(INFO) << "cache miss";
    /* 1. Wait for free page available in free list */
    /* 2. free list remove cn */
    std::optional<size_t> new_cn_index_optional = cache_shrd_ptr_->scr_bitmap_.ReserveCacheNode();
    if (!new_cn_index_optional.has_value()) return unexpected(std::make_error_code(std::errc::not_enough_memory));
    new_index = new_cn_index_optional.value();

    CHECK(new_index < cache_shrd_ptr_->Size())
        << "Invalid cn_index: " << new_index << " cache size: " << cache_shrd_ptr_->Size();

    DLOG(INFO) << "Admit: Reserved @" << new_index << " from SharedBitmap"
               << "  ,block_id: " << block_id;

    new_node = cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index);

    /* 6. Fetch Content */

    if (local_meta->file_backed_) {
        ssize_t result = original_syscalls.pread(static_cast<int>(block_id.GetServerId()),
                                                 reinterpret_cast<char *>(cache_shrd_ptr_->GetDataSlot(new_index)),
                                                 static_cast<size_t>(SLOT_SIZE), block_id.GetOffset());
        if (reinterpret_cast<char *>(cache_shrd_ptr_->GetDataSlot(new_index)) == nullptr) {
            LOG(FATAL) << "Data slot is nullptr";
        }
        // LOG(INFO) << "pread requested: " << block_id.GetOffset() << " " << SLOT_SIZE << " " << block_id.GetServerId()
        //           << " fd: " << static_cast<int>(block_id.GetServerId()) << " result: " << result;
        if (result > 0) {
            new_node->SetLength(static_cast<size_t>(result));
        } else {
            LOG(FATAL) << "Error: " << std::strerror(errno) << " attempting to read non existent block " << block_id;
        }
    }

    // 7. cn set blockid
    new_node->Reinitialize(block_id);
    cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, false);
#ifndef FULL_COHERENCE
    c3po_->cache_flush(reinterpret_cast<char *>(cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index)),
                       sizeof(common::CacheNode));
#endif

    /* 9. CheckInsert GCD */
#ifdef DYN_WMETA
    common::NrGcdError error = c3po_->Gcd()->CheckAndInsert(block_id, new_index, shared_cache_node_
#ifdef NR
                                                            ,
                                                            nr_meta
#endif
    );
#else
    std::optional<size_t> new_wmeta_index_optional = new_index;
    common::NrGcdError error = c3po_->Gcd()->CheckAndInsert(block_id, new_index, shared_cache_node_
#ifdef NR
                                                            ,
                                                            nr_meta
#endif
                                                            ,
                                                            new_wmeta_index_optional);
#endif

    // RecycleCacheNode if CheckAndInsert fails
    switch (error) {
        case common::NrGcdError::GCD_NO_ERROR:
            local_meta->pt_stat.admit_cnt++;
            break;
        case common::NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED:
            // free allocated wmeta
        case common::NrGcdError::GCD_SLOT_UPDATE_FAILED:
            DLOG(WARNING) << "Admit: GCD page @" << new_node << " checkinsert failed: concurrent insertion to node "
                          << shared_cache_node_ << " ,block_id: " << block_id;

            /* 10. free list insert cn */
            cache_shrd_ptr_->scr_bitmap_.RecycleCacheNode(new_index);
            // std::optional<common::GCDEntry> entry_optional = c3po_->Gcd()->Get(block_id, nr_meta);
            // print_entry(&(*entry_optional));
            new_node_result = unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
            goto failed;
        default:
    }

    /* TODO: enable evict manager for logical node */
    // evict_mgr_[local_meta->logical_node_id_]->insert_shared(new_index);

    new_node_result = new_node;
failed:
    return new_node_result;
}

/**
 * Admit to CXL first
 * different from admit to CXL, initialize page in rw shared
 * input: const uint8_t* write_data
 *     optimization for special case when full-page aligned page is written
 *     no need to bring page from disk cuz anyway overwritten
 *     but we cannot allow read to happen after admit before the write (garbage read)
 *     Solution: directly memcpy write data in admission
 *     if write_data != nullptr indicates full-page aligned write*/
template <typename Policy>
expected<common::CacheNode *, std::error_code> SharedMemoryObjectHandle<Policy>::AdmitWrite(
    const common::BlockId &block_id, const uint8_t *write_data, ThreadLocalMeta<Policy> *local_meta) {
    expected<common::CacheNode *, std::error_code> new_node_result;
    std::optional<size_t> new_wmeta_index_optional = std::nullopt;
    size_t new_index;
    common::CacheNode *new_node;
#ifdef NR
    auto nr_meta = local_meta->nr_meta_per_thread_;
#else
    (void)local_meta;
#endif

    /* 1. Wait for free page available in free list */
    std::optional<size_t> new_cn_index_optional = cache_shrd_ptr_->scr_bitmap_.ReserveCacheNode();
    if (!new_cn_index_optional.has_value()) return unexpected(std::make_error_code(std::errc::not_enough_memory));
    new_index = new_cn_index_optional.value();

    CHECK(new_index < cache_shrd_ptr_->Size())
        << "Invalid cn_index: " << new_index << " cache size: " << cache_shrd_ptr_->Size();

    DLOG(INFO) << "Admit: Reserved @" << new_index << " from SharedBitmap"
               << "  ,block_id: " << block_id;

    new_node = cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index);

#ifdef DYN_WMETA
    do {
        // new_wmeta_index_optional = c3po_->Scr_meta()->ReserveWmeta(block_id, true, new_wmeta_index_optional);
        // new_wmeta_index_optional =
        //     c3po_->Scr_meta()->ReserveWmetaEarlyTerm(block_id, 100, true, new_wmeta_index_optional);
        new_wmeta_index_optional = c3po_->Scr_meta()->CheckReserveWmeta(block_id, true, new_wmeta_index_optional);
        if (new_wmeta_index_optional.has_value()) break;

        // critical path reclamation
        new_wmeta_index_optional = c3po_->Scr_meta()->SampleVictim(5);
        if (!new_wmeta_index_optional.has_value()) continue;

        common::WriteMetadata *cur = c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value());
        common::BlockId vic_block_id = cur->GetBlockID();

        new_wmeta_index_optional = c3po_->Gcd()->SwitchToReadOnly(vic_block_id
#ifdef NR
                                                                  ,
                                                                  nr_meta
#endif /* NR */
        );
        if (new_wmeta_index_optional.has_value()) {
            c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value());
        } else {
            cur->WUnlockOnly();
        }

        cpu_relax();
    } while (true);
#else
    new_wmeta_index_optional = new_index;
    c3po_->Scr_meta()->GetWmeta(new_index)->WSeqBegin();
#endif

    /* 6. Fetch Content */
    if (write_data) {
        size_t slot_size = (size_t)SLOT_SIZE;
        memcpy(static_cast<void *>(cache_shrd_ptr_->GetDataSlot(new_index)), write_data, slot_size);
#ifndef FULL_COHERENCE
        c3po_->cache_flush(reinterpret_cast<char *>(cache_shrd_ptr_->GetDataSlot(new_index)), slot_size);
#endif
        new_node->SetLength(slot_size);
        DLOG(INFO) << "admit write wout fetch block";
    } else {
        if (local_meta->file_backed_) {
            ssize_t result = original_syscalls.pread(static_cast<int>(block_id.GetServerId()),
                                                     reinterpret_cast<char *>(cache_shrd_ptr_->GetDataSlot(new_index)),
                                                     static_cast<size_t>(SLOT_SIZE), block_id.GetOffset());
            if (result > 0) {
                new_node->SetLength(static_cast<size_t>(result));
            } else {
                LOG(ERROR) << "Error: " << std::strerror(errno) << " attempting to read non existent block "
                           << block_id;
            }
            DLOG(INFO) << "admit write fetch block from disk";
        }
    }

    // 7. cn set valid & clean
    new_node->Reinitialize(block_id);
    cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, write_data != nullptr);
#ifndef FULL_COHERENCE
    c3po_->cache_flush(reinterpret_cast<char *>(cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index)),
                       sizeof(common::CacheNode));
#endif

    /* 8. CheckInsert GCD */
#ifdef NR
    common::NrGcdError error =
        c3po_->Gcd()->CheckAndInsert(block_id, new_index, shared_cache_node_, nr_meta, new_wmeta_index_optional);
#else
    common::NrGcdError error =
        c3po_->Gcd()->CheckAndInsert(block_id, new_index, shared_cache_node_, new_wmeta_index_optional);
#endif

    // RecycleCacheNode if CheckAndInsert fails
    switch (error) {
        case common::NrGcdError::GCD_NO_ERROR:
            break;
        case common::NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED:
            c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), true);
        case common::NrGcdError::GCD_SLOT_UPDATE_FAILED:
            DLOG(WARNING) << "Admit: GCD page @" << new_node << " checkinsert failed: concurrent insertion to node "
                          << shared_cache_node_ << " ,block_id: " << block_id;

            /* 10. free list insert cn */
            c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), true);
            cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, false);
            cache_shrd_ptr_->scr_bitmap_.RecycleCacheNode(new_index);
            // std::optional<common::GCDEntry> entry_optional = c3po_->Gcd()->Get(block_id, nr_meta);
            // print_entry(&(*entry_optional));
            new_node_result = unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
            goto failed;
        default:
    }

    c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value())->WSeqEnd();
    new_node_result = new_node;
failed:
    return new_node_result;
}

/** KV-Store functions */
template <typename Policy>
expected<common::CacheNode *, std::error_code> SharedMemoryObjectHandle<Policy>::CreateEntry(
    const common::BlockId &block_id, const uint8_t *write_data, size_t count, ThreadLocalMeta<Policy> *local_meta) {
    expected<common::CacheNode *, std::error_code> new_node_result;
    std::optional<size_t> new_wmeta_index_optional = std::nullopt;
    size_t new_index;
    common::CacheNode *new_node;
#ifdef NR
    auto nr_meta = local_meta->nr_meta_per_thread_;
#else
    (void)local_meta;
#endif

    /* 1. Wait for free page available in free list */
    std::optional<size_t> new_cn_index_optional = cache_shrd_ptr_->scr_bitmap_.ReserveCacheNode();
    if (!new_cn_index_optional.has_value()) return unexpected(std::make_error_code(std::errc::not_enough_memory));
    new_index = new_cn_index_optional.value();

    CHECK(new_index < cache_shrd_ptr_->Size())
        << "Invalid cn_index: " << new_index << " cache size: " << cache_shrd_ptr_->Size();

    DLOG(INFO) << "Admit: Reserved @" << new_index << " from SharedBitmap"
               << "  ,block_id: " << block_id;

    new_node = cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index);

    // special case: if write_data nullptr, then initialize entry in RO mode
    if (write_data) {
#ifdef DYN_WMETA
        do {
            new_wmeta_index_optional = c3po_->Scr_meta()->CheckReserveWmeta(block_id, true, new_wmeta_index_optional);
            if (new_wmeta_index_optional.has_value()) break;

            // critical path reclamation
            new_wmeta_index_optional = c3po_->Scr_meta()->SampleVictim(MAX_WMETA_SAMPLING_SIZE);
            if (!new_wmeta_index_optional.has_value()) continue;

            common::WriteMetadata *cur = c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value());
            common::BlockId vic_block_id = cur->GetBlockID();

            new_wmeta_index_optional = c3po_->Gcd()->SwitchToReadOnly(vic_block_id
#ifdef NR
                                                                      ,
                                                                      nr_meta
#endif /* NR */
            );
            if (new_wmeta_index_optional.has_value()) {
                c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value());
            } else {
                cur->WUnlockOnly();
            }

            cpu_relax();
        } while (true);
#else
        new_wmeta_index_optional = new_index;
        c3po_->Scr_meta()->GetWmeta(new_index)->WSeqBegin();
#endif

        /* 6. Fetch Content */
        size_t slot_size = (size_t)SLOT_SIZE;
        memcpy(static_cast<void *>(cache_shrd_ptr_->GetDataSlot(new_index)), write_data, count);
#ifndef FULL_COHERENCE
        c3po_->cache_flush(reinterpret_cast<char *>(cache_shrd_ptr_->GetDataSlot(new_index)), count);
#endif
        new_node->SetLength(slot_size);
        DLOG(INFO) << "admit write wout fetch block";
    }

    // 7. cn set valid & clean
    new_node->Reinitialize(block_id);
    cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, write_data != nullptr);
#ifndef FULL_COHERENCE
    c3po_->cache_flush(reinterpret_cast<char *>(cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index)),
                       sizeof(common::CacheNode));
#endif

    /* 8. CheckInsert GCD */
#ifdef NR
    common::NrGcdError error =
        c3po_->Gcd()->CheckAndInsert(block_id, new_index, shared_cache_node_, nr_meta, new_wmeta_index_optional);
#else
    common::NrGcdError error =
        c3po_->Gcd()->CheckAndInsert(block_id, new_index, shared_cache_node_, new_wmeta_index_optional);
#endif

    // RecycleCacheNode if CheckAndInsert fails
    switch (error) {
        case common::NrGcdError::GCD_NO_ERROR:
            break;
        case common::NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED:
            c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), true);
        case common::NrGcdError::GCD_SLOT_UPDATE_FAILED:
            DLOG(WARNING) << "Admit: GCD page @" << new_node << " checkinsert failed: concurrent insertion to node "
                          << shared_cache_node_ << " ,block_id: " << block_id;

            /* 10. free list insert cn */
            c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), true);
            cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, false);
            cache_shrd_ptr_->scr_bitmap_.RecycleCacheNode(new_index);
            new_node_result = unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
            goto failed;
        default:
    }

    if (write_data) c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value())->WSeqEnd();
    new_node_result = new_node;
failed:

    return new_node_result;
}

template <typename Policy>
std::error_code SharedMemoryObjectHandle<Policy>::SwitchRW(const common::WriteHandle &wh,
                                                           ThreadLocalMeta<Policy> *local_meta) {
    std::optional<size_t> new_wmeta_index_optional = std::nullopt;
#ifdef NR
    auto nr_meta = local_meta->nr_meta_per_thread_;
#endif
#ifdef DYN_WMETA
    do {
        // new_wmeta_index_optional = c3po_->Scr_meta()->ReserveWmeta(wh.key, true, new_wmeta_index_optional);
        // new_wmeta_index_optional =
        //     c3po_->Scr_meta()->ReserveWmetaEarlyTerm(wh.key, 100, true, new_wmeta_index_optional);
        new_wmeta_index_optional = c3po_->Scr_meta()->CheckReserveWmeta(wh.key, true, new_wmeta_index_optional);
        if (new_wmeta_index_optional.has_value()) break;

        // critical path reclamation
        new_wmeta_index_optional = c3po_->Scr_meta()->SampleVictim(MAX_WMETA_SAMPLING_SIZE);
        if (!new_wmeta_index_optional.has_value()) continue;

        common::WriteMetadata *cur = c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value());
        common::BlockId vic_block_id = cur->GetBlockID();

        new_wmeta_index_optional = c3po_->Gcd()->SwitchToReadOnly(vic_block_id
#ifdef NR
                                                                  ,
                                                                  nr_meta
#endif /* NR */
        );
        if (new_wmeta_index_optional.has_value()) {
            c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value());
        } else {
            cur->WUnlockOnly();
        }

        cpu_relax();
    } while (true);
#else
    new_wmeta_index_optional = wh.cn_index;
#endif

#ifdef NR
    size_t rc = c3po_->Gcd()->InvalidateSwitchToRWShared(wh.key, new_wmeta_index_optional.value(), nr_meta);
#else
    (void)local_meta;
    size_t rc = c3po_->Gcd()->InvalidateSwitchToRWShared(wh.key, new_wmeta_index_optional.value());
#endif

    if (rc == common::NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED) {
        c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), true);
    } else if (rc == common::NrGcdError::GCD_ENTRY_NOEXIST) {
        c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), true);
        return std::make_error_code(std::errc::operation_not_permitted);
    } else {
        c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value())->WSeqEnd();
    }

    return std::error_code{};
}

template <typename Policy>
ReadLocation SharedMemoryObjectHandle<Policy>::selectCacheNode(ReadHandle &rh) {
    if (c3po_->IsEntryEmpty(rh.entry_optional)) { /* Cache miss: GCD get failed */
        set_read_handle_errno(rh, ReadErrno::PAGE_NOT_FOUND);
        return ReadLocation::NO_ENTRY;
    } else if (c3po_->ExistOnLogicalNode(rh.entry_optional, rh.current_nid)) {
        rh.cn_index = c3po_->CacheNodeIndexOnLogicalNode(rh.entry_optional, rh.current_nid);
        DLOG_IF(INFO, !rh.cn_index.has_value()) << "CacheNode not found on current node";
        if (c3po_->ExistOnCxl(rh.entry_optional)) {
            return ReadLocation::LOCAL_NODE;
        } else {
            return ReadLocation::LOCAL_NODE_EXCLUSIVE;
        }
    } else if (c3po_->ExistOnCxl(rh.entry_optional)) {
        // GCDEntry hit --> Page exists on CXL
        // potentially replicated on other node, but not on this NUMA Node
        rh.cn_index = c3po_->CacheNodeIndexOnCxl(rh.entry_optional);
        DLOG_IF(INFO, !rh.cn_index.has_value()) << "CacheNode not found on CXL";
        rh.from_cxl = true;
        return ReadLocation::CXL_SHARED;
    } else {
        // GCDEntry hit --> Page exists on other node,
        // but not on this NUMA Node or CXL
        /* Request other node to move page to CXL */

        // rh.cn_index = c3po_->FindRemoteCacheNodeIndex(rh.entry_optional, rh.current_nid, rh.remote_nid);
        // DLOG_IF(INFO, !rh.cn_index.has_value()) << "CacheNode not found on remote node";
        return ReadLocation::REMOTE_NODE;
    }
}

/**
 * case 1: no entry -> PAGE_NOT_FOUND
 * case 2: RO with multiple replicas -> MULTIPLE_REPLICA (SwitchRW will invalidate non-CXL entries)
 * case 3: RO single replica -> PAGE_RO (SwitchRW promotes to RW)
 * case 4: RW shared (CXL) -> no error, write to CXL
 * case 5: local exclusive -> no error, write locally
 */
template <typename Policy>
void SharedMemoryObjectHandle<Policy>::checkCacheNode(WriteHandle &wh) {
    if (c3po_->IsEntryEmpty(wh.entry_optional)) {
        set_write_handle_errno(wh, WriteErrno::PAGE_NOT_FOUND);
    } else if (c3po_->IsRO(wh.entry_optional)) {
        wh.cn_index = c3po_->CacheNodeIndexOnCxl(wh.entry_optional);
        wh.from_cxl = true;
        if (c3po_->CheckReplicas(wh.entry_optional)) {
            // Multiple replicas exist; all must be RO (invariant: non-CXL replica only added in RO mode).
            DLOG_IF(WARNING, false) << "MULTIPLE_REPLICA detected for block " << wh.key;
            set_write_handle_errno(wh, WriteErrno::MULTIPLE_REPLICA);
        } else {
            set_write_handle_errno(wh, WriteErrno::PAGE_RO);
        }
    } else {
        bool on_cxl = c3po_->ExistOnCxl(wh.entry_optional);
        if (on_cxl)
            wh.cn_index = c3po_->CacheNodeIndexOnCxl(wh.entry_optional);
        else
            wh.cn_index = c3po_->CacheNodeIndexOnLogicalNode(wh.entry_optional, wh.current_nid);
        wh.from_cxl = on_cxl;
    }
}

/**
 * assumes there is local exclusive copy of the page, need to hold the local lock before read
 */
template <typename Policy>
expected<size_t, ReadErrno> SharedMemoryObjectHandle<Policy>::ReadFromLocalExclusive(
    struct ReadHandle &rh, uint8_t *ptr, size_t count, size_t offset_into_page, ThreadLocalMeta<Policy> *local_meta) {
    (void)local_meta;  // Unused for local reads
    size_t bytes_read = 0;
    common::LocalCacheNode *local_cn = reinterpret_cast<common::LocalCacheNode *>(rh.cn_index.value());

    // Start read sequence (wait until sequence is even = no active writer)
    uint32_t seq = local_cn->RSeqBegin();

    // Read page data
    bytes_read = std::min(count, local_cn->GetLength());
    if (bytes_read == 0) {
        bytes_read = SLOT_SIZE;  // Default to full slot if length not set
    }
    memcpy(ptr, local_cn->GetDataSlot() + offset_into_page, bytes_read);

    // Update eviction counter
    local_cn->DecreaseEvict();

    // Check if sequence changed during read (retry if concurrent write)
    if (local_cn->RSeqRetry(seq)) {
        set_read_handle_errno(rh, ReadErrno::PAGE_INCOHERENT);
        return unexpected(rh.rh_errno);
    }

    return bytes_read;
}

// TODO: change size_t offset_into_page to off_t
template <typename Policy>
expected<size_t, ReadErrno> SharedMemoryObjectHandle<Policy>::ReadFromLocal(struct ReadHandle &rh, uint8_t *ptr,
                                                                            size_t count, size_t offset_into_page,
                                                                            ThreadLocalMeta<Policy> *local_meta) {
    size_t bytes_read = 0;
#ifdef NR
    auto nr_meta = local_meta->nr_meta_per_thread_;
#else
    (void)local_meta;
#endif
    DLOG(INFO) << "Cache hit: GCD get succeed, Accessing Local page!";

    /* 4. read page (memcpy) */
    bytes_read = CopyToUserBufferLocal(rh, ptr, count, offset_into_page);

    /* GCD recheck via NR notification */
    if (!c3po_->CheckNotification(
#ifdef NR
            nr_meta
#endif
            )) {
        set_read_handle_errno(rh, ReadErrno::PAGE_INCOHERENT);
        return unexpected(rh.rh_errno);
    }

    if (bytes_read == 0) {
        LOG(WARNING) << "bytes_read=0: block_id= " << rh.key << ", cn_index=@" << rh.cn_index.value();
    }
    return bytes_read;
}

// _mm_clflushopt: switch back to _mm_clflush if hardware does not support -mclflushopt
template <typename Policy>
inline void SharedMemoryObjectHandle<Policy>::cache_flush_data(const struct ReadHandle &rh) {
#if defined(SCR) || defined(NO_COHERENCE)
    const size_t slot_size = (size_t)SLOT_SIZE;
    void *page = cache_shrd_ptr_->page_data_.GetDataSlot(rh.cn_index.value());
    c3po_->cache_flush(reinterpret_cast<char *>(page), slot_size);
#endif
}

template <typename Policy>
inline void SharedMemoryObjectHandle<Policy>::cache_flush_metadata(const struct ReadHandle &rh) {
#if defined(SCR) || defined(NO_COHERENCE)
    common::CacheNode *cn = cache_shrd_ptr_->cache_slot_.GetCacheNode(rh.cn_index.value());
    c3po_->cache_flush(reinterpret_cast<char *>(cn), sizeof(common::CacheNode));
#endif
}

template <typename Policy>
expected<size_t, ReadErrno> SharedMemoryObjectHandle<Policy>::ReadFromCXL(struct ReadHandle &rh, uint8_t *ptr,
                                                                          size_t count, size_t offset_into_page,
                                                                          ThreadLocalMeta<Policy> *local_meta) {
    size_t bytes_read = 0;
#ifdef NR
    auto nr_meta = local_meta->nr_meta_per_thread_;
#else
    (void)local_meta;
#endif
    DLOG(INFO) << "Cache hit: GCD get succeed, Accessing CXL page!";

    /* read seq begin */
    // C3PO API
    if (!c3po_->read_seq_start(rh
#ifdef NR
                               ,
                               nr_meta
#endif
                               )) {
        return unexpected(rh.rh_errno);
    }
    // cache_->policy_.Touch(*entry_optional);
    /* 4. read page (memcpy) */
    bytes_read = CopyToUserBufferCXL(rh, ptr, count, offset_into_page);

    /* read seq check retry */
    // C3PO API
    if (!c3po_->read_seq_end(rh
#ifdef NR
                             ,
                             nr_meta
#endif
                             )) {
        return unexpected(rh.rh_errno);
    }

    /* 6. Enqueue for background replication if enabled, RO, no local replica yet, and counter triggered */
    if (cfg_.DoReplication() && c3po_->IsRO(rh.entry_optional) &&
        !c3po_->ExistOnLogicalNode(rh.entry_optional, rh.current_nid) && repl_mgr_[rh.current_nid] &&
        repl_mgr_[rh.current_nid]->ShouldReplicate(rh.cn_index.value())) {
        if (shouldReplicate(ptr)) {
            bool succeed = repl_mgr_[rh.current_nid]->enqueue(rh.key, rh.cn_index.value());
            if (!succeed) repl_mgr_[rh.current_nid]->ClearReplicate(rh.cn_index.value());
        }
    }

    return bytes_read;
}

// TODO: think about local locking
template <typename Policy>
expected<size_t, WriteErrno> SharedMemoryObjectHandle<Policy>::WriteToLocal(struct WriteHandle &wh, const uint8_t *ptr,
                                                                            size_t count, size_t offset_into_page,
                                                                            ThreadLocalMeta<Policy> *local_meta) {
    (void)local_meta;  // Unused for local writes
    size_t bytes_write = 0;
    common::LocalCacheNode *local_cn = reinterpret_cast<common::LocalCacheNode *>(wh.cn_index.value());

    // Acquire write lock (increments sequence, sets LOCK_BIT)
    if (!local_cn->WSeqBegin()) {
        // Lock acquisition failed (should not happen for local exclusive pages)
        set_write_handle_errno(wh, WriteErrno::META_OUTDATE);
        return unexpected(wh.wh_errno);
    }

    // Copy data to local page buffer
    bytes_write = std::min(count, (size_t)SLOT_SIZE - offset_into_page);
    uint8_t *page = local_cn->GetDataSlot();
    memcpy(page + offset_into_page, ptr, bytes_write);

    // NOTE: No cache flush needed - local DRAM is coherent
    // NOTE: No dirty tracking - local exclusive pages not backed to disk

    // Release write lock (increments sequence, clears LOCK_BIT)
    local_cn->WSeqEnd();

    // Update eviction counter
    local_cn->DecreaseEvict();

    if (bytes_write == 0) LOG(WARNING) << "bytes_write=0";
    return bytes_write;
}

template <typename Policy>
expected<size_t, WriteErrno> SharedMemoryObjectHandle<Policy>::WriteToCXL(struct WriteHandle &wh, const uint8_t *ptr,
                                                                          size_t count, size_t offset_into_page,
                                                                          ThreadLocalMeta<Policy> *local_meta) {
    size_t bytes_write = 0;
#ifdef NR
    auto nr_meta = local_meta->nr_meta_per_thread_;
#else
    (void)local_meta;
#endif

    /* write seq begin */
    // C3PO API
    if (!c3po_->write_seq_start(wh
#ifdef NR
                                ,
                                nr_meta
#endif
                                ))
        return unexpected(wh.wh_errno);

    // TODO: update eviction count

    /* 4. write page (memcpy) */
    bytes_write = CopyFromUserBufferCXL(wh, ptr, count, offset_into_page);

    cache_shrd_ptr_->scr_bitmap_.SetDirty(wh.cn_index.value(), true);

    // C3PO API
    c3po_->write_seq_end(wh, nr_meta);

    common::CacheNode *cn = cache_shrd_ptr_->cache_slot_.GetCacheNode(wh.cn_index.value());
    cn->DecreaseEvict();

    if (bytes_write == 0) LOG(WARNING) << "bytes_write=0";
    return bytes_write;
}

template <typename Policy>
size_t SharedMemoryObjectHandle<Policy>::Read(const common::BlockId &block_id, uint8_t *ptr, size_t count,
                                              size_t offset_into_page, ThreadLocalMeta<Policy> *local_meta) {
    struct ReadHandle rh;
    std::optional<common::GCDEntry> entry_optional;
    uint64_t admit_retry_cnt = 0;
    uint64_t coherence_retry_cnt[LOGICAL_NODE_NUM] = {0};  // for retry on coherence failure
    uint64_t c_retry = 0;
    bool did_retry = false;
    local_meta->pt_stat.read_cnt++;
#ifdef NR
    auto nr_meta = local_meta->nr_meta_per_thread_;
#endif
    if (offset_into_page + count > common::BlockId::kBlockSize) {
        LOG(FATAL) << "SharedMemoryObjectHandle::Read attempting to read " << count << " bytes at an offset of "
                   << offset_into_page << " into the page (exceeds page limit)";
    }

retry:
    size_t bytes_read = 0;

    entry_optional = c3po_->Gcd()->Get(block_id
#ifdef NR
                                       ,
                                       nr_meta
#endif
    );

    init_read_handle(rh, block_id, entry_optional, local_meta->logical_node_id_);

    expected<size_t, ReadErrno> result;
    switch (selectCacheNode(rh)) {
        case ReadLocation::CXL_SHARED:
            result = ReadFromCXL(rh, ptr, count, offset_into_page, local_meta);
            break;
        case ReadLocation::LOCAL_NODE:
            result = ReadFromLocal(rh, ptr, count, offset_into_page, local_meta);
            break;
        case ReadLocation::LOCAL_NODE_EXCLUSIVE:
            result = ReadFromLocalExclusive(rh, ptr, count, offset_into_page, local_meta);
            break;
        case ReadLocation::REMOTE_NODE: { /* Cache hit: Page exists on other node, but not on this NUMA Node or CXL */
            LOG(FATAL) << "Move Remote Page to CXL not implemented";
            break;
        }
        case ReadLocation::NO_ENTRY: { /* Cache miss: GCD get failed */
            DLOG(INFO) << "Cache miss: GCD get failed, ADMIT new page!";
            auto admit_result = Admit(block_id, local_meta);
            admit_retry_cnt++;
            goto retry;
        }
        case ReadLocation::INVALID:
            LOG(FATAL) << "ReadLocation not initialized";
    }

    if (!result.has_value()) {
        switch (rh.rh_errno) {
            case ReadErrno::NO_ERROR:
                break;
            case ReadErrno::META_OUTDATE:
                goto retry;
            case ReadErrno::PAGE_INCOHERENT:
                if (rh.from_cxl) {
#ifndef FULL_COHERENCE
                    cache_flush_data(rh);
#endif
                }
                coherence_retry_cnt[rh.from_cxl ? 0 : rh.current_nid]++;
                c_retry++;
                did_retry = true;
                DLOG(WARNING) << "Read: failed due to cache_flush, retry_cnt: "
                              << coherence_retry_cnt[rh.from_cxl ? 0 : rh.current_nid];
                goto retry;
            default:
                LOG(FATAL) << "Wrong error code";
        }
    }
    bytes_read = result.value();

    (void)admit_retry_cnt;

    local_meta->pt_stat.read_retry_cnt += c_retry;
    if (did_retry) local_meta->pt_stat.read_retry_invocation++;
    return bytes_read;
}

template <typename Policy>
size_t SharedMemoryObjectHandle<Policy>::Get(const common::BlockId &block_id, uint8_t *ptr, size_t count,
                                             size_t offset_into_page, ThreadLocalMeta<Policy> *local_meta) {
    struct ReadHandle rh;
    std::optional<common::GCDEntry> entry_optional;
    uint64_t admit_retry_cnt = 0;
    uint64_t coherence_retry_cnt[LOGICAL_NODE_NUM] = {0};  // for retry on coherence failure
#ifdef NR
    auto nr_meta = local_meta->nr_meta_per_thread_;
#endif

    if (offset_into_page + count > common::BlockId::kBlockSize) {
        LOG(FATAL) << "SharedMemoryObjectHandle::Read attempting to read " << count << " bytes at an offset of "
                   << offset_into_page << " into the page (exceeds page limit)";
    }

retry:
    size_t bytes_read = 0;

    entry_optional = c3po_->Gcd()->Get(block_id
#ifdef NR
                                       ,
                                       nr_meta
#endif
    );

    init_read_handle(rh, block_id, entry_optional, local_meta->logical_node_id_);

    expected<size_t, ReadErrno> result;
    switch (selectCacheNode(rh)) {
        case ReadLocation::CXL_SHARED:
            result = ReadFromCXL(rh, ptr, count, offset_into_page, local_meta);
            break;
        case ReadLocation::LOCAL_NODE:
            result = ReadFromLocal(rh, ptr, count, offset_into_page, local_meta);
            break;
        case ReadLocation::LOCAL_NODE_EXCLUSIVE:
            result = ReadFromLocalExclusive(rh, ptr, count, offset_into_page, local_meta);
            break;
        case ReadLocation::REMOTE_NODE: { /* Cache hit: Page exists on other node, but not on this NUMA Node or CXL */
            /* Request other node to move page to CXL */
            LOG(FATAL) << "Move Remote Page to CXL not implemented";
            break;
        }
        case ReadLocation::NO_ENTRY: { /* Cache miss: GCD get failed */
            return 0;
        }
        case ReadLocation::INVALID:
            LOG(FATAL) << "ReadLocation not initialized";
    }

    if (!result.has_value()) {
        switch (rh.rh_errno) {
            case ReadErrno::NO_ERROR:
                break;
            case ReadErrno::META_OUTDATE:
                goto retry;
            case ReadErrno::PAGE_INCOHERENT:
                if (rh.from_cxl) {
#ifndef FULL_COHERENCE
                    cache_flush_data(rh);
#endif
                }
                coherence_retry_cnt[rh.from_cxl ? 0 : rh.current_nid]++;
                DLOG(WARNING) << "Read: failed due to cache_flush, retry_cnt: "
                              << coherence_retry_cnt[rh.from_cxl ? 0 : rh.current_nid];
                goto retry;
            default:
                LOG(FATAL) << "Wrong error code";
        }
    }
    bytes_read = result.value();
    (void)admit_retry_cnt;

    return bytes_read;
}

/**
 * Write:
 * Steps:
 * 1. gcd get
 * 2. from c3po errno: check if multiple replica
 *   * if local only:
 *         use local locks
 * 3. collapse if multiple replica; retry
 * 4. from c3po errno: check if rw shared
 *   * switch to rw if not; retry
 * 5. write seq
 *   * (sol 2): lock object, recheck gcd
 * 6. memcpy
 * 7. write end
 * (5, 6, 7): packed in WriteToCXL
 */
template <typename Policy>
size_t SharedMemoryObjectHandle<Policy>::Write(const common::BlockId &block_id, const uint8_t *ptr, size_t count,
                                               size_t offset_into_page, ThreadLocalMeta<Policy> *local_meta) {
    struct WriteHandle wh;
    std::optional<common::GCDEntry> entry_optional;
    uint64_t retry_cnt = 0;
#ifdef NR
    auto nr_meta = local_meta->nr_meta_per_thread_;
#endif

    if (offset_into_page + count > common::BlockId::kBlockSize) {
        LOG(FATAL) << "SharedMemoryObjectHandle::Write attempting to write " << count << " bytes at an offset of "
                   << offset_into_page << " into the page (exceeds page limit)";
    }

retry:
    size_t bytes_write = 0;
#ifdef NR
    entry_optional = c3po_->Gcd()->GetWithLock(block_id, nr_meta);
#else
    entry_optional = c3po_->Gcd()->GetAnchor(block_id);
#endif

    init_write_handle(wh, block_id, entry_optional, local_meta->logical_node_id_);

    checkCacheNode(wh);
    // TODO: think about local only copy
    // check for errors
    switch (wh.wh_errno) {
        case WriteErrno::NO_ERROR: {
            DLOG(INFO) << "cache hit with no error";
            break;
        }
        case WriteErrno::PAGE_NOT_FOUND: {  // Cache miss
            DLOG(INFO) << "page not found";
#ifdef NR
            if (wh.from_cxl) c3po_->Gcd()->ReleaseSeqLock(block_id, nr_meta);
#endif
            const uint8_t *to_write;
            if (count != (size_t)SLOT_SIZE)
                to_write = nullptr;
            else
                to_write = ptr;
            auto result = AdmitWrite(block_id, to_write, local_meta);
            if (result.error() == std::make_error_code(std::errc::not_enough_memory)) return bytes_write;
            if (!to_write) {
                retry_cnt++;
                goto retry;
            }
            return (size_t)SLOT_SIZE;  // write wout fetch block
        }
        case WriteErrno::MULTIPLE_REPLICA:
        case WriteErrno::PAGE_RO: {
#ifdef NR
            // AllLog: no RO/RW state; all pages have a GCD seqlock.
            // Release the lock acquired via GetWithLock before retrying.
            c3po_->Gcd()->ReleaseSeqLock(block_id, nr_meta);
            retry_cnt++;
            goto retry;
#else
            DLOG(INFO) << "page RO";
            auto ec = SwitchRW(wh, local_meta);
            if (ec == std::make_error_code(std::errc::not_enough_memory)) return bytes_write;
            retry_cnt++;
            goto retry;
#endif
        }
        case WriteErrno::MULTIPLE_REPLICA_NOT_ON_CXL: {
            // need to first move local to cxl and then invalidate
            LOG(FATAL) << "MULTIPLE_REPLICA_NOT_ON_CXL not handled";
            break;
        }
        case WriteErrno::PAGE_ON_REMOTE_NODE: {
            LOG(FATAL) << "Move Remote Page to CXL not implemented";
            break;
        }
        default:
            LOG(FATAL) << "Unknown error code";
    }

    expected<size_t, WriteErrno> result;

    if (wh.from_cxl)
        result = WriteToCXL(wh, ptr, count, offset_into_page, local_meta);
    else
        result = WriteToLocal(wh, ptr, count, offset_into_page, local_meta);

    if (!result.has_value()) {
        // metadata out of date
        retry_cnt++;
        goto retry;
    } else {
        bytes_write = *result;
    }

    DLOG(INFO) << "num of retries: " << retry_cnt;
    return bytes_write;
}

template <typename Policy>
size_t SharedMemoryObjectHandle<Policy>::Put(const common::BlockId &block_id, const uint8_t *ptr, size_t count,
                                             size_t offset_into_page, ThreadLocalMeta<Policy> *local_meta) {
    struct WriteHandle wh;
    std::optional<common::GCDEntry> entry_optional;
    uint64_t retry_cnt = 0;
#ifdef NR
    auto nr_meta = local_meta->nr_meta_per_thread_;
#endif

    if (offset_into_page + count > common::BlockId::kBlockSize) {
        LOG(FATAL) << "SharedMemoryObjectHandle::Write attempting to write " << count << " bytes at an offset of "
                   << offset_into_page << " into the page (exceeds page limit)";
    }

retry:
    size_t bytes_write = 0;

#ifdef NR
    entry_optional = c3po_->Gcd()->GetWithLock(block_id, nr_meta);
#else
    entry_optional = c3po_->Gcd()->GetAnchor(block_id);
#endif

    init_write_handle(wh, block_id, entry_optional, local_meta->logical_node_id_);

    checkCacheNode(wh);
    // TODO: think about local only copy
    // check for errors
    switch (wh.wh_errno) {
        case WriteErrno::NO_ERROR: {
            DLOG(INFO) << "cache hit with no error";
            break;
        }
        case WriteErrno::PAGE_NOT_FOUND: {  // Cache miss
#ifdef NR
            if (wh.from_cxl) c3po_->Gcd()->ReleaseSeqLock(block_id, nr_meta);
#endif
            auto result = CreateEntry(block_id, ptr, count, local_meta);
            if (result.error() == std::make_error_code(std::errc::not_enough_memory)) return bytes_write;
            return count;
        }
        case WriteErrno::MULTIPLE_REPLICA:
        case WriteErrno::PAGE_RO: {
#ifdef NR
            c3po_->Gcd()->ReleaseSeqLock(block_id, nr_meta);
            retry_cnt++;
            goto retry;
#else
            DLOG(INFO) << "page RO";
            auto ec = SwitchRW(wh, local_meta);
            if (ec == std::make_error_code(std::errc::not_enough_memory)) return bytes_write;
            retry_cnt++;
            goto retry;
#endif
        }
        case WriteErrno::MULTIPLE_REPLICA_NOT_ON_CXL: {
            // need to first move local to cxl and then invalidate
            LOG(FATAL) << "MULTIPLE_REPLICA_NOT_ON_CXL not handled";
            break;
        }
        case WriteErrno::PAGE_ON_REMOTE_NODE: {
            LOG(FATAL) << "Move Remote Page to CXL not implemented";
            break;
        }
        default:
            LOG(FATAL) << "Unknown error code";
    }

    expected<size_t, WriteErrno> result;

    if (wh.from_cxl)
        result = WriteToCXL(wh, ptr, count, offset_into_page, local_meta);
    else
        result = WriteToLocal(wh, ptr, count, offset_into_page, local_meta);

    if (!result.has_value()) {
        retry_cnt++;
        goto retry;
    } else {
        bytes_write = *result;
    }

    DLOG(INFO) << "num of retries: " << retry_cnt;
    return bytes_write;
}

template class SharedMemoryObjectHandle<CurrPolicy>;

}  // namespace rackobj::lib

// #endif  // SHM_PAGE_CACHE_HANDLE_HPP
