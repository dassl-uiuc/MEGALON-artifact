#pragma once
#include <absl/synchronization/mutex.h>
#include <pthread.h>

#include "ankerl/unordered_dense.h"
#include "common/constants.h"
#include "core/blockid.h"
#include "core/cache_node.h"
#include "gtl/phmap.hpp"
#include "shared_memory/allocator.h"

namespace rackobj::common {

class GlobalCacheDirectory {
    using HashMapValue = GCDEntry;
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

    friend class GlobalCacheDirectoryHandle;

    using FineGrainedHashMap =
        gtl::parallel_flat_hash_map<BlockId, HashMapValue, BlockIdRobinMapHash, std::equal_to<BlockId>, KVPairAllocator,
                                    BUCKET_POW, absl::Mutex>;

public:
    explicit GlobalCacheDirectory(size_t num_entries, LocalMemoryByteAllocator allocator);

    ~GlobalCacheDirectory();

private:
    alignas(64) pthread_rwlock_t lock_;

    alignas(64) FineGrainedHashMap map_;
    void ReadLock() { pthread_rwlock_rdlock(&lock_); }

    void WriteLock() { pthread_rwlock_wrlock(&lock_); }

    void Unlock() { pthread_rwlock_unlock(&lock_); }
};

class GlobalCacheDirectoryHandle {
public:
    static std::unique_ptr<GlobalCacheDirectoryHandle> CreateOrMap(
        size_t num_entries, void* map_address, int numa_node, const uint8_t* base_addr,
        const std::shared_ptr<common::AllocatableLocalMemoryRegion>& sc_shm_region);

    GlobalCacheDirectoryHandle(GlobalCacheDirectory* gcd, std::shared_ptr<SharedMemoryRegion> region) noexcept
        : gcd_(gcd), region_(region) {}

    ~GlobalCacheDirectoryHandle();

    GlobalCacheDirectoryHandle(const GlobalCacheDirectoryHandle&) = delete;
    GlobalCacheDirectoryHandle& operator=(const GlobalCacheDirectoryHandle&) = delete;

    GlobalCacheDirectoryHandle(GlobalCacheDirectoryHandle&&) = default;
    GlobalCacheDirectoryHandle& operator=(GlobalCacheDirectoryHandle&&) = default;

    std::optional<GCDEntry> Get(const BlockId& block_id);

    std::optional<GCDEntry> GetAnchor(const BlockId& block_id);

    NrGcdDeleteError DeleteIfReadOnly(const BlockId& to_insert, int nid);

    std::optional<ssize_t> Delete(const BlockId& to_remove);

    bool DeleteLocal(const BlockId& to_remove, int nid);

    bool Insert(const BlockId& to_insert, size_t cn_index, int nid);

    NrGcdError CheckAndInsert(const BlockId& to_insert, size_t cn_index, int nid,
                              std::optional<size_t> wmeta_idx = std::nullopt);

    bool SwitchToReadOnly(const BlockId& to_modify);

    bool SwitchToRWShared(const BlockId& to_modify, size_t wmeta_idx);

    NrGcdError InvalidateSwitchToRWShared(const BlockId& to_modify, size_t wmeta_idx);

    bool CheckCoherence(const BlockId& to_check);
    bool CheckCoherence(const size_t key);

    void ResetCoherence(const BlockId& to_check);

    bool CheckCoherenceReset(const BlockId& to_check);

    bool CheckNotificationReset();

    void ReadLock() { pthread_rwlock_rdlock(&gcd_->lock_); }

    void WriteLock() { pthread_rwlock_wrlock(&gcd_->lock_); }

    void Unlock() { pthread_rwlock_unlock(&gcd_->lock_); }

    // unused
    bool Swap(const BlockId& to_remove, const BlockId& to_insert, GCDEntry cn);

    // TODO: implement GCD CheckInsertUpdate
    NrGcdError CheckInsertUpdate(const BlockId& to_insert, size_t cn_index, int new_nid, int old_nid) {
        (void)to_insert;
        (void)cn_index;
        (void)new_nid;
        (void)old_nid;
        LOG(FATAL) << "CheckInsertUpdate not implemented";
    };

private:
    GlobalCacheDirectory* gcd_;

    std::shared_ptr<SharedMemoryRegion> region_;
};

}  // namespace rackobj::common
