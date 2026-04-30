#ifndef RACKOBJ_SHARED_MEMORY_ALLOCATOR_H
#define RACKOBJ_SHARED_MEMORY_ALLOCATOR_H

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <memory>
#include <optional>
#include <string>

#include "region.h"

namespace rackobj::common {

template <typename T>
class SharedMemoryAllocator {
protected:
    template <typename U>
    friend class SharedMemoryAllocator;

public:
    typedef T value_type;

    SharedMemoryAllocator(std::shared_ptr<AllocatableSharedMemoryRegion> shm_region) noexcept;

    template <typename U>
    SharedMemoryAllocator(const SharedMemoryAllocator<U>& other) noexcept;

    template <typename U>
    bool operator==(const SharedMemoryAllocator<U>&) const noexcept {
        return true;
    }

    template <typename U>
    bool operator!=(const SharedMemoryAllocator<U>&) const noexcept {
        return false;
    }

    std::shared_ptr<AllocatableSharedMemoryRegion> GetSharedMemoryRegion() { return shm_region_; }

    T* allocate(const size_t n = 1) const;

    template <MemoryAlignment Alignment>
    T* AllocateAligned(const size_t n = 1) const;

    void deallocate(T* const p, size_t n) const noexcept;

private:
    std::shared_ptr<AllocatableSharedMemoryRegion> shm_region_;
};

typedef SharedMemoryAllocator<uint8_t> SharedMemoryByteAllocator;

template <typename T>
struct RebindSharedMemoryByteAllocator {
    typedef std::allocator_traits<SharedMemoryByteAllocator>::template rebind_alloc<T> type;
};

template <typename T>
using RebindSharedMemoryByteAllocatorT = typename RebindSharedMemoryByteAllocator<T>::type;

// local allocator
template <typename T>
class LocalMemoryAllocator {
protected:
    template <typename U>
    friend class LocalMemoryAllocator;

public:
    typedef T value_type;

    LocalMemoryAllocator(std::shared_ptr<AllocatableLocalMemoryRegion> shm_region) noexcept;

    template <typename U>
    LocalMemoryAllocator(const LocalMemoryAllocator<U>& other) noexcept;

    template <typename U>
    bool operator==(const LocalMemoryAllocator<U>&) const noexcept {
        return true;
    }

    template <typename U>
    bool operator!=(const LocalMemoryAllocator<U>&) const noexcept {
        return false;
    }

    std::shared_ptr<AllocatableLocalMemoryRegion> GetLocalMemoryRegion() { return shm_region_; }

    T* allocate(const size_t n = 1) const;

    template <MemoryAlignment Alignment>
    T* AllocateAligned(const size_t n = 1) const;

    void deallocate(T* const p, size_t n) const noexcept;

private:
    std::shared_ptr<AllocatableLocalMemoryRegion> shm_region_;
};

typedef LocalMemoryAllocator<uint8_t> LocalMemoryByteAllocator;

template <typename T>
struct RebindLocalMemoryByteAllocator {
    typedef std::allocator_traits<LocalMemoryByteAllocator>::template rebind_alloc<T> type;
};

template <typename T>
using RebindLocalMemoryByteAllocatorT = typename RebindLocalMemoryByteAllocator<T>::type;

}  // namespace rackobj::common

#include "allocator.hpp"

#endif  // RACKOBJ_SHARED_MEMORY_ALLOCATOR_H