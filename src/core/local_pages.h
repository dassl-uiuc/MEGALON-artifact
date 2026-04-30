#pragma once

#include "local_cache_node.h"
#include "shared_memory/allocator.h"

namespace rackobj::common {

class LocalPageList {
public:
    class ListNode {
    public:
        explicit ListNode() : cn_index_(UINT64_MAX), prev(nullptr), next(nullptr) {}

        size_t cn_index() const { return cn_index_; }

    private:
        size_t cn_index_;
        ListNode* prev;
        ListNode* next;

        friend class LocalPageList;
        friend class LocalPageListHead;
    };

    LocalPageList(size_t max_elements, const LocalMemoryAllocator<ListNode>& allocator);

    ~LocalPageList();

    void Initialize(size_t max_elements);

    bool Insert(size_t cn_idx);

    size_t GetMaxElements() const { return max_elements_; }

    size_t GetCountLocked() const { return count_; }

    size_t GetCount() const {
        pthread_spin_lock(&ll_lock_);
        size_t count = GetCountLocked();
        pthread_spin_unlock(&ll_lock_);
        return count;
    }

    ListNode* GetListHead() const { return head_; }
    ListNode* GetListTail() const { return tail_; }

    int LockList() { return pthread_spin_lock(&ll_lock_); }
    int UnlockList() { return pthread_spin_unlock(&ll_lock_); }

    void RecycleListNode(ListNode* entry);
    void RecycleListNodeBulk(ListNode* head, ListNode* tail);

    void RecycleListNodeLocked(ListNode* entry);
    void RecycleListNodeBulkLocked(ListNode* head, ListNode* tail);

    /* Delete @to_evict from Local page List */
    void Evict(ListNode* to_evict) {
        pthread_spin_lock(&ll_lock_);
        EvictLocked(to_evict);
        pthread_spin_unlock(&ll_lock_);
    }

    void EvictLocked(ListNode* to_evict);

    /* assume list being locked */
    ListNode* NextListNodeLocked(ListNode* entry) { return entry->next ? entry->next : head_; }

    /* assume list being locked */
    ListNode* PrevListNodeLocked(ListNode* entry) { return entry->prev ? entry->prev : tail_; }

private:
    // Grab a node from the free list. It is an error to call this function
    // without knowledge that there is a free node available.
    ListNode* ReserveListNode();

    bool IsFullLocked() const { return GetCountLocked() >= max_elements_; }

    // A lock in shared memory to serialize access to the linked list.
    alignas(64) mutable pthread_spinlock_t ll_lock_;

    // A lock in shared memory to serialize access to the free list.
    alignas(64) pthread_spinlock_t free_list_lock_;

    // A pointer to the block given by the allocation of nodes for this linked list.
    alignas(64) ListNode* nodes_;

    // The head of the doubly-linked list that represents the LRU list.
    // This element is the least recently used element.
    ListNode* head_;

    // The tail of the doubly-linked list that represents the LRU list.
    // This element is the most recently used element.
    ListNode* tail_;

    // A singly-linked list of nodes free to use for new elements
    ListNode* free_list_;

    size_t max_elements_;

    size_t count_;

    size_t free_count_;
};

}  // namespace rackobj::common
