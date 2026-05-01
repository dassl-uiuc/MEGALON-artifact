#!/usr/bin/env bash

# Refresh sudo credentials for maximum time
sudo -v
while true; sleep 60; do sudo -n true; kill -0 "$$" || exit; done > /dev/null 2>&1 &

PREHEAT_TIME=70
EXEC_TIME=30

cd $(dirname "$0")/../..
PROJECT_ROOT="$(pwd)"

# ---- Experiment Parameters ----
VARIANTS=("partitioned")
WORKLOADS=("partial-partitioned")
W_RATIOS=(0.0)  
ZIPFS=(0.99)
# THREAD_COUNTS=(42)
NUM_OBJS=18000000
PARTITION_RATIOS=(1.0 0.8 0.6 0.4 0.2 0.0)
# PARTITION_RATIOS=(1.0)
SCR_SIZES=(200)

CONFIG_FILE="config/a.yaml"
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

if [ -z "$RACKOBJ_RESULT_DIR" ]; then
    echo "Error: RACKOBJ_RESULT_DIR is not set or is empty."
    exit 1
fi

# ============================================================
#                FIRST LOOP — EXECUTE EXPERIMENTS
# ============================================================

for variant in "${VARIANTS[@]}"; do
    rm -rf ${LOG_ROOT}/${variant}
    mkdir -p ${LOG_ROOT}/${variant}

    echo "Variant $variant"
    cp cmake-variant/CMakeLists_${variant}.txt CMakeLists.txt

    rm -rf ${RESULT_ROOT}/${variant}
    sudo mkdir -p ${RESULT_ROOT}/${variant}
    sudo chown -R ${USER}:${USER} ${RESULT_ROOT}/${variant}

    ./scripts/build.sh > ${LOG_ROOT}/${variant}/build_errors.log 2>&1
    if [ $? -ne 0 ]; then
        echo "Error: build.sh failed."
        exit 1
    fi

    # ---- SCR LOOP ----
    for scr_size in "${SCR_SIZES[@]}"; do
        echo "Setting logical_scr_size: ${scr_size}MB"
        sed -i -E "s/(logical_scr_size: )[0-9]+MB/\1${scr_size}MB/" ${CONFIG_FILE}

        # Set key_space and slots ONE TIME (no loop)
        new_pages=$((NUM_OBJS + 1000))
        sed -i -E "s/(slots: )[0-9]+/\1${new_pages}/" ${CONFIG_FILE}
        sed -i -E "s/(key_space: )[0-9]+/\1${NUM_OBJS}/" ${CONFIG_FILE}

        for workload in "${WORKLOADS[@]}"; do
            for zipf in "${ZIPFS[@]}"; do
                for w_ratio in "${W_RATIOS[@]}"; do
                    for part_ratio in "${PARTITION_RATIOS[@]}"; do
                        # Set partition_ratio in config file
                        sed -i -E "s/(partition_ratio: )[0-9.]+/\1${part_ratio}/" ${CONFIG_FILE}
                        echo "Running benchmark: scr=${scr_size}MB workload=${workload} zipf=${zipf} w=${w_ratio} part=${part_ratio}"

                        src_dir="${RACKOBJ_RESULT_DIR}${workload}-${w_ratio}/${zipf}"
                        dst_dir="${RESULT_ROOT}/${variant}/${workload}-${w_ratio}-${part_ratio}-${zipf}-${scr_size}MB"

                        sudo rm -rf "$src_dir"
                        sudo mkdir -p "$src_dir"
                        sudo chown -R ${USER}:${USER} "$src_dir"

                        ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1

                        if awk "BEGIN {exit !($part_ratio >= 0.6)}"; then
                            num_threads=52
                        else
                            num_threads=45
                        fi

                        sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/kv-store \
                            $workload $RACKOBJ_RESULT_DIR $num_threads \
                            $w_ratio $zipf $part_ratio $part_ratio \
                            $PREHEAT_TIME $EXEC_TIME \
                            >> "${LOG_ROOT}/${variant}/output_${workload}_${w_ratio}_${zipf}_${scr_size}MB.log" 2>&1

                        ./scripts/set_uncore_frequency.sh > /dev/null 2>&1

                        # ---- COPY RESULT ----
                        if [ -d "$src_dir" ] && [ "$(ls -A "$src_dir")" ]; then
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

# ============================================================
#                SECOND LOOP — ANALYSIS
# ============================================================

# echo ""
# echo "Starting analysis phase..."
# cd benchmarks/script

# for variant in "${VARIANTS[@]}"; do
#     for workload in "${WORKLOADS[@]}"; do
#         for zipf in "${ZIPFS[@]}"; do
#             for w_ratio in "${W_RATIOS[@]}"; do
#                 for scr_size in "${SCR_SIZES[@]}"; do

#                     echo "Analyzing: workload=$workload zipf=$zipf w=$w_ratio scr=${scr_size}MB" \
#                         >> ${LOG_ROOT}/${variant}/stat.log

#                     for part_ratio in "${PARTITION_RATIOS[@]}"; do
#                         dst_dir="${RESULT_ROOT}/${variant}/${workload}-${w_ratio}-${part_ratio}-${zipf}-${scr_size}MB"

#                         python3 avg_lat.py $dst_dir rwtf \
#                             >> ${LOG_ROOT}/${variant}/stat.log
#                     done

#                     echo "" >> ${LOG_ROOT}/${variant}/stat.log
#                 done
#             done
#         done
#     done
# done

# cd ../..

echo "Reverting CMakeLists.txt back to CMakeLists_megalon.txt"
cp cmake-variant/CMakeLists_megalon.txt CMakeLists.txt

echo "Done"
