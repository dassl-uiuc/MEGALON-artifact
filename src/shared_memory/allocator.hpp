#ifndef RACKOBJ_SHARED_MEMORY_DETAIL_ALLOCATOR_HPP
#define RACKOBJ_SHARED_MEMORY_DETAIL_ALLOCATOR_HPP
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "allocator.h"
#include "common/debug.h"

namespace rackobj::common {

template <typename T>
SharedMemoryAllocator<T>::SharedMemoryAllocator(std::shared_ptr<AllocatableSharedMemoryRegion> shm_region) noexcept
    : shm_region_(shm_region) {
    CHECK(shm_region_ != nullptr) << "SharedMemoryAllocator initialized with "
                                     "uninitialized SharedMemoryRegion";
}

template <typename T>
template <typename U>
SharedMemoryAllocator<T>::SharedMemoryAllocator(const SharedMemoryAllocator<U>& other) noexcept
    : shm_region_(other.shm_region_) {
    CHECK(shm_region_ != nullptr) << "SharedMemoryAllocator initialized with "
                                     "uninitialized SharedMemoryRegion";
}

template <typename T>
T* SharedMemoryAllocator<T>::allocate(const size_t n) const {
    return AllocateAligned<MemoryAlignment::kPageAlign>(n);
}

template <MemoryAlignment Alignment>
constexpr const char* AlignmentToString() {
    if constexpr (Alignment == MemoryAlignment::kCacheAlign) {
        return "kCacheAlign";
    } else if constexpr (Alignment == MemoryAlignment::kPageAlign) {
        return "kPageAlign";
    } else if constexpr (Alignment == MemoryAlignment::kNoAlign) {
        return "kNoAlign";
    } else {
        static_assert(rackobj::not_implemented_t<MemoryAlignment>::value, "Invalid alignment");
    }
}

template <typename T>
template <MemoryAlignment Alignment>
T* SharedMemoryAllocator<T>::AllocateAligned(const size_t n) const {
    CHECK(n != 0) << "zero length allocation";
    CHECK(n <= static_cast<size_t>(-1) / sizeof(T)) << "bad_array_new_length";

    DLOG(INFO) << "AllocateAligned<" << AlignmentToString<Alignment>() << ">(" << n << ") [" << TypeToString<T>()
               << "]";
    size_t requested = n * sizeof(T);
    return (T*)shm_region_->Allocate<Alignment>(requested);
}

template <typename T>
void SharedMemoryAllocator<T>::deallocate(T* const p, size_t n) const noexcept {
    DLOG(INFO) << "deallocate(" << p << ", " << n << ")";
}

// local allocator
template <typename T>
LocalMemoryAllocator<T>::LocalMemoryAllocator(std::shared_ptr<AllocatableLocalMemoryRegion> shm_region) noexcept
    : shm_region_(shm_region) {
    CHECK(shm_region_ != nullptr) << "LocalMemoryAllocator initialized with "
                                     "uninitialized LocalMemoryRegion";
}

template <typename T>
template <typename U>
LocalMemoryAllocator<T>::LocalMemoryAllocator(const LocalMemoryAllocator<U>& other) noexcept
    : shm_region_(other.shm_region_) {
    CHECK(shm_region_ != nullptr) << "LocalMemoryAllocator initialized with "
                                     "uninitialized LocalMemoryRegion";
}

template <typename T>
T* LocalMemoryAllocator<T>::allocate(const size_t n) const {
    // return AllocateAligned<MemoryAlignment::kPageAlign>(n);
    return AllocateAligned<MemoryAlignment::kNoAlign>(n);
}

template <typename T>
template <MemoryAlignment Alignment>
T* LocalMemoryAllocator<T>::AllocateAligned(const size_t n) const {
    CHECK(n != 0) << "zero length allocation";
    CHECK(n <= static_cast<size_t>(-1) / sizeof(T)) << "bad_array_new_length";

    DLOG(INFO) << "Local AllocateAligned<" << AlignmentToString<Alignment>() << ">(" << n << ") [" << TypeToString<T>()
               << "]";
    size_t requested = n * sizeof(T);
    auto ret = shm_region_->Allocate<Alignment>(requested);
    CHECK(ret != nullptr) << "AllocateAligned failed for allocating " << requested << " on region with seglength "
                          << shm_region_->GetLength() << " with data type " << TypeToString<T>();
    return (T*)ret;
}

template <typename T>
void LocalMemoryAllocator<T>::deallocate(T* const p, size_t n) const noexcept {
    DLOG(INFO) << "deallocate(" << p << ", " << n << ")";
    shm_region_->Deallocate(reinterpret_cast<uint8_t*>(p));
}

}  // namespace rackobj::common

#endif  // RACKOBJ_SHARED_MEMORY_DETAIL_ALLOCATOR_HPP
