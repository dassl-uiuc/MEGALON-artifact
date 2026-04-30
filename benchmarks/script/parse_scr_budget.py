#!/usr/bin/env python3
import argparse
import re
from collections import defaultdict

parser = argparse.ArgumentParser(description="Cluster throughput data by ZIPF, W_RATIOS, and THREADS")
parser.add_argument("stats_file", help="Path to stats file")
args = parser.parse_args()

# Key: (ZIPF, W_RATIOS, THREADS) → list of (SCR_SIZE, THROUGHPUT)
data = defaultdict(list)

current_zipf = current_w = current_threads = None
current_scr = None

with open(args.stats_file) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue

        # Parse the header line
        if line.startswith("Analyzing tput:"):
            match = re.search(
                r"ZIPF:\s*([\d.]+)\s+W_RATIOS:\s*([\d.]+)\s+OBJS:\s*\d+\s+SCR_SIZE:\s*(\d+)(MB|GB)\s+THREADS:\s*(\d+)",
                line,
            )
            if match:
                current_zipf, current_w, scr_val, unit, current_threads = match.groups()
                scr_val = float(scr_val)
                if unit == "GB":
                    scr_val *= 1024
                current_scr = scr_val
            continue

        # Parse the throughput line
        if line.startswith("Throughput:"):
            match = re.search(r"Throughput:\s*([\d,\.]+)", line)
            if match and current_zipf and current_w and current_threads and current_scr is not None:
                tput = float(match.group(1).replace(",", ""))
                key = (current_zipf, current_w, current_threads)
                data[key].append((current_scr, tput))

# Print output
for (zipf, w_ratio, threads), entries in sorted(data.items()):
    print(f"\n# zipf={zipf}, w_ratio={w_ratio}, threads={threads}")
    for scr_size, tput in sorted(entries):
        print(f"{int(scr_size)},{tput}")
