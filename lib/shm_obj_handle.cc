#include "shm_obj_handle.h"

#include <tuple>

#include "common/partitions.h"
#include "core/cc_primitive.h"
#include "core/local_cache_node.h"
#include "globals.h"
#include "ipc/constants.h"
#include "ipc/tigon_client.hpp"
#include "ipc/tigon_server.hpp"
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
      cfg_(cfg),
      partition_to_shared_{},
      shared_to_partition_{},
      read_admit_retry_count_{0},
      read_coherence_retry_count_{0} {
    // round up num_pages to the nearest multiple of 64
    size_t shared_num_slots = static_cast<size_t>((static_cast<size_t>(cfg.GetNumSlots()) + 63) & ~63ULL);
    CHECK(numa_available() != -1) << "libnuma not available";

    CHECK((size_t)NUM_NUMA > 1) << "There is only 1 NUMA node";
    size_t num_slots = shared_num_slots / static_cast<size_t>(LOGICAL_NODE_NUM);

    LOG(INFO) << "Initializing cache with num element " << shared_num_slots << " on node " << node;

    sc_shm_region_ = std::make_shared<common::AllocatableLocalMemoryRegion>(node, cfg.GetSCRSize());
    // keep passing this region around to the servers
    nc_shm_region_ = std::make_shared<common::AllocatableLocalMemoryRegion>(node, cfg.GetNCRSize());
    cache_shrd_ptr_ =
        common::SharedMemoryObject::CreateSharedMemoryObject(shared_num_slots, sc_shm_region_, nc_shm_region_);
    cache_ = cache_shrd_ptr_.get();
    c3po_ = C3POHandle::CreateOrMap(shared_num_slots, (void *)0x6f0000000000, NUMA_MEM, cache_->GetPageBaseAddr(),
                                    sc_shm_region_, cfg.GetLogicalSCRSize());
    c3po_->InitSharedMetadata(shared_num_slots, sc_shm_region_, &cache_shrd_ptr_->scr_bitmap_);

    // allocate local memory for each numa node except node 0 (memory node)
    for (int i = 0; i < NUM_NUMA; i++) {
        if (i == node) continue;
        local_region_[i] = std::make_shared<common::AllocatableLocalMemoryRegion>(i, cfg.GetLocalSize());
    }

    for (int i = 0; i < LOGICAL_NODE_NUM; i++) {
        int target_numa_node = RidToNumaNode(i);
        lcds_[i] =
            common::LocalCacheDirectoryHandle::Create(num_slots, nullptr, i, nullptr, local_region_[target_numa_node]);
        cache_lcl_ptr_[i] =
            common::LocalMemoryObject<Policy>::CreateLocalMemoryObject(num_slots, local_region_[target_numa_node]);
        c3po_->CreateLocalSeqMap(i, local_region_[target_numa_node]);
    }
    c3po_->SetSharedPageCache(cache_shrd_ptr_);

    // Init servers and clients for logical nodes
    for (int i = 0; i < LOGICAL_NODE_NUM; i++) {
        ipc_servers[i] = std::make_shared<common::TigonIPCServer<Policy>>(this, i, sc_shm_region_);
        ipc_servers[i]->Run();
    }
    for (int i = 0; i < LOGICAL_NODE_NUM; i++) {
        for (int j = 0; j < LOGICAL_NODE_NUM; j++) {
            if (i == j) {
                continue;
            }
            std::shared_ptr<common::TigonIPCServer<Policy>> server = ipc_servers[j];
            uint8_t *server_buf = server->GetMemRegion();
            ipc_clients[i][j] = std::make_shared<rackobj::common::TigonIPCClient>(server_buf);
        }
    }

    c3po_->StartManager();
}

template <typename Policy>
SharedMemoryObjectHandle<Policy>::~SharedMemoryObjectHandle() {
    for (int i = 0; i < LOGICAL_NODE_NUM; i++) {
        if (ipc_servers[i]) ipc_servers[i]->Shutdown();
    }
    c3po_->StopManager();

#ifdef DYN_WMETA
    size_t write_page_count = c3po_->Scr_meta()->GetWmetaCount();

    LOG(INFO) << "seqlock alloc ratio: (" << write_page_count << "|" << cache_shrd_ptr_->page_count_ << ") = "
              << static_cast<double>(write_page_count) * 100 / static_cast<double>(cache_shrd_ptr_->page_count_) << "%";
#endif

    for (int i = 0; i < LOGICAL_NODE_NUM + 1; i++) {
        size_t nr_active, nr_free, nr_total;
        common::BasePageCache *cachep;
        int rid = i - 1;

        if (i == shared_cache_node_)
            cachep = cache_shrd_ptr_.get();
        else
            cachep = cache_lcl_ptr_[rid].get();
        nr_active = cachep->GetActivePageCount();
        nr_total = cachep->page_count_;

        nr_free = 0;

        if (i == shared_cache_node_) {
            LOG(INFO) << "num pages cxl cache : (active|free) " << nr_active << "|" << nr_free << " out of " << nr_total
                      << " (" << nr_active * 100 / nr_total << "%)";
        } else {
            LOG(INFO) << "num pages local on node " << rid << ": (active|free) " << nr_active << "|" << nr_free
                      << " out of " << nr_total << " (" << nr_active * 100 / nr_total << "%)"
                      << "\n"
                      << "partition_to_shared: " << partition_to_shared_[rid].value
                      << ", shared_to_partition_: " << shared_to_partition_[rid].value;
        }
    }
    LOG(INFO) << "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++";
}

template <typename Policy>
void SharedMemoryObjectHandle<Policy>::StartReplMgr() {
    for (int i = 0; i < NUM_NUMA; i++) {
        if (i == shared_cache_node_) continue;
        repl_mgr_[i]->Run();
    }
}

template <typename Policy>
size_t SharedMemoryObjectHandle<Policy>::CopyToUserBufferCXL(const ReadHandle &rh, void *ptr, size_t count,
                                                             size_t offset_into_page) {
    common::CacheNode *cn;

    CHECK(rh.from_cxl) << "CacheNode from Local";

    if (!cfg_.GetMoveToShare()) {
    } else {
        cn = cache_shrd_ptr_->cache_slot_.GetCacheNode(rh.cn_index.value());
        cn->DecreaseEvict();
    }

    // size_t bytes_read = std::min(count, cn->GetLength());
    size_t bytes_read = std::min(count, static_cast<size_t>(SLOT_SIZE));
    memcpy(ptr, cache_shrd_ptr_->page_data_.GetPage(rh.cn_index.value()) + offset_into_page, bytes_read);

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
    memcpy(ptr, local_cn->GetPage() + offset_into_page, bytes_read);

    return bytes_read;
}

