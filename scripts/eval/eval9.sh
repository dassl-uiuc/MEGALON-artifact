#!/usr/bin/env bash

# evaluating replicated workload (kv-store)

# Refresh sudo credentials for maximum time
sudo -v
while true; sleep 60; do sudo -n true; kill -0 "$$" || exit; done > /dev/null 2>&1 &

PREHEAT_TIME=180
EXEC_TIME=30

cd $(dirname "$0")/../..
PROJECT_ROOT="$(pwd)"

# ---- Experiment Parameters ----
VARIANT="replicated"
WORKLOAD="zipfian"
THREAD_COUNTS=(52)
W_RATIO=0.0
ZIPF=0.99
CONFIG_FILE="config/replicate.yaml"
LOG_ROOT=${PROJECT_ROOT}/logs/eval9
RESULT_ROOT=${RACKOBJ_RESULT_DIR}eval9

if [ -z "$RACKOBJ_RESULT_DIR" ]; then
    echo "Error: RACKOBJ_RESULT_DIR is not set or is empty."
    exit 1
fi

# Clean and prepare log/result directories
rm -rf ${LOG_ROOT}/${VARIANT}
mkdir -p ${LOG_ROOT}/${VARIANT}

sudo rm -rf ${RESULT_ROOT}/${VARIANT}
sudo mkdir -p ${RESULT_ROOT}/${VARIANT}
sudo chown -R ${USER}:${USER} ${RESULT_ROOT}/${VARIANT}

# Build
cp cmake-variant/CMakeLists_megalon.txt CMakeLists.txt
./scripts/build.sh > ${LOG_ROOT}/${VARIANT}/build_errors.log 2>&1
if [ $? -ne 0 ]; then
    echo "Error: build.sh failed."
    exit 1
fi

for num_threads in "${THREAD_COUNTS[@]}"; do
    echo "Running replicated workload: threads=${num_threads}"
    src_dir="${RACKOBJ_RESULT_DIR}${WORKLOAD}-${W_RATIO}/${ZIPF}"
    dst_dir="${RESULT_ROOT}/${VARIANT}/${WORKLOAD}-${W_RATIO}-${ZIPF}-${num_threads}threads"

    sudo rm -rf "$src_dir"
    sudo mkdir -p "$src_dir"
    sudo chown -R ${USER}:${USER} "$src_dir"

    # Set uncore frequency
    ./scripts/set_uncore_frequency.sh 800000 > /dev/null 2>&1

    sudo RACKOBJ_CONFIG=${CONFIG_FILE} ./build/benchmarks/kv-store $WORKLOAD $RACKOBJ_RESULT_DIR $num_threads $W_RATIO $ZIPF 1.0 1.0 $PREHEAT_TIME $EXEC_TIME \
        >> "${LOG_ROOT}/${VARIANT}/output_${WORKLOAD}_${W_RATIO}_${ZIPF}_${num_threads}threads.log" 2>&1

    ./scripts/set_uncore_frequency.sh > /dev/null 2>&1

    # ---- COPY RESULT ----
    if [ -d "$src_dir" ] && [ "$(ls -A "$src_dir")" ]; then
        cp -r "$src_dir" "$dst_dir"
    else
        echo "Error: $src_dir does not exist or is empty"
        exit 1
    fi

done

# -------------------------------
# Analysis: compute latency (follow eval1.sh)
# -------------------------------
echo ""
echo "Starting analysis phase..."
cd benchmarks/script

echo "Analyzing latency: workload=$WORKLOAD, zipf=$ZIPF, W_RATIO=$W_RATIO" >> ${LOG_ROOT}/${VARIANT}/stat.log
for num_threads in "${THREAD_COUNTS[@]}"; do
    dst_dir="${RESULT_ROOT}/${VARIANT}/${WORKLOAD}-${W_RATIO}-${ZIPF}-${num_threads}threads"
    python3 avg_lat.py $dst_dir rwtf >> ${LOG_ROOT}/${VARIANT}/stat.log
done
echo "" >> ${LOG_ROOT}/${VARIANT}/stat.log
cd ${PROJECT_ROOT}

echo "Reverting CMakeLists.txt back to CMakeLists_megalon.txt"
cp cmake-variant/CMakeLists_megalon.txt CMakeLists.txt
echo "Done"
