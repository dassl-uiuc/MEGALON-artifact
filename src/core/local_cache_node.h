#pragma once

#include <optional>

#include "blockid.h"
#include "cache_node.h"
#include "common/constants.h"
#include "seqcount.h"

namespace rackobj::lib {

template <typename Policy>
class SharedMemoryObjectHandle;

}  // namespace rackobj::lib

namespace rackobj::common {

class LocalCacheNode : public CacheNode {
    friend class SharedMemoryObject;

    template <typename Policy>
    friend class lib::SharedMemoryObjectHandle;

public:
    explicit LocalCacheNode(size_t cn_index, uint8_t* page)
        : CacheNode(),
          cn_index_(cn_index),
          page_ptr_(page)
    //, valid_(false)
    {
        pthread_rwlock_init(&evict_rw_lock_, NULL);
        // Initialize seqlock to 0 (allocated, unlocked, no FREE_BIT)
        seqlock_.sequence = 0;
        seqlock_.active_node = 0;
    }

    virtual ~LocalCacheNode() { pthread_rwlock_destroy(&evict_rw_lock_); }

    LocalCacheNode(const LocalCacheNode&) = delete;
    LocalCacheNode& operator=(const LocalCacheNode&) = delete;
    LocalCacheNode(LocalCacheNode&&) = delete;
    LocalCacheNode& operator=(LocalCacheNode&&) = delete;

    void Reinitialize(const BlockId& new_block_id) { std::construct_at(&block_id_, new_block_id); }

    size_t GetIndex() const { return cn_index_; }

    const BlockId& GetBlockId() const { return block_id_; }

    uint8_t* GetDataSlot() const { return page_ptr_; }

    // uint64_t GetCachedServer() const { return cached_server_id_; }

    size_t GetLength() const { return static_cast<size_t>(length_); }

    void SetLength(size_t len) { length_ = static_cast<uint16_t>(len); }

    // void SetPage(uint8_t* page_ptr) { page_ptr_ = page_ptr; }

    // void SetCachedServer(uint64_t server_id) { cached_server_id_ = server_id; }

    bool Compare(LocalCacheNode* cn) {
        if ((!(block_id_ == cn->block_id_)) || page_ptr_ != cn->page_ptr_ || length_ != cn->length_) return false;
        return true;
    }

    [[nodiscard]] bool TryReadLock() { return !pthread_rwlock_tryrdlock(&evict_rw_lock_); }

    [[nodiscard]] bool EvictTryLock() { return !pthread_rwlock_trywrlock(&evict_rw_lock_); }

    void Unlock() { pthread_rwlock_unlock(&evict_rw_lock_); }

    // Seqlock methods for read/write consistency
    [[nodiscard]] bool WSeqBegin() { return write_seqlock_begin(&seqlock_); }
    uint32_t WSeqEnd() { return write_seqlock_end(&seqlock_); }
    [[nodiscard]] uint32_t RSeqBegin() { return read_seqlock_begin(&seqlock_); }
    [[nodiscard]] bool RSeqRetry(uint32_t sequence) { return read_seqlock_retry(&seqlock_, sequence); }

protected:
    size_t cn_index_;

    uint8_t* page_ptr_;

    // This lock is to synchronize filling this block from an RPC and/or writes
    // to this block from competing processes. This is a spinlock since we do
    // not anticipate much contention to this lock and since a full mutex is
    // 40 bytes as opposed to the spinlocks 4 bytes.
    pthread_rwlock_t evict_rw_lock_;

    // Seqlock for read/write consistency on local exclusive pages
    seqlock_t seqlock_;

    // std::atomic<bool> valid_;
};

typedef std::function<void(const LocalCacheNode&)> ConstLocalCacheNodeCallback;
typedef std::function<void(LocalCacheNode&)> LocalCacheNodeCallback;

}  // namespace rackobj::common