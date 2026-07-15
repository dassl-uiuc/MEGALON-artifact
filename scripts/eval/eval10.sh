#!/usr/bin/env bash

# Refresh sudo credentials for maximum time
sudo -v
while true; sleep 60; do sudo -n true; kill -0 "$$" || exit; done > /dev/null 2>&1 &

PREHEAT_TIME=30
EXEC_TIME=30

cd $(dirname "$0")/../..
PROJECT_ROOT="$(pwd)"

# Define loop conditions
VARIANTS=("alllog")
WORKLOADS=("zipfian")  # Can be expanded to include "hotspot-local"
W_RATIOS=(0.0 0.05 0.5)  # Can be expanded to (0.1 0.05 0.01)
ZIPFS=(0.99)
THREAD_COUNTS=(42)
NUN_OBJS=(18000000)
SCR_SIZES=(200)

CONFIG_FILE="config/a.yaml"
LOG_ROOT=${PROJECT_ROOT}/logs/eval10
RESULT_ROOT=${RACKOBJ_RESULT_DIR}eval10

if [ -z "$RACKOBJ_RESULT_DIR" ]; then
    echo "Error: RACKOBJ_RESULT_DIR is not set or is empty."
    exit 1
fi

# -------------------------------
# First loop: Run all benchmarks
# -------------------------------
for variant in "${VARIANTS[@]}"; do
    rm -rf ${LOG_ROOT}/${variant}
    mkdir -p ${LOG_ROOT}/$variant
    echo "Variant $variant"
    cp cmake-variant/CMakeLists_${variant}.txt CMakeLists.txt

    sudo rm -rf ${RESULT_ROOT}/${variant}
    sudo mkdir -p ${RESULT_ROOT}/${variant}
    sudo chown -R ${USER}:${USER} ${RESULT_ROOT}/${variant}

    # set_c3_rwlock

    ./scripts/build.sh > ${LOG_ROOT}/${variant}/build_errors.log 2>&1
    if [ $? -ne 0 ]; then
        echo "Error: build.sh failed."
        exit 1
    fi

    # Outer loop for SCR_SIZES
    for scr_size in "${SCR_SIZES[@]}"; do
        echo "Setting logical_scr_size: ${scr_size}MB"
        sed -i -E "s/(logical_scr_size: )[0-9]+MB/\1${scr_size}MB/" ${CONFIG_FILE}

        for num_obj in "${NUN_OBJS[@]}"; do
            new_pages=$((num_obj + 1000))
            sed -i -E "s/(slots: )[0-9]+/\1${new_pages}/" ${CONFIG_FILE}
            sed -i -E "s/(key_space: )[0-9]+/\1${num_obj}/" ${CONFIG_FILE}

            for workload in "${WORKLOADS[@]}"; do
                for zipf in "${ZIPFS[@]}"; do
                    for w_ratio in "${W_RATIOS[@]}"; do
                        echo "Running benchmark: scr_size=${scr_size}MB, workload=$workload, zipf=$zipf, w_ratio=$w_ratio, num_obj=$num_obj"

                        src_dir="${RACKOBJ_RESULT_DIR}${workload}-${w_ratio}/${zipf}"
                        dst_dir="${RESULT_ROOT}/${variant}/${workload}-${w_ratio}-${num_obj}-${zipf}-${scr_size}MB"

                        sudo rm -rf "$src_dir"
                        mkdir -p "$src_dir"

                        # # Select thread count dynamically
                        # if [[ "$w_ratio" == "0.5" && "$zipf" == "0.99" ]]; then
                        #     thread_list=(24)
                        # else
                        #     thread_list=(42)
                        # fi

                        ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1
                        for num_threads in "${THREAD_COUNTS[@]}"; do
                            sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/kv-store \
                                $workload $RACKOBJ_RESULT_DIR $num_threads $w_ratio $zipf 0.0 0.0 $PREHEAT_TIME $EXEC_TIME \
                                >> "${LOG_ROOT}/${variant}/output_${workload}_${w_ratio}_${zipf}_${scr_size}MB.log" 2>&1
                        done
                        ./scripts/set_uncore_frequency.sh > /dev/null 2>&1

                        if [ -d "$src_dir" ] && [ "$(ls -A "$src_dir" 2>/dev/null)" ]; then
                            cp -r "$src_dir" "$dst_dir"
                        else
                            echo "Error: $src_dir does not exist or is empty"
                            exit 1
                        fi
                    done
                done
            done
        done
    done
done

echo "Reverting CMakeLists.txt back to CMakeLists_alllog.txt"
cp cmake-variant/CMakeLists_alllog.txt CMakeLists.txt
