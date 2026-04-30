#!/usr/bin/env bash

# Refresh sudo credentials for maximum time
sudo -v
while true; sleep 60; do sudo -n true; kill -0 "$$" || exit; done > /dev/null 2>&1 &

PREHEAT_TIME=30
EXEC_TIME=30

cd $(dirname "$0")/../..
PROJECT_ROOT="$(pwd)"

# ---- Experiment Parameters ----
VARIANTS=("megalon")
WORKLOADS=("zipfian")
W_RATIOS=(0.5)
ZIPFS=(0.7 0.99)
THREAD_COUNTS=(24 42)
NUM_OBJS=18000000
SEQLOCK_SIZES=(32 24 16 8)
SCR_SIZES=(32)

CONFIG_FILE="config/a.yaml"
CONSTANT_FILE="src/common/constants.h"
LOG_ROOT=${PROJECT_ROOT}/logs/eval6
RESULT_ROOT=${RACKOBJ_RESULT_DIR}eval6

BACKUP_CONSTANT_FILE="${CONSTANT_FILE}.bak.eval6.$$"
cp "${CONSTANT_FILE}" "${BACKUP_CONSTANT_FILE}"

restore_constant_file() {
    if [ -f "${BACKUP_CONSTANT_FILE}" ]; then
        mv -f "${BACKUP_CONSTANT_FILE}" "${CONSTANT_FILE}"
        echo "Restored ${CONSTANT_FILE} from backup"
    fi
}

trap restore_constant_file EXIT

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

    # ---- SEQLOCK_SIZES LOOP ----
    for seqlock_size in "${SEQLOCK_SIZES[@]}"; do
        echo "Setting SEQLOCK_BITS to $seqlock_size"
        sed -i -E "s/(#define SEQLOCK_BITS )[0-9]+/\1${seqlock_size}/" ${CONSTANT_FILE}

        # Rebuild program after changing SEQLOCK_BITS
        ./scripts/build.sh > ${LOG_ROOT}/${variant}/build_${seqlock_size}.log 2>&1
        if [ $? -ne 0 ]; then
            echo "Error: build.sh failed for SEQLOCK_BITS=$seqlock_size."
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
                        echo "Running benchmark: scr=${scr_size}MB workload=${workload} zipf=${zipf} w=${w_ratio} seqlock=${seqlock_size}"

                        src_dir="${RACKOBJ_RESULT_DIR}${workload}-${w_ratio}/${zipf}"
                        dst_dir="${RESULT_ROOT}/${variant}/${workload}-${w_ratio}-${zipf}-${scr_size}MB-${seqlock_size}"

                        sudo rm -rf "$src_dir"
                        sudo mkdir -p "$src_dir"
                        sudo chown -R ${USER}:${USER} "$src_dir"

                        # Select thread count dynamically
                        if [[ "$w_ratio" == "0.5" && "$zipf" == "0.99" ]]; then
                            thread_list=(24)
                        else
                            thread_list=(42)
                        fi

                        ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1

                        for num_threads in "${thread_list[@]}"; do
                            sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/kv-store \
                                $workload $RACKOBJ_RESULT_DIR $num_threads \
                                $w_ratio $zipf 0 0 \
                                $PREHEAT_TIME $EXEC_TIME \
                                >> "${LOG_ROOT}/${variant}/output_${workload}_${w_ratio}_${zipf}_${scr_size}MB-${seqlock_size}.log" 2>&1
                        done

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

echo "Reverting CMakeLists.txt back to CMakeLists_megalon.txt"
cp cmake-variant/CMakeLists_megalon.txt CMakeLists.txt

echo "Done"