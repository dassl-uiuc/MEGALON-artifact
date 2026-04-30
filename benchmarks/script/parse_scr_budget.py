#!/usr/bin/env python3
import argparse
import re
from collections import defaultdict

parser = argparse.ArgumentParser(description="Cluster throughput data by zipf, W_RATIOS, and threads")
parser.add_argument("stats_file", help="Path to the stats file")
args = parser.parse_args()

# Key: (zipf, w_ratio, threads) → list of (SCR_SIZE, throughput)
data = defaultdict(list)

current_zipf = current_w = current_scr = None

with open(args.stats_file) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue

        # Parse the header line
        if line.startswith("Analyzing tput:"):
            match = re.search(
                r"zipf=(\d+\.\d+),\s*W_RATIOS=(\d+\.\d+),\s*SCR_SIZE=(\d+)(MB|GB)",
                line,
            )
            if match:
                current_zipf, current_w, current_scr, unit = match.groups()
                # Normalize SCR_SIZE to MB numeric value
                scr_val = float(current_scr)
                if unit == "GB":
                    scr_val *= 1024
                current_scr = scr_val
            continue

        # Parse data line (threads, ..., throughput)
        parts = line.split(",")
        if len(parts) < 2 or not current_zipf or not current_w or not current_scr:
            continue
        threads = parts[0].strip()
        throughput = parts[-1].strip()
        key = (current_zipf, current_w, threads)
        data[key].append((current_scr, float(throughput)))

# Print output
for (zipf, w_ratio, threads), entries in sorted(data.items()):
    print(f"\n# zipf={zipf}, w_ratio={w_ratio}, threads={threads}")
    for scr_size, tput in sorted(entries):
        print(f"{int(scr_size)},{tput}")
