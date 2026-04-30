#!/usr/bin/env bash

# Refresh sudo credentials for maximum time
sudo -v
while true; sleep 60; do sudo -n true; kill -0 "$$" || exit; done > /dev/null 2>&1 &

PREHEAT_TIME=30
EXEC_TIME=60

# Define loop conditions
variant="tigon"
WORKLOADS=("zipfian")  # Can be expanded to include "hotspot-local"
W_RATIOS=(0.0)  # Can be expanded to (0.1 0.05 0.01)
ZIPFS=(0.7 0.99)
# THREAD_COUNTS=(24 42)
THREAD_COUNTS=(6 12 18 24 36 42 48)
# NUN_OBJS=(1200000 2400000 4800000 7200000 9600000 12000000 18000000)
NUN_OBJS=(7200000)
# SCR_SIZES=(140 168 180)  # <--- new loop variable
SCR_SIZES=(100 500)
CONFIG_FILE="config/a.yaml"

set_c3_rwlock() {
    local cmake_file="CMakeLists.txt"
    if [ ! -f "$cmake_file" ]; then
        echo "Error: $cmake_file not found"
        return 1
    fi
    sed -i -e 's/# add_definitions(-DC3_RWLOCK=[0-9]*)/add_definitions(-DC3_RWLOCK=1)/' \
           -e 's/^add_definitions(-DC3_RWLOCK=[0-9]*)/add_definitions(-DC3_RWLOCK=1)/' "$cmake_file"
    echo "C3_RWLOCK has been set to 1 in $cmake_file"
}

# Check uncore freq
if ! lsmod | grep -q intel_uncore_frequency; then
    echo "Uncore frequency driver not installed. Retry the setup steps."
    exit 1
fi

cd ../..
if [ -z "$RACKOBJ_RESULT_DIR" ]; then
    echo "Error: RACKOBJ_RESULT_DIR is not set or is empty."
    exit 1
fi

rm -rf logs/$variant
mkdir -p logs/$variant

echo "Variant $variant"

sudo rm -rf ${RACKOBJ_RESULT_DIR}${variant}
sudo mkdir -p ${RACKOBJ_RESULT_DIR}${variant}

sudo chown -R $USER ${RACKOBJ_RESULT_DIR}${variant}
# set_c3_rwlock

./build.sh > logs/${variant}/build_errors.log 2>&1
if [ $? -ne 0 ]; then
    echo "Error: build.sh failed."
    exit 1
fi

