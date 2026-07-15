#ifndef WRAPPER_H
#define WRAPPER_H

#ifndef NUM_NODE
#define NUM_NODE 4
#endif

#include <cstdint>
#include <cstddef>

typedef struct CNStatus_t {
    ssize_t cn_;
    bool invalidate_;
} CNStatus;

typedef struct SeqLock_t {
    bool lock_;
    ssize_t seqcount_;
} SeqLock;

typedef struct GCDEntry_t {
    SeqLock wmeta_;
    ssize_t wmeta_idx_;
    CNStatus cn_array_[NUM_NODE];
} GCDEntry;

typedef struct TrySeqLockResult_t {
    GCDEntry entry_;
    ssize_t status_;
} TrySeqLockResult;

class BlockId {
public:
    BlockId(uint64_t id) {
        server_id_ = id;
        inode_ = 0;
        offset_ = 0;
    }

    uint64_t server_id_;
    ino_t inode_;
    off_t offset_;
};

namespace NrFfi {
    struct NrRapper;
    struct NrMeta;

    extern "C" {
        NrRapper* create_node_replicated(uint64_t cap, uint64_t num_replica);
        NrRapper* create_node_replicated_numa(uint64_t cap, uint64_t num_replica);
        // bground_sync_period in us
        // NrRapper* create_node_replicated_numa_async(uint64_t cap, uint64_t bground_sync_period, uint64_t num_replica);
        void free_node_replicated(NrRapper* nrht_wrapper);
        NrMeta* register_node_replicated(NrRapper* nrht_wrapper, size_t idx);
        void unregister_node_replicated(NrMeta* metadata);
        void Put(NrMeta* metadata, BlockId key, GCDEntry val);
        ssize_t Delete(NrMeta* metadata, BlockId key);
        bool CheckPut(NrMeta* metadata, BlockId key, GCDEntry val);
        bool Swap(NrMeta* metadata, BlockId to_insert, BlockId to_remove, GCDEntry val);
        GCDEntry Get(NrMeta* metadata, BlockId key);
        GCDEntry GetAsync(NrMeta* metadata, BlockId key);

        void PutArray(NrMeta* metadata, BlockId key, ssize_t cn, uint64_t idx, ssize_t wmeta_idx = -1);
        bool DeleteArray(NrMeta* metadata, BlockId key, uint64_t idx);

        /**
         * remove the gcd entry specified by idx
         * check wmeta if the target idx is cxl (not allow evict when in RW mode)
         *   0: remove succeeds
         *   1/2/3: remove fails: 1) wmeta valid; 2) entry does not exist; 3) the replica on idx is already deleted
         */
        size_t DeleteIfArray(NrMeta* metadata, BlockId key, uint64_t idx);
        void InvalidateArray(NrMeta* metadata, BlockId key, uint64_t idx);
        void InvalidateExceptArray(NrMeta* metadata, BlockId key, uint64_t idx);
        /** 
         * invalidate replica entries & set page to rw by updating valid wmeta
         *   0: no error
         *   1: entry does not exist
         *   2: wmeta already allocated
         */
        size_t InvalidateExceptArrayUpdateWmeta(
            NrMeta* metadata, BlockId key, uint64_t idx, ssize_t wmeta_idx);
        /**
         return value:
            0: no error
            1: slot & wmeta update unsuccessful
            2: only slot update unsuccessful
         */
        size_t CheckPutArray(NrMeta* metadata, BlockId key, ssize_t cn, uint64_t idx, ssize_t wmeta_idx = -1);
        ssize_t CheckSwitchWmetaArray(NrMeta* metadata, BlockId key, ssize_t wmeta_idx = -1);
        bool CheckPutSwapArray(NrMeta* metadata, BlockId key, void *cn, uint64_t new_idx, uint64_t old_idx);
        
        bool CheckCoherence(NrMeta* metadata, BlockId key);
        void ResetCoherence(NrMeta* metadata, BlockId key);
        bool CheckCoherenceReset(NrMeta* metadata, BlockId key);

        /**
         * TrySeqLock: try to acquire the per-entry seqlock.
         *   status_ == 1: acquired; status_ == 0: contended; status_ == -1: no entry
         */
        TrySeqLockResult TrySeqLock(NrMeta* metadata, BlockId key);

        /**
         * ReleaseSeqLock: release the per-entry seqlock.
         * Returns new seqcount, or -1 if no entry.
         */
        ssize_t ReleaseSeqLock(NrMeta* metadata, BlockId key);

        /**
         * GetSeqCount: read the current seqcount (immutable, no lock taken).
         * Returns seqcount, or -1 if no entry.
         */
        ssize_t GetSeqCount(NrMeta* metadata, BlockId key);
    }
}

#endif