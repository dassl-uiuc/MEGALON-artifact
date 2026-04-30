#include "lru_policy.h"

#include "absl/log/check.h"

namespace rackobj::common {

#if 0
static constexpr size_t ReserveNodeCount(size_t elements) {
    return static_cast<size_t>(static_cast<double>(elements) * 0.1);
}
#endif

LruPolicy::LruPolicy(size_t max_elements, const LocalMemoryAllocator<PolicyNode>& allocator)
    //    : nodes_(allocator.allocate(max_elements + ReserveNodeCount(max_elements))),
    : nodes_(allocator.allocate(max_elements)),
      head_(nullptr),
      tail_(nullptr),
      free_list_(nodes_),
      max_elements_(max_elements),
      count_(0),
      free_count_(max_elements) {
    pthread_mutexattr_t attr;
    pthread_condattr_t cond_attr;

    CHECK(nodes_) << "Lru PolicyNode allocation failed!";

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    // pthread_mutex_init(&ll_lock_, &attr);
    pthread_spin_init(&ll_lock_, 1);
    // pthread_mutex_init(&free_list_lock_, &attr);
    pthread_spin_init(&free_list_lock_, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&ll_cond_lock_, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_condattr_init(&cond_attr);
    pthread_cond_init(&ll_cond_, &cond_attr);
    pthread_condattr_destroy(&cond_attr);
    DLOG(INFO) << "LruPolicy list constructed";
}

LruPolicy::~LruPolicy() {
    LOG(INFO) << "lru cache elem number: " << GetCountLocked() << " free: " << free_count_
              << " total: " << GetCountLocked() + free_count_ << " out of " << max_elements_;

    for (size_t i = 0; i < max_elements_; ++i) std::destroy_at(&nodes_[i]);

    // pthread_mutex_destroy(&ll_lock_);
    pthread_spin_destroy(&ll_lock_);
    // pthread_mutex_destroy(&free_list_lock_);
    pthread_spin_destroy(&free_list_lock_);
    pthread_mutex_destroy(&ll_cond_lock_);
}

void LruPolicy::Initialize(uint8_t* pages, size_t max_elements) {
    CHECK(max_elements == max_elements_) << "max_elements mismatch";
    DLOG(INFO) << "init nodes";
    for (size_t i = 0; i < max_elements; ++i) {
        std::construct_at(&nodes_[i], i, pages + (i * common::BlockId::kBlockSize));
        if (i == max_elements - 1) break;
        nodes_[i].next = nodes_ + i + 1;
        // Free list is singly-linked list. No node->prev initialization.
    }

    //    size_t total_elements = max_elements + ReserveNodeCount(max_elements);
    size_t total_elements = max_elements;

    DLOG(INFO) << "lru policy setting max elements: " << max_elements << ", total_elements: " << total_elements;
    LOG_IF(WARNING, free_list_ == nullptr) << "free_list_ is null";
}

LruPolicy::PolicyNode* LruPolicy::EvictOneLocked() {
    // execute with ll_lock_ grabbed
    PolicyNode* pn = head_;
    DLOG(INFO) << "Evict " << pn->block_id_;

    if (pn->prev == nullptr) {
        head_ = pn->next;
    } else {
        pn->prev->next = pn->next;
    }

    if (pn->next == nullptr) {
        tail_ = pn->prev;
    } else {
        pn->next->prev = pn->prev;
    }
    count_--;

    pthread_cond_signal(&ll_cond_);
    // pn->next = nullptr;
    // pn->prev = nullptr;
    return pn;
}

uint8_t* LruPolicy::EvictLocked(LocalCacheNode* cn) {
    uint8_t* ret = cn->GetPage();
    PolicyNode* pn = static_cast<PolicyNode*>(cn);

    if (pn->prev == nullptr) {  // pn is head
        head_ = pn->next;
        if (!head_)
            tail_ = nullptr;  // pn is the only node in the list
        else
            head_->prev = nullptr;
    } else if (pn->next == nullptr) {  // pn is tail
        tail_ = pn->prev;
        tail_->next = nullptr;
    } else {
        pn->prev->next = pn->next;
        pn->next->prev = pn->prev;
    }
    count_--;

    pn->next = nullptr;
    pn->prev = nullptr;
    pthread_cond_signal(&ll_cond_);
    // pn->SetPage(nullptr);
    // RecycleCacheNode(pn);

    DLOG(INFO) << "LruPolicy::Evict @" << pn << " with block_id " << pn->block_id_ << " (" << count_ << " of "
               << max_elements_ << ")";

    return ret;
}

/* Delete @cn from Policy List */
uint8_t* LruPolicy::Evict(LocalCacheNode* cn) {
    uint8_t* ret;

    // pthread_mutex_lock(&ll_lock_);
    pthread_spin_lock(&ll_lock_);
    ret = EvictLocked(cn);
    // pthread_mutex_unlock(&ll_lock_);
    pthread_spin_unlock(&ll_lock_);

    return ret;
}

bool LruPolicy::Insert(LocalCacheNode* to_insert, ConstLocalCacheNodeCallback&& eviction_cb) {
    size_t count;
    bool should_evict = false;
    // uint8_t* page_ptr = nullptr;
    PolicyNode* inserted = static_cast<PolicyNode*>(to_insert);

    pthread_spin_lock(&ll_lock_);
    // pthread_mutex_lock(&ll_lock_);
    while (IsFullLocked()) {
        DLOG(INFO) << "LRUPolicy::Insert LruPolict list is Full;"
                   << " Waiting for eviction";
        pthread_mutex_lock(&ll_cond_lock_);
        pthread_spin_unlock(&ll_lock_);
        pthread_cond_wait(&ll_cond_, &ll_cond_lock_);
        pthread_spin_lock(&ll_lock_);
    }
    // PolicyNode* evicted = EvictOneLocked();
    // page_ptr = evicted->GetPage();
    // evicted->SetPage(nullptr); // TODO: pass actual page content to eviction_callback
    // RecycleCacheNode(evicted);
    // eviction_cb(*evicted);
    // DLOG(INFO) << "LRUPolicy::Insert Evicted @" << evicted
    //         << "with blockid" << evicted->block_id_;
    (void)eviction_cb;

    count = ++count_;

    if (head_ == nullptr) {
        head_ = inserted;
        CHECK(head_) << "head_ should not be null after inserting the first node";
    } else {
        tail_->next = inserted;
        inserted->prev = tail_;
    }

    tail_ = inserted;
    inserted->next = nullptr;
    // pthread_mutex_unlock(&ll_lock_);
    pthread_spin_unlock(&ll_lock_);

    DLOG(INFO) << "LruPolicy::Insert @" << inserted << " with block_id " << inserted->block_id_ << " (" << count
               << " of " << max_elements_ << ")";

    return should_evict;
}
#if 0
    CacheNodeGuard g(inserted);
    g->Reinitialize(block_id);
    if (page_ptr != nullptr) {
        CHECK(g->GetPage() == nullptr) << "Node already has assigned page: " << (void*)g->GetPage();
        g->SetPage(page_ptr);
    }
    CHECK(g->GetPage() != nullptr) << "Inserting node with no page assigned";
    // DLOG(INFO) << "g->GetPage()=" << (void*)(g->GetPage());
#endif

void LruPolicy::Touch(LocalCacheNode* node) {
    // pthread_mutex_lock(&ll_lock_);
    pthread_spin_lock(&ll_lock_);
    TouchLocked(node);
    // pthread_mutex_unlock(&ll_lock_);
    pthread_spin_unlock(&ll_lock_);
}

void LruPolicy::TouchLocked(LocalCacheNode* cn) {
    PolicyNode* pn = static_cast<PolicyNode*>(cn);

    if (pn->next == nullptr) {  // pn is already tail
        return;
    } else if (pn->prev == nullptr) {  // pn is head
        head_ = pn->next;
        head_->prev = nullptr;
    } else {
        pn->prev->next = pn->next;
        pn->next->prev = pn->prev;
    }

    if (head_ == nullptr) {
        head_ = pn;
        pn->prev = nullptr;
    } else {
        tail_->next = pn;
        pn->prev = tail_;
    }
    tail_ = pn;
    pn->next = nullptr;

    DLOG(INFO) << "LruPolicy::TouchLocked @" << pn << " with block_id " << pn->block_id_;
}

LocalCacheNode* LruPolicy::ReserveCacheNode() {
    // DLOG(INFO) << "Reserving cache node";
    // pthread_mutex_lock(&free_list_lock_);
    pthread_spin_lock(&free_list_lock_);

    PolicyNode* out = free_list_;
    if (out == nullptr) {
        LOG(ERROR) << "No free nodes remaining";
    } else {
        free_list_ = out->next;
        out->next = nullptr;
        free_count_--;
    }
    // pthread_mutex_unlock(&free_list_lock_);
    pthread_spin_unlock(&free_list_lock_);

    DLOG(INFO) << "LruPolicy::ReserveCacheNode reserved @" << out;

    return out;
}

/* assume list being locked */
LocalCacheNode* LruPolicy::NextCacheNodeLocked(LocalCacheNode* cn) {
    PolicyNode* pn = static_cast<PolicyNode*>(cn);
    return pn->next ? pn->next : head_;
}

/* assume list being locked */
LocalCacheNode* LruPolicy::PrevCacheNodeLocked(LocalCacheNode* cn) {
    PolicyNode* pn = static_cast<PolicyNode*>(cn);
    return pn->prev ? pn->prev : tail_;
}

void LruPolicy::RecycleCacheNode(LocalCacheNode* cn) {
    // pthread_mutex_lock(&free_list_lock_);
    pthread_spin_lock(&free_list_lock_);
    RecycleCacheNodeLocked(cn);
    // pthread_mutex_unlock(&free_list_lock_);
    pthread_spin_unlock(&free_list_lock_);
}

void LruPolicy::RecycleCacheNodeBulk(LocalCacheNode* head, LocalCacheNode* tail) {
    // pthread_mutex_lock(&free_list_lock_);
    pthread_spin_lock(&free_list_lock_);
    RecycleCacheNodeBulkLocked(head, tail);
    // pthread_mutex_unlock(&free_list_lock_);
    pthread_spin_unlock(&free_list_lock_);
}

void LruPolicy::RecycleCacheNodeLocked(LocalCacheNode* cn) {
    PolicyNode* pn = static_cast<PolicyNode*>(cn);
    pn->next = free_list_;
    pn->prev = nullptr;
    free_list_ = pn;
    free_count_++;
}

void LruPolicy::RecycleCacheNodeBulkLocked(LocalCacheNode* head, LocalCacheNode* tail) {
    PolicyNode* head_pn = static_cast<PolicyNode*>(head);
    PolicyNode* tail_pn = static_cast<PolicyNode*>(tail);
    tail_pn->next = free_list_;
    head_pn->prev = nullptr;
    free_list_ = head_pn;
}

}  // namespace rackobj::common
