#pragma once

#include "absl/strings/str_format.h"
#include "common/constants.h"

namespace rackobj::common {

class BlockId {
public:
    static constexpr size_t kBlockSize = SLOT_SIZE;

    BlockId() = default;

    BlockId(uint64_t server_id, ino_t inode, off_t offset) : server_id_(server_id), inode_(inode), offset_(offset) {}

    void IncrementPage() { offset_ += kBlockSize; }

    uint64_t GetServerId() const { return server_id_; }

    ino_t GetInode() const { return inode_; }

    off_t GetOffset() const { return offset_; }

    friend bool operator==(const BlockId& lhs, const BlockId& rhs) {
        return lhs.inode_ == rhs.inode_ && lhs.offset_ == rhs.offset_;
    }

    template <typename Sink>
    friend void AbslStringify(Sink& sink, const BlockId& block) {
        absl::Format(&sink, "BlockId(server=%zu, inode=%zu, offset=%u)", block.server_id_, block.inode_, block.offset_);
    }

    template <typename H>
    friend H AbslHashValue(H h, const BlockId& b) {
        return H::combine(std::move(h), b.inode_, b.offset_);
    }

private:
    uint64_t server_id_;
    ino_t inode_;
    off_t offset_;
};

}  // namespace rackobj::common