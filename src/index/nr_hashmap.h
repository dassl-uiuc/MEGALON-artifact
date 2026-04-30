#pragma once

#include <cstddef>
#include <cstdint>

#include "core/blockid.h"
#include "core/cache_node.h"

namespace NrFfi {
struct NrRapper;
struct NrMeta;

typedef struct CNStatus {
    ssize_t cn_idx_;
    bool invalidate_;
} CNStatus_t;

typedef struct GCDEntry_t {
    ssize_t wmeta_idx_;
    CNStatus_t cn_array_[LOGICAL_NODE_NUM + 1];
} GCDEntry;

extern "C" {
NrRapper* create_node_replicated(uint64_t cap, uint64_t num_replica);
NrRapper* create_node_replicated_numa(uint64_t cap, uint64_t num_replica, const uint8_t* base_addr, size_t slot_size,
                                      uint64_t numa_mem, size_t numa_num);
// bground_sync_period in us
// NrRapper* create_node_replicated_numa_async(uint64_t cap, uint64_t bground_sync_period, uint64_t num_replica);
void free_node_replicated(NrRapper* nrht_wrapper);
const NrMeta* register_node_replicated(NrRapper* nrht_wrapper, size_t idx);
void unregister_node_replicated(const NrMeta* metadata);
void Put(const NrMeta* metadata, rackobj::common::BlockId key, rackobj::common::GCDEntry val);
ssize_t Delete(const NrMeta* metadata, rackobj::common::BlockId key);
bool CheckPut(const NrMeta* metadata, rackobj::common::BlockId key, rackobj::common::GCDEntry val);
bool Swap(const NrMeta* metadata, rackobj::common::BlockId to_insert, rackobj::common::BlockId to_remove,
          rackobj::common::GCDEntry val);
GCDEntry Get(const NrMeta* metadata, rackobj::common::BlockId key);
GCDEntry GetAnchor(const NrMeta* metadata, rackobj::common::BlockId key);
GCDEntry GetAsync(const NrMeta* metadata, rackobj::common::BlockId key);

void PutArray(const NrMeta* metadata, rackobj::common::BlockId key, ssize_t cn, uint64_t idx, ssize_t wmeta_idx = -1);
bool DeleteArray(const NrMeta* metadata, rackobj::common::BlockId key, uint64_t idx);

/**
 * remove the gcd entry specified by idx
 * check wmeta if the target idx is cxl (not allow evict when in RW mode)
 *   0: remove succeeds
 *   1/2/3: remove fails: 1) wmeta valid; 2) entry does not exist; 3) the replica on idx is already deleted
 */
size_t DeleteIfArray(const NrMeta* metadata, rackobj::common::BlockId key, uint64_t idx);
void InvalidateArray(const NrMeta* metadata, rackobj::common::BlockId key, uint64_t idx);
void InvalidateExceptArray(const NrMeta* metadata, rackobj::common::BlockId key, uint64_t idx);

/**
 * invalidate replica entries & set page to rw by updating valid wmeta
 *   0: no error
 *   1: entry does not exist
 *   2: wmeta already allocated
 */
size_t InvalidateExceptArrayUpdateWmeta(const NrMeta* metadata, rackobj::common::BlockId key, uint64_t idx,
                                        ssize_t wmeta_idx);

/**
 return value:
    0: no error
    1: slot & wmeta update unsuccessful
    2: only slot update unsuccessful
 */
size_t CheckPutArray(const NrMeta* metadata, rackobj::common::BlockId key, ssize_t cn, uint64_t idx,
                     ssize_t wmeta_idx = -1);

/**
 * change the wemta of an entry, but with failure condition:
 * condition 1: entry does not exist
 * condition 2: wmeta is already set (not -1)
 */
ssize_t CheckSwitchWmetaArray(const NrMeta* metadata, rackobj::common::BlockId key, ssize_t wmeta_idx = -1);

ssize_t CheckMoveLocalArray(const NrMeta* metadata, rackobj::common::BlockId key, ssize_t cn, uint64_t idx);

bool CheckPutSwapArray(const NrMeta* metadata, rackobj::common::BlockId key, ssize_t cn, uint64_t new_idx,
                       uint64_t old_idx);

ssize_t CheckMoveLocalArray(const NrMeta* metadata, rackobj::common::BlockId key, ssize_t cn, uint64_t idx);

bool CheckSwapArray(const NrMeta* metadata, rackobj::common::BlockId old_key, rackobj::common::BlockId new_key,
                    void* cn, uint64_t new_idx, uint64_t old_idx);

bool CheckCoherence(const NrMeta* metadata, rackobj::common::BlockId key);
bool CheckCoherenceSlot(const NrMeta* metadata, size_t key);
void ResetCoherence(const NrMeta* metadata, rackobj::common::BlockId key);
bool CheckCoherenceReset(const NrMeta* metadata, rackobj::common::BlockId key);

bool CheckNotificationReset(const NrMeta* metadata);

/**
 * not modifying the replicas, just adding a log entry for notification invalidation
 */
void DummyEvent(const NrMeta* metadata, rackobj::common::BlockId key);

}  // extern "C"
}  // namespace NrFfi
