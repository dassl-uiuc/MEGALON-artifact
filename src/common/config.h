#pragma once

#include <pthread.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace rackobj {

class RackOBJConfig {
public:
    RackOBJConfig();

    const std::filesystem::path& GetMountDirectory() const { return mount_directory_; }

    uint64_t GetServerId() const { return server_id_; }

    uint64_t GetServerMemNuma() const { return shm_numa_node_; }

    uint64_t GetServerExecNuma() const { return exec_numa_node_; }

    size_t GetNumSlots() const { return num_slots_; }

    size_t GetSCRSize() const { return scr_size_; }

    size_t GetLogicalSCRSize() const { return logical_scr_size_; }

    size_t GetNCRSize() const { return ncr_size_; }

    size_t GetLocalSize() const { return local_size_; }

    size_t GetKeySpace() const { return key_space_; }

    bool DoReplication() const { return replicate_; }

    bool DoFlush() const { return flush_; }

    bool DoEviction() const { return evict_; }

    bool GetMoveToShare() const { return move_to_share_; }

    bool IsPathExcluded(const std::filesystem::path& path) const;

private:
    uint64_t server_id_;
    uint64_t shm_numa_node_;
    uint64_t exec_numa_node_;

    size_t num_slots_;
    size_t scr_size_;
    size_t logical_scr_size_;
    size_t ncr_size_;
    size_t local_size_;
    size_t key_space_;

    /* bg threads */
    bool replicate_;  // for local replication
    bool flush_;      // for flushing to disk (for page cache application)
    bool evict_;      // for bg eviction of slot elements
    bool move_to_share_;

    std::filesystem::path mount_directory_;
    std::vector<std::filesystem::path> excluded_files_;
    std::vector<std::string> excluded_directories_;
};

}  // namespace rackobj