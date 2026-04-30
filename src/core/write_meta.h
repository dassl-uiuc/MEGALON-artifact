#pragma once

#include <optional>
#include <random>

#include "blockid.h"
#include "common/constants.h"
#include "seqcount.h"
#include "shared_memory/allocator.h"

namespace rackobj::common {

struct SpinLockWrapper {
    pthread_spinlock_t raw;

    SpinLockWrapper() { pthread_spin_init(&raw, PTHREAD_PROCESS_PRIVATE); }
    ~SpinLockWrapper() { pthread_spin_destroy(&raw); }

    void lock() { pthread_spin_lock(&raw); }
    void unlock() { pthread_spin_unlock(&raw); }
};

// RW shared page

// SCR
class alignas(64) WriteMetadata {
public:
    explicit WriteMetadata(bool alloc = false) : seqlock_(alloc ? 0 : FREE_BIT) {}

    [[nodiscard]] bool WSeqBegin();

    uint32_t WSeqEnd();

    [[nodiscard]] bool WLockOnly();

    void WUnlockOnly();

    [[nodiscard]] uint32_t RSeqBegin();

    [[nodiscard]] bool RSeqRetry(uint32_t sequence);

    [[nodiscard]] uint32_t seqcount();

    [[nodiscard]] uint32_t seqlock(bool relaxed = true);

    [[nodiscard]] bool TryAllocateLock(const BlockId &block_id, bool with_lock = true);

    [[nodiscard]] bool FreeLock(bool with_lock = true);

    [[nodiscard]] bool FreeLockReset(bool with_lock = true);

    uint8_t GetActiveNode() { return get_active_node(&seqlock_); }
    void SetActiveNode(uint8_t node) { set_active_node(&seqlock_, node); }

    void SetBlockID(const BlockId &block_id) { block_id_ = block_id; }

    void Reinitialize(const BlockId &block_id) {
        block_id_ = block_id;
        // seqcount_init(&seqcount_);
        seqlock_init(&seqlock_);
    }

    BlockId GetBlockID() { return block_id_; }

private:
    // seqcount_t seqcount_;
    seqlock_t seqlock_;
    BlockId block_id_;

#ifndef C3_RWLOCK
    static_assert(sizeof(seqlock_t) + sizeof(BlockId) <= CACHE_LINE_SIZE,
                  "WriteMetadata members exceed cache line size.");
#endif
};

#ifndef C3_RWLOCK
static_assert(sizeof(WriteMetadata) == CACHE_LINE_SIZE, "WriteMetadata must be exactly one cache line (64 bytes)");
#else
static_assert(sizeof(WriteMetadata) % CACHE_LINE_SIZE == 0, "WriteMetadata must be cache line (64 bytes) aligned");
#endif

template <typename MetaT>
class BaseMetadata {
public:
    BaseMetadata(const size_t wmeta_slot_len) : wmeta_slot_len_(wmeta_slot_len) {
        wmeta_start_addr_ = nullptr;
        allocated_slot_num_ = 0;
        reclaim_count_ = 0;
        allocate_count_ = 0;
    };

    MetaT *GetWmeta(size_t wm_index) {
        // sanity check
        if (wm_index < 0) return nullptr;
        MetaT *wmetap = &wmeta_start_addr_[wm_index];
        return wmetap;
    }

    alignas(64) MetaT *wmeta_start_addr_;

    std::unique_ptr<LocalMemoryAllocator<MetaT>> wmeta_allocator_;

    const size_t wmeta_slot_len_;
    std::atomic<size_t> allocated_slot_num_;
    std::atomic<uint64_t> reclaim_count_;
    std::atomic<uint64_t> allocate_count_;
};

class SharedMetadata {
public:
    using LocalSeqcountT = int64_t;
    using VectorElement = std::pair<std::atomic<std::optional<LocalSeqcountT>>, std::atomic<uint8_t>>;
    using VecEntryAllocator = RebindLocalMemoryByteAllocatorT<VectorElement>;
    using LockVecEntryAllocator = RebindLocalMemoryByteAllocatorT<SpinLockWrapper>;
    explicit SharedMetadata(int shared_cache_nid, size_t num_slots, size_t wmeta_slot_len,
                            const std::shared_ptr<AllocatableLocalMemoryRegion> &sc_shm_region) noexcept
        : b_meta_(wmeta_slot_len), shared_cache_nid_(shared_cache_nid), num_slots_(num_slots) {
        CHECK(wmeta_slot_len) << "write metadata slot length should not be zero!";
        b_meta_.wmeta_allocator_ = std::make_unique<LocalMemoryAllocator<WriteMetadata>>(sc_shm_region);
        b_meta_.wmeta_start_addr_ = b_meta_.wmeta_allocator_->allocate(wmeta_slot_len);
        CHECK(b_meta_.wmeta_start_addr_) << "write metadata allocation failed!";

        for (size_t i = 0; i < wmeta_slot_len; i++) std::construct_at(&b_meta_.wmeta_start_addr_[i]);
    }

    ~SharedMetadata() {
        for (size_t i = 0; i < b_meta_.wmeta_slot_len_; i++) std::destroy_at(&b_meta_.wmeta_start_addr_[i]);
    }

    /**
     * randomly sample {sample_size} sequence locks, return the one with smallest sequence number that is not free
     * return nullopt if all samples are free
     * {with_lock}: whether return the wmeta_idx with the lock specifically acquired by this function
     */
    std::optional<size_t> SampleVictim(const size_t sample_size, bool with_lock = true);

