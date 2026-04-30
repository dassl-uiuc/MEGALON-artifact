#!/usr/bin/env python3
import os
import re
import argparse
from collections import defaultdict

# ---------------------------------------------
# Regex patterns
# ---------------------------------------------
RE_THREADS = re.compile(r"Running\s+(\d+)\s+concurrent threads")
RE_NUM_OBJS = re.compile(r"loading system with\s+(\d+)\s+keys", re.IGNORECASE)
RE_TPUT = re.compile(
    r"GLOBAL THROUGHPUT:\s+avg=(\d+)\s+ops/s,\s+stddev=(\d+)\s+ops/s"
)
RE_FILENAME = re.compile(
    r"output_zipfian_(?P<w_ratio>[0-9.]+)_(?P<zipf>[0-9.]+)_(?P<scr>\d+)MB\.log"
)

RE_CHURN = re.compile(
    r"partition_to_shared:\s*(\d+).*?shared_to_partition_:\s*(\d+)"
)

# ---------------------------------------------
# Parse a single log file (UNCHANGED)
# ---------------------------------------------
def parse_single_file(filepath, collect_churn=False):
    fname = os.path.basename(filepath)
    m = RE_FILENAME.match(fname)
    if not m:
        print(f"WARNING: Filename {fname} does not match expected format.")
        return []

    w_ratio = float(m.group("w_ratio"))
    zipf = float(m.group("zipf"))
    scr_size = int(m.group("scr"))

    results = []

    with open(filepath, "r", errors="ignore") as f:
        lines = f.readlines()

    runs = []
    current = None

    for line in lines:
        m_thr = RE_THREADS.search(line)
        if m_thr:
            if current:
                runs.append(current)
            current = {
                "threads": int(m_thr.group(1)),
                "num_objs": None,
                "tput": None,
                "stddev": None,
                "churn_p2s": 0,
                "churn_s2p": 0,
            }
            continue

        if current is None:
            continue

        m_objs = RE_NUM_OBJS.search(line)
        if m_objs:
            current["num_objs"] = int(m_objs.group(1))
            continue

        m_tput = RE_TPUT.search(line)
        if m_tput:
            current["tput"] = int(m_tput.group(1))
            current["stddev"] = int(m_tput.group(2))
            continue

        if collect_churn:
            m_churn = RE_CHURN.search(line)
            if m_churn:
                current["churn_p2s"] += int(m_churn.group(1))
                current["churn_s2p"] += int(m_churn.group(2))
                continue

    if current:
        runs.append(current)

    results = []
    for r in runs:
        if r["threads"] is None or r["tput"] is None:
            print(f"Warning {r['num_objs']} result incomplete")
            print(r)
            continue
        results.append({
            "scr_size": scr_size,
            "write_ratio": w_ratio,
            "zipf": zipf,
            "threads": r["threads"],
            "num_objs": r["num_objs"],
            "tput": r["tput"],
            "stddev": r["stddev"],
            "churn_p2s": r["churn_p2s"],
            "churn_s2p": r["churn_s2p"],
        })

    return results

# ---------------------------------------------
# Parse directory/file (UNCHANGED)
# ---------------------------------------------
def collect_all(path, collect_churn=False):
    all_results = []
    if os.path.isdir(path):
        for fname in os.listdir(path):
            if fname.startswith("output_zipfian_") and fname.endswith(".log"):
                fp = os.path.join(path, fname)
                all_results.extend(parse_single_file(fp, collect_churn))
    else:
        all_results.extend(parse_single_file(path, collect_churn))

    return all_results

# ---------------------------------------------
# NEW: Group by everything EXCEPT not_group_by
# ---------------------------------------------
def group_results(results, not_group_by, churn):
    # Always group top-level on (w_ratio, zipf)
    top_groups = defaultdict(list)
    for r in results:
        key = (r["write_ratio"], r["zipf"])
        top_groups[key].append(r)

    for key, entries in top_groups.items():
        w, z = key
        print(f"\n=== WRITE_RATIO={w}, ZIPF={z} ===")

        def fmt(e):
            if churn:
                return f"{e['tput']},{e['stddev']},{e['churn_p2s']},{e['churn_s2p']}"
            else:
                return f"{e['tput']},{e['stddev']}"

        # ------------------------------------------------------------------
        # CASE 1: NOT grouping by scr_size → group by (threads, num_objs)
        # ------------------------------------------------------------------
        if not_group_by == "scr_size":
            sub = defaultdict(list)
            for e in entries:
                key2 = (e["threads"], e["num_objs"])
                sub[key2].append(e)

            for (thr, objs), group in sorted(sub.items()):
                print(f"\n-- THREADS={thr}, NUM_OBJS={objs} --")
                for e in sorted(group, key=lambda x: x["scr_size"]):
                    print(f"{e['scr_size']},{fmt(e)}")

        # ------------------------------------------------------------------
        # CASE 2: NOT grouping by threads → group by (scr_size, num_objs)
        # ------------------------------------------------------------------
        elif not_group_by == "threads":
            sub = defaultdict(list)
            for e in entries:
                key2 = (e["scr_size"], e["num_objs"])
                sub[key2].append(e)

            for (scr, objs), group in sorted(sub.items()):
                print(f"\n-- SCR_SIZE={scr}MB, NUM_OBJS={objs} --")
                for e in sorted(group, key=lambda x: x["threads"]):
                    print(f"{e['threads']},{fmt(e)}")

        # ------------------------------------------------------------------
        # CASE 3: NOT grouping by num_objs → group by (scr_size, threads)
        # ------------------------------------------------------------------
        elif not_group_by == "num_objs":
            sub = defaultdict(list)
            for e in entries:
                key2 = (e["scr_size"], e["threads"])
                sub[key2].append(e)

            for (scr, thr), group in sorted(sub.items()):
                print(f"\n-- SCR_SIZE={scr}MB, THREADS={thr} --")
                for e in sorted(group, key=lambda x: x["num_objs"]):
                    print(f"{e['num_objs']},{fmt(e)}")

        else:
            raise ValueError("Invalid --not-group-by option")


# ---------------------------------------------
# Main
# ---------------------------------------------
if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("path", help="Path to log file or directory")

    # NEW ARG: same semantics as previous script
    parser.add_argument(
        "-n",
        "--not-group-by",
        required=True,
        choices=["scr_size", "threads", "num_objs"],
        help="Metric that should NOT be grouped by (this becomes the varying dimension)"
    )

    parser.add_argument(
        "--churn",
        action="store_true",
        help="Aggregate churn metrics"
    )

    args = parser.parse_args()

    results = collect_all(args.path, args.churn)

    if not results:
        print("No valid results found.")
    else:
        group_results(results, args.not_group_by, args.churn)
