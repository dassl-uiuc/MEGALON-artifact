#include "seqcount.h"

bool write_seqlock_begin(seqlock_t* s) {
    while (true) {
        unsigned int curr = s->sequence.load(std::memory_order_relaxed);
        if (curr & FREE_BIT) return false;  // lock is freed

        if ((curr & LOCK_BIT) == 0) {
            uint32_t seq = (curr & SEQ_MASK);
            seq = (seq + 1) & SEQ_MASK;  // wrap to 30 bits
            uint32_t new_seq = seq | LOCK_BIT;

            if (s->sequence.compare_exchange_weak(curr, new_seq, std::memory_order_acq_rel)) return true;
        } else {
            cpu_relax();
        }
    }
}

unsigned int write_seqlock_end(seqlock_t* s) {
    unsigned int curr = s->sequence.load(std::memory_order_acquire);
    unsigned int new_seq = (curr + 1) & SEQ_MASK;
    s->sequence.store(new_seq, std::memory_order_release);
    return new_seq;
}

void free_seqlock(seqlock_t* s) {
    while (true) {
        uint32_t curr = s->sequence.load(std::memory_order_relaxed);

        if (!(curr & LOCK_BIT)) {
            if (curr & FREE_BIT) return;

            uint32_t new_seq = curr | FREE_BIT;

            if (s->sequence.compare_exchange_weak(curr, new_seq, std::memory_order_acq_rel)) return;
        }
        cpu_relax();
    }
}

bool try_allocate_seqlock(seqlock_t* s, bool with_lock) {
    unsigned int curr = s->sequence.load(std::memory_order_relaxed);
    if ((curr & FREE_BIT) == 0 || (curr & LOCK_BIT) != 0) return false;

    unsigned int new_seq = curr & SEQ_MASK;

    if (with_lock) {
        new_seq = (new_seq + 1) & SEQ_MASK;
        new_seq = new_seq | LOCK_BIT;
    }
    new_seq = new_seq & ~FREE_BIT;  // clear free bit

    return s->sequence.compare_exchange_weak(curr, new_seq, std::memory_order_acq_rel);
}

unsigned int seqlock_count(seqlock_t* s) { return s->sequence.load(std::memory_order_acquire) & SEQ_MASK; }

unsigned int read_seqlock_begin(seqlock_t* s) {
    unsigned int seq;

    do {
        seq = s->sequence.load(std::memory_order_acquire);
        if (!(seq & 1)) break;
        cpu_relax();
    } while (true);

    return seq & SEQ_MASK;
}

bool read_seqlock_retry(seqlock_t* s, unsigned int start_seq) {
    unsigned int end_seq = s->sequence.load(std::memory_order_acquire);
    return (start_seq != end_seq) || (end_seq & FREE_BIT);
    // either seq number does not match or the entry is free
}