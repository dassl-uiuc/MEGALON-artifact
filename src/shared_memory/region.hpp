#ifndef RACKOBJ_SHARED_MEMORY_DETAIL_REGION_HPP
#define RACKOBJ_SHARED_MEMORY_DETAIL_REGION_HPP
#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <numeric>
#include <ranges>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "common/not_implemented.h"
#include "shared_memory/region.h"

namespace rackobj::common {

inline SharedMemoryRegion::SharedMemoryRegion(void *map_addr, size_t length, bool require_free) noexcept
    : segment_length_(length), region_(static_cast<uint8_t *>(map_addr)), require_free_(require_free) {
    DLOG(INFO) << "construct shmregion with seg_length " << segment_length_;
}

inline SharedMemoryRegion::SharedMemoryRegion(const std::string &shm_name, void *map_addr) noexcept
    : segment_length_(0), region_(nullptr) {
    int fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
    PCHECK(fd != -1) << "Failed to open shared memory region at " << shm_name;

    struct stat stbuf;
    int rc = fstat(fd, &stbuf);
    PCHECK(rc != -1) << "Failed to fstat " << shm_name;
    segment_length_ = static_cast<size_t>(stbuf.st_size);

    int flags = MAP_SHARED;
    if (map_addr != nullptr) {
        flags |= MAP_FIXED_NOREPLACE;
    }
    void *mmap_result = mmap(map_addr, segment_length_, PROT_READ | PROT_WRITE, flags, fd, 0);
    PCHECK(mmap_result != MAP_FAILED) << "Failed to create memory mapping of " << shm_name << " at address " << map_addr
                                      << " with length " << segment_length_;
    close(fd);

    region_ = static_cast<uint8_t *>(mmap_result);

    flags = MADV_WILLNEED | MADV_POPULATE_WRITE;
    rc = madvise(mmap_result, segment_length_, flags);
    PCHECK(rc != -1) << "madvise(" << mmap_result << ", " << segment_length_ << ", MADV_WILLNEED) failed";
}

inline SharedMemoryRegion::~SharedMemoryRegion() {
    if (!require_free_) return;
    void *ptr = static_cast<void *>(region_);
    if (is_local_) {
        if (ptr) {
            DLOG(INFO) << "freeing region of length " << segment_length_ << " at " << ptr;
            free(ptr);
        }
    } else {
        DLOG(INFO) << "Unmapping region of length " << segment_length_ << " at " << ptr;
        if (segment_length_ > 0) {
            int rc = munmap(ptr, segment_length_);
            PCHECK(rc != -1) << "munmap(" << ptr << ", " << segment_length_ << ") failed";
        }
    }
}

inline bool SharedMemoryRegion::Exists(const std::string &shm_name) {
    int fd = shm_open(shm_name.c_str(), O_RDONLY, 0666);
    close(fd);

    return fd != -1;
}

inline AllocatableSharedMemoryRegion::AllocatableSharedMemoryRegion(const std::string &shm_name, void *map_address,
                                                                    bool exclusive) noexcept
    : SharedMemoryRegion(map_address),
      shm_name_(shm_name),
      fd_(shm_open(shm_name_.c_str(), O_CREAT | O_RDWR | (exclusive ? O_EXCL : 0), 0666)),
      exclusive_(exclusive) {
    PCHECK(fd_ != -1) << "Failed to open shared memory region";
    DLOG(INFO) << "Opening AllocatableSharedMemoryRegion at address " << (void *)region_;
}

inline AllocatableSharedMemoryRegion::~AllocatableSharedMemoryRegion() {
    int rc = close(fd_);
    PCHECK(rc != -1) << "Failed to close shared memory file";

    if (exclusive_) {
        rc = shm_unlink(shm_name_.c_str());
        PCHECK(rc != -1) << "Failed to unlink shared memory at " << shm_name_;
    }
}

template <MemoryAlignment Alignment>
inline uint8_t *AllocatableSharedMemoryRegion::Allocate(size_t nbytes) {
    size_t offset = 0;
    if constexpr (Alignment == MemoryAlignment::kCacheAlign) {
        offset = RoundUpToPowerOf64(segment_length_);
    } else if constexpr (Alignment == MemoryAlignment::kPageAlign) {
        offset = RoundUpToPowerOf4096(segment_length_);
    } else if constexpr (Alignment == MemoryAlignment::kNoAlign) {
        offset = segment_length_;
    } else {
        static_assert(rackobj::not_implemented_t<MemoryAlignment>::value, "Invalid alignment");
    }

    size_t new_length = offset + nbytes;
    if (new_length == 0) {
        LOG(FATAL) << "Cannot allocate 0 bytes";
    }

    LOG(INFO) << "Allocating " << nbytes << " (growing shared memory from " << segment_length_ << " to " << new_length
              << ")";

    int rc = ftruncate64(fd_, static_cast<loff_t>(new_length));
    PCHECK(rc != -1) << "ftruncate64 " << shm_name_ << " failed";

    if (segment_length_ > 0) {
        void *ptr = mremap(region_, segment_length_, new_length, 0);
        PCHECK(ptr != MAP_FAILED) << "mremap failed";
        CHECK(static_cast<uint8_t *>(ptr) == region_) << "mremap moved shared memory to different address";
    } else {
        int prot = PROT_READ | PROT_WRITE;
        int flags = MAP_SHARED;
        if (region_ != nullptr) {
            flags |= MAP_FIXED_NOREPLACE;
        }

        void *ptr = mmap(region_, new_length, prot, flags, fd_, 0);
        PCHECK(ptr != MAP_FAILED) << "mmap failed";
        if (region_ == nullptr) {
            region_ = static_cast<uint8_t *>(ptr);
        } else if (static_cast<uint8_t *>(ptr) != region_) {
            LOG(ERROR) << "mmap mapped shared memory to different address than "
                          "specified (expected="
                       << (void *)region_ << " vs actual=" << ptr << ")";
        }
    }

    // if (target_node_ != -1) {
    //     // TODO: investigate correctness
    //     // unsigned long nodemask = 1UL << target_node_;
    //     // if (mbind((void*) region_, new_length, MPOL_BIND, &nodemask, sizeof(nodemask)*8, MPOL_MF_STRICT) != 0) {
    //     //     LOG(ERROR) << "mbind failure";
    //     // }
    //     numa_tonode_memory((void*) region_, new_length, target_node_);
    //     // set_mempolicy(MPOL_BIND, &nodemask, sizeof(nodemask)*8);

    //     VerifyPagesPinnedToNode(target_node_);
    //     // while (!VerifyPagesPinnedToNode(target_node_)) {
    //     //     LOG(WARNING) << "remap";
    //     //     mremap(region_, new_length, new_length, 0);
    //     //     mbind((void*) region_, new_length, MPOL_BIND, &nodemask, sizeof(nodemask)*8, MPOL_MF_STRICT);
    //     // }
    //     // numa_tonode_memory((void*) region_, new_length, target_node_);
    //     // MovePagesToNode(target_node_);
    //     // set_mempolicy(MPOL_DEFAULT, &nodemask, sizeof(nodemask)*8);
    // }

    segment_length_ = new_length;
    return region_ + offset;
}

inline bool AllocatableSharedMemoryRegion::VerifyPagesPinnedToNode(int node) {
    CHECK(numa_available() != -1) << "libnuma not available";

    pid_t pid = getpid();
    int status = -1;
    int ret_code;
    uint8_t *page_ptr = nullptr;
    bool ret = true;

    for (size_t offset = 0; offset < segment_length_; offset += 4096) {
        page_ptr = region_ + offset;

        // Touch the page so we can pagefault and allocate a physical page
        // See: https://stackoverflow.com/a/2219839
        ((uint8_t volatile *)page_ptr)[0] = page_ptr[0];

        // See: https://stackoverflow.com/a/8015480
        ret_code = numa_move_pages(pid, 1, (void **)&page_ptr, NULL, &status, 0);
        PCHECK(ret_code != -1) << "numa_move_pages failed";
        if (ret_code == -1) {
            ret = false;
            return ret;
        }

        LOG_IF(WARNING, status != node) << "page at offset " << offset << " (virtual address " << (void *)page_ptr
                                        << ") on different numa node than specified (status=" << status << ")";
        if (status != node) {
            ret = false;
            return ret;
        }
    }

    return ret;
}

inline void AllocatableSharedMemoryRegion::MovePagesToNode(int node) {
    CHECK(numa_available() != -1) << "libnuma not available";

    size_t cnt = segment_length_ / 4096UL;
    if (segment_length_ % 4096UL > 0) {
        cnt += 1;
    }

    if (cnt == 0) {
        LOG(WARNING) << "MovePagesToNode invoked with 0 pages available";
        return;
    }

    DLOG(INFO) << "Moving region of length " << segment_length_ << " at " << (void *)region_ << " to NUMA node "
               << node;

    std::vector<int> nodes(cnt, node), status(cnt, -1);
    std::vector<size_t> offsets(cnt);
    std::vector<void *> pages(cnt);
    pid_t pid = getpid();

    std::iota(offsets.begin(), offsets.end(), 0);
    std::for_each(offsets.begin(), offsets.end(), [](size_t &i) { i *= 4096; });
    std::transform(offsets.begin(), offsets.end(), pages.begin(), [this](size_t &i) { return region_ + i; });

    int rc = numa_move_pages(pid, cnt, pages.data(), nodes.data(), status.data(), MPOL_MF_MOVE_ALL);
    PCHECK(rc != -1) << "numa_move_pages failed";
    for (size_t i = 0; i < cnt; ++i) {
        if (status[i] == node) DLOG(INFO) << "success for " << pages[i];
        CHECK(status[i] == node) << "page at offset " << offsets[i] << " (virtual address " << pages[i]
                                 << ") on different numa node than specified (status=" << status[i] << ")";

        // LOG_IF(WARNING, status[i] != node)
        //     << "page at offset " << offsets[i] << " (virtual address "
        //     << pages[i] << ") on different numa node than specified (status="
        //     << status[i] << ")";
    }
}

inline void AllocatableSharedMemoryRegion::MovePagesToNodeNew(int node) {
    CHECK(numa_available() != -1) << "libnuma not available";

    if (segment_length_ == 0) {
        LOG(WARNING) << "MovePagesToNode invoked with 0 pages available";
        return;
    }

    DLOG(INFO) << "Moving region of length " << segment_length_ << " at " << (void *)region_ << " to NUMA node "
               << node;

    numa_tonode_memory((void *)region_, segment_length_, node);
    VerifyPagesPinnedToNode(node);
}

inline void AllocatableSharedMemoryRegion::SetTargetNode(int node) { target_node_ = node; }

// local memory region
inline AllocatableLocalMemoryRegion::AllocatableLocalMemoryRegion(int node, size_t length, bool exclusive) noexcept
    : SharedMemoryRegion(nullptr, length) {
    is_local_ = true;
    target_node_ = node;
    exclusive_ = exclusive;
    CHECK(numa_available() != -1) << "libnuma not available";
    DLOG(INFO) << "init memkind region of size " << length << " on node " << node;

    size_t alignment = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    if (posix_memalign(&region_, alignment, length) != 0) {
        perror("posix_memalign fail");
        exit(1);
    }

    unsigned long nodemask = 1UL << target_node_;
    if (mbind(region_, length, MPOL_BIND, &nodemask, sizeof(nodemask), MPOL_MF_STRICT | MPOL_MF_MOVE_ALL) != 0) {
        perror("mbind fail");
        free(region_);
        exit(1);
    }

    int ret = memkind_create_fixed(region_, length, &kind_);
    CHECK(ret == 0) << "memkind_create_fixed failed";

    // fault the page
    size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    char *base = static_cast<char *>(region_);
    for (size_t offset = 0; offset < length; offset += page_size) {
        *(volatile char *)(base + offset) = 0;
    }

    LOG(INFO) << "memkind created with size " << length << " on node " << node;
}

inline AllocatableLocalMemoryRegion::~AllocatableLocalMemoryRegion() {
    memkind_destroy_kind(kind_);
    numa_free(region_, segment_length_);
}

template <MemoryAlignment Alignment>
inline uint8_t *AllocatableLocalMemoryRegion::Allocate(size_t nbytes) {
    if (nbytes == 0) {
        LOG(FATAL) << "Cannot allocate 0 bytes";
    }

    size_t alignment = 0;
    if constexpr (Alignment == MemoryAlignment::kCacheAlign) {
        alignment = 64;
    } else if constexpr (Alignment == MemoryAlignment::kPageAlign) {
        alignment = 4096;
    }

    DLOG(INFO) << "Allocating " << nbytes << " to node " << target_node_ << " with seglength " << segment_length_
               << " alignment " << alignment;

    void *region;
    if (alignment > 0) {
        int ret = memkind_posix_memalign(kind_, &region, alignment, nbytes);
        DLOG_IF(WARNING, ret != 0) << "memkind_posix_memalign failed " << ret;
    } else {
        region = memkind_malloc(kind_, nbytes);
        DLOG_IF(WARNING, region == NULL) << "memkind_malloc failed for allocating " << nbytes
                                         << " on region with seglength " << segment_length_;
    }

    return static_cast<uint8_t *>(region);
}

inline void AllocatableLocalMemoryRegion::Deallocate(uint8_t *ptr) { memkind_free(kind_, ptr); }

inline void AllocatableLocalMemoryRegion::SetTargetNode(int node) { target_node_ = node; }

}  // namespace rackobj::common

#endif  // RACKOBJ_SHARED_MEMORY_DETAIL_REGION_HPP