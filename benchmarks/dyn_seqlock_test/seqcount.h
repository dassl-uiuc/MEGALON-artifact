#ifndef FIO_SEQLOCK_H
#define FIO_SEQLOCK_H

#include "arch.h"
// #include "helper.h"

constexpr unsigned int FREE_BIT = 1u << 31;
constexpr unsigned int LOCK_BIT = 1u << 30;
constexpr unsigned int SEQ_MASK = ~(FREE_BIT | LOCK_BIT);

/**
 * seqlock:
 * dynamic sequence lock implementation
 * data layout:
 * | free bit | lock bit | ... sequence count ... |
 */
typedef struct seqlock {
#ifdef __cplusplus
    std::atomic<unsigned int> sequence;
#else
    volatile unsigned int sequence;
#endif
} seqlock_t;

static inline void seqlock_init(seqlock_t *s) { s->sequence = FREE_BIT; }
static inline void seqlock_init_without_free(seqlock_t *s) { s->sequence = 0; }

unsigned int read_seqlock_begin(seqlock_t *s);

bool read_seqlock_retry(seqlock_t *s, unsigned int seq);

bool write_seqlock_begin(seqlock_t *s);

unsigned int write_seqlock_end(seqlock_t *s);

unsigned int seqlock_count(seqlock_t *s);

void free_seqlock(seqlock_t *s);

bool try_allocate_seqlock(seqlock_t *s, bool with_lock = true);

#endif
