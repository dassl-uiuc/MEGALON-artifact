#include "gcd.h"

namespace rackobj::common {

#define NUMA_MEM_IDX 0

using std::construct_at;
using std::make_shared;
using std::make_unique;
using std::unique_ptr;

inline GCDEntry init_empty_gcd_entry() {
    GCDEntry entry;
    entry.wmeta_idx_ = std::nullopt;
    for (int i = 0; i < LOGICAL_NODE_NUM + 1; i++) {
        entry.cn_array_[i].cn_idx_ = std::nullopt;
        entry.cn_array_[i].invalidate_ = false;
    }

    return entry;
}

inline GCDEntry init_empty_gcd_entry_with_wmeta_idx(size_t wmeta_idx) {
    GCDEntry entry;
    entry.wmeta_idx_ = wmeta_idx;
    for (int i = 0; i < LOGICAL_NODE_NUM + 1; i++) {
        entry.cn_array_[i].cn_idx_ = std::nullopt;
        entry.cn_array_[i].invalidate_ = false;
    }
    return entry;
}

inline GCDEntry init_gcd_entry(size_t cn, int idx) {
    GCDEntry entry;
    for (int i = 0; i < LOGICAL_NODE_NUM + 1; i++) {
        if (i == idx) {
            entry.cn_array_[i].cn_idx_ = cn;
        } else {
            entry.cn_array_[i].cn_idx_ = std::nullopt;
        }
        entry.cn_array_[i].invalidate_ = false;
    }
    entry.wmeta_idx_ = std::nullopt;
    return entry;
}

GlobalCacheDirectory::GlobalCacheDirectory(size_t num_entries, LocalMemoryByteAllocator allocator)
    : map_(KVPairAllocator(allocator)) {
    LOG(INFO) << "Constructing GCD with num entries " << num_entries;

    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(&lock_, &attr);
    pthread_rwlockattr_destroy(&attr);
}

GlobalCacheDirectory::~GlobalCacheDirectory() {
    LOG(INFO) << "Destroying GCD";
    // pthread_rwlock_destroy(&lock_);
}

unique_ptr<GlobalCacheDirectoryHandle> GlobalCacheDirectoryHandle::CreateOrMap(
    size_t num_entries, void* map_address, int numa_node, const uint8_t* base_addr,
    const std::shared_ptr<common::AllocatableLocalMemoryRegion>& sc_shm_region) {
    (void)map_address;
    (void)base_addr;
    (void)numa_node;
    LocalMemoryAllocator<GlobalCacheDirectory> alloc(sc_shm_region);

    auto* gcd = alloc.allocate();
    construct_at(gcd, num_entries, LocalMemoryByteAllocator(alloc));

    return make_unique<GlobalCacheDirectoryHandle>(gcd, sc_shm_region);
}

GlobalCacheDirectoryHandle::~GlobalCacheDirectoryHandle() {}

std::optional<GCDEntry> GlobalCacheDirectoryHandle::Get(const BlockId& block_id) {
    GCDEntry entry = init_empty_gcd_entry();
    gcd_->map_.if_contains(block_id, [&entry](const std::pair<const BlockId, GCDEntry>& p) { entry = p.second; });
    return entry;
}

std::optional<GCDEntry> GlobalCacheDirectoryHandle::GetAnchor(const BlockId& block_id) {
    GCDEntry entry = init_empty_gcd_entry();
    gcd_->map_.if_contains(block_id, [&entry](const std::pair<const BlockId, GCDEntry>& p) { entry = p.second; });
    return entry;
}

std::optional<ssize_t> GlobalCacheDirectoryHandle::Delete(const BlockId& to_remove) {
    std::optional<ssize_t> old_wmeta_index = std::nullopt;
    gcd_->map_.erase_if(to_remove, [&](std::pair<const BlockId, GCDEntry>& row) {
        auto& entry = row.second;
        old_wmeta_index = entry.wmeta_idx_;
        return true;
    });

    return old_wmeta_index;
}

bool GlobalCacheDirectoryHandle::DeleteLocal(const BlockId& to_remove, int nid) {
    bool entry_exists = false;
    return gcd_->map_.erase_if(to_remove, [&](std::pair<const BlockId, GCDEntry>& row) {
        auto& entry = row.second;
        entry_exists = true;
        entry.cn_array_[nid].cn_idx_ = std::nullopt;
        entry.cn_array_[nid].invalidate_ = false;
        return std::all_of(std::begin(entry.cn_array_), std::end(entry.cn_array_),
                           [](const CNStatus& status) { return !status.cn_idx_.has_value(); });
    });
    return entry_exists;
}

/*
enum NrGcdDeleteError {
    NR_GCD_DELETE_SUCCESS = 0,
    NR_GCD_DELETE_WMETA_VALID,    // wmeta valid
    NR_GCD_DELETE_ENTRY_NOEXIST,  // entry does not exist
    NR_GCD_DELETE_ALRDY_DELETED,  // the replica on idx is already deleted (not used in current implementation)
};
*/
NrGcdDeleteError GlobalCacheDirectoryHandle::DeleteIfReadOnly(const BlockId& to_remove, int nid) {
    NrGcdDeleteError return_status = NrGcdDeleteError::NR_GCD_DELETE_ENTRY_NOEXIST;
    gcd_->map_.erase_if(to_remove, [&](std::pair<const BlockId, GCDEntry>& row) {
        auto& entry = row.second;
        if (nid == NUMA_MEM_IDX) {
            if (entry.wmeta_idx_.has_value()) {
                return_status = NrGcdDeleteError::NR_GCD_DELETE_WMETA_VALID;
                return false;
            }
            if (!entry.cn_array_[nid].cn_idx_.has_value() && !entry.cn_array_[nid].invalidate_) {
                return_status = NrGcdDeleteError::NR_GCD_DELETE_ALRDY_DELETED;
                return false;
            }
        } else {
            if (!entry.cn_array_[nid].cn_idx_.has_value() && !entry.cn_array_[nid].invalidate_) {
                return_status = NrGcdDeleteError::NR_GCD_DELETE_ALRDY_DELETED;
                return false;
            }
            // if (entry.cn_array_[nid].invalidate_ != true && entry.wmeta_idx_.has_value()) {
            //     throw std::runtime_error("BUG: local replica still valid when page in RW shared");
            // }
        }

        entry.cn_array_[nid].cn_idx_ = std::nullopt;
        entry.cn_array_[nid].invalidate_ = false;
        return_status = NrGcdDeleteError::NR_GCD_DELETE_SUCCESS;
        return std::all_of(std::begin(entry.cn_array_), std::end(entry.cn_array_),
                           [](const CNStatus& status) { return !status.cn_idx_.has_value(); });
    });
    return return_status;
}

bool GlobalCacheDirectoryHandle::Insert(const BlockId& to_insert, size_t cn_index, int nid) {
    gcd_->map_.lazy_emplace_l(
        to_insert,
        [&](std::pair<const BlockId, GCDEntry>& row) {
            auto& entry = row.second;
            entry.cn_array_[nid].cn_idx_ = cn_index;
            entry.cn_array_[nid].invalidate_ = false;
            entry.wmeta_idx_ = std::nullopt;
        },
        [&](const decltype(gcd_->map_)::constructor& ctor) {
            GCDEntry entry = init_gcd_entry(cn_index, nid);
            entry.wmeta_idx_ = std::nullopt;
            ctor(to_insert, entry);
        });
    return true;
}

NrGcdError GlobalCacheDirectoryHandle::CheckAndInsert(const BlockId& to_insert, size_t cn_index, int nid,
                                                      std::optional<size_t> wmeta_idx) {
    NrGcdError return_status = NrGcdError::GCD_NO_ERROR;
    gcd_->map_.lazy_emplace_l(
        to_insert,
        [&](std::pair<const BlockId, GCDEntry>& row) {
            auto& entry = row.second;
            if (!entry.cn_array_[nid].cn_idx_.has_value()) {
                if (entry.cn_array_[nid].invalidate_) {
                    throw std::runtime_error("BUG: Attempted to put into an invalidated entry at idx " +
                                             std::to_string(nid));
                }
                if (entry.wmeta_idx_.has_value() && nid != NUMA_MEM_IDX) {
                    throw std::runtime_error("BUG: Attempted to add local replica while wmeta valid");
                }
                entry.cn_array_[nid].cn_idx_ = cn_index;
                entry.cn_array_[nid].invalidate_ = false;
                if (nid == NUMA_MEM_IDX) {
                    entry.wmeta_idx_ = wmeta_idx;
                }
            } else if (nid == NUMA_MEM_IDX) {
                if (!wmeta_idx.has_value()) {
                    entry.wmeta_idx_ = wmeta_idx;
                    return_status = NrGcdError::GCD_SLOT_UPDATE_FAILED;
                } else if (!entry.wmeta_idx_.has_value()) {
                    entry.wmeta_idx_ = wmeta_idx;
                    return_status = NrGcdError::GCD_SLOT_UPDATE_FAILED;
                } else {
                    return_status = NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED;
                }
            }
        },
        [&](const decltype(gcd_->map_)::constructor& ctor) {
            GCDEntry entry = init_gcd_entry(cn_index, nid);
            if (nid == NUMA_MEM_IDX) {
                entry.wmeta_idx_ = wmeta_idx;
            }
            ctor(to_insert, entry);
        });
    return return_status;
}

bool GlobalCacheDirectoryHandle::SwitchToReadOnly(const BlockId& to_modify) {
    return gcd_->map_.modify_if(to_modify, [](std::pair<const BlockId, GCDEntry>& row) {
        auto& entry = row.second;
        entry.wmeta_idx_ = std::nullopt;
    });
}

bool GlobalCacheDirectoryHandle::CheckCoherence(const size_t key) {
    (void)key;
    return true;
}

bool GlobalCacheDirectoryHandle::CheckNotificationReset() { return true; }

// bool GlobalCacheDirectoryHandle::CheckNotificationReset() { return true; }

bool GlobalCacheDirectoryHandle::SwitchToRWShared(const BlockId& to_modify, size_t wmeta_idx) {
    return gcd_->map_.modify_if(to_modify, [wmeta_idx](std::pair<const BlockId, GCDEntry>& row) {
        auto& entry = row.second;
        if (!entry.wmeta_idx_.has_value()) {
            entry.wmeta_idx_ = wmeta_idx;
        }
    });
}

NrGcdError GlobalCacheDirectoryHandle::InvalidateSwitchToRWShared(const BlockId& to_modify, size_t wmeta_idx) {
    NrGcdError return_status = NrGcdError::GCD_NO_ERROR;
    bool modified = gcd_->map_.modify_if(to_modify, [&](std::pair<const BlockId, GCDEntry>& row) {
        auto& entry = row.second;
        for (int i = 0; i < LOGICAL_NODE_NUM + 1; i++) {
            if (entry.cn_array_[i].cn_idx_.has_value() && i != NUMA_MEM_IDX) {
                entry.cn_array_[i].invalidate_ = true;
            }
        }
        if (!entry.wmeta_idx_.has_value()) {
            entry.wmeta_idx_ = wmeta_idx;
        } else {
            return_status = NrGcdError::GCD_SLOT_WMETA_UPDATE_FAILED;
        }
    });

    if (!modified) {
        return_status = NrGcdError::GCD_ENTRY_NOEXIST;
    }
    return return_status;
}

bool GlobalCacheDirectoryHandle::CheckCoherence(const BlockId& to_check) {
    (void)to_check;
    return true;
}

void GlobalCacheDirectoryHandle::ResetCoherence(const BlockId& to_check) { (void)to_check; }

bool GlobalCacheDirectoryHandle::CheckCoherenceReset(const BlockId& to_check) {
    (void)to_check;
    return true;
}

}  // namespace rackobj::common