#include "lcd.h"

namespace rackobj::common {

using std::construct_at;
using std::make_shared;
using std::make_unique;
using std::unique_ptr;

LocalCacheDirectory::LocalCacheDirectory(size_t num_entries, LocalMemoryByteAllocator allocator)
    : map_(KVPairAllocator(allocator)) {
    DLOG(INFO) << "Constructing LCD";
    map_.reserve(num_entries);
}

LocalCacheDirectory::~LocalCacheDirectory() { DLOG(INFO) << "Destroying GCD"; }

unique_ptr<LocalCacheDirectoryHandle> LocalCacheDirectoryHandle::Create(
    size_t num_entries, void* map_address, int numa_node, const uint8_t* base_addr,
    const std::shared_ptr<common::AllocatableLocalMemoryRegion>& local_region) {
    (void)map_address;
    (void)numa_node;
    (void)base_addr;
    LOG(INFO) << "construct local index on node " << numa_node;
    LocalMemoryAllocator<LocalCacheDirectory> alloc(local_region);

    auto* lcd = alloc.allocate();
    construct_at(lcd, num_entries, LocalMemoryByteAllocator(alloc));

    return make_unique<LocalCacheDirectoryHandle>(lcd, local_region);
}

LocalCacheDirectoryHandle::~LocalCacheDirectoryHandle() {}

std::optional<std::reference_wrapper<LCDEntry>> LocalCacheDirectoryHandle::Get(const BlockId& block_id) {
    std::optional<std::reference_wrapper<LCDEntry>> entry = std::nullopt;
    lcd_->map_.if_contains(block_id, [&entry](auto& p) { entry = std::ref(p.second); });
    return entry;
}

std::optional<ssize_t> LocalCacheDirectoryHandle::Delete(const BlockId& to_remove) {
    std::optional<ssize_t> old_wmeta_index = std::nullopt;
    lcd_->map_.erase_if(to_remove, [&](std::pair<const BlockId, LCDEntry>& row) {
        auto& entry = row.second;
        old_wmeta_index = entry.wmeta_idx_;
        return true;
    });

    return old_wmeta_index;
}

bool LocalCacheDirectoryHandle::Insert(const BlockId& to_insert, size_t cn_index, std::optional<size_t> wmeta_idx) {
    lcd_->map_.lazy_emplace_l(
        to_insert,
        [&](std::pair<const BlockId, LCDEntry>& row) {
            auto& entry = row.second;
            entry.cn_idx_ = cn_index;
            entry.wmeta_idx_ = wmeta_idx;
        },
        [&](const decltype(lcd_->map_)::constructor& ctor) {
            LCDEntry entry(cn_index);
            entry.wmeta_idx_ = wmeta_idx;
            ctor(to_insert, entry);
        });
    return true;
}

}  // namespace rackobj::common