#!/usr/bin/env python3
import os
import re
import argparse
from collections import defaultdict

# Regex to extract parameters from filename
FNAME_RE = re.compile(r"output_zipfian_([\d.]+)_([\d.]+)_([\d]+)MB\.log")

# Regexes to extract log info
OBJS_RE = re.compile(r"loading system with (\d+) keys", re.IGNORECASE)
THREAD_RE = re.compile(r"Running (\d+) concurrent threads", re.IGNORECASE)
SHARED_RE = re.compile(r"shared_to_partition_: (\d+)", re.IGNORECASE)

def main():
    parser = argparse.ArgumentParser(description="Aggregate shared_to_partition_ from logs supporting multiple runs")
    parser.add_argument("log_dir", help="Directory containing output_zipfian logs")
    args = parser.parse_args()
    log_dir = args.log_dir

    # data[(zipf, write_ratio, objs, threads)][scr_size] = list of aggregates
    data = defaultdict(lambda: defaultdict(list))

    for fname in os.listdir(log_dir):
        if not fname.startswith("output_zipfian"):
            continue
        match = FNAME_RE.match(fname)
        if not match:
            continue

        write_ratio, zipf, scr_size = match.groups()
        scr_size = int(scr_size)
        filepath = os.path.join(log_dir, fname)

        with open(filepath, "r", errors="ignore") as f:
            lines = f.readlines()

        # Find all run start indices
        runs = []
        current_objs = None
        current_threads = None
        for i, line in enumerate(lines):
            m_objs = OBJS_RE.search(line)
            if m_objs:
                current_objs = int(m_objs.group(1))
            m_threads = THREAD_RE.search(line)
            if m_threads:
                current_threads = int(m_threads.group(1))
            if current_objs is not None and current_threads is not None:
                runs.append((i, current_objs, current_threads))
                current_objs = None
                current_threads = None

        if not runs:
            print(f"[warn] No runs found in {fname}, skipping")
            continue

        # Append sentinel for slicing
        runs.append((len(lines), None, None))

        # Process each run
        for i in range(len(runs) - 1):
            start_idx, objs, threads = runs[i]
            end_idx = runs[i + 1][0]

            shared_vals = []
            for line in lines[start_idx:end_idx]:
                m_shared = SHARED_RE.search(line)
                if m_shared:
                    shared_vals.append(int(m_shared.group(1)))

            if not shared_vals:
                print(f"[warn] No shared_to_partition_ found for objs={objs}, threads={threads} in {fname}")
                continue

            agg = sum(shared_vals)
            data[(zipf, write_ratio, objs, threads)][scr_size].append(agg)

    # Print clusters
    for (zipf, write_ratio, objs, threads), scr_dict in sorted(data.items()):
        print(f"\n=== Cluster: ZIPF={zipf}, Write Ratio={write_ratio}, OBJS={objs}, Threads={threads} ===")
        print("scr_size_MB,aggregate_shared_to_partition")
        for scr_size in sorted(scr_dict.keys()):
            for agg in scr_dict[scr_size]:
                print(f"{scr_size},{agg}")

if __name__ == "__main__":
    main()