/** Need to be performed with wlock */
template <typename Policy>
size_t SharedMemoryObjectHandle<Policy>::CopyFromUserBufferCXL(const WriteHandle &wh, const void *ptr, size_t count,
                                                               size_t offset_into_page) {
    CHECK(wh.from_cxl) << "CacheNode from Local";

    size_t bytes_write = std::min(count, static_cast<size_t>(SLOT_SIZE) - offset_into_page);
    uint8_t *page = cache_shrd_ptr_->page_data_.GetPage(wh.cn_index.value());
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

    // memcpy(ptr, local_cn->GetPage() + offset_into_page, bytes_read);

    return 0;
}

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
common::LocalMemoryObject<Policy> *SharedMemoryObjectHandle<Policy>::RegisterLocalMem() {
    std::thread::id tid = std::this_thread::get_id();
    uint64_t numa = GetCurrentNuma();
    DLOG(INFO) << "thread " << tid << " register local cache on numa " << numa;
    return cache_lcl_ptr_[numa].get();
}

/** KV-Store functions */
template <typename Policy>
expected<common::CacheNode *, std::error_code> SharedMemoryObjectHandle<Policy>::CreatePage(
    const common::BlockId &block_id, const uint8_t *write_data, size_t count, ThreadLocalMeta<Policy> *local_meta) {
    expected<common::CacheNode *, std::error_code> new_node_result;
    std::optional<size_t> new_wmeta_index_optional = std::nullopt;
    size_t new_index;
    common::CacheNode *new_node;
    (void)local_meta;

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
        new_wmeta_index_optional =
            c3po_->Scr_meta()->CheckReserveWmeta(block_id, true, PLACEHOLDER_0, new_wmeta_index_optional);
        if (new_wmeta_index_optional.has_value()) break;

        // critical path reclamation
        new_wmeta_index_optional = c3po_->Scr_meta()->SampleVictim(MAX_WMETA_SAMPLING_SIZE, PLACEHOLDER_0);
        if (!new_wmeta_index_optional.has_value()) continue;

        common::WriteMetadata *cur = c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value());
        common::BlockId vic_block_id = cur->GetBlockID();

        new_wmeta_index_optional = c3po_->Gcd()->SwitchToReadOnly(vic_block_id);
        if (new_wmeta_index_optional.has_value()) {
            c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), PLACEHOLDER_0);
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
        size_t slot_size = static_cast<size_t>(SLOT_SIZE);
        memcpy(static_cast<void *>(cache_shrd_ptr_->GetPage(new_index)), write_data, count);
#ifndef FULL_COHERENCE
        c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->GetPage(new_index)), count);
#endif
        new_node->SetLength(slot_size);
        DLOG(INFO) << "admit write wout fetch block";
    }

    // 7. cn set valid & clean
    new_node->Reinitialize(block_id);
    cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, write_data != nullptr);
#ifndef FULL_COHERENCE
    c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index)),
                       sizeof(common::CacheNode));
#endif

    /* 8. CheckInsert GCD */
    common::NrGcdError error =
        c3po_->Gcd()->CheckAndInsert(block_id, new_index, shared_cache_node_, new_wmeta_index_optional);

    // RecycleCacheNode if CheckAndInsert fails
    switch (error) {
        case common::NrGcdError::GCD_NO_ERROR:
            break;
        case common::NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED:
            c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), PLACEHOLDER_0, true);
        case common::NrGcdError::GCD_SLOT_UPDATE_FAILED:
            DLOG(WARNING) << "Admit: GCD page @" << new_node << " checkinsert failed: concurrent insertion to node "
                          << shared_cache_node_ << " ,block_id: " << block_id;

            /* 10. free list insert cn */
            c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), PLACEHOLDER_0, true);
            cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, false);
            cache_shrd_ptr_->scr_bitmap_.RecycleCacheNode(new_index);
            new_node_result = unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
            goto failed;
        default:
    }

    c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value())->WSeqEnd();
    new_node_result = new_node;
failed:

    return new_node_result;
}

/**
 * @brief Use Admit to preload an entry in CXL if the key belongs to this node's partition
 * @return CacheNode* on successful preload, out-of-memory, or operation-not-permitted if the key
 * does not belong to this partition
 */
template <typename Policy>
expected<common::CacheNode *, std::error_code> SharedMemoryObjectHandle<Policy>::PreloadAdmit(
    const common::BlockId &block_id, const uint8_t *write_data, size_t count, ThreadLocalMeta<Policy> *local_meta) {
    (void)local_meta;
    int my_partition = local_meta->logical_node_id_;
    bool my_key = is_my_key(block_id.GetOffset(), my_partition);
    bool move_to_share = cfg_.GetMoveToShare();
    if (my_key && move_to_share) {
        // move to share, so things are default allocated to local memory

        // allocate local object slot
        std::shared_ptr<rackobj::common::LocalMemoryObject<Policy>> my_memory = cache_lcl_ptr_[my_partition];
        common::LocalCacheNode *new_node = my_memory->policy_.ReserveCacheNode();
        if (new_node == nullptr) return unexpected(std::make_error_code(std::errc::not_enough_memory));

        /**
         * TODO: do I need to do new_node->SetSize()?
         */

        // write data to slot
        if (write_data) {
            memcpy(static_cast<void *>(new_node->GetPage()), write_data, count);
            // NO WRITE FLUSH HERE, local mem is always coherent
        }

        // create entry in local index for the object
        auto result = lcds_[my_partition]->Insert(block_id, reinterpret_cast<size_t>(new_node));
        CHECK(result) << "insert to local index failed on node " << my_partition;

        return new_node;
    } else if (my_key) {
        // without moving to share, so things are shared by default
        size_t new_index;
        common::CacheNode *new_node;
        std::optional<size_t> new_cn_index_optional = cache_shrd_ptr_->scr_bitmap_.ReserveCacheNode();
        if (!new_cn_index_optional.has_value()) return unexpected(std::make_error_code(std::errc::not_enough_memory));

        new_index = new_cn_index_optional.value();
        CHECK(new_index < cache_shrd_ptr_->Size())
            << "Invalid cn_index: " << new_index << " cache size: " << cache_shrd_ptr_->Size();

        new_node = cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index);

        if (write_data) {
            memcpy(static_cast<void *>(cache_shrd_ptr_->GetPage(new_index)), write_data, count);
            c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->GetPage(new_index)), count);
        }

        size_t slot_size = static_cast<size_t>(SLOT_SIZE);
        new_node->SetLength(slot_size);
        new_node->Reinitialize(block_id);
        cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, write_data != nullptr);

        c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index)),
                           sizeof(common::CacheNode));

        auto result = lcds_[my_partition]->Insert(block_id, new_index);  // admit to local index
        CHECK(result) << "insert to local index failed on node " << my_partition;

        if (!result) {
            // recycle slot
            cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, false);
            cache_shrd_ptr_->scr_bitmap_.RecycleCacheNode(new_index);
            return unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
        }

        return new_node;
    } else {
        // key not in my partition
        return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
}

