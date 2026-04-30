#include "scr_bitmap.h"

namespace rackobj::common {

SharedBitmap::SharedBitmap(size_t num_pages, size_t wmeta_threshold,
                           const std::shared_ptr<AllocatableLocalMemoryRegion> &sc_shm_region)
    :  //    page_count_(num_pages),
      wlock_bitmap_start_addr_(nullptr),
      dirty_bitmap_start_addr_(nullptr),
      free_bitmap_start_addr_(nullptr),
      wmeta_bitmap_start_addr_(nullptr),
      num_pages_(num_pages),
      count_(0),
      wmeta_count_(0),
      dirty_count_(0)
/*wmeta_count_threshold_(0)*/
{
    (void)wmeta_threshold;
    bitmap_allocator_ = std::make_unique<BitmapAllocator>(sc_shm_region);
    wlock_bitmap_start_addr_ =
        bitmap_allocator_->AllocateAligned<MemoryAlignment::kCacheAlign>(BITS_TO_LONGS(num_pages));
    dirty_bitmap_start_addr_ =
        bitmap_allocator_->AllocateAligned<MemoryAlignment::kCacheAlign>(BITS_TO_LONGS(num_pages));
    free_bitmap_start_addr_ =
        bitmap_allocator_->AllocateAligned<MemoryAlignment::kCacheAlign>(BITS_TO_LONGS(num_pages));
    wmeta_bitmap_start_addr_ =
        bitmap_allocator_->AllocateAligned<MemoryAlignment::kCacheAlign>(BITS_TO_LONGS(num_pages));

    memset(wlock_bitmap_start_addr_, 0, BITS_TO_LONGS(num_pages) * sizeof(uint64_t));
    memset(dirty_bitmap_start_addr_, 0, BITS_TO_LONGS(num_pages) * sizeof(uint64_t));
    memset(free_bitmap_start_addr_, UCHAR_MAX, BITS_TO_LONGS(num_pages) * sizeof(uint64_t));
    memset(wmeta_bitmap_start_addr_, UCHAR_MAX, BITS_TO_LONGS(num_pages) * sizeof(uint64_t));

    LOG(INFO) << "&wlock_bitmap_=@" << (void *)&wlock_bitmap_start_addr_ << ", &dirty_bitmap_=@"
              << (void *)&dirty_bitmap_start_addr_ << ", &free_bitmap_=@" << (void *)&free_bitmap_start_addr_
              << ", &wmeta_bitmap_start_addr_=@" << (void *)&wmeta_bitmap_start_addr_;
}

SharedBitmap::SharedBitmap(size_t num_pages, const std::shared_ptr<AllocatableLocalMemoryRegion> &sc_shm_region)
    :  //    page_count_(num_pages),
      wlock_bitmap_start_addr_(nullptr),
      dirty_bitmap_start_addr_(nullptr),
      free_bitmap_start_addr_(nullptr),
      wmeta_bitmap_start_addr_(nullptr),
      num_pages_(num_pages),
      count_(0),
      wmeta_count_(0),
      dirty_count_(0)
/*wmeta_count_threshold_(0)*/
{
    bitmap_allocator_ = std::make_unique<BitmapAllocator>(sc_shm_region);
    wlock_bitmap_start_addr_ =
        bitmap_allocator_->AllocateAligned<MemoryAlignment::kCacheAlign>(BITS_TO_LONGS(num_pages));
    dirty_bitmap_start_addr_ =
        bitmap_allocator_->AllocateAligned<MemoryAlignment::kCacheAlign>(BITS_TO_LONGS(num_pages));
    free_bitmap_start_addr_ =
        bitmap_allocator_->AllocateAligned<MemoryAlignment::kCacheAlign>(BITS_TO_LONGS(num_pages));
    wmeta_bitmap_start_addr_ =
        bitmap_allocator_->AllocateAligned<MemoryAlignment::kCacheAlign>(BITS_TO_LONGS(num_pages));

    memset(wlock_bitmap_start_addr_, 0, BITS_TO_LONGS(num_pages) * sizeof(uint64_t));
    memset(dirty_bitmap_start_addr_, 0, BITS_TO_LONGS(num_pages) * sizeof(uint64_t));
    memset(free_bitmap_start_addr_, UCHAR_MAX, BITS_TO_LONGS(num_pages) * sizeof(uint64_t));
    memset(wmeta_bitmap_start_addr_, UCHAR_MAX, BITS_TO_LONGS(num_pages) * sizeof(uint64_t));

    DLOG(INFO) << "&wlock_bitmap_=@" << (void *)&wlock_bitmap_start_addr_ << ", &dirty_bitmap_=@"
               << (void *)&dirty_bitmap_start_addr_ << ", &free_bitmap_=@" << (void *)&free_bitmap_start_addr_
               << ", &wmeta_bitmap_start_addr_=@" << (void *)&wmeta_bitmap_start_addr_;
}

SharedBitmap::~SharedBitmap() { DLOG(INFO) << "cache slot elem number: " << GetCount() << " out of " << num_pages_; }

thread_local std::minstd_rand SharedBitmap::rng;

}  // namespace rackobj::common