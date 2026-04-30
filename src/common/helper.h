#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace rackobj {

void pinThreadtoNumaNode(int numa_node);
bool verifyThreadNuma(int numa_node);
void pinProcesstoNumaNode(int numa_node);
uint32_t GetCurrentNuma();
void cache_flush(char *addr, size_t size);

size_t convert_mem_size(std::string input);

// Map logical replica id to physical NUMA node (mirrors Rust NUMA_EXEC)
int RidToNumaNode(int rid);

// Get all logical replica ids that map to a given physical NUMA node
std::vector<int> NumaNodeRids(int numa_node);

int TidToRid(int tid);

}  // namespace rackobj