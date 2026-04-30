#!/usr/bin/env bash

echo "Checking System State:"
echo -n "Hyperthreading Enabled: "
[[ `cat /sys/devices/system/cpu/smt/active` == 1 ]] && echo true || echo false

# echo -n "Turbo Boost Enabled: "
# [[ `cat /sys/devices/system/cpu/intel_pstate/no_turbo` == 0 ]] && echo true || echo false

bash turbo-boost.sh check

echo -n "Scaling Governor set to \"performance\": "
[[ `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | uniq` = "performance" ]] && echo true || echo false

echo

echo -n "Enable Performance Mode: "
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Reference: https://serverfault.com/a/967597
echo -n "Disabling Hyperthreading: "
echo off | sudo tee /sys/devices/system/cpu/smt/control

# Reference: https://askubuntu.com/a/620114
echo -n "Disabling Turbo Boost: "
# echo "1" | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
bash turbo-boost.sh disable

echo 

echo "Be sure to add the following line to /etc/default/grub, then run update-grub, then restart the machine"
echo "GRUB_CMDLINE_LINUX=\"intel_iommu=on intel_idle.max_cstates=0 idle=poll intel_pstate=disable\""