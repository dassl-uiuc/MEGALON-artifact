#pragma once

#include <cstring>

#include "core/object_slot.h"

#ifdef NR
#include "index/nr_hashmap.h"
#endif  // NR

namespace rackobj {

typedef struct pthread_stats {
    uint64_t read_retry_cnt;         // number of time reads have retried (single read call can have multiple retries)
    uint64_t read_retry_invocation;  // number of read function invocations that had at least one retry
    uint64_t read_cnt;
    uint64_t admit_cnt;
    uint64_t remote_partition_miss_cnt;
    uint64_t remote_partition_hit_cnt;
    uint64_t remote_partition_access_cnt;
    uint64_t owned_partition_hit_cnt;
    uint64_t owned_partition_miss_cnt;
    uint64_t owned_partition_access_cnt;
} pthread_stats_t;

template <typename Policy>
class ThreadLocalMeta {
public:
#ifdef NR
    ThreadLocalMeta() : nr_meta_per_thread_(nullptr), local_cache_ptr_(nullptr) {
        memset(&pt_stat, 0, sizeof(pt_stat));
    }

    const NrFfi::NrMeta* nr_meta_per_thread_;
#else
    ThreadLocalMeta() : local_cache_ptr_(nullptr) { memset(&pt_stat, 0, sizeof(pt_stat)); }
#endif
    bool file_backed_ = true;
    common::LocalMemoryObject<Policy>* local_cache_ptr_;
    int logical_node_id_;
    pthread_stats_t pt_stat;
};

}  // namespace rackobj
