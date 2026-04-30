#!/usr/bin/env bash

# Refresh sudo credentials for maximum time
sudo -v
while true; do sudo -n true; sleep 60; kill -0 "$$" || exit; done 2>/dev/null &

PREHEAT_TIME=30
EXEC_TIME=30

cd $(dirname "$0")/../..
PROJECT_ROOT="$(pwd)"

# ---- Experiment Parameters ----
KEY_SIZES=(10 15 20 30 50 90)
VARIANTS=("megalon")
WORKLOADS=("zipfian")
W_RATIOS=(0.0)
ZIPFS=(0.99)
THREAD_COUNTS=(42)
NUN_OBJS=(4800000)
SCR_SIZES=(200)

CONFIG_FILE="config/a.yaml"
CONSTANT_FILE="src/common/constants.h"
LOG_ROOT=${PROJECT_ROOT}/logs/eval5
RESULT_ROOT=${RACKOBJ_RESULT_DIR}eval5

# Backup files once
CPP_CONST="src/common/constants.h"
RS_CONST="third-party/nr_rust/ffi/constants.rs"

cp "$CPP_CONST" "${CPP_CONST}.bak"
cp "$RS_CONST" "${RS_CONST}.bak"

restore_constants() {
    mv "${CPP_CONST}.bak" "$CPP_CONST"
    mv "${RS_CONST}.bak" "$RS_CONST"
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

if [ -z "$RACKOBJ_RESULT_DIR" ]; then
    echo "Error: RACKOBJ_RESULT_DIR is not set."
    exit 1
fi

# Helper: update KEY_SIZE in C++ + Rust
update_key_size() {
    local ks="$1"

    sed -i -E "s/(#define[[:space:]]+KEY_SIZE[[:space:]]+)[0-9]+/\1${ks}/" "$CPP_CONST"
    sed -i -E "s/(pub const KEY_SIZE:[[:space:]]+usize[[:space:]]*=[[:space:]]*)[0-9]+/\1${ks}/" "$RS_CONST"

    echo "Updated KEY_SIZE → ${ks}"
}

# -------------------------------
# FIRST LOOP: BENCHMARKS
# -------------------------------
for variant in "${VARIANTS[@]}"; do
    echo "Variant $variant"
    cp cmake-variant/CMakeLists_${variant}.txt CMakeLists.txt

    # rm -rf ${LOG_ROOT}/${variant}
    # mkdir -p ${LOG_ROOT}/${variant}

    # rm -rf ${RESULT_ROOT}/${variant}
    # sudo mkdir -p ${RESULT_ROOT}/${variant}
    # sudo chown -R ${USER}:${USER} ${RESULT_ROOT}/${variant}

    for key_size in "${KEY_SIZES[@]}"; do
        update_key_size "$key_size"

        LOG_DIR="${LOG_ROOT}/${variant}/${key_size}"
        rm -rf ${LOG_DIR}
        mkdir -p "$LOG_DIR"

        RESULT_BASE="${RESULT_ROOT}/${variant}/${key_size}"
        rm -rf ${RESULT_BASE}
        mkdir -p "$RESULT_BASE"

        # Build once per key size
        ./scripts/build.sh > "${LOG_DIR}/build_errors.log" 2>&1
        if [ $? -ne 0 ]; then
            echo "Error: build.sh failed."
            exit 1
        fi

        for scr_size in "${SCR_SIZES[@]}"; do
            sed -i -E "s/(logical_scr_size: )[0-9]+MB/\1${scr_size}MB/" ${CONFIG_FILE}

            for num_obj in "${NUN_OBJS[@]}"; do
                new_pages=$((num_obj + 1000))
                sed -i -E "s/(slots: )[0-9]+/\1${new_pages}/" ${CONFIG_FILE}
                sed -i -E "s/(key_space: )[0-9]+/\1${num_obj}/" ${CONFIG_FILE}

                for workload in "${WORKLOADS[@]}"; do
                    for zipf in "${ZIPFS[@]}"; do
                        for w_ratio in "${W_RATIOS[@]}"; do

                            echo "Run: key=${key_size}, scr=${scr_size}, zipf=${zipf}, w_ratio=${w_ratio}"
                            # Source dir from limits-thread
                            src_dir="${RACKOBJ_RESULT_DIR}${workload}-${w_ratio}/${zipf}"
                            dst_dir="${RESULT_BASE}/${workload}-${w_ratio}-${num_obj}-${zipf}-${scr_size}MB"

                            sudo rm -rf "$src_dir"
                            sudo mkdir -p "$src_dir"
                            sudo chown -R ${USER}:${USER} "$src_dir"

                            ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1
                            for num_threads in "${THREAD_COUNTS[@]}"; do
                                sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/kv-store \
                                    $workload $RACKOBJ_RESULT_DIR $num_threads $w_ratio $zipf 0 0 \
                                    $PREHEAT_TIME $EXEC_TIME \
                                    >> "${LOG_DIR}/output_${workload}_${w_ratio}_${zipf}_${scr_size}MB.log" 2>&1
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
done

echo "Reverting CMakeLists.txt back to CMakeLists_megalon.txt"
cp cmake-variant/CMakeLists_megalon.txt CMakeLists.txt

echo "Done"
