#!/usr/bin/env bash

# Enable hyperthreading
echo -n "Enabling Hyperthreading: "
echo on | sudo tee /sys/devices/system/cpu/smt/control

# Disable turbo boost 
echo -n "Enabling Turbo Boost (restore=0): "
echo "0" | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

echo -n "Enable Powersave Mode: "
echo powersave | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor