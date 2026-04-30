#ifndef RACKOBJ_SHARED_BITMAP_H
#define RACKOBJ_SHARED_BITMAP_H

#include <bitset>
#include <random>

#include "absl/log/log.h"
#include "arch/arch.h"
#include "bitmap.h"
#include "common/constants.h"
#include "seqcount.h"
#include "shared_memory/allocator.h"
#include "shared_memory/region.h"

namespace rackobj::common {

enum bitmap_entry_flags {
    BITMAP_VALID = BIT(0),
    BITMAP_FREE = BIT(1),
    BITMAP_DIRTY = BIT(2),
};

// SCR
class SharedBitmap final : public BaseBitmap {
    using BitmapAllocator = RebindLocalMemoryByteAllocatorT<uint64_t>;

public:
    explicit SharedBitmap(size_t num_pages, size_t wmeta_threshold,
                          const std::shared_ptr<AllocatableLocalMemoryRegion> &sc_shm_region);

    explicit SharedBitmap(size_t num_pages, const std::shared_ptr<AllocatableLocalMemoryRegion> &sc_shm_region);

    ~SharedBitmap();

    void MarkDirty(size_t cn_index) { SetDirty(cn_index, true); }

    bool IsDirty(size_t cn_index) const { return bitmap_testbit(dirty_bitmap_start_addr_, cn_index); }

    void SetDirty(size_t cn_index, bool dirty) {
        if (dirty) {
            bitmap_setbit(dirty_bitmap_start_addr_, cn_index);
            dirty_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            bitmap_clearbit(dirty_bitmap_start_addr_, cn_index);
            dirty_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // Grab a node from the free list. It is an error to call this function
    // without knowledge that there is a free node available.
    bool ReserveCacheNode(size_t cn_index)  // Insert
    {
        bool succeed;
        size_t index = bitmap_index(cn_index);
        uint32_t offset = bitmap_offset(cn_index);

        succeed = try_clearbit_exclusive(free_bitmap_start_addr_, index, offset);
        if (succeed) count_.fetch_add(1, std::memory_order_release);
        return succeed;
    }

    constexpr size_t bitmap_cn_index(size_t index, uint32_t offset) const { return (index << BITMAP_SHIFT) + offset; }

    /* Grab a node from the free list. */
    std::optional<size_t> ReserveCacheNode() {
        bool succeed;
        size_t index;
        uint32_t offset;
        do {
            size_t bitmap_len = BITS_TO_LONGS(num_pages_);
#ifdef RANDSLOTALLOC
            uint32_t start_point = static_cast<uint32_t>(static_cast<size_t>(std::rand()) % bitmap_len);
#else
            uint32_t start_point = static_cast<uint32_t>(count_.load(std::memory_order_relaxed));
#endif
            if (!bitmap_find_setbit(free_bitmap_start_addr_, bitmap_len, index, offset, start_point))
                return std::nullopt;
            succeed = try_clearbit_exclusive(free_bitmap_start_addr_, index, offset);
        } while (!succeed);

        count_.fetch_add(1, std::memory_order_release);
        return bitmap_cn_index(index, offset);
    }

    bool RecycleCacheNode(size_t cn_index)  // Evict
    {
        bool succeed;
        size_t index = bitmap_index(static_cast<size_t>(cn_index));
        uint32_t offset = bitmap_offset(static_cast<size_t>(cn_index));

        succeed = try_setbit(free_bitmap_start_addr_, index, offset);
        if (succeed) count_.fetch_sub(1, std::memory_order_release);
        return succeed;
    }

    bool IsFull() const { return GetCount() >= num_pages_; }

    size_t GetCount() const { return count_.load(std::memory_order_acquire); }

    size_t GetDirtyCount() const { return dirty_count_.load(std::memory_order_acquire); }

private:
    // size_t page_count_; --> use page_count in shm_obj class

    // This lock is to synchronize filling this block from an RPC and/or writes
    // to this block from competing processes. This is a spinlock since we do
    // not anticipate much contention to this lock and since a full mutex is
    // 40 bytes as opposed to the spinlocks 4 bytes.
    alignas(64) uint64_t *wlock_bitmap_start_addr_;

    alignas(64) uint64_t *dirty_bitmap_start_addr_;

    alignas(64) uint64_t *free_bitmap_start_addr_;

    alignas(64) uint64_t *wmeta_bitmap_start_addr_;

    std::unique_ptr<BitmapAllocator> bitmap_allocator_;

    const size_t num_pages_;
    // size_t wmeta_slots_;

    std::atomic<size_t> count_;
    std::atomic<size_t> wmeta_count_;
    std::atomic<size_t> dirty_count_;
    // int64_t wmeta_count_threshold_;

    static thread_local std::minstd_rand rng;
};

/*
 *
unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags)
{
    return kmalloc_array(BITS_TO_LONGS(nbits), sizeof(unsigned long),
                 flags);
}

static inline void *kmalloc_array(unsigned n, size_t s, gfp_t gfp)
{
    return kmalloc(n * s, gfp);
}
 *
 * */

}  // namespace rackobj::common

#endif  // RACKOBJ_SHARED_BITMAP_H