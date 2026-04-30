#!/usr/bin/env bash

cd ..
# echo 0 | sudo tee /proc/sys/kernel/numa_balancing
# sudo sysctl -w kernel.numa_balancing=0

# ./scripts/set_uncore_frequency.sh 2200000
./scripts/set_uncore_frequency.sh 800000
sleep 1


sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/load-write
# sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/load
./scripts/set_uncore_frequency.sh
