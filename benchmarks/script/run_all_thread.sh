#!/usr/bin/env bash

cd ../..
# echo 0 | sudo tee /proc/sys/kernel/numa_balancing
# sudo sysctl -w kernel.numa_balancing=0
# sudo pkill rackobj-daemon

# ./scripts/set_uncore_frequency.sh 2200000
./scripts/set_uncore_frequency.sh 800000
sleep 1

# for i in 1 3 6 9 12;
# for i in 1 3 6 9 12 18 24 36 48 60;
# for i in {1..1}
for i in {40..40}
do
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread hotcache $RACKOBJ_RESULT_DIR $i 0.01
    sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread zipfian $RACKOBJ_RESULT_DIR 2400000 $i 0.5 0.7
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread partial-partitioned $RACKOBJ_RESULT_DIR $i 0.0 0.7 1.0 1.0 10 30
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread random $RACKOBJ_RESULT_DIR $i 0.0 0.7 1.0 1.0 10 30
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread-dump partial-partitioned $RACKOBJ_RESULT_DIR $i 0.0 0.7 1.0 1.0 10 10
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/megalon partial-partitioned $RACKOBJ_RESULT_DIR $i 0.0 0.99 1.0 1.0 10 10
    sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/megalon zipfian $RACKOBJ_RESULT_DIR $i 0.0 0.99 1.0 1.0 10 10
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/ycsb_benchmark c $RACKOBJ_RESULT_DIR $i 0.99 10 10
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread hotspot-local $RACKOBJ_RESULT_DIR $i 0.1 0.1
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread hotcachepartial $i 0.75
    
    echo "--------------------------------"
    sleep 2
done

./scripts/set_uncore_frequency.sh
