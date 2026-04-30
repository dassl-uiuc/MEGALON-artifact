#include "local_pages.h"

#include "absl/log/check.h"

namespace rackobj::common {

#if 0
static constexpr size_t ReserveNodeCount(size_t elements) {
    return static_cast<size_t>(static_cast<double>(elements) * 0.1);
}
#endif

LocalPageList::LocalPageList(size_t max_elements, const LocalMemoryAllocator<ListNode>& allocator)
    //    : nodes_(allocator.allocate(max_elements + ReserveNodeCount(max_elements))),
    : nodes_(allocator.allocate(max_elements)),
      head_(nullptr),
      tail_(nullptr),
      free_list_(nodes_),
      max_elements_(max_elements),
      count_(0),
      free_count_(max_elements) {
    CHECK(nodes_) << "Local page list ListNode allocation failed!";

    pthread_spin_init(&ll_lock_, PTHREAD_PROCESS_SHARED);
    pthread_spin_init(&free_list_lock_, PTHREAD_PROCESS_SHARED);
    DLOG(INFO) << "Local page list constructed";
}

LocalPageList::~LocalPageList() {
    LOG(INFO) << "lru cache elem number: " << GetCountLocked() << " free: " << free_count_
              << " total: " << GetCountLocked() + free_count_ << " out of " << max_elements_;

    for (size_t i = 0; i < max_elements_; ++i) std::destroy_at(&nodes_[i]);

    pthread_spin_destroy(&ll_lock_);
    pthread_spin_destroy(&free_list_lock_);
}

void LocalPageList::Initialize(size_t max_elements) {
    CHECK(max_elements == max_elements_) << "max_elements mismatch";
    DLOG(INFO) << "init nodes";
    for (size_t i = 0; i < max_elements; ++i) {
        std::construct_at(&nodes_[i]);
        if (i == max_elements - 1) break;
        nodes_[i].next = nodes_ + i + 1;
        // Free list is singly-linked list. No node->prev initialization.
    }

    DLOG(INFO) << "Local page list setting total elements: " << max_elements;
    LOG_IF(WARNING, free_list_ == nullptr) << "free_list_ is null";
}

bool LocalPageList::Insert(size_t cn_idx) {
    size_t count;
    ListNode* to_insert = ReserveListNode();
    to_insert->cn_index_ = cn_idx;

    pthread_spin_lock(&ll_lock_);
    if (IsFullLocked()) {
        DLOG(INFO) << "LocalPageList::Insert page list is Full";
        pthread_spin_unlock(&ll_lock_);
        return false;
    }

    count = ++count_;

    if (head_ == nullptr) {
        head_ = to_insert;
        CHECK(head_) << "head_ should not be null after inserting the first node";
    } else {
        tail_->next = to_insert;
        to_insert->prev = tail_;
    }

    tail_ = to_insert;
    to_insert->next = nullptr;
    pthread_spin_unlock(&ll_lock_);

    DLOG(INFO) << "LocalPageList::Insert @" << to_insert << " with cn_index " << to_insert->cn_index_ << " (" << count
               << " of " << max_elements_ << ")";

    return true;
}

LocalPageList::ListNode* LocalPageList::ReserveListNode() {
    pthread_spin_lock(&free_list_lock_);

    ListNode* out = free_list_;
    if (out == nullptr) {
        DLOG(ERROR) << "No free nodes remaining";
    } else {
        free_list_ = out->next;
        out->next = nullptr;
        free_count_--;
    }
    pthread_spin_unlock(&free_list_lock_);

    DLOG(INFO) << "LocalPageList::ReserveCacheNode reserved @" << out;

    return out;
}

void LocalPageList::RecycleListNode(ListNode* entry) {
    pthread_spin_lock(&free_list_lock_);
    RecycleListNodeLocked(entry);
    pthread_spin_unlock(&free_list_lock_);
}

void LocalPageList::RecycleListNodeBulk(ListNode* head, ListNode* tail) {
    pthread_spin_lock(&free_list_lock_);
    RecycleListNodeBulkLocked(head, tail);
    pthread_spin_unlock(&free_list_lock_);
}

void LocalPageList::RecycleListNodeLocked(ListNode* entry) {
    entry->next = free_list_;
    entry->prev = nullptr;
    free_list_ = entry;
    free_count_++;
}

void LocalPageList::RecycleListNodeBulkLocked(ListNode* head, ListNode* tail) {
    tail->next = free_list_;
    head->prev = nullptr;
    free_list_ = head;
}

void LocalPageList::EvictLocked(ListNode* to_evict) {
    if (to_evict->prev == nullptr) {  // @to_evict is head
        head_ = to_evict->next;
        if (!head_)
            tail_ = nullptr;  // @to_evict is the only node in the list
        else
            head_->prev = nullptr;
    } else if (to_evict->next == nullptr) {  // @to_evict is tail
        tail_ = to_evict->prev;
        tail_->next = nullptr;
    } else {
        to_evict->prev->next = to_evict->next;
        to_evict->next->prev = to_evict->prev;
    }
    count_--;

    to_evict->cn_index_ = UINT64_MAX;
    to_evict->next = nullptr;
    to_evict->prev = nullptr;

    DLOG(INFO) << "LocalPageList::Evict @" << to_evict << " (" << count_ << " of " << max_elements_ << ")";
}

}  // namespace rackobj::common
