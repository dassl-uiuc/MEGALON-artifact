import numpy as np
import argparse
import concurrent.futures
import os
import fnmatch

class Stats:
    def __init__(self, stat_name, numbers, percentiles):
        self.stat_name = stat_name
        self.average = np.mean(numbers)
        self.std_dev = np.std(numbers)
        self.p50 = 0 if not percentiles else np.percentile(numbers, 50)
        self.p99 = 0 if not percentiles else np.percentile(numbers, 99)
        self.p999 = 0 if not percentiles else np.percentile(numbers, 99.9)
    
    def __str__(self):
        return f"{self.stat_name}: {self.average}, {self.std_dev}, {self.p50}, {self.p99}, {self.p999}"

def Stats_constructor(stat_name, numbers, percentiles):
    return Stats(stat_name, numbers, percentiles)

def get_files_starting_with(directory, prefix):
    files = []
    try:
        for file_name in os.listdir(directory):
            full_path = os.path.join(directory, file_name)
            if os.path.isfile(full_path) and fnmatch.fnmatch(file_name, f"{prefix}*"):
                files.append(full_path)
    except FileNotFoundError:
        pass
    return files

def process_single_file(file_name: str) -> np.ndarray:
    try:
        data = np.loadtxt(file_name, dtype=np.float32, ndmin=1)
        return data
    except FileNotFoundError:
        pass
    except ValueError:
        print("Non-numeric data in file: " + file_name)
    return None

def get_numbers_from_files(files):
    numbers_from_files = []
    for file in files:
        numbers_from_files.append(process_single_file(file))
    return np.concatenate(numbers_from_files)

def latency_stats(result_dir, latency_args, percentiles):
   #get files for the specified latency stat
    stat_to_files = {}
    for lat_calc in latency_args:
        if(lat_calc == "all"):
            stat_to_files["all"] = get_files_starting_with(result_dir, "latencies-*-r")
            stat_to_files["all"].extend(get_files_starting_with(result_dir, "write-latencies-*-w")) 
        if(lat_calc == "read"):
            stat_to_files["read"] = get_files_starting_with(result_dir, "latencies-*-r")
        if(lat_calc == "write"):
            stat_to_files["write"] = get_files_starting_with(result_dir, "write-latencies-*-w")
        if(lat_calc == "owned"):
            stat_to_files["owned"] = get_files_starting_with(result_dir, "latencies-*-r_o")
            stat_to_files["owned"].extend(get_files_starting_with(result_dir, "write-latencies-*-w_o"))
        if(lat_calc == "remote"):
            stat_to_files["remote"] = get_files_starting_with(result_dir, "latencies-*-r_r")
            stat_to_files["remote"].extend(get_files_starting_with(result_dir, "write-latencies-*-w_r"))
        if(lat_calc == "r_o"):
            stat_to_files["r_o"] = get_files_starting_with(result_dir, "latencies-*-r_o_h")
            stat_to_files["r_o"].extend(get_files_starting_with(result_dir, "latencies-*-r_o_m"))
        if(lat_calc == "r_r"):
            stat_to_files["r_r"] = get_files_starting_with(result_dir, "latencies-*-r_r_h")
            stat_to_files["r_r"].extend(get_files_starting_with(result_dir, "latencies-*-r_r_m"))
        if(lat_calc == "w_o"):
            stat_to_files["w_o"] = get_files_starting_with(result_dir, "write-latencies-*-w_o_h")
            stat_to_files["w_o"].extend(get_files_starting_with(result_dir, "write-latencies-*-w_o_m"))
        if(lat_calc == "w_r"):
            stat_to_files["w_r"] = get_files_starting_with(result_dir, "write-latencies-*-w_r_h")
            stat_to_files["w_r"].extend(get_files_starting_with(result_dir, "write-latencies-*-w_r_m"))
        if(lat_calc == "r_o_h"):
            stat_to_files["r_o_h"] = get_files_starting_with(result_dir, "latencies-*-r_o_h")
        if(lat_calc == "r_o_m"):
            stat_to_files["r_o_m"] = get_files_starting_with(result_dir, "latencies-*-r_o_m")
        if(lat_calc == "r_r_h"):
            stat_to_files["r_r_h"] = get_files_starting_with(result_dir, "latencies-*-r_r_h")
        if(lat_calc == "r_r_m"):
            stat_to_files["r_r_m"] = get_files_starting_with(result_dir, "latencies-*-r_r_m")
        if(lat_calc == "w_o_h"):
            stat_to_files["w_o_h"] = get_files_starting_with(result_dir, "write-latencies-*-w_o_h")
        if(lat_calc == "w_o_m"):
            stat_to_files["w_o_m"] = get_files_starting_with(result_dir, "write-latencies-*-w_o_m")
        if(lat_calc == "w_r_h"):
            stat_to_files["w_r_h"] = get_files_starting_with(result_dir, "write-latencies-*-w_r_h")
        if(lat_calc == "w_r_m"):
            stat_to_files["w_r_m"] = get_files_starting_with(result_dir, "write-latencies-*-w_r_m")

    #get numbers from files for each stat
    stat_to_numbers = {}
    with concurrent.futures.ThreadPoolExecutor() as executor:
        stat_to_future = {}
        for key, value in stat_to_files.items():
            if(len(value) == 0):
                print(f"Skipping {key} becasue no files matched")
            else:
                stat_to_future[key] = executor.submit(get_numbers_from_files, value)

        for key, future in stat_to_future.items():
            stat_to_numbers[key] = future.result()
    
    #calculate stats for each stat
    stat_to_stats = {}
    with concurrent.futures.ProcessPoolExecutor() as executor:
        stat_to_future_stats = {}
        for stat, numbers in stat_to_numbers.items():
            stat_to_future_stats[stat] = executor.submit(Stats_constructor, stat, numbers, percentiles)
        
        for stat, future in stat_to_future_stats.items():
            stat_to_stats[stat] = future.result()
    
    for value in stat_to_stats.values():
        print(value) 

def calculate_throughput(file_name: str) -> float:
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

def throughput_stats(results_dir):
    throughput_files = get_files_starting_with(results_dir, "throughput")
    total_throughput = 0.0
    for file in throughput_files:
        total_throughput += calculate_throughput(file)
    print(f"Throughput: {total_throughput:,} ops/second")


def main():
    parser = argparse.ArgumentParser(description="Parse output stats from tigon-like.")
    parser.add_argument("results_dir", help="The directory to calculate statistics from")
    parser.add_argument("--latency", "-l", nargs="+",  help="The latency stats to calculate")
    parser.add_argument("--throughput", "-t", action="store_true",  help="Calculate throughput")
    parser.add_argument("--percentiles", "-p", action="store_true",  help="Calculate throughput")
    args = parser.parse_args()

    if args.latency is not None:
        latency_stats(args.results_dir, args.latency, args.percentiles)

    if(args.throughput):
        throughput_stats(args.results_dir)
    

if __name__ == "__main__":
    main() 