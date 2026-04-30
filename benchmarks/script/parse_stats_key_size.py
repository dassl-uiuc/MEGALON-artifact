#!/usr/bin/env python3
import argparse
import re
from collections import defaultdict

parser = argparse.ArgumentParser(description="Cluster throughput data by OBJS, ZIPF, W_RATIOS, and THREADS")
parser.add_argument("stats_file", help="Path to stats file")
args = parser.parse_args()

# Key: (OBJS, ZIPF, W_RATIOS, THREADS) → list of (KEY_SIZE, THROUGHPUT)
data = defaultdict(list)

current_key_size = current_zipf = current_w = current_threads = None
current_objs = None

with open(args.stats_file) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue

        # Parse the header line
        if line.startswith("Analyzing tput:"):
            match = re.search(
                r"KEY_SIZE:\s*(\d+)B\s+"
                r"ZIPFS:\s*([\d.]+)\s+"
                r"W_RATIOS:\s*([\d.]+)\s+"
                r"OBJS:\s*(\d+)\s+"
                r"SCR_SIZE:\s*\d+(MB|GB)\s+"
                r"THREADS:\s*(\d+)",
                line,
            )
            if match:
                current_key_size, current_zipf, current_w, current_objs, _, current_threads = match.groups()
                current_key_size = int(current_key_size)
                current_objs = int(current_objs)
            continue

        # Parse throughput line
        if line.startswith("Throughput:"):
            match = re.search(r"Throughput:\s*([\d,\.]+)", line)
            if (
                match
                and current_key_size is not None
                and current_zipf
                and current_w
                and current_threads
                and current_objs is not None
            ):
                tput = float(match.group(1).replace(",", ""))

                # New grouping: OBJS, zipf, w_ratio, threads
                key = (current_objs, current_zipf, current_w, current_threads)

                # Store KEY_SIZE inside each group
                data[key].append((current_key_size, tput))

# Print sorted output
for (objs, zipf, w_ratio, threads), entries in sorted(
    data.items(),
    key=lambda kv: (int(kv[0][0]), float(kv[0][1]), float(kv[0][2]), int(kv[0][3])),
):
    print(f"\n# objs={objs}, zipf={zipf}, w_ratio={w_ratio}, threads={threads}")

    # Sort by KEY_SIZE inside each group
    for key_size, tput in sorted(entries, key=lambda e: e[0]):
        print(f"{key_size},{tput}")