template <typename Policy>
std::error_code SharedMemoryObjectHandle<Policy>::SwitchRW(const common::WriteHandle &wh,
                                                           ThreadLocalMeta<Policy> *local_meta) {
    std::optional<size_t> new_wmeta_index_optional = std::nullopt;
#ifdef DYN_WMETA
    do {
        // new_wmeta_index_optional = c3po_->Scr_meta()->ReserveWmeta(wh.key, true, new_wmeta_index_optional);
        // new_wmeta_index_optional =
        //     c3po_->Scr_meta()->ReserveWmetaEarlyTerm(wh.key, 100, true, new_wmeta_index_optional);
        new_wmeta_index_optional =
            c3po_->Scr_meta()->CheckReserveWmeta(wh.key, PLACEHOLDER_0, true, new_wmeta_index_optional);
        if (new_wmeta_index_optional.has_value()) break;

        // critical path reclamation
        new_wmeta_index_optional = c3po_->Scr_meta()->SampleVictim(MAX_WMETA_SAMPLING_SIZE, PLACEHOLDER_0);
        if (!new_wmeta_index_optional.has_value()) continue;

        common::WriteMetadata *cur = c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value());
        common::BlockId vic_block_id = cur->GetBlockID();

        new_wmeta_index_optional = c3po_->Gcd()->SwitchToReadOnly(vic_block_id);
        if (new_wmeta_index_optional.has_value()) {
            c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), PLACEHOLDER_0);
        } else {
            cur->WUnlockOnly();
        }

        cpu_relax();
    } while (true);
#else
    new_wmeta_index_optional = wh.cn_index;
#endif

    (void)local_meta;
    size_t rc = c3po_->Gcd()->InvalidateSwitchToRWShared(wh.key, new_wmeta_index_optional.value());

    if (rc == common::NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED) {
        c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), PLACEHOLDER_0, true);
    } else if (rc == common::NrGcdError::GCD_ENTRY_NOEXIST) {
        c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), PLACEHOLDER_0, true);
        return std::make_error_code(std::errc::operation_not_permitted);
    } else {
        c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value())->WSeqEnd();
    }

    return std::error_code{};
}

template <typename Policy>
ReadLocation SharedMemoryObjectHandle<Policy>::selectCacheNode(ReadHandle &rh) {
    (void)rh;
    LOG(ERROR) << "not implemented";
    return ReadLocation::INVALID;
}

template <typename Policy>
void SharedMemoryObjectHandle<Policy>::checkCacheNode(WriteHandle &wh) {
    (void)wh;
    LOG(ERROR) << "not implemented";
}

// TODO: change size_t offset_into_page to off_t
template <typename Policy>
expected<size_t, ReadErrno> SharedMemoryObjectHandle<Policy>::ReadFromLocal(struct ReadHandle &rh, uint8_t *ptr,
                                                                            size_t count, size_t offset_into_page,
                                                                            ThreadLocalMeta<Policy> *local_meta) {
    size_t bytes_read = 0;
    (void)local_meta;
    DLOG(INFO) << "Cache hit: GCD get succeed, Accessing CXL page!";

    /* read seq begin */
    // local index
    rh.seqcount = read_seqlock_begin(&rh.lmeta->get().seqlock_);
    /* 4. read page (memcpy) */
    bytes_read = CopyToUserBufferLocal(rh, ptr, count, offset_into_page);

    if (read_seqlock_retry(&rh.lmeta->get().seqlock_, static_cast<uint32_t>(rh.seqcount))) {
        set_read_handle_errno(rh, ReadErrno::PAGE_INCOHERENT);
    }

    // LOG_IF(WARNING, bytes_read == 0) << "bytes_read=0";
    return bytes_read;
}

// _mm_clflushopt: switch back to _mm_clflush if hardware does not support -mclflushopt
template <typename Policy>
inline void SharedMemoryObjectHandle<Policy>::cache_flush_data(const struct ReadHandle &rh) {
#if defined(SCR) || defined(NO_COHERENCE)
    const size_t slot_size = static_cast<size_t>(SLOT_SIZE);
    void *page = cache_shrd_ptr_->page_data_.GetPage(rh.cn_index.value());
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
    (void)local_meta;
    DLOG(INFO) << "Cache hit: GCD get succeed, Accessing CXL page!";

    bool from_cxl_tmp = rh.from_cxl;
    rh.from_cxl = true;  // need to set this temporality to aviod sanity checks
    /* read seq begin */
    // C3PO API
    if (from_cxl_tmp) {
        if (!c3po_->read_seq_start(rh)) {
            return unexpected(rh.rh_errno);
        }
    } else {
        // local index
        rh.seqcount = read_seqlock_begin(&rh.lmeta->get().seqlock_);
    }
    // cache_->policy_.Touch(*entry_optional);
    /* 4. read page (memcpy) */
    bytes_read = CopyToUserBufferCXL(rh, ptr, count, offset_into_page);

    if (from_cxl_tmp) {
        if (!c3po_->read_seq_end(rh)) {
            return unexpected(rh.rh_errno);
        }
    } else {
        if (read_seqlock_retry(&rh.lmeta->get().seqlock_, static_cast<uint32_t>(rh.seqcount))) {
            set_read_handle_errno(rh, ReadErrno::PAGE_INCOHERENT);
        }
    }

    rh.from_cxl = from_cxl_tmp;

    // LOG_IF(WARNING, bytes_read == 0) << "bytes_read=0";
    return bytes_read;
}

// TODO: think about local locking
template <typename Policy>
expected<size_t, WriteErrno> SharedMemoryObjectHandle<Policy>::WriteToLocal(const struct WriteHandle &wh,
                                                                            const uint8_t *ptr, size_t count,
                                                                            size_t offset_into_page,
                                                                            ThreadLocalMeta<Policy> *local_meta) {
    (void)wh;
    (void)ptr;
    (void)count;
    (void)offset_into_page;
    (void)local_meta;

    LOG(FATAL) << "not implemented";
    return (size_t)0;
}

