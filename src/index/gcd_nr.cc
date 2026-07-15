#include "gcd_nr.h"

#include <array>
#include <limits>
#include <random>

#include "nr_hashmap.h"

namespace rackobj::common {
using std::construct_at;
using std::make_shared;
using std::make_unique;
using std::unique_ptr;

static inline uint64_t rdtsc_ns() {
    uint64_t tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc));
    return tsc / CPU_FREQ_GHZ;
}

static inline void busy_sleep_ns(uint64_t ns) {
    uint64_t end = rdtsc_ns() + ns;
    while (rdtsc_ns() < end) __asm__ volatile("pause" ::: "memory");
}

GlobalCacheDirectoryNr::GlobalCacheDirectoryNr(size_t num_entries, size_t num_replicas, const uint8_t* base_addr) {
    DLOG(INFO) << "Constructing GCD";
    nrht_wrapper =
        NrFfi::create_node_replicated_numa(num_entries, num_replicas, base_addr, SLOT_SIZE, NUMA_MEM, NUM_NUMA);

    memset(register_count, 0, sizeof(register_count));

    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(&lock_, &attr);
    pthread_rwlockattr_destroy(&attr);
}

GlobalCacheDirectoryNr::~GlobalCacheDirectoryNr() {
    LOG(INFO) << "Destroying GCD";
    pthread_rwlock_destroy(&lock_);
    free_node_replicated(nrht_wrapper);
}

unique_ptr<GlobalCacheDirectoryHandleNr> GlobalCacheDirectoryHandleNr::CreateOrMap(
    size_t num_entries, void* map_address, int numa_node, const uint8_t* base_addr,
    const std::shared_ptr<common::AllocatableLocalMemoryRegion>& sc_shm_region) {
    (void)map_address;
    (void)numa_node;
    (void)sc_shm_region;
    // currently, we do not allocate on shared memory region
    GlobalCacheDirectoryNr* gcd = new GlobalCacheDirectoryNr(num_entries, LOGICAL_NODE_NUM, base_addr);

    return make_unique<GlobalCacheDirectoryHandleNr>(gcd);
}

GlobalCacheDirectoryHandleNr::~GlobalCacheDirectoryHandleNr() {}

// assumes round robin
const NrFfi::NrMeta* GlobalCacheDirectoryHandleNr::GetNrMetaTid(int rid, bool lock) {
    (void)lock;
    const NrFfi::NrMeta* nr_meta_per_thread = nullptr;
    std::thread::id tid = std::this_thread::get_id();
    uint32_t numa_node = UINT32_MAX;
    int rc = getcpu(nullptr, &numa_node);
    if (rc == -1 || numa_node == NUMA_MEM) {
        LOG(ERROR) << "getcpu() failed || numa node is " << NUMA_MEM;
        return nullptr;
    }

    // Check that the passed rid is actually on the physical NUMA node the current thread is running on
    int rid_numa_node = rackobj::RidToNumaNode(rid);
    if (rid_numa_node != static_cast<int>(numa_node)) {
        LOG(ERROR) << "rid " << rid << " maps to NUMA node " << rid_numa_node << " but current thread is on NUMA node "
                   << numa_node;
        return nullptr;
    }

    gcd_->WriteLock();
    nr_meta_per_thread = register_node_replicated(gcd_->nrht_wrapper, static_cast<size_t>(rid));
    gcd_->register_count[rid]++;
    gcd_->Unlock();
    LOG(INFO) << "register NR for tid " << tid << " on replica " << rid;

    return nr_meta_per_thread;
}

bool GlobalCacheDirectoryHandleNr::UnRegisterThread(const NrFfi::NrMeta* nr_meta, bool lock) {
    (void)lock;
    if (nr_meta) {
        unregister_node_replicated(nr_meta);
        return true;
    }

    return false;
}

