#pragma once
#include <pthread.h>

#include <thread>

#include "absl/log/log.h"
#include "common/constants.h"
#include "core/blockid.h"
#include "core/cache_node.h"
#include "nr_hashmap.h"
#include "shared_memory/region.h"

namespace rackobj::common {

class GlobalCacheDirectoryNr {
    friend class GlobalCacheDirectoryHandleNr;

public:
    explicit GlobalCacheDirectoryNr(size_t num_entries, size_t num_replicas, const uint8_t* base_addr = nullptr);

    ~GlobalCacheDirectoryNr();

private:
    alignas(64) pthread_rwlock_t lock_;
    alignas(64) NrFfi::NrRapper* nrht_wrapper;
    alignas(64) std::unordered_map<std::thread::id, NrFfi::NrMeta*> nr_thread_map;  // bookkeep thread register for nr
    alignas(64) int register_count[LOGICAL_NODE_NUM];  // bookkeep nr replica id registered thread count

    void ReadLock() { pthread_rwlock_rdlock(&lock_); }

    void WriteLock() { pthread_rwlock_wrlock(&lock_); }

    void Unlock() { pthread_rwlock_unlock(&lock_); }
};

class GlobalCacheDirectoryHandleNr {
public:
    static std::unique_ptr<GlobalCacheDirectoryHandleNr> CreateOrMap(
        size_t num_entries, void* map_address, int numa_node, const uint8_t* base_addr,
        const std::shared_ptr<common::AllocatableLocalMemoryRegion>& sc_shm_region);

    GlobalCacheDirectoryHandleNr(GlobalCacheDirectoryNr* gcd) noexcept : gcd_(gcd) {}

    ~GlobalCacheDirectoryHandleNr();

    GlobalCacheDirectoryHandleNr(const GlobalCacheDirectoryHandleNr&) = delete;
    GlobalCacheDirectoryHandleNr& operator=(const GlobalCacheDirectoryHandleNr&) = delete;

    GlobalCacheDirectoryHandleNr(GlobalCacheDirectoryHandleNr&&) = default;
    GlobalCacheDirectoryHandleNr& operator=(GlobalCacheDirectoryHandleNr&&) = default;

    std::optional<GCDEntry> Get(const BlockId& block_id, const NrFfi::NrMeta* nr_meta);

    std::optional<GCDEntry> GetAnchor(const BlockId& block_id, const NrFfi::NrMeta* nr_meta);

    /**
     * Acquire the per-entry seqlock via NR with exponential backoff.
     * Returns the GCDEntry with lock held, or nullopt if no entry.
     */
    std::optional<GCDEntry> GetWithLock(const BlockId& block_id, const NrFfi::NrMeta* nr_meta);

    /**
     * Release the per-entry seqlock.
     * Returns new seqcount, or -1 if no entry.
     */
    ssize_t ReleaseSeqLock(const BlockId& block_id, const NrFfi::NrMeta* nr_meta);

    /**
     * Spin until the seqcount is even (no active writer), return the saved seq.
     */
    size_t ReadSeqStart(const BlockId& block_id, const NrFfi::NrMeta* nr_meta);

    /**
     * Return true if the seqcount has changed since saved_seq (read must retry).
     */
    bool ReadSeqRetry(const BlockId& block_id, const NrFfi::NrMeta* nr_meta, size_t saved_seq);

    std::optional<ssize_t> Delete(const BlockId& to_remove, const NrFfi::NrMeta* nr_meta);

    bool DeleteLocal(const BlockId& to_remove, int nid, const NrFfi::NrMeta* nr_meta);

    NrGcdDeleteError DeleteIfReadOnly(const BlockId& to_remove, int nid, const NrFfi::NrMeta* nr_meta);

    bool Insert(const BlockId& to_insert, size_t cn_index, int nid, const NrFfi::NrMeta* nr_meta);

    NrGcdError CheckAndInsert(const BlockId& to_insert, size_t cn_index, int nid, const NrFfi::NrMeta* nr_meta,
                              std::optional<size_t> wmeta_idx = std::nullopt);

    bool Swap(const BlockId& to_remove, const BlockId& to_insert, CacheNode* cn, int new_nid, int old_nid,
              const NrFfi::NrMeta* nr_meta);

    NrGcdError CheckInsertUpdate(const BlockId& to_insert, size_t cn_index, int new_nid, int old_nid,
                                 const NrFfi::NrMeta* nr_meta);

    std::optional<size_t> SwitchToReadOnly(const BlockId& to_modify, const NrFfi::NrMeta* nr_meta);

    bool SwitchToRWShared(const BlockId& to_modify, size_t wmeta_idx, const NrFfi::NrMeta* nr_meta);

    NrGcdError InvalidateSwitchToRWShared(const BlockId& to_modify, size_t wmeta_idx, const NrFfi::NrMeta* nr_meta);

    bool MoveToLocal(const BlockId& to_modify, void* cn, int nid, const NrFfi::NrMeta* nr_meta);

    bool CheckCoherence(const BlockId& to_check, const NrFfi::NrMeta* nr_meta);

    bool MoveIndexToLocal(const BlockId& to_modify, void* cn, int nid, const NrFfi::NrMeta* nr_meta);

    bool CheckCoherence(const size_t key, const NrFfi::NrMeta* nr_meta);

    void ResetCoherence(const BlockId& to_check, const NrFfi::NrMeta* nr_meta);

    bool CheckCoherenceReset(const BlockId& to_check, const NrFfi::NrMeta* nr_meta);

    bool CheckNotificationReset(const NrFfi::NrMeta* nr_meta);

    void SeqCountWrapAround(const BlockId& key, const NrFfi::NrMeta* nr_meta);

    bool UnRegisterThread(const NrFfi::NrMeta* nr_meta, bool lock = true);

    void ReadLock() { pthread_rwlock_rdlock(&gcd_->lock_); }

    void WriteLock() { pthread_rwlock_wrlock(&gcd_->lock_); }

    void Unlock() { pthread_rwlock_unlock(&gcd_->lock_); }

    const NrFfi::NrMeta* GetNrMetaTid(int rid, bool lock = false);

private:
    GlobalCacheDirectoryNr* gcd_;
};

}  // namespace rackobj::common
