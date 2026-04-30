#include "pmeta_manager.h"

#include <cstring>

#include "absl/log/log.h"
#include "common/constants.h"
#include "core/c3.h"
#include "core/object_slot.h"
#include "core/write_meta.h"
#include "original_syscalls.h"
#include "shm_obj_handle.h"

namespace rackobj::common {

PartitionMetadataManager::PartitionMetadataManager(C3POHandle *c3po, std::vector<int> run_nodes,
                                                   std::vector<int> target_partitions, int shared_cache_nid) noexcept
    : exec_nodes_(run_nodes),
      target_partitions_(target_partitions),
      shared_cache_node_(shared_cache_nid),
      curr_switch_idx_(0),
      c3po_(c3po) {}

PartitionMetadataManager::~PartitionMetadataManager() noexcept {}

void PartitionMetadataManager::Run() {
    CHECK(exec_nodes_.size() == target_partitions_.size())
        << "exec_nodes_ size: " << exec_nodes_.size() << " != target_partitions_ size: " << target_partitions_.size();

    LOG(ERROR) << "not implemented";

    // for (int i = 0; i < exec_nodes_.size(); ++i) {
    //     workers_.push_back(std::jthread(std::bind(&PartitionMetadataManager::work_fn, this, std::placeholders::_1)));
    // }
    // LOG(INFO) << "PartitionMetadataManager for node " << shared_cache_node_ << " starts on node " << exec_node_;
}

void PartitionMetadataManager::Shutdown() {
    LOG(ERROR) << "not implemented";
    LOG(INFO) << "PartitionMetadataManager for node " << shared_cache_node_ << " joined";
}

void PartitionMetadataManager::work_fn(std::stop_token stoken, int exec_node, int target_partition) {
    pinThreadtoNumaNode(exec_node);
    std::chrono::nanoseconds sleep_duration(WMETA_MGR_SLEEP_INTERVAL_NS);

    while (!stoken.stop_requested()) {
        DoReclaim(target_partition);

        std::this_thread::sleep_for(sleep_duration);
    }
}

size_t PartitionMetadataManager::SampleReclaim(size_t count) {
    (void)count;
    LOG(ERROR) << "not implemented";
    return 0;
}

/**
 * reclaim occupied wmeta
 */
size_t PartitionMetadataManager::DoReclaim(int target_partition) {
    (void)target_partition;
    size_t wmeta_over_thres = c3po_->WmetaOverThreshold();

    DLOG(INFO) << "before reclaim wmeta_over_thres: " << wmeta_over_thres;

    wmeta_over_thres -= SampleReclaim(wmeta_over_thres);

    DLOG(INFO) << "after reclaim wmeta_over_thres: " << wmeta_over_thres;

    return wmeta_over_thres;
}

}  // namespace rackobj::common
