#pragma once
#include <absl/synchronization/mutex.h>
#include <pthread.h>

#include "ankerl/unordered_dense.h"
#include "common/constants.h"
#include "core/blockid.h"
#include "core/cache_node.h"
#include "core/cc_primitive.h"
#include "gtl/phmap.hpp"
#include "shared_memory/allocator.h"

namespace rackobj::common {

typedef struct LCDEntry_t {
    std::optional<size_t> cn_idx_;
    std::optional<size_t> wmeta_idx_;
    seqlock_t seqlock_;

    LCDEntry_t(size_t cn = 0) : cn_idx_(cn), wmeta_idx_(std::nullopt) { seqlock_clear(&seqlock_); }

    LCDEntry_t(const LCDEntry_t& other) : cn_idx_(other.cn_idx_), wmeta_idx_(other.wmeta_idx_) {
        seqlock_clear(&seqlock_);
    }
} LCDEntry;

class LocalCacheDirectory {
    using HashMapValue = LCDEntry;
    using HashMapElement = std::pair<BlockId, HashMapValue>;
    using KVPairAllocator = RebindLocalMemoryByteAllocatorT<HashMapElement>;
    struct BlockIdRobinMapHash {
#if KEY_SIZE == 24
        size_t operator()(const BlockId& b) const noexcept {
            static_assert(std::has_unique_object_representations_v<BlockId>);
            return ankerl::unordered_dense::detail::wyhash::hash(reinterpret_cast<const char*>(&b) + sizeof(uint64_t),
                                                                 sizeof(BlockId) - sizeof(uint64_t));
        }
#else
        size_t operator()(const BlockId& b) const noexcept {
            static_assert(std::has_unique_object_representations_v<BlockId>);
            return ankerl::unordered_dense::detail::wyhash::hash(reinterpret_cast<const char*>(&b), sizeof(uint64_t));
        }
#endif
    };

    friend class LocalCacheDirectoryHandle;

    using FineGrainedHashMap =
        gtl::parallel_flat_hash_map<BlockId, HashMapValue, BlockIdRobinMapHash, std::equal_to<BlockId>, KVPairAllocator,
                                    BUCKET_POW_LOCAL, absl::Mutex>;

public:
    explicit LocalCacheDirectory(size_t num_entries, LocalMemoryByteAllocator allocator);

    ~LocalCacheDirectory();

private:
    alignas(64) FineGrainedHashMap map_;
};

class LocalCacheDirectoryHandle {
public:
    static std::unique_ptr<LocalCacheDirectoryHandle> Create(
        size_t num_entries, void* map_address, int numa_node, const uint8_t* base_addr,
        const std::shared_ptr<common::AllocatableLocalMemoryRegion>& local_region);

    LocalCacheDirectoryHandle(LocalCacheDirectory* lcd, std::shared_ptr<SharedMemoryRegion> region) noexcept
        : lcd_(lcd), index_mutex_(std::make_unique<absl::Mutex>()), region_(region) {}

    ~LocalCacheDirectoryHandle();

    LocalCacheDirectoryHandle(const LocalCacheDirectoryHandle&) = delete;
    LocalCacheDirectoryHandle& operator=(const LocalCacheDirectoryHandle&) = delete;

    LocalCacheDirectoryHandle(LocalCacheDirectoryHandle&&) = default;
    LocalCacheDirectoryHandle& operator=(LocalCacheDirectoryHandle&&) = default;

    std::optional<std::reference_wrapper<LCDEntry>> Get(const BlockId& block_id);

    std::optional<ssize_t> Delete(const BlockId& to_remove);

    bool Insert(const BlockId& to_insert, size_t cn_index, std::optional<size_t> wmeta_idx = std::nullopt);

private:
    LocalCacheDirectory* lcd_;

    std::unique_ptr<absl::Mutex> index_mutex_;  // not used

    std::shared_ptr<SharedMemoryRegion> region_;
};

}  // namespace rackobj::common
