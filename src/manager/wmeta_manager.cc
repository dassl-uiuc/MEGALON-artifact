#include "wmeta_manager.h"

#include <cstring>

#include "absl/log/log.h"
#include "common/constants.h"
#include "core/c3.h"
#include "core/object_slot.h"
#include "core/write_meta.h"
#include "original_syscalls.h"
#include "shm_obj_handle.h"

namespace rackobj::common {

WriteMetadataManager::WriteMetadataManager(C3POHandle *c3po, int run_node, int shared_cache_nid) noexcept
    : exec_node_(run_node), shared_cache_node_(shared_cache_nid), curr_switch_idx_(0), c3po_(c3po) {}

WriteMetadataManager::~WriteMetadataManager() noexcept {}

void WriteMetadataManager::Run() {
    worker_ = std::jthread(std::bind(&WriteMetadataManager::work_fn, this, std::placeholders::_1));
    LOG(INFO) << "WriteMetadataManager for node " << shared_cache_node_ << " starts on node " << exec_node_;
}

void WriteMetadataManager::Shutdown() {
    worker_.request_stop();
    // LOG(INFO) << "WriteMetadataManager for node " << shared_cache_node_ << " on node " << exec_node_
    //           << " stop requested";
    worker_.join();
    LOG(INFO) << "WriteMetadataManager for node " << shared_cache_node_ << " on node " << exec_node_ << " joined";
}

void WriteMetadataManager::work_fn(std::stop_token stoken) {
    pinThreadtoNumaNode(exec_node_);
    std::chrono::nanoseconds sleep_duration(WMETA_MGR_SLEEP_INTERVAL_NS);

#ifdef NR
    const NrFfi::NrMeta *nr_meta = c3po_->Gcd()->GetNrMetaTid(true);
    if (!nr_meta) {
        LOG(FATAL) << "RO switching thread for node " << shared_cache_node_ << " on node " << exec_node_
                   << " registration failed!!";
        return;
    }
#endif

    while (!stoken.stop_requested()) {
        DoReclaim(
#ifdef NR
            nr_meta
#endif
        );

        std::this_thread::sleep_for(sleep_duration);
    }
}

size_t WriteMetadataManager::IterativeReclaim(size_t count
#ifdef NR
                                              ,
                                              const NrFfi::NrMeta *nr_meta
#endif
) {
    WriteMetadata *cur;
    size_t wmeta_idx;
    size_t num_wmeta_slot = c3po_->Scr_meta()->GetWmetaSlotLen();
    size_t wmeta_over_thres = count;
    for (size_t i = 0; i < num_wmeta_slot; i++) {
        if (wmeta_over_thres <= 0) {
            break;
        }

        wmeta_idx = (curr_switch_idx_ + i) % num_wmeta_slot;

        cur = c3po_->Scr_meta()->GetWmeta(wmeta_idx);
        if (!cur->WLockOnly()) continue;

        BlockId block_id = cur->GetBlockID();

        // switch back to RO
        std::optional<size_t> wmeta_idx_optional = c3po_->Gcd()->SwitchToReadOnly(block_id
#ifdef NR
                                                                                  ,
                                                                                  nr_meta
#endif /* NR */
        );
        if (wmeta_idx_optional.has_value()) {
            // CHECK(!shared_cache->scr_bitmap_.IsWmetaFree(wmeta_idx)) << "the slot is already free before freeing";
            // CHECK(wmeta_idx == wmeta_idx_optional.value())
            //     << "wmeta idx and optional idx doesnot match: " << wmeta_idx << ":" << wmeta_idx_optional.value();
            c3po_->Scr_meta()->RecycleWmeta(wmeta_idx_optional.value(), 0);  // placeholder
            wmeta_over_thres--;
            // updated policy: iteration starts from last index that is switched to RO
            curr_switch_idx_ = (wmeta_idx + 1) % num_wmeta_slot;
        } else {
            cur->WUnlockOnly();
        }
    }
    return count - wmeta_over_thres;
}

size_t WriteMetadataManager::SampleReclaim(size_t count
#ifdef NR
                                           ,
                                           const NrFfi::NrMeta *nr_meta
#endif
) {
    WriteMetadata *cur;
    std::optional<size_t> wmeta_idx;
    size_t wmeta_over_thres = count;

    while (wmeta_over_thres > 0) {
        wmeta_idx = c3po_->Scr_meta()->SampleVictim(MAX_WMETA_SAMPLING_SIZE, 0);  // placeholder

        if (!wmeta_idx.has_value()) continue;

        cur = c3po_->Scr_meta()->GetWmeta(wmeta_idx.value());
        BlockId block_id = cur->GetBlockID();

        std::optional<size_t> wmeta_idx_optional = c3po_->Gcd()->SwitchToReadOnly(block_id
#ifdef NR
                                                                                  ,
                                                                                  nr_meta
#endif /* NR */
        );
        if (wmeta_idx_optional.has_value()) {
            c3po_->Scr_meta()->RecycleWmeta(wmeta_idx_optional.value(), 0);  // placeholder
            wmeta_over_thres--;
        } else {
            cur->WUnlockOnly();
        }
    }
    return count - wmeta_over_thres;
}

/**
 * reclaim occupied wmeta
 */
size_t WriteMetadataManager::DoReclaim(
#ifdef NR
    const NrFfi::NrMeta *nr_meta
#endif
) {
    // SharedMemoryObject *shared_cache = static_cast<SharedMemoryObject *>(page_cache_.get());
    size_t wmeta_over_thres = c3po_->WmetaOverThreshold();

    DLOG(INFO) << "before reclaim wmeta_over_thres: " << wmeta_over_thres;

#ifdef SAMPLING
    wmeta_over_thres -= SampleReclaim(wmeta_over_thres
#ifdef NR
                                      ,
                                      nr_meta
#endif
    );
#else
    wmeta_over_thres -= IterativeReclaim(wmeta_over_thres
#ifdef NR
                                         ,
                                         nr_meta
#endif
    );
#endif

    DLOG(INFO) << "after reclaim wmeta_over_thres: " << wmeta_over_thres;

    return wmeta_over_thres;
}

}  // namespace rackobj::common
