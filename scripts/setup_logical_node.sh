#!/usr/bin/env bash

if [[ -z "$1" || -z "$2" || -z "$3" ]]; then
    echo "Usage: $0 <NUMA_MEM> <LOGICAL_NODE_NUM> <KEY_SIZE>"
    echo "  <NUMA_MEM>: The NUMA memory node id (e.g. 0)"
    echo "  <LOGICAL_NODE_NUM>: Number of logical nodes/replicas (e.g. 9)"
    echo "  <KEY_SIZE>: Key size in bytes (e.g. 12 or 24)"
    exit 1
fi

# jump to the current directory
CUR_DIR="$(dirname "$0")"
cd $CUR_DIR
CUR_DIR="$(pwd)"

NUMA_MEM=$1
LOGICAL_NODE_NUM=$2
KEY_SIZE=$3
constants_file="../src/common/constants.h"
common_file="../benchmarks/common.h"

get_numa_nodes() {
    if command -v lscpu >/dev/null 2>&1; then
        lscpu | awk -F: '/NUMA node\(s\)/ {gsub(/ /,"",$2); print $2}'
    elif [ -d /sys/devices/system/node ]; then
        ls -d /sys/devices/system/node/node* 2>/dev/null | wc -l
    else
        echo 1  # fallback if NUMA info not available
    fi
}

numa_nodes=$(get_numa_nodes)
# Sanity check: require at least 2 NUMA nodes
if [ "$numa_nodes" -lt 2 ]; then
    echo "Warning: Detected only $numa_nodes NUMA node(s). At least 2 NUMA nodes are required for proper logical node setup."
    echo "Aborting setup."
    exit 1
fi

echo "detected hardware numa nodes: $numa_nodes, set NUMA_MEM=$NUMA_MEM, LOGICAL_NODE_NUM=$LOGICAL_NODE_NUM, KEY_SIZE=$KEY_SIZE"

# Update LOGICAL_NODE_NUM to value of $LOGICAL_NODE_NUM
sed -i "s/^\(#define[ \t]*LOGICAL_NODE_NUM[ \t]*\).*\$/\1$LOGICAL_NODE_NUM  \/\/ this should match the rust library/" "$constants_file"

# Update NUM_NUMA to value of $numa_nodes
sed -i "s/^\(#define[ \t]*NUM_NUMA[ \t]*\).*\$/\1$numa_nodes  \/\/ number of NUMA nodes on the machine (including mem. node)/" "$constants_file"

# Update NUMA_MEM to value of $NUMA_MEM
sed -i "s/^\(#define[ \t]*NUMA_MEM[ \t]*\).*\$/\1$NUMA_MEM/" "$constants_file"

# Update KEY_SIZE in C++ constants.h
sed -i "s/^\(#define[ \t]*KEY_SIZE[ \t]*\).*\$/\1$KEY_SIZE/" "$constants_file"

CPU_GHZ=$(awk -F: '/cpu MHz/ { sum += $2; count++ } END { if(count>0) print int((sum/count + 50)/100)*100 / 1000 }' /proc/cpuinfo)

sed -i "s/^#define CPU_FREQ_GHZ .*/#define CPU_FREQ_GHZ $CPU_GHZ/" "$common_file"
echo "CPU frequency detected: $CPU_GHZ GHz, updated in $common_file"