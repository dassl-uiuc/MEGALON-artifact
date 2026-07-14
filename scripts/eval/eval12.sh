#!/usr/bin/env bash

# evaluating page cache application

# Refresh sudo credentials for maximum time
sudo -v
while true; sleep 60; do sudo -n true; kill -0 "$$" || exit; done > /dev/null 2>&1 &

PREHEAT_TIME=30
EXEC_TIME=30

cd $(dirname "$0")/../..
PROJECT_ROOT="$(pwd)"

# ---- Experiment Parameters ----
VARIANTS=("hcmeta_page_cache")
WORKLOADS=("zipfian")
W_RATIOS=(0.5 0.05 0.0)  
ZIPFS=(0.99)
THREAD_COUNTS=(42)
NUM_OBJS=12000000
SCR_SIZES=(200)

CONFIG_FILE="config/file.yaml"
LOG_ROOT=${PROJECT_ROOT}/logs/eval12
RESULT_ROOT=${RACKOBJ_RESULT_DIR}eval12

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

    sudo rm -rf ${RESULT_ROOT}/${variant}
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
                    echo "Running benchmark: scr=${scr_size}MB workload=${workload} zipf=${zipf} w=${w_ratio}"

                    src_dir="${RACKOBJ_RESULT_DIR}hcmeta-page-cache/${workload}-${zipf}-${w_ratio}"
                    dst_dir="${RESULT_ROOT}/${variant}/${workload}-${w_ratio}-${zipf}-${scr_size}MB"

                    sudo rm -rf "$src_dir"
                    sudo mkdir -p "$src_dir"
                    sudo chown -R ${USER}:${USER} "$src_dir"

                    ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1

                    for num_threads in "${THREAD_COUNTS[@]}"; do
                        sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/page-cache \
                            $workload $RACKOBJ_RESULT_DIR $num_threads \
                            $w_ratio $zipf \
                            $PREHEAT_TIME $EXEC_TIME \
                            >> "${LOG_ROOT}/${variant}/output_${workload}_${w_ratio}_${zipf}_${scr_size}MB.log" 2>&1
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


echo "Reverting CMakeLists.txt back to CMakeLists_hcmeta.txt"
cp cmake-variant/CMakeLists_hcmeta.txt CMakeLists.txt

echo "Done"
