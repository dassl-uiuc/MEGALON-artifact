#pragma once

#include <pthread.h>

namespace rackobj::common {

class Spinlock {
public:
    constexpr Spinlock() noexcept { pthread_spin_init(&lock_, 0); }

    ~Spinlock() { pthread_spin_destroy(&lock_); }

    void Lock() noexcept(true) { pthread_spin_lock(&lock_); }

    bool TryLock() noexcept(true) { return pthread_spin_trylock(&lock_) == 0; }

    void Unlock() noexcept(true) { pthread_spin_unlock(&lock_); }

private:
    pthread_spinlock_t lock_;
};

}  // namespace rackobj::common