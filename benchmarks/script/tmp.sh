#!/usr/bin/env bash

# Define loop conditions
variant="tigon"

variant="tigon"
WORKLOADS=("zipfian")  # Can be expanded to include "hotspot-local"
W_RATIOS=(0.05 0.5)  # Can be expanded to (0.1 0.05 0.01)
ZIPF=(0.7 0.99)
THREAD_COUNTS=(24 42)
NUN_OBJS=(2400000 4800000 7200000 9600000 12000000 18000000)
# NUN_OBJS=(7200000)

if [ -z "$RACKOBJ_RESULT_DIR" ]; then
    echo "Error: RACKOBJ_RESULT_DIR is not set or is empty."
    exit 1
fi

rw=rw

echo "Starting analysis phase..."
for workload in "${WORKLOADS[@]}"; do
    if [ $workload == "zipfian" ]; then
        for zipf_skew in "${ZIPF[@]}"; do
            for w_ratio in "${W_RATIOS[@]}"; do
                for num_obj in "${NUN_OBJS[@]}"; do
                    for num_threads in "${THREAD_COUNTS[@]}"; do
                        echo "Analyzing tput: ZIPF: $zipf_skew W_RATIOS: $w_ratio OBJS: $num_obj THREADS: $num_threads"
                        dst_dir="${RACKOBJ_RESULT_DIR}eval3/${variant}/${workload}-${zipf_skew}-${w_ratio}-${num_obj}/${num_threads}" 
                        
                        # echo "analyzing read latency"
                        # python3 avg_lat.py $dst_dir rf
                        # if (( $(echo "$w_ratio != 0" | bc -l) )); then
                        #     echo "analyzing write latency"
                        #     python3 avg_lat.py $dst_dir wf
                        # fi
                        # echo "analyzing read-write latency"
                        python3 avg_lat.py $dst_dir t
                        # echo ""

                        # python3 analysis.py $dst_dir -l all -t
                    done
                done
            done
        done
    else 
        for w_ratio in "${W_RATIOS[@]}"; do
            for num_obj in "${NUN_OBJS[@]}"; do
                echo "Analyzing tput: W_RATIOS: $w_ratio OBJS: $num_obj"
                dst_dir="${RACKOBJ_RESULT_DIR}${variant}/${workload}-${w_ratio}-${num_obj}" 

                python3 analysis.py $dst_dir all
            done
            echo ""
        done
    fi
done
cd ../..