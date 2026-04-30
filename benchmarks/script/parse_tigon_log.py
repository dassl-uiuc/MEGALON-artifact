#!/usr/bin/env python3
import re
import sys
from collections import defaultdict

if len(sys.argv) != 2:
    print(f"Usage: {sys.argv[0]} <log_file>")
    sys.exit(1)

log_file = sys.argv[1]

# regex patterns
theta_re = re.compile(r"RESULT DIR .*zipfian-([0-9.]+)-")
threads_re = re.compile(r"Running (\d+) concurrent threads")
objs_re = re.compile(r"loading system with (\d+) keys")
shared_re = re.compile(r"shared_to_partition_: (\d+)")

# cluster results by theta
results = defaultdict(list)

with open(log_file) as f:
    theta = None
    threads = None
    objs = None
    shared_sum = 0
    shared_count = 0

    for line in f:
        # detect theta
        m = theta_re.search(line)
        if m:
            # save previous run if valid
            if theta is not None and threads is not None and objs is not None and shared_count > 0:
                results[theta].append((threads, objs, shared_sum))
            theta = float(m.group(1))
            threads = None
            objs = None
            shared_sum = 0
            shared_count = 0
            continue

        # detect threads
        m = threads_re.search(line)
        if m:
            if threads is not None and objs is not None and shared_count > 0:
                results[theta].append((threads, objs, shared_sum))
            threads = int(m.group(1))
            shared_sum = 0
            shared_count = 0
            continue

        # detect objs
        m = objs_re.search(line)
        if m:
            objs = int(m.group(1))
            continue

        # detect shared_to_partition_
        m = shared_re.search(line)
        if m:
            shared_sum += int(m.group(1))
            shared_count += 1

    # save last run
    if theta is not None and threads is not None and objs is not None and shared_count > 0:
        results[theta].append((threads, objs, shared_sum))

# print results sorted by theta and thread count
for theta in sorted(results.keys()):
    print(f"### Zipf theta: {theta} ###")
    print("threads,objs,shared_sum")
    for t, o, s in sorted(results[theta], key=lambda x: (x[0], x[1])):
        print(f"{t}, {o}, {s}")
    print()
