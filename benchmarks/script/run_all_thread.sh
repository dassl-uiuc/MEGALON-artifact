#!/usr/bin/env bash

cd ../..
# echo 0 | sudo tee /proc/sys/kernel/numa_balancing
# sudo sysctl -w kernel.numa_balancing=0
# sudo pkill rackobj-daemon

# ./scripts/set_uncore_frequency.sh 2200000
./scripts/set_uncore_frequency.sh 800000
sleep 1

# for i in 1 3 6 9 12;
for i in 42;
# for i in {1..1}
# for i in {18..18}
do
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread hotcache $RACKOBJ_RESULT_DIR $i 0.01
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread partial-partitioned $RACKOBJ_RESULT_DIR $i 0.0 0.7 1.0 1.0 10 30
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread random $RACKOBJ_RESULT_DIR $i 0.0 0.7 1.0 1.0 10 30
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread-dump partial-partitioned $RACKOBJ_RESULT_DIR $i 0.0 0.7 1.0 1.0 10 10
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/megalon partial-partitioned $RACKOBJ_RESULT_DIR $i 0.0 0.99 1.0 1.0 30 30
    # sudo RACKOBJ_CONFIG=config/file.yaml ./build/benchmarks/megalon-file zipfian $RACKOBJ_RESULT_DIR $i 0.0 0.99 1.0 1.0 10 30

    sudo RACKOBJ_CONFIG=config/ycsb.yaml ./build/benchmarks/ycsb_benchmark d $RACKOBJ_RESULT_DIR $i 0.99 10 10
    
    echo "--------------------------------"
    sleep 2
done

./scripts/set_uncore_frequency.sh

# cp -r /mydata/jiyu/rackobj-benchmarks/benchmarks/results/zipfian-0.5/0.99/40 /mydata/jiyu/rackobj/benchmarks/fg_tmp/0.99
