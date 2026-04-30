#!/usr/bin/env python3
import os
import re
import argparse
from collections import defaultdict

# Regex to extract parameters from filename
FNAME_RE = re.compile(r"output_zipfian_([\d.]+)_([\d.]+)_([\d]+)MB\.log")
# Regex to extract reclaim count
RECLAIM_RE = re.compile(r"relaim count: (\d+)")
# Regex to extract thread count
THREAD_RE = re.compile(r"Running (\d+) concurrent threads")

def main():
    parser = argparse.ArgumentParser(description="Aggregate reclaim count from logs")
    parser.add_argument("log_dir", help="Directory containing output_zipfian logs")
    args = parser.parse_args()
    log_dir = args.log_dir

    # data[(zipf, write_ratio, thread_count)][scr_size] = list of reclaim counts per run
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

        with open(filepath, "r") as f:
            thread_counts = []
            reclaim_counts = []
            for line in f:
                m_thread = THREAD_RE.search(line)
                if m_thread:
                    thread_counts.append(int(m_thread.group(1)))
                m_reclaim = RECLAIM_RE.search(line)
                if m_reclaim:
                    reclaim_counts.append(int(m_reclaim.group(1)))

            if len(thread_counts) != len(reclaim_counts):
                print(f"[warn] mismatch in thread counts and reclaim counts in {fname}, skipping ({len(thread_counts)} != {len(reclaim_counts)})")
                continue

            for tc, rc in zip(thread_counts, reclaim_counts):
                data[(zipf, write_ratio, tc)][scr_size].append(rc)

    # Print clusters
    for (zipf, write_ratio, thread_count), scr_dict in sorted(data.items()):
        print(f"\n=== Cluster: ZIPF={zipf}, Write Ratio={write_ratio}, Threads={thread_count} ===")
        print("scr_size_MB,reclaim_count")
        for scr_size in sorted(scr_dict.keys()):
            for rc in scr_dict[scr_size]:
                print(f"{scr_size},{rc}")

if __name__ == "__main__":
    main()
