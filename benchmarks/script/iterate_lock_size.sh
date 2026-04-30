#!/usr/bin/env bash

# Refresh sudo credentials for maximum time
sudo -v
while true; do sudo -n true; sleep 60; kill -0 "$$" || exit; done 2>/dev/null &

PREHEAT_TIME=30
EXEC_TIME=60

# -------------------------------
# Loop Variables
# -------------------------------
VARIANTS=("c3")
WORKLOADS=("zipfian")
W_RATIOS=(0.05 0.5)
ZIPFS=(0.99)
THREAD_COUNTS=(42)
NUN_OBJS=(7200000)
SCR_SIZES=(16)
SEQLOCK_BITS=(4 5 6 7 8 16 24 32)

PROJ_ROOT="$(dirname "$0")/../.."
cd $PROJ_ROOT

# -------------------------------
# Backup files once
# -------------------------------
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
    echo "Error: ${RACKOBJ_RESULT_DIR} is not set or is empty."
    exit 1
fi

# -------------------------------
# Helper: update KEY_SIZE in C++ + Rust
# -------------------------------
update_key_size() {
    local ks="$1"

    sed -i -E "s/(#define[[:space:]]+SEQLOCK_BITS[[:space:]]+)[0-9]+/\1${ks}/" "$CPP_CONST"

    echo "Updated SEQLOCK_BITS → ${ks}"
}

# -------------------------------
# FIRST LOOP: BENCHMARKS
# -------------------------------

if [ -z "$RACKOBJ_RESULT_DIR" ]; then
    echo "Error: RACKOBJ_RESULT_DIR is not set."
    exit 1
fi

for variant in "${VARIANTS[@]}"; do
    echo "VARIANT = ${variant}"

    # Clean logs and recreate
    rm -rf logs/${variant}
    mkdir -p logs/${variant}

    # Prepare results root
    rm -rf ${RACKOBJ_RESULT_DIR}/${variant}
    mkdir -p ${RACKOBJ_RESULT_DIR}/${variant}

    # Apply variant CMake file
    cp cmake-variant/CMakeLists_${variant}.txt CMakeLists.txt

    for lock_size in "${SEQLOCK_BITS[@]}"; do
        update_key_size "$lock_size"

        LOG_DIR="logs/${variant}/${lock_size}"
        mkdir -p "$LOG_DIR"

        RESULT_BASE="${RACKOBJ_RESULT_DIR}/${variant}/${lock_size}"
        mkdir -p "$RESULT_BASE"

        # Build once per key size
        ./build.sh > "${LOG_DIR}/build_errors.log" 2>&1
        if [ $? -ne 0 ]; then
            echo "Error: build.sh failed."
            exit 1
        fi

        for scr_size in "${SCR_SIZES[@]}"; do
            sed -i -E "s/(logical_scr_size: )[0-9]+MB/\1${scr_size}MB/" config/a.yaml

            for num_obj in "${NUN_OBJS[@]}"; do
                new_pages=$((num_obj + 1000))
                sed -i -E "s/(slots: )[0-9]+/\1${new_pages}/" config/a.yaml
                sed -i -E "s/(key_space: )[0-9]+/\1${num_obj}/" config/a.yaml

                for workload in "${WORKLOADS[@]}"; do
                    for zipf in "${ZIPFS[@]}"; do
                        for w_ratio in "${W_RATIOS[@]}"; do

                            echo "Run: lock_size=${lock_size}, scr=${scr_size}, zipf=${zipf}, w_ratio=${w_ratio}"

                            # Source dir from limits-thread
                            src_dir="${RACKOBJ_RESULT_DIR}${workload}-${w_ratio}/${zipf}"

                            # New result directory layout
                            dst_dir="${RESULT_BASE}/${workload}-${w_ratio}-${num_obj}-${zipf}-${scr_size}MB"

                            sudo rm -rf "$src_dir"
                            mkdir -p "$src_dir"

                            ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1
                            for num_threads in "${THREAD_COUNTS[@]}"; do
                                sudo RACKOBJ_CONFIG=config/a.yaml ./build/benchmarks/megalon \
                                    $workload $RACKOBJ_RESULT_DIR $num_threads $w_ratio $zipf \
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

# -------------------------------
# SECOND LOOP: ANALYSIS
# -------------------------------
echo "Starting analysis..."
cd benchmarks/script

for variant in "${VARIANTS[@]}"; do
    STATS_FILE="../../logs/${variant}/stats.log"
    rm -f "$STATS_FILE"
    touch "$STATS_FILE"

    for lock_size in "${SEQLOCK_BITS[@]}"; do
        for workload in "${WORKLOADS[@]}"; do
            for zipf in "${ZIPFS[@]}"; do
                for w_ratio in "${W_RATIOS[@]}"; do
                    for scr_size in "${SCR_SIZES[@]}"; do

                        echo "Analyzing: lock_size=${lock_size}, zipf=${zipf}, w_ratio=${w_ratio}, scr=${scr_size}" >> "$STATS_FILE"

                        RESULT_BASE="${RACKOBJ_RESULT_DIR}${variant}/${lock_size}"

                        for num_obj in "${NUN_OBJS[@]}"; do
                            for num_threads in "${THREAD_COUNTS[@]}"; do
                                dst_dir="${RESULT_BASE}/${workload}-${w_ratio}-${num_obj}-${zipf}-${scr_size}MB/${num_threads}"

                                python3 avg_lat.py $dst_dir t >> "$STATS_FILE"
                            done
                        done

                        echo "" >> "$STATS_FILE"
                    done
                done
            done
        done
    done
done

cd ../..
cp cmake-variant/CMakeLists_c3.txt CMakeLists.txt

echo "Done — constants restored automatically."
echo "BUT ARE YOU RUNNING 32B OR 4K???"