template <typename Policy>
expected<size_t, WriteErrno> SharedMemoryObjectHandle<Policy>::WriteToCXL(struct WriteHandle &wh, const uint8_t *ptr,
                                                                          size_t count, size_t offset_into_page,
                                                                          ThreadLocalMeta<Policy> *local_meta) {
    size_t bytes_write = 0;
    (void)local_meta;

    bool from_cxl_tmp = wh.from_cxl;
    wh.from_cxl = true;  // need to set this temporality to aviod sanity checks

    /* write seq begin */
    // C3PO API
    if (from_cxl_tmp) {
        if (!c3po_->write_seq_start(wh)) return unexpected(wh.wh_errno);
    } else {
        if (!write_seqlock_begin(&wh.lmeta->get().seqlock_)) {
            return unexpected(WriteErrno::META_OUTDATE);
        }
    }

    /* 4. write page (memcpy) */
    bytes_write = CopyFromUserBufferCXL(wh, ptr, count, offset_into_page);

    cache_shrd_ptr_->scr_bitmap_.SetDirty(wh.cn_index.value(), true);

    if (from_cxl_tmp) {
        c3po_->write_seq_end(wh);
    } else {
        write_seqlock_end(&wh.lmeta->get().seqlock_);
    }

    wh.from_cxl = from_cxl_tmp;

    LOG_IF(WARNING, bytes_write == 0) << "bytes_write=0";
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
    uint8_t partition_num = whose_key(block_id.GetOffset());
    uint8_t my_partition = static_cast<uint8_t>(local_meta->logical_node_id_);
    bool move_to_share = cfg_.GetMoveToShare();

    local_meta->pt_stat.read_cnt++;
    if (offset_into_page + count > common::BlockId::kBlockSize) {
        LOG(FATAL) << "SharedMemoryObjectHandle::Read attempting to read " << count << " bytes at an offset of "
                   << offset_into_page << " into the page (exceeds page limit)";
    }

retry:
    size_t bytes_read = 0;

    init_read_handle(rh, block_id, my_partition);

    if (partition_num == my_partition) {
        // Owned partition: check local cache directory
        auto lcd_entry_opt = lcds_[partition_num]->Get(block_id);

        if (!lcd_entry_opt.has_value()) {
            DLOG(INFO) << "Cache miss: LCD entry not found, ADMIT new page!";
            if (move_to_share) {
                std::shared_ptr<rackobj::common::LocalMemoryObject<Policy>> my_memory = cache_lcl_ptr_[my_partition];
                common::LocalCacheNode *new_node = my_memory->policy_.ReserveCacheNode();
                CHECK(new_node != nullptr)
                    << "(Ran out of memory?) reserving local index on partition " << my_partition << " with read";

                if (local_meta->file_backed_) {
                    size_t slot_size = static_cast<size_t>(SLOT_SIZE);
                    ssize_t result = original_syscalls.pread(static_cast<int>(block_id.GetServerId()),
                                                             reinterpret_cast<char *>(new_node->page_ptr_), slot_size,
                                                             block_id.GetOffset());
                    if (result <= 0) {
                        LOG(ERROR) << "Error reading from disk: " << std::strerror(errno);
                    }
                }

                lcds_[partition_num]->Insert(block_id, reinterpret_cast<size_t>(new_node));
                admit_retry_cnt++;
                goto retry;
            } else {
                size_t new_index;
                common::CacheNode *new_node;
                std::optional<size_t> new_cn_index_optional = cache_shrd_ptr_->scr_bitmap_.ReserveCacheNode();
                CHECK(new_cn_index_optional.has_value())
                    << "(Ran out of memory?) inserting to local index on partition " << my_partition << " with read";

                new_index = new_cn_index_optional.value();
                CHECK(new_index < cache_shrd_ptr_->Size())
                    << "Invalid cn_index: " << new_index << " cache size: " << cache_shrd_ptr_->Size();

                new_node = cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index);

                if (local_meta->file_backed_) {
                    size_t slot_size = static_cast<size_t>(SLOT_SIZE);
                    ssize_t result = original_syscalls.pread(
                        static_cast<int>(block_id.GetServerId()),
                        reinterpret_cast<char *>(cache_shrd_ptr_->page_data_.GetPage(new_index)), slot_size,
                        block_id.GetOffset());
                    if (result > 0) {
                        new_node->SetLength(static_cast<size_t>(result));
                    } else {
                        LOG(ERROR) << "Error reading from disk: " << std::strerror(errno);
                    }
                }

                size_t slot_size = static_cast<size_t>(SLOT_SIZE);
                new_node->SetLength(slot_size);
                new_node->Reinitialize(block_id);
                cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, 0);

                c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index)),
                                   sizeof(common::CacheNode));

                auto result = lcds_[my_partition]->Insert(block_id, new_index);
                CHECK(result) << "insert to local index failed on node " << my_partition;

                admit_retry_cnt++;
                goto retry;
            }
        }

        common::LCDEntry &lmeta = lcd_entry_opt->get();
        if (!lmeta.wmeta_idx_.has_value()) {
            // Local exclusive: not shared
            update_read_handle(rh, block_id, lmeta);
            rh.cn_index = lmeta.cn_idx_.value();
        } else {
            // Shared: entry has wmeta_idx
            rh.entry_optional = common::GCDEntry{.wmeta_idx_ = lmeta.wmeta_idx_.value(), .cn_array_ = {}};
            if (cfg_.GetMoveToShare()) {
                rh.cn_index = lmeta.wmeta_idx_.value();
            } else {
                rh.cn_index = lmeta.cn_idx_.value();
            }
        }
    } else {
        // Remote partition: check global cache directory
        entry_optional = c3po_->Gcd()->GetAnchor(block_id);
        update_read_handle(rh, block_id, entry_optional);

        if (c3po_->IsEntryEmpty(rh.entry_optional)) {
            // Entry not in GCD: request share from owning partition via IPC
            std::shared_ptr<rackobj::common::TigonIPCClient> client_to_remote_node =
                ipc_clients[my_partition][partition_num];

            // Serialize inode and key offset
            auto serialize = [block_id](uint64_t *data) {
                data[0] = static_cast<uint64_t>(block_id.GetInode());
                data[1] = static_cast<uint64_t>(block_id.GetOffset());
            };

            // Deserialize wmeta_idx and cn_idx
            size_t wmeta_idx, cn_idx;
            auto deserialize = [&wmeta_idx, &cn_idx](const uint64_t *data) {
                wmeta_idx = data[0];
                cn_idx = data[1];
            };

            // Send IPC request to remote partition
            (void)client_to_remote_node->Call<uint64_t>(rackobj::common::TigonIPCOp::RequestShareGet, serialize,
                                                        deserialize);

            if (wmeta_idx == 0 && cn_idx == 0) {
                // Concurrent admission detected, retry
                goto retry;
            }

            // Construct GCD entry from IPC response
            rh.entry_optional = common::GCDEntry{
                .wmeta_idx_ = wmeta_idx, .cn_array_ = {common::CNStatus_t{.cn_idx_ = cn_idx, .invalidate_ = false}}};
        }
        rh.cn_index = rh.entry_optional.value().cn_array_[0].cn_idx_;
    }

    expected<size_t, ReadErrno> result;
    if (cfg_.GetMoveToShare() && !rh.from_cxl) {
        result = ReadFromLocal(rh, ptr, count, offset_into_page, local_meta);
    } else {
        result = ReadFromCXL(rh, ptr, count, offset_into_page, local_meta);
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
    uint8_t partition_num = whose_key(block_id.GetOffset());
    uint8_t my_partition = static_cast<uint8_t>(local_meta->logical_node_id_);

    if (offset_into_page + count > common::BlockId::kBlockSize) {
        LOG(FATAL) << "SharedMemoryObjectHandle::Read attempting to read " << count << " bytes at an offset of "
                   << offset_into_page << " into the page (exceeds page limit)";
    }
    status_e status;
retry:
    init_read_handle(rh, block_id, my_partition);

    if (partition_num == my_partition) {
        auto lcd_entry_opt = lcds_[partition_num]->Get(block_id);

        if (!lcd_entry_opt.has_value()) {
            goto retry;
        }
        // CHECK(lcd_entry_opt.has_value()) << block_id << " not found on local partition " << my_partition;

        common::LCDEntry &lmeta = lcd_entry_opt->get();
        if (!lmeta.wmeta_idx_.has_value()) {
            // the entry can be read local
            update_read_handle(rh, block_id, lmeta);
            rh.cn_index = lmeta.cn_idx_.value();
            status = owned_hit;
        } else {
            // the locally owned key is shared
            rh.entry_optional = common::GCDEntry{.wmeta_idx_ = lmeta.wmeta_idx_.value(), .cn_array_ = {}};
            if (cfg_.GetMoveToShare()) {
                rh.cn_index = lmeta.wmeta_idx_.value();
            } else {
                rh.cn_index = lmeta.cn_idx_.value();
            }
            status = owned_miss;
        }
    } else {
        entry_optional = c3po_->Gcd()->GetAnchor(block_id);
        update_read_handle(rh, block_id, entry_optional);
        if (c3po_->IsEntryEmpty(rh.entry_optional)) {
            // 1. Get IPC client that is connected to the IPC server on the NUMA node with this key
            std::shared_ptr<rackobj::common::TigonIPCClient> client_to_remote_node =
                ipc_clients[my_partition][partition_num];

            // serialize inode and key offset
            auto serialize = [block_id](uint64_t *data) {
                data[0] = static_cast<uint64_t>(block_id.GetInode());
                data[1] = static_cast<uint64_t>(block_id.GetOffset());
            };
            // deserialize the wmeta_idx for the now-shared object
            size_t wmeta_idx, cn_idx;
            auto deserialize = [&wmeta_idx, &cn_idx](const uint64_t *data) {
                wmeta_idx = data[0];
                cn_idx = data[1];
            };

            // 2. Send a RequestShare IPC using Call() on IPC client. This will be synchrounous in our impl
            // ignore return value
            (void)client_to_remote_node->Call<uint64_t>(rackobj::common::TigonIPCOp::RequestShareGet, serialize,
                                                        deserialize);
            if (wmeta_idx == 0 && cn_idx == 0) {
                // handle someone already making the entry after we dispactched the IPC
                goto retry;
            }
            // we now know where the scr entry is
            rh.entry_optional = common::GCDEntry{
                .wmeta_idx_ = wmeta_idx, .cn_array_ = {common::CNStatus_t{.cn_idx_ = cn_idx, .invalidate_ = false}}};
            status = remote_miss;
        } else {
            status = remote_hit;
        }
        rh.cn_index = rh.entry_optional.value().cn_array_[0].cn_idx_;
    }

    size_t bytes_read = 0;
    (void)bytes_read;  // Suppress unused variable warning
    expected<size_t, ReadErrno> result;
    if (cfg_.GetMoveToShare() && !rh.from_cxl) {
        result = ReadFromLocal(rh, ptr, count, offset_into_page, local_meta);
    } else {
        result = ReadFromCXL(rh, ptr, count, offset_into_page, local_meta);
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
                LOG(FATAL) << "Wrong error code " << static_cast<int>(rh.rh_errno);
        }
    }

    // if (rh.from_cxl) {
    //     local_meta->pt_stat.remote_access_cnt++;
    // } else {
    //     local_meta->pt_stat.local_access_cnt++;
    // }
    switch (status) {
        case owned_hit:
            local_meta->pt_stat.owned_partition_hit_cnt++;
            local_meta->pt_stat.owned_partition_access_cnt++;
            break;
        case owned_miss:
            local_meta->pt_stat.owned_partition_miss_cnt++;
            local_meta->pt_stat.owned_partition_access_cnt++;
            break;
        case remote_hit:
            local_meta->pt_stat.remote_partition_hit_cnt++;
            local_meta->pt_stat.remote_partition_access_cnt++;
            break;
        case remote_miss:
            local_meta->pt_stat.remote_partition_miss_cnt++;
            local_meta->pt_stat.remote_partition_access_cnt++;
            break;
    }
    bytes_read = result.value();
    (void)admit_retry_cnt;

    return status;
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
    uint8_t partition_num = whose_key(block_id.GetOffset());
    uint8_t my_partition = static_cast<uint8_t>(local_meta->logical_node_id_);
    bool move_to_share = cfg_.GetMoveToShare();

    if (offset_into_page + count > common::BlockId::kBlockSize) {
        LOG(FATAL) << "SharedMemoryObjectHandle::Write attempting to write " << count << " bytes at an offset of "
                   << offset_into_page << " into the page (exceeds page limit)";
    }