# Outer loop: SCR_SIZES
for scr_size in "${SCR_SIZES[@]}"; do
    echo "Setting logical_scr_size: ${scr_size}MB"
    sed -i -E "s/(logical_scr_size: )[0-9]+MB/\1${scr_size}MB/" ${CONFIG_FILE}

    for num_obj in "${NUN_OBJS[@]}"; do
        new_pages=$((num_obj + 1000))
        sed -i -E "s/(slots: )[0-9]+/\1${new_pages}/" ${CONFIG_FILE}
        sed -i -E "s/(key_space: )[0-9]+/\1${num_obj}/" ${CONFIG_FILE}

        for workload in "${WORKLOADS[@]}"; do
            if [ $workload == "zipfian" ]; then
                for zipf_skew in "${ZIPFS[@]}"; do
                    for w_ratio in "${W_RATIOS[@]}"; do
                        echo "Running benchmark: scr_size=${scr_size}MB workload=$workload zipf=$zipf_skew w_ratio=$w_ratio num_obj=$num_obj"

                        ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1
                        for num_threads in "${THREAD_COUNTS[@]}"; do
                            src_dir="${RACKOBJ_RESULT_DIR}${variant}/${workload}-${zipf_skew}-${w_ratio}"
                            dst_dir="${RACKOBJ_RESULT_DIR}${variant}/${workload}-${zipf_skew}-${w_ratio}-${num_obj}-${scr_size}MB"

                            sudo rm -rf "$src_dir"
                            mkdir -p "$src_dir"
                            mkdir -p "$dst_dir"

                            sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/tigon \
                                $workload $RACKOBJ_RESULT_DIR $num_threads $w_ratio $zipf_skew $PREHEAT_TIME $EXEC_TIME \
                                >> "logs/${variant}/output_${workload}_${w_ratio}_${zipf_skew}_${scr_size}MB.log" 2>&1

                            cp -r "$src_dir/${num_threads}" "$dst_dir"
                            sudo rm -rf "$src_dir"
                        done
                        ./scripts/set_uncore_frequency.sh > /dev/null 2>&1
                    done
                done
            else
                for w_ratio in "${W_RATIOS[@]}"; do
                    echo "Running benchmark: scr_size=${scr_size}MB workload=$workload w_ratio=$w_ratio num_obj=$num_obj"

                    src_dir="${RACKOBJ_RESULT_DIR}${variant}/${workload}-${w_ratio}"
                    dst_dir="${RACKOBJ_RESULT_DIR}${variant}/${workload}-${w_ratio}-${num_obj}-${scr_size}MB"

                    sudo rm -rf "$src_dir"
                    mkdir -p "$src_dir"

                    ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1
                    for num_threads in "${THREAD_COUNTS[@]}"; do
                        sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/tigon \
                            $workload $RACKOBJ_RESULT_DIR $num_threads $w_ratio \
                            >> "logs/${variant}/output_${workload}_${w_ratio}_${scr_size}MB.log" 2>&1
                    done
                    ./scripts/set_uncore_frequency.sh > /dev/null 2>&1

                    cp -r "$src_dir" "$dst_dir"
                    sudo rm -rf "$src_dir"
                done
            fi
        done
    done
done

# -------------------------------
# Second loop: Run all analysis
# -------------------------------
echo ""
echo "Starting analysis phase..."
cd benchmarks/script

for workload in "${WORKLOADS[@]}"; do
    if [ $workload == "zipfian" ]; then
        for zipf_skew in "${ZIPFS[@]}"; do
            for w_ratio in "${W_RATIOS[@]}"; do
                for scr_size in "${SCR_SIZES[@]}"; do
                    for num_obj in "${NUN_OBJS[@]}"; do
                        for num_threads in "${THREAD_COUNTS[@]}"; do
                            echo "Analyzing tput: ZIPF: $zipf_skew W_RATIOS: $w_ratio OBJS: $num_obj SCR_SIZE: ${scr_size}MB THREADS: $num_threads" \
                                >> ../../logs/${variant}/stats.log

                            dst_dir="${RACKOBJ_RESULT_DIR}${variant}/${workload}-${zipf_skew}-${w_ratio}-${num_obj}-${scr_size}MB/${num_threads}" 

                            python3 analysis.py $dst_dir -l all -t >> ../../logs/${variant}/stats.log 2>&1
                        done
                    done
                done
            done
        done
    else 
        for w_ratio in "${W_RATIOS[@]}"; do
            for scr_size in "${SCR_SIZES[@]}"; do
                for num_obj in "${NUN_OBJS[@]}"; do
                    echo "Analyzing tput: W_RATIOS: $w_ratio OBJS: $num_obj SCR_SIZE: ${scr_size}MB" >> ../../logs/${variant}/stats.log
                    dst_dir="${RACKOBJ_RESULT_DIR}${variant}/${workload}-${w_ratio}-${num_obj}-${scr_size}MB" 

                    python3 analysis.py $dst_dir all >> ../../logs/${variant}/stats.log 2>&1
                done
                echo "" >> ../../logs/${variant}/stats.log
            done
        done
    fi
done
cd ../..

# echo "Reverting CMakeLists.txt back to CMakeLists_c3.txt"
# cp cmake-variant/CMakeLists_c3.txt CMakeLists.txt

echo "Done, BUT ARE YOU RUNNING 32B OR 4K???"