#ifndef RACKOBJ_SHARED_MEMORY_REGION_H
#define RACKOBJ_SHARED_MEMORY_REGION_H

#include <memkind.h>

#include <string>
#include <vector>

namespace rackobj::common {

static constexpr size_t RoundUpToPowerOf64(size_t val) { return ((val - 1UL) | 63UL) + 1UL; }

static constexpr size_t RoundUpToPowerOf4096(size_t val) { return ((val - 1UL) | 4095UL) + 1UL; }

class SharedMemoryRegion {
public:
    /**
     * Map a pre-existing shared memory region. This constructor assumes that
     * the region mapped by the shared memory file specified by `shm_name` has
     * already been fully allocated.
     */
    SharedMemoryRegion(const std::string& shm_name, void* map_address = nullptr) noexcept;

    /**
     * Create a shared memory region at an unspecified address with length 0
     * using the shared memory file specified by `shm_name`. This constructor
     * assumes that the shared memory region will be grown by a derived class.
     */
    explicit SharedMemoryRegion(void* map_addr, size_t length = 0, bool require_free = true) noexcept;

    virtual ~SharedMemoryRegion();

    SharedMemoryRegion(const SharedMemoryRegion&) = delete;
    SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;

    SharedMemoryRegion(SharedMemoryRegion&&) = default;
    SharedMemoryRegion& operator=(SharedMemoryRegion&&) = default;

    void* GetRegion() { return region_; }

    size_t GetLength() const { return segment_length_; }

    static bool Exists(const std::string& shm_name);

protected:
    // The length of the mmap'ed shared memory segment
    size_t segment_length_;

    // The base virtual address of the shared memory segment in the userspace
    uint8_t* region_;

    bool is_local_ = false;
    bool require_free_ = true;
};

enum class MemoryAlignment { kNoAlign, kCacheAlign, kPageAlign };

class AllocatableSharedMemoryRegion : public SharedMemoryRegion {
public:
    AllocatableSharedMemoryRegion(const std::string& shm_name, void* map_address = nullptr,
                                  bool exclusive = true) noexcept;

    ~AllocatableSharedMemoryRegion();

    AllocatableSharedMemoryRegion(const AllocatableSharedMemoryRegion&) = delete;
    AllocatableSharedMemoryRegion& operator=(const AllocatableSharedMemoryRegion&) = delete;
    AllocatableSharedMemoryRegion(AllocatableSharedMemoryRegion&&) = default;
    AllocatableSharedMemoryRegion& operator=(AllocatableSharedMemoryRegion&&) = default;

    template <MemoryAlignment Alignment>
    uint8_t* Allocate(size_t nbytes);

    bool VerifyPagesPinnedToNode(int node);

    void MovePagesToNode(int node);

    void MovePagesToNodeNew(int node);

    void SetTargetNode(int node);

    const std::string& GetName() const { return shm_name_; }

private:
    std::string shm_name_;

    int fd_;

    int target_node_ = -1;

    // This determines whether or not to remove the shared memory segment upon
    // this object's destruction.
    bool exclusive_;
};

class AllocatableLocalMemoryRegion : public SharedMemoryRegion {
public:
    AllocatableLocalMemoryRegion(int node, size_t length, bool exclusive = true) noexcept;

    ~AllocatableLocalMemoryRegion();

    AllocatableLocalMemoryRegion(const AllocatableLocalMemoryRegion&) = delete;
    AllocatableLocalMemoryRegion& operator=(const AllocatableLocalMemoryRegion&) = delete;
    AllocatableLocalMemoryRegion(AllocatableLocalMemoryRegion&&) = default;
    AllocatableLocalMemoryRegion& operator=(AllocatableLocalMemoryRegion&&) = default;

    template <MemoryAlignment Alignment>
    uint8_t* Allocate(size_t nbytes);

    void Deallocate(uint8_t* ptr);

    void SetTargetNode(int node);

    int GetTargetNode() { return target_node_; }

private:
    int target_node_ = -1;

    // This determines whether or not to remove the shared memory segment upon
    // this object's destruction.
    bool exclusive_;

    // std::vector<void*> allocated_regions_;
    // std::vector<size_t> region_seglen_;

    void* region_;
    memkind_t kind_;
};

}  // namespace rackobj::common

#include "region.hpp"

#endif  // RACKOBJ_SHARED_MEMORY_REGION_H