#ifndef RACKOBJ_LOCAL_WORK_ALLOCATOR_H
#define RACKOBJ_LOCAL_WORK_ALLOCATOR_H

#define TBB_PREVIEW_MEMORY_POOL 1
#include "allocator.h"
#include "common/constants.h"
#include "region.h"
#include "tbb/cache_aligned_allocator.h"
#include "tbb/memory_pool.h"

namespace rackobj::common {

// mempool size = (queuesize * 1.1) * worksize

template <typename T>
class LocalWorkAllocator : public tbb::cache_aligned_allocator<T> {
protected:
    template <typename U>
    friend class LocalWorkAllocator;

public:
    using value_type = T;

    explicit LocalWorkAllocator(const std::shared_ptr<AllocatableLocalMemoryRegion> &region) noexcept
        : tbb::cache_aligned_allocator<T>(), allocator_(region) {
        size_t bufsize = (REPL_WQ_SIZE + ReserveSize(REPL_WQ_SIZE)) * sizeof(value_type);
        T *buffer = allocator_.allocate(bufsize);
        work_pool_ = std::make_shared<tbb::fixed_pool>(buffer, bufsize);
    }

    ~LocalWorkAllocator() {
        // work_pool_->recycle();
    }

    template <typename U>
    LocalWorkAllocator(const LocalWorkAllocator<U> &other)
        : allocator_(other.allocator_), work_pool_(other.work_pool_) {}

    template <typename U>
    LocalWorkAllocator(LocalWorkAllocator<U> &&other)
        : allocator_(std::move(other.allocator_)), work_pool_(std::move(other.work_pool_)) {}

    [[nodiscard]] T *allocate(std::size_t n) {
        void *p = work_pool_->malloc(n * sizeof(value_type));
        if (!p) {
            tbb::detail::d1::throw_exception(std::bad_alloc());
        }
        return reinterpret_cast<T *>(p);
        // return allocator_.allocate(n * sizeof(value_type));
    }

    //! Free block of memory that starts on a cache line
    void deallocate(T *const p, std::size_t n) const noexcept {
        (void)n;
        work_pool_->free(p);
        // allocator_.deallocate(p, n);
    }

private:
    LocalMemoryAllocator<T> allocator_;
    std::shared_ptr<tbb::fixed_pool> work_pool_;

    static constexpr size_t ReserveSize(size_t elements) {
        return static_cast<size_t>(static_cast<double>(elements) * 0.1);
    }
};

}  // namespace rackobj::common

#endif  // RACKOBJ_LOCAL_WORK_ALLOCATOR_H
