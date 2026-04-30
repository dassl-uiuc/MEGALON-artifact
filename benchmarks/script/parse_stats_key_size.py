#!/usr/bin/env python3
import argparse
import re
from collections import defaultdict

parser = argparse.ArgumentParser(description="Parse stats grouped by THREADS, ZIPF, W_RATIO; ordered by KEY_SIZE")
parser.add_argument("stats_file", help="Path to stats file")
args = parser.parse_args()

# Key: (threads, zipf, w_ratio) → list of (key_size, latency, std, throughput)
data = defaultdict(list)

current_key = current_zipf = current_w = None

with open(args.stats_file) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue

        # Parse header line
        if line.startswith("Analyzing:"):
            m = re.search(
                r"key=(\d+),\s*zipf=([\d.]+),\s*w_ratio=([\d.]+)",
                line
            )
            if m:
                current_key, current_zipf, current_w = m.groups()
                current_key = int(current_key)
            continue

        # Parse stats line
        if current_key is not None:
            parts = line.split(",")
            if len(parts) >= 6:
                threads = int(parts[0])
                lat = float(parts[1])
                std = float(parts[2])
                tput = float(parts[5])

                key = (threads, float(current_zipf), float(current_w))
                data[key].append((current_key, lat, std, tput))

# Print output grouped & sorted
for (threads, zipf, w_ratio), entries in sorted(
    data.items(),
    key=lambda kv: (kv[0][0], kv[0][1], kv[0][2])
):
    print(f"\n# threads={threads}, zipf={zipf}, w_ratio={w_ratio}")

    # Sort inside group by key_size
    for key_size, _, _, tput in sorted(entries, key=lambda e: e[0]):
        print(f"{key_size},{tput}")