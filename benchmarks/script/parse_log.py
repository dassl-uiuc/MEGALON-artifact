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

# Regex to extract number of objects
OBJ_RE = re.compile(r"warming up with (\d+) ops")


def main():
    parser = argparse.ArgumentParser(description="Aggregate reclaim count from logs")
    parser.add_argument("log_dir", help="Directory containing output_zipfian logs")
    args = parser.parse_args()

    # data[(zipf, write_ratio, thread_count)][num_objects] = list of reclaim counts
    data = defaultdict(lambda: defaultdict(list))

    for fname in os.listdir(args.log_dir):
        if not fname.startswith("output_zipfian"):
            continue

        m = FNAME_RE.match(fname)
        if not m:
            continue

        write_ratio, zipf, _ = m.groups()

        filepath = os.path.join(args.log_dir, fname)
        with open(filepath, "r") as f:
            current_threads = None
            current_objects = None

            for line in f:
                # update thread count
                m_thread = THREAD_RE.search(line)
                if m_thread:
                    current_threads = int(m_thread.group(1))

                # update object count
                m_obj = OBJ_RE.search(line)
                if m_obj:
                    current_objects = int(m_obj.group(1))

                # pair reclaim with current objects + current threads
                m_reclaim = RECLAIM_RE.search(line)
                if m_reclaim:
                    if current_objects is None or current_threads is None:
                        print(f"[warn] Missing object/thread info in {fname}, skipping entry")
                        continue

                    rc = int(m_reclaim.group(1))
                    data[(zipf, write_ratio, current_threads)][current_objects].append(rc)

    # Print clusters
    for (zipf, write_ratio, thread_count), obj_dict in sorted(data.items()):
        print(f"\n=== Cluster: ZIPF={zipf}, Write Ratio={write_ratio}, Threads={thread_count} ===")
        print("num_objects,reclaim_count")

        for num_objects in sorted(obj_dict.keys()):
            for rc in obj_dict[num_objects]:
                print(f"{num_objects},{rc}")


if __name__ == "__main__":
    main()
