#!/usr/bin/env bash

# Refresh sudo credentials
sudo -v
while true; sleep 60; do sudo -n true; kill -0 "$$" || exit; done > /dev/null 2>&1 &

PREHEAT_TIME=30
EXEC_TIME=30

cd $(dirname "$0")/../..
PROJECT_ROOT="$(pwd)"

# ---- Experiment Parameters ----
VARIANT="hcmeta"
WORKLOADS=("partial-partitioned")
W_RATIOS=(0.0)
ZIPFS=(0.99)
THREAD_COUNTS=(42)
NUM_OBJS=7200000
# PARTITION_RATIOS=(1.0 0.4 0.2 0.0)
PARTITION_RATIOS=(1.0)
SCR_SIZES=(100)

CONFIG_FILE="config/b.yaml"
LOG_ROOT=${PROJECT_ROOT}/logs/eval8
RESULT_ROOT=${RACKOBJ_RESULT_DIR}eval8

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

# Check uncore frequency driver
if ! lsmod | grep -q intel_uncore_frequency; then
    echo "Uncore frequency driver not installed. Retry setup steps."
    exit 1
fi

if [ -z "$RACKOBJ_RESULT_DIR" ]; then
    echo "Error: RACKOBJ_RESULT_DIR is not set."
    exit 1
fi

variant=$VARIANT

sudo rm -rf ${LOG_ROOT}/${variant}
mkdir -p ${LOG_ROOT}/${variant}

echo "Variant $variant"

sudo rm -rf ${RESULT_ROOT}/${variant}
sudo mkdir -p ${RESULT_ROOT}/${variant}
sudo chown -R $USER:${USER} ${RESULT_ROOT}/${variant}

cp cmake-variant/CMakeLists_hcmeta.txt CMakeLists.txt

./scripts/build.sh > ${LOG_ROOT}/${variant}/build_errors.log 2>&1
if [ $? -ne 0 ]; then
    echo "Error: build.sh failed."
    exit 1
fi

# ---- SCR LOOP ----
for scr_size in "${SCR_SIZES[@]}"; do
    echo "Setting logical_scr_size: ${scr_size}MB"
    sed -i -E "s/(logical_scr_size: )[0-9]+MB/\1${scr_size}MB/" ${CONFIG_FILE}

    new_pages=$((NUM_OBJS + 1000))
    sed -i -E "s/(slots: )[0-9]+/\1${new_pages}/" ${CONFIG_FILE}
    sed -i -E "s/(key_space: )[0-9]+/\1${NUM_OBJS}/" ${CONFIG_FILE}

    for workload in "${WORKLOADS[@]}"; do
        for part_ratio in "${PARTITION_RATIOS[@]}"; do
            if [[ "$workload" == "zipfian" || "$workload" == "partial-partitioned" ]]; then
                for zipf_skew in "${ZIPFS[@]}"; do
                    for w_ratio in "${W_RATIOS[@]}"; do

                        echo "Running benchmark: scr=${scr_size}MB workload=$workload zipf=$zipf_skew w=$w_ratio part=$part_ratio"

                        ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1

                        for num_threads in "${THREAD_COUNTS[@]}"; do
                            src_dir="${RACKOBJ_RESULT_DIR}hcmeta/${workload}-${zipf_skew}-${w_ratio}"
                            dst_dir="${RESULT_ROOT}/${variant}/${workload}-${zipf_skew}-${w_ratio}-${part_ratio}-${scr_size}MB"

                            sudo rm -rf "$src_dir"
                            sudo mkdir -p "$src_dir"
                            sudo chown -R ${USER}:${USER} "$src_dir"
                            mkdir -p "$dst_dir"

                            sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/hcmeta \
                                $workload $RACKOBJ_RESULT_DIR $num_threads \
                                $w_ratio $zipf_skew $part_ratio $part_ratio \
                                $PREHEAT_TIME $EXEC_TIME \
                                >> "${LOG_ROOT}/${variant}/output_${workload}_${w_ratio}_${zipf_skew}_${scr_size}MB.log" 2>&1

                            if [ -d "$src_dir" ] && [ "$(ls -A "$src_dir" 2>/dev/null)" ]; then
                                cp -r "$src_dir/${num_threads}" "$dst_dir"
                                sudo rm -rf "$src_dir"
                            else
                                echo "Error: $src_dir does not exist or is empty"
                                exit 1
                            fi
                        done

                        ./scripts/set_uncore_frequency.sh > /dev/null 2>&1
                    done
                done
            else
                for w_ratio in "${W_RATIOS[@]}"; do
                    echo "Running benchmark: scr=${scr_size}MB workload=$workload w=$w_ratio part=$part_ratio"

                    src_dir="${RACKOBJ_RESULT_DIR}hcmeta/${workload}-${w_ratio}-${part_ratio}"
                    dst_dir="${RESULT_ROOT}/${variant}/${workload}-${w_ratio}-${part_ratio}-${NUM_OBJS}-${scr_size}MB"

                    sudo rm -rf "$src_dir"
                    mkdir -p "$src_dir"

                    ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1
                    for num_threads in "${THREAD_COUNTS[@]}"; do
                        sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/hcmeta \
                            $workload $RACKOBJ_RESULT_DIR $num_threads \
                            $w_ratio $part_ratio \
                            $PREHEAT_TIME $EXEC_TIME \
                            >> "${LOG_ROOT}/${variant}/output_${workload}_${w_ratio}_${scr_size}MB.log" 2>&1
                    done
                    ./scripts/set_uncore_frequency.sh > /dev/null 2>&1

                    cp -r "$src_dir" "$dst_dir"
                    sudo rm -rf "$src_dir"
                done
            fi
        done
    done
done

echo "Reverting CMakeLists.txt back to CMakeLists_hcmeta.txt"
cp cmake-variant/CMakeLists_hcmeta.txt CMakeLists.txt
echo "Done"
