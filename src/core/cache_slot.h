#pragma once

#include "cache_node.h"
#include "shared_memory/allocator.h"

namespace rackobj::common {

class CacheSlot final {
public:
    CacheSlot(size_t num_pages, const std::shared_ptr<AllocatableLocalMemoryRegion> &nc_shm_region)
        //    : slots_(allocator.allocate(max_elements + ReserveNodeCount(max_elements))),
        : slots_(nullptr), max_elements_(num_pages) {
        LocalMemoryAllocator<CacheNode> cn_allocator(nc_shm_region);
        slots_ = cn_allocator.AllocateAligned<MemoryAlignment::kCacheAlign>(num_pages);
        CHECK(slots_) << "cache slot allocation failed!";
        DLOG(INFO) << "Non-Coherent Region CacheSlot constructed, &slot_=@" << (void *)slots_;
    }

    ~CacheSlot() {
        for (size_t i = 0; i < max_elements_; ++i) std::destroy_at(&slots_[i]);
    }

    void Reinitialize(size_t cn_index, const BlockId &new_block_id) {
        CacheNode *cn = &slots_[cn_index];
        cn->Reinitialize(new_block_id);
    }

    void Initialize(size_t max_elements) {
        DLOG(INFO) << "init cachenodes";
        for (size_t i = 0; i < max_elements; ++i) {
            std::construct_at(&slots_[i]);
        }

        //    size_t total_elements = max_elements + ReserveNodeCount(max_elements);
        size_t total_elements = max_elements;

        LOG(INFO) << "cache slot setting max elements: " << max_elements << ", total_elements: " << total_elements;
    }

    size_t GetMaxElements() const { return max_elements_; }

    CacheNode *GetCacheNode(size_t cn_index) {
        CacheNode *cn = &slots_[cn_index];
        return cn;
    }

private:
    alignas(64) CacheNode *slots_;

    size_t max_elements_;
};

}  // namespace rackobj::common
