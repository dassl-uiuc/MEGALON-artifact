import numpy as np
import sys
import os
import fnmatch

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

def process_files(file_names):
    numbers = []
    for file_name in file_names:
        try:
            data = np.loadtxt(file_name, dtype=np.float32, ndmin=1)
            numbers.append(data)
        except FileNotFoundError:
            print("File not found: " + file_name)
        except ValueError:
            print("Non-numeric data in file: " + file_name)

    if not numbers:
        return None  # Skip empty results

    numbers = np.concatenate(numbers)
    numbers = np.array(numbers, dtype=np.float32)

    average = np.mean(numbers)
    std_dev = calculate_std_dev(numbers)
    p999 = calculate_p(numbers, 99.9)
    p99 = calculate_p(numbers, 99)
    p50 = calculate_p(numbers, 50)

    return f"{average},{std_dev},{p50},{p99},{p999}"

def main():
    args = sys.argv[1:]

    if not args:
        print("Usage: script.py <path> [r|w|rw]")
        sys.exit(1)

    path = args[0]
    mode = args[1] if len(args) > 1 else "rw"

    def collect_files(directory):
        if mode == "r":
            return get_files_starting_with(directory, "latencies")
        elif mode == "w":
            return get_files_starting_with(directory, "write-latencies")
        else:
            files = get_files_starting_with(directory, "latencies")
            files += get_files_starting_with(directory, "write-latencies")
            return files

    if os.path.isfile(path):
        result = process_files([path])
        if result:
            print(result)
    elif os.path.isdir(path):
        contents = [os.path.join(path, f) for f in os.listdir(path)]
        all_dirs = all(os.path.isdir(p) for p in contents)
        all_files = all(os.path.isfile(p) for p in contents)

        if all_dirs:
            for subdir in sorted(contents, key=lambda d: int(os.path.basename(d))):
                files = collect_files(subdir)
                result = process_files(files)
                if result:
                    print(f"{os.path.basename(subdir)},{result}")
        elif all_files:
            files = collect_files(path)
            result = process_files(files)
            if result:
                print(result)
        else:
            print("Error: Mixed files and directories in the path.")
    else:
        print("Invalid path.")

if __name__ == "__main__":
    main()
