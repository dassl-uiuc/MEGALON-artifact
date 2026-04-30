#!/usr/bin/env bash

# prerequisite: sudo modprobe intel_uncore_frequency

set -euo pipefail

freq="${1-}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONSTANTS_FILE="${PROJECT_ROOT}/src/common/constants.h"

FAST_FREQ_KHZ=2400000

get_numa_mem() {
    # NUMA_MEM is configured by scripts/setup_logical_node.sh
    if [[ -f "${CONSTANTS_FILE}" ]]; then
        local v
        v=$(awk '/^[[:space:]]*#define[[:space:]]+NUMA_MEM[[:space:]]+/ {print $3; exit}' "${CONSTANTS_FILE}" || true)
        if [[ "${v}" =~ ^[0-9]+$ ]]; then
            echo "${v}"
            return 0
        fi
    fi
    return 1
}

package_dir() {
    local node="$1"
    printf '/sys/devices/system/cpu/intel_uncore_frequency/package_%02d_die_00' "${node}"
}

list_packages() {
    local base='/sys/devices/system/cpu/intel_uncore_frequency'
    local d
    shopt -s nullglob
    for d in "${base}"/package_*_die_00; do
        [[ -d "${d}" ]] || continue
        # d ends with .../package_XX_die_00
        basename "${d}" | awk -F'[ _]' '{printf "%d\n", $2}'
    done
    shopt -u nullglob
}

set_freq() {
    local node="$1"
    local minormax="$2"
    local ffreq="$3"
    local dir

    dir="$(package_dir "${node}")"
    if [[ ! -d "${dir}" ]]; then
        echo "Error: uncore freq sysfs path not found: ${dir}" >&2
        exit 1
    fi

    echo "echo ${ffreq} > ${dir}/${minormax}_freq_khz"
    sudo sh -c "echo ${ffreq} > '${dir}/${minormax}_freq_khz'"
}

NUMA_MEM="$(get_numa_mem)" || {
    echo "Error: failed to read NUMA_MEM from ${CONSTANTS_FILE}. Run ./scripts/setup_logical_node.sh first." >&2
    exit 1
}

mapfile -t packages < <(list_packages)
if [[ "${#packages[@]}" -eq 0 ]]; then
    echo "Error: no intel_uncore_frequency packages found under /sys/devices/system/cpu/intel_uncore_frequency" >&2
    exit 1
fi

found_slow_node=0
for p in "${packages[@]}"; do
    if [[ "${p}" == "${NUMA_MEM}" ]]; then
        found_slow_node=1
        break
    fi
done
if [[ "${found_slow_node}" -ne 1 ]]; then
    echo "Error: NUMA_MEM=${NUMA_MEM} not found in available uncore packages: ${packages[*]}" >&2
    exit 1
fi

if [[ "${freq}" =~ ^[1-9][0-9]*$ ]]; then
    # Slow down NUMA_MEM (CXL-memory simulation node) and keep others at FAST_FREQ_KHZ.
    for p in "${packages[@]}"; do
        if [[ "${p}" == "${NUMA_MEM}" ]]; then
            set_freq "${p}" min "${freq}"
            set_freq "${p}" max "${freq}"
        else
            set_freq "${p}" min "${FAST_FREQ_KHZ}"
            set_freq "${p}" max "${FAST_FREQ_KHZ}"
        fi
    done
else
    echo "resetting uncore frequencies to defaults"
    for p in "${packages[@]}"; do
        dir="$(package_dir "${p}")"
        init_max_freq=$(cat "${dir}/initial_max_freq_khz")
        init_min_freq=$(cat "${dir}/initial_min_freq_khz")
        set_freq "${p}" min "${init_min_freq}"
        set_freq "${p}" max "${init_max_freq}"
    done
fi
