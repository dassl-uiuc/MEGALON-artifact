#!/usr/bin/env python3
import re
import argparse
from collections import defaultdict

parser = argparse.ArgumentParser(description="Cluster throughput data by ZIPF, W_RATIOS, and THREADS")
parser.add_argument("stats_file", help="Path to the stats file")
args = parser.parse_args()

data = defaultdict(dict)
current = {}

with open(args.stats_file, "r") as f:
    for line in f:
        line = line.strip()

        # Match metadata line
        m = re.search(
            r"ZIPF:\s*([\d\.]+)\s+W_RATIOS:\s*([\d\.]+)\s+OBJS:\s*(\d+).*THREADS:\s*(\d+)",
            line
        )
        if m:
            current = {
                "zipf": float(m.group(1)),
                "w_ratio": float(m.group(2)),
                "objs": int(m.group(3)),
                "threads": int(m.group(4))
            }
            continue

        # Match throughput line
        m = re.search(r"Throughput:\s*([\d,\.]+)", line)
        if m and current:
            throughput = float(m.group(1).replace(",", ""))
            key = (current["zipf"], current["w_ratio"], current["threads"])
            data[key][current["objs"]] = throughput
            current = {}  # reset

# Print tables
for (zipf, w_ratio, threads), objs_dict in sorted(data.items()):
    print(f"\n# Cluster: ZIPF={zipf}, W_RATIOS={w_ratio}, THREADS={threads}")
    print("OBJS,Throughput")
    for objs in sorted(objs_dict.keys()):
        print(f"{objs},{objs_dict[objs]}")
