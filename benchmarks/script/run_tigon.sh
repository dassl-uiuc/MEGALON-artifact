#!/usr/bin/env bash

PREHEAT_TIME=10
EXEC_TIME=10

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
# for i in {40..40}
do
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread hotcache $RACKOBJ_RESULT_DIR $i 0.01
    sudo RACKOBJ_CONFIG=config/b.yaml ./build/benchmarks/tigon partial-partitioned $RACKOBJ_RESULT_DIR $i 0.0 0.99 0.0 0.0 ${PREHEAT_TIME} ${EXEC_TIME}
    #sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/tigon zipfian $RACKOBJ_RESULT_DIR $i 0.0 0.99 $PREHEAT_TIME $EXEC_TIME
    sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/ycsb_benchmark f $RACKOBJ_RESULT_DIR $i 0.99 $PREHEAT_TIME $EXEC_TIME >> "logs/tigon/ycsb_f_log" 2>&1
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread hotspot-local $RACKOBJ_RESULT_DIR $i 0.1 0.1
    # sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/limits-thread hotcachepartial $i 0.75
    
    echo "--------------------------------"
    sleep 2
done

./scripts/set_uncore_frequency.sh

# cp -r /mydata/jiyu/rackobj-benchmarks/benchmarks/results/tigon/zipfian-0.99-0.5 /mydata/jiyu/rackobj/benchmarks/tigon_tmp/
