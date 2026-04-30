#ifndef FIO_SEQLOCK_H
#define FIO_SEQLOCK_H

#include "arch/arch.h"
#include "common/constants.h"
#include "common/helper.h"

#if C3_RWLOCK == 1
#include <shared_mutex>
#elif C3_RWLOCK == 2
#include <absl/synchronization/mutex.h>
#endif

namespace rackobj {

#if C3_RWLOCK != 0
class C3RwLock {
public:
#if C3_RWLOCK == 1
    std::shared_mutex inner;

    void lock_shared() { inner.lock_shared(); }
    void unlock_shared() { inner.unlock_shared(); }
    void lock() { inner.lock(); }
    void unlock() { inner.unlock(); }

#elif C3_RWLOCK == 2
    absl::Mutex inner;

    void lock_shared() { inner.ReaderLock(); }
    void unlock_shared() { inner.ReaderUnlock(); }
    void lock() { inner.WriterLock(); }
    void unlock() { inner.WriterUnlock(); }
#endif
};
#endif

typedef struct seqcount {
#ifdef __cplusplus
    std::atomic<uint32_t> sequence;
#else
    volatile uint32_t sequence;
#endif
} seqcount_t;

static inline void seqcount_init(struct seqcount *s) { s->sequence = 0; }

uint32_t read_seqcount_begin(struct seqcount *s);

bool read_seqcount_retry(struct seqcount *s, uint32_t seq);

static inline void write_seqcount_begin(struct seqcount *s) {
    std::atomic_fetch_add_explicit(&s->sequence, 1, std::memory_order_release);
#ifdef NO_COHERENCE
    cache_flush(reinterpret_cast<char *>(&s->sequence), sizeof(s->sequence));
#endif
}

static inline uint32_t write_seqcount_end(struct seqcount *s) {
    uint32_t new_sequence = std::atomic_fetch_add_explicit(&s->sequence, 1, std::memory_order_release) + 1;
#ifdef NO_COHERENCE
    cache_flush(reinterpret_cast<char *>(&s->sequence), sizeof(s->sequence));
#endif
    return new_sequence;
}

constexpr uint32_t FREE_BIT = 1u << 31;
constexpr uint32_t LOCK_BIT = 1u << 30;
constexpr uint32_t SEQ_MASK = ~(FREE_BIT | LOCK_BIT);

/**
 * seqlock:
 * dynamic sequence lock implementation
 * data layout:
 * | free bit | lock bit | ... sequence count ... |
 */
typedef struct seqlock {
#ifdef __cplusplus
    std::atomic<uint32_t> sequence;
#else
    volatile uint32_t sequence;
#endif

#if C3_RWLOCK != 0
    C3RwLock rw_lock;
    explicit seqlock(uint32_t initial = 0) : sequence(initial) {}
#endif
} seqlock_t;

static inline void seqlock_clear(seqlock_t *s) { s->sequence = 0u; }

static inline void seqlock_init(seqlock_t *s) { s->sequence = FREE_BIT; }

uint32_t read_seqlock_begin(seqlock_t *s);

bool read_seqlock_retry(seqlock_t *s, uint32_t seq);

bool write_seqlock_begin(seqlock_t *s);

uint32_t write_seqlock_end(seqlock_t *s);

bool write_seqlock_only(seqlock_t *s);

void write_sequnlock_only(seqlock_t *s);

uint32_t seqlock_count(seqlock_t *s);

uint32_t seqlock_full(seqlock_t *s);

uint32_t seqlock_full_relaxed(seqlock_t *s);

void free_seqlock(seqlock_t *s);

bool free_seqlock_with_lock(seqlock_t *s);

bool try_allocate_seqlock(seqlock_t *s, bool with_lock = true);

}  // namespace rackobj

#endif
