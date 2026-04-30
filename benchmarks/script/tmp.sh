#!/usr/bin/env bash

# VARIANTS=("c3")
# WORKLOADS=("zipfian")  # Can be expanded to include "hotspot-local"
# W_RATIOS=(0.05 0.5)  # Can be expanded to (0.1 0.05 0.01)
# ZIPFS=(0.7 0.99)
# THREAD_COUNTS=(24 42)
# NUN_OBJS=(600000 800000 1200000 2400000 4800000 7200000 9600000 12000000 18000000)
# # SCR_SIZES=(50)  # <--- new loop variable
# SCR_SIZES=(50)

# if [ -z "$RACKOBJ_RESULT_DIR" ]; then
#     echo "Error: RACKOBJ_RESULT_DIR is not set or is empty."
#     exit 1
# fi

# workload=zipfian
# rw=rw

# echo ""
# echo ""
# echo "Starting analysis phase..."

# for variant in "${VARIANTS[@]}"; do
#     for workload in "${WORKLOADS[@]}"; do
#         for zipf in "${ZIPFS[@]}"; do
#             for w_ratio in "${W_RATIOS[@]}"; do
#                 for scr_size in "${SCR_SIZES[@]}"; do
#                     echo "Analyzing tput: workload=$workload, zipf=$zipf, W_RATIOS=$w_ratio, SCR_SIZE=${scr_size}MB" >> ../../logs/${variant}/stat.log
#                     for num_obj in "${NUN_OBJS[@]}"; do
#                         dst_dir="${RACKOBJ_RESULT_DIR}${variant}/${workload}-${w_ratio}-${num_obj}-${zipf}-${scr_size}MB"

#                         python3 avg_lat.py $dst_dir t >> ../../logs/${variant}/stat.log
#                     done
#                     echo "" >> ../../logs/${variant}/stat.log
#                 done
#             done
#         done
#     done
# done

# sudo -v
# while true; sleep 60; do sudo -n true; kill -0 "$$" || exit; done > /dev/null 2>&1 &

# /mydata/jiyu/rackobj/scripts/eval/eval2.sh
# /mydata/jiyu/rackobj/scripts/eval/eval3.sh
# /mydata/jiyu/rackobj-tigon/scripts/eval/eval3.sh

./iterate.sh
cd /mydata/jiyu/rackobj-tigon/benchmarks/script
./iterate.sh