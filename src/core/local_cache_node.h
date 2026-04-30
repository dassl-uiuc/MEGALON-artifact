#pragma once

#include <optional>

#include "blockid.h"
#include "cache_node.h"
#include "cc_primitive.h"
#include "common/constants.h"

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
    }

    virtual ~LocalCacheNode() { pthread_rwlock_destroy(&evict_rw_lock_); }

    LocalCacheNode(const LocalCacheNode&) = delete;
    LocalCacheNode& operator=(const LocalCacheNode&) = delete;
    LocalCacheNode(LocalCacheNode&&) = delete;
    LocalCacheNode& operator=(LocalCacheNode&&) = delete;

    void Reinitialize(const BlockId& new_block_id) { std::construct_at(&block_id_, new_block_id); }

    size_t GetIndex() const { return cn_index_; }

    const BlockId& GetBlockId() const { return block_id_; }

    uint8_t* GetPage() const { return page_ptr_; }

    // uint64_t GetCachedServer() const { return cached_server_id_; }

    size_t GetLength() const { return static_cast<size_t>(length_); }

    void SetLength(size_t len) { length_ = static_cast<uint16_t>(len); }

    // void SetPage(uint8_t* page_ptr) { page_ptr_ = page_ptr; }

    // void SetCachedServer(uint64_t server_id) { cached_server_id_ = server_id; }
#if 0
    bool IsValid() const { return valid_.load(std::memory_order_release); }

    [[nodiscard]] bool SetValid(bool valid) {
        // valid_.store(valid, std::memory_order_acquire);
        bool expected = !valid;
        while (!valid_.compare_exchange_weak(expected, valid, std::memory_order_seq_cst)) {
            if (expected == !valid) {
                // Spurious failure; false negative: The atomic value didn't actually change.
                continue;
            } else {
                // Actual data mismatch: The atomic value was different.
                DLOG(INFO) << "CacheNode::SetValid @" << this << " with block_id " << GetBlockId()
                           << " current:" << expected << "expected: " << !valid;
                return false;
            }
        }
        return true;
    }
#endif

    bool Compare(LocalCacheNode* cn) {
        if ((!(block_id_ == cn->block_id_)) || page_ptr_ != cn->page_ptr_ || length_ != cn->length_) return false;
        return true;
    }

    [[nodiscard]] bool TryReadLock() { return !pthread_rwlock_tryrdlock(&evict_rw_lock_); }

    [[nodiscard]] bool EvictTryLock() { return !pthread_rwlock_trywrlock(&evict_rw_lock_); }

    void Unlock() { pthread_rwlock_unlock(&evict_rw_lock_); }

protected:
    size_t cn_index_;

    uint8_t* page_ptr_;

    // This lock is to synchronize filling this block from an RPC and/or writes
    // to this block from competing processes. This is a spinlock since we do
    // not anticipate much contention to this lock and since a full mutex is
    // 40 bytes as opposed to the spinlocks 4 bytes.
    pthread_rwlock_t evict_rw_lock_;

    // std::atomic<bool> valid_;
};

typedef std::function<void(const LocalCacheNode&)> ConstLocalCacheNodeCallback;
typedef std::function<void(LocalCacheNode&)> LocalCacheNodeCallback;

}  // namespace rackobj::common