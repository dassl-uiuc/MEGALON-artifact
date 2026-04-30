#ifndef RACKOBJ_SHARED_MEMORY_PAGE_CACHE_H
#define RACKOBJ_SHARED_MEMORY_PAGE_CACHE_H

#include <pthread.h>

#include <optional>
#include <type_traits>

#include "ankerl/unordered_dense.h"
#include "blockid.h"
#include "cache_slot.h"
#include "lru_policy.h"
#include "page_data.h"
#include "scr_bitmap.h"
#include "tsl/robin_map.h"

namespace rackobj::lib {

template <typename Policy>
class SharedMemoryObjectHandle;
template <typename Policy>
class PageServer;

}  // namespace rackobj::lib

namespace rackobj::common {

template <typename Policy>
class ReplicateManager;

template <typename Policy>
class EvictManager;

class BasePageCache {
    template <typename Policy>
    friend class rackobj::lib::SharedMemoryObjectHandle;
    template <typename Policy>
    friend class rackobj::lib::PageServer;
    template <typename Policy>
    friend class EvictManager;
    friend class CacheNodeHandle;
    friend class FlushManager;

public:
    BasePageCache(size_t num_pages) : page_count_(num_pages) {}

    virtual ~BasePageCache() {}

    virtual size_t GetActivePageCount() const = 0;

    virtual size_t GetDirtyPageCount() const = 0;

    size_t Size() { return page_count_; }

protected:
    size_t page_count_; /* total num pages in this cache */
};

class SharedMemoryObject : public BasePageCache {
    template <typename Policy>
    friend class rackobj::lib::SharedMemoryObjectHandle;
    template <typename Policy>
    friend class rackobj::lib::PageServer;
    template <typename Policy>
    friend class ReplicateManager;
    template <typename Policy>
    friend class EvictManager;
    friend class CacheNodeHandle;
    friend class FlushManager;
    friend class WriteMetadataManager;

public:
    static std::shared_ptr<SharedMemoryObject> CreateSharedMemoryObject(
        size_t num_pages, const std::shared_ptr<AllocatableLocalMemoryRegion> &sc_shm_region,
        const std::shared_ptr<AllocatableLocalMemoryRegion> &nc_shm_region);

    SharedMemoryObject(size_t num_pages, const std::shared_ptr<AllocatableLocalMemoryRegion> &sc_shm_region,
                       const std::shared_ptr<AllocatableLocalMemoryRegion> &nc_shm_region);

    ~SharedMemoryObject() {}

    size_t GetActivePageCount() const override { return scr_bitmap_.GetCount(); }

    size_t GetDirtyPageCount() const override { return scr_bitmap_.GetDirtyCount(); }

    void Reinitialize(size_t cn_idx, const BlockId &new_block_id) {
        cache_slot_.Reinitialize(cn_idx, new_block_id);
        scr_bitmap_.SetDirty(cn_idx, false);
    }

    uint8_t *GetPage(size_t cn_idx) const { return page_data_.GetPage(cn_idx); }

    uint8_t *GetPageBaseAddr() const { return page_data_.GetPageBaseAddr(); }

protected:
    SharedBitmap scr_bitmap_;  // resides on SCR

    CacheSlot cache_slot_;  // resides on NCR

    PageData page_data_;  // resides on NCR
};

template <typename Policy>
class LocalMemoryObject : public BasePageCache {
    friend class rackobj::lib::SharedMemoryObjectHandle<Policy>;
    friend class rackobj::lib::PageServer<Policy>;
    friend class ReplicateManager<Policy>;
    friend class EvictManager<Policy>;

    using PolicyAllocator = RebindLocalMemoryByteAllocatorT<typename Policy::PolicyNode>;

public:
    static std::shared_ptr<LocalMemoryObject<Policy>> CreateLocalMemoryObject(
        size_t num_pages, const std::shared_ptr<AllocatableLocalMemoryRegion> &local_region);

    explicit LocalMemoryObject(size_t num_pages, const std::shared_ptr<AllocatableLocalMemoryRegion> &local_region)
        : BasePageCache(num_pages), policy_(num_pages, PolicyAllocator(local_region)) {
        // TODO: we are currently temporarily sticking pages at end of allocation
        LocalMemoryByteAllocator byte_allocator(local_region);
        pages_ = byte_allocator.allocate(common::BlockId::kBlockSize * num_pages);
        CHECK(pages_) << "page data allocation failed!";
        policy_.Initialize(pages_, num_pages);

        DLOG(INFO) << "LocalMemoryObject &policy_=" << (void *)&policy_ << ", pages_=" << (void *)pages_;
    }

    size_t GetActivePageCount() const override { return policy_.GetCount(); }

    size_t GetDirtyPageCount() const override { return 0; }

private:
    uint8_t *pages_;

    alignas(64) Policy policy_;
};

}  // namespace rackobj::common

//#include "detail/object_slot.hpp"

#endif  // RACKOBJ_SHARED_MEMORY_PAGE_CACHE_H
