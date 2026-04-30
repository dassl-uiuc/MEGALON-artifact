#pragma once

#include <cstring>

#include "absl/log/log.h"
#include "absl/strings/str_format.h"
#include "common/constants.h"

namespace rackobj::common {

class __attribute__((packed)) BlockId {
public:
    static constexpr size_t kBlockSize = SLOT_SIZE;
    static constexpr size_t kKeySize = KEY_SIZE;
    static constexpr size_t kOffsetSize = sizeof(uint64_t);
    static_assert(kOffsetSize <= kKeySize, "KEY_SIZE too small for offset field");

    BlockId() = default;

    explicit BlockId(off_t offset) : offset_(static_cast<uint64_t>(offset)) {
        std::memset(padding_, 0, sizeof(padding_));
    }

    BlockId(uint64_t server_id, ino_t inode, off_t offset) : offset_(static_cast<uint64_t>(offset)) {
        (void)server_id;
        (void)inode;
        std::memset(padding_, 0, sizeof(padding_));
    }

    void IncrementPage() { LOG(FATAL) << "not implemented"; }

    off_t GetOffset() const { return static_cast<off_t>(offset_); }

    uint64_t GetServerId() const { LOG(FATAL) << "not implemented"; }

    ino_t GetInode() const { LOG(FATAL) << "not implemented"; }

    friend bool operator==(const BlockId& lhs, const BlockId& rhs) { return lhs.offset_ == rhs.offset_; }

    template <typename H>
    friend H AbslHashValue(H h, const BlockId& b) {
        return H::combine(std::move(h), b.offset_);
    }

    template <typename Sink>
    friend void AbslStringify(Sink& sink, const BlockId& b) {
        absl::Format(&sink, "BlockId(offset=%lld, keysize=%zu)", (long long)b.offset_, kKeySize);
    }

private:
    uint64_t offset_;
    uint8_t padding_[kKeySize - kOffsetSize];
};

static_assert(sizeof(BlockId) == KEY_SIZE);

}  // namespace rackobj::common