retry:
    size_t bytes_write = 0;

    init_write_handle(wh, block_id, my_partition);

    if (partition_num == my_partition) {
        // Owned partition: check local cache directory
        auto lcd_entry_opt = lcds_[partition_num]->Get(block_id);

        if (!lcd_entry_opt.has_value()) {
            if (move_to_share) {
                std::shared_ptr<rackobj::common::LocalMemoryObject<Policy>> my_memory = cache_lcl_ptr_[my_partition];
                common::LocalCacheNode *new_node = my_memory->policy_.ReserveCacheNode();
                CHECK(new_node != nullptr)
                    << "(Ran out of memory?) reserving local index on partition " << my_partition << " with write";

                if (count != static_cast<size_t>(SLOT_SIZE) && local_meta->file_backed_) {
                    size_t slot_size = static_cast<size_t>(SLOT_SIZE);
                    ssize_t result = original_syscalls.pread(static_cast<int>(block_id.GetServerId()),
                                                             reinterpret_cast<char *>(new_node->page_ptr_), slot_size,
                                                             block_id.GetOffset());
                    if (result <= 0) {
                        LOG(ERROR) << "Error reading from disk: " << std::strerror(errno);
                    }
                }

                lcds_[partition_num]->Insert(block_id, reinterpret_cast<size_t>(new_node));
                goto retry;
            } else {
                size_t new_index;
                common::CacheNode *new_node;
                std::optional<size_t> new_cn_index_optional = cache_shrd_ptr_->scr_bitmap_.ReserveCacheNode();
                CHECK(new_cn_index_optional.has_value())
                    << "(Ran out of memory?) inserting to local index on partition " << my_partition << " with write";

                new_index = new_cn_index_optional.value();
                CHECK(new_index < cache_shrd_ptr_->Size())
                    << "Invalid cn_index: " << new_index << " cache size: " << cache_shrd_ptr_->Size();

                new_node = cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index);

                if (count != static_cast<size_t>(SLOT_SIZE) && local_meta->file_backed_) {
                    size_t slot_size = static_cast<size_t>(SLOT_SIZE);
                    ssize_t result = original_syscalls.pread(
                        static_cast<int>(block_id.GetServerId()),
                        reinterpret_cast<char *>(cache_shrd_ptr_->page_data_.GetPage(new_index)), slot_size,
                        block_id.GetOffset());
                    if (result <= 0) {
                        LOG(ERROR) << "Error reading from disk: " << std::strerror(errno);
                    }
                }

                size_t slot_size = static_cast<size_t>(SLOT_SIZE);
                new_node->SetLength(slot_size);
                new_node->Reinitialize(block_id);
                cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, 0);

                c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index)),
                                   sizeof(common::CacheNode));

                auto result = lcds_[my_partition]->Insert(block_id, new_index);
                CHECK(result) << "insert to local index failed on node " << my_partition;

                goto retry;
            }
        }

        CHECK(lcd_entry_opt.has_value()) << block_id << " not found on local";

        common::LCDEntry &lmeta = lcd_entry_opt->get();
        if (!lmeta.wmeta_idx_.has_value()) {
            update_write_handle(wh, block_id, lmeta);
        } else {
            wh.entry_optional = common::GCDEntry{
                .wmeta_idx_ = lmeta.wmeta_idx_.value(),
                .cn_array_ = {common::CNStatus_t{.cn_idx_ = lmeta.cn_idx_.value(), .invalidate_ = false}}};
        }
        wh.cn_index = lmeta.cn_idx_.value();
    } else {
        // Remote partition: check global cache directory
        entry_optional = c3po_->Gcd()->GetAnchor(block_id);
        update_write_handle(wh, block_id, entry_optional);

        if (c3po_->IsEntryEmpty(wh.entry_optional)) {
            // Entry not in GCD: request share from owning partition via IPC
            std::shared_ptr<rackobj::common::TigonIPCClient> client_to_remote_node =
                ipc_clients[my_partition][partition_num];

            // Serialize inode and key offset
            auto serialize = [block_id](uint64_t *data) {
                data[0] = static_cast<uint64_t>(block_id.GetInode());
                data[1] = static_cast<uint64_t>(block_id.GetOffset());
            };

            // Deserialize wmeta_idx and cn_idx
            size_t wmeta_idx, cn_idx;
            auto deserialize = [&wmeta_idx, &cn_idx](const uint64_t *data) {
                wmeta_idx = data[0];
                cn_idx = data[1];
            };

            // Send IPC request to remote partition
            (void)client_to_remote_node->Call<uint64_t>(rackobj::common::TigonIPCOp::RequestShareCreate, serialize,
                                                        deserialize);

            if (wmeta_idx == 0 && cn_idx == 0) {
                // Concurrent admission detected, retry
                goto retry;
            }

            // Construct GCD entry from IPC response
            wh.entry_optional = common::GCDEntry{
                .wmeta_idx_ = wmeta_idx, .cn_array_ = {common::CNStatus_t{.cn_idx_ = cn_idx, .invalidate_ = false}}};
        }
        wh.cn_index = wh.entry_optional.value().cn_array_[0].cn_idx_;
    }

    expected<size_t, WriteErrno> result = WriteToCXL(wh, ptr, count, offset_into_page, local_meta);

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
    uint8_t partition_num = whose_key(block_id.GetOffset());
    uint8_t my_partition = static_cast<uint8_t>(local_meta->logical_node_id_);
    bool move_to_share = cfg_.GetMoveToShare();

    if (offset_into_page + count > common::BlockId::kBlockSize) {
        LOG(FATAL) << "SharedMemoryObjectHandle::Write attempting to write " << count << " bytes at an offset of "
                   << offset_into_page << " into the page (exceeds page limit)";
    }

    status_e status;
