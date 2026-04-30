#!/usr/bin/env python3
import numpy as np
import sys
import os
import fnmatch
import concurrent.futures
from typing import List, Optional, Tuple
from dataclasses import dataclass
import subprocess

@dataclass
class Stats:
    average: float
    std_dev: float
    p50: float
    p99: float
    p999: float

def calculate_stats(numbers: np.ndarray) -> Stats:
    return Stats(
        average=np.mean(numbers),
        std_dev=np.std(numbers),
        p50=np.percentile(numbers, 50),
        p99=np.percentile(numbers, 99),
        p999=np.percentile(numbers, 99.9)
    )

def calculate_cdf(numbers):
    sorted_numbers = np.sort(numbers)
    n = len(numbers)
    cdf = np.arange(1, n + 1) / n
    return sorted_numbers, cdf

def calculate_p(numbers, p):
    return np.percentile(numbers, p)

def calculate_std_dev(numbers):
    return np.std(numbers)

def get_files_starting_with(directory, prefix):
    files = []
    for file_name in os.listdir(directory):
        full_path = os.path.join(directory, file_name)
        if os.path.isfile(full_path) and fnmatch.fnmatch(file_name, f"{prefix}*"):
            files.append(full_path)
    return files

def process_single_file(file_name: str) -> Optional[np.ndarray]:
    try:
        data = np.loadtxt(file_name, dtype=np.float32, ndmin=1)
        return data
    except FileNotFoundError:
        print("File not found: " + file_name)
    except ValueError:
        print("Non-numeric data in file: " + file_name)
    return None

def count_lines(file_name: str) -> Optional[int]:
    try:
        # Use wc -l which is much faster than Python's line counting
        result = subprocess.run(['wc', '-l', file_name], capture_output=True, text=True)
        if result.returncode == 0:
            return int(result.stdout.split()[0])
        return None
    except Exception as e:
        print(f"Error reading file {file_name}: {e}")
        return None

def calculate_throughput(file_name: str) -> Optional[float]:
    """Calculate throughput from a throughput file.
    File format: first line is operations, second line is duration in seconds.
    Returns operations per second."""
    try:
        with open(file_name, 'r') as f:
            lines = f.readlines()
            if len(lines) >= 2:
                operations = float(lines[0].strip())
                duration = float(lines[1].strip())
                return operations / duration
        return None
    except Exception as e:
        print(f"Error reading throughput file {file_name}: {e}")
        return None

def process_latency_files(file_names: List[str], mode: str) -> Optional[Tuple[str, int]]:
    """Process latency files and return statistics."""
    numbers = []
    
    # Use ThreadPoolExecutor for file I/O operations
    with concurrent.futures.ThreadPoolExecutor() as executor:
        # Submit all file processing tasks
        future_to_file = {executor.submit(process_single_file, file_name): file_name 
                         for file_name in file_names}
        
        # Collect results as they complete
        for future in concurrent.futures.as_completed(future_to_file):
            result = future.result()
            if result is not None:
                numbers.append(result)

    if not numbers:
        return None  # Skip empty results

    numbers = np.concatenate(numbers)
    numbers = np.array(numbers, dtype=np.float32)
    total_count = len(numbers)

    # Check if fast mode is enabled (mode ends with 'f')
    is_fast_mode = mode.endswith('f')

    if is_fast_mode:
        # Fast mode - only calculate mean and std
        stats_str = f"{np.mean(numbers)},{np.std(numbers)},,,"  # Keep empty fields with commas
        return stats_str, total_count

    # Split the data into chunks for parallel processing
    num_chunks = os.cpu_count() or 4
    chunk_size = len(numbers) // num_chunks
    chunks = [numbers[i:i + chunk_size] for i in range(0, len(numbers), chunk_size)]

    # Use ProcessPoolExecutor for CPU-bound calculations
    with concurrent.futures.ProcessPoolExecutor() as executor:
        # Calculate stats for each chunk in parallel
        chunk_stats = list(executor.map(calculate_stats, chunks))

    # Combine results from all chunks
    combined_stats = Stats(
        average=np.mean([s.average for s in chunk_stats]),
        std_dev=np.sqrt(np.mean([s.std_dev**2 for s in chunk_stats])),
        p50=np.percentile(numbers, 50),  # These need the full dataset
        p99=np.percentile(numbers, 99),
        p999=np.percentile(numbers, 99.9)
    )

    stats_str = f"{combined_stats.average},{combined_stats.std_dev},{combined_stats.p50},{combined_stats.p99},{combined_stats.p999}"
    return stats_str, total_count

