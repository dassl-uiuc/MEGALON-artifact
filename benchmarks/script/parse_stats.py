#!/usr/bin/env python3
import argparse
from collections import defaultdict

parser = argparse.ArgumentParser(description="Cluster throughput by zipf, W_RATIOS, and threads")
parser.add_argument("stats_file", help="Path to stats file")
args = parser.parse_args()

# Preset OBJS order
objs_config = [1200000, 2400000, 4800000, 7200000, 9600000, 12000000, 18000000]
# objs_config = [600000, 800000]

# Data structure: data[(zipf, w_ratio)][threads] = list of throughput values
data = defaultdict(lambda: defaultdict(list))

current_cluster = None
line_counter = 0

with open(args.stats_file, "r") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        # Detect cluster line
        if line.startswith("Analyzing tput:"):
            # Parse zipf and W_RATIOS
            zipf = float(line.split("zipf=")[1].split(",")[0])
            w_ratio = float(line.split("W_RATIOS=")[1].split(",")[0])
            current_cluster = (zipf, w_ratio)
            line_counter = 0
        # Detect thread line
        elif line[0].isdigit() and current_cluster:
            parts = line.split(",")
            thread = int(parts[0])
            # Last non-empty value as throughput
            throughput = None
            for p in reversed(parts):
                if p.strip():
                    throughput = float(p.strip())
                    break
            if throughput is not None:
                data[current_cluster][thread].append(throughput)
                line_counter += 1

# Print tables per cluster and per thread
for (zipf, w_ratio), threads_dict in sorted(data.items()):
    print(f"\n# Cluster: zipf={zipf}, W_RATIOS={w_ratio}")
    for thread in sorted(threads_dict.keys()):
        print(f"\n# Thread={thread}")
        print("OBJS,Throughput")
        tputs = threads_dict[thread]
        for i, tput in enumerate(tputs):
            if i >= len(objs_config):
                break
            print(f"{objs_config[i]},{tput}")