    std::optional<size_t> ReserveWmeta(const BlockId &block_id, bool with_lock = true,
                                       std::optional<size_t> hint = std::nullopt);

    std::optional<size_t> ReserveWmetaEarlyTerm(const BlockId &block_id, size_t count, bool with_lock = true,
                                                std::optional<size_t> hint = std::nullopt);

    std::optional<size_t> CheckReserveWmeta(const BlockId &block_id, bool with_lock = true,
                                            std::optional<size_t> hint = std::nullopt);

    bool RecycleWmeta(size_t wm_index, bool with_lock = true);

    size_t GetWmetaCount() { return b_meta_.allocated_slot_num_.load(std::memory_order_relaxed); }

    size_t WmetaOverThreshold() {
        int64_t count = static_cast<int64_t>(b_meta_.allocated_slot_num_.load(std::memory_order_relaxed));

        LOG_IF(WARNING, b_meta_.wmeta_slot_len_ < static_cast<size_t>(count))
            << "allocated over wmeta_slot_len_ (" << b_meta_.wmeta_slot_len_ << ":" << count << ")";
        return static_cast<size_t>(count - wmeta_count_threshold_ > 0 ? count - wmeta_count_threshold_ : 0);
    }

    void SetWmetaThreshold(size_t threshold) {
        DLOG(INFO) << "WRITE THRESHOLD set to " << threshold;
        wmeta_count_threshold_ = static_cast<int64_t>(threshold);
    }

    std::optional<LocalSeqcountT> GetLocalSeqcount(const size_t cn_index, const int nid) {
        return local_seqcount_vec_[nid]->at(cn_index).first.load(std::memory_order_acquire);
    }

    void UpdateLocalSeqcount(const size_t cn_index, const int nid, const std::optional<LocalSeqcountT> &seqcount) {
        local_seqcount_vec_[nid]->at(cn_index).first.store(seqcount, std::memory_order_release);
    }

    uint8_t GetCurrentNodeCount(const size_t cn_index, const int nid) {
        return local_seqcount_vec_[nid]->at(cn_index).second.load(std::memory_order_relaxed);
    }

    void SetCurrentNodeCount(const size_t cn_index, const int nid, const uint8_t node) {
        local_seqcount_vec_[nid]->at(cn_index).second.store(node, std::memory_order_relaxed);
    }

    uint8_t IncrementCurrentNodeCount(const size_t cn_index, const int nid) {
        return local_seqcount_vec_[nid]->at(cn_index).second.fetch_add(1, std::memory_order_relaxed);
    }

    void ResetCurrentNodeCount(const size_t cn_index, const int nid) {
        local_seqcount_vec_[nid]->at(cn_index).second.store(0, std::memory_order_relaxed);
    }

    WriteMetadata *GetWmeta(size_t wm_index) { return b_meta_.GetWmeta(wm_index); }

    void CreateLocalSeqMap(int nid, const std::shared_ptr<common::AllocatableLocalMemoryRegion> &local_region);

    void LockLocalMap(const int nid, const size_t idx) { (*local_map_lock_[nid])[idx].lock(); }

    void UnlockLocalMap(const int nid, const size_t idx) { (*local_map_lock_[nid])[idx].unlock(); }

    size_t GetWmetaSlotLen() const { return b_meta_.wmeta_slot_len_; }

    size_t GetRecycleCount() const { return b_meta_.reclaim_count_; }
    size_t GetAllocateCount() const { return b_meta_.allocate_count_; }

    void ResetReclaimCount() {
        b_meta_.reclaim_count_ = 0;
        b_meta_.allocate_count_ = 0;
    }

private:
    // per-NUMA node seqcount vector <cn_index, local_seqcount>
    BaseMetadata<WriteMetadata> b_meta_;

    std::unique_ptr<std::vector<VectorElement, VecEntryAllocator>> local_seqcount_vec_[LOGICAL_NODE_NUM];
    std::unique_ptr<std::vector<SpinLockWrapper, LockVecEntryAllocator>> local_map_lock_[LOGICAL_NODE_NUM];

    int shared_cache_nid_;

    const size_t num_slots_;

    int64_t wmeta_count_threshold_;

    static thread_local std::minstd_rand rng;
};

/* not used */
class LocalMetadata {
public:
    explicit LocalMetadata(size_t wmeta_slot_len,
                           const std::shared_ptr<AllocatableLocalMemoryRegion> &local_region) noexcept
        : b_meta_(wmeta_slot_len) {
        CHECK(wmeta_slot_len) << "write metadata slot length should not be zero!";
        b_meta_.wmeta_allocator_ = std::make_unique<LocalMemoryAllocator<WriteMetadata>>(local_region);
        b_meta_.wmeta_start_addr_ = b_meta_.wmeta_allocator_->allocate(wmeta_slot_len);
        CHECK(b_meta_.wmeta_start_addr_) << "write metadata allocation failed!";

        for (size_t i = 0; i < wmeta_slot_len; i++) std::construct_at(&b_meta_.wmeta_start_addr_[i], true);
    }

    ~LocalMetadata() {
        for (size_t i = 0; i < b_meta_.wmeta_slot_len_; i++) std::destroy_at(&b_meta_.wmeta_start_addr_[i]);
    }

    WriteMetadata *GetWmeta(size_t wm_index) { return b_meta_.GetWmeta(wm_index); }

private:
    BaseMetadata<WriteMetadata> b_meta_;
};

}  // namespace rackobj::common
