#pragma once

#include <optional>
#include <random>

#include "blockid.h"
#include "cc_primitive.h"
#include "common/constants.h"
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
    explicit WriteMetadata() : /*seqcount_(0),*/ seqlock_(FREE_BIT) {}

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
    using LocalSeqcountT = int64_t;
    using VectorElement = std::atomic<std::optional<LocalSeqcountT>>;
    using VecEntryAllocator = RebindLocalMemoryByteAllocatorT<VectorElement>;
    using LockVecEntryAllocator = RebindLocalMemoryByteAllocatorT<SpinLockWrapper>;
    virtual ~BaseMetadata() = default;
    BaseMetadata(const size_t num_slots, const size_t wmeta_slot_len, const int shared_cache_nid,
                 const std::shared_ptr<AllocatableLocalMemoryRegion> &mem_region)
        : num_slots_(num_slots), wmeta_slot_len_(wmeta_slot_len), shared_cache_nid_(shared_cache_nid) {
        CHECK(wmeta_slot_len) << "write metadata slot length should not be zero!";
        wmeta_allocator_ = std::make_unique<LocalMemoryAllocator<MetaT>>(mem_region);
        wmeta_start_addr_ = wmeta_allocator_->allocate(wmeta_slot_len);
        CHECK(wmeta_start_addr_) << "write metadata allocation failed!";

        for (size_t i = 0; i < wmeta_slot_len; i++) std::construct_at(&wmeta_start_addr_[i]);
    };

    MetaT *GetWmeta(size_t wm_index) {
        // sanity check
        if (wm_index < 0) return nullptr;
        MetaT *wmetap = &wmeta_start_addr_[wm_index];
        return wmetap;
    }

    virtual size_t GetWmetaCount() { return allocated_slot_num_.load(std::memory_order_relaxed); }

    std::optional<LocalSeqcountT> GetLocalSeqcount(const size_t cn_index, const int nid) {
        return local_seqcount_vec_[nid]->at(cn_index).load(std::memory_order_acquire);
    }

    void UpdateLocalSeqcount(const size_t cn_index, const int nid, const std::optional<LocalSeqcountT> &seqcount) {
        local_seqcount_vec_[nid]->at(cn_index).store(seqcount, std::memory_order_release);
    }

    size_t GetWmetaSlotLen() const { return wmeta_slot_len_; }

    void CreateLocalSeqMap(int nid, const std::shared_ptr<common::AllocatableLocalMemoryRegion> &local_region);

protected:
    alignas(64) MetaT *wmeta_start_addr_;

    std::unique_ptr<LocalMemoryAllocator<MetaT>> wmeta_allocator_;

    const size_t num_slots_;
    const size_t wmeta_slot_len_;
    std::atomic<size_t> allocated_slot_num_;

    int shared_cache_nid_;

    std::unique_ptr<std::vector<VectorElement, VecEntryAllocator>> local_seqcount_vec_[LOGICAL_NODE_NUM];
    std::unique_ptr<std::vector<SpinLockWrapper, LockVecEntryAllocator>> local_map_lock_[LOGICAL_NODE_NUM];
};

/**
 * @args
 * placeholder: not used for this class, for interface compatibility with PartitionMetadata
 */
class SharedMetadata : public BaseMetadata<WriteMetadata> {
public:
    explicit SharedMetadata(int shared_cache_nid, size_t num_slots, size_t wmeta_slot_len,
                            const std::shared_ptr<AllocatableLocalMemoryRegion> &sc_shm_region,
                            const size_t num_partition) noexcept
        : BaseMetadata(num_slots, wmeta_slot_len, shared_cache_nid, sc_shm_region) {
        (void)num_partition;  // not needed for shared metadata
        LOG(INFO) << "Initialized SharedMetadata with wmeta_slot_len: " << wmeta_slot_len;
    }

    ~SharedMetadata() override {
        for (size_t i = 0; i < wmeta_slot_len_; i++) std::destroy_at(&wmeta_start_addr_[i]);
    }

    /**
     * randomly sample {sample_size} sequence locks, return the one with smallest sequence number that is not free
     * return nullopt if all samples are free
     * {with_lock}: whether return the wmeta_idx with the lock specifically acquired by this function
     */
    std::optional<size_t> SampleVictim(const size_t sample_size, const size_t placeholder, bool with_lock = true);

    std::optional<size_t> ReserveWmeta(const BlockId &block_id, const size_t placeholder, bool with_lock = true,
                                       std::optional<size_t> hint = std::nullopt);

    std::optional<size_t> ReserveWmetaEarlyTerm(const BlockId &block_id, const size_t placeholder, size_t count,
                                                bool with_lock = true, std::optional<size_t> hint = std::nullopt);

