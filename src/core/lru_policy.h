#pragma once

#include "local_cache_node.h"
#include "shared_memory/allocator.h"

namespace rackobj::common {

class LruPolicy {
public:
    class PolicyNode : public LocalCacheNode {
    public:
        explicit PolicyNode(size_t cn_index, uint8_t* page)
            : LocalCacheNode(cn_index, page), prev(nullptr), next(nullptr) {}

    private:
        PolicyNode* prev;
        PolicyNode* next;

        friend class LruPolicy;
    };

    LruPolicy(size_t max_elements, const LocalMemoryAllocator<PolicyNode>& allocator);

    ~LruPolicy();

    bool Insert(LocalCacheNode* to_insert, ConstLocalCacheNodeCallback&& eviction_cb);

    void Touch(LocalCacheNode* node);
    void TouchLocked(LocalCacheNode* node);

    void Initialize(uint8_t* pages, size_t max_elements);

    size_t GetMaxElements() const { return max_elements_; }

    size_t GetCountLocked() const { return count_; }

    size_t GetCount() const {
        pthread_spin_lock(&ll_lock_);
        size_t count = GetCountLocked();
        pthread_spin_unlock(&ll_lock_);
        return count;
    }

    LocalCacheNode* GetListHead() const { return head_; }
    LocalCacheNode* GetListTail() const { return tail_; }

    // int LockList() { return pthread_mutex_lock(&ll_lock_); }
    int LockList() { return pthread_spin_lock(&ll_lock_); }
    // int UnlockList() { return pthread_mutex_unlock(&ll_lock_); }
    int UnlockList() { return pthread_spin_unlock(&ll_lock_); }

    // Grab a node from the free list. It is an error to call this function
    // without knowledge that there is a free node available.
    LocalCacheNode* ReserveCacheNode();

    void RecycleCacheNode(LocalCacheNode* cn);
    void RecycleCacheNodeBulk(LocalCacheNode* head, LocalCacheNode* tail);

    void RecycleCacheNodeLocked(LocalCacheNode* cn);
    void RecycleCacheNodeBulkLocked(LocalCacheNode* head, LocalCacheNode* tail);

    uint8_t* Evict(LocalCacheNode* cn);
    uint8_t* EvictLocked(LocalCacheNode* cn);

    LocalCacheNode* NextCacheNodeLocked(LocalCacheNode* cn);
    LocalCacheNode* PrevCacheNodeLocked(LocalCacheNode* cn);

private:
    bool IsFullLocked() const { return GetCountLocked() >= max_elements_; }

    // A lock in shared memory to serialize access to the linked list.
    // alignas(64) pthread_mutex_t ll_lock_;
    alignas(64) mutable pthread_spinlock_t ll_lock_;

    // A lock in shared memory to serialize access to the free list.
    // alignas(64) pthread_mutex_t free_list_lock_;
    alignas(64) pthread_spinlock_t free_list_lock_;

    // A pointer to the block given by the allocation of nodes for this linked
    // list.
    alignas(64) PolicyNode* nodes_;

    // The head of the doubly-linked list that represents the LRU list.
    // This element is the least recently used element.
    PolicyNode* head_;

    // The tail of the doubly-linked list that represents the LRU list.
    // This element is the most recently used element.
    PolicyNode* tail_;

    // A singly-linked list of nodes free to use for new elements
    PolicyNode* free_list_;

    pthread_cond_t ll_cond_;
    alignas(64) pthread_mutex_t ll_cond_lock_;

    size_t max_elements_;

    size_t count_;

    size_t free_count_;

    // Evict a node from the cache. Precondition: the lock for this linked list
    // is held by the calling thread.
    PolicyNode* EvictOneLocked();
};

}  // namespace rackobj::common
