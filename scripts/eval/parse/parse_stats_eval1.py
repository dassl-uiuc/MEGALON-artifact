#!/usr/bin/env python3
import re
import argparse
from collections import defaultdict

parser = argparse.ArgumentParser(description="Cluster throughput data by ZIPF, W_RATIOS, and OBJS")
parser.add_argument("stats_file", help="Path to the stats file")
args = parser.parse_args()

# data[(zipf, w_ratio, objs)][threads] = (throughput, latency)
data = defaultdict(dict)
current = {}

with open(args.stats_file, "r") as f:
    for line in f:
        line = line.strip()

        # Match metadata line
        m = re.search(
            r"ZIPF:\s*([\d\.]+)\s+W_RATIOS:\s*([\d\.]+)\s+OBJS:\s*(\d+)\s+SCR_SIZE:\s*([^\s]+)\s+THREADS:\s*(\d+)",
            line
        )
        if m:
            current = {
                "zipf": float(m.group(1)),
                "w_ratio": float(m.group(2)),
                "objs": int(m.group(3)),
                "scr_size": m.group(4),     # NEW
                "threads": int(m.group(5)),
                "latency": None,
            }
            continue

        # Match latency line (after "all:")
        m = re.search(r"all:\s*([\d\.Ee+-]+)", line)
        if m and current:
            current["latency"] = float(m.group(1))
            continue

        # Match throughput line
        m = re.search(r"Throughput:\s*([\d,\.Ee+-]+)", line)
        if m and current:
            throughput = float(m.group(1).replace(",", ""))
            latency = current.get("latency", 0.0)
            key = (current["zipf"], current["w_ratio"], current["objs"], current["scr_size"])
            data[key][current["threads"]] = (throughput, latency)
            current = {}  # reset

# Print tables
for (zipf, w_ratio, objs, scr_size), threads_dict in sorted(data.items()):
    print(f"\n# Cluster: ZIPF={zipf}, W_RATIOS={w_ratio}, OBJS={objs}, SCR_SIZE={scr_size}")
    print("THREADS,Throughput,Latency")
    for threads in sorted(threads_dict.keys()):
        throughput, latency = threads_dict[threads]
        print(f"{threads}, {throughput}, {latency}")
