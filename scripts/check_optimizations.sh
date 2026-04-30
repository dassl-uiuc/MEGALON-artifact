#!/usr/bin/env bash

echo "Checking System State:"
echo -n "Hyperthreading Enabled: "
[[ `cat /sys/devices/system/cpu/smt/active` == 1 ]] && echo true || echo false

echo -n "Turbo Boost Enabled: "
[[ `cat /sys/devices/system/cpu/intel_pstate/no_turbo` == 0 ]] && echo true || echo false

echo -n "Scaling Governor set to \"performance\": "
[[ `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | uniq` = "performance" ]] && echo true || echo false