std::optional<GCDEntry> GlobalCacheDirectoryHandleNr::Get(const BlockId& block_id, const NrFfi::NrMeta* nr_meta) {
    NrFfi::GCDEntry nr_entry = NrFfi::Get(nr_meta, block_id);
    GCDEntry entry;
    entry.wmeta_.lock_ = nr_entry.wmeta_.lock_;
    entry.wmeta_.seqcount_ = static_cast<size_t>(nr_entry.wmeta_.seqcount_);
    for (int i = 0; i < LOGICAL_NODE_NUM + 1; i++) {
        NrFfi::CNStatus_t& nr_status = nr_entry.cn_array_[i];
        CNStatus_t& status = entry.cn_array_[i];
        if (nr_status.cn_idx_ != -1)
            status.cn_idx_ = nr_status.cn_idx_;
        else
            status.cn_idx_ = std::nullopt;
        status.invalidate_ = nr_status.invalidate_;
    }
    if (nr_entry.wmeta_idx_ != -1)
        entry.wmeta_idx_ = nr_entry.wmeta_idx_;
    else
        entry.wmeta_idx_ = std::nullopt;
    return entry;
}

std::optional<GCDEntry> GlobalCacheDirectoryHandleNr::GetAnchor(const BlockId& block_id, const NrFfi::NrMeta* nr_meta) {
    NrFfi::GCDEntry nr_entry = NrFfi::GetAnchor(nr_meta, block_id);
    GCDEntry entry;
    entry.wmeta_.lock_ = nr_entry.wmeta_.lock_;
    entry.wmeta_.seqcount_ = static_cast<size_t>(nr_entry.wmeta_.seqcount_);
    for (int i = 0; i < LOGICAL_NODE_NUM + 1; i++) {
        NrFfi::CNStatus_t& nr_status = nr_entry.cn_array_[i];
        CNStatus_t& status = entry.cn_array_[i];
        if (nr_status.cn_idx_ != -1)
            status.cn_idx_ = nr_status.cn_idx_;
        else
            status.cn_idx_ = std::nullopt;
        status.invalidate_ = nr_status.invalidate_;
    }
    if (nr_entry.wmeta_idx_ != -1)
        entry.wmeta_idx_ = nr_entry.wmeta_idx_;
    else
        entry.wmeta_idx_ = std::nullopt;
    return entry;
}

/**
 * remove the gcd entry
 *   old_wmeta_index (>=0): remove succeeds
 *   -1/-2: remove fails: -1) wmeta does not exist; -2) entry does not exist
 */
std::optional<ssize_t> GlobalCacheDirectoryHandleNr::Delete(const BlockId& to_remove, const NrFfi::NrMeta* nr_meta) {
    ssize_t old_wmeta_index = NrFfi::Delete(nr_meta, to_remove);
    if (old_wmeta_index < -1) {
        // entry does not exist
        return std::nullopt;
    }
    return static_cast<ssize_t>(old_wmeta_index);
}

/**
 * remove the gcd entry specified by nid
 * check wmeta if the target nid is cxl (not allow evict when in RW mode)
 *   true: remove succeeds
 *   false: remove fails: entry does not exist
 */
bool GlobalCacheDirectoryHandleNr::DeleteLocal(const BlockId& to_remove, int nid, const NrFfi::NrMeta* nr_meta) {
    return NrFfi::DeleteArray(nr_meta, to_remove, static_cast<uint64_t>(nid));
}

/**
 * remove the gcd entry specified by nid
 * check wmeta if the target nid is cxl (not allow evict when in RW mode)
 *   0: remove succeeds
 *   1/2/3: remove fails: 1) wmeta valid; 2) entry does not exist; 3) the replica on idx is already deleted
 */
NrGcdDeleteError GlobalCacheDirectoryHandleNr::DeleteIfReadOnly(const BlockId& to_remove, int nid,
                                                                const NrFfi::NrMeta* nr_meta) {
    return static_cast<NrGcdDeleteError>(NrFfi::DeleteIfArray(nr_meta, to_remove, static_cast<uint64_t>(nid)));
}

