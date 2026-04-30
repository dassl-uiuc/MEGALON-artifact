//#ifndef RACKOBJ_SHARED_MEMORY_DETAIL_OBJECT_HPP
//#define RACKOBJ_SHARED_MEMORY_DETAIL_OBJECT_HPP

#include "core/object_slot.h"

#include "absl/log/log.h"
#include "common/debug.h"
#include "globals.h"
#include "shared_memory/region.h"

namespace rackobj::common {

using std::remove_pointer_t;
using std::shared_ptr;
using std::string;

std::shared_ptr<SharedMemoryObject> SharedMemoryObject::CreateSharedMemoryObject(
    size_t num_pages, const std::shared_ptr<AllocatableLocalMemoryRegion> &sc_shm_region,
    const std::shared_ptr<AllocatableLocalMemoryRegion> &nc_shm_region) {
    LocalMemoryAllocator<SharedMemoryObject> cache_alloc(nc_shm_region);
    auto cache = cache_alloc.AllocateAligned<MemoryAlignment::kCacheAlign>();
    std::construct_at(cache, num_pages, sc_shm_region, nc_shm_region);

    return shared_ptr<SharedMemoryObject>(cache, [cache](auto ptr) {
        DLOG(INFO) << "Deallocating " << TypeToString(cache) << " at " << (void *)ptr;
        std::destroy_at(ptr);
    });
}

SharedMemoryObject::SharedMemoryObject(size_t num_pages,
                                       const std::shared_ptr<AllocatableLocalMemoryRegion> &sc_shm_region,
                                       const std::shared_ptr<AllocatableLocalMemoryRegion> &nc_shm_region)
    : BasePageCache(num_pages),
      scr_bitmap_(num_pages, sc_shm_region),
      cache_slot_(num_pages, nc_shm_region),
      page_data_(num_pages, nc_shm_region) {
    // TODO: we are currently temporarily sticking pages at end of allocation
    cache_slot_.Initialize(num_pages);
}

template <typename Policy>
std::shared_ptr<LocalMemoryObject<Policy>> LocalMemoryObject<Policy>::CreateLocalMemoryObject(
    size_t num_pages, const std::shared_ptr<AllocatableLocalMemoryRegion> &local_region) {
    LocalMemoryAllocator<LocalMemoryObject<Policy>> cache_alloc(local_region);
    LocalMemoryObject<Policy> *cache = cache_alloc.allocate();
    std::construct_at(cache, num_pages, local_region);

    LOG(INFO) << "LocalMemoryObject constructed with slots: " << num_pages;

    return std::shared_ptr<LocalMemoryObject<Policy>>(cache, [cache](auto ptr) {
        DLOG(INFO) << "Deallocating " << TypeToString(cache) << " at " << (void *)ptr;
        std::destroy_at(ptr);
    });
}

template class LocalMemoryObject<lib::CurrPolicy>;

}  // namespace rackobj::common

//#endif  // RACKOBJ_SHARED_MEMORY_DETAIL_PAGE_CACHE_HPP