retry:
    size_t bytes_write = 0;
    (void)bytes_write;  // Suppress unused variable warning
    init_write_handle(wh, block_id, static_cast<uint8_t>(local_meta->logical_node_id_));

    if (partition_num == my_partition) {
        auto lcd_entry_opt = lcds_[partition_num]->Get(block_id);

        /**
         * Handle insertion of keys
         */
        if (!lcd_entry_opt.has_value()) {
            if (move_to_share) {
                // find local slot and insert the entry to the local index if we do not already have it
                //  allocate local object slot
                std::shared_ptr<rackobj::common::LocalMemoryObject<Policy>> my_memory = cache_lcl_ptr_[my_partition];
                common::LocalCacheNode *new_node = my_memory->policy_.ReserveCacheNode();
                CHECK(new_node != nullptr)
                    << "(Ran out of memory?) reserving local index on partition " << my_partition << " with put";

                lcds_[partition_num]->Insert(block_id, reinterpret_cast<size_t>(new_node));
                goto retry;
            } else {
                // find CXL slot and insert the entry to local index
                size_t new_index;
                common::CacheNode *new_node;
                std::optional<size_t> new_cn_index_optional = cache_shrd_ptr_->scr_bitmap_.ReserveCacheNode();
                CHECK(new_cn_index_optional.has_value())
                    << "(Ran out of memory?) inserting to local index on partition " << my_partition << " with put";

                new_index = new_cn_index_optional.value();
                CHECK(new_index < cache_shrd_ptr_->Size())
                    << "Invalid cn_index: " << new_index << " cache size: " << cache_shrd_ptr_->Size();

                new_node = cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index);

                size_t slot_size = static_cast<size_t>(SLOT_SIZE);
                new_node->SetLength(slot_size);
                new_node->Reinitialize(block_id);
                cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, 0);

                c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index)),
                                   sizeof(common::CacheNode));

                auto result = lcds_[my_partition]->Insert(block_id, new_index);  // admit to local index
                CHECK(result) << "insert to local index failed on node " << my_partition;

                // if (!result) {
                //     // recycle slot
                //     cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, false);
                //     cache_shrd_ptr_->scr_bitmap_.RecycleCacheNode(new_index);
                //     return unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
                // }
                goto retry;
            }
        }

        CHECK(lcd_entry_opt.has_value()) << block_id << " not found on local";

        common::LCDEntry &lmeta = lcd_entry_opt->get();
        if (!lmeta.wmeta_idx_.has_value()) {
            // the entry can be written local
            status = owned_hit;
            update_write_handle(wh, block_id, lmeta);
        } else {
            // the locally owned key is shared
            wh.entry_optional = common::GCDEntry{
                .wmeta_idx_ = lmeta.wmeta_idx_.value(),
                .cn_array_ = {common::CNStatus_t{.cn_idx_ = lmeta.cn_idx_.value(), .invalidate_ = false}}};
            status = owned_miss;
        }
        wh.cn_index = lmeta.cn_idx_.value();
    } else {
        entry_optional = c3po_->Gcd()->GetAnchor(block_id);
        update_write_handle(wh, block_id, entry_optional);
        if (c3po_->IsEntryEmpty(wh.entry_optional)) {
            // 1. Get IPC client that is connected to the IPC server on the NUMA node with this key
            std::shared_ptr<rackobj::common::TigonIPCClient> client_to_remote_node =
                ipc_clients[my_partition][partition_num];

            // serialize inode and key offset
            auto serialize = [block_id](uint64_t *data) {
                data[0] = static_cast<uint64_t>(block_id.GetInode());
                data[1] = static_cast<uint64_t>(block_id.GetOffset());
            };
            // deserialize the wmeta_idx for the now-shared object
            size_t wmeta_idx, cn_idx;
            auto deserialize = [&wmeta_idx, &cn_idx](const uint64_t *data) {
                wmeta_idx = data[0];
                cn_idx = data[1];
            };

            // 2. Send a RequestShare IPC using Call() on IPC client. This will be synchrounous in our impl
            // ignore return value
            (void)client_to_remote_node->Call<uint64_t>(rackobj::common::TigonIPCOp::RequestShareCreate, serialize,
                                                        deserialize);

            // we now know where the scr entry is, setting cn_array is needed for checking entry consistency
            if (wmeta_idx == 0 && cn_idx == 0) {
                // handle someone already making the entry after we dispactched the IPC
                goto retry;
            }
            wh.entry_optional = common::GCDEntry{
                .wmeta_idx_ = wmeta_idx, .cn_array_ = {common::CNStatus_t{.cn_idx_ = cn_idx, .invalidate_ = false}}};
            status = remote_miss;
        } else {
            status = remote_hit;
        }
        wh.cn_index = wh.entry_optional.value().cn_array_[0].cn_idx_;
    }

    expected<size_t, WriteErrno> result = WriteToCXL(wh, ptr, count, offset_into_page, local_meta);

    if (!result.has_value()) {
        retry_cnt++;
        DLOG(WARNING) << "retry with error " << wh.wh_errno;
        goto retry;
    } else {
        bytes_write = *result;
        // if (wh.from_cxl) {
        //     local_meta->pt_stat.remote_access_cnt++;
        // } else {
        //     local_meta->pt_stat.local_access_cnt++;
        // }
    }

    switch (status) {
        case owned_hit:
            local_meta->pt_stat.owned_partition_hit_cnt++;
            local_meta->pt_stat.owned_partition_access_cnt++;
            break;
        case owned_miss:
            local_meta->pt_stat.owned_partition_miss_cnt++;
            local_meta->pt_stat.owned_partition_access_cnt++;
            break;
        case remote_hit:
            local_meta->pt_stat.remote_partition_hit_cnt++;
            local_meta->pt_stat.remote_partition_access_cnt++;
            break;
        case remote_miss:
            local_meta->pt_stat.remote_partition_miss_cnt++;
            local_meta->pt_stat.remote_partition_access_cnt++;
            break;
    }

    DLOG(INFO) << "num of retries: " << retry_cnt;
    return status;
}

