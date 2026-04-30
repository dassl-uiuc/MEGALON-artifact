#!/usr/bin/env python3
import argparse
import re
from collections import defaultdict

parser = argparse.ArgumentParser(description="Cluster throughput data by ZIPF, W_RATIOS, and THREADS")
parser.add_argument("stats_file", help="Path to stats file")
args = parser.parse_args()

# Key: (ZIPF, W_RATIOS, THREADS) → list of (OBJS, THROUGHPUT)
data = defaultdict(list)

current_zipf = current_w = current_threads = None
current_objs = None

with open(args.stats_file) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue

        # Parse the header line
        if line.startswith("Analyzing tput:"):
            match = re.search(
                r"ZIPF:\s*([\d.]+)\s+"
                r"W_RATIOS:\s*([\d.]+)\s+"
                r"OBJS:\s*(\d+)\s+"
                r"SCR_SIZE:\s*\d+(MB|GB)\s+"
                r"THREADS:\s*(\d+)",
                line,
            )
            if match:
                current_zipf, current_w, current_objs, _, current_threads = match.groups()
                current_objs = int(current_objs)
            continue

        # Parse the throughput line
        if line.startswith("Throughput:"):
            match = re.search(r"Throughput:\s*([\d,\.]+)", line)
            if match and current_zipf and current_w and current_threads and current_objs is not None:
                tput = float(match.group(1).replace(",", ""))
                key = (current_zipf, current_w, current_threads)
                data[key].append((current_objs, tput))

# Print output sorted properly
for (zipf, w_ratio, threads), entries in sorted(
    data.items(),
    key=lambda kv: (float(kv[0][0]), float(kv[0][1]), int(kv[0][2])),
):
    print(f"\n# zipf={zipf}, w_ratio={w_ratio}, threads={threads}")
    # Sort within group by OBJS ascending
    for objs, tput in sorted(entries, key=lambda e: e[0]):
        print(f"{objs}, {tput}")
        # print(f"{tput}")