    std::optional<size_t> CheckReserveWmeta(const BlockId &block_id, const size_t placeholder, bool with_lock = true,
                                            std::optional<size_t> hint = std::nullopt);

    bool RecycleWmeta(size_t wm_index, const size_t placeholder, bool with_lock = true);

    size_t WmetaOverThreshold() {
        int64_t count = static_cast<int64_t>(allocated_slot_num_.load(std::memory_order_relaxed));

        LOG_IF(WARNING, wmeta_slot_len_ < static_cast<size_t>(count))
            << "allocated over wmeta_slot_len_ (" << wmeta_slot_len_ << ":" << count << ")";
        return static_cast<size_t>(count - wmeta_count_threshold_ > 0 ? count - wmeta_count_threshold_ : 0);
    }

    void SetWmetaThreshold(size_t threshold) {
        DLOG(INFO) << "WRITE THRESHOLD set to " << threshold;
        wmeta_count_threshold_ = static_cast<int64_t>(threshold);
    }

    void LockLocalMap(const int nid, const size_t idx) { (*local_map_lock_[nid])[idx].lock(); }

    void UnlockLocalMap(const int nid, const size_t idx) { (*local_map_lock_[nid])[idx].unlock(); }

private:
    [[maybe_unused]] int64_t wmeta_count_threshold_;

    static thread_local std::minstd_rand rng;
};

/* for tigon-like partitioned index */
class PartitionMetadata : public BaseMetadata<WriteMetadata> {
public:
    using LocalSeqcountT = int64_t;
    using VectorElement = std::atomic<std::optional<LocalSeqcountT>>;
    using VecEntryAllocator = RebindLocalMemoryByteAllocatorT<VectorElement>;
    using LockVecEntryAllocator = RebindLocalMemoryByteAllocatorT<SpinLockWrapper>;

    virtual ~PartitionMetadata() = default;

    explicit PartitionMetadata(int shared_cache_nid, size_t num_slots, size_t wmeta_slot_len,
                               const std::shared_ptr<AllocatableLocalMemoryRegion> &mem_region,
                               const size_t num_partition) noexcept
        : BaseMetadata(num_slots, wmeta_slot_len, shared_cache_nid, mem_region), num_partition_(num_partition) {
        per_partition_slot_len_ = wmeta_slot_len / num_partition;
        if (!wmeta_slot_len % num_partition == 0) {
            LOG(WARNING) << "wmeta_slot_len: " << wmeta_slot_len << " not divisible by num_partition: " << num_partition
                         << ", per_partition_slot_len_: " << per_partition_slot_len_;
        }

        allocated_slot_num_arr_ = std::vector<std::atomic<size_t>>(num_partition);

        LOG(INFO) << "Initialized PartitionMetadata with wmeta_slot_len: " << wmeta_slot_len
                  << " num_partition: " << num_partition_;
    }

    /**
     * @args: target_partition
     *     partition wmeta_slots into partitions, so starts from 0
     *     e.g. three numa node 1, 2, 3 represents three partitions
     *     one possible target_partition mapping:
     *          numa: 1 -> partition: 0
     *          numa: 2 -> partition: 1
     *          numa: 3 -> partition: 2
     */
    std::optional<size_t> CheckReserveWmeta(const BlockId &block_id, const size_t target_partition,
                                            bool with_lock = true, std::optional<size_t> hint = std::nullopt);

    size_t GetWmetaCountPartition(const size_t target_partition) {
        return allocated_slot_num_arr_[target_partition].load(std::memory_order_relaxed);
    }

    size_t GetWmetaCount() override {
        size_t total = 0;
        for (size_t i = 0; i < num_partition_; ++i) {
            total += allocated_slot_num_arr_[i].load(std::memory_order_relaxed);
        }
        return total;
    }

    bool RecycleWmeta(size_t wm_index, const size_t target_partition, bool with_lock = true);

    std::optional<size_t> SampleVictim(const size_t sample_size, const size_t target_partition, bool with_lock = true);

    /* manager functions not implemented yet */
    size_t WmetaOverThreshold() {
        LOG(WARNING) << "WmetaOverThreshold() not implemented";
        return 0;
    }

    void SetWmetaThreshold(size_t threshold) {
        (void)threshold;
        LOG(WARNING) << "WmetaOverThreshold() not implemented";
    }

private:
    size_t num_partition_;
    size_t per_partition_slot_len_;

    std::vector<std::atomic<size_t>> allocated_slot_num_arr_;

    [[maybe_unused]] int64_t wmeta_count_threshold_;

    static thread_local std::minstd_rand rng;
};

}  // namespace rackobj::common