template <typename Policy>
std::tuple<size_t, size_t> SharedMemoryObjectHandle<Policy>::PartitionToSharedGet(const common::BlockId &block_id,
                                                                                  const size_t target_partition) {
    auto lmeta_opt = lcds_[target_partition]->Get(block_id);
    /**
     * Handle insertion of key spurred by remote partition
     */
    if (!lmeta_opt.has_value()) {
        return std::tuple<size_t, size_t>(0, 0);
    }

    common::LCDEntry &lmeta = lmeta_opt->get();
    std::tuple<size_t, size_t> result;
    write_seqlock_begin(&lmeta.seqlock_);
    if (!lmeta.wmeta_idx_.has_value()) {
        // not shared, need to move

        // reserve smeta slot
        std::optional<size_t> new_wmeta_index_optional = std::nullopt;
        do {
            // find an available metadata slot
            new_wmeta_index_optional =
                c3po_->Scr_meta()->CheckReserveWmeta(block_id, target_partition, true, new_wmeta_index_optional);

            if (new_wmeta_index_optional.has_value()) {
                if (cfg_.GetMoveToShare()) {
                    // reserve a remote slot
                    size_t new_index = new_wmeta_index_optional.value();
                    common::CacheNode *new_node;
                    // std::optional<size_t> new_cn_index_optional = cache_shrd_ptr_->scr_bitmap_.ReserveCacheNode();
                    // new_index = new_cn_index_optional.value();

                    new_node = cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index);

                    CHECK(!lmeta.wmeta_idx_.has_value()) << "lmeta has a wmeta_idx when it should not";
                    lmeta.wmeta_idx_ = new_index;

                    // Copy entire local slot to remote slot
                    memcpy(static_cast<void *>(cache_shrd_ptr_->GetPage(new_index)),
                           reinterpret_cast<void *>(lmeta.cn_idx_.value()), SLOT_SIZE);
                    c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->GetPage(new_index)), SLOT_SIZE);

                    // setup remote slot
                    size_t slot_size = static_cast<size_t>(SLOT_SIZE);
                    new_node->SetLength(slot_size);
                    new_node->Reinitialize(block_id);
                    cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, true);

                    c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index)),
                                       sizeof(common::CacheNode));
                }
                break;
            }

            // no metadata slot available yet, try to evict another one
            //  smeta lock is held in this step
            new_wmeta_index_optional = c3po_->Scr_meta()->SampleVictim(MAX_WMETA_SAMPLING_SIZE, target_partition);
            if (!new_wmeta_index_optional.has_value()) continue;

            common::WriteMetadata *vic_smeta = c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value());
            common::BlockId vic_block_id = vic_smeta->GetBlockID();

            SharedToPartition(vic_block_id, vic_smeta, target_partition);

            cpu_relax();
        } while (true);

        std::optional<size_t> cn_idx;
        if (cfg_.GetMoveToShare()) {
            cn_idx = lmeta.wmeta_idx_;
        } else {
            cn_idx = lmeta.cn_idx_;
        }
        CHECK(cn_idx.has_value()) << "lmeta cn_index does not have value for " << block_id;
        common::NrGcdError error =
            c3po_->Gcd()->CheckAndInsert(block_id, cn_idx.value(), shared_cache_node_, new_wmeta_index_optional);

        switch (error) {
            case common::NrGcdError::GCD_NO_ERROR: {
                // set lmeta to point to wmeta
                lmeta.wmeta_idx_ = new_wmeta_index_optional.value();
                c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value())->WSeqEnd();
                result = std::make_tuple(lmeta.wmeta_idx_.value(), cn_idx.value());

                // a key has been shared
                partition_to_shared_[target_partition].value++;
                break;
            }
            case common::NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED:
                // c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), true);
            case common::NrGcdError::GCD_SLOT_UPDATE_FAILED:
                c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), target_partition, true);
                result = std::tuple<size_t, size_t>(0, 0);
                LOG(WARNING) << "Bad spot in partition to shared on partition " << target_partition << " " << block_id;
                break;
            default:
                break;
        }
    } else {
        // TODO: increase the refcount following Tigon
        if (cfg_.GetMoveToShare()) {
            result = std::make_tuple(lmeta.wmeta_idx_.value(), lmeta.wmeta_idx_.value());
        } else {
            result = std::tuple<size_t, size_t>(lmeta.wmeta_idx_.value(), lmeta.cn_idx_.value());
        }
    }

    write_seqlock_end(&lmeta.seqlock_);
    return result;
}

