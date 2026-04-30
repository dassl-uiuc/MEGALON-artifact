#include "helper.h"

#include <emmintrin.h>
#include <immintrin.h>
#include <numa.h>
#include <pthread.h>
#include <sched.h>

#include <array>
#include <iostream>
#include <limits>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "constants.h"

namespace rackobj {

void pinThreadtoNumaNode(int target_node) {
    if (target_node < 0 || target_node > numa_max_node()) {
        LOG(ERROR) << "invalid numa node";
        return;
    }

    struct bitmask* cpus = numa_allocate_cpumask();
    numa_node_to_cpus(target_node, cpus);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (unsigned int i = 0; i < (unsigned int)cpus->size; ++i) {
        if (numa_bitmask_isbitset(cpus, i)) {
            CPU_SET(i, &cpuset);
        }
    }
    numa_free_cpumask(cpus);

    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    uint32_t numa_node = UINT32_MAX;
    int rc_getcpu = getcpu(nullptr, &numa_node);
    CHECK(rc_getcpu != -1) << "getcpu() failed";
    CHECK(numa_node == (uint32_t)target_node) << "not running on target node: " << numa_node;
}

bool verifyThreadNuma(int numa_node) {
    cpu_set_t check_cpuset;
    pthread_t thread = pthread_self();
    CPU_ZERO(&check_cpuset);
    if (pthread_getaffinity_np(thread, sizeof(cpu_set_t), &check_cpuset) != 0) {
        LOG(ERROR) << "pthread_getaffinity_np failed";
        return false;
    }

    for (unsigned int i = 0; i < CPU_SETSIZE; ++i) {
        if ((numa_node == (int)i && !CPU_ISSET(i, &check_cpuset)) ||
            (numa_node != (int)i && CPU_ISSET(i, &check_cpuset)))
            return false;
    }
    return true;
}

void pinProcesstoNumaNode(int numa_node) {
    if (numa_node < 0 || numa_node > numa_max_node()) {
        LOG(ERROR) << "invalid numa node";
        return;
    }

    // Get the bitmask for the CPUs of the specified NUMA node
    struct bitmask* cpumask = numa_allocate_cpumask();
    numa_node_to_cpus(numa_node, cpumask);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (unsigned int i = 0; i < cpumask->size; ++i) {
        if (numa_bitmask_isbitset(cpumask, i)) {
            CPU_SET(i, &cpuset);
        }
    }
    numa_free_cpumask(cpumask);

    pid_t pid = getpid();
    if (sched_setaffinity(pid, sizeof(cpu_set_t), &cpuset) != 0) {
        LOG(ERROR) << "Failed to set process affinity";
        return;
    }

    LOG(INFO) << "Process pinned to NUMA node " << numa_node;
}

uint32_t GetCurrentNuma() {
    uint32_t numa_node = UINT32_MAX;
    int rc = getcpu(nullptr, &numa_node);
    CHECK(rc != -1) << "getcpu() failed";
    return numa_node;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("clflushopt")))
#endif
void cache_flush(char* addr, size_t size) {
#if defined(SCR) || defined(NO_COHERENCE)
    size_t aligned_size =
        (size + CACHE_LINE_SIZE - 1) & ~static_cast<size_t>(CACHE_LINE_SIZE - 1);  // Round up to nearest cache line
    _mm_sfence();  // prevent writes from ordered after this
    for (size_t offset = 0; offset < aligned_size; offset += CACHE_LINE_SIZE) {
        _mm_clflushopt(addr + offset);
    }
    _mm_mfence();  // prevent clflush from getting reordred after this
#endif
}

size_t convert_mem_size(std::string input) {
    // Remove leading/trailing whitespace
    input.erase(0, input.find_first_not_of(" \t\n\r"));
    input.erase(input.find_last_not_of(" \t\n\r") + 1);

    if (input.empty()) return 0;

    // Find where the number ends and the unit begins
    size_t idx = 0;
    while (idx < input.size() && isdigit(input[idx])) ++idx;

    if (idx == 0) return 0;  // No number found

    size_t number = std::stoull(input.substr(0, idx));
    std::string unit = input.substr(idx);

    // Remove whitespace from unit
    unit.erase(0, unit.find_first_not_of(" \t\n\r"));
    unit.erase(unit.find_last_not_of(" \t\n\r") + 1);

    size_t multiplier = 1;
    if (unit.empty() || unit == "B" || unit == "b") {
        multiplier = 1;
    } else if (unit == "K" || unit == "KB" || unit == "k" || unit == "kb") {
        multiplier = 1024ULL;
    } else if (unit == "M" || unit == "MB" || unit == "m" || unit == "mb") {
        multiplier = 1024ULL * 1024ULL;
    } else if (unit == "G" || unit == "GB" || unit == "g" || unit == "gb") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else if (unit == "T" || unit == "TB" || unit == "t" || unit == "tb") {
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    } else {
        // Unknown unit, treat as bytes
        multiplier = 1;
    }

    return number * multiplier;
}

// Build NUMA_EXEC equivalent: all node indices except NUMA_MEM
// Static one-time initialization since it only depends on constants
static const std::vector<int>& GetExecNodes() {
    static const std::vector<int> exec_nodes = []() {
        std::vector<int> nodes;
        const int kNumExec = NUM_NUMA - 1;  // equals Rust NUM_EXEC
        nodes.reserve(kNumExec);
        for (int candidate = 0; static_cast<int>(nodes.size()) < kNumExec; ++candidate) {
            if (candidate == NUMA_MEM) continue;
            nodes.push_back(candidate);
        }
        return nodes;
    }();
    return exec_nodes;
}

int RidToNumaNode(int rid) {
    const std::vector<int>& exec_nodes = GetExecNodes();
    const int kNumExec = static_cast<int>(exec_nodes.size());
    int exec_idx = rid % kNumExec;
    return exec_nodes[static_cast<size_t>(exec_idx)];
}

std::vector<int> NumaNodeRids(int numa_node) {
    std::vector<int> result;
    const std::vector<int>& exec_nodes = GetExecNodes();
    const int kNumExec = static_cast<int>(exec_nodes.size());

    int exec_idx = -1;
    for (int i = 0; i < kNumExec; ++i) {
        if (exec_nodes[static_cast<size_t>(i)] == numa_node) {
            exec_idx = i;
            break;
        }
    }
    if (exec_idx < 0) return result;

    for (int rid = 0; rid < LOGICAL_NODE_NUM; ++rid) {
        if (rid % kNumExec == exec_idx) result.push_back(rid);
    }
    return result;
}

int TidToRid(int tid) {
    // simple round-robin assignment of thread to logical nodes
    return tid % LOGICAL_NODE_NUM;
}

}  // namespace rackobj