def process_files(file_names: List[str], mode: str) -> Optional[Tuple[str, int]]:
    if mode == "t":
        # For throughput-only mode, calculate throughput from throughput files
        total_throughput = 0.0
        with concurrent.futures.ThreadPoolExecutor() as executor:
            future_to_file = {executor.submit(calculate_throughput, file_name): file_name 
                            for file_name in file_names}
            for future in concurrent.futures.as_completed(future_to_file):
                result = future.result()
                if result is not None:
                    total_throughput += result
        return str(total_throughput), int(total_throughput)
    
    if mode == "rwt" or mode == "rwtf":
        # For rwt mode, process latency files for stats and throughput files for throughput
        latency_files = [f for f in file_names if "latencies" in f or "write-latencies" in f]
        throughput_files = [f for f in file_names if "throughput" in f]
        
        # Process latency files for statistics
        latency_stats = None
        if latency_files:
            latency_stats = process_latency_files(latency_files, mode)
        
        # Process throughput files for throughput calculation
        total_throughput = 0.0
        if throughput_files:
            with concurrent.futures.ThreadPoolExecutor() as executor:
                future_to_file = {executor.submit(calculate_throughput, file_name): file_name 
                                for file_name in throughput_files}
                for future in concurrent.futures.as_completed(future_to_file):
                    result = future.result()
                    if result is not None:
                        total_throughput += result
        
        if latency_stats:
            stats_str, latency_count = latency_stats
            # For rwt mode, include throughput in stats string
            # For rwtf mode, throughput is printed separately as count
            if mode.endswith('f'):
                return stats_str, int(total_throughput)
            else:
                return f"{stats_str},{total_throughput}", int(total_throughput)
        else:
            return str(total_throughput), int(total_throughput)

    # For r, w, rw modes, process latency files
    return process_latency_files(file_names, mode)

def main():
    args = sys.argv[1:]

    if not args:
        print("Usage: script.py <path> [r|w|rw|t|rwt][f]")
        print("  f flag can be added to any mode to skip percentile calculations")
        sys.exit(1)

    path = args[0]
    mode = args[1] if len(args) > 1 else "rw"

    def collect_files(directory):
        base_mode = mode.rstrip('f')  # Remove 'f' flag if present
        if base_mode == "r":
            return get_files_starting_with(directory, "latencies")
        elif base_mode == "w":
            return get_files_starting_with(directory, "write-latencies")
        elif base_mode == "t":
            # For throughput mode, get throughput files instead of latency files
            return get_files_starting_with(directory, "throughput")
        elif base_mode == "rwt":
            # For rwt mode, get both latency and throughput files
            files = get_files_starting_with(directory, "latencies")
            files += get_files_starting_with(directory, "write-latencies")
            files += get_files_starting_with(directory, "throughput")
            return files
        else:  # rw mode
            files = get_files_starting_with(directory, "latencies")
            files += get_files_starting_with(directory, "write-latencies")
            return files

    if os.path.isfile(path):
        result = process_files([path], mode)
        if result:
            if mode == "t":
                print(result[0])  # Only print count
            elif "t" in mode:
                stats, count = result
                if mode.endswith('f'):
                    print(f"{stats}{count}")  # No extra comma for fast mode with throughput
                else:
                    print(f"{stats},{count}")  # Keep comma for full mode with throughput
            else:
                print(result[0])  # Print stats
    elif os.path.isdir(path):
        contents = [os.path.join(path, f) for f in os.listdir(path)]
        all_dirs = all(os.path.isdir(p) for p in contents)
        all_files = all(os.path.isfile(p) for p in contents)

        if all_dirs:
            for subdir in sorted(contents, key=lambda d: int(os.path.basename(d))):
                files = collect_files(subdir)
                result = process_files(files, mode)
                stats, count = result
                if result:
                    if mode == "t":
                        print(f"{os.path.basename(subdir)}, {result[0]}")  # Only print count
                    elif "t" in mode:
                        if mode.endswith('f'):
                            print(f"{os.path.basename(subdir)},{stats}{count}")  # No extra comma for fast mode with throughput
                        else:
                            print(f"{os.path.basename(subdir)},{stats},{count}")  # Keep comma for full mode with throughput
                    else:
                        print(f"{os.path.basename(subdir)},{stats}")  # Print subdir and stats
        elif all_files:
            files = collect_files(path)
            result = process_files(files, mode)
            if result:
                if mode == "t":
                    print(result[0])  # Only print count
                elif "t" in mode:
                    stats, count = result
                    if mode.endswith('f'):
                        print(f"{stats}{count}")  # No extra comma for fast mode with throughput
                    else:
                        print(f"{stats},{count}")  # Keep comma for full mode with throughput
                else:
                    print(result[0])  # Print stats
        else:
            print("Error: Mixed files and directories in the path.")
    else:
        print("Invalid path.")

if __name__ == "__main__":
    main() 