template <typename Policy>
std::tuple<size_t, size_t> SharedMemoryObjectHandle<Policy>::PartitionToShared(const common::BlockId &block_id,
                                                                               const size_t target_partition) {
    auto lmeta_opt = lcds_[target_partition]->Get(block_id);
    bool move_to_share = cfg_.GetMoveToShare();
    /**
     * Handle insertion of key spurred by remote partition
     */
    if (!lmeta_opt.has_value()) {
        if (move_to_share) {
            LOG(FATAL) << "Insertion of key by remote partition in move_to_share mode is not implemented";
        } else {
            // Insert the key to CXL and local index and retry get
            // find CXL slot and insert the entry to local index
            size_t new_index;
            common::CacheNode *new_node;
            std::optional<size_t> new_cn_index_optional = cache_shrd_ptr_->scr_bitmap_.ReserveCacheNode();
            CHECK(new_cn_index_optional.has_value())
                << "(Ran out of memory?) inserting to local index on partition " << target_partition << " with put";

            new_index = new_cn_index_optional.value();
            CHECK(new_index < cache_shrd_ptr_->Size())
                << "Invalid cn_index: " << new_index << " cache size: " << cache_shrd_ptr_->Size();

            new_node = cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index);

            size_t slot_size = static_cast<size_t>(SLOT_SIZE);
            new_node->SetLength(slot_size);
            new_node->Reinitialize(block_id);
            cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, 0);

            c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index)),
                               sizeof(common::CacheNode));

            auto result = lcds_[target_partition]->Insert(block_id, new_index);  // admit to local index
            CHECK(result) << "insert to local index failed on node " << target_partition;
        }
        lmeta_opt = lcds_[target_partition]->Get(block_id);
    }

    common::LCDEntry &lmeta = lmeta_opt->get();
    std::tuple<size_t, size_t> result;
    write_seqlock_begin(&lmeta.seqlock_);
    if (!lmeta.wmeta_idx_.has_value()) {
        // not shared, need to move

        // reserve smeta slot
        std::optional<size_t> new_wmeta_index_optional = std::nullopt;
        do {
            // find an available metadata slot
            new_wmeta_index_optional =
                c3po_->Scr_meta()->CheckReserveWmeta(block_id, target_partition, true, new_wmeta_index_optional);

            if (new_wmeta_index_optional.has_value()) {
                if (cfg_.GetMoveToShare()) {
                    // reserve a remote slot
                    size_t new_index = new_wmeta_index_optional.value();
                    common::CacheNode *new_node;
                    // std::optional<size_t> new_cn_index_optional = cache_shrd_ptr_->scr_bitmap_.ReserveCacheNode();
                    // new_index = new_cn_index_optional.value();

                    new_node = cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index);

                    CHECK(!lmeta.wmeta_idx_.has_value()) << "lmeta has a wmeta_idx when it should not";
                    lmeta.wmeta_idx_ = new_index;

                    // Copy entire local slot to remote slot
                    memcpy(static_cast<void *>(cache_shrd_ptr_->GetPage(new_index)),
                           reinterpret_cast<void *>(lmeta.cn_idx_.value()), SLOT_SIZE);
                    c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->GetPage(new_index)), SLOT_SIZE);

                    // setup remote slot
                    size_t slot_size = static_cast<size_t>(SLOT_SIZE);
                    new_node->SetLength(slot_size);
                    new_node->Reinitialize(block_id);
                    cache_shrd_ptr_->scr_bitmap_.SetDirty(new_index, true);

                    c3po_->write_flush(reinterpret_cast<char *>(cache_shrd_ptr_->cache_slot_.GetCacheNode(new_index)),
                                       sizeof(common::CacheNode));
                }
                break;
            }

            // no metadata slot available yet, try to evict another one
            //  smeta lock is held in this step
            new_wmeta_index_optional = c3po_->Scr_meta()->SampleVictim(MAX_WMETA_SAMPLING_SIZE, target_partition);
            if (!new_wmeta_index_optional.has_value()) continue;

            common::WriteMetadata *vic_smeta = c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value());
            common::BlockId vic_block_id = vic_smeta->GetBlockID();

            SharedToPartition(vic_block_id, vic_smeta, target_partition);

            cpu_relax();
        } while (true);

        std::optional<size_t> cn_idx;
        if (cfg_.GetMoveToShare()) {
            cn_idx = lmeta.wmeta_idx_;
        } else {
            cn_idx = lmeta.cn_idx_;
        }
        CHECK(cn_idx.has_value()) << "lmeta cn_index does not have value for " << block_id;
        common::NrGcdError error =
            c3po_->Gcd()->CheckAndInsert(block_id, cn_idx.value(), shared_cache_node_, new_wmeta_index_optional);

        switch (error) {
            case common::NrGcdError::GCD_NO_ERROR: {
                // set lmeta to point to wmeta
                lmeta.wmeta_idx_ = new_wmeta_index_optional.value();
                c3po_->Scr_meta()->GetWmeta(new_wmeta_index_optional.value())->WSeqEnd();
                result = std::make_tuple(lmeta.wmeta_idx_.value(), cn_idx.value());

                // a key has been shared
                partition_to_shared_[target_partition].value++;
                break;
            }
            case common::NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED:
                // c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), true);
            case common::NrGcdError::GCD_SLOT_UPDATE_FAILED:
                c3po_->Scr_meta()->RecycleWmeta(new_wmeta_index_optional.value(), target_partition, true);
                result = std::tuple<size_t, size_t>(0, 0);
                LOG(WARNING) << "Bad spot in partition to shared on partition " << target_partition << " " << block_id;
                break;
            default:
                break;
        }
    } else {
        // TODO: increase the refcount following Tigon
        if (cfg_.GetMoveToShare()) {
            result = std::make_tuple(lmeta.wmeta_idx_.value(), lmeta.wmeta_idx_.value());
        } else {
            result = std::tuple<size_t, size_t>(lmeta.wmeta_idx_.value(), lmeta.cn_idx_.value());
        }
    }

    write_seqlock_end(&lmeta.seqlock_);
    return result;
}

/**
 * @note Assumption is that the write lock for smeta is already held when calling and will be released when
 * returning
 */
template <typename Policy>
std::optional<size_t> SharedMemoryObjectHandle<Policy>::SharedToPartition(const common::BlockId &block_id,
                                                                          common::WriteMetadata *smeta,
                                                                          const size_t target_partition) {
    auto lmeta_opt = lcds_[target_partition]->Get(block_id);
    std::optional<size_t> wmeta_idx_optional = std::nullopt;

    if (!lmeta_opt.has_value()) {
        LOG(WARNING) << "IndexToShared a non-existing entry " << block_id;
        return wmeta_idx_optional;
    }

    common::LCDEntry &lmeta = lmeta_opt->get();
    write_seqlock_begin(&lmeta.seqlock_);

    if (lmeta.wmeta_idx_.has_value()) {
        // object has shared index
        CHECK(lmeta.cn_idx_.has_value()) << "lmeta cn_index does not have value for " << block_id;
        auto cn_idx = lmeta.cn_idx_.value();

        common::BlockId curr_block_id = smeta->GetBlockID();
        CHECK(curr_block_id == block_id) << curr_block_id << " mismatch " << block_id;

        // This removes the GCDEntry from the shared index
        wmeta_idx_optional = c3po_->Gcd()->Delete(block_id);

        if (wmeta_idx_optional.has_value()) {
            if (cfg_.GetMoveToShare()) {
                // copy data back to local slot
                uint8_t *remote_data = cache_shrd_ptr_->GetPage(lmeta.wmeta_idx_.value());
                memcpy(reinterpret_cast<void *>(cn_idx), remote_data, SLOT_SIZE);
            }
            c3po_->Scr_meta()->RecycleWmeta(wmeta_idx_optional.value(), target_partition);
            lmeta.wmeta_idx_ = std::nullopt;
            // a key has been unshared
            shared_to_partition_[target_partition].value++;
        } else {
            LOG(WARNING) << "not able to delete from shared idx " << block_id;
            smeta->WUnlockOnly();
        }

    } else {
        LOG(WARNING) << " local idx of " << block_id << " is not shared";
        smeta->WUnlockOnly();
    }
    write_seqlock_end(&lmeta.seqlock_);

    return wmeta_idx_optional;
}

template class SharedMemoryObjectHandle<CurrPolicy>;

}  // namespace rackobj::lib

// #endif  // SHM_PAGE_CACHE_HANDLE_HPP
