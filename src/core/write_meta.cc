#include "write_meta.h"

namespace rackobj::common {

bool WriteMetadata::WSeqBegin() { return write_seqlock_begin(&seqlock_); }

uint32_t WriteMetadata::WSeqEnd() { return write_seqlock_end(&seqlock_); }

bool WriteMetadata::WLockOnly() { return write_seqlock_only(&seqlock_); }

void WriteMetadata::WUnlockOnly() { write_sequnlock_only(&seqlock_); }

uint32_t WriteMetadata::RSeqBegin() { return read_seqlock_begin(&seqlock_); }

bool WriteMetadata::RSeqRetry(uint32_t sequence) { return read_seqlock_retry(&seqlock_, sequence); }

uint32_t WriteMetadata::seqcount() { return seqlock_count(&seqlock_); }

uint32_t WriteMetadata::seqlock(bool relaxed) {
    if (relaxed) return seqlock_full_relaxed(&seqlock_);
    return seqlock_full(&seqlock_);
}

bool WriteMetadata::TryAllocateLock(const BlockId &block_id, bool with_lock) {
    if (try_allocate_seqlock(&seqlock_, with_lock)) {
        block_id_ = block_id;
        return true;
    }
    return false;
}

bool WriteMetadata::FreeLock(bool with_lock) {
    if (with_lock) return free_seqlock_with_lock(&seqlock_);
    free_seqlock(&seqlock_);
    return true;
}

template <typename MetaT>
void BaseMetadata<MetaT>::CreateLocalSeqMap(int nid,
                                            const std::shared_ptr<common::AllocatableLocalMemoryRegion> &local_region) {
    LocalMemoryAllocator<std::vector<VectorElement, VecEntryAllocator>> vector_alloc(local_region);
    auto vector = vector_alloc.allocate();
    std::construct_at(vector, wmeta_slot_len_, VecEntryAllocator(local_region));
    local_seqcount_vec_[nid] = std::unique_ptr<std::vector<VectorElement, VecEntryAllocator>>(vector);
    std::for_each(local_seqcount_vec_[nid]->begin(), local_seqcount_vec_[nid]->end(),
                  [](VectorElement &entry) { entry = std::nullopt; });

    LocalMemoryAllocator<std::vector<SpinLockWrapper, LockVecEntryAllocator>> lock_alloc(local_region);
    auto lock_vec = lock_alloc.allocate();
    std::construct_at(lock_vec, wmeta_slot_len_, LockVecEntryAllocator(local_region));
    local_map_lock_[nid] = std::unique_ptr<std::vector<SpinLockWrapper, LockVecEntryAllocator>>(lock_vec);
}

// Explicit instantiation to ensure the definition is emitted for the linker
template void BaseMetadata<WriteMetadata>::CreateLocalSeqMap(
    int nid, const std::shared_ptr<common::AllocatableLocalMemoryRegion> &local_region);

std::optional<size_t> SharedMetadata::SampleVictim(const size_t sample_size, const size_t placeholder, bool with_lock) {
    (void)placeholder;
    struct Candidate {
        size_t index;
        uint32_t seq;
    };

    assert(sample_size <= MAX_WMETA_SAMPLING_SIZE && "sample_size exceeds hard limit");

    Candidate candidates[MAX_WMETA_SAMPLING_SIZE];
    size_t count = 0;

    for (size_t i = 0; i < sample_size; ++i) {
        size_t candidate_idx = rng() % wmeta_slot_len_;
        WriteMetadata *wmeta = &wmeta_start_addr_[candidate_idx];

        uint32_t raw_seq = wmeta->seqlock();
        if (raw_seq & FREE_BIT) continue;

        candidates[count++] = Candidate{candidate_idx, raw_seq & SEQ_MASK};
    }

    if (count == 0) return std::nullopt;

    // Simple insertion sort for small arrays
    // for (size_t i = 1; i < count; ++i) {
    //     Candidate key = candidates[i];
    //     size_t j = i;
    //     while (j > 0 && candidates[j - 1].seq > key.seq) {
    //         candidates[j] = candidates[j - 1];
    //         --j;
    //     }
    //     candidates[j] = key;
    // }

    std::sort(candidates, candidates + count, [](const Candidate &a, const Candidate &b) { return a.seq < b.seq; });

    for (size_t i = 0; i < count; ++i) {
        size_t idx = candidates[i].index;
        if (!with_lock) return idx;

        WriteMetadata *wmeta = &wmeta_start_addr_[idx];
        if (wmeta->WLockOnly()) {
            return idx;  // locking succeeded
        }
    }

    return std::nullopt;  // all locking attempts failed
}

/* SharedMetadata */
std::optional<size_t> SharedMetadata::ReserveWmeta(const BlockId &block_id, const size_t placeholder, bool with_lock,
                                                   std::optional<size_t> hint) {
    (void)placeholder;
    size_t wm_index;
    size_t start_line;
    if (hint.has_value()) {
        // assert(hint.value() < b_meta_.wmeta_slot_len_);
        start_line = hint.value();
    } else {
        start_line = static_cast<size_t>(rng());
    }
    for (size_t i = 0; i < wmeta_slot_len_; ++i) {
        wm_index = (i + start_line) % wmeta_slot_len_;
        WriteMetadata *wmeta = &wmeta_start_addr_[wm_index];
        if (wmeta->TryAllocateLock(block_id, with_lock)) {
            allocated_slot_num_.fetch_add(1, std::memory_order_relaxed);
            return wm_index;
        }
        cpu_relax();
    }

    return std::nullopt;
}

std::optional<size_t> SharedMetadata::ReserveWmetaEarlyTerm(const BlockId &block_id, const size_t placeholder,
                                                            size_t count, bool with_lock, std::optional<size_t> hint) {
    (void)placeholder;
    size_t wm_index;
    size_t start_line;
    if (hint.has_value()) {
        // assert(hint.value() < b_meta_.wmeta_slot_len_);
        start_line = hint.value();
    } else {
        start_line = static_cast<size_t>(rng());
    }
    for (size_t i = 0; i < std::min(wmeta_slot_len_, count); ++i) {
        wm_index = (i + start_line) % wmeta_slot_len_;
        WriteMetadata *wmeta = &wmeta_start_addr_[wm_index];
        if (wmeta->TryAllocateLock(block_id, with_lock)) {
            allocated_slot_num_.fetch_add(1, std::memory_order_relaxed);
            return wm_index;
        }
        cpu_relax();
    }

    return std::nullopt;
}

std::optional<size_t> SharedMetadata::CheckReserveWmeta(const BlockId &block_id, const size_t placeholder,
                                                        bool with_lock, std::optional<size_t> hint) {
    (void)placeholder;
    size_t wm_index;
    size_t start_line;
    if (hint.has_value()) {
        // assert(hint.value() < b_meta_.wmeta_slot_len_);
        start_line = hint.value();
    } else {
        size_t cnt = GetWmetaCount();
        /**
         * check the allocation ratio of wmeta slots.
         * E(# of slots to iterate until a free slot) = 1/(cnt/wmeta_slot_len_)
         * e.g. when cnt/wmeta_slot_len_ = 0.95 (slots are 5% empty),
         * expectedly we need to traverse 20 slots to find a free slot.
         */
        if (static_cast<double>(cnt) / static_cast<double>(wmeta_slot_len_) > 0.95) return std::nullopt;
        start_line = static_cast<size_t>(rng());
    }
    for (size_t i = 0; i < wmeta_slot_len_; ++i) {
        wm_index = (i + start_line) % wmeta_slot_len_;
        WriteMetadata *wmeta = &wmeta_start_addr_[wm_index];
        if (wmeta->TryAllocateLock(block_id, with_lock)) {
            allocated_slot_num_.fetch_add(1, std::memory_order_relaxed);
            return wm_index;
        }
        cpu_relax();
    }

    return std::nullopt;
}

bool SharedMetadata::RecycleWmeta(size_t wm_index, const size_t placeholder, bool with_lock) {
    (void)placeholder;
    WriteMetadata *wmeta = &wmeta_start_addr_[wm_index];
    if (with_lock) {
        if (wmeta->FreeLock()) {
            allocated_slot_num_.fetch_sub(1, std::memory_order_relaxed);
            return true;
        } else
            return false;
    } else {
        (void)wmeta->FreeLock(false);  // explicitly ignore the return value
        allocated_slot_num_.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }
}

thread_local std::minstd_rand SharedMetadata::rng;

/**
 * PartitionMetadata for tigon-like partitioned index
 */
std::optional<size_t> PartitionMetadata::CheckReserveWmeta(const BlockId &block_id, const size_t target_partition,
                                                           bool with_lock, std::optional<size_t> hint) {
    size_t wm_index;
    size_t start_line;
    size_t min_idx = target_partition * per_partition_slot_len_;
    size_t max_idx = min_idx + per_partition_slot_len_;
    CHECK(target_partition < num_partition_)
        << "target_partition=" << target_partition << ">= num_partition_=" << num_partition_;
    if (hint.has_value()) {
        start_line = hint.value();
        if (hint.value() >= max_idx || hint.value() < min_idx) {
            size_t cnt = GetWmetaCountPartition(target_partition);
            if (static_cast<double>(cnt) / static_cast<double>(per_partition_slot_len_) > 0.95) return std::nullopt;
            start_line = static_cast<size_t>(rng());

            LOG(WARNING) << "invalid hint: " << hint.value() << " (min_idx=" << min_idx << ", max_idx=" << max_idx
                         << ")";
        }
    } else {
        size_t cnt = GetWmetaCountPartition(target_partition);
        /**
         * check the allocation ratio of wmeta slots.
         * E(# of slots to iterate until a free slot) = 1/(cnt/wmeta_slot_len_)
         * e.g. when cnt/wmeta_slot_len_ = 0.95 (slots are 5% empty),
         * expectedly we need to traverse 20 slots to find a free slot.
         */
        if (static_cast<double>(cnt) / static_cast<double>(per_partition_slot_len_) > 0.95) return std::nullopt;
        start_line = static_cast<size_t>(rng());
    }
    for (size_t i = 0; i < per_partition_slot_len_; ++i) {
        wm_index = ((i + start_line) % per_partition_slot_len_) + min_idx;
        WriteMetadata *wmeta = &wmeta_start_addr_[wm_index];
        if (wmeta->TryAllocateLock(block_id, with_lock)) {
            allocated_slot_num_.fetch_add(1, std::memory_order_relaxed);
            allocated_slot_num_arr_[target_partition].fetch_add(1, std::memory_order_relaxed);
            return wm_index;
        }
        cpu_relax();
    }

    return std::nullopt;
}

bool PartitionMetadata::RecycleWmeta(size_t wm_index, const size_t target_partition, bool with_lock) {
    // sanity check
    size_t min_idx = target_partition * per_partition_slot_len_;
    size_t max_idx = min_idx + per_partition_slot_len_;
    CHECK(target_partition < num_partition_)
        << "target_partition=" << target_partition << ">= num_partition_=" << num_partition_;
    if (wm_index >= max_idx || wm_index < min_idx) {
        LOG(WARNING) << "invalid wm_index: " << wm_index << " (min_idx=" << min_idx << ", max_idx=" << max_idx << ")";
        return false;
    }

    WriteMetadata *wmeta = &wmeta_start_addr_[wm_index];
    if (with_lock) {
        if (wmeta->FreeLock()) {
            allocated_slot_num_.fetch_sub(1, std::memory_order_relaxed);  // OPTION: remove to save time
            allocated_slot_num_arr_[target_partition].fetch_sub(1, std::memory_order_relaxed);
            return true;
        } else
            return false;
    } else {
        (void)wmeta->FreeLock(false);                                 // explicitly ignore the return value
        allocated_slot_num_.fetch_sub(1, std::memory_order_relaxed);  // OPTION: remove to save time
        allocated_slot_num_arr_[wm_index].fetch_sub(1, std::memory_order_relaxed);
        return true;
    }
}

std::optional<size_t> PartitionMetadata::SampleVictim(const size_t sample_size, const size_t target_partition,
                                                      bool with_lock) {
    struct Candidate {
        size_t index;
        uint32_t seq;
    };

    CHECK(sample_size <= MAX_WMETA_SAMPLING_SIZE) << "sample_size exceeds hard limit";

    Candidate candidates[MAX_WMETA_SAMPLING_SIZE];
    size_t count = 0;

    size_t min_idx = target_partition * per_partition_slot_len_;

    for (size_t i = 0; i < sample_size; ++i) {
        size_t candidate_idx = min_idx + (rng() % per_partition_slot_len_);
        WriteMetadata *wmeta = &wmeta_start_addr_[candidate_idx];

        uint32_t raw_seq = wmeta->seqlock();
        if (raw_seq & FREE_BIT) continue;

        candidates[count++] = Candidate{candidate_idx, raw_seq & SEQ_MASK};
    }

    if (count == 0) return std::nullopt;

    std::sort(candidates, candidates + count, [](const Candidate &a, const Candidate &b) { return a.seq < b.seq; });

    for (size_t i = 0; i < count; ++i) {
        size_t idx = candidates[i].index;
        if (!with_lock) return idx;

        WriteMetadata *wmeta = &wmeta_start_addr_[idx];
        if (wmeta->WLockOnly()) {
            return idx;  // locking succeeded
        }
    }

    return std::nullopt;  // all locking attempts failed
}

thread_local std::minstd_rand PartitionMetadata::rng;

}  // namespace rackobj::common
