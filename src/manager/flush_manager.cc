#include "flush_manager.h"

#include <cstring>

#include "absl/log/log.h"
#include "common/constants.h"
#include "core/object_slot.h"
#include "core/write_meta.h"
#include "original_syscalls.h"
#include "shm_obj_handle.h"

namespace rackobj::common {

FlushManager::FlushManager(int run_node, int target_node, int shared_cache_nid) noexcept
    : exec_node_(run_node),
      target_node_(target_node),
      shared_cache_node_(shared_cache_nid),
      curr_flush_idx_(0),
      watermark_ratio_(FLUSH_WATERMARK),
      threshold_(0) {}

FlushManager::~FlushManager() noexcept {}

void FlushManager::Run() {
    worker_ = std::jthread(std::bind(&FlushManager::work_fn, this, std::placeholders::_1));
    LOG(INFO) << "FlushManager for node " << target_node_ << " starts on node " << exec_node_;
}

void FlushManager::Shutdown() {
    worker_.request_stop();
    LOG(INFO) << "FlushManager for node " << target_node_ << " on node " << exec_node_ << " stop requested";
    worker_.join();
    LOG(INFO) << "FlushManager for node " << target_node_ << " on node " << exec_node_ << " joined";
}

void FlushManager::work_fn(std::stop_token stoken) {
    pinThreadtoNumaNode(exec_node_);
    std::chrono::nanoseconds sleep_duration(FLUSH_MGR_SLEEP_INTERVAL_NS);

#ifdef NR
    const NrFfi::NrMeta *nr_meta = c3po_->Gcd()->GetNrMetaTid(true);
    if (!nr_meta) {
        LOG(FATAL) << "Flusher thread for node " << target_node_ << " on node " << exec_node_
                   << " registration failed!!";
        return;
    }
#endif

    while (!stoken.stop_requested()) {
        DoFlush(
#ifdef NR
            nr_meta
#endif
        );

        std::this_thread::sleep_for(sleep_duration);
    }
}

/**
 * flush shared page
 * procedure:
 * copy page atomically to local buffer with read sequence
 * flush to disk
 * lock page
 * recheck sequence #: if valid, set clean
 * unlock page
 *
 * recheck seqence # guarantee that no write happened in between flush and set clean
 */
size_t FlushManager::DoFlushShared(
#ifdef NR
    const NrFfi::NrMeta *nr_meta
#endif
) {
    SharedMemoryObject *shared_cache = static_cast<SharedMemoryObject *>(page_cache_.get());
    CacheNode *cur;
    size_t cn_index;
    size_t max_elements = shared_cache->page_count_;
    ssize_t flush_over_thres = NrToFlush();
    std::optional<common::GCDEntry> entry_optional;
    struct ReadHandle rh;

    DLOG(INFO) << "before flush over_thres: " << flush_over_thres;

    for (size_t i = 0; i < max_elements; i++) {
        if (flush_over_thres <= 0) {
            break;
        }

        cn_index = (curr_flush_idx_ + i) % max_elements;
        cur = shared_cache->cache_slot_.GetCacheNode(cn_index);
#ifndef FULL_COHERENCE
        c3po_->cache_flush(reinterpret_cast<char *>(&(cur->block_id_)), sizeof(BlockId));
#endif
        const BlockId block_id = cur->block_id_;

        if (shared_cache->scr_bitmap_.IsDirty(cn_index)) {
            // use the read sequence to ensure correctness

            entry_optional = c3po_->Gcd()->Get(block_id
#ifdef NR
                                               ,
                                               nr_meta
#endif /* NR */
            );

            CHECK(entry_optional.has_value());
            init_read_handle(rh, block_id, static_cast<uint8_t>(exec_node_));
            update_read_handle(rh, block_id, entry_optional);
            rh.from_cxl = true;

            if (!c3po_->flush_seq_start(rh)) {
                // read sequence fail
                continue;
            }

            // actual mem copy
            memcpy(buf_, shared_cache->page_data_.GetPage(cn_index), static_cast<size_t>(SLOT_SIZE));

            if (!c3po_->flush_seq_end(rh)) {
                // read sequence fail
                continue;
            }

            // flush to disk
            lib::original_syscalls.pwrite(static_cast<int>(block_id.GetServerId()), buf_,
                                          static_cast<size_t>(SLOT_SIZE), block_id.GetOffset());

            // lock; check and set dirty
            common::WriteMetadata *wmeta = c3po_->GetWmeta(entry_optional.value().wmeta_idx_.value());
            if (wmeta->WLockOnly()) {
                if (c3po_->flush_seq_end(rh)) {
                    // read sequence succeed, clear dirty
                    shared_cache->scr_bitmap_.SetDirty(cn_index, false);
                    // updated policy: iteration starts from last index that is switched to RO
                    curr_flush_idx_ = (cn_index + 1) % max_elements;
                }
                wmeta->WUnlockOnly();
            }
        }
    }

    DLOG(INFO) << "after flush wmeta_over_thres: " << flush_over_thres;

    return 0;
}

size_t FlushManager::DoFlushLocal() { return 0; }

}  // namespace rackobj::common
