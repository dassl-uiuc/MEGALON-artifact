#!/usr/bin/env bash

# Refresh sudo credentials for maximum time
sudo -v
while true; do sudo -n true; sleep 60; kill -0 "$$" || exit; done 2>/dev/null &

PREHEAT_TIME=30
EXEC_TIME=30

cd $(dirname "$0")/../..
PROJECT_ROOT="$(pwd)"

# Define loop conditions
variant="hcmeta"
WORKLOADS=("zipfian")  # Can be expanded to include "hotspot-local"
W_RATIOS=(0.0)  # Can be expanded to (0.1 0.05 0.01)
ZIPFS=(0.99)
THREAD_COUNTS=(42)
NUN_OBJS=(4800000)
SCR_SIZES=(200)
KEY_SIZES=(10 15 20 30 50 90)

CONFIG_FILE="config/a.yaml"
CONSTANT_FILE="src/common/constants.h"
LOG_ROOT=${PROJECT_ROOT}/logs/eval5
RESULT_ROOT=${RACKOBJ_RESULT_DIR}eval5

CPP_CONST="src/common/constants.h"

cp "$CPP_CONST" "${CPP_CONST}.bak"

restore_constants() {
    mv "${CPP_CONST}.bak" "$CPP_CONST"
}

trap restore_constants EXIT

# Check uncore freq
if ! lsmod | grep -q intel_uncore_frequency; then
    echo "Uncore frequency driver not installed. Retry the setup steps."
    exit 1
fi

if [ -z "$RACKOBJ_RESULT_DIR" ]; then
    echo "Error: RACKOBJ_RESULT_DIR is not set or is empty."
    exit 1
fi

echo "Variant $variant"

# sudo rm -rf ${RESULT_ROOT}/${variant}
# sudo mkdir -p ${RESULT_ROOT}/${variant}
# sudo chown -R ${USER}:${USER} ${RESULT_ROOT}/${variant}

# Outermost loop: KEY_SIZES
for key_size in "${KEY_SIZES[@]}"; do
    echo "Updated KEY_SIZE → ${key_size}"
    if ! sed -i -E "s/(#define KEY_SIZE )[0-9]+/\1${key_size}/" "$CPP_CONST"; then
        echo "Error: Failed to update KEY_SIZE in $CPP_CONST"
        exit 1
    fi

    LOG_DIR="${LOG_ROOT}/${variant}/${key_size}"
    sudo rm -rf "$LOG_DIR"
    mkdir -p "$LOG_DIR"

    ./scripts/build.sh > "${LOG_DIR}/build_errors.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "Error: build.sh failed for KEY_SIZE=${key_size}."
        exit 1
    fi

    result_base="${RESULT_ROOT}/${variant}/${key_size}"
    sudo rm -rf $result_base
    sudo mkdir -p "$result_base"
    sudo chown -R $USER "$result_base"

    # Outer loop: SCR_SIZES
    for scr_size in "${SCR_SIZES[@]}"; do
        echo "Setting logical_scr_size: ${scr_size}MB"
        sed -i -E "s/(logical_scr_size: )[0-9]+MB/\1${scr_size}MB/" ${CONFIG_FILE}

        for num_obj in "${NUN_OBJS[@]}"; do
            new_pages=$((num_obj + 1000))
            sed -i -E "s/(slots: )[0-9]+/\1${new_pages}/" ${CONFIG_FILE}
            sed -i -E "s/(key_space: )[0-9]+/\1${num_obj}/" ${CONFIG_FILE}

            for workload in "${WORKLOADS[@]}"; do
                for zipf_skew in "${ZIPFS[@]}"; do
                    for w_ratio in "${W_RATIOS[@]}"; do
                        echo "Running benchmark: key_size=${key_size}B scr_size=${scr_size}MB workload=$workload zipf=$zipf_skew w_ratio=$w_ratio num_obj=$num_obj"

                        src_dir="${RACKOBJ_RESULT_DIR}hcmeta/${workload}-${zipf_skew}-${w_ratio}"
                        dst_dir="${result_base}/${workload}-${w_ratio}-${num_obj}-${zipf_skew}-${scr_size}MB"

                        sudo rm -rf "$src_dir"
                        sudo mkdir -p "$src_dir"
                        sudo chown -R ${USER}:${USER} "$src_dir"

                        ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1
                        for num_threads in "${THREAD_COUNTS[@]}"; do
                            sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/kv-store \
                                $workload $RACKOBJ_RESULT_DIR $num_threads $w_ratio $zipf_skew 0 0 $PREHEAT_TIME $EXEC_TIME \
                                >> "${LOG_DIR}/output_${workload}_${w_ratio}_${zipf_skew}_${scr_size}MB.log" 2>&1
                        done
                        ./scripts/set_uncore_frequency.sh > /dev/null 2>&1

                        if [ -d "$src_dir" ] && [ "$(ls -A "$src_dir")" ]; then
                            cp -r "$src_dir" "$dst_dir"
                        else
                            echo "Error: missing or empty $src_dir"
                            exit 1
                        fi
                    done
                done
            done
        done
    done
done

echo "Reverting CMakeLists.txt back to CMakeLists_hcmeta.txt"
cp cmake-variant/CMakeLists_hcmeta.txt CMakeLists.txt
echo "Done"