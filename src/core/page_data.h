#pragma once

#include <x86intrin.h>

#include "cache_node.h"
#include "shared_memory/allocator.h"

namespace rackobj::common {

class PageData final {
public:
    PageData(size_t num_pages, const std::shared_ptr<AllocatableLocalMemoryRegion>& nc_shm_region)
        : page_data_base_addr_(nullptr) {
        LocalMemoryByteAllocator ncr_byte_allocator(nc_shm_region);
        page_data_base_addr_ =
            ncr_byte_allocator.AllocateAligned<MemoryAlignment::kPageAlign>(common::BlockId::kBlockSize * num_pages);
        CHECK(page_data_base_addr_) << "page data allocation failed!";
        DLOG(INFO) << "Non-Coherent Region PageData constructed, &page_data_base_addr_=@"
                   << (void*)page_data_base_addr_;
    }

    ~PageData() {
        // DLOG(INFO) << "page data elem number: " << GetCount() << " out of " << max_elements_;
    }

    uint8_t* GetDataSlot(size_t cn_idx) const {
        size_t offset = common::BlockId::kBlockSize * cn_idx;
        if (!page_data_base_addr_) {
            LOG(ERROR) << "page data base address is nullptr";
            return nullptr;
        }
        return &page_data_base_addr_[offset];
    }

    uint8_t* GetPageFlush(size_t cn_idx) const {
        uint8_t* addr = GetDataSlot(cn_idx);
        _mm_mfence(); /* prevent clflush from being reordered by the CPU or the compiler in this direction */
        _mm_clflush(addr);
        _mm_mfence(); /* this properly orders both clflush and rdtsc*/
        return addr;
    }

    uint8_t* GetPageBaseAddr() const { return page_data_base_addr_; };

    // bool IsFull() const { return GetCount() >= max_elements_; }

    // size_t GetMaxElements() const { return max_elements_; }

    // size_t GetCount() const { return count_.load(std::memory_order_acquire); }

private:
    // A pointer to the block given by the allocation of nodes for this linked
    // list.
    alignas(64) uint8_t* page_data_base_addr_;  // resides on NCR

    // size_t max_elements_;
};

}  // namespace rackobj::common