bool GlobalCacheDirectoryHandleNr::Insert(const BlockId& to_insert, size_t cn_index, int nid,
                                          const NrFfi::NrMeta* nr_meta) {
    NrFfi::PutArray(nr_meta, to_insert, static_cast<ssize_t>(cn_index), static_cast<uint64_t>(nid));

    return true;
}

/**
 return value:
    0: no error
    1: slot & wmeta update unsuccessful
    2: only slot update unsuccessful
 */
NrGcdError GlobalCacheDirectoryHandleNr::CheckAndInsert(const BlockId& to_insert, size_t cn_index, int nid,
                                                        const NrFfi::NrMeta* nr_meta, std::optional<size_t> wmeta_idx) {
    return static_cast<NrGcdError>(
        NrFfi::CheckPutArray(nr_meta, to_insert, static_cast<ssize_t>(cn_index), static_cast<uint64_t>(nid),
                             wmeta_idx.has_value() ? static_cast<ssize_t>(wmeta_idx.value()) : -1));
}

std::optional<size_t> GlobalCacheDirectoryHandleNr::SwitchToReadOnly(const BlockId& to_modify,
                                                                     const NrFfi::NrMeta* nr_meta) {
    ssize_t result = NrFfi::CheckSwitchWmetaArray(nr_meta, to_modify, -1);
    if (result < 0) return std::nullopt;
    return static_cast<size_t>(result);
}

bool GlobalCacheDirectoryHandleNr::SwitchToRWShared(const BlockId& to_modify, size_t wmeta_idx,
                                                    const NrFfi::NrMeta* nr_meta) {
    ssize_t result = NrFfi::CheckSwitchWmetaArray(nr_meta, to_modify, static_cast<ssize_t>(wmeta_idx));
    if (result < 0) return false;
    return true;
}

NrGcdError GlobalCacheDirectoryHandleNr::InvalidateSwitchToRWShared(const BlockId& to_modify, size_t wmeta_idx,
                                                                    const NrFfi::NrMeta* nr_meta) {
    // only keep cxl replica
    auto rc = NrFfi::InvalidateExceptArrayUpdateWmeta(nr_meta, to_modify, 0, static_cast<ssize_t>(wmeta_idx));
    switch (rc) {
        case 1:
            return NrGcdError::GCD_ENTRY_NOEXIST;
        case 2:
            return NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED;
        default:
    }
    return NrGcdError::GCD_NO_ERROR;
}

bool GlobalCacheDirectoryHandleNr::MoveIndexToLocal(const BlockId& to_modify, void* cn, int nid,
                                                    const NrFfi::NrMeta* nr_meta) {
    auto result = NrFfi::CheckMoveLocalArray(nr_meta, to_modify, reinterpret_cast<ssize_t>(cn), nid + 1);
    if (result == 0) return true;
    return false;
}

bool GlobalCacheDirectoryHandleNr::Swap(const BlockId& to_remove, const BlockId& to_insert, CacheNode* cn, int new_nid,
                                        int old_nid, const NrFfi::NrMeta* nr_meta) {
    return NrFfi::CheckSwapArray(nr_meta, to_remove, to_insert, cn, static_cast<uint64_t>(new_nid),
                                 static_cast<uint64_t>(old_nid));
}

NrGcdError GlobalCacheDirectoryHandleNr::CheckInsertUpdate(const BlockId& to_insert, size_t cn_index, int new_nid,
                                                           int old_nid, const NrFfi::NrMeta* nr_meta) {
    return static_cast<NrGcdError>(NrFfi::CheckPutSwapArray(nr_meta, to_insert, static_cast<ssize_t>(cn_index),
                                                            static_cast<uint64_t>(new_nid),
                                                            static_cast<uint64_t>(old_nid)));
}

bool GlobalCacheDirectoryHandleNr::CheckCoherence(const BlockId& to_check, const NrFfi::NrMeta* nr_meta) {
    return NrFfi::CheckCoherence(nr_meta, to_check);
}

bool GlobalCacheDirectoryHandleNr::CheckCoherence(const size_t key, const NrFfi::NrMeta* nr_meta) {
    return NrFfi::CheckCoherenceSlot(nr_meta, key);
}

