#pragma once

#include <optional>

#include "absl/log/log.h"
#include "blockid.h"
#include "common/constants.h"
#include "common/types.h"
#include "seqcount.h"

namespace rackobj::lib {

template <typename Policy>
class SharedMemoryObjectHandle;

}  // namespace rackobj::lib

namespace rackobj::common {

class CacheNode {
    friend class SharedMemoryObject;

    template <typename Policy>
    friend class lib::SharedMemoryObjectHandle;

    friend class FlushManager;
    friend class WriteMetadataManager;

public:
    explicit CacheNode() : block_id_(), length_(0), evict_cnt_(1), active_node_(0) {}

    virtual ~CacheNode() {}

    CacheNode(const CacheNode&) = delete;
    CacheNode& operator=(const CacheNode&) = delete;
    CacheNode(CacheNode&&) = delete;
    CacheNode& operator=(CacheNode&&) = delete;

    void Reinitialize(const BlockId& new_block_id) {
        std::construct_at(&block_id_, new_block_id);
        // SetLength(0);
    }

    const BlockId& GetBlockId() const { return block_id_; }

    // uint64_t GetCachedServer() const { return cached_server_id_; }

    size_t GetLength() const { return static_cast<size_t>(length_.load(std::memory_order_acquire)); }

    void SetLength(size_t len) { length_.store(static_cast<uint16_t>(len), std::memory_order_release); }

    // void SetCachedServer(uint64_t server_id) { cached_server_id_ = server_id; }

    bool ShouldEvict() {
        int64_t count;

        count = evict_cnt_.load(std::memory_order_relaxed);
        if (count >= 4) {
            DLOG(INFO) << "cn evict_cnt: " << static_cast<uint32_t>(count);
            return true;
        }

        while (!evict_cnt_.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel)) {
            if (count >= 4) {
                DLOG(INFO) << "cn evict_cnt: " << static_cast<uint32_t>(count);
                return true;
            }
        }
        return count + 1 == 4;
    }

    bool DecreaseEvict() {
        int64_t count;

        count = evict_cnt_.load(std::memory_order_relaxed);
        if (count <= 0) {
            DLOG(INFO) << "cn evict_cnt: " << static_cast<int64_t>(count);
            return false;
        }

        while (!evict_cnt_.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel)) {
            if (count <= 0) {
                DLOG(INFO) << "cn evict_cnt: " << static_cast<int64_t>(count);
                return false;
            }
        }
        return true;
    }

    void ClearEvict() {
        int64_t count;

        count = evict_cnt_.load(std::memory_order_relaxed);
        while (!evict_cnt_.compare_exchange_weak(count, 0, std::memory_order_acq_rel))
            ;
    }

    uint8_t GetActiveNode() const { return active_node_.load(std::memory_order_relaxed); }

    void SetActiveNode(uint8_t node) { active_node_.store(node, std::memory_order_relaxed); }

protected:
    BlockId block_id_;

    // The amount of data the page has filled in this block. The most the page
    // can fill is 4096 bytes. This field is meant for edge cases such as that
    // of the last block in a file that might not be completely filled. We want
    // to be aware of this fact so we do not accidentally increase the backing
    // file's length when writing back. Since the max length is 4096, we really
    // only need 12 bits from this field, so a future optimization could be to
    // include any flags in the most significant 4 bits of the field.
    std::atomic<uint16_t> length_;

    std::atomic<int64_t> evict_cnt_;

    std::atomic<uint8_t> active_node_;
};

typedef std::function<void(const CacheNode&)> ConstCacheNodeCallback;
typedef std::function<void(CacheNode&)> CacheNodeCallback;

// Read only page

typedef struct CNStatus {
    std::optional<size_t> cn_idx_;
    bool invalidate_;
} CNStatus_t;

/* assumes the first index is cxl metadata, the remaining is logical node metadata */
typedef struct GCDEntry_t {
    std::optional<size_t> wmeta_idx_;
    CNStatus_t cn_array_[LOGICAL_NODE_NUM + 1];
} GCDEntry;

static inline bool isEqual(const std::optional<GCDEntry>& lhs, const std::optional<GCDEntry>& rhs) {
    if (!lhs.has_value() && !rhs.has_value()) {
        return true;
    }
    if (!lhs.has_value() || !rhs.has_value()) {
        return false;
    }

    const GCDEntry& l = *lhs;
    const GCDEntry& r = *rhs;

    if (l.wmeta_idx_ != r.wmeta_idx_) {
        return false;
    }

    for (size_t i = 0; i < LOGICAL_NODE_NUM + 1; ++i) {
        const std::optional<CNStatus_t>& l_cn = l.cn_array_[i];
        const std::optional<CNStatus_t>& r_cn = r.cn_array_[i];

        if (!l_cn.has_value() && !r_cn.has_value()) {
            continue;
        }

        if (!l_cn.has_value() || !r_cn.has_value()) {
            return false;
        }

        const CNStatus_t& l_status = *l_cn;
        const CNStatus_t& r_status = *r_cn;

        if (l_status.cn_idx_ != r_status.cn_idx_) {
            return false;
        }
        if (l_status.invalidate_ != r_status.invalidate_) {
            return false;
        }
    }

    return true;
}

static inline void print_entry(common::GCDEntry* entry) {
    LOG(INFO)
        << "{("
        << (entry->cn_array_[0].cn_idx_.has_value() ? std::to_string(entry->cn_array_[0].cn_idx_.value()) : "null")
        << ", " << entry->cn_array_[0].invalidate_ << "), ("
        << (entry->cn_array_[1].cn_idx_.has_value() ? std::to_string(entry->cn_array_[1].cn_idx_.value()) : "null")
        << ", " << entry->cn_array_[1].invalidate_ << "), ("
        << (entry->cn_array_[2].cn_idx_.has_value() ? std::to_string(entry->cn_array_[2].cn_idx_.value()) : "null")
        << ", " << entry->cn_array_[2].invalidate_ << "), ("
        << (entry->cn_array_[3].cn_idx_.has_value() ? std::to_string(entry->cn_array_[3].cn_idx_.value()) : "null")
        << ", " << entry->cn_array_[3].invalidate_ << ")}" << std::endl;
}

}  // namespace rackobj::common
