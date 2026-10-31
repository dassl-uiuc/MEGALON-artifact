#pragma once

#include "cache_node.h"
#include "shared_memory/allocator.h"

namespace rackobj::common {

class CacheSlot final {
    // Each shared CacheNode gets its own cache line: hosts write back whole lines, so neighbouring
    // nodes must not share one (a stale write-back would clobber the neighbour's block_id_).
    struct alignas(64) PaddedCacheNode {
        CacheNode node;
    };
    static_assert(sizeof(PaddedCacheNode) == 64, "CacheNode must fit in one cache line");

public:
    CacheSlot(size_t num_pages, const std::shared_ptr<AllocatableLocalMemoryRegion> &nc_shm_region)
        //    : slots_(allocator.allocate(max_elements + ReserveNodeCount(max_elements))),
        : slots_(nullptr), evict_cnt_(nullptr), max_elements_(num_pages) {
        LocalMemoryAllocator<PaddedCacheNode> cn_allocator(nc_shm_region);
        slots_ = cn_allocator.AllocateAligned<MemoryAlignment::kCacheAlign>(num_pages);
        CHECK(slots_) << "cache slot allocation failed!";
        LocalMemoryAllocator<EvictCounter> evict_allocator(nc_shm_region);
        evict_cnt_ = evict_allocator.AllocateAligned<MemoryAlignment::kCacheAlign>(num_pages);
        CHECK(evict_cnt_) << "evict counter allocation failed!";
        DLOG(INFO) << "Non-Coherent Region CacheSlot constructed, &slot_=@" << (void *)slots_;
    }

    ~CacheSlot() {
        for (size_t i = 0; i < max_elements_; ++i) {
            std::destroy_at(&slots_[i].node);
            std::destroy_at(&evict_cnt_[i]);
        }
    }

    void Reinitialize(size_t cn_index, const BlockId &new_block_id) {
        CacheNode *cn = &slots_[cn_index].node;
        cn->Reinitialize(new_block_id);
    }

    void Initialize(size_t max_elements) {
        DLOG(INFO) << "init cachenodes";
        for (size_t i = 0; i < max_elements; ++i) {
            std::construct_at(&slots_[i].node);
            std::construct_at(&evict_cnt_[i]);
        }

        //    size_t total_elements = max_elements + ReserveNodeCount(max_elements);
        size_t total_elements = max_elements;

        LOG(INFO) << "cache slot setting max elements: " << max_elements << ", total_elements: " << total_elements;
    }

    size_t GetMaxElements() const { return max_elements_; }

    CacheNode *GetCacheNode(size_t cn_index) {
        DCHECK(cn_index < max_elements_) << "cn_index " << cn_index << " out of range " << max_elements_;
        CacheNode *cn = &slots_[cn_index].node;
        return cn;
    }

    // Eviction hints of shared CacheNodes. They are updated on every access by every host and are
    // allowed to be inaccurate, so they live in a separate array instead of the CacheNode line.
    bool ShouldEvict(size_t cn_index) { return evict_cnt_[cn_index].ShouldEvict(); }

    bool DecreaseEvict(size_t cn_index) { return evict_cnt_[cn_index].DecreaseEvict(); }

    void ClearEvict(size_t cn_index) { evict_cnt_[cn_index].ClearEvict(); }

private:
    alignas(64) PaddedCacheNode *slots_;

    EvictCounter *evict_cnt_;

    size_t max_elements_;
};

}  // namespace rackobj::common