void GlobalCacheDirectoryHandleNr::ResetCoherence(const BlockId& to_check, const NrFfi::NrMeta* nr_meta) {
    NrFfi::ResetCoherence(nr_meta, to_check);
}

bool GlobalCacheDirectoryHandleNr::CheckCoherenceReset(const BlockId& to_check, const NrFfi::NrMeta* nr_meta) {
    return NrFfi::CheckCoherenceReset(nr_meta, to_check);
}

bool GlobalCacheDirectoryHandleNr::CheckNotificationReset(const NrFfi::NrMeta* nr_meta) {
    return NrFfi::CheckNotificationReset(nr_meta);
}

void GlobalCacheDirectoryHandleNr::SeqCountWrapAround(const BlockId& key, const NrFfi::NrMeta* nr_meta) {
    return NrFfi::DummyEvent(nr_meta, key);
}

static GCDEntry convert_nr_entry(const NrFfi::GCDEntry& nr_entry) {
    GCDEntry entry;
    entry.wmeta_.lock_ = nr_entry.wmeta_.lock_;
    entry.wmeta_.seqcount_ = static_cast<size_t>(nr_entry.wmeta_.seqcount_);
    for (int i = 0; i < LOGICAL_NODE_NUM + 1; i++) {
        const NrFfi::CNStatus_t& nr_status = nr_entry.cn_array_[i];
        CNStatus_t& status = entry.cn_array_[i];
        status.cn_idx_ = (nr_status.cn_idx_ != -1) ? std::optional<size_t>(nr_status.cn_idx_) : std::nullopt;
        status.invalidate_ = nr_status.invalidate_;
    }
    entry.wmeta_idx_ = (nr_entry.wmeta_idx_ != -1) ? std::optional<size_t>(nr_entry.wmeta_idx_) : std::nullopt;
    return entry;
}

std::optional<GCDEntry> GlobalCacheDirectoryHandleNr::GetWithLock(const BlockId& block_id,
                                                                  const NrFfi::NrMeta* nr_meta) {
    thread_local std::mt19937 rng{std::random_device{}()};
    uint64_t backoff_base = BACKOFF_BASE_NS;

    for (;;) {
        NrFfi::TrySeqLockResult res = NrFfi::TrySeqLock(nr_meta, block_id);
        if (res.status_ == -1) return std::nullopt;  // no entry
        if (res.status_ == 1) return convert_nr_entry(res.entry_);
        // contended: exponential backoff with jitter
        uint64_t backoff_range = std::min(backoff_base * 2, static_cast<uint64_t>(BACKOFF_MAX_NS));
        std::uniform_int_distribution<uint64_t> dist(0, backoff_range);
        busy_sleep_ns(dist(rng));
        backoff_base = backoff_range;
    }
}

ssize_t GlobalCacheDirectoryHandleNr::ReleaseSeqLock(const BlockId& block_id, const NrFfi::NrMeta* nr_meta) {
    return NrFfi::ReleaseSeqLock(nr_meta, block_id);
}

size_t GlobalCacheDirectoryHandleNr::ReadSeqStart(const BlockId& block_id, const NrFfi::NrMeta* nr_meta) {
    ssize_t seq;
    do {
        seq = NrFfi::GetSeqCount(nr_meta, block_id);
        if (seq == -1) return 0;            // no entry; caller should detect miss
        __asm__ volatile("" ::: "memory");  // compiler barrier
    } while (seq & 1);                      // spin while odd (writer active)
    return static_cast<size_t>(seq);
}

bool GlobalCacheDirectoryHandleNr::ReadSeqRetry(const BlockId& block_id, const NrFfi::NrMeta* nr_meta,
                                                size_t saved_seq) {
    ssize_t cur = NrFfi::GetSeqCount(nr_meta, block_id);
    if (cur == -1) return true;  // entry gone: treat as retry
    return static_cast<size_t>(cur) != saved_seq;
}

}  // namespace rackobj::common
