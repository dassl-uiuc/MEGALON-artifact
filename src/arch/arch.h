#ifndef FIO_ARCH_H
#define FIO_ARCH_H

#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif

#ifdef __cplusplus
#define atomic_add(p, v) std::atomic_fetch_add(p, (v))
#define atomic_sub(p, v) std::atomic_fetch_sub(p, (v))
#define atomic_load_relaxed(p) std::atomic_load_explicit(p, std::memory_order_relaxed)
#define atomic_load_acquire(p) std::atomic_load_explicit(p, std::memory_order_acquire)
#define atomic_store_release(p, v) std::atomic_store_explicit(p, (v), std::memory_order_release)
#else
#define atomic_add(p, v) atomic_fetch_add((_Atomic typeof(*(p)) *)(p), v)
#define atomic_sub(p, v) atomic_fetch_sub((_Atomic typeof(*(p)) *)(p), v)
#define atomic_load_relaxed(p) atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_relaxed)
#define atomic_load_acquire(p) atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_acquire)
#define atomic_store_release(p, v) atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v), memory_order_release)
#endif

#if defined(__x86_64__)
#include "arch-x86_64.h"
#else
#warning "Unknown architecture, attempting to use generic model."
#include "arch-generic.h"
#endif

#endif /* FIO_ARCH_H */
