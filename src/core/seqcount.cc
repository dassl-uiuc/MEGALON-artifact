#include "seqcount.h"

namespace rackobj {

uint32_t read_seqcount_begin(struct seqcount* s) {
    uint32_t seq;

    do {
#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char*>(s), sizeof(s));
#endif
        seq = atomic_load_acquire(&s->sequence);
        if (!(seq & 1)) break;
        cpu_relax();
    } while (1);

    return seq;
}

uint8_t get_active_node(seqlock_t* s) { return s->active_node.load(std::memory_order_relaxed); }

void set_active_node(seqlock_t* s, uint8_t node) { s->active_node.store(node, std::memory_order_relaxed); }

bool read_seqcount_retry(struct seqcount* s, uint32_t seq) {
#ifdef NO_COHERENCE
    cache_flush(reinterpret_cast<char*>(s), sizeof(s));
#endif
    read_barrier();
    return s->sequence != seq;
}

bool write_seqlock_begin(seqlock_t* s) {
#if C3_RWLOCK == 0
    while (true) {
        uint32_t curr = s->sequence.load(std::memory_order_relaxed);
#ifdef DYN_WMETA
        if (curr & FREE_BIT) {
            return false;
        }
        // lock is freed
#endif

        if ((curr & LOCK_BIT) == 0) {
            uint32_t seq = (curr & SEQ_MASK);
            seq = (seq + 1) & SEQ_MASK;  // wrap to 30 bits
            uint32_t new_seq = seq | LOCK_BIT;

#ifdef NO_COHERENCE
            cache_flush(reinterpret_cast<char*>(s), sizeof(s));
#endif
            if (s->sequence.compare_exchange_weak(curr, new_seq, std::memory_order_acq_rel)) {
#ifdef NO_COHERENCE
                cache_flush(reinterpret_cast<char*>(s), sizeof(s));
#endif
                return true;
            }
        } else {
            cpu_relax();
        }
    }
#else

    s->rw_lock.lock();
    uint32_t curr = s->sequence.load(std::memory_order_relaxed);
#ifdef DYN_WMETA
    if (curr & FREE_BIT) {
        s->rw_lock.unlock();
        return false;
    }
#endif

    return true;
#endif
}

// fault tolerant
bool write_seqlock_begin(seqlock_t* s, uint8_t owner) {
    // owner must be 0–3
    uint32_t owner_bits = (uint32_t)(owner & 0x3) << 28;

    while (true) {
        uint32_t curr = s->sequence.load(std::memory_order_relaxed);

        if (curr & FREE_BIT) {
            return false;
        }

        if ((curr & LOCK_BIT) == 0) {
            uint32_t seq = curr & SEQ_MASK;
            seq = (seq + 1) & SEQ_MASK;

            // Insert LOCK_BIT + OWNER_BITS
            uint32_t new_val = seq | LOCK_BIT | owner_bits;

            // Attempt to install
            if (s->sequence.compare_exchange_weak(curr, new_val, std::memory_order_acq_rel)) {
                return true;
            }

        } else {
            cpu_relax();
        }
    }
}

uint32_t write_seqlock_end(seqlock_t* s) {
#if C3_RWLOCK == 0
    uint32_t curr = s->sequence.load(std::memory_order_acquire);
    uint32_t new_seq = (curr + 1) & SEQ_MASK;
    s->sequence.store(new_seq, std::memory_order_release);
#ifdef NO_COHERENCE
    cache_flush(reinterpret_cast<char*>(s), sizeof(s));
#endif
    return new_seq;

#else

    uint32_t curr = s->sequence.load(std::memory_order_relaxed);
    uint32_t new_seq = (curr + 1) & SEQ_MASK;
    s->sequence.store(new_seq, std::memory_order_relaxed);
    s->rw_lock.unlock();
#ifdef NO_COHERENCE
    cache_flush(reinterpret_cast<char*>(s), sizeof(s));
#endif

    return new_seq;
#endif
}

/* fault tolerant */
#if 0
uint32_t write_seqlock_end(seqlock_t* s) {
    while (true) {
        uint32_t curr = s->sequence.load(std::memory_order_acquire);
        uint32_t seq = curr & SEQ_MASK & OWNER_MASK;
        uint32_t new_seq = (seq + 1) & SEQ_MASK & SEQ_MASK & OWNER_MASK;
        uint32_t next_val = new_seq;  

        if (s->sequence.compare_exchange_weak(
                curr, next_val,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return new_seq;
        }

        cpu_relax();
    }
}
#endif

bool write_seqlock_only(seqlock_t* s) {
#if C3_RWLOCK == 0
    while (true) {
        uint32_t curr = s->sequence.load(std::memory_order_relaxed);
#ifdef DYN_WMETA
        if (curr & FREE_BIT) return false;  // lock is freed
#endif

        if ((curr & LOCK_BIT) == 0) {
            uint32_t new_seq = curr | LOCK_BIT;

#ifdef NO_COHERENCE
            cache_flush(reinterpret_cast<char*>(s), sizeof(s));
#endif

            if (s->sequence.compare_exchange_weak(curr, new_seq, std::memory_order_acq_rel)) return true;
        } else {
            cpu_relax();
        }
    }

#else

    s->rw_lock.lock();
    uint32_t curr = s->sequence.load(std::memory_order_relaxed);
#ifdef DYN_WMETA
    if (curr & FREE_BIT) {
        s->rw_lock.unlock();
        return false;
    }
#endif

    return true;
#endif /* C3_RWLOCK */
}

void write_sequnlock_only(seqlock_t* s) {
#if C3_RWLOCK == 0
    uint32_t curr = s->sequence.load(std::memory_order_acquire);
    uint32_t new_seq = (curr)&SEQ_MASK;
    s->sequence.store(new_seq, std::memory_order_release);

#else

    s->rw_lock.unlock();
#endif
}

void free_seqlock(seqlock_t* s) {
#ifdef DYN_WMETA

#if C3_RWLOCK == 0
    while (true) {
        uint32_t curr = s->sequence.load(std::memory_order_relaxed);

        if (!(curr & LOCK_BIT)) {
            if (curr & FREE_BIT) return;

            uint32_t seq = curr & SEQ_MASK;
            if (seq & 1) {
                // Sequence is odd; make it even by incrementing
                seq = (seq + 1) & SEQ_MASK;
            }

            uint32_t new_seq = seq | FREE_BIT;

            if (s->sequence.compare_exchange_weak(curr, new_seq, std::memory_order_acq_rel)) return;
        }
        cpu_relax();
    }

#else

    s->rw_lock.lock();
    uint32_t curr = s->sequence.load(std::memory_order_relaxed);
    uint32_t new_seq = curr | FREE_BIT;
    s->sequence.store(new_seq, std::memory_order_relaxed);
    s->rw_lock.unlock();
#endif /* C3_RWLOCK */
#endif /* DYN_WMETA */
}

bool free_seqlock_with_lock(seqlock_t* s) {
#ifdef DYN_WMETA

#if C3_RWLOCK == 0
    uint32_t curr = s->sequence.load(std::memory_order_relaxed);

    if (curr & FREE_BIT) return true;
    if (!(curr & LOCK_BIT)) return false;

    uint32_t seq = curr & SEQ_MASK;
    if (seq & 1) {
        seq = (seq + 1) & SEQ_MASK;
    }

    uint32_t new_seq = seq | FREE_BIT;  // set free
    new_seq = new_seq & ~LOCK_BIT;      // clear lock

    return s->sequence.compare_exchange_weak(curr, new_seq, std::memory_order_acq_rel);

#else

    uint32_t curr = s->sequence.load(std::memory_order_relaxed);
    uint32_t new_seq = curr | FREE_BIT;
    s->sequence.store(new_seq, std::memory_order_relaxed);
    s->rw_lock.unlock();
    return true;
#endif /* C3_RWLOCK */
#else
    return true;
#endif /* DYN_WMETA */
}

bool try_allocate_seqlock(seqlock_t* s, bool with_lock) {
#ifdef DYN_WMETA

#if C3_RWLOCK == 0
    uint32_t curr = s->sequence.load(std::memory_order_relaxed);
    if ((curr & FREE_BIT) == 0 || (curr & LOCK_BIT) != 0) return false;

    // uint32_t new_seq = curr & SEQ_MASK;
    uint32_t new_seq = 0;  // initialize to 0 instead of keeping old sequence value

    if (with_lock) {
        new_seq = (new_seq + 1) & SEQ_MASK;
        new_seq = new_seq | LOCK_BIT;
    }
    new_seq = new_seq & ~FREE_BIT;  // clear free bit

    return s->sequence.compare_exchange_weak(curr, new_seq, std::memory_order_acq_rel);
#else

    uint32_t curr = s->sequence.load(std::memory_order_relaxed);
    if ((curr & FREE_BIT) == 0) return false;

    uint32_t new_seq = 0;
    if (with_lock) {
        s->rw_lock.lock();
    }
    new_seq = new_seq & ~FREE_BIT;

    bool ret = s->sequence.compare_exchange_weak(curr, new_seq, std::memory_order_acq_rel);
    if (!ret && with_lock) {
        s->rw_lock.unlock();
    }
    return ret;
#endif /* C3_RWLOCK */
#else
    return true;
#endif /* DYN_WMETA */
}

uint32_t seqlock_count(seqlock_t* s) { return s->sequence.load(std::memory_order_acquire) & SEQ_MASK; }

uint32_t seqlock_full(seqlock_t* s) { return s->sequence.load(std::memory_order_acquire); }

uint32_t seqlock_full_relaxed(seqlock_t* s) { return s->sequence.load(std::memory_order_relaxed); }

uint32_t read_seqlock_begin(seqlock_t* s) {
#if C3_RWLOCK == 0
    uint32_t seq;

    do {
#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char*>(s), sizeof(*s));
#endif
        seq = s->sequence.load(std::memory_order_acquire);
        if (!(seq & 1)) break;
        cpu_relax();
    } while (true);

    return seq & SEQ_MASK;
#else

    s->rw_lock.lock_shared();
    return s->sequence.load(std::memory_order_relaxed);
#endif /* C3_RWLOCK */
}

bool read_seqlock_retry(seqlock_t* s, uint32_t start_seq) {
#if C3_RWLOCK == 0

#ifdef NO_COHERENCE
    cache_flush(reinterpret_cast<char*>(s), sizeof(*s));
#endif
    uint32_t end_seq = s->sequence.load(std::memory_order_acquire);
#ifdef DYN_WMETA
    return (start_seq != (end_seq & SEQ_MASK)) || (end_seq & FREE_BIT);
    // either seq number does not match or the entry is free
#else
    return start_seq != (end_seq & SEQ_MASK);
#endif

#else

    (void)start_seq;  // shall never be incoherent
    s->rw_lock.unlock_shared();
    return false;
#endif /* C3_RWLOCK */
}

}  // namespace rackobj