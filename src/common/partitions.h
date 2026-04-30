#pragma once

#include <numa.h>
#include <utmpx.h>

#include "../src/common/helper.h"
#include "MurmurHash3.h"
#include "common/constants.h"
#include "globals.h"

#ifndef PARTITION_STRATEGY
#define PARTITION_STRATEGY PARTITION_RANGE
#endif

// Function declarations
uint8_t partition_hash(off_t key);
uint8_t partition_range(off_t key);
uint8_t partition_default(off_t key);
uint8_t whose_key(off_t key);
bool is_my_key(off_t key, int my_logical_node);

/**
 * @brief Partitions the key space by using a hash algorithm
 * @param key The key to use the partition strategy on
 * @return The partition (NUMA node [1-3]) that owns the @p key
 */
uint8_t partition_hash(off_t key) { return static_cast<uint8_t>(key % LOGICAL_NODE_NUM); }

/**
 * @brief Partitions the key space using a range partition
 * @param key The key to use the partition strategy on
 * @return The partition that owns the \p key
 */
uint8_t partition_range(off_t key) {
    uint64_t keyspace = static_cast<uint64_t>(rackobj::lib::client_cfg.GetKeySpace());
    uint64_t partitions = static_cast<uint64_t>(LOGICAL_NODE_NUM);

    if (partitions == 0 || keyspace == 0) {
        LOG(FATAL) << "Invalid configuration: partitions=" << partitions << " keyspace=" << keyspace;
        return UINT8_MAX;
    }

    if (key < 0 || static_cast<uint64_t>(key) >= keyspace) {
        LOG(FATAL) << "key " << key << " is out of range of [0-" << keyspace << ")";
        return UINT8_MAX;
    }

    // Proportional mapping: evenly map [0, keyspace) to [0, partitions)
    uint8_t partition = static_cast<uint8_t>((static_cast<uint64_t>(key) * partitions) / keyspace);
    return partition;
}

/**
 * @brief Implements the default or specified partition strategy
 * @param key The key to find the partition of
 * @return The corrected partition number (NUMA node, [1-3]) of the key
 */
uint8_t partition_default(off_t key) {
    uint8_t partition;
#if PARTITION_STRATEGY == PARTITION_RANGE
    partition = partition_range(key);
#elif PARTITION_STRATEGY == PARTITION_HASH
    partition = partition_hash(key);
#else
    partition = partition_range(key);
#endif
    return partition;
}

/**
 * @param key The key to use the partition strategy on
 * @return The partition (LOGICAL_NODE_NUM) that owns the @p key
 */
uint8_t whose_key(off_t key) { return partition_default(key); }

/**
 * @brief Determines whether or not the @p key belongs to the current partition (NUMA node)
 * @param key The key to identify the partition of
 * @return True if the key belongs to this partition; otherwise, false
 */
bool is_my_key(off_t key, int my_logical_node) {
    // check which partition the key belongs to
    uint8_t key_partition = partition_default(key);

    return my_logical_node == key_partition;
}