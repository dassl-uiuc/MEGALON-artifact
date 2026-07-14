#!/usr/bin/env bash

# Refresh sudo credentials for maximum time
sudo -v
while true; sleep 60; do sudo -n true; kill -0 "$$" || exit; done > /dev/null 2>&1 &

PREHEAT_TIME=10
EXEC_TIME=10
WORKLOADS=("a" "b" "c" "d" "f")
D_KEY_SPACE=7200000

cd $(dirname "$0")/../..
PROJECT_ROOT="$(pwd)"

# ---- Experiment Parameters ----
variant="hcmeta"
zipf=0.99

CONFIG_FILE=${PROJECT_ROOT}/config/ycsb.yaml
LOG_ROOT=${PROJECT_ROOT}/logs/eval11
RESULT_ROOT=${RACKOBJ_RESULT_DIR}eval11
ORIGINAL_KEY_SPACE=$(awk '/^key_space:/ {print $2; exit}' "${CONFIG_FILE}")

if [ -z "$RACKOBJ_RESULT_DIR" ]; then
    echo "Error: RACKOBJ_RESULT_DIR is not set or is empty."
    exit 1
fi

if [ -z "${ORIGINAL_KEY_SPACE}" ]; then
    echo "Error: failed to read key_space from ${CONFIG_FILE}."
    exit 1
fi

sudo rm -rf ${LOG_ROOT}/${variant}
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

./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1

for workload in "${WORKLOADS[@]}"; do
    workload_log_dir="${LOG_ROOT}/${variant}/${workload}"
    workload_log_file="${workload_log_dir}/output.log"
    workload_upper=${workload^^}

    if [ "$workload" = "d" ]; then
        sed -i -E "s/(key_space: )[0-9]+/\1${D_KEY_SPACE}/" "${CONFIG_FILE}"
    else
        sed -i -E "s/(key_space: )[0-9]+/\1${ORIGINAL_KEY_SPACE}/" "${CONFIG_FILE}"
    fi

    if [ "$workload" = "a" ]; then
        num_threads=24
    else
        num_threads=42
    fi

    src_dir="${RACKOBJ_RESULT_DIR}ycsb-${workload_upper}"
    dst_dir="${RESULT_ROOT}/${variant}/ycsb-${workload_upper}"

    rm -rf "$workload_log_dir"
    mkdir -p "$workload_log_dir"

    sudo rm -rf "$src_dir"
    sudo mkdir -p "$src_dir"
    sudo chown -R ${USER}:${USER} "$src_dir"

    echo "Running ycsb workload=${workload} threads=${num_threads} zipf=${zipf}"

    sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/ycsb_benchmark \
        $workload $RACKOBJ_RESULT_DIR $num_threads \
        $zipf $PREHEAT_TIME $EXEC_TIME \
        >> "$workload_log_file" 2>&1

    if [ $? -ne 0 ]; then
        echo "Error: ycsb_benchmark failed for workload ${workload}."
        exit 1
    fi

    if [ -d "$src_dir" ] && [ "$(ls -A "$src_dir")" ]; then
        rm -rf "$dst_dir"
        cp -r "$src_dir" "$dst_dir"
    else
        echo "Error: $src_dir does not exist or is empty"
        exit 1
    fi
done

sed -i -E "s/(key_space: )[0-9]+/\1${ORIGINAL_KEY_SPACE}/" "${CONFIG_FILE}"

./scripts/set_uncore_frequency.sh > /dev/null 2>&1

echo "Reverting CMakeLists.txt back to CMakeLists_hcmeta.txt"
cp cmake-variant/CMakeLists_hcmeta.txt CMakeLists.txt

echo "Done"