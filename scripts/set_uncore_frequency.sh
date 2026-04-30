#!/bin/bash

# prerequisite: sudo modprobe intel_uncore_frequency

freq=$1

function set_freq {
    node=$1
    minormax=$2
    ffreq=$3

    echo "echo ${ffreq} > /sys/devices/system/cpu/intel_uncore_frequency/package_0${node}_die_00/${minormax}_freq_khz"
    sudo sh -c "echo ${ffreq} > /sys/devices/system/cpu/intel_uncore_frequency/package_0${node}_die_00/${minormax}_freq_khz"
}

if [[ $freq =~ ^[1-9][0-9]*$ ]]; then
# set node 0 to be slower
    set_freq "0" "min" $freq
    set_freq "0" "max" $freq
    set_freq "1" "min" 2400000
    set_freq "1" "max" 2400000
    set_freq "2" "min" 2400000
    set_freq "2" "max" 2400000
    set_freq "3" "min" 2400000
    set_freq "3" "max" 2400000
else
    echo "resetting both nodes to defaults"
    init_max_freq=$(cat /sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/initial_max_freq_khz)
    init_min_freq=$(cat /sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/initial_min_freq_khz)
    set_freq "0" "min" $init_min_freq
    set_freq "0" "max" $init_max_freq

    init_max_freq=$(cat /sys/devices/system/cpu/intel_uncore_frequency/package_01_die_00/initial_max_freq_khz)
    init_min_freq=$(cat /sys/devices/system/cpu/intel_uncore_frequency/package_01_die_00/initial_min_freq_khz)
    set_freq "1" "min" $init_min_freq
    set_freq "1" "max" $init_max_freq

    init_max_freq=$(cat /sys/devices/system/cpu/intel_uncore_frequency/package_02_die_00/initial_max_freq_khz)
    init_min_freq=$(cat /sys/devices/system/cpu/intel_uncore_frequency/package_02_die_00/initial_min_freq_khz)
    set_freq "2" "min" $init_min_freq
    set_freq "2" "max" $init_max_freq

    init_max_freq=$(cat /sys/devices/system/cpu/intel_uncore_frequency/package_03_die_00/initial_max_freq_khz)
    init_min_freq=$(cat /sys/devices/system/cpu/intel_uncore_frequency/package_03_die_00/initial_min_freq_khz)
    set_freq "3" "min" $init_min_freq
    set_freq "3" "max" $init_max_freq
fi
echo "==========================================="