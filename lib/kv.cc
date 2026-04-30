#include "kv.h"

#include "core/blockid.h"
#include "globals.h"

namespace rackobj::lib {

using rackobj::common::BlockId;

ssize_t RackOBJKV::Get(void* buf, uint64_t count, off_t offset) {
    BlockId block_id(static_cast<uint64_t>(0), 0, offset);

    assert(count <= (size_t)SLOT_SIZE);

    size_t bytes_read = page_cache.Get(block_id, static_cast<uint8_t*>(buf), count, 0, &thread_local_meta);

    return static_cast<ssize_t>(bytes_read);
}

ssize_t RackOBJKV::Put(const void* buf, uint64_t count, off_t offset) {
    BlockId block_id(static_cast<uint64_t>(0), 0, offset);

    assert(count <= (size_t)SLOT_SIZE);

    size_t bytes_write = page_cache.Put(block_id, static_cast<const uint8_t*>(buf), count, 0, &thread_local_meta);

    return static_cast<ssize_t>(bytes_write);
}

ssize_t RackOBJKV::Preload(const void* buf, uint64_t count, off_t offset) {
    BlockId block_id(static_cast<uint64_t>(0), 0, offset);

    assert(count <= (size_t)SLOT_SIZE);

    auto result = page_cache.PreloadAdmit(block_id, static_cast<const uint8_t*>(buf), count, &thread_local_meta);

    CHECK(result || result.error() == std::make_error_code(std::errc::operation_not_permitted))
        << "abnormal exception: " << result.error();

    return 0;
}

}  // namespace rackobj::lib