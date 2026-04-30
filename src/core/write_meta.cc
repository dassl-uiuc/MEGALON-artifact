#include "write_meta.h"

namespace rackobj::common {

bool WriteMetadata::WSeqBegin() { return write_seqlock_begin(&seqlock_); }

uint32_t WriteMetadata::WSeqEnd() { return write_seqlock_end(&seqlock_); }

bool WriteMetadata::WLockOnly() { return write_seqlock_only(&seqlock_); }

void WriteMetadata::WUnlockOnly() { write_sequnlock_only(&seqlock_); }

uint32_t WriteMetadata::RSeqBegin() { return read_seqlock_begin(&seqlock_); }

/**
 * return:
 * true: the sequence is not the same as the given sequence (need retry)
 * false: the sequence is the same as the given sequence (no need retry)
 */
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

std::optional<size_t> SharedMetadata::SampleVictim(const size_t sample_size, bool with_lock) {
    struct Candidate {
        size_t index;
        uint32_t seq;
    };

    assert(sample_size <= MAX_WMETA_SAMPLING_SIZE && "sample_size exceeds hard limit");

    Candidate candidates[MAX_WMETA_SAMPLING_SIZE];
    size_t count = 0;

    for (size_t i = 0; i < sample_size; ++i) {
        size_t candidate_idx = rng() % b_meta_.wmeta_slot_len_;
        WriteMetadata *wmeta = &b_meta_.wmeta_start_addr_[candidate_idx];

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

        WriteMetadata *wmeta = &b_meta_.wmeta_start_addr_[idx];
        if (wmeta->WLockOnly()) {
            return idx;  // locking succeeded
        }
    }

    return std::nullopt;  // all locking attempts failed
}

/* SharedMetadata */
std::optional<size_t> SharedMetadata::ReserveWmeta(const BlockId &block_id, bool with_lock,
                                                   std::optional<size_t> hint) {
    size_t wm_index;
    size_t start_line;
    if (hint.has_value()) {
        assert(hint.value() < wmeta_slot_len_);
        start_line = hint.value();
    } else {
        start_line = static_cast<size_t>(rng());
    }
    for (size_t i = 0; i < b_meta_.wmeta_slot_len_; ++i) {
        wm_index = (i + start_line) % b_meta_.wmeta_slot_len_;
        WriteMetadata *wmeta = &b_meta_.wmeta_start_addr_[wm_index];
        if (wmeta->TryAllocateLock(block_id, with_lock)) {
            b_meta_.allocated_slot_num_.fetch_add(1, std::memory_order_relaxed);
            b_meta_.allocate_count_.fetch_add(1, std::memory_order_relaxed);
            return wm_index;
        }
        cpu_relax();
    }

    return std::nullopt;
}

std::optional<size_t> SharedMetadata::ReserveWmetaEarlyTerm(const BlockId &block_id, size_t count, bool with_lock,
                                                            std::optional<size_t> hint) {
    size_t wm_index;
    size_t start_line;
    if (hint.has_value()) {
        assert(hint.value() < wmeta_slot_len_);
        start_line = hint.value();
    } else {
        start_line = static_cast<size_t>(rng());
    }
    for (size_t i = 0; i < std::min(b_meta_.wmeta_slot_len_, count); ++i) {
        wm_index = (i + start_line) % b_meta_.wmeta_slot_len_;
        WriteMetadata *wmeta = &b_meta_.wmeta_start_addr_[wm_index];
        if (wmeta->TryAllocateLock(block_id, with_lock)) {
            b_meta_.allocated_slot_num_.fetch_add(1, std::memory_order_relaxed);
            b_meta_.allocate_count_.fetch_add(1, std::memory_order_relaxed);
            return wm_index;
        }
        cpu_relax();
    }

    return std::nullopt;
}

std::optional<size_t> SharedMetadata::CheckReserveWmeta(const BlockId &block_id, bool with_lock,
                                                        std::optional<size_t> hint) {
    size_t wm_index;
    size_t start_line;
    if (hint.has_value()) {
        assert(hint.value() < wmeta_slot_len_);
        start_line = hint.value();
    } else {
        size_t cnt = GetWmetaCount();
        /**
         * check the allocation ratio of wmeta slots.
         * E(# of slots to iterate until a free slot) = 1/(cnt/wmeta_slot_len_)
         * e.g. when cnt/wmeta_slot_len_ = 0.95 (slots are 5% empty),
         * expectedly we need to traverse 20 slots to find a free slot.
         */
        if (static_cast<double>(cnt) / static_cast<double>(b_meta_.wmeta_slot_len_) > 0.95) return std::nullopt;
        start_line = static_cast<size_t>(rng());
    }
    for (size_t i = 0; i < b_meta_.wmeta_slot_len_; ++i) {
        wm_index = (i + start_line) % b_meta_.wmeta_slot_len_;
        WriteMetadata *wmeta = &b_meta_.wmeta_start_addr_[wm_index];
        if (wmeta->TryAllocateLock(block_id, with_lock)) {
            b_meta_.allocated_slot_num_.fetch_add(1, std::memory_order_relaxed);
            b_meta_.allocate_count_.fetch_add(1, std::memory_order_relaxed);
            return wm_index;
        }
        cpu_relax();
    }

    return std::nullopt;
}

bool SharedMetadata::RecycleWmeta(size_t wm_index, bool with_lock) {
    WriteMetadata *wmeta = &b_meta_.wmeta_start_addr_[wm_index];
    if (with_lock) {
        if (wmeta->FreeLock()) {
            b_meta_.allocated_slot_num_.fetch_sub(1, std::memory_order_relaxed);
            b_meta_.reclaim_count_.fetch_add(1, std::memory_order_relaxed);
            return true;
        } else
            return false;
    } else {
        (void)wmeta->FreeLock(false);  // explicitly ignore the return value
        b_meta_.allocated_slot_num_.fetch_sub(1, std::memory_order_relaxed);
        b_meta_.reclaim_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
}

void SharedMetadata::CreateLocalSeqMap(int nid,
                                       const std::shared_ptr<common::AllocatableLocalMemoryRegion> &local_region) {
    LocalMemoryAllocator<std::vector<VectorElement, VecEntryAllocator>> vector_alloc(local_region);
    auto vector = vector_alloc.allocate();
    std::construct_at(vector, b_meta_.wmeta_slot_len_, VecEntryAllocator(local_region));
    local_seqcount_vec_[nid] = std::unique_ptr<std::vector<VectorElement, VecEntryAllocator>>(vector);
    std::for_each(local_seqcount_vec_[nid]->begin(), local_seqcount_vec_[nid]->end(), [](VectorElement &entry) {
        entry.first = std::nullopt;
        entry.second = 0;
    });

    LocalMemoryAllocator<std::vector<SpinLockWrapper, LockVecEntryAllocator>> lock_alloc(local_region);
    auto lock_vec = lock_alloc.allocate();
    std::construct_at(lock_vec, b_meta_.wmeta_slot_len_, LockVecEntryAllocator(local_region));
    local_map_lock_[nid] = std::unique_ptr<std::vector<SpinLockWrapper, LockVecEntryAllocator>>(lock_vec);
    // std::for_each(local_map_lock_[nid]->begin(), local_map_lock_[nid]->end(),
    //               [](SpinLockWrapper &entry) { pthread_spin_init(&entry, PTHREAD_PROCESS_SHARED); });
}

thread_local std::minstd_rand SharedMetadata::rng;

}  // namespace rackobj::common
